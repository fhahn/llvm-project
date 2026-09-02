//===-- ConstraintElimination.cpp - Eliminate conds using constraints. ----===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Eliminate conditions based on constraints collected from dominating
// conditions.
//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/Scalar/ConstraintElimination.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/ScopeExit.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/Analysis/ConstraintSystem.h"
#include "llvm/Analysis/GlobalsModRef.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/MemoryBuiltins.h"
#include "llvm/Analysis/OptimizationRemarkEmitter.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/Analysis/ScalarEvolutionExpressions.h"
#include "llvm/Analysis/ScalarEvolutionPatternMatch.h"
#include "llvm/Analysis/TargetLibraryInfo.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/DebugInfo.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/PatternMatch.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Pass.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/DebugCounter.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include "llvm/Transforms/Utils/ValueMapper.h"

#include <optional>
#include <string>

using namespace llvm;
using namespace PatternMatch;
using namespace SCEVPatternMatch;

#define DEBUG_TYPE "constraint-elimination"

STATISTIC(NumCondsRemoved, "Number of instructions removed");
STATISTIC(NumOverflowSCEVQueries,
          "Number of SCEV signed-range queries for overflow simplification");
DEBUG_COUNTER(EliminatedCounter, "conds-eliminated",
              "Controls which conditions are eliminated");

static cl::opt<unsigned>
    MaxRows("constraint-elimination-max-rows", cl::init(500), cl::Hidden,
            cl::desc("Maximum number of rows to keep in constraint system"));

static cl::opt<bool> DumpReproducers(
    "constraint-elimination-dump-reproducers", cl::init(false), cl::Hidden,
    cl::desc("Dump IR to reproduce successful transformations."));

static int64_t MaxConstraintValue = std::numeric_limits<int64_t>::max();
static int64_t MinSignedConstraintValue = std::numeric_limits<int64_t>::min();

static Instruction *getContextInstForUse(Use &U) {
  Instruction *UserI = cast<Instruction>(U.getUser());
  if (auto *Phi = dyn_cast<PHINode>(UserI))
    UserI = Phi->getIncomingBlock(U)->getTerminator();
  return UserI;
}

/// Returns the closest program point dominating all uses of \p I, or nullptr if
/// \p I has no uses or all of them are in unreachable blocks. Note that uses
/// must only be removed or moved to dominated points afterwards, otherwise the
/// returned instruction may no longer dominate all uses.
static Instruction *findCommonDominatorOfUses(Instruction &I,
                                              DominatorTree &DT) {
  Instruction *CommonDom = nullptr;
  for (Use &U : I.uses()) {
    Instruction *UserI = getContextInstForUse(U);
    CommonDom =
        CommonDom ? DT.findNearestCommonDominator(CommonDom, UserI) : UserI;
  }
  // Uses in unreachable blocks are not in the dominator tree.
  return CommonDom && DT.getNode(CommonDom->getParent()) ? CommonDom : nullptr;
}

namespace {
using Entry = ConstraintSystem::Entry;
using RowTy = ConstraintSystem::RowTy;

/// Struct to express a condition of the form %Op0 Pred %Op1.
struct ConditionTy {
  CmpPredicate Pred;
  Value *Op0 = nullptr;
  Value *Op1 = nullptr;

  ConditionTy() = default;
  ConditionTy(CmpPredicate Pred, Value *Op0, Value *Op1)
      : Pred(Pred), Op0(Op0), Op1(Op1) {}
};

/// Represents either
///  * a condition that holds on entry to a block (=condition fact)
///  * an assume (=assume fact)
///  * a use of a compare instruction to simplify.
/// It also tracks the Dominator DFS in and out numbers for each entry.
struct FactOrCheck {
  enum class EntryTy {
    ConditionFact, /// A condition that holds on entry to a block.
    InstFact,      /// A fact that holds after Inst executed (e.g. an assume or
                   /// min/mix intrinsic.
    InstCheck,     /// An instruction to simplify (e.g. an overflow math
                   /// intrinsics) or whose flags may be strengthened.
    UseCheck       /// An use of a compare instruction to simplify.
  };

  union {
    Instruction *Inst;
    Use *U;
    ConditionTy Cond;
  };

  union {
    /// A pre-condition that must hold for the current fact to be added to the
    /// system. Only used by condition facts.
    ConditionTy DoesHold;

    /// The instruction the entry is anchored at, used to order entries within
    /// a block. Equal to Inst, except for checks anchored at a point that
    /// dominates, but does not contain, Inst. Not used by condition facts,
    /// which are anchored at the start of their block.
    Instruction *ContextInst;
  };

  unsigned NumIn;
  unsigned NumOut;
  EntryTy Ty;

  FactOrCheck(EntryTy Ty, DomTreeNode *DTN, Instruction *Inst,
              Instruction *ContextInst = nullptr)
      : Inst(Inst), ContextInst(ContextInst ? ContextInst : Inst),
        NumIn(DTN->getDFSNumIn()), NumOut(DTN->getDFSNumOut()), Ty(Ty) {}

  FactOrCheck(DomTreeNode *DTN, Use *U)
      : U(U), ContextInst(nullptr), NumIn(DTN->getDFSNumIn()),
        NumOut(DTN->getDFSNumOut()), Ty(EntryTy::UseCheck) {}

  FactOrCheck(DomTreeNode *DTN, CmpPredicate Pred, Value *Op0, Value *Op1,
              ConditionTy Precond = {})
      : Cond(Pred, Op0, Op1), DoesHold(Precond), NumIn(DTN->getDFSNumIn()),
        NumOut(DTN->getDFSNumOut()), Ty(EntryTy::ConditionFact) {}

  static FactOrCheck getConditionFact(DomTreeNode *DTN, CmpPredicate Pred,
                                      Value *Op0, Value *Op1,
                                      ConditionTy Precond = {}) {
    return FactOrCheck(DTN, Pred, Op0, Op1, Precond);
  }

  static FactOrCheck getInstFact(DomTreeNode *DTN, Instruction *Inst) {
    return FactOrCheck(EntryTy::InstFact, DTN, Inst);
  }

  static FactOrCheck getCheck(DomTreeNode *DTN, Use *U) {
    return FactOrCheck(DTN, U);
  }

  /// Returns an entry to check \p I at \p DTN. \p ContextInst, if set, anchors
  /// the entry within \p DTN's block; it is needed when \p DTN dominates \p I's
  /// uses without containing \p I itself. Defaults to \p I.
  static FactOrCheck getCheck(DomTreeNode *DTN, Instruction *I,
                              Instruction *ContextInst = nullptr) {
    assert((ContextInst ? ContextInst : I)->getParent() == DTN->getBlock() &&
           "anchoring instruction must be in DTN's block");
    return FactOrCheck(EntryTy::InstCheck, DTN, I, ContextInst);
  }

  bool isCheck() const {
    return Ty == EntryTy::InstCheck || Ty == EntryTy::UseCheck;
  }

  Instruction *getContextInst() const {
    assert(!isConditionFact());
    if (Ty == EntryTy::UseCheck)
      return getContextInstForUse(*U);
    return ContextInst;
  }

  const ConditionTy &getDoesHold() const {
    assert(isConditionFact());
    return DoesHold;
  }

  Instruction *getInstructionToSimplify() const {
    assert(isCheck());
    if (Ty == EntryTy::InstCheck)
      return Inst;
    // The use may have been simplified to a constant already.
    return dyn_cast<Instruction>(*U);
  }

  bool isConditionFact() const { return Ty == EntryTy::ConditionFact; }
};

// There is one entry per fact and check in a function; keep it within a cache
// line.
static_assert(sizeof(FactOrCheck) <= 64,
              "FactOrCheck should stay within 64 bytes");

/// The senses in which an induction phi is monotonic, together with the
/// direction it moves in.
struct MonotonicInfo {
  /// True if the phi steps by a negative constant.
  bool Decreasing = false;
  /// True if the phi is monotonic in the unsigned sense.
  bool Unsigned = false;
  /// True if the phi is monotonic in the signed sense.
  bool Signed = false;
};

/// Keep state required to build worklist.
struct State {
  DominatorTree &DT;
  LoopInfo &LI;
  ScalarEvolution &SE;
  TargetLibraryInfo &TLI;
  SmallVector<FactOrCheck, 64> WorkList;

  State(DominatorTree &DT, LoopInfo &LI, ScalarEvolution &SE,
        TargetLibraryInfo &TLI)
      : DT(DT), LI(LI), SE(SE), TLI(TLI) {}

  /// Process block \p BB and add known facts to work-list.
  void addInfoFor(BasicBlock &BB);

  /// If \p BB is a loop header, bound each induction phi in it by its start
  /// value.
  void addBoundsForHeaderInductions(BasicBlock &BB);

  /// Try to add facts for loop inductions (AddRecs) in EQ/NE compares
  /// controlling the loop header.
  void addInfoForInductions(BasicBlock &BB);

  /// Returns the direction the induction phi \p PN with backedge value \p Step
  /// moves in, and the senses in which it is monotonic in that direction.
  MonotonicInfo getMonotonicityInfo(PHINode &PN, Value *Step);

  /// Returns true if we can add a known condition from BB to its successor
  /// block Succ.
  bool canAddSuccessor(BasicBlock &BB, BasicBlock *Succ) const {
    return DT.dominates(BasicBlockEdge(&BB, Succ), Succ);
  }
};

class ConstraintInfo;

struct StackEntry {
  unsigned NumIn;
  unsigned NumOut;
  bool IsSigned = false;
  /// Variables that can be removed from the system once the stack entry gets
  /// removed.
  SmallVector<Value *, 2> ValuesToRelease;

  StackEntry(unsigned NumIn, unsigned NumOut, bool IsSigned,
             SmallVector<Value *, 2> ValuesToRelease)
      : NumIn(NumIn), NumOut(NumOut), IsSigned(IsSigned),
        ValuesToRelease(std::move(ValuesToRelease)) {}
};

struct ConstraintTy {
  RowTy Coefficients;

  /// Number of variables the constraint is defined over.
  unsigned NumVars = 0;

  bool IsSigned = false;

  ConstraintTy() = default;

  ConstraintTy(RowTy Coefficients, unsigned NumVars, bool IsSigned, bool IsEq,
               bool IsNe)
      : Coefficients(std::move(Coefficients)), NumVars(NumVars),
        IsSigned(IsSigned), IsEq(IsEq), IsNe(IsNe) {}

  bool empty() const { return Coefficients.empty(); }

  /// Returns true if the constraint does not reference any variable, i.e. it is
  /// of the form 'c >= 0'.
  bool isConstantOnly() const { return Coefficients.size() < 2; }

  bool isEq() const { return IsEq; }

  bool isNe() const { return IsNe; }

  /// Check if the current constraint is implied by the given ConstraintSystem.
  ///
  /// \return true or false if the constraint is proven to be respectively true,
  /// or false. When the constraint cannot be proven to be either true or false,
  /// std::nullopt is returned.
  std::optional<bool> isImpliedBy(const ConstraintSystem &CS) const;

private:
  bool IsEq = false;
  bool IsNe = false;
};

/// Wrapper encapsulating separate constraint systems and corresponding value
/// mappings for both unsigned and signed information. Facts are added to and
/// conditions are checked against the corresponding system depending on the
/// signed-ness of their predicates. While the information is kept separate
/// based on signed-ness, certain conditions can be transferred between the two
/// systems.
class ConstraintInfo {

  ConstraintSystem UnsignedCS;
  ConstraintSystem SignedCS;

  const DataLayout &DL;

  /// Value components (`extractvalue <0>`) of signed checked add/sub
  /// intrinsics that are only evaluated when the operation did not overflow.
  /// See collectGuardedCheckedOps.
  SmallPtrSet<const Value *, 4> GuardedCheckedOps;

public:
  ConstraintInfo(const DataLayout &DL, ArrayRef<Value *> FunctionArgs)
      : UnsignedCS(FunctionArgs), SignedCS(FunctionArgs), DL(DL) {
    auto &Value2Index = getValue2Index(false);
    // Add Arg > -1 constraints to unsigned system for all function arguments.
    for (Value *Arg : FunctionArgs)
      UnsignedCS.addRow({Entry(0, 0), Entry(-1, Value2Index.at(Arg))},
                        Value2Index.size());
  }

  DenseMap<Value *, unsigned> &getValue2Index(bool Signed) {
    return Signed ? SignedCS.getValue2Index() : UnsignedCS.getValue2Index();
  }
  const DenseMap<Value *, unsigned> &getValue2Index(bool Signed) const {
    return Signed ? SignedCS.getValue2Index() : UnsignedCS.getValue2Index();
  }

  ConstraintSystem &getCS(bool Signed) {
    return Signed ? SignedCS : UnsignedCS;
  }
  const ConstraintSystem &getCS(bool Signed) const {
    return Signed ? SignedCS : UnsignedCS;
  }

  void popLastConstraint(bool Signed) { getCS(Signed).popLastConstraint(); }
  void popLastNVariables(bool Signed, unsigned N) {
    getCS(Signed).popLastNVariables(N);
  }

  bool doesHold(CmpInst::Predicate Pred, Value *A, Value *B) const;

  /// Returns true if \p V is known to be non-negative, either because the
  /// signed system implies it or because ValueTracking can prove it.
  bool isKnownNonNegative(Value *V) const;

  /// Record that \p EV, the value component of a signed checked add/sub, is
  /// only reachable when that operation did not overflow, so it can be
  /// decomposed as the exact sum/difference of its operands.
  void addGuardedCheckedOp(const Value *EV) { GuardedCheckedOps.insert(EV); }

  /// Returns true if \p V was recorded by addGuardedCheckedOp.
  bool isGuardedCheckedOp(const Value *V) const {
    return GuardedCheckedOps.contains(V);
  }

  void addFact(CmpInst::Predicate Pred, Value *A, Value *B, unsigned NumIn,
               unsigned NumOut, SmallVectorImpl<StackEntry> &DFSInStack);

  /// Add the fact `\p Scale * \p A u<= \p B` to the unsigned system. Scaling
  /// \p A is needed for facts whose left-hand side is not an IR value and can
  /// therefore not be named by a plain addFact.
  void addScaledFact(int64_t Scale, Value *A, Value *B, unsigned NumIn,
                     unsigned NumOut, SmallVectorImpl<StackEntry> &DFSInStack);

  /// Turn a comparison of the form \p Op0Scale * \p Op0 \p Pred \p Op1 into a
  /// vector of constraints, using indices from the corresponding constraint
  /// system. New variables that need to be added to the system are collected in
  /// \p NewVariables.
  ///
  /// \p Op0Addend / \p Op1Addend, when set, are added to the respective side.
  /// This is the only way to query a relation about a *sum* of two IR values:
  /// the sum itself is not an IR value, so it cannot be named by an operand.
  /// Only supported for ICMP_SLE/ICMP_ULE, which leave the operand order
  /// unchanged below.
  ConstraintTy getConstraint(CmpInst::Predicate Pred, Value *Op0, Value *Op1,
                             SmallVectorImpl<Value *> &NewVariables,
                             bool ForceSignedSystem = false,
                             int64_t Op0Scale = 1, Value *Op0Addend = nullptr,
                             Value *Op1Addend = nullptr) const;

  /// Turns a comparison of the form \p Op0 \p Pred \p Op1 into a vector of
  /// constraints using getConstraint. Returns an empty constraint if the result
  /// cannot be used to query the existing constraint system, e.g. because it
  /// would require adding new variables. Also tries to convert signed
  /// predicates to unsigned ones if possible to allow using the unsigned system
  /// which increases the effectiveness of the signed <-> unsigned transfer
  /// logic.
  ConstraintTy getConstraintForSolving(CmpInst::Predicate Pred, Value *Op0,
                                       Value *Op1) const;

  /// Try to add information from \p A \p Pred \p B to the unsigned/signed
  /// system if \p Pred is signed/unsigned.
  void transferToOtherSystem(CmpInst::Predicate Pred, Value *A, Value *B,
                             unsigned NumIn, unsigned NumOut,
                             SmallVectorImpl<StackEntry> &DFSInStack);

private:
  /// Adds facts into constraint system. \p ForceSignedSystem can be set when
  /// the \p Pred is eq/ne, and signed constraint system is used when it's
  /// specified. \p Op0Scale scales the decomposition of \p A.
  void addFactImpl(CmpInst::Predicate Pred, Value *A, Value *B, unsigned NumIn,
                   unsigned NumOut, SmallVectorImpl<StackEntry> &DFSInStack,
                   bool ForceSignedSystem, int64_t Op0Scale = 1);

