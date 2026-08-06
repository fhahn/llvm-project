//===- VPlanAnalysis.cpp - Various Analyses working on VPlan ----*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "VPlanAnalysis.h"
#include "VPlan.h"
#include "VPlanCFG.h"
#include "VPlanDominatorTree.h"
#include "VPlanHelpers.h"
#include "VPlanPatternMatch.h"
#include "VPlanUtils.h"
#include "llvm/ADT/PostOrderIterator.h"
#include "llvm/ADT/TypeSwitch.h"
#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/Analysis/ScalarEvolutionPatternMatch.h"
#include "llvm/Analysis/TargetTransformInfo.h"
#include "llvm/Analysis/VectorUtils.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Operator.h"
#include "llvm/IR/PatternMatch.h"

using namespace llvm;
using namespace VPlanPatternMatch;
using namespace SCEVPatternMatch;

#define DEBUG_TYPE "vplan"

void llvm::collectEphemeralRecipesForVPlan(
    VPlan &Plan, DenseSet<VPRecipeBase *> &EphRecipes) {
  // First, collect seed recipes which are operands of assumes.
  SmallVector<VPRecipeBase *> Worklist;
  for (VPBasicBlock *VPBB : VPBlockUtils::blocksOnly<VPBasicBlock>(
           vp_depth_first_deep(Plan.getVectorLoopRegion()->getEntry()))) {
    for (VPRecipeBase &R : *VPBB) {
      auto *RepR = dyn_cast<VPReplicateRecipe>(&R);
      if (!RepR || !match(RepR, m_Intrinsic<Intrinsic::assume>()))
        continue;
      Worklist.push_back(RepR);
      EphRecipes.insert(RepR);
    }
  }

  // Process operands of candidates in worklist and add them to the set of
  // ephemeral recipes, if they don't have side-effects and are only used by
  // other ephemeral recipes.
  while (!Worklist.empty()) {
    VPRecipeBase *Cur = Worklist.pop_back_val();
    for (VPValue *Op : Cur->operands()) {
      auto *OpR = Op->getDefiningRecipe();
      if (!OpR || OpR->mayHaveSideEffects() || EphRecipes.contains(OpR))
        continue;
      if (any_of(Op->users(), [EphRecipes](VPUser *U) {
            auto *UR = dyn_cast<VPRecipeBase>(U);
            return !UR || !EphRecipes.contains(UR);
          }))
        continue;
      EphRecipes.insert(OpR);
      Worklist.push_back(OpR);
    }
  }
}

bool VPDominatorTree::properlyDominates(const VPRecipeBase *A,
                                        const VPRecipeBase *B) const {
  if (A == B)
    return false;

  auto LocalComesBefore = [](const VPRecipeBase *A, const VPRecipeBase *B) {
    for (auto &R : *A->getParent()) {
      if (&R == A)
        return true;
      if (&R == B)
        return false;
    }
    llvm_unreachable("recipe not found");
  };
  const VPBlockBase *ParentA = A->getParent();
  const VPBlockBase *ParentB = B->getParent();
  if (ParentA == ParentB)
    return LocalComesBefore(A, B);

  return Base::properlyDominates(ParentA, ParentB);
}

InstructionCost
VPRegisterUsage::spillCost(const TargetTransformInfo &TTI,
                           TargetTransformInfo::TargetCostKind CostKind,
                           unsigned OverrideMaxNumRegs) const {
  InstructionCost Cost;
  for (const auto &[RegClass, MaxUsers] : MaxLocalUsers) {
    unsigned AvailableRegs = OverrideMaxNumRegs > 0
                                 ? OverrideMaxNumRegs
                                 : TTI.getNumberOfRegisters(RegClass);
    if (MaxUsers > AvailableRegs) {
      // Assume that for each register used past what's available we get one
      // spill and reload.
      unsigned Spills = MaxUsers - AvailableRegs;
      InstructionCost SpillCost =
          TTI.getRegisterClassSpillCost(RegClass, CostKind) +
          TTI.getRegisterClassReloadCost(RegClass, CostKind);
      InstructionCost TotalCost = Spills * SpillCost;
      LLVM_DEBUG(dbgs() << "LV(REG): Cost of " << TotalCost << " from "
                        << Spills << " spills of "
                        << TTI.getRegisterClassName(RegClass) << "\n");
      Cost += TotalCost;
    }
  }
  return Cost;
}

