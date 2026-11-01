//===- MatrixUtils.cpp - Utilities to lower matrix intrinsics ---*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Utilities for lowering matrix operations: emitting code for matrices split
// into vectors of the same type (columns for column-major, rows for row-major
// layout) and generating tiled loops.
//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/Utils/MatrixUtils.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Analysis/DomTreeUpdater.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/TargetTransformInfo.h"
#include "llvm/Analysis/VectorUtils.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/MDBuilder.h"
#include "llvm/IR/ProfDataUtils.h"
#include "llvm/IR/Type.h"
#include "llvm/Support/CommandLine.h"

#include <cmath>

using namespace llvm;

namespace llvm {
extern cl::opt<bool> ProfcheckDisableMetadataFixes;
} // end namespace llvm

BasicBlock *TileInfo::CreateLoop(BasicBlock *Preheader, BasicBlock *Exit,
                                 ConstantInt *Bound, ConstantInt *Step,
                                 StringRef Name, IRBuilderBase &B,
                                 DomTreeUpdater &DTU, Loop *L, LoopInfo &LI) {
  LLVMContext &Ctx = Preheader->getContext();
  BasicBlock *Header = BasicBlock::Create(
      Preheader->getContext(), Name + ".header", Preheader->getParent(), Exit);
  BasicBlock *Body = BasicBlock::Create(Header->getContext(), Name + ".body",
                                        Header->getParent(), Exit);
  BasicBlock *Latch = BasicBlock::Create(Header->getContext(), Name + ".latch",
                                         Header->getParent(), Exit);

  Type *I32Ty = Type::getInt64Ty(Ctx);
  UncondBrInst::Create(Body, Header);
  UncondBrInst::Create(Latch, Body);
  PHINode *IV =
      PHINode::Create(I32Ty, 2, Name + ".iv", Header->getTerminator()->getIterator());
  IV->addIncoming(ConstantInt::get(I32Ty, 0), Preheader);

  B.SetInsertPoint(Latch);
  Value *Inc = B.CreateAdd(IV, Step, Name + ".step");
  Value *Cond = B.CreateICmpNE(Inc, Bound, Name + ".cond");
  auto *BR = B.CreateCondBr(Cond, Header, Exit);
  if (!ProfcheckDisableMetadataFixes) {
    assert(Step->getZExtValue() != 0 &&
           "Expected a non-zero step size. This is chosen by the pass and "
           "should always be non-zero to imply a finite loop.");
    MDBuilder MDB(Preheader->getContext());
    setFittedBranchWeights(
        *BR, {Bound->getZExtValue() / Step->getZExtValue(), 1}, false);
  }
  IV->addIncoming(Inc, Latch);

  UncondBrInst *PreheaderBr = cast<UncondBrInst>(Preheader->getTerminator());
  BasicBlock *Tmp = PreheaderBr->getSuccessor();
  PreheaderBr->setSuccessor(0, Header);
  DTU.applyUpdatesPermissive({
      {DominatorTree::Delete, Preheader, Tmp},
      {DominatorTree::Insert, Header, Body},
      {DominatorTree::Insert, Body, Latch},
      {DominatorTree::Insert, Latch, Header},
      {DominatorTree::Insert, Latch, Exit},
      {DominatorTree::Insert, Preheader, Header},
  });

  L->addBasicBlockToLoop(Header, LI);
  L->addBasicBlockToLoop(Body, LI);
  L->addBasicBlockToLoop(Latch, LI);
  return Body;
}