  /// Try to use the inequality \p A != \p B to tighten a non-strict bound the
  /// system already implies to the corresponding strict bound.
  void tightenBoundUsingNe(Value *A, Value *B, unsigned NumIn, unsigned NumOut,
                           SmallVectorImpl<StackEntry> &DFSInStack);
};

/// Represents a (Coefficient * Variable) entry after IR decomposition.
struct DecompEntry {
  int64_t Coefficient;
  Value *Variable;

  DecompEntry(int64_t Coefficient, Value *Variable)
      : Coefficient(Coefficient), Variable(Variable) {}
};

/// Represents an Offset + Coefficient1 * Variable1 + ... decomposition.
struct Decomposition {
  int64_t Offset = 0;
  SmallVector<DecompEntry, 3> Vars;

  Decomposition(int64_t Offset) : Offset(Offset) {}
  Decomposition(Value *V) { Vars.emplace_back(1, V); }
  Decomposition(int64_t Offset, ArrayRef<DecompEntry> Vars)
      : Offset(Offset), Vars(Vars) {}

  /// Add \p OtherOffset and return true if the operation overflows, i.e. the
  /// new decomposition is invalid.
  [[nodiscard]] bool add(int64_t OtherOffset) {
    return AddOverflow(Offset, OtherOffset, Offset);
  }

  /// Add \p Other and return true if the operation overflows, i.e. the new
  /// decomposition is invalid.
  [[nodiscard]] bool add(const Decomposition &Other) {
    if (add(Other.Offset))
      return true;
    append_range(Vars, Other.Vars);
    return false;
  }

  /// Subtract \p Other and return true if the operation overflows, i.e. the new
  /// decomposition is invalid.
  [[nodiscard]] bool sub(const Decomposition &Other) {
    Decomposition Tmp = Other;
    if (Tmp.mul(-1))
      return true;
    if (add(Tmp.Offset))
      return true;
    append_range(Vars, Tmp.Vars);
    return false;
  }

  /// Multiply all coefficients by \p Factor and return true if the operation
  /// overflows, i.e. the new decomposition is invalid.
  [[nodiscard]] bool mul(int64_t Factor) {
    if (MulOverflow(Offset, Factor, Offset))
      return true;
    for (auto &Var : Vars)
      if (MulOverflow(Var.Coefficient, Factor, Var.Coefficient))
        return true;
    return false;
  }
};

// Variable and constant offsets for a chain of GEPs, with base pointer BasePtr.
struct OffsetResult {
  Value *BasePtr;
  APInt ConstantOffset;
  SmallMapVector<Value *, APInt, 4> VariableOffsets;
  GEPNoWrapFlags NW;

  OffsetResult() : BasePtr(nullptr), ConstantOffset(0, uint64_t(0)) {}

  OffsetResult(GEPOperator &GEP, const DataLayout &DL)
      : BasePtr(GEP.getPointerOperand()), NW(GEP.getNoWrapFlags()) {
    ConstantOffset = APInt(DL.getIndexTypeSizeInBits(BasePtr->getType()), 0);
  }
};
} // namespace

// Try to collect variable and constant offsets for \p GEP, partly traversing
// nested GEPs. Returns an OffsetResult with nullptr as BasePtr of collecting
// the offset fails.
static OffsetResult collectOffsets(GEPOperator &GEP, const DataLayout &DL) {
  OffsetResult Result(GEP, DL);
  unsigned BitWidth = Result.ConstantOffset.getBitWidth();
  if (!GEP.collectOffset(DL, BitWidth, Result.VariableOffsets,
                         Result.ConstantOffset))
    return {};

  // If we have a nested GEP, check if we can combine the constant offset of the
  // inner GEP with the outer GEP.
  if (auto *InnerGEP = dyn_cast<GetElementPtrInst>(Result.BasePtr)) {
    SmallMapVector<Value *, APInt, 4> VariableOffsets2;
    APInt ConstantOffset2(BitWidth, 0);
    bool CanCollectInner = InnerGEP->collectOffset(
        DL, BitWidth, VariableOffsets2, ConstantOffset2);
    // TODO: Support cases with more than 1 variable offset.
    if (!CanCollectInner || Result.VariableOffsets.size() > 1 ||
        VariableOffsets2.size() > 1 ||
        (Result.VariableOffsets.size() >= 1 && VariableOffsets2.size() >= 1)) {
      // More than 1 variable index, use outer result.
      return Result;
    }
    Result.BasePtr = InnerGEP->getPointerOperand();
    Result.ConstantOffset += ConstantOffset2;
    if (Result.VariableOffsets.size() == 0 && VariableOffsets2.size() == 1)
      Result.VariableOffsets = std::move(VariableOffsets2);
    Result.NW &= InnerGEP->getNoWrapFlags();
  }
  return Result;
}

static Decomposition decompose(Value *V, const ConstraintInfo &Info,
                               bool IsSigned, const DataLayout &DL);

static bool canUseSExt(ConstantInt *CI) {
  const APInt &Val = CI->getValue();
  return Val.sgt(MinSignedConstraintValue) && Val.slt(MaxConstraintValue);
}

/// Returns true if the pre-condition \p Op \p Pred \p RHS, required to look
/// through an expression while decomposing it, is known to hold given \p Info.
static bool preconditionHolds(const ConstraintInfo &Info,
                              CmpInst::Predicate Pred, Value *Op, int64_t RHS) {
  return Info.doesHold(Pred, Op, ConstantInt::get(Op->getType(), RHS));
}

/// If \p V is the value component of a signed checked add/sub that is only
/// evaluated when the operation did not overflow (see collectGuardedCheckedOps)
/// decompose it as the exact sum/difference of the intrinsic's operands.
/// Returns std::nullopt if \p V is not such a value, or if the decomposition is
/// not valid in the requested system.
static std::optional<Decomposition>
decomposeGuardedCheckedOp(Value *V, const ConstraintInfo &Info, bool IsSigned,
                          const DataLayout &DL) {
  if (!Info.isGuardedCheckedOp(V))
    return std::nullopt;
  // The recorded value may since have been scheduled for removal, which detaches
  // it from its intrinsic by poisoning the aggregate operand.  There is nothing
  // left to decompose then.
  auto *WO = dyn_cast<WithOverflowInst>(
      cast<ExtractValueInst>(V)->getAggregateOperand());
  if (!WO)
    return std::nullopt;
  bool IsAdd = WO->getIntrinsicID() == Intrinsic::sadd_with_overflow;
  Value *Op0 = WO->getLHS();
  Value *Op1 = WO->getRHS();

  // The absence of signed overflow makes the result the exact mathematical
  // sum/difference, which is all the signed system needs. The unsigned system
  // additionally requires the operation to not wrap when interpreted as
  // unsigned: an add needs both operands non-negative (the sum is then at most
  // SMAX and cannot wrap), a sub needs the minuend to not be smaller.
  if (!IsSigned) {
    if (IsAdd) {
      for (Value *Op : {Op0, Op1})
        if (!isKnownNonNegative(Op, DL) &&
            !preconditionHolds(Info, CmpInst::ICMP_SGE, Op, 0))
          return std::nullopt;
    } else if (!Info.doesHold(CmpInst::ICMP_UGE, Op0, Op1))
      return std::nullopt;
  }

  auto ResA = decompose(Op0, Info, IsSigned, DL);
  auto ResB = decompose(Op1, Info, IsSigned, DL);
  if (IsAdd ? ResA.add(ResB) : ResA.sub(ResB))
    return std::nullopt;
  return ResA;
}

static Decomposition decomposeGEP(GEPOperator &GEP, const ConstraintInfo &Info,
                                  bool IsSigned, const DataLayout &DL) {
  // Do not reason about pointers where the index size is larger than 64 bits,
  // as the coefficients used to encode constraints are 64 bit integers.
  if (DL.getIndexTypeSizeInBits(GEP.getPointerOperand()->getType()) > 64)
    return &GEP;

  assert(!IsSigned && "The logic below only supports decomposition for "
                      "unsigned predicates at the moment.");
  const auto &[BasePtr, ConstantOffset, VariableOffsets, NW] =
      collectOffsets(GEP, DL);
  // We support either plain gep nuw, or gep nusw with non-negative offset,
  // which implies gep nuw.
  if (!BasePtr || NW == GEPNoWrapFlags::none())
    return &GEP;

  // For a nuw-only GEP (nuw without nusw/inbounds), the offset must be
  // interpreted as unsigned.
  if (!NW.hasNoUnsignedSignedWrap() && ConstantOffset.isNegative())
    return &GEP;

  Decomposition Result(ConstantOffset.getSExtValue(), DecompEntry(1, BasePtr));
  for (auto [Index, Scale] : VariableOffsets) {
    if (!NW.hasNoUnsignedWrap()) {
      // Try to prove nuw from nusw and nneg. If the index cannot be proven
      // non-negative, keep the GEP as-is instead of decomposing it.
      assert(NW.hasNoUnsignedSignedWrap() && "Must have nusw flag");
      if (!isKnownNonNegative(Index, DL) &&
          !preconditionHolds(Info, CmpInst::ICMP_SGE, Index, 0))
        return &GEP;
    }

    auto IdxResult = decompose(Index, Info, IsSigned, DL);
    if (IdxResult.mul(Scale.getSExtValue()))
      return &GEP;
    if (Result.add(IdxResult))
      return &GEP;
  }
  return Result;
}

// Decomposes \p V into a constant offset + list of pairs { Coefficient,
// Variable } where Coefficient * Variable. The sum of the constant offset and
// pairs equals \p V.
//
// Looking through certain expressions is only valid if a pre-condition holds.
// Pre-conditions are checked against \p Info as needed.
static Decomposition decompose(Value *V, const ConstraintInfo &Info,
                               bool IsSigned, const DataLayout &DL) {
  auto MergeResults = [&Info, IsSigned,
                       &DL](Value *A, Value *B,
                            bool IsSignedB) -> std::optional<Decomposition> {
    auto ResA = decompose(A, Info, IsSigned, DL);
    auto ResB = decompose(B, Info, IsSignedB, DL);
    if (ResA.add(ResB))
      return std::nullopt;
    return ResA;
  };

  Type *Ty = V->getType()->getScalarType();
  if (Ty->isPointerTy() && !IsSigned) {
    if (auto *GEP = dyn_cast<GEPOperator>(V))
      return decomposeGEP(*GEP, Info, IsSigned, DL);
    if (isa<ConstantPointerNull>(V))
      return int64_t(0);

    return V;
  }

  // Don't handle integers > 64 bit. Our coefficients are 64-bit large, so
  // coefficient add/mul may wrap, while the operation in the full bit width
  // would not.
  if (!Ty->isIntegerTy() || Ty->getIntegerBitWidth() > 64)
    return V;

  // A checked add/sub whose value component is only evaluated when the
  // operation did not overflow is exactly that sum/difference there.
  if (auto Decomp = decomposeGuardedCheckedOp(V, Info, IsSigned, DL))
    return *Decomp;

  // Decompose \p V used with a signed predicate.
  if (IsSigned) {
    if (auto *CI = dyn_cast<ConstantInt>(V)) {
      if (canUseSExt(CI))
        return CI->getSExtValue();
    }
    Value *Op0;
    Value *Op1;

    if (match(V, m_SExt(m_Value(Op0))))
      V = Op0;
    else if (match(V, m_NNegZExt(m_Value(Op0)))) {
      V = Op0;
    } else if (match(V, m_NSWTrunc(m_Value(Op0)))) {
      if (Op0->getType()->getScalarSizeInBits() <= 64)
        V = Op0;
    }

    if (match(V, m_NSWAddLike(m_Value(Op0), m_Value(Op1)))) {
      if (auto Decomp = MergeResults(Op0, Op1, IsSigned))
        return *Decomp;
      return V;
    }

    // `xor %x, -1` is equivalent to `sub nsw -1, %x`.
    if (match(V, m_Not(m_Value(Op0)))) {
      Decomposition Result(-1);
      if (!Result.sub(decompose(Op0, Info, IsSigned, DL)))
        return Result;
      return V;
    }

    if (match(V, m_NSWSub(m_Value(Op0), m_Value(Op1)))) {
      auto ResA = decompose(Op0, Info, IsSigned, DL);
      auto ResB = decompose(Op1, Info, IsSigned, DL);
      if (!ResA.sub(ResB))
        return ResA;
      return V;
    }

    ConstantInt *CI;
    if (match(V, m_NSWMul(m_Value(Op0), m_ConstantInt(CI))) && canUseSExt(CI)) {
      auto Result = decompose(Op0, Info, IsSigned, DL);
      if (!Result.mul(CI->getSExtValue()))
        return Result;
      return V;
    }

    // (shl nsw x, shift) is (mul nsw x, (1<<shift)), with the exception of
    // shift == bw-1.
    if (match(V, m_NSWShl(m_Value(Op0), m_ConstantInt(CI)))) {
      uint64_t Shift = CI->getValue().getLimitedValue();
      if (Shift < Ty->getIntegerBitWidth() - 1) {
        assert(Shift < 64 && "Would overflow");
        auto Result = decompose(Op0, Info, IsSigned, DL);
        if (!Result.mul(int64_t(1) << Shift))
          return Result;
        return V;
      }
    }

    return V;
  }

  if (auto *CI = dyn_cast<ConstantInt>(V)) {
    if (CI->uge(MaxConstraintValue))
      return V;
    return int64_t(CI->getZExtValue());
  }

  Value *Op0;
  if (match(V, m_ZExt(m_Value(Op0)))) {
    V = Op0;
  } else if (match(V, m_SExt(m_Value(Op0)))) {
    // Looking through the sext is only valid if the operand is non-negative.
    if (!preconditionHolds(Info, CmpInst::ICMP_SGE, Op0, 0))
      return V;
    V = Op0;
  } else if (auto *Trunc = dyn_cast<TruncInst>(V)) {
    if (Trunc->getSrcTy()->getScalarSizeInBits() <= 64 &&
        (Trunc->hasNoUnsignedWrap() || Trunc->hasNoSignedWrap())) {
      Value *Src = Trunc->getOperand(0);
      // A trunc nsw only truncates without unsigned wrap if its operand is
      // non-negative.
      if (!Trunc->hasNoUnsignedWrap() &&
          !preconditionHolds(Info, CmpInst::ICMP_SGE, Src, 0))
        return V;
      V = Src;
    }
  }

  Value *Op1;
  ConstantInt *CI;
  if (match(V, m_NUWAddLike(m_Value(Op0), m_Value(Op1)))) {
    if (auto Decomp = MergeResults(Op0, Op1, IsSigned))
      return *Decomp;
    return V;
  }

  if (match(V, m_Add(m_Value(Op0), m_ConstantInt(CI))) && CI->isNegative() &&
      canUseSExt(CI)) {
    // Adding a negative constant only wraps if Op0 is smaller than it.
    if (!preconditionHolds(Info, CmpInst::ICMP_UGE, Op0,
                           CI->getSExtValue() * -1))
      return V;
    if (auto Decomp = MergeResults(Op0, CI, true))
      return *Decomp;
    return V;
  }

  if (match(V, m_NSWAdd(m_Value(Op0), m_Value(Op1)))) {
    // An add nsw only adds without unsigned wrap if both operands are
    // non-negative.
    if ((!isKnownNonNegative(Op0, DL) &&
         !preconditionHolds(Info, CmpInst::ICMP_SGE, Op0, 0)) ||
        (!isKnownNonNegative(Op1, DL) &&
         !preconditionHolds(Info, CmpInst::ICMP_SGE, Op1, 0)))
      return V;

    if (auto Decomp = MergeResults(Op0, Op1, IsSigned))
      return *Decomp;
    return V;
  }

  if (match(V, m_NUWShl(m_Value(Op1), m_ConstantInt(CI))) && canUseSExt(CI)) {
    // The scale 1 << shift must fit in the signed coefficient, so reject a
    // shift of 63, for which int64_t{1} << 63 is INT64_MIN.
    if (CI->getSExtValue() < 0 || CI->getSExtValue() >= 63)
      return V;
    auto Result = decompose(Op1, Info, IsSigned, DL);
    if (!Result.mul(int64_t{1} << CI->getSExtValue()))
      return Result;
    return V;
  }

  if (match(V, m_NUWMul(m_Value(Op1), m_ConstantInt(CI))) && canUseSExt(CI) &&
      (!CI->isNegative())) {
    auto Result = decompose(Op1, Info, IsSigned, DL);
    if (!Result.mul(CI->getSExtValue()))
      return Result;
    return V;
  }

  if (match(V, m_Sub(m_Value(Op0), m_Value(Op1)))) {
    // a - b can be decomposed when there is no unsigned wrap (either known via
    // flag or proven as precondition).
    if (!cast<OverflowingBinaryOperator>(V)->hasNoUnsignedWrap() &&
        !Info.doesHold(CmpInst::ICMP_ULE, Op1, Op0))
      return V;
    auto ResA = decompose(Op0, Info, IsSigned, DL);
    auto ResB = decompose(Op1, Info, IsSigned, DL);
    if (!ResA.sub(ResB))
      return ResA;
    return V;
  }

  return V;
}

