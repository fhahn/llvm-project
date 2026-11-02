//===- VPlanAnalysis.cpp - Various Analyses working on VPlan ----*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "VPlanAnalysis.h"
#include "LoopVectorizationPlanner.h"
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
    if (Ty->isTokenTy() || !VectorType::isValidElementType(Ty))
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

/// Returns the live-in IR value \p Ptr is based on, looking through GEPs, or
/// nullptr if there is none.
static const Value *getBaseObject(VPValue *Ptr) {
  while (auto *VPI = dyn_cast<VPInstruction>(Ptr)) {
    if (VPI->getOpcode() != Instruction::GetElementPtr)
      break;
    Ptr = VPI->getOperand(0);
  }
  auto *IRV = dyn_cast<VPIRValue>(Ptr);
  return IRV ? IRV->getValue() : nullptr;
}

/// Returns true if \p ObjA and \p ObjB are known and \p AA proves they don't
/// alias.
static bool provablyDistinctObjects(AAResults &AA, const Value *ObjA,
                                    const Value *ObjB) {
  return ObjA && ObjB && AA.isNoAlias(ObjA, ObjB);
}

/// Returns true if a store of \p StoredTy to \p Addr in \p VPBB writes
/// different locations on each iteration of \p OuterLoop.
static bool writesDisjointLocationsPerIteration(VPValue *Addr, Type *StoredTy,
                                                VPBasicBlock *VPBB,
                                                VPBasicBlock *Latch,
                                                const VPDominatorTree &VPDT,
                                                PredicatedScalarEvolution &PSE,
                                                const Loop *OuterLoop) {
  // The address cannot repeat if it is an nusw GEP on a live-in base executed
  // in every iteration, with a single index that does not wrap.
  auto *GEP = dyn_cast<VPInstruction>(Addr);
  if (!GEP || GEP->getOpcode() != Instruction::GetElementPtr ||
      GEP->getNumOperands() != 2 || !isa<VPIRValue>(GEP->getOperand(0)) ||
      !GEP->getGEPNoWrapFlags().hasNoUnsignedSignedWrap() ||
      !VPDT.dominates(VPBB, Latch))
    return false;
  auto *Idx = dyn_cast<SCEVAddRecExpr>(
      vputils::getSCEVExprForVPValue(GEP->getOperand(1), PSE, OuterLoop));
  if (!Idx || !Idx->hasNoUnsignedWrap())
    return false;

  if (hasIrregularType(StoredTy, Latch->getPlan()->getDataLayout()))
    return false;

  // Stride != 0 ensures no write-after-write.
  std::optional<int64_t> Stride =
      vputils::getConstantStride(Addr, StoredTy, PSE, OuterLoop);
  return Stride && *Stride != 0;
}

namespace {
/// A load or store of the nest, and the index of its range in the checked
/// ranges, once formed.
struct Access {
  VPValue *Addr;
  Type *AccessTy;
  VPBasicBlock *VPBB;
  std::optional<unsigned> RangeIdx = std::nullopt;
};
} // namespace