// Creates the following loop nest skeleton:
//  for C = 0; C < NumColumns; C += TileSize
//    for R = 0; R < NumRows; R += TileSize
//      for K = 0; K < Inner ; K += TileSize
BasicBlock *TileInfo::CreateTiledLoops(BasicBlock *Start, BasicBlock *End,
                                       IRBuilderBase &B, DomTreeUpdater &DTU,
                                       LoopInfo &LI) {
  Loop *ColumnLoopInfo = LI.AllocateLoop();
  Loop *RowLoopInfo = LI.AllocateLoop();
  Loop *KLoopInfo = LI.AllocateLoop();
  RowLoopInfo->addChildLoop(KLoopInfo);
  ColumnLoopInfo->addChildLoop(RowLoopInfo);
  if (Loop *ParentL = LI.getLoopFor(Start))
    ParentL->addChildLoop(ColumnLoopInfo);
  else
    LI.addTopLevelLoop(ColumnLoopInfo);

  BasicBlock *ColBody =
      CreateLoop(Start, End, B.getInt64(NumColumns), B.getInt64(TileSize),
                 "cols", B, DTU, ColumnLoopInfo, LI);
  ColumnLoop.Latch = ColBody->getSingleSuccessor();
  BasicBlock *RowBody =
      CreateLoop(ColBody, ColumnLoop.Latch, B.getInt64(NumRows),
                 B.getInt64(TileSize), "rows", B, DTU, RowLoopInfo, LI);
  RowLoop.Latch = RowBody->getSingleSuccessor();

  BasicBlock *InnerBody =
      CreateLoop(RowBody, RowLoop.Latch, B.getInt64(NumInner),
                 B.getInt64(TileSize), "inner", B, DTU, KLoopInfo, LI);
  KLoop.Latch = InnerBody->getSingleSuccessor();
  ColumnLoop.Header = ColBody->getSinglePredecessor();
  RowLoop.Header = RowBody->getSinglePredecessor();
  KLoop.Header = InnerBody->getSinglePredecessor();
  RowLoop.Index = &*RowLoop.Header->begin();
  ColumnLoop.Index = &*ColumnLoop.Header->begin();
  KLoop.Index = &*KLoop.Header->begin();

  return InnerBody;
}

unsigned llvm::estimateNumVectorOps(const TargetTransformInfo &TTI, Type *EltTy,
                                    unsigned NumElts) {
  return std::ceil(
      (EltTy->getPrimitiveSizeInBits() * NumElts).getFixedValue() /
      double(TTI.getRegisterBitWidth(TargetTransformInfo::RGK_FixedWidthVector)
                 .getFixedValue()));
}

SmallVector<Value *> llvm::splitMatrix(IRBuilderBase &Builder, Value *Matrix,
                                       unsigned NumElts) {
  unsigned MatrixNumElts =
      cast<FixedVectorType>(Matrix->getType())->getNumElements();
  SmallVector<Value *> Vectors;
  for (unsigned MaskStart = 0; MaskStart < MatrixNumElts;
       MaskStart += NumElts) {
    Value *V = Builder.CreateShuffleVector(
        Matrix, createSequentialMask(MaskStart, NumElts, 0), "split");
    Vectors.push_back(V);
  }
  return Vectors;
}

Value *llvm::flattenMatrix(IRBuilderBase &Builder, ArrayRef<Value *> Vectors) {
  return Vectors.size() == 1 ? Vectors[0]
                             : concatenateVectors(Builder, Vectors);
}

// Given an element pointer \p BasePtr to the start of a (sub) matrix, compute
// the start address of vector \p VecIdx with type (\p EltType x \p NumElements)
// assuming \p Stride elements between start two consecutive vectors.
// \p Stride must be >= \p NumElements.
// For column-major matrixes, the function computes the address of a column
// vectors and \p NumElements must be set to the number of elements in a column
// (= number of rows of the matrix). For row-major matrixes, the function
// computes the address of a row vector and \p NumElements must be set to the
// number of elements in a column (= number of columns of the matrix).
//
// Consider a 4x4 matrix in column-mjaor layout like below
//
//      0       1      2      3
// 0   v_0_0  v_0_1  v_0_2  v_0_3
// 1   v_1_0  v_1_1  v_1_2  v_1_3
// 2   v_2_0  v_2_1  v_2_2  v_2_3
// 3   v_3_0  v_3_1  v_3_2  v_3_3