ConstraintTy
ConstraintInfo::getConstraint(CmpInst::Predicate Pred, Value *Op0, Value *Op1,
                              SmallVectorImpl<Value *> &NewVariables,
                              bool ForceSignedSystem, int64_t Op0Scale,
                              Value *Op0Addend, Value *Op1Addend) const {
  assert(NewVariables.empty() && "NewVariables must be empty when passed in");
  assert((!ForceSignedSystem || CmpInst::isEquality(Pred)) &&
         "signed system can only be forced on eq/ne");
  assert((Op0Scale == 1 || Pred == CmpInst::ICMP_ULE) &&
         "scaling Op0 is only supported for unsigned <=, which leaves the "
         "operand order unchanged below");
  assert(((!Op0Addend && !Op1Addend) || Pred == CmpInst::ICMP_SLE ||
          Pred == CmpInst::ICMP_ULE) &&
         "addends are only supported for <=, which leaves the operand order "
         "unchanged below");

  bool IsEq = false;
  bool IsNe = false;

  // Try to convert Pred to one of ULE/ULT/SLE/SLT.
  switch (Pred) {
  case CmpInst::ICMP_UGT:
  case CmpInst::ICMP_UGE:
  case CmpInst::ICMP_SGT:
  case CmpInst::ICMP_SGE: {
    Pred = CmpInst::getSwappedPredicate(Pred);
    std::swap(Op0, Op1);
    break;
  }
  case CmpInst::ICMP_EQ:
    if (!ForceSignedSystem && match(Op1, m_Zero())) {
      Pred = CmpInst::ICMP_ULE;
    } else {
      IsEq = true;
      Pred = CmpInst::ICMP_ULE;
    }
    break;
  case CmpInst::ICMP_NE:
    if (!ForceSignedSystem && match(Op1, m_Zero())) {
      Pred = CmpInst::getSwappedPredicate(CmpInst::ICMP_UGT);
      std::swap(Op0, Op1);
    } else {
      IsNe = true;
      Pred = CmpInst::ICMP_ULE;
    }
    break;
  default:
    break;
  }

  if (Pred != CmpInst::ICMP_ULE && Pred != CmpInst::ICMP_ULT &&
      Pred != CmpInst::ICMP_SLE && Pred != CmpInst::ICMP_SLT)
    return {};

  bool IsSigned = ForceSignedSystem || CmpInst::isSigned(Pred);
  auto &Value2Index = getValue2Index(IsSigned);
  auto ADec = decompose(Op0->stripPointerCastsSameRepresentation(), *this,
                        IsSigned, DL);
  if (Op0Scale != 1 && ADec.mul(Op0Scale))
    return {};
  auto BDec = decompose(Op1->stripPointerCastsSameRepresentation(), *this,
                        IsSigned, DL);
  for (auto [Addend, Dec] :
       {std::pair(Op0Addend, &ADec), std::pair(Op1Addend, &BDec)}) {
    if (!Addend)
      continue;
    auto AddDec = decompose(Addend->stripPointerCastsSameRepresentation(), *this,
                            IsSigned, DL);
    if (Dec->add(AddDec))
      return {};
  }
  int64_t Offset1 = ADec.Offset;
  int64_t Offset2 = BDec.Offset;
  if (MulOverflow(Offset1, int64_t(-1), Offset1))
    return {};

  auto &VariablesA = ADec.Vars;
  auto &VariablesB = BDec.Vars;

  // First try to look up \p V in Value2Index and NewVariables. Otherwise add a
  // new entry to NewVariables.
  auto GetOrAddIndex = [&Value2Index, &NewVariables](Value *V) -> unsigned {
    auto V2I = Value2Index.find(V);
    if (V2I != Value2Index.end())
      return V2I->second;
    unsigned Idx = find(NewVariables, V) - NewVariables.begin();
    if (Idx == NewVariables.size())
      NewVariables.push_back(V);
    return Value2Index.size() + Idx + 1;
  };

  // Build result constraint, by first adding all coefficients from A and then
  // subtracting all coefficients from B.
  RowTy R(1, Entry(0, 0));
  auto GetCoefficient = [&R](unsigned Idx) -> int64_t & {
    // The entry for Idx, or the place to insert it at, is the first entry with
    // an index >= Idx.
    Entry *I =
        find_if(drop_begin(R), [Idx](const Entry &E) { return E.Id >= Idx; });
    if (I == R.end() || I->Id != Idx)
      I = R.insert(I, Entry(0, Idx));
    return I->Coefficient;
  };
  for (const auto &KV : VariablesA)
    GetCoefficient(GetOrAddIndex(KV.Variable)) += KV.Coefficient;

  for (const auto &KV : VariablesB) {
    auto &Coeff = GetCoefficient(GetOrAddIndex(KV.Variable));
    if (SubOverflow(Coeff, KV.Coefficient, Coeff))
      return {};
  }

  int64_t OffsetSum;
  if (AddOverflow(Offset1, Offset2, OffsetSum))
    return {};
  if (Pred == CmpInst::ICMP_SLT || Pred == CmpInst::ICMP_ULT)
    if (AddOverflow(OffsetSum, int64_t(-1), OffsetSum))
      return {};
  R[0].Coefficient = OffsetSum;

  // Drop coefficients that cancelled out.
  erase_if(R, [](const Entry &E) { return E.Id != 0 && E.Coefficient == 0; });

  // Remove any new variable without a coefficient in the row.
  unsigned NumV2I = Value2Index.size();
  NewVariables.truncate(R.back().Id > NumV2I ? R.back().Id - NumV2I : 0);

  return ConstraintTy(std::move(R), Value2Index.size() + NewVariables.size(),
                      IsSigned, IsEq, IsNe);
}

ConstraintTy ConstraintInfo::getConstraintForSolving(CmpInst::Predicate Pred,
                                                     Value *Op0,
                                                     Value *Op1) const {
  Constant *NullC = Constant::getNullValue(Op0->getType());
  // Handle trivially true compares directly to avoid adding V UGE 0 constraints
  // for all variables in the unsigned system.
  if ((Pred == CmpInst::ICMP_ULE && Op0 == NullC) ||
      (Pred == CmpInst::ICMP_UGE && Op1 == NullC)) {
    // Return constraint that's trivially true.
    return ConstraintTy(RowTy(1, Entry(0, 0)), /*NumVars=*/0,
                        /*IsSigned=*/false, /*IsEq=*/false, /*IsNe=*/false);
  }

  // If both operands are known to be non-negative, change signed predicates to
  // unsigned ones. This increases the reasoning effectiveness in combination
  // with the signed <-> unsigned transfer logic.
  if (CmpInst::isSigned(Pred) &&
      ::isKnownNonNegative(Op0, DL, /*Depth=*/MaxAnalysisRecursionDepth - 1) &&
      ::isKnownNonNegative(Op1, DL, /*Depth=*/MaxAnalysisRecursionDepth - 1))
    Pred = ICmpInst::getUnsignedPredicate(Pred);

  SmallVector<Value *> NewVariables;
  ConstraintTy R = getConstraint(Pred, Op0, Op1, NewVariables);
  if (!NewVariables.empty())
    return {};
  return R;
}

std::optional<bool>
ConstraintTy::isImpliedBy(const ConstraintSystem &CS) const {
  const auto &[SubCS, NewCoefficients] = CS.getSubSystem(Coefficients);
  bool IsConditionImplied = SubCS.isConditionImplied(NewCoefficients);

  if (IsEq || IsNe) {
    auto NegatedOrEqual = ConstraintSystem::negateOrEqual(NewCoefficients);
    bool IsNegatedOrEqualImplied =
        !NegatedOrEqual.empty() && SubCS.isConditionImplied(NegatedOrEqual);

    // In order to check that `%a == %b` is true (equality), both conditions `%a
    // >= %b` and `%a <= %b` must hold true. When checking for equality (`IsEq`
    // is true), we return true if they both hold, false in the other cases.
    if (IsConditionImplied && IsNegatedOrEqualImplied)
      return IsEq;

    auto Negated = ConstraintSystem::negate(NewCoefficients);
    bool IsNegatedImplied =
        !Negated.empty() && SubCS.isConditionImplied(Negated);

    auto StrictLessThan = ConstraintSystem::toStrictLessThan(NewCoefficients);
    bool IsStrictLessThanImplied =
        !StrictLessThan.empty() && SubCS.isConditionImplied(StrictLessThan);

    // In order to check that `%a != %b` is true (non-equality), either
    // condition `%a > %b` or `%a < %b` must hold true. When checking for
    // non-equality (`IsNe` is true), we return true if one of the two holds,
    // false in the other cases.
    if (IsNegatedImplied || IsStrictLessThanImplied)
      return IsNe;

    return std::nullopt;
  }

  if (IsConditionImplied)
    return true;

  auto Negated = ConstraintSystem::negate(NewCoefficients);
  auto IsNegatedImplied = !Negated.empty() && SubCS.isConditionImplied(Negated);
  if (IsNegatedImplied)
    return false;

  // Neither the condition nor its negated holds, did not prove anything.
  return std::nullopt;
}

bool ConstraintInfo::doesHold(CmpInst::Predicate Pred, Value *A,
                              Value *B) const {
  auto R = getConstraintForSolving(Pred, A, B);
  return !R.empty() &&
         getCS(R.IsSigned).isConditionImpliedInSubSystem(R.Coefficients);
}

bool ConstraintInfo::isKnownNonNegative(Value *V) const {
  return doesHold(CmpInst::ICMP_SGE, V, ConstantInt::get(V->getType(), 0)) ||
         ::isKnownNonNegative(V, DL, /*Depth=*/MaxAnalysisRecursionDepth - 1);
}

void ConstraintInfo::transferToOtherSystem(
    CmpInst::Predicate Pred, Value *A, Value *B, unsigned NumIn,
    unsigned NumOut, SmallVectorImpl<StackEntry> &DFSInStack) {
  // Check if we can combine facts from the signed and unsigned systems to
  // derive additional facts.
  if (!A->getType()->isIntegerTy())
    return;
  // FIXME: This currently depends on the order we add facts. Ideally we
  // would first add all known facts and only then try to add additional
  // facts.
  switch (Pred) {
  default:
    break;
  case CmpInst::ICMP_ULT:
  case CmpInst::ICMP_ULE:
    //  If B is a signed positive constant, then A >=s 0 and A <s (or <=s) B.
    if (isKnownNonNegative(B)) {
      addFact(CmpInst::ICMP_SGE, A, ConstantInt::get(B->getType(), 0), NumIn,
              NumOut, DFSInStack);
      addFact(ICmpInst::getSignedPredicate(Pred), A, B, NumIn, NumOut,
              DFSInStack);
    }
    break;
  case CmpInst::ICMP_UGE:
  case CmpInst::ICMP_UGT:
    //  If A is a signed positive constant, then B >=s 0 and A >s (or >=s) B.
    if (isKnownNonNegative(A)) {
      addFact(CmpInst::ICMP_SGE, B, ConstantInt::get(B->getType(), 0), NumIn,
              NumOut, DFSInStack);
      addFact(ICmpInst::getSignedPredicate(Pred), A, B, NumIn, NumOut,
              DFSInStack);
    }
    break;
  case CmpInst::ICMP_SLT:
  case CmpInst::ICMP_SLE:
    if (isKnownNonNegative(A))
      addFact(ICmpInst::getUnsignedPredicate(Pred), A, B, NumIn, NumOut,
              DFSInStack);
    break;
  case CmpInst::ICMP_SGT: {
    if (doesHold(CmpInst::ICMP_SGE, B, Constant::getAllOnesValue(B->getType())))
      addFact(CmpInst::ICMP_UGE, A, ConstantInt::get(B->getType(), 0), NumIn,
              NumOut, DFSInStack);
    if (isKnownNonNegative(B))
      addFact(CmpInst::ICMP_UGT, A, B, NumIn, NumOut, DFSInStack);

    break;
  }
  case CmpInst::ICMP_SGE:
    if (isKnownNonNegative(B))
      addFact(CmpInst::ICMP_UGE, A, B, NumIn, NumOut, DFSInStack);
    break;
  }
}

#ifndef NDEBUG

static void dumpConstraint(ArrayRef<Entry> C,
                           const DenseMap<Value *, unsigned> &Value2Index) {
  ConstraintSystem CS(Value2Index);
  CS.addRow(C, Value2Index.size());
  CS.dump();
}
#endif

/// Splits the induction phi \p PN into the start value, coming from the loop
/// predecessor \p LoopPred, and the backedge value, coming from inside the
/// loop. Returns {nullptr, nullptr} if \p PN has other incoming values.
static std::pair<Value *, Value *>
getStartAndBackedgeValue(const PHINode &PN, const BasicBlock *LoopPred) {
  assert(PN.getBasicBlockIndex(LoopPred) >= 0 &&
         "LoopPred must be a predecessor of the phi's block");
  if (PN.getNumIncomingValues() != 2)
    return {nullptr, nullptr};
  unsigned StartIdx = PN.getIncomingBlock(0) == LoopPred ? 0 : 1;
  return {PN.getIncomingValue(StartIdx), PN.getIncomingValue(1 - StartIdx)};
}

/// Matches an increment of the phi matched by \p PhiM by a constant offset,
/// captured in \p Off. A checked add's value component is the same value as
/// the corresponding plain add, so match that too.
template <typename PhiMatchTy>
static auto m_IncrementOf(const PhiMatchTy &PhiM, const APInt *&Off) {
  return m_CombineOr(
      m_c_Add(PhiM, m_APInt(Off)),
      m_ExtractValue<0>(
          m_c_Intrinsic<Intrinsic::sadd_with_overflow>(PhiM, m_APInt(Off))));
}

MonotonicInfo State::getMonotonicityInfo(PHINode &PN, Value *Step) {
  MonotonicInfo Info;
  const APInt *StepOffset = nullptr;
  if (match(Step, m_IncrementOf(m_Specific(&PN), StepOffset))) {
    Info.Decreasing = StepOffset->isNegative();
    // A checked add carries no no-wrap flags of its own, so leave it to SCEV
    // below, which knows the overflow flag guards its uses.
    if (const auto *Add = dyn_cast<OverflowingBinaryOperator>(Step)) {
      Info.Unsigned = !Info.Decreasing && Add->hasNoUnsignedWrap();
      Info.Signed = Add->hasNoSignedWrap();
    }
  } else if (const auto *GEP = dyn_cast<GEPOperator>(Step)) {
    // TODO: Handle the non-increasing direction, which needs a nusw GEP with a
    // negative constant offset.
    const DataLayout &DL = PN.getDataLayout();
    APInt GEPOffset(DL.getIndexTypeSizeInBits(GEP->getType()), 0);
    Info.Unsigned = GEP->getPointerOperand() == &PN &&
                    (GEP->hasNoUnsignedWrap() ||
                     ((GEP->hasNoUnsignedSignedWrap() &&
                       GEP->accumulateConstantOffset(DL, GEPOffset) &&
                       !GEPOffset.isNegative())));
  }

  // Forming the SCEV of a phi is expensive, so only consult it for a PN + C
  // step whose no-wrap flags prove nothing.
  if (Info.Unsigned || Info.Signed || !StepOffset)
    return Info;

  const auto *AR = dyn_cast<SCEVAddRecExpr>(SE.getSCEV(&PN));
  if (!AR)
    return Info;
  ScalarEvolution::MonotonicPredicateType Expected =
      Info.Decreasing ? ScalarEvolution::MonotonicallyDecreasing
                      : ScalarEvolution::MonotonicallyIncreasing;
  auto IsMonotonic = [&](CmpInst::Predicate Pred) {
    return SE.getMonotonicPredicateType(AR, Pred) == Expected;
  };
  Info.Signed = IsMonotonic(CmpInst::ICMP_SGT);
  Info.Unsigned = !Info.Decreasing && IsMonotonic(CmpInst::ICMP_UGT);
  return Info;
}