SmallVector<VPRegisterUsage, 8> llvm::calculateRegisterUsageForPlan(
    VPlan &Plan, ArrayRef<ElementCount> VFs, const TargetTransformInfo &TTI,
    const SmallPtrSetImpl<const Value *> &ValuesToIgnore) {
  // Each 'key' in the map opens a new interval. The values
  // of the map are the index of the 'last seen' usage of the
  // VPValue that is the key.
  using IntervalMap = SmallDenseMap<VPValue *, unsigned, 16>;

  // Maps indices to recipes.
  SmallVector<VPRecipeBase *, 64> Idx2Recipe;
  // Marks the end of each interval.
  IntervalMap EndPoint;
  // Saves the list of VPValues that are used in the loop.
  SmallPtrSet<VPValue *, 8> Ends;
  // Saves the list of values that are used in the loop but are defined outside
  // the loop (not including non-recipe values such as arguments and
  // constants).
  SmallSetVector<VPValue *, 8> LoopInvariants;
  if (!Plan.getVectorTripCount().user_empty())
    LoopInvariants.insert(&Plan.getVectorTripCount());

  // We scan the loop in a topological order in order and assign a number to
  // each recipe. We use RPO to ensure that defs are met before their users. We
  // assume that each recipe that has in-loop users starts an interval. We
  // record every time that an in-loop value is used, so we have a list of the
  // first occurences of each recipe and last occurrence of each VPValue.
  VPRegionBlock *LoopRegion = Plan.getVectorLoopRegion();
  ReversePostOrderTraversal<VPBlockDeepTraversalWrapper<VPBlockBase *>> RPOT(
      LoopRegion);
  for (VPBasicBlock *VPBB : VPBlockUtils::blocksOnly<VPBasicBlock>(RPOT)) {
    if (!VPBB->getParent())
      break;
    for (VPRecipeBase &R : *VPBB) {
      Idx2Recipe.push_back(&R);

      // Save the end location of each USE.
      for (VPValue *U : R.operands()) {
        if (isa<VPRecipeValue>(U)) {
          // Overwrite previous end points.
          EndPoint[U] = Idx2Recipe.size();
          Ends.insert(U);
        } else if (auto *IRV = dyn_cast<VPIRValue>(U)) {
          // Ignore non-recipe values such as arguments, constants, etc.
          // FIXME: Might need some motivation why these values are ignored. If
          // for example an argument is used inside the loop it will increase
          // the register pressure (so shouldn't we add it to LoopInvariants).
          if (!isa<Instruction>(IRV->getValue()))
            continue;
          // This recipe is outside the loop, record it and continue.
          LoopInvariants.insert(U);
        }
        // Other types of VPValue are currently not tracked.
      }
    }
    if (VPBB == LoopRegion->getExiting()) {
      // VPWidenIntOrFpInductionRecipes are used implicitly at the end of the
      // exiting block, where their increment will get materialized eventually.
      for (auto &R : LoopRegion->getEntryBasicBlock()->phis()) {
        if (auto *WideIV = dyn_cast<VPWidenIntOrFpInductionRecipe>(&R)) {
          EndPoint[WideIV] = Idx2Recipe.size();
          Ends.insert(WideIV);
        }
      }
    }
  }

  // Saves the list of intervals that end with the index in 'key'.
  using VPValueList = SmallVector<VPValue *, 2>;
  SmallDenseMap<unsigned, VPValueList, 16> TransposeEnds;

  // Next, we transpose the EndPoints into a multi map that holds the list of
  // intervals that *end* at a specific location.
  for (auto &Interval : EndPoint)
    TransposeEnds[Interval.second].push_back(Interval.first);

  SmallPtrSet<VPValue *, 8> OpenIntervals;
  SmallVector<VPRegisterUsage, 8> RUs(VFs.size());
  SmallVector<SmallMapVector<unsigned, unsigned, 4>, 8> MaxUsages(VFs.size());

  LLVM_DEBUG(dbgs() << "LV(REG): Calculating max register usage:\n");

  const auto &TTICapture = TTI;
  auto GetRegUsage = [&TTICapture](Type *Ty, ElementCount VF) -> unsigned {
    if (Ty->isTokenTy() || !VectorType::isValidElementType(Ty) ||
        (VF.isScalable() &&
         !TTICapture.isElementTypeLegalForScalableVector(Ty)))
      return 0;
    return TTICapture.getRegUsageForType(VectorType::get(Ty, VF));
  };

  VPValue *CanIV = LoopRegion->getCanonicalIV();
  // Note: canonical IVs are retained even if they have no users.
  if (!CanIV->user_empty())
    OpenIntervals.insert(CanIV);

  // We scan the instructions linearly and record each time that a new interval
  // starts, by placing it in a set. If we find this value in TransposEnds then
  // we remove it from the set. The max register usage is the maximum register
  // usage of the recipes of the set.
  for (unsigned int Idx = 0, Sz = Idx2Recipe.size(); Idx < Sz; ++Idx) {
    VPRecipeBase *R = Idx2Recipe[Idx];

    // Remove all of the VPValues that end at this location.
    VPValueList &List = TransposeEnds[Idx];
    for (VPValue *ToRemove : List)
      OpenIntervals.erase(ToRemove);

    // Ignore recipes that are never used within the loop and do not have side
    // effects.
    if (none_of(R->definedValues(),
                [&Ends](VPValue *Def) { return Ends.count(Def); }) &&
        !R->mayHaveSideEffects())
      continue;

    // Skip recipes for ignored values.
    // TODO: Should mark recipes for ephemeral values that cannot be removed
    // explictly in VPlan.
    if (isa<VPSingleDefRecipe>(R) &&
        ValuesToIgnore.contains(
            cast<VPSingleDefRecipe>(R)->getUnderlyingValue()))
      continue;

    // For each VF find the maximum usage of registers.
    for (unsigned J = 0, E = VFs.size(); J < E; ++J) {
      // Count the number of registers used, per register class, given all open
      // intervals.
      // Note that elements in this SmallMapVector will be default constructed
      // as 0. So we can use "RegUsage[ClassID] += n" in the code below even if
      // there is no previous entry for ClassID.
      SmallMapVector<unsigned, unsigned, 4> RegUsage;

      for (auto *VPV : OpenIntervals) {
        // Skip artificial values or values that weren't present in the original
        // loop.
        // TODO: Remove skipping values that weren't present in the original
        // loop after removing the legacy
        // LoopVectorizationCostModel::calculateRegisterUsage
        if (isa<VPVectorPointerRecipe, VPVectorEndPointerRecipe,
                VPBranchOnMaskRecipe>(VPV) ||
            match(VPV, m_ExtractLastPart(m_VPValue())))
          continue;

        if (VFs[J].isScalar() || VPV == CanIV ||
            isa<VPReplicateRecipe, VPDerivedIVRecipe,
                VPCurrentIterationPHIRecipe, VPScalarIVStepsRecipe>(VPV) ||
            (isa<VPInstruction>(VPV) && vputils::onlyScalarValuesUsed(VPV)) ||
            (isa<VPReductionPHIRecipe>(VPV) &&
             (cast<VPReductionPHIRecipe>(VPV))->isInLoop())) {
          unsigned ClassID =
              TTI.getRegisterClassForType(false, VPV->getScalarType());
          // FIXME: The target might use more than one register for the type
          // even in the scalar case.
          RegUsage[ClassID] += 1;
        } else {
          // The output from scaled phis and scaled reductions actually has
          // fewer lanes than the VF.
          unsigned ScaleFactor =
              vputils::getVFScaleFactor(VPV->getDefiningRecipe());
          ElementCount VF = VFs[J];
          if (ScaleFactor > 1) {
            VF = VFs[J].divideCoefficientBy(ScaleFactor);
            LLVM_DEBUG(dbgs() << "LV(REG): Scaled down VF from " << VFs[J]
                              << " to " << VF << " for " << *R << "\n";);
          }

          Type *ScalarTy = VPV->getScalarType();
          unsigned ClassID = TTI.getRegisterClassForType(true, ScalarTy);
          RegUsage[ClassID] += GetRegUsage(ScalarTy, VF);
        }
      }

      for (const auto &Pair : RegUsage) {
        auto &Entry = MaxUsages[J][Pair.first];
        Entry = std::max(Entry, Pair.second);
      }
    }

    LLVM_DEBUG(dbgs() << "LV(REG): At #" << Idx << " Interval # "
                      << OpenIntervals.size() << '\n');

    // Add used VPValues defined by the current recipe to the list of open
    // intervals.
    for (VPValue *DefV : R->definedValues())
      if (Ends.contains(DefV))
        OpenIntervals.insert(DefV);
  }

  // We also search for instructions that are defined outside the loop, but are
  // used inside the loop. We need this number separately from the max-interval
  // usage number because when we unroll, loop-invariant values do not take
  // more register.
  VPRegisterUsage RU;
  for (unsigned Idx = 0, End = VFs.size(); Idx < End; ++Idx) {
    // Note that elements in this SmallMapVector will be default constructed
    // as 0. So we can use "Invariant[ClassID] += n" in the code below even if
    // there is no previous entry for ClassID.
    SmallMapVector<unsigned, unsigned, 4> Invariant;

    for (auto *In : LoopInvariants) {
      // FIXME: The target might use more than one register for the type
      // even in the scalar case.
      bool IsScalar = vputils::onlyScalarValuesUsed(In);

      ElementCount VF = IsScalar ? ElementCount::getFixed(1) : VFs[Idx];
      unsigned ClassID =
          TTI.getRegisterClassForType(VF.isVector(), In->getScalarType());
      Invariant[ClassID] += GetRegUsage(In->getScalarType(), VF);
    }

    LLVM_DEBUG({
      dbgs() << "LV(REG): VF = " << VFs[Idx] << '\n';
      dbgs() << "LV(REG): Found max usage: " << MaxUsages[Idx].size()
             << " item\n";
      for (const auto &pair : MaxUsages[Idx]) {
        dbgs() << "LV(REG): RegisterClass: "
               << TTI.getRegisterClassName(pair.first) << ", " << pair.second
               << " registers\n";
      }
      dbgs() << "LV(REG): Found invariant usage: " << Invariant.size()
             << " item\n";
      for (const auto &pair : Invariant) {
        dbgs() << "LV(REG): RegisterClass: "
               << TTI.getRegisterClassName(pair.first) << ", " << pair.second
               << " registers\n";
      }
    });

    RU.LoopInvariantRegs = Invariant;
    RU.MaxLocalUsers = MaxUsages[Idx];
    RUs[Idx] = RU;
  }

  return RUs;
}

