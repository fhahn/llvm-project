//===- MatrixUtils.h - Utilities to lower matrix intrinsics -----*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Utilities for lowering matrix operations: emitting code for matrices split
// into vectors of the same type (columns for column-major, rows for row-major
// layout), lowering single matrix intrinsic calls and generating tiled loops.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TRANSFORMS_UTILS_MATRIXUTILS_H
#define LLVM_TRANSFORMS_UTILS_MATRIXUTILS_H

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Alignment.h"

namespace llvm {
class DomTreeUpdater;
class BasicBlock;
class ConstantInt;
class FixedVectorType;
class IntrinsicInst;
class Value;
class Loop;
class LoopInfo;
class IRBuilderBase;
class TargetTransformInfo;
class Type;

/// A helper struct to create IR loop nests for tiling in IR of the following
/// form:
///   for ColumnLoop.Index = 0..NumColumns
///     for RowLoop.Index = 0..NumRows
///       for KLoop.Index = 0..NumInner
struct TileInfo {
  /// Number of rows of the matrix.
  unsigned NumRows;

  /// Number of columns of the matrix.
  unsigned NumColumns;

  /// Number of columns of the first matrix of a multiply /
  /// number of rows of the second matrix of a multiply.
  unsigned NumInner;

  /// Number of rows/columns in a tile.
  unsigned TileSize = -1;

  /// Properties of a single loop used when generating the tiled loop nest.
  struct MatrixLoop {
    /// The index updated on every iteration.
    Value *Index = nullptr;
    /// The header and latch of the loop.
    BasicBlock *Header = nullptr;
    BasicBlock *Latch = nullptr;
  };

  /// The loop iterating on the rows.
  MatrixLoop RowLoop;
  /// The loop iterating on the columns.
  MatrixLoop ColumnLoop;
  /// The loop iterating on k (inner dimension).
  MatrixLoop KLoop;

  TileInfo(unsigned NumRows, unsigned NumColumns, unsigned NumInner,
           unsigned TileSize)
      : NumRows(NumRows), NumColumns(NumColumns), NumInner(NumInner),
        TileSize(TileSize) {}

  /// Creates an IR loop nests for tiling of the form below. Returns the block
  /// for the inner loop body and sets {Column,Row,Inner}LoopHeader/Latch
  /// fields.
  ///
  /// for ColumnLoop.Index = 0..NumColumns
  ///   for RowLoop.Index = 0..NumRows
  ///     for InnerLoop.Index = 0..NumInner
  LLVM_ABI BasicBlock *CreateTiledLoops(BasicBlock *Start, BasicBlock *End,
                                        IRBuilderBase &B, DomTreeUpdater &DTU,
                                        LoopInfo &LI);

private:
  /// Creates a new loop with header, body and latch blocks that iterates from
  /// [0, Bound). Updates \p Preheader to branch to the new header and uses \p
  /// Exit as exit block.  Adds the new loop blocks to \L and applies dominator
  /// tree updates to \p DTU.
  static BasicBlock *CreateLoop(BasicBlock *Preheader, BasicBlock *Exit,
                                ConstantInt *Bound, ConstantInt *Step,
                                StringRef Name, IRBuilderBase &B,
                                DomTreeUpdater &DTU, Loop *L, LoopInfo &LI);
};

/// Return the estimated number of native vector operations for an operation on
/// \p NumElts elements of type \p EltTy.
LLVM_ABI unsigned estimateNumVectorOps(const TargetTransformInfo &TTI,
                                       Type *EltTy, unsigned NumElts);

/// Split the flat vector \p Matrix into vectors of \p NumElts elements.
LLVM_ABI SmallVector<Value *> splitMatrix(IRBuilderBase &Builder, Value *Matrix,
                                          unsigned NumElts);

/// Concatenate \p Vectors into a flat vector.
LLVM_ABI Value *flattenMatrix(IRBuilderBase &Builder,
                              ArrayRef<Value *> Vectors);

/// Load \p NumVecs vectors of type \p VecTy starting at \p Ptr, with \p Stride
/// elements between the starts of consecutive vectors.
LLVM_ABI SmallVector<Value *>
emitMatrixLoad(IRBuilderBase &Builder, FixedVectorType *VecTy, unsigned NumVecs,
               Value *Ptr, Value *Stride, MaybeAlign Alignment,
               bool IsVolatile);

/// Store \p Vectors starting at \p Ptr, with \p Stride elements between the
/// starts of consecutive vectors.
LLVM_ABI void emitMatrixStore(IRBuilderBase &Builder, ArrayRef<Value *> Vectors,
                              Value *Ptr, Value *Stride, MaybeAlign Alignment,
                              bool IsVolatile);

/// Transpose the matrix given by \p Vectors. The result uses the same layout.
LLVM_ABI SmallVector<Value *> emitMatrixTranspose(IRBuilderBase &Builder,
                                                  ArrayRef<Value *> Vectors);

/// Compute \p A * \p B into \p Result with left-associating addition, using
/// the fast-math flags of \p Builder. \p Result must hold vectors of the result
/// type. If \p Accumulate (column-major only), accumulate onto them. Returns
/// the estimated number of compute operations.
///
/// We can fold a transpose into the operand that is used to extract scalars.
/// This is the first operand with row-major and the second with column-major.
/// If \p IsScalarMatrixTransposed we assume the appropriate operand is
/// transposed. Illegal remainder vectors over \p SplitRemainderThreshold bits
/// are split.
LLVM_ABI unsigned emitMatrixMultiply(IRBuilderBase &Builder,
                                     MutableArrayRef<Value *> Result,
                                     ArrayRef<Value *> A, ArrayRef<Value *> B,
                                     const TargetTransformInfo &TTI,
                                     bool IsColumnMajor = true,
                                     bool Accumulate = false,
                                     bool IsScalarMatrixTransposed = false,
                                     unsigned SplitRemainderThreshold = 0);

/// Lower the llvm.matrix.multiply call \p MatMul to vector operations and
/// erase it. \p IsColumnMajor selects the layout of the flat matrices.
LLVM_ABI void lowerMatrixMultiply(IntrinsicInst *MatMul,
                                  const TargetTransformInfo &TTI,
                                  bool IsColumnMajor = true);

/// Lower the llvm.matrix.transpose call \p Transpose to vector operations and
/// erase it. \p IsColumnMajor selects the layout of the flat matrices.
LLVM_ABI void lowerMatrixTranspose(IntrinsicInst *Transpose,
                                   bool IsColumnMajor = true);

/// Lower the llvm.matrix.column.major.load call \p Load to vector loads and
/// erase it.
LLVM_ABI void lowerMatrixColumnMajorLoad(IntrinsicInst *Load);

/// Lower the llvm.matrix.column.major.store call \p Store to vector stores and
/// erase it.
LLVM_ABI void lowerMatrixColumnMajorStore(IntrinsicInst *Store);
} // namespace llvm

#endif