void State::addBoundsForHeaderInductions(BasicBlock &BB) {
  Loop *L = LI.getLoopFor(&BB);
  if (!L || L->getHeader() != &BB)
    return;
  BasicBlock *LoopPred = L->getLoopPredecessor();
  if (!LoopPred)
    return;

  DomTreeNode *DTN = DT.getNode(&BB);
  for (PHINode &PN : BB.phis()) {
    if (!PN.getType()->isIntegerTy() && !PN.getType()->isPointerTy())
      continue;

    auto [Start, Step] = getStartAndBackedgeValue(PN, LoopPred);
    if (!Start)
      continue;

    MonotonicInfo Info = getMonotonicityInfo(PN, Step);
    // Every variable in the unsigned system already has a `V >= 0` row, so a
    // zero start value would just duplicate it.
    if (match(Start, m_Zero()))
      Info.Unsigned = false;
    if (!Info.Unsigned && !Info.Signed)
      continue;

    // A non-decreasing induction cannot step below its start value, and a
    // non-increasing one cannot step above it.
    Value *LHS = &PN, *RHS = Start;
    if (Info.Decreasing)
      std::swap(LHS, RHS);
    CmpPredicate Pred(Info.Unsigned ? CmpInst::ICMP_UGE : CmpInst::ICMP_SGE,
                      /*HasSameSign=*/Info.Unsigned && Info.Signed);
    WorkList.push_back(FactOrCheck::getConditionFact(DTN, Pred, LHS, RHS));
  }
}

void State::addInfoForInductions(BasicBlock &BB) {
  auto *L = LI.getLoopFor(&BB);
  if (!L)
    return;

  BasicBlock *Header = L->getHeader();
  BasicBlock *Latch = L->getLoopLatch();
  if (Header != &BB && Latch != &BB)
    return;

  // A is either a phi or a post-increment PN + C with constant step. For the
  // latter, extract the constant IncStep. The post-increment may be a plain
  // add or the value component of a checked add (sadd.with.overflow), which
  // Swift emits for overflow-checked counters; the extractvalue<0> equals
  // PN + C regardless of the overflow flag.
  Value *A;
  Value *B;
  PHINode *PN = nullptr;
  const APInt *IncStep = nullptr;
  CmpPredicate Pred;
  auto IndValue =
      m_Value(A, m_CombineOr(m_Phi(PN), m_IncrementOf(m_Phi(PN), IncStep)));

  auto *Br = dyn_cast<CondBrInst>(BB.getTerminator());
  if (!Br)
    return;

  auto CountingCmp = m_c_ICmp(Pred, IndValue, m_Value(B));
  std::optional<bool> PeeledOnEdge;
  if (!match(Br->getCondition(), CountingCmp)) {
    // Look through AND/OR, and remember which edge requires all operands to be
    // true.
    if (match(Br->getCondition(), m_c_LogicalAnd(CountingCmp, m_Value())))
      PeeledOnEdge = true;
    else if (match(Br->getCondition(), m_c_LogicalOr(CountingCmp, m_Value())))
      PeeledOnEdge = false;
    else
      return;
  }

  if (PN->getParent() != Header || PN->getNumIncomingValues() != 2 ||
      !SE.isSCEVable(PN->getType()))
    return;

  // For latch conditions, we need to inject the condition that holds for the
  // next iteration into the header. We limit to post-inc conditions, for which
  // an original PN + Step != B condition results in a PN < B constraint in the
  // header, which also holds for the next loop iteration. This would no longer
  // be correct if the post-inc handling would inject a more precise PN + Step <
  // B constraint instead.
  if (&BB == Latch && !IncStep)
    return;

  bool ContinueOnTrue =
      Pred == CmpInst::ICMP_NE || ICmpInst::isLT(Pred) || ICmpInst::isLE(Pred);
  CmpInst::Predicate ContinuePred =
      ContinueOnTrue ? Pred.dropSameSign() : CmpInst::getInversePredicate(Pred);
  BasicBlock *InLoopSucc = Br->getSuccessor(ContinueOnTrue ? 0 : 1);

  // The peeled condition only implies the compare on the edge where the
  // combined condition forces its operands, which must be the in-loop edge.
  if (PeeledOnEdge && *PeeledOnEdge != ContinueOnTrue)
    return;

  if (!L->contains(InLoopSucc) || !L->isLoopExiting(&BB) || InLoopSucc == &BB)
    return;

  BasicBlock *LoopPred = L->getLoopPredecessor();
  if (!LoopPred || !L->isLoopInvariant(B))
    return;

  auto [StartValue, Backedge] = getStartAndBackedgeValue(*PN, LoopPred);
  DomTreeNode *DTN = DT.getNode(InLoopSucc);

  if (ICmpInst::isRelational(ContinuePred)) {
    if (A != Backedge)
      return;

    // The latch condition ensures ContinuePred holds in the header on each
    // iteration other than the first. Together with a precondition on the start
    // value (StartValue ContinuePred B), we can add B as bound of PN.
    WorkList.push_back(FactOrCheck::getConditionFact(
        DTN, ContinuePred, PN, B, ConditionTy(ContinuePred, StartValue, B)));

    // If the backedge value is non-negative, a signed latch condition also
    // bounds PN in the unsigned system. Add that with an unsigned precondition,
    // which can be discharged from a `B != 0` guard when starting at 0, unlike
    // the signed `0 s< B` above.
    if (ICmpInst::isSigned(ContinuePred) &&
        isKnownNonNegative(Backedge, BB.getDataLayout())) {
      CmpInst::Predicate UPred = ICmpInst::getUnsignedPredicate(ContinuePred);
      WorkList.push_back(FactOrCheck::getConditionFact(
          DTN, UPred, PN, B, ConditionTy(UPred, StartValue, B)));
    }

    // A relational latch steps past B rather than landing on it, so none of the
    // reasoning below applies.
    return;
  }

  const APInt *StepOffset = nullptr;
  const SCEV *StartSCEV = nullptr;
  if (match(Backedge, m_c_Add(m_Specific(PN), m_APInt(StepOffset)))) {
    if (StepOffset->isZero())
      return;
  } else {
    const SCEV *Expr = SE.getSCEV(PN);
    if (!match(Expr,
               m_scev_AffineAddRec(m_SCEV(StartSCEV), m_scev_APInt(StepOffset),
                                   m_SpecificLoop(L))))
      return;
  }

  // If we looked through `PN + C`, only derive facts when that add is
  // really the induction's post-increment or post-decrement.
  if (IncStep && *IncStep != *StepOffset)
    return;

  MonotonicInfo Info = getMonotonicityInfo(*PN, Backedge);

  // Handle negative steps.
  if (StepOffset->isNegative()) {
    // TODO: Extend to allow steps > -1.
    if (!(-*StepOffset).isOne())
      return;

    // AR may wrap.
    // The loop exits once the compared value reaches B, that is at PN == B when
    // comparing the phi, and at PN == B + 1 for a post-decrement. Use
    // non-strict predicate for the former, and a strict one for the latter to
    // ensure the loop exits before wrapping.
    CmpInst::Predicate UPrecond =
        IncStep ? CmpInst::ICMP_ULT : CmpInst::ICMP_ULE;
    ConditionTy BBeforeStartUnsigned = {UPrecond, B, StartValue};
    ConditionTy BBeforeStartSigned = {ICmpInst::getSignedPredicate(UPrecond), B,
                                      StartValue};

    // AR may wrap, so both facts are conditional on B being below StartValue.
    // Add StartValue >= PN, which holds as the loop exits before wrapping.
    WorkList.push_back(FactOrCheck::getConditionFact(
        DTN, CmpInst::ICMP_UGE, StartValue, PN, BBeforeStartUnsigned));
    if (!(Info.Decreasing && Info.Signed))
      WorkList.push_back(FactOrCheck::getConditionFact(
          DTN, CmpInst::ICMP_SGE, StartValue, PN, BBeforeStartSigned));
    // Add PN > B, which holds as the loop exits when reaching B.
    WorkList.push_back(FactOrCheck::getConditionFact(DTN, CmpInst::ICMP_UGT, PN,
                                                     B, BBeforeStartUnsigned));
    WorkList.push_back(FactOrCheck::getConditionFact(DTN, CmpInst::ICMP_SGT, PN,
                                                     B, BBeforeStartSigned));
    return;
  }

  // Make sure AR either steps by 1 or that the value we compare against is a
  // GEP based on the same start value and all offsets are a multiple of the
  // step size, to guarantee that the induction will reach the value.
  if (StepOffset->isZero() || StepOffset->isNegative())
    return;

  if (!StepOffset->isOne()) {
    // Check whether B-Start is known to be a multiple of StepOffset.
    if (!StartSCEV)
      StartSCEV = SE.getSCEV(StartValue);
    const SCEV *BMinusStart = SE.getMinusSCEV(SE.getSCEV(B), StartSCEV);
    if (isa<SCEVCouldNotCompute>(BMinusStart) ||
        !SE.getConstantMultiple(BMinusStart).urem(*StepOffset).isZero())
      return;
  }

  // We already established that B - Start is a multiple of Step above. The loop
  // exits once the compared value reaches B, that is at PN == B when comparing
  // the phi, and at PN + Step == B for a post-increment. Together with the
  // added precondition StartValue <= B for the former and the strict
  // StartValue < B for the latter (which implies StartValue + Step <= B),
  // neither PN nor the increment can wrap.
  CmpInst::Predicate UPrecond = IncStep ? CmpInst::ICMP_ULT : CmpInst::ICMP_ULE;
  ConditionTy StartBeforeBoundUnsigned = {UPrecond, StartValue, B};
  ConditionTy StartBeforeBoundSigned = {ICmpInst::getSignedPredicate(UPrecond),
                                        StartValue, B};

  // Add PN >= StartValue, as the loop exits before wrapping.
  if (!Info.Unsigned)
    WorkList.push_back(FactOrCheck::getConditionFact(
        DTN, CmpInst::ICMP_UGE, PN, StartValue, StartBeforeBoundUnsigned));
  if (!Info.Signed)
    WorkList.push_back(FactOrCheck::getConditionFact(
        DTN, CmpInst::ICMP_SGE, PN, StartValue, StartBeforeBoundSigned));
  // Add PN < B, as the loop exits once the compared value reaches B.
  WorkList.push_back(FactOrCheck::getConditionFact(DTN, CmpInst::ICMP_SLT, PN,
                                                   B, StartBeforeBoundSigned));
  WorkList.push_back(FactOrCheck::getConditionFact(
      DTN, CmpInst::ICMP_ULT, PN, B, StartBeforeBoundUnsigned));

  // Try to add condition from the header or latch to the dedicated exit
  // blocks. When exiting either with EQ or NE, we know that the induction value
  // must be u<= B, as other exits may only exit earlier.
  assert(!StepOffset->isNegative() && "induction must be increasing");
  assert(ContinuePred == CmpInst::ICMP_NE && "unsupported predicate");
  SmallVector<BasicBlock *> ExitBBs;
  L->getExitBlocks(ExitBBs);
  for (BasicBlock *EB : ExitBBs) {
    // Bail out on non-dedicated exits.
    if (DT.dominates(&BB, EB)) {
      WorkList.emplace_back(FactOrCheck::getConditionFact(
          DT.getNode(EB), CmpInst::ICMP_ULE, A, B, StartBeforeBoundUnsigned));
    }
  }
}

static bool getConstraintFromMemoryAccess(GetElementPtrInst &GEP,
                                          uint64_t AccessSize,
                                          CmpPredicate &Pred, Value *&A,
                                          Value *&B, const DataLayout &DL,
                                          const TargetLibraryInfo &TLI) {
  auto Offset = collectOffsets(cast<GEPOperator>(GEP), DL);
  if (!Offset.NW.hasNoUnsignedWrap())
    return false;

  if (Offset.VariableOffsets.size() != 1)
    return false;

  uint64_t BitWidth = Offset.ConstantOffset.getBitWidth();
  auto &[Index, Scale] = Offset.VariableOffsets.front();
  // Bail out on non-canonical GEPs.
  if (Index->getType()->getScalarSizeInBits() != BitWidth)
    return false;

  ObjectSizeOpts Opts;
  // Workaround for gep inbounds, ptr null, idx.
  Opts.NullIsUnknownSize = true;
  // Be conservative since we are not clear on whether an out of bounds access
  // to the padding is UB or not.
  Opts.RoundToAlign = true;
  std::optional<TypeSize> Size =
      getBaseObjectSize(Offset.BasePtr, DL, &TLI, Opts);
  if (!Size || Size->isScalable())
    return false;

  // Index * Scale + ConstOffset + AccessSize <= AllocSize
  // With nuw flag, we know that the index addition doesn't have unsigned wrap.
  // If (AllocSize - (ConstOffset + AccessSize)) wraps around, there is no valid
  // value for Index.
  APInt MaxIndex = (APInt(BitWidth, Size->getFixedValue() - AccessSize,
                          /*isSigned=*/false, /*implicitTrunc=*/true) -
                    Offset.ConstantOffset)
                       .udiv(Scale);
  Pred = ICmpInst::ICMP_ULE;
  A = Index;
  B = ConstantInt::get(Index->getType(), MaxIndex);
  return true;
}

/// Returns true if \p I is a candidate whose poison-generating flags may be
/// strengthened using the constraint systems.
static bool canStrengthenFlags(Instruction *I) {
  auto *BO = dyn_cast<BinaryOperator>(I);
  if (!BO || !BO->getType()->isIntegerTy())
    return false;

  switch (BO->getOpcode()) {
  case Instruction::Sub:
    // A - B does not wrap unsigned, if A >=u B. Subs with constant operands get
    // canonicalized to Add.
    return !BO->hasNoUnsignedWrap() && !isa<Constant>(BO->getOperand(1));
  case Instruction::Mul:
  case Instruction::Shl:
    if (BO->hasNoUnsignedWrap() && BO->hasNoSignedWrap())
      return false;
    // With a constant second operand, we can use bounds on the first operand to
    // refine no-wrap flags. Independently, nuw can be added for nsw if the
    // operands are non-negative.
    return isa<ConstantInt>(BO->getOperand(1)) || BO->hasNoSignedWrap();
  default:
    return false;
  }
}

/// Returns true if \p Info implies that \p Op is in \p R, interpreting \p R as
/// a signed range if \p Signed is set and as an unsigned range otherwise.
static bool doesHoldInRange(const ConstraintInfo &Info, Value *Op,
                            const ConstantRange &R, bool Signed) {
  if (R.isEmptySet() || (Signed ? R.isSignWrappedSet() : R.isWrappedSet()))
    return false;

  if (R.isFullSet())
    return true;

  unsigned BitWidth = R.getBitWidth();
  APInt Min = Signed ? R.getSignedMin() : R.getUnsignedMin();
  APInt Max = Signed ? R.getSignedMax() : R.getUnsignedMax();
  APInt MinVal = Signed ? APInt::getSignedMinValue(BitWidth)
                        : APInt::getMinValue(BitWidth);
  APInt MaxVal = Signed ? APInt::getSignedMaxValue(BitWidth)
                        : APInt::getMaxValue(BitWidth);
  Type *Ty = Op->getType();
  if (Min != MinVal &&
      !Info.doesHold(Signed ? CmpInst::ICMP_SGE : CmpInst::ICMP_UGE, Op,
                     ConstantInt::get(Ty, Min)))
    return false;
  if (Max != MaxVal &&
      !Info.doesHold(Signed ? CmpInst::ICMP_SLE : CmpInst::ICMP_ULE, Op,
                     ConstantInt::get(Ty, Max)))
    return false;
  return true;
}

