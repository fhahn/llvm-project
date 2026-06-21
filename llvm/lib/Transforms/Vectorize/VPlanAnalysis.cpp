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

template void DomTreeBuilder::Calculate<DominatorTreeBase<VPBlockBase, false>>(
    DominatorTreeBase<VPBlockBase, false> &DT);

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

        if (VFs[J].isScalar() ||
            isa<VPRegionValue, VPReplicateRecipe, VPDerivedIVRecipe,
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

/// Per-outer-iteration address range [Start, End) over the inner loop for
/// one memory access recipe.
struct OuterLoopMemAccess {
  VPInstruction *Recipe;
  const VPValue *Ptr;
  const SCEV *Start;
  const SCEV *End;
  const SCEV *OuterStep;
  /// The inner-loop step of the address AddRec (in bytes), or nullptr if the
  /// access is inner-loop invariant. Used to recognize interleaved accesses
  /// whose lanes stay distinct across the vectorized outer loop.
  const SCEV *InnerStep;
  /// The number of bytes accessed (the store size of the access type). The
  /// lane-distinctness reasoning requires the per-lane outer stride to be at
  /// least this large, otherwise adjacent lanes overlap in bytes.
  uint64_t AccessSize;
  bool IsStore;
};

} // end anonymous namespace

/// Compute the per-outer-iteration address range [Start, End) for a pointer
/// SCEV, using pure SCEV operations. If \p PtrSCEV is an inner-loop AddRec
/// {Start, +, InnerStep}<inner>, the range is
/// [Start, Start + InnerMaxBTC*InnerStep + AccessSize). Otherwise the
/// access is inner-invariant and the range is [Ptr, Ptr + AccessSize).
/// Returns a pair of SCEVCouldNotCompute on failure (e.g. a non-positive
/// inner step would invert the range under unsigned arithmetic).
static std::pair<const SCEV *, const SCEV *>
computeInnerRange(const SCEV *PtrSCEV, const Loop *InnerLoop,
                  const SCEV *InnerMaxBTC, uint64_t AccessSize,
                  ScalarEvolution &SE, const SCEV **InnerStepOut = nullptr) {
  // All address arithmetic below is done in the pointer expression's integer
  // type so that getAddExpr never sees mismatched operand widths (the inner
  // backedge-taken count and inner step may be narrower, e.g. an i32 inner
  // counter with i64-width addresses).
  Type *PtrTy = SE.getEffectiveSCEVType(PtrSCEV->getType());
  const auto *InnerAR = dyn_cast<SCEVAddRecExpr>(PtrSCEV);
  if (InnerStepOut)
    *InnerStepOut = nullptr;
  if (!InnerAR || InnerAR->getLoop() != InnerLoop || !InnerAR->isAffine()) {
    const SCEV *Sz = SE.getConstant(PtrTy, AccessSize);
    return {PtrSCEV, SE.getAddExpr(PtrSCEV, Sz)};
  }
  const SCEV *Start = InnerAR->getStart();
  const SCEV *InnerStep = InnerAR->getStepRecurrence(SE);
  if (InnerStepOut)
    *InnerStepOut = InnerStep;
  // The [Start, End) range is computed with unsigned arithmetic. A
  // non-positive InnerStep would yield End < Start, inverting the range
  // and making subsequent unsigned predicates spuriously hold. Bail out
  // unless the step is known strictly positive.
  //
  // For a symbolic step such as 4*%M the plain range query fails: the guarded
  // form knows %M >= 1, but isKnownPositive still cannot prove the scaled step
  // 4*%M positive because the product may overflow for very large %M. Prove
  // positivity of the index-level factor instead: the byte step is a product of
  // a positive constant (the access size) and the symbolic index step, so it is
  // positive iff that index step is. Strip a leading positive constant factor
  // and prove the remaining factor positive via the inner loop's guards (e.g.
  // the %M > 0 guard that gates the inner loop). The downstream range still
  // uses the original (possibly wrapping) InnerStep; a wrapped Span only widens
  // End, which the whole-range overlap test below rejects rather than accepts,
  // so admitting symbolic steps here cannot make a store pair pass unsoundly.
  if (!SE.isKnownPositive(InnerStep)) {
    bool KnownPositive = false;
    if (const auto *Mul = dyn_cast<SCEVMulExpr>(InnerStep))
      if (Mul->getNumOperands() == 2 &&
          SE.isKnownPositive(Mul->getOperand(0))) {
        const SCEV *RestGuarded =
            SE.applyLoopGuards(Mul->getOperand(1), InnerLoop);
        KnownPositive = SE.isKnownPositive(RestGuarded);
      }
    if (!KnownPositive) {
      const SCEV *CNC = SE.getCouldNotCompute();
      return {CNC, CNC};
    }
  }
  // Compute Span = MaxBTC * InnerStep in the pointer type, zero-extending the
  // (possibly narrower) backedge-taken count and step so no magnitude is lost
  // and all operands share the address width. If the backedge-taken count is
  // wider than the address type (exotic), bail out rather than narrow it.
  if (SE.getTypeSizeInBits(InnerMaxBTC->getType()) >
          SE.getTypeSizeInBits(PtrTy) ||
      SE.getTypeSizeInBits(InnerStep->getType()) >
          SE.getTypeSizeInBits(PtrTy)) {
    const SCEV *CNC = SE.getCouldNotCompute();
    return {CNC, CNC};
  }
  const SCEV *MaxBTC = SE.getNoopOrZeroExtend(InnerMaxBTC, PtrTy);
  const SCEV *Step = SE.getNoopOrZeroExtend(InnerStep, PtrTy);
  const SCEV *Span = SE.getMulExpr(MaxBTC, Step);
  const SCEV *AccessSz = SE.getConstant(PtrTy, AccessSize);
  return {Start, SE.getAddExpr(Start, SE.getAddExpr(Span, AccessSz))};
}