/// Returns the range of memory \p A touches over all iterations of \p
/// OuterLoop and the loop nested in it, or std::nullopt if it cannot be
/// bounded. \p Latch is the latch of \p OuterLoop, and \p NestedLatch the
/// latch of the loop nested in it, if there is exactly one.
static std::optional<VPMemoryChecksPredicate::Range>
getAccessRange(const Access &A, const VPBasicBlock *Latch,
               const VPBlockBase *NestedLatch, const VPDominatorTree &VPDT,
               PredicatedScalarEvolution &PSE, const Loop *OuterLoop,
               ScalarEvolution::LoopGuards &LoopGuards) {
  // Every address an inbounds GEP forms on an iteration where it is
  // dereferenced lies in one allocated object, so none of the recurrences
  // below wraps the address space: they are monotonic, and evaluating them at
  // a backedge-taken count gives the last address they form. The access must
  // execute on every iteration of the loops of those recurrences, so every
  // address the GEP forms is a dereferenced one.
  if (!vputils::getGEPFlagsForPtr(A.Addr).isInBounds() ||
      !VPDT.dominates(A.VPBB, Latch))
    return std::nullopt;

  ScalarEvolution &SE = *PSE.getSE();
  const SCEV *Expr = vputils::getSCEVExprForVPValue(A.Addr, PSE, OuterLoop);
  if (isa<SCEVCouldNotCompute>(Expr))
    return std::nullopt;
  const DataLayout &DL = Latch->getPlan()->getDataLayout();
  const SCEV *AccessSize =
      SE.getStoreSizeOfExpr(DL.getIndexType(Expr->getType()), A.AccessTy);

  // Collapse the recurrence of the loop nested in the outer loop into the
  // number of bytes a single outer-loop iteration touches. With a
  // non-negative step, the addresses grow from the first to the last
  // iteration of the nested loop, so their distance is the span it adds. Where
  // the span starts may vary with the outer loop, but its size must not.
  const auto *AR = dyn_cast<SCEVAddRecExpr>(Expr);
  if (AR && AR->getLoop() != OuterLoop && OuterLoop->contains(AR->getLoop())) {
    const SCEV *NestedBTC = SE.getBackedgeTakenCount(AR->getLoop());
    if (!AR->isAffine() || !SE.isKnownNonNegative(AR->getStepRecurrence(SE)) ||
        !NestedLatch || !VPDT.dominates(A.VPBB, NestedLatch) ||
        isa<SCEVCouldNotCompute>(NestedBTC))
      return std::nullopt;
    AccessSize = SE.getAddExpr(
        AccessSize, SE.getMinusSCEV(AR->evaluateAtIteration(NestedBTC, SE),
                                    AR->getStart()));
    Expr = AR->getStart();
  }

  // What remains must be invariant in the outer loop or an affine recurrence
  // of it.
  const auto *OuterAR = dyn_cast<SCEVAddRecExpr>(Expr);
  bool IsOuterAffine =
      OuterAR && OuterAR->getLoop() == OuterLoop && OuterAR->isAffine();
  if ((!IsOuterAffine && !SE.isLoopInvariant(Expr, OuterLoop)) ||
      !SE.isLoopInvariant(AccessSize, OuterLoop))
    return std::nullopt;
  const SCEV *OuterBTC = PSE.getBackedgeTakenCount();
  auto [Low, High] = getStartAndEndForAccess(
      OuterLoop, Expr, AccessSize, OuterBTC, OuterBTC, &SE,
      /*PointerBounds=*/nullptr, /*DT=*/nullptr, /*AC=*/nullptr, LoopGuards);
  if (isa<SCEVCouldNotCompute>(Low))
    return std::nullopt;
  return VPMemoryChecksPredicate::Range{Low, High};
}