/// Try to strengthen \p I's poison generating flags using \p Info. Returns
/// true if \p I was modified.
static bool tryToStrengthenFlags(Instruction *I, ConstraintInfo &Info,
                                 SmallVectorImpl<Instruction *> &ToRemove) {
  assert(canStrengthenFlags(I) && "not a candidate for flag strengthening");

  using OBO = OverflowingBinaryOperator;
  Value *Op0 = I->getOperand(0), *Op1 = I->getOperand(1);
  switch (I->getOpcode()) {
  case Instruction::Sub: {
    // Op0 - Op1 does not wrap unsigned, if Op0 >=u Op1.
    if (!Info.doesHold(CmpInst::ICMP_UGE, Op0, Op1))
      return false;
    LLVM_DEBUG(dbgs() << "Adding nuw to " << *I << "\n");
    I->setHasNoUnsignedWrap();
    return true;
  }
  case Instruction::Mul:
  case Instruction::Shl: {
    auto Opcode = static_cast<Instruction::BinaryOps>(I->getOpcode());
    bool Changed = false;
    // For a constant Op1, the ranges of Op0 for which the operation does not
    // wrap are known exactly; check if the systems imply one of them.
    if (auto *C = dyn_cast<ConstantInt>(Op1)) {
      ConstantRange Other(C->getValue());
      if (!I->hasNoUnsignedWrap() &&
          doesHoldInRange(Info, Op0,
                          ConstantRange::makeGuaranteedNoWrapRegion(
                              Opcode, Other, OBO::NoUnsignedWrap),
                          /*Signed=*/false)) {
        LLVM_DEBUG(dbgs() << "Adding nuw to " << *I << "\n");
        I->setHasNoUnsignedWrap();
        Changed = true;
      }
      if (!I->hasNoSignedWrap() &&
          doesHoldInRange(Info, Op0,
                          ConstantRange::makeGuaranteedNoWrapRegion(
                              Opcode, Other, OBO::NoSignedWrap),
                          /*Signed=*/true)) {
        LLVM_DEBUG(dbgs() << "Adding nsw to " << *I << "\n");
        I->setHasNoSignedWrap();
        Changed = true;
      }
    }
    if (!I->hasNoUnsignedWrap() && I->hasNoSignedWrap() &&
        Info.isKnownNonNegative(Op0) &&
        (Opcode == Instruction::Shl || Info.isKnownNonNegative(Op1))) {
      LLVM_DEBUG(dbgs() << "Adding nuw to " << *I << "\n");
      I->setHasNoUnsignedWrap();
      Changed = true;
    }
    return Changed;
  }
  default:
    return false;
  }
}

void State::addInfoFor(BasicBlock &BB) {
  addBoundsForHeaderInductions(BB);
  addInfoForInductions(BB);
  auto &DL = BB.getDataLayout();

  Value *A, *B;
  CmpPredicate Pred;
  // True as long as the current instruction is guaranteed to execute.
  bool GuaranteedToExecute = true;
  // Queue conditions and assumes.
  for (Instruction &I : BB) {
    if (match(&I, m_ICmpLike(Pred, m_Value(), m_Value()))) {
      for (Use &U : I.uses()) {
        auto *UserI = getContextInstForUse(U);
        auto *DTN = DT.getNode(UserI->getParent());
        if (!DTN)
          continue;
        WorkList.push_back(FactOrCheck::getCheck(DTN, &U));
      }
      continue;
    }

    auto AddFactFromMemoryAccess = [&](Value *Ptr, Type *AccessType) {
      auto *GEP = dyn_cast<GetElementPtrInst>(Ptr);
      if (!GEP)
        return;
      TypeSize AccessSize = DL.getTypeStoreSize(AccessType);
      if (!AccessSize.isFixed())
        return;
      if (GuaranteedToExecute) {
        if (getConstraintFromMemoryAccess(*GEP, AccessSize.getFixedValue(),
                                          Pred, A, B, DL, TLI)) {
          // The memory access is guaranteed to execute when BB is entered,
          // hence the constraint holds on entry to BB.
          WorkList.emplace_back(FactOrCheck::getConditionFact(
              DT.getNode(I.getParent()), Pred, A, B));
        }
      } else {
        WorkList.emplace_back(
            FactOrCheck::getInstFact(DT.getNode(I.getParent()), &I));
      }
    };

    if (auto *LI = dyn_cast<LoadInst>(&I)) {
      if (!LI->isVolatile())
        AddFactFromMemoryAccess(LI->getPointerOperand(), LI->getAccessType());
    }
    if (auto *SI = dyn_cast<StoreInst>(&I)) {
      if (!SI->isVolatile())
        AddFactFromMemoryAccess(SI->getPointerOperand(), SI->getAccessType());
    }

    auto *II = dyn_cast<IntrinsicInst>(&I);
    Intrinsic::ID ID = II ? II->getIntrinsicID() : Intrinsic::not_intrinsic;
    switch (ID) {
    case Intrinsic::assume: {
      if (!match(I.getOperand(0), m_ICmpLike(Pred, m_Value(A), m_Value(B))))
        break;
      if (GuaranteedToExecute) {
        // The assume is guaranteed to execute when BB is entered, hence Cond
        // holds on entry to BB.
        WorkList.emplace_back(FactOrCheck::getConditionFact(
            DT.getNode(I.getParent()), Pred, A, B));
      } else {
        WorkList.emplace_back(
            FactOrCheck::getInstFact(DT.getNode(I.getParent()), &I));
      }
      break;
    }
    // Enqueue sadd/ssub_with_overflow for simplification.
    case Intrinsic::sadd_with_overflow:
    case Intrinsic::ssub_with_overflow:
    case Intrinsic::ucmp:
    case Intrinsic::scmp:
      WorkList.push_back(
          FactOrCheck::getCheck(DT.getNode(&BB), cast<CallInst>(&I)));
      break;
    // Enqueue the intrinsics to add extra info.
    case Intrinsic::umin:
    case Intrinsic::umax:
    case Intrinsic::smin:
    case Intrinsic::smax:
    case Intrinsic::usub_sat:
      // TODO: handle llvm.abs as well
      WorkList.push_back(
          FactOrCheck::getCheck(DT.getNode(&BB), cast<CallInst>(&I)));
      [[fallthrough]];
    case Intrinsic::uadd_sat:
      // TODO: Check if it is possible to instead only added the min/max facts
      // when simplifying uses of the min/max intrinsics.
      if (!isGuaranteedNotToBePoison(&I))
        break;
      [[fallthrough]];
    case Intrinsic::abs:
      WorkList.push_back(FactOrCheck::getInstFact(DT.getNode(&BB), &I));
      break;
    }

    // Add facts from unsigned division, remainder and logical shift right, and
    // from signed division and remainder.
    //   urem x, n: result < n  and  result <= x
    //   udiv x, n: result <= x
    //   lshr x, n: result <= x
    //   srem x, n: result >= 0 and result <= x, if x >= 0
    //              result < n,                  if n > 0
    //   sdiv x, C: result >= 0 and result <= x, if x >= 0 and C > 1
    //              result < x,                  if x > 0 and C > 1
    if (auto *BO = dyn_cast<BinaryOperator>(&I)) {
      if ((BO->getOpcode() == Instruction::URem ||
           BO->getOpcode() == Instruction::UDiv ||
           BO->getOpcode() == Instruction::LShr ||
           BO->getOpcode() == Instruction::SRem ||
           BO->getOpcode() == Instruction::SDiv) &&
          isGuaranteedNotToBePoison(BO))
        WorkList.push_back(FactOrCheck::getInstFact(DT.getNode(&BB), BO));
    }

    // Queue instructions whose flags may be strengthened, checked at the
    // closest point dominating all uses. That point may be strictly below the
    // defining block: if the operation does not wrap there, any use of a
    // wrapped (and hence poison) result is unreachable, so recording the flag
    // on the definition remains valid.
    if (canStrengthenFlags(&I))
      if (Instruction *CommonDom = findCommonDominatorOfUses(I, DT))
        WorkList.push_back(FactOrCheck::getCheck(
            DT.getNode(CommonDom->getParent()), &I, CommonDom));

    GuaranteedToExecute &= isGuaranteedToTransferExecutionToSuccessor(&I);
  }

  if (auto *Switch = dyn_cast<SwitchInst>(BB.getTerminator())) {
    for (auto &Case : Switch->cases()) {
      BasicBlock *Succ = Case.getCaseSuccessor();
      Value *V = Case.getCaseValue();
      if (!canAddSuccessor(BB, Succ))
        continue;
      WorkList.emplace_back(FactOrCheck::getConditionFact(
          DT.getNode(Succ), CmpInst::ICMP_EQ, Switch->getCondition(), V));
    }
    return;
  }

  auto *Br = dyn_cast<CondBrInst>(BB.getTerminator());
  if (!Br)
    return;

  Value *Cond = Br->getCondition();

  // If the condition is a chain of ORs/AND and the successor only has the
  // current block as predecessor, queue conditions for the successor.
  Value *Op0, *Op1;
  if (match(Cond, m_LogicalOr(m_Value(Op0), m_Value(Op1))) ||
      match(Cond, m_LogicalAnd(m_Value(Op0), m_Value(Op1)))) {
    bool IsOr = match(Cond, m_LogicalOr());
    bool IsAnd = match(Cond, m_LogicalAnd());
    // If there's a select that matches both AND and OR, we need to commit to
    // one of the options. Arbitrarily pick OR.
    if (IsOr && IsAnd)
      IsAnd = false;

    BasicBlock *Successor = Br->getSuccessor(IsOr ? 1 : 0);
    if (canAddSuccessor(BB, Successor)) {
      SmallVector<Value *> CondWorkList;
      SmallPtrSet<Value *, 8> SeenCond;
      auto QueueValue = [&CondWorkList, &SeenCond](Value *V) {
        if (SeenCond.insert(V).second)
          CondWorkList.push_back(V);
      };
      QueueValue(Op1);
      QueueValue(Op0);
      while (!CondWorkList.empty()) {
        Value *Cur = CondWorkList.pop_back_val();
        if (match(Cur, m_ICmpLike(Pred, m_Value(A), m_Value(B)))) {
          WorkList.emplace_back(FactOrCheck::getConditionFact(
              DT.getNode(Successor),
              IsOr ? CmpPredicate::getInverse(Pred) : Pred, A, B));
          continue;
        }
        if (IsOr && match(Cur, m_LogicalOr(m_Value(Op0), m_Value(Op1)))) {
          QueueValue(Op1);
          QueueValue(Op0);
          continue;
        }
        if (IsAnd && match(Cur, m_LogicalAnd(m_Value(Op0), m_Value(Op1)))) {
          QueueValue(Op1);
          QueueValue(Op0);
          continue;
        }
      }
    }
    return;
  }

  if (!match(Br->getCondition(), m_ICmpLike(Pred, m_Value(A), m_Value(B))))
    return;
  if (canAddSuccessor(BB, Br->getSuccessor(0)))
    WorkList.emplace_back(FactOrCheck::getConditionFact(
        DT.getNode(Br->getSuccessor(0)), Pred, A, B));
  if (canAddSuccessor(BB, Br->getSuccessor(1)))
    WorkList.emplace_back(FactOrCheck::getConditionFact(
        DT.getNode(Br->getSuccessor(1)), CmpPredicate::getInverse(Pred), A, B));
}

#ifndef NDEBUG
static void dumpUnpackedICmp(raw_ostream &OS, ICmpInst::Predicate Pred,
                             Value *LHS, Value *RHS) {
  OS << "icmp " << Pred << ' ';
  LHS->printAsOperand(OS, /*PrintType=*/true);
  OS << ", ";
  RHS->printAsOperand(OS, /*PrintType=*/false);
}
#endif

namespace {
/// Helper to keep track of a condition and if it should be treated as negated
/// for reproducer construction.
/// Pred == Predicate::BAD_ICMP_PREDICATE indicates that this entry is a
/// placeholder to keep the ReproducerCondStack in sync with DFSInStack.
struct ReproducerEntry {
  ICmpInst::Predicate Pred;
  Value *LHS;
  Value *RHS;

  ReproducerEntry(ICmpInst::Predicate Pred, Value *LHS, Value *RHS)
      : Pred(Pred), LHS(LHS), RHS(RHS) {}
};
} // namespace

/// Helper function to generate a reproducer function for simplifying \p Cond.
/// The reproducer function contains a series of @llvm.assume calls, one for
/// each condition in \p Stack. For each condition, the operand instruction are
/// cloned until we reach operands that have an entry in \p Value2Index. Those
/// will then be added as function arguments. \p DT is used to order cloned
/// instructions. The reproducer function will get added to \p M, if it is
/// non-null. Otherwise no reproducer function is generated.
static void generateReproducer(Instruction *Cond, bool IsSigned, Module *M,
                               ArrayRef<ReproducerEntry> Stack,
                               ConstraintInfo &Info, DominatorTree &DT) {
  if (!M)
    return;

  LLVMContext &Ctx = Cond->getContext();

  LLVM_DEBUG(dbgs() << "Creating reproducer for " << *Cond << "\n");

  ValueToValueMapTy Old2New;
  SmallVector<Value *> Args;
  SmallPtrSet<Value *, 8> Seen;
  // Traverse Cond and its operands recursively until we reach a value that's in
  // Value2Index or not an instruction, or not a operation that
  // ConstraintElimination can decompose. Such values will be considered as
  // external inputs to the reproducer, they are collected and added as function
  // arguments later.
  auto CollectArguments = [&](ArrayRef<Value *> Ops, bool IsSigned) {
    auto &Value2Index = Info.getValue2Index(IsSigned);
    SmallVector<Value *, 4> WorkList(Ops);
    while (!WorkList.empty()) {
      Value *V = WorkList.pop_back_val();
      if (!Seen.insert(V).second)
        continue;
      if (Old2New.find(V) != Old2New.end())
        continue;
      if (isa<Constant>(V))
        continue;

      auto *I = dyn_cast<Instruction>(V);
      if (Value2Index.contains(V) || !I ||
          !isa<CmpInst, BinaryOperator, GEPOperator, CastInst>(V)) {
        Old2New[V] = V;
        Args.push_back(V);
        LLVM_DEBUG(dbgs() << "  found external input " << *V << "\n");
      } else {
        append_range(WorkList, I->operands());
      }
    }
  };

  for (auto &Entry : Stack)
    if (Entry.Pred != ICmpInst::BAD_ICMP_PREDICATE)
      CollectArguments({Entry.LHS, Entry.RHS}, ICmpInst::isSigned(Entry.Pred));
  CollectArguments(Cond, IsSigned);

  SmallVector<Type *> ParamTys;
  for (auto *P : Args)
    ParamTys.push_back(P->getType());

  FunctionType *FTy = FunctionType::get(Cond->getType(), ParamTys,
                                        /*isVarArg=*/false);
  Function *F = Function::Create(FTy, Function::ExternalLinkage,
                                 Cond->getModule()->getName() +
                                     Cond->getFunction()->getName() + "repro",
                                 M);
  // Add arguments to the reproducer function for each external value collected.
  for (unsigned I = 0; I < Args.size(); ++I) {
    F->getArg(I)->setName(Args[I]->getName());
    Old2New[Args[I]] = F->getArg(I);
  }

  BasicBlock *Entry = BasicBlock::Create(Ctx, "entry", F);
  IRBuilder<> Builder(Entry);
  Builder.CreateRet(Builder.getTrue());
  Builder.SetInsertPoint(Entry->getTerminator());

  // Clone instructions in \p Ops and their operands recursively until reaching
  // an value in Value2Index (external input to the reproducer). Update Old2New
  // mapping for the original and cloned instructions. Sort instructions to
  // clone by dominance, then insert the cloned instructions in the function.
  auto CloneInstructions = [&](ArrayRef<Value *> Ops, bool IsSigned) {
    SmallVector<Value *, 4> WorkList(Ops);
    SmallVector<Instruction *> ToClone;
    auto &Value2Index = Info.getValue2Index(IsSigned);
    while (!WorkList.empty()) {
      Value *V = WorkList.pop_back_val();
      if (Old2New.find(V) != Old2New.end())
        continue;

      auto *I = dyn_cast<Instruction>(V);
      if (!Value2Index.contains(V) && I) {
        Old2New[V] = nullptr;
        ToClone.push_back(I);
        append_range(WorkList, I->operands());
      }
    }

    sort(ToClone,
         [&DT](Instruction *A, Instruction *B) { return DT.dominates(A, B); });
    for (Instruction *I : ToClone) {
      Instruction *Cloned = I->clone();
      Old2New[I] = Cloned;
      Old2New[I]->setName(I->getName());
      Cloned->insertBefore(Builder.GetInsertPoint());
      Cloned->dropUnknownNonDebugMetadata();
      Cloned->setDebugLoc({});
    }
  };

  // Materialize the assumptions for the reproducer using the entries in Stack.
  // That is, first clone the operands of the condition recursively until we
  // reach an external input to the reproducer and add them to the reproducer
  // function. Then add an ICmp for the condition (with the inverse predicate if
  // the entry is negated) and an assert using the ICmp.
  for (auto &Entry : Stack) {
    if (Entry.Pred == ICmpInst::BAD_ICMP_PREDICATE)
      continue;

    LLVM_DEBUG(dbgs() << "  Materializing assumption ";
               dumpUnpackedICmp(dbgs(), Entry.Pred, Entry.LHS, Entry.RHS);
               dbgs() << "\n");
    CloneInstructions({Entry.LHS, Entry.RHS}, CmpInst::isSigned(Entry.Pred));

    auto *Cmp = Builder.CreateICmp(Entry.Pred, Entry.LHS, Entry.RHS);
    Builder.CreateAssumption(Cmp);
  }

  // Finally, clone the condition to reproduce and remap instruction operands in
  // the reproducer using Old2New.
  CloneInstructions(Cond, IsSigned);
  Entry->getTerminator()->setOperand(0, Cond);
  remapInstructionsInBlocks({Entry}, Old2New);

  assert(!verifyFunction(*F, &dbgs()));
}