//===----------------------------------------------------------------------===//
// Outer-loop memory safety analysis (VPlan-native).
//===----------------------------------------------------------------------===//

namespace {

/// Per-outer-iteration address range [Start, End) of a memory access, plus the
/// step of its address w.r.t. the inner loop.
struct InnerRange {
  const SCEV *Start;
  const SCEV *End;
  /// Step of the address AddRec w.r.t. the inner loop, in bytes, or nullptr if
  /// the address does not vary in the inner loop.
  const SCEV *InnerStep;
};

/// Per-outer-iteration address range of one memory access recipe.
struct OuterLoopMemAccess {
  VPValue *Ptr;
  const SCEV *Start;
  const SCEV *End;
  /// Step of Start w.r.t. the outer loop, or nullptr if the address does not
  /// vary in the outer loop.
  const SCEV *OuterStep;
  /// Step of the address w.r.t. the inner loop, or nullptr if the address does
  /// not vary in the inner loop.
  const SCEV *InnerStep;
  /// Number of bytes accessed, i.e. the store size of the accessed type.
  uint64_t AccessSize;
  bool IsStore;
};

} // end anonymous namespace

/// Compute the per-outer-iteration address range [Start, End) for a pointer
/// SCEV, using pure SCEV operations. If \p PtrSCEV is an inner-loop AddRec
/// {Start, +, InnerStep}<inner>, the range is
/// [Start, Start + InnerMaxBTC*InnerStep + AccessSize). Otherwise the
/// access is inner-invariant and the range is [Ptr, Ptr + AccessSize).
/// Returns std::nullopt on failure (e.g. a non-positive inner step would
/// invert the range under unsigned arithmetic).
static std::optional<InnerRange> computeInnerRange(const SCEV *PtrSCEV,
                                                   const Loop *InnerLoop,
                                                   const SCEV *InnerMaxBTC,
                                                   uint64_t AccessSize,
                                                   ScalarEvolution &SE) {
  // All address arithmetic below is done in the pointer expression's integer
  // type so that getAddExpr never sees mismatched operand widths (the inner
  // backedge-taken count and inner step may be narrower, e.g. an i32 inner
  // counter with i64-width addresses).
  Type *PtrTy = SE.getEffectiveSCEVType(PtrSCEV->getType());
  const auto *InnerAR = dyn_cast<SCEVAddRecExpr>(PtrSCEV);
  if (!InnerAR || InnerAR->getLoop() != InnerLoop || !InnerAR->isAffine())
    return InnerRange{PtrSCEV,
                      SE.getAddExpr(PtrSCEV, SE.getConstant(PtrTy, AccessSize)),
                      nullptr};

  const SCEV *Start = InnerAR->getStart();
  const SCEV *InnerStep = InnerAR->getStepRecurrence(SE);
  // The [Start, End) range is computed with unsigned arithmetic. A
  // non-positive InnerStep would yield End < Start, inverting the range
  // and making subsequent unsigned predicates spuriously hold. Bail out
  // unless the step is known strictly positive.
  //
  // For a symbolic step such as 4*%M, isKnownPositive fails because the product
  // may overflow for very large %M, even though the guarded form knows %M >= 1.
  // The byte step is a product of a positive constant (the access size) and the
  // index step, so it is positive iff the index step is: strip the constant
  // factor and prove the rest positive via the inner loop's guards.
  if (!SE.isKnownPositive(InnerStep)) {
    const auto *Mul = dyn_cast<SCEVMulExpr>(InnerStep);
    if (!Mul || Mul->getNumOperands() != 2 ||
        !SE.isKnownPositive(Mul->getOperand(0)) ||
        !SE.isKnownPositive(SE.applyLoopGuards(Mul->getOperand(1), InnerLoop)))
      return std::nullopt;
  }
  // Compute Span = MaxBTC * InnerStep in the pointer type, zero-extending the
  // (possibly narrower) backedge-taken count and step so no magnitude is lost
  // and all operands share the address width. If the backedge-taken count is
  // wider than the address type (exotic), bail out rather than narrow it.
  if (SE.getTypeSizeInBits(InnerMaxBTC->getType()) >
          SE.getTypeSizeInBits(PtrTy) ||
      SE.getTypeSizeInBits(InnerStep->getType()) > SE.getTypeSizeInBits(PtrTy))
    return std::nullopt;

  const SCEV *MaxBTC = SE.getNoopOrZeroExtend(InnerMaxBTC, PtrTy);
  const SCEV *Step = SE.getNoopOrZeroExtend(InnerStep, PtrTy);
  const SCEV *Span = SE.getMulExpr(MaxBTC, Step);
  const SCEV *AccessSz = SE.getConstant(PtrTy, AccessSize);
  return InnerRange{Start, SE.getAddExpr(Start, SE.getAddExpr(Span, AccessSz)),
                    InnerStep};
}