// To compute the column addresses for a 2x3 sub-matrix at row 1 and column 1,
// we need a pointer to the first element of the submatrix as base pointer.
// Then we can use computeVectorAddr to compute the addresses for the columns
// of the sub-matrix.
//
// Column 0: computeVectorAddr(Base, 0 (column), 4 (stride), 2 (num rows), ..)
//           -> just returns Base
// Column 1: computeVectorAddr(Base, 1 (column), 4 (stride), 2 (num rows), ..)
//           -> returns Base + (1 * 4)
// Column 2: computeVectorAddr(Base, 2 (column), 4 (stride), 2 (num rows), ..)
//           -> returns Base + (2 * 4)
//
// The graphic below illustrates the number of elements in a column (marked
// with |) and the number of skipped elements (marked with }).
//
//         v_0_0  v_0_1 {v_0_2 {v_0_3
//                Base   Col 1  Col 2
//                  |     |      |
//         v_1_0 |v_1_1 |v_1_2 |v_1_3
//         v_2_0 |v_2_1 |v_2_2 |v_2_3
//         v_3_0 {v_3_1 {v_3_2  v_3_3
//
static Value *computeVectorAddr(Value *BasePtr, Value *VecIdx, Value *Stride,
                                unsigned NumElements, Type *EltType,
                                IRBuilderBase &Builder) {

  assert((!isa<ConstantInt>(Stride) ||
          cast<ConstantInt>(Stride)->getZExtValue() >= NumElements) &&
         "Stride must be >= the number of elements in the result vector.");

  // Compute the start of the vector with index VecIdx as VecIdx * Stride.
  Value *VecStart = Builder.CreateMul(VecIdx, Stride, "vec.start");

  // Get pointer to the start of the selected vector. Skip GEP creation,
  // if we select vector 0.
  if (isa<ConstantInt>(VecStart) && cast<ConstantInt>(VecStart)->isZero())
    VecStart = BasePtr;
  else
    VecStart = Builder.CreateInBoundsGEP(EltType, BasePtr, VecStart, "vec.gep");

  return VecStart;
}

/// Compute the alignment for a column/row \p Idx with \p Stride between them.
/// The address at \p Idx == 0 has alignment \p A. If \p Stride is a
/// ConstantInt, reduce the initial alignment based on the byte offset. For
/// non-ConstantInt strides, return the common alignment of the initial
/// alignment and the element size in bytes.
static Align getAlignForIndex(const DataLayout &DL, unsigned Idx, Value *Stride,
                              Type *ElementTy, MaybeAlign A) {
  Align InitialAlign = DL.getValueOrABITypeAlignment(A, ElementTy);
  if (Idx == 0)
    return InitialAlign;

  TypeSize ElementSizeInBits = DL.getTypeSizeInBits(ElementTy);
  if (auto *ConstStride = dyn_cast<ConstantInt>(Stride)) {
    uint64_t StrideInBytes =
        ConstStride->getZExtValue() * ElementSizeInBits / 8;
    return commonAlignment(InitialAlign, Idx * StrideInBytes);
  }
  return commonAlignment(InitialAlign, ElementSizeInBits / 8);
}

static Value *castToIndexType(Value *Ptr, Value *V, IRBuilderBase &Builder) {
  assert(isa<IntegerType>(V->getType()) &&
         "Attempted to cast non-integral type to integer index");
  // In case the data layout's index type differs in width from the type of
  // the value we're given, truncate or zero extend to the appropriate width.
  // We zero extend here as indices are unsigned.
  return Builder.CreateZExtOrTrunc(
      V, Builder.getDataLayout().getIndexType(Ptr->getType()),
      V->getName() + ".cast");
}

SmallVector<Value *> llvm::emitMatrixLoad(IRBuilderBase &Builder,
                                          FixedVectorType *VecTy,
                                          unsigned NumVecs, Value *Ptr,
                                          Value *Stride, MaybeAlign Alignment,
                                          bool IsVolatile) {
  Type *EltTy = VecTy->getElementType();
  SmallVector<Value *> Vectors;
  Stride = castToIndexType(Ptr, Stride, Builder);
  for (unsigned I = 0; I < NumVecs; ++I) {
    Value *GEP = computeVectorAddr(
        Ptr, Builder.getIntN(Stride->getType()->getScalarSizeInBits(), I),
        Stride, VecTy->getNumElements(), EltTy, Builder);
    Value *Vector = Builder.CreateAlignedLoad(
        VecTy, GEP,
        getAlignForIndex(Builder.getDataLayout(), I, Stride, EltTy, Alignment),
        IsVolatile, "col.load");

    Vectors.push_back(Vector);
  }
  return Vectors;
}

