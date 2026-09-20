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
#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/Analysis/LoopAccessAnalysis.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/ScalarEvolutionExpressions.h"
#include "llvm/Analysis/TargetTransformInfo.h"

using namespace llvm;
using namespace VPlanPatternMatch;

#define DEBUG_TYPE "vplan"

void llvm::collectEphemeralRecipesForVPlan(
    VPlan &Plan, DenseSet<VPRecipeBase *> &EphRecipes) {
  // First, collect seed recipes which are operands of assumes.
  SmallVector<VPRecipeBase *> Worklist;
  for (VPBasicBlock *VPBB : VPBlockUtils::blocksOnly<VPBasicBlock>(
           vp_depth_first_deep(Plan.getVectorLoopRegion()->getEntry()))) {
    for (VPReplicateRecipe &RepR : make_isa_range<VPReplicateRecipe>(*VPBB)) {
      if (!match(&RepR, m_Intrinsic<Intrinsic::assume>()))
        continue;
      Worklist.push_back(&RepR);
      EphRecipes.insert(&RepR);
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

SmallVector<VPRegisterUsage, 8>
llvm::calculateRegisterUsageForPlan(VPlan &Plan, ArrayRef<ElementCount> VFs,
                                    const TargetTransformInfo &TTI) {
  DenseSet<VPRecipeBase *> EphemeralRecipes;
  collectEphemeralRecipesForVPlan(Plan, EphemeralRecipes);

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
      for (auto &WideIV : make_isa_range<VPWidenIntOrFpInductionRecipe>(
               LoopRegion->getEntryBasicBlock()->phis())) {
        EndPoint[&WideIV] = Idx2Recipe.size();
        Ends.insert(&WideIV);
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

    // Skip recipes for ephemeral values, i.e. those only feeding assumes. They
    // are removed before code generation and must not contribute to the
    // register pressure of the plan.
    if (EphemeralRecipes.contains(R))
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
// Outer-loop memory safety analysis.
//===----------------------------------------------------------------------===//

/// Returns the object \p Ptr is based on by following its chain of GEP recipes,
/// or nullptr if the chain does not end at a live-in.
static const Value *getBaseObject(VPValue *Ptr) {
  while (auto *VPI = dyn_cast<VPInstruction>(Ptr)) {
    if (VPI->getOpcode() != Instruction::GetElementPtr)
      break;
    Ptr = VPI->getOperand(0);
  }
  auto *IRV = dyn_cast<VPIRValue>(Ptr);
  return IRV ? IRV->getValue() : nullptr;
}

/// Returns true if \p AA can prove \p ObjA and \p ObjB are distinct objects.
/// Both must be known; an unknown object may be derived from the other through
/// an opaque operation, which even a noalias base does not rule out.
static bool provablyDistinctObjects(AAResults &AA, const Value *ObjA,
                                    const Value *ObjB) {
  return ObjA && ObjB && AA.isNoAlias(ObjA, ObjB);
}

/// Returns true if the store recipe \p Store writes disjoint bytes on every
/// iteration of \p OuterLoop, so that no two lanes of a vector iteration write
/// the same location.
static bool writesDisjointBytesPerIteration(VPInstruction *Store,
                                            PredicatedScalarEvolution &PSE,
                                            const Loop *OuterLoop) {
  VPValue *Addr = Store->getOperand(1);
  // Every address an inbounds GEP forms on an iteration where it is
  // dereferenced - here by the store itself - lies in one allocated object, so
  // the offsets it adds do not wrap the address space. The addresses of two
  // iterations therefore differ exactly by the difference of their offsets,
  // which is what the step comparison below relies on.
  if (!vputils::getGEPFlagsForPtr(Addr).isInBounds())
    return false;

  const DataLayout &DL = Store->getParent()->getPlan()->getDataLayout();
  TypeSize StoreSize =
      DL.getTypeStoreSize(Store->getOperand(0)->getScalarType());
  if (StoreSize.isScalable())
    return false;

  // The address must advance with the outer loop by at least the number of
  // bytes written. Requiring the outer loop's own recurrence also rules out an
  // address varying in a nested loop: SCEV nests the innermost loop outermost,
  // so such an address is a recurrence of that loop instead.
  ScalarEvolution &SE = *PSE.getSE();
  const auto *AR = dyn_cast<SCEVAddRecExpr>(
      vputils::getSCEVExprForVPValue(Addr, PSE, OuterLoop));
  if (!AR || AR->getLoop() != OuterLoop || !AR->isAffine())
    return false;
  const auto *Step = dyn_cast<SCEVConstant>(AR->getStepRecurrence(SE));
  return Step && Step->getAPInt().abs().uge(StoreSize.getFixedValue());
}

/// Returns the range of memory \p Access touches over all iterations of
/// \p OuterLoop and the loops nested inside it, or
/// std::nullopt if it cannot be bounded. The access must execute on every
/// iteration of each recurrence whose range is bounded.
static std::optional<VPMemoryRange>
getAccessRange(VPInstruction &Access, PredicatedScalarEvolution &PSE,
               const Loop *OuterLoop, const VPDominatorTree &VPDT,
               const DenseMap<const Loop *, VPBasicBlock *> &LoopHeaders) {
  VPlan &Plan = *Access.getParent()->getPlan();
  auto [OuterHeader, OuterLatch] = VPBlockUtils::getPlainCFGHeaderAndLatch(Plan);
  if (!VPDT.dominates(OuterHeader, Access.getParent()) ||
      !VPDT.dominates(Access.getParent(), OuterLatch))
    return std::nullopt;

  bool IsStore = Access.getOpcode() == Instruction::Store;
  VPValue *Addr = Access.getOperand(IsStore ? 1 : 0);
  Type *AccessTy = (IsStore ? Access.getOperand(0) : Access.getVPSingleValue())
                       ->getScalarType();
  // Every address an inbounds GEP forms on an iteration where it is
  // dereferenced lies in one allocated object, so none of the recurrences below
  // wraps the address space: they are monotonic, and evaluating them at a
  // backedge-taken count gives the last address they form. Below we check
  // that the access executes on every iteration of each recurrence, so that
  // the intermediate addresses are dereferenced as well.
  if (!vputils::getGEPFlagsForPtr(Addr).isInBounds())
    return std::nullopt;

  ScalarEvolution &SE = *PSE.getSE();
  const SCEV *Expr = vputils::getSCEVExprForVPValue(Addr, PSE, OuterLoop);
  if (isa<SCEVCouldNotCompute>(Expr))
    return std::nullopt;

  const DataLayout &DL = Plan.getDataLayout();
  const SCEV *AccessSize =
      SE.getStoreSizeOfExpr(DL.getIndexType(Expr->getType()), AccessTy);

  // Collapse the recurrences of the loops nested inside the outer loop into the
  // number of bytes a single outer-loop iteration touches. SCEV nests the
  // innermost loop outermost, so those recurrences are peeled off first.
  while (const auto *AR = dyn_cast<SCEVAddRecExpr>(Expr)) {
    const Loop *L = AR->getLoop();
    if (L == OuterLoop)
      break;
    if (!OuterLoop->contains(L) || !AR->isAffine() ||
        !SE.isKnownNonNegative(AR->getStepRecurrence(SE)))
      return std::nullopt;
    // Use the current plan's recurrence domain. A load after the inner loop
    // only dereferences its final pointer; intermediate inbounds GEPs may be
    // poison, so they cannot establish a non-wrapping range.
    VPBasicBlock *Header = LoopHeaders.lookup(L);
    if (!Header || Header->getNumPredecessors() != 2)
      return std::nullopt;
    auto *Pred0 = Header->getPredecessors()[0];
    auto *Pred1 = Header->getPredecessors()[1];
    bool Backedge0 = VPDT.dominates(Header, Pred0);
    bool Backedge1 = VPDT.dominates(Header, Pred1);
    if (Backedge0 == Backedge1)
      return std::nullopt;
    // Nested predecessor order is only canonicalized by createLoopRegions,
    // which runs after this analysis.
    auto *Latch = Backedge0 ? Pred0 : Pred1;
    auto *Preheader = Backedge0 ? Pred1 : Pred0;
    if (!VPDT.dominates(Preheader, Header) ||
        !VPDT.dominates(Header, Access.getParent()) ||
        !VPDT.dominates(Access.getParent(), Latch))
      return std::nullopt;

    const SCEV *BTC = SE.getBackedgeTakenCount(L);
    if (isa<SCEVCouldNotCompute>(BTC) || !SE.isLoopInvariant(BTC, OuterLoop))
      return std::nullopt;
    // With a non-negative step the addresses of the nested loop grow from the
    // first to the last iteration, so their distance is the span it adds.
    const SCEV *Span =
        SE.getMinusSCEV(AR->evaluateAtIteration(BTC, SE), AR->getStart());
    if (isa<SCEVCouldNotCompute>(Span))
      return std::nullopt;
    AccessSize = SE.getAddExpr(AccessSize, Span);
    Expr = AR->getStart();
  }

  // What remains must vary with the outer loop alone, if at all.
  const auto *AR = dyn_cast<SCEVAddRecExpr>(Expr);
  if (AR ? AR->getLoop() != OuterLoop || !AR->isAffine()
         : !SE.isLoopInvariant(Expr, OuterLoop))
    return std::nullopt;

  // The span of a nested recurrence can vary with the outer iteration even
  // when its trip count is invariant. Such ranges cannot be checked before
  // entering the outer loop with this bound computation.
  if (!SE.isLoopInvariant(AccessSize, OuterLoop))
    return std::nullopt;

  const SCEV *BTC = SE.getBackedgeTakenCount(OuterLoop);
  if (isa<SCEVCouldNotCompute>(BTC))
    return std::nullopt;
  std::optional<ScalarEvolution::LoopGuards> LoopGuards;
  auto [Start, End] =
      getStartAndEndForAccess(OuterLoop, Expr, AccessSize, BTC, BTC, &SE,
                              /*PointerBounds=*/nullptr, /*DT=*/nullptr,
                              /*AC=*/nullptr, LoopGuards);
  if (isa<SCEVCouldNotCompute>(Start) || isa<SCEVCouldNotCompute>(End))
    return std::nullopt;
  return VPMemoryRange{Start, End};
}

bool llvm::proveOuterLoopMemorySafety(VPlan &Plan,
                                      PredicatedScalarEvolution &PSE,
                                      AAResults &AA,
                                      const VPDominatorTree &VPDT,
                                      Loop *OuterLoop) {
  // An access of the nest: the object it is based on, null if it could not be
  // determined, and the memory it touches, empty if it could not be bounded.
  struct Access {
    VPInstruction *Recipe;
    const Value *Object;
    bool IsParallel;
    std::optional<VPMemoryRange> Range;
  };
  SmallVector<Access, 8> Loads, Stores;
  DenseMap<const Loop *, VPBasicBlock *> LoopHeaders;
  for (VPBasicBlock *VPBB : VPBlockUtils::blocksOnly<VPBasicBlock>(
           vp_depth_first_deep(Plan.getEntry()))) {
    // VPIRBasicBlocks wrap IR outside the vectorized loop; running several of
    // the loop's iterations as lanes does not reorder it.
    if (isa<VPIRBasicBlock>(VPBB))
      continue;
    for (VPRecipeBase &R : *VPBB) {
      if (auto *Phi = dyn_cast<VPPhi>(&R))
        if (const Loop *L = Phi->getSCEVLoop())
          LoopHeaders.try_emplace(L, VPBB);
      if (!R.mayReadOrWriteMemory())
        continue;
      // Anything touching memory that is not a plain load or store - atomicrmw,
      // cmpxchg, fence, memory intrinsics - is not covered by the reasoning
      // below.
      auto *VPI = dyn_cast<VPInstruction>(&R);
      if (!VPI)
        return false;
      // These intrinsics model optimization constraints through inaccessible
      // memory, not accesses to objects used by the loop's loads and stores.
      switch (vputils::getIntrinsicID(VPI)) {
      case Intrinsic::assume:
      case Intrinsic::sideeffect:
      case Intrinsic::pseudoprobe:
        continue;
      default:
        break;
      }
      if (VPI->getOpcode() != Instruction::Load &&
          VPI->getOpcode() != Instruction::Store)
        return false;

      MDNode *AccessGroup = VPI->getMetadata(LLVMContext::MD_access_group);
      auto IsParallelGroup = [&](Metadata *Group) {
        return is_contained(Plan.getParallelAccessGroups(), Group);
      };
      bool IsParallel =
          AccessGroup &&
          (AccessGroup->getNumOperands() == 0
               ? IsParallelGroup(AccessGroup)
               : any_of(AccessGroup->operands(), [&](const MDOperand &Group) {
                   return IsParallelGroup(Group.get());
                 }));

      bool IsStore = VPI->getOpcode() == Instruction::Store;
      VPValue *Addr = VPI->getOperand(IsStore ? 1 : 0);
      (IsStore ? Stores : Loads)
          .push_back({VPI, getBaseObject(Addr), IsParallel, std::nullopt});
    }
  }

  if (!all_of(Stores, [&](const Access &Store) {
        return Store.IsParallel ||
               writesDisjointBytesPerIteration(Store.Recipe, PSE, OuterLoop);
      }))
    return false;

  // Only compute a range when annotations and static disambiguation could
  // not discharge a pair. Cache bounds shared by several pairs.
  auto GetRange = [&](Access &A) -> const std::optional<VPMemoryRange> & {
    if (!A.Range)
      A.Range = getAccessRange(*A.Recipe, PSE, OuterLoop, VPDT, LoopHeaders);
    return A.Range;
  };

  // Each pair involving a store must be independent across outer iterations.
  // Access groups can establish this for a pair even if other accesses need
  // disambiguation. Otherwise separate the objects statically or require
  // disjoint ranges at runtime, provided both bounds are known and have the
  // same pointer type. Two loads need no disambiguation.
  unsigned NumPredicates = 0;
  for (auto [I, Store] : enumerate(Stores))
    for (Access &Other : concat<Access>(
             MutableArrayRef(Stores).drop_front(I + 1), MutableArrayRef(Loads))) {
      if ((Store.IsParallel && Other.IsParallel) ||
          provablyDistinctObjects(AA, Store.Object, Other.Object))
        continue;
      if (!GetRange(Store) || !GetRange(Other) ||
          Store.Range->Start->getType() != Other.Range->Start->getType())
        return false;
      if (++NumPredicates > VectorizerParams::RuntimeMemoryCheckThreshold)
        return false;
      Plan.addPredicate(std::make_unique<VPNoMemoryOverlapPredicate>(
          *Store.Range, *Other.Range));
    }
  return true;
}