/// Returns true if the base objects of \p PtrA and \p PtrB provably cannot
/// point to the same object. Mirrors the distinct-underlying-object reasoning
/// in BasicAAResult::aliasCheck: two distinct underlying objects do not alias
/// when (a) both are identified objects, or (b) one is an argument and the
/// other is an identified function-local object (alloca / noalias call /
/// noalias or byval argument). It is NOT enough for just one base to be a
/// noalias argument or alloca: such a pointer only guarantees non-aliasing
/// with pointers not based on it, so the *other* base could be derived from it
/// via an opaque op (a call or load result) that getUnderlyingObject cannot
/// trace.
static bool provablyDistinctObjects(const VPValue *PtrA, const VPValue *PtrB) {
  // Use the VPlan-level base pointers rather than reaching into the underlying
  // IR for AA.
  auto GetIRBase = [](const VPValue *Ptr) -> const Value * {
    // Follow a chain of GEP recipes to the base pointer.
    while (auto *VPI =
               dyn_cast_or_null<VPInstruction>(Ptr->getDefiningRecipe())) {
      if (VPI->getOpcode() != Instruction::GetElementPtr)
        break;
      Ptr = VPI->getOperand(0);
    }
    const auto *IRV = dyn_cast<VPIRValue>(Ptr);
    return IRV ? IRV->getValue() : nullptr;
  };
  // An argument is not "based on" a distinct identified function-local object,
  // so the two cannot alias.
  const Value *ObjA = GetIRBase(PtrA);
  const Value *ObjB = GetIRBase(PtrB);
  if (!ObjA || !ObjB || ObjA == ObjB)
    return false;
  return (isIdentifiedObject(ObjA) && isIdentifiedObject(ObjB)) ||
         (isa<Argument>(ObjA) && isIdentifiedFunctionLocal(ObjB)) ||
         (isa<Argument>(ObjB) && isIdentifiedFunctionLocal(ObjA));
}