void llvm::emitMatrixStore(IRBuilderBase &Builder, ArrayRef<Value *> Vectors,
                           Value *Ptr, Value *Stride, MaybeAlign Alignment,
                           bool IsVolatile) {
  auto *VecTy = cast<FixedVectorType>(Vectors[0]->getType());
  Type *EltTy = VecTy->getElementType();
  Stride = castToIndexType(Ptr, Stride, Builder);
  for (auto Vec : enumerate(Vectors)) {
    Value *GEP = computeVectorAddr(
        Ptr,
        Builder.getIntN(Stride->getType()->getScalarSizeInBits(), Vec.index()),
        Stride, VecTy->getNumElements(), EltTy, Builder);
    Builder.CreateAlignedStore(Vec.value(), GEP,
                               getAlignForIndex(Builder.getDataLayout(),
                                                Vec.index(), Stride, EltTy,
                                                Alignment),
                               IsVolatile);
  }
}

SmallVector<Value *> llvm::emitMatrixTranspose(IRBuilderBase &Builder,
                                               ArrayRef<Value *> Vectors) {
  auto *VectorTy = cast<FixedVectorType>(Vectors[0]->getType());
  SmallVector<Value *> Result;
  for (unsigned I = 0, E = VectorTy->getNumElements(); I < E; ++I) {
    // Build a single result vector. First initialize it.
    Value *ResultVector = PoisonValue::get(
        FixedVectorType::get(VectorTy->getElementType(), Vectors.size()));
    // Go through the old elements and insert it into the resulting vector.
    for (auto J : enumerate(Vectors)) {
      Value *Elt = Builder.CreateExtractElement(J.value(), I);
      // Row and column indices are transposed.
      ResultVector = Builder.CreateInsertElement(ResultVector, Elt, J.index());
    }
    Result.push_back(ResultVector);
  }
  return Result;
}

/// Extract \p NumElts elements of \p Vec starting at \p Start.
static Value *extractVector(Value *Vec, unsigned Start, unsigned NumElts,
                            IRBuilderBase &Builder) {
  assert(cast<FixedVectorType>(Vec->getType())->getNumElements() >= NumElts &&
         "Extracted vector will contain poison values");
  return Builder.CreateShuffleVector(
      Vec, createSequentialMask(Start, NumElts, 0), "block");
}

// Set elements I..I+NumElts-1 to Block
static Value *insertVector(Value *Col, unsigned I, Value *Block,
                           IRBuilderBase &Builder) {

  // First, bring Block to the same size as Col
  unsigned BlockNumElts =
      cast<FixedVectorType>(Block->getType())->getNumElements();
  unsigned NumElts = cast<FixedVectorType>(Col->getType())->getNumElements();
  assert(NumElts >= BlockNumElts && "Too few elements for current block");

  Block = Builder.CreateShuffleVector(
      Block, createSequentialMask(0, BlockNumElts, NumElts - BlockNumElts));

  // If Col is 7 long and I is 2 and BlockNumElts is 2 the mask is: 0, 1, 7,
  // 8, 4, 5, 6
  SmallVector<int, 16> Mask;
  unsigned i;
  for (i = 0; i < I; i++)
    Mask.push_back(i);

  unsigned VecNumElts = cast<FixedVectorType>(Col->getType())->getNumElements();
  for (; i < I + BlockNumElts; i++)
    Mask.push_back(i - I + VecNumElts);

  for (; i < VecNumElts; i++)
    Mask.push_back(i);

  return Builder.CreateShuffleVector(Col, Block, Mask);
}

static Value *createMulAdd(Value *Sum, Value *A, Value *B, bool UseFPOp,
                           IRBuilderBase &Builder, bool AllowContraction,
                           const TargetTransformInfo &TTI,
                           unsigned &NumComputeOps) {
  auto *VecTy = cast<FixedVectorType>(A->getType());
  unsigned NumOps = estimateNumVectorOps(TTI, VecTy->getElementType(),
                                         VecTy->getNumElements());
  NumComputeOps += NumOps;
  if (!Sum)
    return UseFPOp ? Builder.CreateFMul(A, B) : Builder.CreateMul(A, B);

  if (UseFPOp) {
    if (AllowContraction) {
      // Use fmuladd for floating point operations and let the backend decide
      // if that's profitable.
      return Builder.CreateIntrinsic(Intrinsic::fmuladd, A->getType(),
                                     {A, B, Sum});
    }
    NumComputeOps += NumOps;
    Value *Mul = Builder.CreateFMul(A, B);
    return Builder.CreateFAdd(Sum, Mul);
  }

  NumComputeOps += NumOps;
  Value *Mul = Builder.CreateMul(A, B);
  return Builder.CreateAdd(Sum, Mul);
}