static std::optional<bool> checkCondition(CmpInst::Predicate Pred, Value *A,
                                          Value *B, Instruction *CheckInst,
                                          ConstraintInfo &Info) {
  LLVM_DEBUG(dbgs() << "Checking " << *CheckInst << "\n");

  auto TryWithConstraint = [&](const ConstraintTy &R) -> std::optional<bool> {
    if (R.empty()) {
      LLVM_DEBUG(dbgs() << "   failed to decompose condition\n");
      return std::nullopt;
    }

    auto &CSToUse = Info.getCS(R.IsSigned);
    if (auto ImpliedCondition = R.isImpliedBy(CSToUse)) {
      if (!DebugCounter::shouldExecute(EliminatedCounter))
        return std::nullopt;
      LLVM_DEBUG({
        dbgs() << "Condition ";
        dumpUnpackedICmp(dbgs(),
                         *ImpliedCondition ? Pred
                                           : CmpInst::getInversePredicate(Pred),
                         A, B);
        dbgs() << " implied by dominating constraints\n";
        CSToUse.dump();
      });
      return ImpliedCondition;
    }
    return std::nullopt;
  };

  auto R = Info.getConstraintForSolving(Pred, A, B);
  if (auto ImpliedCondition = TryWithConstraint(R))
    return ImpliedCondition;

  // For non-negative operands unsigned queries can also be checked against the
  // signed system.
  if (CmpInst::isUnsigned(Pred) && A->getType()->isIntegerTy()) {
    SmallVector<Value *> NewVariables;
    auto SR = Info.getConstraint(ICmpInst::getSignedPredicate(Pred), A, B,
                                 NewVariables);
    if (NewVariables.empty() && !SR.empty() && Info.isKnownNonNegative(A) &&
        Info.isKnownNonNegative(B))
      if (auto ImpliedCondition = TryWithConstraint(SR))
        return ImpliedCondition;
  }

  // Additionally, query the signed system for eq/ne predicates if we know about
  // A or B.
  if (CmpInst::isEquality(Pred)) {
    const auto &Value2Index = Info.getValue2Index(/*Signed=*/true);
    if (!Value2Index.contains(A) && !Value2Index.contains(B))
      return std::nullopt;

    SmallVector<Value *> NewVariables;
    auto SR = Info.getConstraint(Pred, A, B, NewVariables,
                                 /*ForceSignedSystem=*/true);
    if (NewVariables.empty())
      if (auto ImpliedCondition = TryWithConstraint(SR))
        return ImpliedCondition;
  }
  return std::nullopt;
}

static bool checkAndReplaceCondition(
    CmpPredicate Pred, Value *A, Value *B, Instruction *CheckInst,
    ConstraintInfo &Info, unsigned NumIn, unsigned NumOut,
    Instruction *ContextInst, Module *ReproducerModule,
    ArrayRef<ReproducerEntry> ReproducerCondStack, DominatorTree &DT,
    SmallVectorImpl<Instruction *> &ToRemove) {
  auto ReplaceCmpWithConstant = [&](Instruction *CheckInst, bool IsTrue) {
    generateReproducer(CheckInst, ICmpInst::isSigned(Pred), ReproducerModule,
                       ReproducerCondStack, Info, DT);
    Constant *ConstantC = ConstantInt::getBool(
        CmpInst::makeCmpResultType(CheckInst->getType()), IsTrue);
    bool Changed = CheckInst->replaceUsesWithIf(ConstantC, [&](Use &U) {
      auto *UserI = getContextInstForUse(U);
      auto *DTN = DT.getNode(UserI->getParent());
      if (!DTN || DTN->getDFSNumIn() < NumIn || DTN->getDFSNumOut() > NumOut)
        return false;
      if (UserI->getParent() == ContextInst->getParent() &&
          UserI->comesBefore(ContextInst))
        return false;

      // Conditions in an assume trivially simplify to true. Skip uses
      // in assume calls to not destroy the available information.
      auto *II = dyn_cast<IntrinsicInst>(U.getUser());
      return !II || II->getIntrinsicID() != Intrinsic::assume;
    });
    NumCondsRemoved++;

    // Update the debug value records that satisfy the same condition used
    // in replaceUsesWithIf.
    SmallVector<DbgVariableRecord *> DVRUsers;
    findDbgUsers(CheckInst, DVRUsers);

    for (auto *DVR : DVRUsers) {
      auto *DTN = DT.getNode(DVR->getParent());
      if (!DTN || DTN->getDFSNumIn() < NumIn || DTN->getDFSNumOut() > NumOut)
        continue;

      auto *MarkedI = DVR->getInstruction();
      if (MarkedI->getParent() == ContextInst->getParent() &&
          MarkedI->comesBefore(ContextInst))
        continue;

      DVR->replaceVariableLocationOp(CheckInst, ConstantC);
    }

    if (CheckInst->use_empty())
      ToRemove.push_back(CheckInst);

    return Changed;
  };

  if (auto ImpliedCondition = checkCondition(Pred, A, B, CheckInst, Info))
    return ReplaceCmpWithConstant(CheckInst, *ImpliedCondition);

  // When the predicate is samesign and unsigned, we can also make use of the
  // signed predicate information.
  if (Pred.hasSameSign() && ICmpInst::isUnsigned(Pred))
    if (auto ImpliedCondition = checkCondition(
            ICmpInst::getSignedPredicate(Pred), A, B, CheckInst, Info))
      return ReplaceCmpWithConstant(CheckInst, *ImpliedCondition);

  return false;
}

static bool checkAndReplaceMinMax(MinMaxIntrinsic *MinMax, ConstraintInfo &Info,
                                  SmallVectorImpl<Instruction *> &ToRemove) {
  auto ReplaceMinMaxWithOperand = [&](MinMaxIntrinsic *MinMax, bool UseLHS) {
    // TODO: generate reproducer for min/max.
    MinMax->replaceAllUsesWith(MinMax->getOperand(UseLHS ? 0 : 1));
    ToRemove.push_back(MinMax);
    return true;
  };

  ICmpInst::Predicate Pred =
      ICmpInst::getNonStrictPredicate(MinMax->getPredicate());
  if (auto ImpliedCondition = checkCondition(
          Pred, MinMax->getOperand(0), MinMax->getOperand(1), MinMax, Info))
    return ReplaceMinMaxWithOperand(MinMax, *ImpliedCondition);
  if (auto ImpliedCondition = checkCondition(
          Pred, MinMax->getOperand(1), MinMax->getOperand(0), MinMax, Info))
    return ReplaceMinMaxWithOperand(MinMax, !*ImpliedCondition);
  return false;
}

static bool checkAndReplaceCmp(CmpIntrinsic *I, ConstraintInfo &Info,
                               SmallVectorImpl<Instruction *> &ToRemove) {
  Value *LHS = I->getOperand(0);
  Value *RHS = I->getOperand(1);
  if (checkCondition(I->getGTPredicate(), LHS, RHS, I, Info).value_or(false)) {
    I->replaceAllUsesWith(ConstantInt::get(I->getType(), 1));
    ToRemove.push_back(I);
    return true;
  }
  if (checkCondition(I->getLTPredicate(), LHS, RHS, I, Info).value_or(false)) {
    I->replaceAllUsesWith(ConstantInt::getSigned(I->getType(), -1));
    ToRemove.push_back(I);
    return true;
  }
  if (checkCondition(ICmpInst::ICMP_EQ, LHS, RHS, I, Info).value_or(false)) {
    I->replaceAllUsesWith(ConstantInt::get(I->getType(), 0));
    ToRemove.push_back(I);
    return true;
  }
  return false;
}

/// Try to replace \p USub by a plain subtract, if \p Info proves it cannot
/// saturate. Returns true if \p USub was replaced.
static bool checkAndReplaceUSubSat(SaturatingInst *USub, ConstraintInfo &Info,
                                   SmallVectorImpl<Instruction *> &ToRemove) {
  // usub.sat(A, B) is A - B exactly when A >=u B.
  Value *A = USub->getLHS();
  Value *B = USub->getRHS();
  if (!checkCondition(CmpInst::ICMP_UGE, A, B, USub, Info).value_or(false))
    return false;

  IRBuilder<> Builder(USub);
  Value *Sub = Builder.CreateSub(A, B, "", /*HasNUW=*/true,
                                 /*HasNSW=*/Info.isKnownNonNegative(A));
  USub->replaceAllUsesWith(Sub);
  Sub->takeName(USub);
  ToRemove.push_back(USub);
  return true;
}

static void
removeEntryFromStack(const StackEntry &E, ConstraintInfo &Info,
                     Module *ReproducerModule,
                     SmallVectorImpl<ReproducerEntry> &ReproducerCondStack,
                     SmallVectorImpl<StackEntry> &DFSInStack) {
  Info.popLastConstraint(E.IsSigned);
  // Remove variables in the system that went out of scope.
  auto &Mapping = Info.getValue2Index(E.IsSigned);
  for (Value *V : E.ValuesToRelease)
    Mapping.erase(V);
  Info.popLastNVariables(E.IsSigned, E.ValuesToRelease.size());
  DFSInStack.pop_back();
  if (ReproducerModule)
    ReproducerCondStack.pop_back();
}

/// Check if either the first condition of an AND or OR is implied by the
/// (negated in case of OR) second condition or vice versa.
static bool checkOrAndOpImpliedByOther(
    FactOrCheck &CB, ConstraintInfo &Info, Module *ReproducerModule,
    SmallVectorImpl<ReproducerEntry> &ReproducerCondStack,
    SmallVectorImpl<StackEntry> &DFSInStack,
    SmallVectorImpl<Instruction *> &ToRemove) {
  Instruction *JoinOp = CB.getContextInst();
  if (JoinOp->use_empty())
    return false;

  Instruction *CmpToCheck = cast<Instruction>(CB.getInstructionToSimplify());
  unsigned OtherOpIdx = JoinOp->getOperand(0) == CmpToCheck ? 1 : 0;

  // Don't try to simplify the first condition of a select by the second, as
  // this may make the select more poisonous than the original one.
  // TODO: check if the first operand may be poison.
  if (OtherOpIdx != 0 && isa<SelectInst>(JoinOp))
    return false;

  unsigned OldSize = DFSInStack.size();
  llvm::scope_exit InfoRestorer([&]() {
    // Remove entries again.
    while (OldSize < DFSInStack.size()) {
      StackEntry E = DFSInStack.back();
      removeEntryFromStack(E, Info, ReproducerModule, ReproducerCondStack,
                           DFSInStack);
    }
  });
  bool IsOr = match(JoinOp, m_LogicalOr());
  SmallVector<Value *, 4> Worklist({JoinOp->getOperand(OtherOpIdx)});
  // Do a traversal of the AND/OR tree to add facts from leaf compares.
  while (!Worklist.empty()) {
    Value *Val = Worklist.pop_back_val();
    Value *LHS, *RHS;
    CmpPredicate Pred;
    if (match(Val, m_ICmpLike(Pred, m_Value(LHS), m_Value(RHS)))) {
      // For OR, check if the negated condition implies CmpToCheck.
      if (IsOr)
        Pred = CmpInst::getInversePredicate(Pred);
      // Optimistically add fact from the other compares in the AND/OR.
      Info.addFact(Pred, LHS, RHS, CB.NumIn, CB.NumOut, DFSInStack);
      continue;
    }
    if (IsOr ? match(Val, m_LogicalOr(m_Value(LHS), m_Value(RHS)))
             : match(Val, m_LogicalAnd(m_Value(LHS), m_Value(RHS)))) {
      Worklist.push_back(LHS);
      Worklist.push_back(RHS);
    }
  }
  if (OldSize == DFSInStack.size())
    return false;

  Value *A, *B;
  CmpPredicate Pred;
  [[maybe_unused]] bool Matched =
      match(CmpToCheck, m_ICmpLike(Pred, m_Value(A), m_Value(B)));
  assert(Matched && "expected icmp-like match");
  // Check if the second condition can be simplified now.
  if (auto ImpliedCondition = checkCondition(Pred, A, B, CmpToCheck, Info)) {
    if (IsOr == *ImpliedCondition)
      JoinOp->replaceAllUsesWith(
          ConstantInt::getBool(JoinOp->getType(), *ImpliedCondition));
    else
      JoinOp->replaceAllUsesWith(JoinOp->getOperand(OtherOpIdx));
    ToRemove.push_back(JoinOp);
    return true;
  }

  return false;
}

void ConstraintInfo::addFact(CmpInst::Predicate Pred, Value *A, Value *B,
                             unsigned NumIn, unsigned NumOut,
                             SmallVectorImpl<StackEntry> &DFSInStack) {
  addFactImpl(Pred, A, B, NumIn, NumOut, DFSInStack, false);
  // If the Pred is eq/ne, also add the fact to signed system.
  if (CmpInst::isEquality(Pred))
    addFactImpl(Pred, A, B, NumIn, NumOut, DFSInStack, true);
  if (Pred == CmpInst::ICMP_NE)
    tightenBoundUsingNe(A, B, NumIn, NumOut, DFSInStack);
}

void ConstraintInfo::addScaledFact(int64_t Scale, Value *A, Value *B,
                                   unsigned NumIn, unsigned NumOut,
                                   SmallVectorImpl<StackEntry> &DFSInStack) {
  LLVM_DEBUG(dbgs() << "Adding scaled fact: " << Scale << " * " << *A
                    << " u<= " << *B << "\n");
  addFactImpl(CmpInst::ICMP_ULE, A, B, NumIn, NumOut, DFSInStack,
              /*ForceSignedSystem=*/false, Scale);
}

void ConstraintInfo::tightenBoundUsingNe(
    Value *A, Value *B, unsigned NumIn, unsigned NumOut,
    SmallVectorImpl<StackEntry> &DFSInStack) {
  if (!A->getType()->isIntegerTy())
    return;

  for (bool IsSigned : {false, true}) {
    // In the unsigned system `A u>= 0` holds for every A, so getConstraint
    // already turned `A != 0` into `A u> 0`.
    if (!IsSigned && match(B, m_Zero()))
      continue;

    // Skip if there are any unknown variables.
    const auto &Value2Index = getValue2Index(IsSigned);
    if (any_of(decompose(A, *this, IsSigned, DL).Vars,
               [&Value2Index](const DecompEntry &E) {
                 return !Value2Index.contains(E.Variable);
               }))
      continue;

    // If the system implies `A >= B` then together with `A != B` we get the
    // strict `A > B`; symmetrically `A <= B` becomes `A < B`.
    CmpInst::Predicate GEPred =
        IsSigned ? CmpInst::ICMP_SGE : CmpInst::ICMP_UGE;
    CmpInst::Predicate LEPred =
        IsSigned ? CmpInst::ICMP_SLE : CmpInst::ICMP_ULE;
    for (CmpInst::Predicate NonStrict : {GEPred, LEPred}) {
      if (!doesHold(NonStrict, A, B))
        continue;
      CmpInst::Predicate Strict = CmpInst::getStrictPredicate(NonStrict);
      LLVM_DEBUG(dbgs() << "Tightening '";
                 dumpUnpackedICmp(dbgs(), NonStrict, A, B); dbgs() << "' to '";
                 dumpUnpackedICmp(dbgs(), Strict, A, B);
                 dbgs() << "' using inequality\n");
      addFactImpl(Strict, A, B, NumIn, NumOut, DFSInStack,
                  /*ForceSignedSystem=*/false);
      break;
    }
  }
}