bool llvm::proveOuterLoopMemorySafety(VPlan &Plan,
                                      PredicatedScalarEvolution &PSE,
                                      AAResults &AA,
                                      const VPDominatorTree &VPDT,
                                      const Loop *OuterLoop) {
  // Collect the loads and stores of the nest. Of the loads, and of the stores,
  // through the same address only the widest is kept, as the bytes it touches
  // include those of the others.
  using AccessMap = MapVector<VPValue *, Access>;
  AccessMap Loads, Stores;
  const DataLayout &DL = Plan.getDataLayout();
  auto IsNoWider = [&DL](Type *A, Type *B) {
    return TypeSize::isKnownLE(DL.getTypeStoreSize(A), DL.getTypeStoreSize(B));
  };
  auto AddAccess = [&IsNoWider](AccessMap &Accesses, Access A) {
    auto [It, Inserted] = Accesses.try_emplace(A.Addr, A);
    if (!Inserted && !IsNoWider(A.AccessTy, It->second.AccessTy))
      It->second = A;
  };
  SmallVector<const VPBlockBase *> NestedLatches;
  auto [Header, Latch] = VPBlockUtils::getPlainCFGHeaderAndLatch(Plan);
  for (VPBasicBlock *VPBB : vp_rpo_plain_cfg_loop_body(Header)) {
    if (VPBB != Latch && VPBlockUtils::isLatch(VPBB, VPDT))
      NestedLatches.push_back(VPBB);
    for (VPRecipeBase &R : *VPBB) {
      if (!R.mayReadOrWriteMemory())
        continue;
      VPValue *Addr, *StoredVal;
      if (match(&R, m_VPInstruction<Instruction::Load>(m_VPValue(Addr)))) {
        AddAccess(Loads, {Addr, R.getVPSingleValue()->getScalarType(), VPBB});
        continue;
      }
      // Bail out on any other memory accesses or write-after-write conflict in
      // the outer loop.
      if (!match(&R, m_VPInstruction<Instruction::Store>(m_VPValue(StoredVal),
                                                         m_VPValue(Addr))) ||
          !writesDisjointLocationsPerIteration(Addr, StoredVal->getScalarType(),
                                               VPBB, Latch, VPDT, PSE,
                                               OuterLoop))
        return false;
      AddAccess(Stores, {Addr, StoredVal->getScalarType(), VPBB});
    }
  }
  // Recurrences of a nested loop are only lowered to SCEV if it is the only
  // one (see getSCEVExprForVPValue).
  const VPBlockBase *NestedLatch =
      NestedLatches.size() == 1 ? NestedLatches.front() : nullptr;

  // Stores must access memory not accessed by any other load or store in the
  // nest, on a different outer-loop iteration. Other pairs that cannot be
  // separated statically are checked at runtime, if the memory both access can
  // be bounded and is in the same address space.
  SmallVector<VPMemoryChecksPredicate::Range> Ranges;
  SmallVector<VPMemoryChecksPredicate::RangeCheck> RangeChecks;
  auto LoopGuards =
      ScalarEvolution::LoopGuards::collectOnDemand(OuterLoop, *PSE.getSE());
  auto GetRangeIdx = [&, Latch = Latch](Access &A) {
    if (!A.RangeIdx) {
      if (std::optional<VPMemoryChecksPredicate::Range> Range = getAccessRange(
              A, Latch, NestedLatch, VPDT, PSE, OuterLoop, LoopGuards)) {
        A.RangeIdx = Ranges.size();
        Ranges.push_back(*Range);
      }
    }
    return A.RangeIdx;
  };
  auto StoreAccesses = make_second_range(Stores);
  auto LoadAccesses = make_second_range(Loads);
  for (auto [I, Store] : enumerate(StoreAccesses))
    for (Access &Other :
         concat<Access>(drop_begin(StoreAccesses, I + 1), LoadAccesses)) {
      // The address of a store advances by at least as many bytes as the store
      // writes on each outer-loop iteration (see
      // writesDisjointLocationsPerIteration), so a load through the same
      // address touching no more bytes only reads bytes of its own lane. The
      // ranges of a wider load and the store would always overlap.
      if (Store.Addr == Other.Addr) {
        if (!IsNoWider(Other.AccessTy, Store.AccessTy))
          return false;
        continue;
      }
      if (provablyDistinctObjects(AA, getBaseObject(Store.Addr),
                                  getBaseObject(Other.Addr)))
        continue;
      std::optional<unsigned> StoreIdx = GetRangeIdx(Store);
      std::optional<unsigned> OtherIdx = GetRangeIdx(Other);
      if (!StoreIdx || !OtherIdx ||
          Store.Addr->getScalarType() != Other.Addr->getScalarType() ||
          RangeChecks.size() == VectorizerParams::VectorizeMemoryCheckThreshold)
        return false;
      RangeChecks.push_back({*StoreIdx, *OtherIdx});
    }
  if (!RangeChecks.empty())
    Plan.addPredicate(std::make_unique<VPMemoryChecksPredicate>(
        std::move(Ranges), std::move(RangeChecks), OuterLoop));
  return true;
}