/// If \p A and \p B are the same interleaved access pattern, return the maximum
/// vectorization factor for which their lanes are guaranteed to be distinct.
///
/// Outer-loop vectorization with factor VF runs VF adjacent outer iterations as
/// lanes of one vector iteration, and vector iterations execute in program
/// order. For an address {{base,+,OuterStep}<outer>,+,InnerStep}<inner>, lane l
/// at inner iteration j touches offset l*OuterStep + j*InnerStep, so two lanes
/// collide only if InnerStep < OuterStep*VF, i.e. the access is collision-free
/// for all VF <= InnerStep/OuterStep. This certifies the column-major
/// A[i + j*M] pattern, which the whole-range overlap test rejects.
///
/// This requires \p A and \p B to share the same address recurrence, so their
/// addresses coincide only within a lane at equal offsets, where program order
/// is preserved. OuterStep must be at least both access sizes, as adjacent
/// lanes are only OuterStep bytes apart, and inbounds keeps all offsets inside
/// a single object, so the reasoning above holds in exact arithmetic.
static std::optional<unsigned> getLaneDistinctMaxVF(const OuterLoopMemAccess &A,
                                                    const OuterLoopMemAccess &B,
                                                    ScalarEvolution &SE) {
  const auto *OuterStep = dyn_cast<SCEVConstant>(A.OuterStep);
  if (!A.InnerStep || A.InnerStep != B.InnerStep || A.Start != B.Start ||
      !OuterStep || !OuterStep->getAPInt().isStrictlyPositive() ||
      OuterStep->getAPInt().ult(A.AccessSize) ||
      OuterStep->getAPInt().ult(B.AccessSize) ||
      !vputils::getGEPFlagsForPtr(A.Ptr).isInBounds() ||
      !vputils::getGEPFlagsForPtr(B.Ptr).isInBounds())
    return std::nullopt;

  // The ratio is rounded down, which only lowers the bound.
  const auto *Ratio =
      dyn_cast<SCEVConstant>(SE.getUDivExpr(A.InnerStep, OuterStep));
  if (!Ratio || !Ratio->getAPInt().ugt(1))
    return std::nullopt;
  return Ratio->getAPInt().getLimitedValue(MaxSafeVFUnbounded);
}