/// Walk the VPlan CFG together with the outer loop's subloop nest and map
/// each header VPBasicBlock to its corresponding IR Loop. Assumes
/// single-level nesting (one inner loop), which matches
/// verifyOuterLoopMemorySafety's precondition.
static DenseMap<const VPBasicBlock *, const Loop *>
buildHeaderVPBBToLoop(VPlan &Plan, const Loop *OuterLoop,
                      const Loop *InnerLoop) {
  DenseMap<const VPBasicBlock *, const Loop *> Map;
  const auto *OuterHeader = cast<VPBasicBlock>(
      Plan.getEntry()->getSuccessors()[1]->getSingleSuccessor());
  Map[OuterHeader] = OuterLoop;

  // Inner-loop headers have not been canonicalized at this stage, so we
  // can't rely on VPBlockUtils::isHeader (which assumes preheader is
  // operand 0, latch is operand 1). Instead, a header is the 2-pred VPBB
  // dominating one of its predecessors (the back-edge source).
  VPDominatorTree VPDT(Plan);
  for (VPBlockBase *VPB : vp_depth_first_deep(Plan.getEntry())) {
    auto *VPBB = dyn_cast<VPBasicBlock>(VPB);
    if (!VPBB || VPBB == OuterHeader)
      continue;
    ArrayRef<VPBlockBase *> Preds = VPBB->getPredecessors();
    if (Preds.size() != 2)
      continue;
    if (VPDT.dominates(VPBB, Preds[0]) || VPDT.dominates(VPBB, Preds[1]))
      Map[VPBB] = InnerLoop;
  }
  return Map;
}

/// Sentinel max-safe-VF meaning "no interleaving constraint": the loop's
/// safety does not depend on the vectorization factor.
static constexpr unsigned MaxSafeVFUnbounded =
    std::numeric_limits<unsigned>::max();