/// Given \p Remainder iterations of the the matmul inner loop,
/// potentially lower \p Blocksize that is used for the underlying
/// vector.
static unsigned capBlockSize(unsigned BlockSize, unsigned Remainder,
                             Type *EltType, const TargetTransformInfo &TTI,
                             unsigned SplitRemainderThreshold) {
  if (BlockSize <= Remainder)
    return BlockSize;

  // If the remainder is also a legal type just use it.
  auto *VecTy = FixedVectorType::get(EltType, Remainder);
  if (TTI.isTypeLegal(VecTy))
    return Remainder;

  // Similarly, if the vector is small enough that we don't want
  // to split further.
  if (VecTy->getPrimitiveSizeInBits() <= SplitRemainderThreshold)
    return Remainder;

  // Gradually lower the vectorization factor to cover the
  // remainder.
  do {
    BlockSize /= 2;
  } while (BlockSize > Remainder);
  return BlockSize;
}

unsigned llvm::emitMatrixMultiply(IRBuilderBase &Builder,
                                  MutableArrayRef<Value *> Result,
                                  ArrayRef<Value *> A, ArrayRef<Value *> B,
                                  const TargetTransformInfo &TTI,
                                  bool IsColumnMajor, bool Accumulate,
                                  bool IsScalarMatrixTransposed,
                                  unsigned SplitRemainderThreshold) {
  assert((IsColumnMajor || !Accumulate) &&
         "accumulating is only supported for column-major matrixes");
  auto *ResultVecTy = cast<FixedVectorType>(Result[0]->getType());
  Type *EltType = ResultVecTy->getElementType();
  const unsigned VF = std::max<unsigned>(
      TTI.getRegisterBitWidth(TargetTransformInfo::RGK_FixedWidthVector)
              .getFixedValue() /
          EltType->getPrimitiveSizeInBits().getFixedValue(),
      1U);
  unsigned R = IsColumnMajor ? ResultVecTy->getNumElements() : Result.size();
  unsigned C = IsColumnMajor ? Result.size() : ResultVecTy->getNumElements();
  unsigned M = IsColumnMajor
                   ? A.size()
                   : cast<FixedVectorType>(A[0]->getType())->getNumElements();

  bool IsFP = EltType->isFloatingPointTy();
  bool AllowContraction = Builder.getFastMathFlags().allowContract();
  unsigned NumComputeOps = 0;

  if (IsColumnMajor) {
    // Multiply columns from the first operand with scalars from the second
    // operand. Then move along the K axes and accumulate the columns.  With
    // this the adds can be vectorized without reassociation.
    for (unsigned J = 0; J < C; ++J) {
      unsigned BlockSize = VF;
      // If Result is zero, we don't need to accumulate in the K==0 iteration.
      bool isSumZero = isa<ConstantAggregateZero>(Result[J]);

      for (unsigned I = 0; I < R; I += BlockSize) {
        // Lower block size to make sure we stay within bounds.
        BlockSize = capBlockSize(BlockSize, R - I, EltType, TTI,
                                 SplitRemainderThreshold);
        Value *Sum = Accumulate
                         ? extractVector(Result[J], I, BlockSize, Builder)
                         : nullptr;
        for (unsigned K = 0; K < M; ++K) {
          Value *L = extractVector(A[K], I, BlockSize, Builder);
          Value *RH =
              Builder.CreateExtractElement(B[IsScalarMatrixTransposed ? K : J],
                                           IsScalarMatrixTransposed ? J : K);
          Value *Splat = Builder.CreateVectorSplat(BlockSize, RH, "splat");
          Sum =
              createMulAdd(isSumZero && K == 0 ? nullptr : Sum, L, Splat, IsFP,
                           Builder, AllowContraction, TTI, NumComputeOps);
        }
        Result[J] = insertVector(Result[J], I, Sum, Builder);
      }
    }
  } else {
    // Multiply rows from the second operand with scalars from the first
    // operand. Then move along the K axes and accumulate the rows.  With this
    // the adds can be vectorized without reassociation.
    for (unsigned I = 0; I < R; ++I) {
      unsigned BlockSize = VF;
      bool isSumZero = isa<ConstantAggregateZero>(Result[I]);
      for (unsigned J = 0; J < C; J += BlockSize) {
        // Lower the vectorization factor to cover the remainder.
        BlockSize = capBlockSize(BlockSize, C - J, EltType, TTI,
                                 SplitRemainderThreshold);

        Value *Sum = nullptr;
        for (unsigned K = 0; K < M; ++K) {
          Value *R = extractVector(B[K], J, BlockSize, Builder);
          Value *LH =
              Builder.CreateExtractElement(A[IsScalarMatrixTransposed ? K : I],
                                           IsScalarMatrixTransposed ? I : K);
          Value *Splat = Builder.CreateVectorSplat(BlockSize, LH, "splat");
          Sum =
              createMulAdd(isSumZero && K == 0 ? nullptr : Sum, Splat, R, IsFP,
                           Builder, AllowContraction, TTI, NumComputeOps);
        }
        Result[I] = insertVector(Result[I], J, Sum, Builder);
      }
    }
  }
  return NumComputeOps;
}