std::optional<unsigned>
llvm::verifyOuterLoopMemorySafety(VPlan &Plan, PredicatedScalarEvolution &PSE,
                                  Loop *OuterLoop) {
  ScalarEvolution &SE = *PSE.getSE();
  const DataLayout &DL = Plan.getDataLayout();

  // We only handle single-level nesting (outer loop with one inner loop). For
  // deeper nests, an access that varies in a loop nested inside InnerLoop has
  // an address AddRec for that loop, as SCEV nests the innermost loop
  // outermost. No outer-loop start recurrence can be extracted from it below,
  // so such a nest is rejected; accesses invariant in the deeper loops are
  // analyzed as usual.
  ArrayRef<Loop *> SubLoops = OuterLoop->getSubLoops();
  if (SubLoops.size() != 1)
    return std::nullopt;
  Loop *InnerLoop = SubLoops.front();

  // The address ranges below use a single inner-loop bound for every outer
  // iteration, so that bound must not vary with the outer loop. (Inner loops
  // with a non-uniform trip count are already rejected by legality, as
  // outer-loop vectorization runs the inner loop once for all lanes.)
  const SCEV *InnerMaxBTC = SE.getSymbolicMaxBackedgeTakenCount(InnerLoop);
  if (isa<SCEVCouldNotCompute>(InnerMaxBTC) ||
      !SE.isLoopInvariant(InnerMaxBTC, OuterLoop))
    return std::nullopt;

  SmallVector<OuterLoopMemAccess, 8> Accesses;

  // Walk VPlan recipes. All structural decisions come from recipe opcodes
  // and VPValue operands; SCEV lookups go through getSCEVExprForVPValue.
  for (VPBasicBlock *VPBB : VPBlockUtils::blocksOnly<VPBasicBlock>(
           vp_depth_first_deep(Plan.getEntry()))) {
    for (VPRecipeBase &R : *VPBB) {
      auto *VPI = dyn_cast<VPInstruction>(&R);
      if (!VPI)
        continue;

      unsigned Opcode = VPI->getOpcode();
      VPValue *PtrOp = nullptr;
      Type *AccessTy = nullptr;
      bool IsStore = false;
      if (Opcode == Instruction::Load) {
        PtrOp = VPI->getOperand(0);
        AccessTy = VPI->getScalarType();
      } else if (Opcode == Instruction::Store) {
        PtrOp = VPI->getOperand(1);
        AccessTy = VPI->getOperand(0)->getScalarType();
        IsStore = true;
      } else {
        // Only calls to trivially vectorizable intrinsics can be widened.
        if (Opcode == Instruction::Call) {
          Function *Callee = VPI->getCalledFunction();
          if (!Callee || !isTriviallyVectorizable(Callee->getIntrinsicID()))
            return std::nullopt;
        }
        // Any other recipe accessing memory (atomicrmw, cmpxchg, fence, memory
        // intrinsics) is not covered by the load/store dependency reasoning
        // below and cannot be widened while preserving its semantics.
        if (VPI->mayReadOrWriteMemory())
          return std::nullopt;
        continue;
      }

      // Volatility and atomicity are not modeled on recipes at all, so this
      // has to come from the underlying load/store.
      auto *UI = dyn_cast_or_null<Instruction>(VPI->getUnderlyingValue());
      if (UI && (UI->isVolatile() || UI->isAtomic()))
        return std::nullopt;

      const SCEV *PtrSCEV =
          vputils::getSCEVExprForVPValue(PtrOp, PSE, OuterLoop);
      if (isa<SCEVCouldNotCompute>(PtrSCEV))
        return std::nullopt;

      uint64_t AccessSize = DL.getTypeStoreSize(AccessTy);
      auto Range =
          computeInnerRange(PtrSCEV, InnerLoop, InnerMaxBTC, AccessSize, SE);
      if (!Range)
        return std::nullopt;

      auto [Start, End, InnerStep] = *Range;
      // Extract the outer step from Start; for a[i*M+j], Start takes the form
      // {base, +, OuterStep}<outer>.
      const SCEV *OuterStep = nullptr;
      match(Start, m_scev_AffineAddRec(m_SCEV(), m_SCEV(OuterStep),
                                       m_SpecificLoop(OuterLoop)));
      LLVM_DEBUG(dbgs() << "LV: outer-loop access " << *PtrSCEV
                        << " IsStore=" << IsStore << " range [" << *Start
                        << ", " << *End << ")\n");

      // Stores must have an outer-loop-dependent address; otherwise they
      // WAW across outer iterations.
      if (!OuterStep && IsStore)
        return std::nullopt;

      Accesses.push_back(
          {PtrOp, Start, End, OuterStep, InnerStep, AccessSize, IsStore});
    }
  }

  if (none_of(Accesses, [](const OuterLoopMemAccess &A) { return A.IsStore; }))
    return MaxSafeVFUnbounded;

  // The largest VF for which every access pair is proven safe. Pairs proven
  // safe regardless of VF (via the whole-range / distinct-base reasoning) leave
  // this unbounded; a pair proven safe only via lane-distinctness lowers it to
  // the inner stride (in elements), since that interleaving stays
  // collision-free only while VF <= inner-stride.
  unsigned GlobalMaxSafeVF = MaxSafeVFUnbounded;

  for (const auto &[I, A] : enumerate(Accesses)) {
    for (const OuterLoopMemAccess &B : ArrayRef(Accesses).drop_front(I)) {
      if (!A.IsStore && !B.IsStore)
        continue;

      if (provablyDistinctObjects(A.Ptr, B.Ptr))
        continue;

      if (!A.OuterStep || !B.OuterStep)
        return std::nullopt;

      // Lane-distinctness: accesses interleaved across the lanes of a vector
      // iteration are safe up to a bounded factor.
      if (std::optional<unsigned> MaxVF = getLaneDistinctMaxVF(A, B, SE)) {
        LLVM_DEBUG(
            dbgs() << "LV: lane-distinct interleaved access, max safe VF="
                   << *MaxVF << "\n");
        GlobalMaxSafeVF = std::min(GlobalMaxSafeVF, *MaxVF);
        continue;
      }

      // Adjacent-iteration non-overlap:
      //   EndA(i) <= StartB(i+1) = StartB(i) + OuterStepB
      //   EndB(i) <= StartA(i+1) = StartA(i) + OuterStepA
      const SCEV *NextStartA = SE.getAddExpr(A.Start, A.OuterStep);
      const SCEV *NextStartB = SE.getAddExpr(B.Start, B.OuterStep);
      if (!SE.isKnownPredicate(ICmpInst::ICMP_ULE, A.End, NextStartB) ||
          !SE.isKnownPredicate(ICmpInst::ICMP_ULE, B.End, NextStartA))
        return std::nullopt;

      // Monotonicity: extend the adjacent-iteration non-overlap to all
      // iteration distances. Inbounds asserts the final pointer stays in
      // the allocation but does not imply unsigned ordering of successive
      // Start values, so rely on SCEV's nowrap-based reasoning instead:
      // require the Start AddRec to have nuw (no unsigned wrap when
      // adding the outer step) and the outer step to be non-negative.
      auto IsMonotonic = [&](const SCEV *Start, const SCEV *Step) {
        const auto *AR = dyn_cast<SCEVAddRecExpr>(Start);
        if (!AR || AR->getLoop() != OuterLoop || !AR->hasNoUnsignedWrap())
          return false;
        return SE.isKnownNonNegative(Step);
      };
      if (!IsMonotonic(A.Start, A.OuterStep) ||
          !IsMonotonic(B.Start, B.OuterStep))
        return std::nullopt;
    }
  }

  return GlobalMaxSafeVF;
}