std::optional<unsigned>
llvm::verifyOuterLoopMemorySafety(VPlan &Plan, PredicatedScalarEvolution &PSE,
                                  Loop *OuterLoop) {
  ScalarEvolution &SE = *PSE.getSE();
  const DataLayout &DL = Plan.getDataLayout();

  // We only handle single-level nesting (outer loop with one inner loop).
  ArrayRef<Loop *> SubLoops = OuterLoop->getSubLoops();
  if (SubLoops.size() != 1)
    return std::nullopt;
  Loop *InnerLoop = SubLoops.front();

  const SCEV *InnerBTC = SE.getBackedgeTakenCount(InnerLoop);
  const SCEV *InnerMaxBTC = SE.getSymbolicMaxBackedgeTakenCount(InnerLoop);
  if (isa<SCEVCouldNotCompute>(InnerBTC) ||
      isa<SCEVCouldNotCompute>(InnerMaxBTC))
    return std::nullopt;

  // Reject triangular loops: a single InnerBTC must bound all outer
  // iterations, i.e. the inner trip count must be outer-loop-invariant.
  if (!SE.isLoopInvariant(InnerBTC, OuterLoop) ||
      !SE.isLoopInvariant(InnerMaxBTC, OuterLoop))
    return std::nullopt;

  // Build the header-VPBB → IR Loop map by walking the VPlan CFG and the
  // outer loop's subloop nest in parallel.
  DenseMap<const VPBasicBlock *, const Loop *> HeaderVPBBToLoop =
      buildHeaderVPBBToLoop(Plan, OuterLoop, InnerLoop);

  SmallVector<OuterLoopMemAccess, 8> Accesses;

  // Walk VPlan recipes. All structural decisions come from recipe opcodes
  // and VPValue operands; SCEV lookups go through getSCEVExprForVPValue.
  for (VPBlockBase *VPB : vp_depth_first_deep(Plan.getEntry())) {
    auto *VPBB = dyn_cast<VPBasicBlock>(VPB);
    if (!VPBB)
      continue;
    for (VPRecipeBase &R : *VPBB) {
      auto *VPI = dyn_cast<VPInstruction>(&R);
      if (!VPI)
        continue;

      unsigned Opcode = VPI->getOpcode();

      // Atomics and fences cannot be widened while preserving their
      // ordering / atomicity across outer iterations.
      if (Opcode == Instruction::AtomicRMW ||
          Opcode == Instruction::AtomicCmpXchg || Opcode == Instruction::Fence)
        return std::nullopt;

      // For call recipes, consult the callee Function via the VPIRValue
      // operand; only trivially-vectorizable intrinsics that don't access
      // memory are acceptable. Memory-accessing intrinsics (e.g.
      // llvm.masked.load/store, llvm.vp.*) take pointer operands that the
      // load/store-only dependency scan below does not analyze, so accept
      // only memory-free intrinsics here.
      if (Opcode == Instruction::Call) {
        Function *Callee = VPI->getCalledFunction();
        if (!Callee)
          return std::nullopt;
        Intrinsic::ID ID = Callee->getIntrinsicID();
        if (ID == Intrinsic::not_intrinsic || !isTriviallyVectorizable(ID) ||
            !Callee->doesNotAccessMemory())
          return std::nullopt;
        continue;
      }

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
        continue;
      }

      // Volatile / atomic load/store: the flag is not expressed at the
      // VPInstruction opcode level. Inspect the underlying Load/Store only
      // to read its simple-access flag — this is the absolutely-necessary
      // IR access for recipes that don't yet model volatility on recipes.
      if (auto *UI = dyn_cast_or_null<Instruction>(VPI->getUnderlyingValue())) {
        if (auto *LI = dyn_cast<LoadInst>(UI))
          if (!LI->isSimple())
            return std::nullopt;
        if (auto *SI = dyn_cast<StoreInst>(UI))
          if (!SI->isSimple())
            return std::nullopt;
      }

      const SCEV *PtrSCEV = vputils::getSCEVExprForVPValue(
          PtrOp, PSE, OuterLoop, &HeaderVPBBToLoop);
      LLVM_DEBUG(dbgs() << "VOLM: PtrSCEV=" << *PtrSCEV
                        << " IsStore=" << IsStore << "\n");
      if (isa<SCEVCouldNotCompute>(PtrSCEV))
        return std::nullopt;

      uint64_t AccessSize = DL.getTypeStoreSize(AccessTy);
      const SCEV *InnerStep = nullptr;
      auto [Start, End] = computeInnerRange(PtrSCEV, InnerLoop, InnerMaxBTC,
                                            AccessSize, SE, &InnerStep);
      LLVM_DEBUG(dbgs() << "VOLM: Start=" << *Start << " End=" << *End << "\n");
      if (isa<SCEVCouldNotCompute>(Start) || isa<SCEVCouldNotCompute>(End))
        return std::nullopt;

      // Extract the outer step from Start; for a[i*M+j], Start takes the
      // form {base, +, OuterStep}<outer>.
      const SCEV *OuterStep = nullptr;
      match(Start, m_scev_AffineAddRec(m_SCEV(), m_SCEV(OuterStep),
                                       m_SpecificLoop(OuterLoop)));
      LLVM_DEBUG(dbgs() << "VOLM: OuterStep="
                        << (OuterStep ? *OuterStep
                                      : *SE.getZero(SE.getEffectiveSCEVType(
                                            PtrSCEV->getType())))
                        << " match=" << (OuterStep ? "yes" : "no") << "\n");

      // Stores must have an outer-loop-dependent address; otherwise they
      // WAW across outer iterations.
      if (!OuterStep && IsStore)
        return std::nullopt;

      Accesses.push_back(
          {VPI, PtrOp, Start, End, OuterStep, InnerStep, AccessSize, IsStore});
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

      // If the two accesses derive from distinct base pointers that provably
      // cannot point to the same object, they cannot alias. This mirrors the
      // distinct-underlying-object reasoning in BasicAA
      // (BasicAAResult::aliasCheck): two distinct underlying objects do not
      // alias when (a) both are identified objects, or (b) one is an argument
      // and the other is an identified function-local object (alloca / noalias
      // call / noalias or byval argument). It is NOT enough for just one base
      // to be a noalias argument or alloca: such a pointer only guarantees
      // non-aliasing with pointers not based on it, so the *other* base could
      // be derived from it via an opaque op (a call or load result) that
      // getUnderlyingObject cannot trace. Use the VPlan-level base pointer
      // VPValue rather than reaching into the underlying IR for AA.
      const VPValue *BaseA = vputils::getUnderlyingObject(A.Ptr);
      const VPValue *BaseB = vputils::getUnderlyingObject(B.Ptr);
      if (BaseA != BaseB) {
        auto GetIRBase = [](const VPValue *V) -> const Value * {
          const auto *IRV = dyn_cast<VPIRValue>(V);
          return IRV ? IRV->getValue() : nullptr;
        };
        // An argument is not "based on" a distinct identified function-local
        // object, so the two cannot alias.
        auto IsArgumentLike = [](const Value *V) {
          if (isa<Argument>(V))
            return true;
          const auto *E = dyn_cast<ExtractValueInst>(V);
          return E && isa<Argument>(E->getOperand(0));
        };
        const Value *ObjA = GetIRBase(BaseA);
        const Value *ObjB = GetIRBase(BaseB);
        if (ObjA && ObjB && ObjA != ObjB) {
          bool CannotAlias =
              (isIdentifiedObject(ObjA) && isIdentifiedObject(ObjB)) ||
              (IsArgumentLike(ObjA) && isIdentifiedFunctionLocal(ObjB)) ||
              (IsArgumentLike(ObjB) && isIdentifiedFunctionLocal(ObjA));
          if (CannotAlias)
            continue;
        }
      }

      if (!A.OuterStep || !B.OuterStep)
        return std::nullopt;

      // Lane-distinctness fast path for a self-pair (a single store against
      // itself). Outer-loop vectorization with factor VF runs VF adjacent
      // outer iterations as lanes of one vector iteration; vector iterations
      // execute in program order. For an interleaved access whose address is
      // {{base,+,OuterStep}<outer>,+,InnerStep}<inner>, lane l at inner
      // iteration j touches offset l*OuterStep + j*InnerStep. Two lanes in the
      // same vector iteration collide only if InnerStep < OuterStep*VF, so the
      // access is collision-free for all VF <= InnerStep/OuterStep. Cross
      // vector-iteration accesses to the same location keep program order, so
      // last-writer-wins is preserved as in the scalar loop. This certifies the
      // column-major A[i + j*M] pattern (OuterStep = element size, InnerStep =
      // M elements) that the whole-range test below rejects. Applies when both
      // accesses share the *same* address recurrence (same Start, OuterStep and
      // InnerStep): their addresses coincide only within a single lane at equal
      // offsets, where program order within the lane is preserved, and differ
      // across lanes by a non-zero multiple of OuterStep that InnerStep never
      // reaches. The per-lane stride OuterStep must also be at least the access
      // size, otherwise adjacent lanes (OuterStep bytes apart) overlap in the
      // bytes they touch even though their start offsets differ. Distinct
      // addresses can still collide and fall through to the whole-range /
      // distinct-base reasoning below.
      const auto *OuterStepC = dyn_cast<SCEVConstant>(A.OuterStep);
      if (A.InnerStep && B.InnerStep == A.InnerStep && A.Start == B.Start &&
          A.OuterStep == B.OuterStep && OuterStepC &&
          OuterStepC->getAPInt().uge(A.AccessSize) &&
          OuterStepC->getAPInt().uge(B.AccessSize)) {
        const SCEV *Ratio = SE.getUDivExpr(A.InnerStep, A.OuterStep);
        if (const auto *C = dyn_cast<SCEVConstant>(Ratio)) {
          const APInt &R = C->getAPInt();
          if (R.ugt(1)) {
            unsigned MaxVF = R.getLimitedValue(MaxSafeVFUnbounded);
            GlobalMaxSafeVF = std::min(GlobalMaxSafeVF, MaxVF);
            LLVM_DEBUG(dbgs() << "VOLM: lane-distinct interleaved access, "
                                 "maxSafeVF="
                              << MaxVF << "\n");
            continue;
          }
        }
      }

      // Adjacent-iteration non-overlap:
      //   EndA(i) <= StartB(i+1) = StartB(i) + OuterStepB
      //   EndB(i) <= StartA(i+1) = StartA(i) + OuterStepA
      const SCEV *NextStartA = SE.getAddExpr(A.Start, A.OuterStep);
      const SCEV *NextStartB = SE.getAddExpr(B.Start, B.OuterStep);
      LLVM_DEBUG({
        dbgs() << "VOLM: pair check\n"
               << "  A.Start=" << *A.Start << " A.End=" << *A.End << "\n"
               << "  B.Start=" << *B.Start << " B.End=" << *B.End << "\n"
               << "  NSA=" << *NextStartA << " NSB=" << *NextStartB << "\n"
               << "  A.End<=NSB: "
               << SE.isKnownPredicate(ICmpInst::ICMP_ULE, A.End, NextStartB)
               << " B.End<=NSA: "
               << SE.isKnownPredicate(ICmpInst::ICMP_ULE, B.End, NextStartA)
               << "\n";
      });
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
        LLVM_DEBUG(dbgs() << "VOLM: IsMonotonic Start=" << *Start
                          << " Step=" << *Step << " AR=" << (AR ? "yes" : "no")
                          << " sameLoop="
                          << (AR && AR->getLoop() == OuterLoop ? "y" : "n")
                          << " nuw=" << (AR ? AR->hasNoUnsignedWrap() : 0)
                          << " stepNN=" << SE.isKnownNonNegative(Step) << "\n");
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