void ConstraintInfo::addFactImpl(CmpInst::Predicate Pred, Value *A, Value *B,
                                 unsigned NumIn, unsigned NumOut,
                                 SmallVectorImpl<StackEntry> &DFSInStack,
                                 bool ForceSignedSystem, int64_t Op0Scale) {
  SmallVector<Value *> NewVariables;
  auto R = getConstraint(Pred, A, B, NewVariables, ForceSignedSystem, Op0Scale);

  // TODO: Support non-equality for facts as well.
  if (R.empty() || R.isNe())
    return;

  LLVM_DEBUG(dbgs() << "Adding '"; dumpUnpackedICmp(dbgs(), Pred, A, B);
             dbgs() << "'\n");
  auto &CSToUse = getCS(R.IsSigned);
  bool Added = CSToUse.addRow(R.Coefficients, R.NumVars);
  if (!Added)
    return;

  // If R has been added to the system, add the new variables and queue it for
  // removal once it goes out-of-scope.
  SmallVector<Value *, 2> ValuesToRelease;
  auto &Value2Index = getValue2Index(R.IsSigned);
  for (Value *V : NewVariables) {
    Value2Index.try_emplace(V, Value2Index.size() + 1);
    ValuesToRelease.push_back(V);
  }

  LLVM_DEBUG({
    dbgs() << "  constraint: ";
    dumpConstraint(R.Coefficients, getValue2Index(R.IsSigned));
    dbgs() << "\n";
  });

  DFSInStack.emplace_back(NumIn, NumOut, R.IsSigned,
                          std::move(ValuesToRelease));

  if (!R.IsSigned) {
    for (Value *V : NewVariables) {
      // Add V > -1 constraints for all new variables.
      CSToUse.addRow({Entry(0, 0), Entry(-1, Value2Index.at(V))},
                     Value2Index.size());
      DFSInStack.emplace_back(NumIn, NumOut, R.IsSigned,
                              SmallVector<Value *, 2>());
    }
  }

  if (R.isEq()) {
    // Also add the inverted constraint for equality constraints.
    for (Entry &E : R.Coefficients)
      if (MulOverflow(E.Coefficient, int64_t(-1), E.Coefficient))
        return;
    CSToUse.addRow(R.Coefficients, R.NumVars);

    DFSInStack.emplace_back(NumIn, NumOut, R.IsSigned,
                            SmallVector<Value *, 2>());
  }
}

/// Replace the uses of \p WO, which has been proven not to overflow, by a plain
/// binary operator and \p false for the overflow flag.
static bool replaceOverflowUses(WithOverflowInst *WO, Value *A, Value *B,
                                SmallVectorImpl<Instruction *> &ToRemove) {
  bool Changed = false;
  IRBuilder<> Builder(WO->getParent(), WO->getIterator());
  Value *Res = nullptr;
  for (User *U : make_early_inc_range(WO->users())) {
    if (match(U, m_ExtractValue<0>(m_Value()))) {
      if (!Res) {
        Res = Builder.CreateBinOp(WO->getBinaryOp(), A, B);
        // The uses are only replaced after proving the operation cannot
        // overflow in the intrinsic's signedness domain, so the replacement can
        // carry the matching no-wrap flag. Keeping the flag matters because the
        // guard that established it is usually folded away at the same time,
        // and later passes (SCEV/IndVarSimplify) can then no longer re-derive
        // the bound.
        if (auto *BO = dyn_cast<BinaryOperator>(Res)) {
          if (WO->isSigned())
            BO->setHasNoSignedWrap();
          else
            BO->setHasNoUnsignedWrap();
        }
      }
      U->replaceAllUsesWith(Res);
      Changed = true;
    } else if (match(U, m_ExtractValue<1>(m_Value()))) {
      U->replaceAllUsesWith(Builder.getFalse());
      Changed = true;
    } else
      continue;

    if (U->use_empty()) {
      auto *I = cast<Instruction>(U);
      ToRemove.push_back(I);
      I->setOperand(0, PoisonValue::get(WO->getType()));
      Changed = true;
    }
  }

  if (WO->use_empty()) {
    // Do not erase WO here: the worklist may still hold Uses of WO's operands.
    for (Use &Arg : WO->args())
      Arg.set(PoisonValue::get(Arg->getType()));
    ToRemove.push_back(WO);
    Changed = true;
  }
  return Changed;
}

/// Try to prove that `sadd.with.overflow(A, B)` with a *variable* B does not
/// overflow, by bounding the mathematical sum from both sides.
///
/// The solver has no name for `A + B`, so neither operand alone can carry the
/// bound; the sum is passed to getConstraint as an addend instead, which folds
/// it into one side of an ordinary two-operand query.
///
/// Bounding the sum by two other program values proves it is in range: a value
/// W of the same type always satisfies SMIN s<= W s<= SMAX and its
/// decomposition is an exact representation of it, so `Lo s<= A + B s<= Hi`
/// leaves no room for overflow in either direction.  A witness must dominate
/// the intrinsic: a value defined below it may be derived from the very sum in
/// question (`A + B + 1`, say), and bounding the sum by itself proves nothing.
static bool
tryToSimplifyVariableSAdd(WithOverflowInst *WO, ConstraintInfo &Info,
                          DominatorTree &DT,
                          SmallVectorImpl<Instruction *> &ToRemove) {
  Value *A = WO->getLHS();
  Value *B = WO->getRHS();
  Type *Ty = A->getType();

  // Query the signed system directly.  Going through doesHold() would let the
  // query be answered by the *unsigned* system, where a bound only limits the
  // sum to UMAX and says nothing about signed overflow.  Only `s<=` is used so
  // the addend stays on the side it was passed for.
  auto SumIsBelow = [&](Value *Lo, Value *LoAddend, Value *Hi,
                        Value *HiAddend) {
    SmallVector<Value *> NewVariables;
    ConstraintTy R =
        Info.getConstraint(CmpInst::ICMP_SLE, Lo, Hi, NewVariables,
                           /*ForceSignedSystem=*/false, /*Op0Scale=*/1,
                           LoAddend, HiAddend);
    if (!NewVariables.empty() || R.empty())
      return false;
    assert(R.IsSigned && "signed predicate must build a signed constraint");
    return Info.getCS(/*Signed=*/true)
        .isConditionImpliedInSubSystem(R.Coefficients);
  };

  // `0 s<= A + B` is the cheap and by far most common lower bound: it holds
  // whenever both operands are known non-negative.
  Value *Zero = ConstantInt::getNullValue(Ty);
  bool HasLower = SumIsBelow(Zero, nullptr, A, B);
  bool HasUpper = false;
  for (auto &KV : Info.getValue2Index(/*Signed=*/true)) {
    if (HasLower && HasUpper)
      break;
    Value *W = KV.first;
    if (W->getType() != Ty || isa<Constant>(W))
      continue;
    auto *I = dyn_cast<Instruction>(W);
    if (I && (I == WO || !DT.dominates(I, WO)))
      continue;
    if (!HasUpper && SumIsBelow(A, B, W, nullptr))
      HasUpper = true;
    if (!HasLower && SumIsBelow(W, nullptr, A, B))
      HasLower = true;
  }
  if (!HasLower || !HasUpper)
    return false;

  return replaceOverflowUses(WO, A, B, ToRemove);
}

static bool
tryToSimplifyOverflowMath(WithOverflowInst *WO, ConstraintInfo &Info,
                          ScalarEvolution &SE, DominatorTree &DT,
                          SmallVectorImpl<Instruction *> &ToRemove) {
  auto DoesConditionHold = [](CmpInst::Predicate Pred, Value *A, Value *B,
                              ConstraintInfo &Info) {
    auto R = Info.getConstraintForSolving(Pred, A, B);
    // Nothing can be proven if the constraint has no variables. This also
    // covers rows that could not be decomposed, which are empty.
    if (R.isConstantOnly())
      return false;

    auto &CSToUse = Info.getCS(R.IsSigned);
    return CSToUse.isConditionImpliedInSubSystem(R.Coefficients);
  };

  bool Changed = false;
  if (WO->getIntrinsicID() == Intrinsic::ssub_with_overflow) {
    // If A s>= B && B s>= 0, ssub.with.overflow(a, b) should not overflow and
    // can be simplified to a regular sub.
    Value *A = WO->getLHS();
    Value *B = WO->getRHS();
    if (!DoesConditionHold(CmpInst::ICMP_SGE, A, B, Info) ||
        !DoesConditionHold(CmpInst::ICMP_SGE, B,
                           ConstantInt::get(A->getType(), 0), Info))
      return false;
    Changed = replaceOverflowUses(WO, A, B, ToRemove);
  } else if (WO->getIntrinsicID() == Intrinsic::sadd_with_overflow) {
    // sadd.with.overflow(A, C) with a constant C does not overflow when A stays
    // within the signed range after adding C: for C s>= 0 that needs
    // A s<= SMAX - C, for C s< 0 it needs A s>= SMIN - C. This is the common
    // shape for a bounded induction increment (`i + 1` with i below the loop
    // bound), where the constraint solver knows the bound on A.
    Value *A = WO->getLHS();
    Value *CVal = WO->getRHS();
    const APInt *C;
    if (!match(CVal, m_APInt(C)))
      return tryToSimplifyVariableSAdd(WO, Info, DT, ToRemove);
    unsigned BitWidth = C->getBitWidth();
    Value *Limit;
    CmpInst::Predicate Pred;
    if (C->isNonNegative()) {
      // A s<= SMAX - C  <=>  A s< SMAX - C + 1 (SMAX - C + 1 does not wrap as
      // C s>= 0, so SMAX - C + 1 s> SMIN).
      Limit = ConstantInt::get(A->getType(),
                               APInt::getSignedMaxValue(BitWidth) - *C);
      Pred = CmpInst::ICMP_SLE;
    } else {
      // A s>= SMIN - C (SMIN - C does not wrap as C s< 0).
      Limit = ConstantInt::get(A->getType(),
                               APInt::getSignedMinValue(BitWidth) - *C);
      Pred = CmpInst::ICMP_SGE;
    }
    if (!Info.doesHold(Pred, A, Limit)) {
      // The constraint solver cannot always discharge the bound on A: it fails
      // when that bound is a variable (e.g. a loop counter `i` bounded by
      // `count - 1`), and for i64 the SMAX/SMIN sentinels reserved by the
      // solver's fixed-width encoding make `A s<= SMAX`/`A s>= SMIN`
      // unrepresentable (see canUseSExt). Fall back to SCEV's signed range for
      // A, which captures the range of a loop induction from its start value
      // and exit bound - exactly the information the solver lacks here. Gated
      // behind the failed (cheap) solver check to limit compile-time.
      bool NoOverflow = false;
      if (SE.isSCEVable(A->getType())) {
        ++NumOverflowSCEVQueries;
        ConstantRange RA = SE.getSignedRange(SE.getSCEV(A));
        NoOverflow = RA.signedAddMayOverflow(ConstantRange(*C)) ==
                     ConstantRange::OverflowResult::NeverOverflows;
      }

      // Final fallback for the unit-step case (the common induction increment
      // `i + 1` / decrement `i - 1`). The SMAX/SMIN limit above is not
      // representable for the widest type, but overflow of A + 1 requires
      // A == SMAX, and overflow of A - 1 requires A == SMIN. It is therefore
      // enough to prove A is strictly bounded by *some* other value already in
      // the system: any such value W is an in-range integer (W s<= SMAX,
      // W s>= SMIN), so A s< W implies A s<= SMAX - 1 (no positive overflow of
      // A + 1) and A s> W implies A s>= SMIN + 1 (no negative overflow of
      // A - 1). This side-steps the sentinel entirely because W is a real
      // operand, not the SMAX/SMIN constant. Scan the signed system's known
      // values for such a witness.
      if (!NoOverflow && (C->isOne() || C->isAllOnes())) {
        CmpInst::Predicate WitnessPred =
            C->isOne() ? CmpInst::ICMP_SLT : CmpInst::ICMP_SGT;
        for (auto &KV : Info.getValue2Index(/*Signed=*/true)) {
          Value *W = KV.first;
          if (W == A || isa<Constant>(W) || W->getType() != A->getType())
            continue;
          if (Info.doesHold(WitnessPred, A, W)) {
            NoOverflow = true;
            break;
          }
        }
      }

      if (!NoOverflow)
        return false;
    }
    Changed = replaceOverflowUses(WO, A, CVal, ToRemove);
  }
  return Changed;
}

/// Collect the value components of signed checked add/sub intrinsics that are
/// only evaluated on the no-overflow edge of the branch on their overflow flag,
/// and record them in \p Info so decompose can look through them.
///
/// Swift emits an overflow-checked `a + b` as
///   %op = call {i64, i1} @llvm.sadd.with.overflow.i64(i64 %a, i64 %b)
///   %ov = extractvalue {i64, i1} %op, 1
///   br i1 %ov, label %trap, label %cont
/// cont:
///   %sum = extractvalue {i64, i1} %op, 0
/// The intrinsic itself is opaque to the solver, which blocks any reasoning
/// about a bounds check on %sum. On the %cont edge the addition provably did
/// not overflow, so there %sum is exactly the mathematical sum %a + %b.
///
/// Soundness relies on placement rather than on a per-use check: the fact is
/// attached to the extractvalue *instruction*, and is only recorded when that
/// instruction's parent block is dominated by the no-overflow edge. By SSA
/// dominance every use of the extractvalue is then also dominated by that edge,
/// so there is no program point at which the recorded decomposition is invalid.
/// An extractvalue placed outside the guarded region (or reachable when the
/// operation did overflow) is simply not recorded and stays opaque.
static void collectGuardedCheckedOps(Function &F, DominatorTree &DT,
                                     ConstraintInfo &Info) {
  for (BasicBlock &BB : F) {
    // Only look at branches on the overflow flag of a signed checked op.
    auto *Br = dyn_cast<CondBrInst>(BB.getTerminator());
    if (!Br)
      continue;
    WithOverflowInst *WO;
    if (!match(Br->getCondition(), m_ExtractValue<1>(m_WithOverflowInst(WO))))
      continue;
    if (WO->getIntrinsicID() != Intrinsic::sadd_with_overflow &&
        WO->getIntrinsicID() != Intrinsic::ssub_with_overflow)
      continue;
    // The overflow flag is false on the second successor, and the value
    // component only equals the exact sum/difference where that edge holds.
    BasicBlockEdge NoOverflowEdge(&BB, Br->getSuccessor(1));

    for (User *U : WO->users()) {
      auto *EV = dyn_cast<ExtractValueInst>(U);
      if (!EV || EV->getNumIndices() != 1 || *EV->idx_begin() != 0)
        continue;
      // Require the extractvalue to be placed inside the no-overflow region,
      // which by SSA dominance covers all of its uses.
      if (DT.dominates(NoOverflowEdge, EV->getParent()))
        Info.addGuardedCheckedOp(EV);
    }
  }
}