static unsigned getDimension(IntrinsicInst *II, unsigned ArgIdx) {
  return cast<ConstantInt>(II->getArgOperand(ArgIdx))->getZExtValue();
}

void llvm::lowerMatrixMultiply(IntrinsicInst *MatMul,
                               const TargetTransformInfo &TTI,
                               bool IsColumnMajor) {
  IRBuilder<> Builder(MatMul);
  if (isa<FPMathOperator>(MatMul))
    Builder.setFastMathFlags(MatMul->getFastMathFlags());
  unsigned R = getDimension(MatMul, 2);
  unsigned M = getDimension(MatMul, 3);
  unsigned C = getDimension(MatMul, 4);
  SmallVector<Value *> A =
      splitMatrix(Builder, MatMul->getArgOperand(0), IsColumnMajor ? R : M);
  SmallVector<Value *> B =
      splitMatrix(Builder, MatMul->getArgOperand(1), IsColumnMajor ? M : C);
  auto *VecTy = FixedVectorType::get(MatMul->getType()->getScalarType(),
                                     IsColumnMajor ? R : C);
  SmallVector<Value *> Result(IsColumnMajor ? C : R, PoisonValue::get(VecTy));
  emitMatrixMultiply(Builder, Result, A, B, TTI, IsColumnMajor);
  MatMul->replaceAllUsesWith(flattenMatrix(Builder, Result));
  MatMul->eraseFromParent();
}

void llvm::lowerMatrixTranspose(IntrinsicInst *Transpose, bool IsColumnMajor) {
  IRBuilder<> Builder(Transpose);
  SmallVector<Value *> Vectors =
      splitMatrix(Builder, Transpose->getArgOperand(0),
                  getDimension(Transpose, IsColumnMajor ? 1 : 2));
  SmallVector<Value *> Result = emitMatrixTranspose(Builder, Vectors);
  Transpose->replaceAllUsesWith(flattenMatrix(Builder, Result));
  Transpose->eraseFromParent();
}

void llvm::lowerMatrixColumnMajorLoad(IntrinsicInst *Load) {
  IRBuilder<> Builder(Load);
  Type *EltTy = Load->getType()->getScalarType();
  auto *ColumnTy = FixedVectorType::get(EltTy, getDimension(Load, 3));
  bool IsVolatile = cast<ConstantInt>(Load->getArgOperand(2))->isOne();
  SmallVector<Value *> Columns = emitMatrixLoad(
      Builder, ColumnTy, getDimension(Load, 4), Load->getArgOperand(0),
      Load->getArgOperand(1), Load->getParamAlign(0), IsVolatile);
  Load->replaceAllUsesWith(flattenMatrix(Builder, Columns));
  Load->eraseFromParent();
}

void llvm::lowerMatrixColumnMajorStore(IntrinsicInst *Store) {
  IRBuilder<> Builder(Store);
  SmallVector<Value *> Columns =
      splitMatrix(Builder, Store->getArgOperand(0), getDimension(Store, 4));
  bool IsVolatile = cast<ConstantInt>(Store->getArgOperand(3))->isOne();
  emitMatrixStore(Builder, Columns, Store->getArgOperand(1),
                  Store->getArgOperand(2), Store->getParamAlign(1), IsVolatile);
  Store->eraseFromParent();
}