static bool eliminateConstraints(Function &F, DominatorTree &DT, LoopInfo &LI,
                                 ScalarEvolution &SE,
                                 OptimizationRemarkEmitter &ORE,
                                 TargetLibraryInfo &TLI) {
  bool Changed = false;
  DT.updateDFSNumbers();
  SmallVector<Value *> FunctionArgs(llvm::make_pointer_range(F.args()));
  ConstraintInfo Info(F.getDataLayout(), FunctionArgs);
  collectGuardedCheckedOps(F, DT, Info);
  State S(DT, LI, SE, TLI);
  std::unique_ptr<Module> ReproducerModule(
      DumpReproducers ? new Module(F.getName(), F.getContext()) : nullptr);

  // First, collect conditions implied by branches and blocks with their
  // Dominator DFS in and out numbers.
  for (BasicBlock &BB : F) {
    if (!DT.getNode(&BB))
      continue;
    S.addInfoFor(BB);
  }

  // Next, sort worklist by dominance, so that dominating conditions to check
  // and facts come before conditions and facts dominated by them. If a
  // condition to check and a fact have the same numbers, conditional facts come
  // first. Assume facts and checks are ordered according to their relative
  // order in the containing basic block. Also make sure conditions with
  // constant operands come before conditions without constant operands. This
  // increases the effectiveness of the current signed <-> unsigned fact
  // transfer logic.
  stable_sort(S.WorkList, [](const FactOrCheck &A, const FactOrCheck &B) {
    auto HasNoConstOp = [](const FactOrCheck &B) {
      Value *V0 = B.isConditionFact() ? B.Cond.Op0 : B.Inst->getOperand(0);
      Value *V1 = B.isConditionFact() ? B.Cond.Op1 : B.Inst->getOperand(1);
      return !isa<ConstantInt>(V0) && !isa<ConstantInt>(V1);
    };
    // If both entries have the same In numbers, conditional facts come first.
    // Otherwise use the relative order in the basic block.
    if (A.NumIn == B.NumIn) {
      if (A.isConditionFact() && B.isConditionFact()) {
        bool NoConstOpA = HasNoConstOp(A);
        bool NoConstOpB = HasNoConstOp(B);
        return NoConstOpA < NoConstOpB;
      }
      if (A.isConditionFact())
        return true;
      if (B.isConditionFact())
        return false;
      auto *InstA = A.getContextInst();
      auto *InstB = B.getContextInst();
      return InstA->comesBefore(InstB);
    }
    return A.NumIn < B.NumIn;
  });

  SmallVector<Instruction *> ToRemove;

  // Finally, process ordered worklist and eliminate implied conditions.
  SmallVector<StackEntry, 16> DFSInStack;
  SmallVector<ReproducerEntry> ReproducerCondStack;
  for (FactOrCheck &CB : S.WorkList) {
    // First, pop entries from the stack that are out-of-scope for CB. Remove
    // the corresponding entry from the constraint system.
    while (!DFSInStack.empty()) {
      auto &E = DFSInStack.back();
      LLVM_DEBUG(dbgs() << "Top of stack : " << E.NumIn << " " << E.NumOut
                        << "\n");
      LLVM_DEBUG(dbgs() << "CB: " << CB.NumIn << " " << CB.NumOut << "\n");
      assert(E.NumIn <= CB.NumIn);
      if (CB.NumOut <= E.NumOut)
        break;
      LLVM_DEBUG({
        dbgs() << "Removing ";
        dumpConstraint(Info.getCS(E.IsSigned).getLastConstraint(),
                       Info.getValue2Index(E.IsSigned));
        dbgs() << "\n";
      });
      removeEntryFromStack(E, Info, ReproducerModule.get(), ReproducerCondStack,
                           DFSInStack);
    }

    CmpPredicate Pred;
    Value *A, *B;
    // For a block, check if any CmpInsts become known based on the current set
    // of constraints.
    if (CB.isCheck()) {
      Instruction *Inst = CB.getInstructionToSimplify();
      if (!Inst)
        continue;
      if (canStrengthenFlags(Inst)) {
        Changed |= tryToStrengthenFlags(Inst, Info, ToRemove);
        continue;
      }
      LLVM_DEBUG(dbgs() << "Processing condition to simplify: " << *Inst
                        << "\n");
      if (auto *II = dyn_cast<WithOverflowInst>(Inst)) {
        Changed |= tryToSimplifyOverflowMath(II, Info, SE, S.DT, ToRemove);
      } else if (match(Inst, m_ICmpLike(Pred, m_Value(A), m_Value(B)))) {
        bool Simplified = checkAndReplaceCondition(
            Pred, A, B, Inst, Info, CB.NumIn, CB.NumOut, CB.getContextInst(),
            ReproducerModule.get(), ReproducerCondStack, S.DT, ToRemove);
        if (!Simplified &&
            match(CB.getContextInst(), m_LogicalOp(m_Value(), m_Value()))) {
          Simplified = checkOrAndOpImpliedByOther(
              CB, Info, ReproducerModule.get(), ReproducerCondStack, DFSInStack,
              ToRemove);
        }
        Changed |= Simplified;
      } else if (auto *MinMax = dyn_cast<MinMaxIntrinsic>(Inst)) {
        Changed |= checkAndReplaceMinMax(MinMax, Info, ToRemove);
      } else if (auto *CmpIntr = dyn_cast<CmpIntrinsic>(Inst)) {
        Changed |= checkAndReplaceCmp(CmpIntr, Info, ToRemove);
      } else if (match(Inst, m_Intrinsic<Intrinsic::usub_sat>())) {
        Changed |=
            checkAndReplaceUSubSat(cast<SaturatingInst>(Inst), Info, ToRemove);
      }
      continue;
    }

    auto AddFact = [&](CmpPredicate Pred, Value *A, Value *B) {
      LLVM_DEBUG(dbgs() << "Processing fact to add to the system: ";
                 dumpUnpackedICmp(dbgs(), Pred, A, B); dbgs() << "\n");
      if (Info.getCS(CmpInst::isSigned(Pred)).size() > MaxRows) {
        LLVM_DEBUG(
            dbgs()
            << "Skip adding constraint because system has too many rows.\n");
        return;
      }

      Info.addFact(Pred, A, B, CB.NumIn, CB.NumOut, DFSInStack);
      if (ReproducerModule && DFSInStack.size() > ReproducerCondStack.size())
        ReproducerCondStack.emplace_back(Pred, A, B);

      if (ICmpInst::isRelational(Pred)) {
        // If samesign is present on the ICmp, simply flip the sign of the
        // predicate, transferring the information from the signed system to the
        // unsigned system, and viceversa.
        if (Pred.hasSameSign())
          Info.addFact(ICmpInst::getFlippedSignednessPredicate(Pred), A, B,
                       CB.NumIn, CB.NumOut, DFSInStack);
        else
          Info.transferToOtherSystem(Pred, A, B, CB.NumIn, CB.NumOut,
                                     DFSInStack);
      }

      // (X | Y) >s -1 implies X >s -1 and Y >s -1, because the sign bit of an
      // OR is the OR of the operand sign bits. Similarly, (X & Y) <s 0 implies
      // X <s 0 and Y <s 0. Look through these canonical forms produced by
      // InstCombine so the sign facts on the operands are available to the
      // solver.
      if ((Pred == CmpInst::ICMP_SGT && match(B, m_AllOnes())) ||
          (Pred == CmpInst::ICMP_SLT && match(B, m_Zero()))) {
        unsigned Opc =
            Pred == CmpInst::ICMP_SGT ? Instruction::Or : Instruction::And;
        SmallVector<Value *> Worklist = {A};
        SmallPtrSet<Value *, 4> Seen;
        while (!Worklist.empty()) {
          Value *Cur = Worklist.pop_back_val();
          auto *BO = dyn_cast<BinaryOperator>(Cur);
          if (!BO || BO->getOpcode() != Opc)
            continue;
          for (Value *Op : {BO->getOperand(0), BO->getOperand(1)}) {
            if (!Seen.insert(Op).second)
              continue;
            Worklist.push_back(Op);
            Info.addFact(Pred, Op, B, CB.NumIn, CB.NumOut, DFSInStack);
          }
        }
      }

      if (ReproducerModule && DFSInStack.size() > ReproducerCondStack.size()) {
        // Add dummy entries to ReproducerCondStack to keep it in sync with
        // DFSInStack.
        for (unsigned I = 0,
                      E = (DFSInStack.size() - ReproducerCondStack.size());
             I < E; ++I) {
          ReproducerCondStack.emplace_back(ICmpInst::BAD_ICMP_PREDICATE,
                                           nullptr, nullptr);
        }
      }
    };

    // Add the fact `Scale * A u<= B`. The scaled left-hand side is not an IR
    // value, so it cannot be handed to AddFact as a pair of values. Only the
    // unsigned system is used, and no signed counterpart is derived.
    auto AddScaledFact = [&](int64_t Scale, Value *A, Value *B) {
      if (Info.getCS(/*Signed=*/false).size() > MaxRows) {
        LLVM_DEBUG(
            dbgs()
            << "Skip adding constraint because system has too many rows.\n");
        return;
      }
      Info.addScaledFact(Scale, A, B, CB.NumIn, CB.NumOut, DFSInStack);
      // The fact cannot be spelled as an icmp, so record placeholders to keep
      // ReproducerCondStack in sync with DFSInStack.
      if (ReproducerModule)
        while (DFSInStack.size() > ReproducerCondStack.size())
          ReproducerCondStack.emplace_back(ICmpInst::BAD_ICMP_PREDICATE,
                                           nullptr, nullptr);
    };

    if (!CB.isConditionFact()) {
      Value *X;
      if (match(CB.Inst, m_Intrinsic<Intrinsic::abs>(m_Value(X)))) {
        // If is_int_min_poison is true then we may assume llvm.abs >= 0.
        if (cast<ConstantInt>(CB.Inst->getOperand(1))->isOne())
          AddFact(CmpInst::ICMP_SGE, CB.Inst,
                  ConstantInt::get(CB.Inst->getType(), 0));
        AddFact(CmpInst::ICMP_SGE, CB.Inst, X);
        continue;
      }

      if (auto *MinMax = dyn_cast<MinMaxIntrinsic>(CB.Inst)) {
        Pred = ICmpInst::getNonStrictPredicate(MinMax->getPredicate());
        AddFact(Pred, MinMax, MinMax->getLHS());
        AddFact(Pred, MinMax, MinMax->getRHS());
        continue;
      }
      if (auto *USatI = dyn_cast<SaturatingInst>(CB.Inst)) {
        switch (USatI->getIntrinsicID()) {
        default:
          llvm_unreachable("Unexpected intrinsic.");
        case Intrinsic::uadd_sat:
          AddFact(ICmpInst::ICMP_UGE, USatI, USatI->getLHS());
          AddFact(ICmpInst::ICMP_UGE, USatI, USatI->getRHS());
          break;
        case Intrinsic::usub_sat:
          AddFact(ICmpInst::ICMP_ULE, USatI, USatI->getLHS());
          break;
        }
        continue;
      }

      if (auto *BO = dyn_cast<BinaryOperator>(CB.Inst)) {
        if (BO->getOpcode() == Instruction::URem) {
          // urem x, n: result < n (remainder is always less than divisor)
          AddFact(CmpInst::ICMP_ULT, BO, BO->getOperand(1));
          // urem x, n: result <= x (remainder is at most the dividend)
          AddFact(CmpInst::ICMP_ULE, BO, BO->getOperand(0));
          continue;
        }
        if (BO->getOpcode() == Instruction::UDiv) {
          // udiv x, n: result <= x (quotient is at most the dividend)
          AddFact(CmpInst::ICMP_ULE, BO, BO->getOperand(0));
          continue;
        }
        if (BO->getOpcode() == Instruction::LShr) {
          // lshr x, n: result <= x (right shift cannot increase the value)
          AddFact(CmpInst::ICMP_ULE, BO, BO->getOperand(0));
          // lshr x, k: result * 2^k <= x, for a constant k. Scaling the shifted
          // value back up by the same power of two recovers x without its low k
          // bits, and dropping those bits cannot increase the value. This is
          // the bound needed to relate a chunk count `x >> k` to the index of
          // the last element those chunks cover. `result * 2^k` is not an IR
          // value, so the row is added with an explicit scale on the shift
          // result rather than as a plain fact.
          auto *Shift = dyn_cast<ConstantInt>(BO->getOperand(1));
          // A shift amount >= the bit width makes the shift poison, and the
          // scale must fit the signed 64-bit coefficients of the system.
          if (Shift && BO->getType()->isIntegerTy() &&
              Shift->getValue().ult(
                  std::min(BO->getType()->getIntegerBitWidth(), 63u)))
            AddScaledFact(int64_t(1) << Shift->getZExtValue(), BO,
                          BO->getOperand(0));
          continue;
        }
        if (BO->getOpcode() == Instruction::SRem) {
          Value *X = BO->getOperand(0);
          Value *N = BO->getOperand(1);
          Constant *Zero = Constant::getNullValue(BO->getType());
          if (Info.doesHold(CmpInst::ICMP_SGE, X, Zero) ||
              isKnownNonNegative(X, F.getDataLayout())) {
            // srem x, n: result >= 0, if x >= 0 (result has the sign of x)
            AddFact(CmpInst::ICMP_SGE, BO, Zero);
            // srem x, n: result <= x, if x >= 0 (|result| <= |x| and both are
            // non-negative)
            AddFact(CmpInst::ICMP_SLE, BO, X);
          }
          if (Info.doesHold(CmpInst::ICMP_SGE, N, Zero) ||
              isKnownPositive(N, F.getDataLayout())) {
            // srem x, n: result <= n, if n >= 0 (|result| < n, so result <= n -
            // 1
            AddFact(CmpInst::ICMP_SLT, BO, N);
          }
          continue;
        }
        if (BO->getOpcode() == Instruction::SDiv) {
          // Only a constant divisor of at least two bounds the quotient by the
          // dividend: C == 0 makes the division poison, for C == 1 the facts
          // below are vacuous, and for a negative C the quotient is not below a
          // non-negative dividend.
          ConstantInt *C;
          if (!match(BO->getOperand(1), m_ConstantInt(C)) ||
              C->getValue().sle(1))
            continue;
          Value *X = BO->getOperand(0);
          Constant *Zero = Constant::getNullValue(BO->getType());
          if (Info.doesHold(CmpInst::ICMP_SGE, X, Zero) ||
              isKnownNonNegative(X, F.getDataLayout())) {
            // sdiv x, C: result >= 0, if x >= 0 and C > 1 (the quotient of a
            // non-negative dividend by a positive divisor is non-negative)
            AddFact(CmpInst::ICMP_SGE, BO, Zero);
            // sdiv x, C: result < x, if x > 0 and C > 1. The division truncates
            // towards zero, so the result is the floor of x / C, which is below
            // x for x > 0 and C > 1. For x == 0 the result is 0 and only the
            // non-strict bound holds. The strict bound is what relates a
            // binary-search midpoint `low + (high - low) / 2` back to `high`.
            if (Info.doesHold(CmpInst::ICMP_SGT, X, Zero) ||
                isKnownPositive(X, F.getDataLayout()))
              AddFact(CmpInst::ICMP_SLT, BO, X);
            else
              AddFact(CmpInst::ICMP_SLE, BO, X);
          }
          continue;
        }
      }

      auto &DL = F.getDataLayout();
      auto AddFactsAboutIndices = [&](Value *Ptr, Type *AccessType) {
        CmpPredicate Pred;
        Value *A, *B;
        if (getConstraintFromMemoryAccess(
                *cast<GetElementPtrInst>(Ptr),
                DL.getTypeStoreSize(AccessType).getFixedValue(), Pred, A, B, DL,
                TLI))
          AddFact(Pred, A, B);
      };

      if (auto *LI = dyn_cast<LoadInst>(CB.Inst)) {
        AddFactsAboutIndices(LI->getPointerOperand(), LI->getAccessType());
        continue;
      }
      if (auto *SI = dyn_cast<StoreInst>(CB.Inst)) {
        AddFactsAboutIndices(SI->getPointerOperand(), SI->getAccessType());
        continue;
      }
    }

    if (CB.isConditionFact()) {
      Pred = CB.Cond.Pred;
      A = CB.Cond.Op0;
      B = CB.Cond.Op1;
      const ConditionTy &DoesHold = CB.getDoesHold();
      if (DoesHold.Pred != CmpInst::BAD_ICMP_PREDICATE &&
          !Info.doesHold(DoesHold.Pred, DoesHold.Op0, DoesHold.Op1)) {
        LLVM_DEBUG({
          dbgs() << "Not adding fact ";
          dumpUnpackedICmp(dbgs(), Pred, A, B);
          dbgs() << " because precondition ";
          dumpUnpackedICmp(dbgs(), DoesHold.Pred, DoesHold.Op0, DoesHold.Op1);
          dbgs() << " does not hold.\n";
        });
        continue;
      }
    } else {
      [[maybe_unused]] bool Matched =
          match(CB.Inst, m_Intrinsic<Intrinsic::assume>(
                             m_ICmpLike(Pred, m_Value(A), m_Value(B))));
      assert(Matched &&
             "Must have an assume intrinsic with a icmp like operand");
    }
    AddFact(Pred, A, B);
  }

  if (ReproducerModule && !ReproducerModule->functions().empty()) {
    std::string S;
    raw_string_ostream StringS(S);
    ReproducerModule->print(StringS, nullptr);
    OptimizationRemark Rem(DEBUG_TYPE, "Reproducer", &F);
    Rem << ore::NV("module") << S;
    ORE.emit(Rem);
  }

#ifndef NDEBUG
  unsigned SignedEntries =
      count_if(DFSInStack, [](const StackEntry &E) { return E.IsSigned; });
  assert(Info.getCS(false).size() - FunctionArgs.size() ==
             DFSInStack.size() - SignedEntries &&
         "updates to CS and DFSInStack are out of sync");
  assert(Info.getCS(true).size() == SignedEntries &&
         "updates to CS and DFSInStack are out of sync");
#endif

  for (Instruction *I : ToRemove)
    I->eraseFromParent();
  return Changed;
}

PreservedAnalyses ConstraintEliminationPass::run(Function &F,
                                                 FunctionAnalysisManager &AM) {
  auto &DT = AM.getResult<DominatorTreeAnalysis>(F);
  auto &LI = AM.getResult<LoopAnalysis>(F);
  auto &SE = AM.getResult<ScalarEvolutionAnalysis>(F);
  auto &ORE = AM.getResult<OptimizationRemarkEmitterAnalysis>(F);
  auto &TLI = AM.getResult<TargetLibraryAnalysis>(F);
  if (!eliminateConstraints(F, DT, LI, SE, ORE, TLI))
    return PreservedAnalyses::all();

  PreservedAnalyses PA;
  PA.preserve<ScalarEvolutionAnalysis>();
  PA.preserveSet<CFGAnalyses>();
  return PA;
}
