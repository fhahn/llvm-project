//===- VPlanLowering.cpp - VPlan-to-VPlan lowering transforms -------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
///
/// \file
/// This file implements VPlan-to-VPlan lowering transformations, which
/// prepare an optimized VPlan for execution.
///
//===----------------------------------------------------------------------===//

#include "LoopVectorizationPlanner.h"
#include "VPlan.h"
#include "VPlanAnalysis.h"
#include "VPlanCFG.h"
#include "VPlanDominatorTree.h"
#include "VPlanHelpers.h"
#include "VPlanPatternMatch.h"
#include "VPlanTransforms.h"
#include "VPlanUtils.h"
#include "llvm/ADT/APInt.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/Analysis/IVDescriptors.h"
#include "llvm/Analysis/LoopAccessAnalysis.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/ScalarEvolutionPatternMatch.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/MDBuilder.h"
#include "llvm/IR/Metadata.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/TypeSize.h"
#include "llvm/Transforms/Utils/ScalarEvolutionExpander.h"

using namespace llvm;
using namespace VPlanPatternMatch;
using namespace SCEVPatternMatch;

void VPlanTransforms::replaceWideCanonicalIVWithWideIV(
    VPlan &Plan, ScalarEvolution &SE, const TargetTransformInfo &TTI,
    TargetTransformInfo::TargetCostKind CostKind, ElementCount VF, unsigned UF,
    const SmallPtrSetImpl<const Value *> &ValuesToIgnore) {
  VPRegionBlock *LoopRegion = Plan.getVectorLoopRegion();
  if (!LoopRegion)
    return;

  auto *WideCanIV =
      findUserOf<VPWidenCanonicalIVRecipe>(LoopRegion->getCanonicalIV());
  if (!WideCanIV)
    return;

  Type *CanIVTy = LoopRegion->getCanonicalIVType();

  // Replace the wide canonical IV with a scalar-iv-steps over the canonical
  // IV.
  if (Plan.hasScalarVFOnly() || vputils::onlyFirstLaneUsed(WideCanIV)) {
    VPBuilder Builder(WideCanIV);
    WideCanIV->replaceAllUsesWith(vputils::createScalarIVSteps(
        Plan, InductionDescriptor::IK_IntInduction, Instruction::Add, nullptr,
        nullptr, Plan.getZero(CanIVTy), Plan.getConstantInt(CanIVTy, 1),
        WideCanIV->getDebugLoc(), Builder,
        {static_cast<bool>(WideCanIV->getNoWrapFlags().HasNUW), false}));
    WideCanIV->eraseFromParent();
    return;
  }

  if (vputils::onlyScalarValuesUsed(WideCanIV))
    return;

  // If a canonical VPWidenIntOrFpInductionRecipe already produces vector lanes
  // in the header, reuse it instead of introducing another wide induction phi.
  VPBasicBlock *Header = LoopRegion->getEntryBasicBlock();
  for (VPRecipeBase &Phi : Header->phis()) {
    VPWidenIntOrFpInductionRecipe *WidenIV;
    if (!match(&Phi, m_CanonicalWidenIV(WidenIV)))
      continue;
    // The reused wide IV feeds the header mask, whose lanes may extend past
    // the trip count; drop flags that only hold inside the scalar loop.
    WidenIV->dropPoisonGeneratingFlags();
    WideCanIV->replaceAllUsesWith(WidenIV);
    WideCanIV->eraseFromParent();
    return;
  }

  // Introduce a new VPWidenIntOrFpInductionRecipe if profitable.
  auto *VecTy = VectorType::get(CanIVTy, VF);
  InstructionCost BroadcastCost = TTI.getShuffleCost(
      TargetTransformInfo::SK_Broadcast, VecTy, VecTy, {}, CostKind);
  InstructionCost PHICost = TTI.getCFInstrCost(Instruction::PHI, CostKind);
  if (PHICost > BroadcastCost)
    return;

  // Bail out if the additional wide induction phi increase the expected spill
  // cost.
  VPRegisterUsage UnrolledBase =
      calculateRegisterUsageForPlan(Plan, VF, TTI, ValuesToIgnore)[0];
  for (unsigned &NumUsers : make_second_range(UnrolledBase.MaxLocalUsers))
    NumUsers *= UF;
  unsigned RegClass = TTI.getRegisterClassForType(/*Vector=*/true, VecTy);
  VPRegisterUsage Projected = UnrolledBase;
  Projected.MaxLocalUsers[RegClass] += TTI.getRegUsageForType(VecTy);
  if (Projected.spillCost(TTI, CostKind) >
      UnrolledBase.spillCost(TTI, CostKind))
    return;

  InductionDescriptor ID =
      InductionDescriptor::getCanonicalIntInduction(CanIVTy, SE);
  VPValue *StepV = Plan.getConstantInt(CanIVTy, 1);
  auto *NewWideIV = new VPWidenIntOrFpInductionRecipe(
      /*IV=*/nullptr, Plan.getZero(CanIVTy), StepV, &Plan.getVF(), ID,
      WideCanIV->getNoWrapFlags(), WideCanIV->getDebugLoc());
  NewWideIV->insertBefore(&*Header->getFirstNonPhi());
  WideCanIV->replaceAllUsesWith(NewWideIV);
  WideCanIV->eraseFromParent();
}

// Add a VPActiveLaneMaskPHIRecipe and related recipes to \p Plan and replace
// the loop terminator with a branch-on-cond recipe with the negated
// active-lane-mask as operand. Note that this turns the loop into an
// uncountable one. Only the existing terminator is replaced, all other existing
// recipes/users remain unchanged, except for poison-generating flags being
// dropped from the canonical IV increment. Return the created
// VPActiveLaneMaskPHIRecipe.
//
// The function adds the following recipes:
//
// vector.ph:
//   %EntryInc = canonical-iv-increment-for-part CanonicalIVStart
//   %EntryALM = active-lane-mask %EntryInc, TC
//
// vector.body:
//   ...
//   %P = active-lane-mask-phi [ %EntryALM, %vector.ph ], [ %ALM, %vector.body ]
//   ...
//   %InLoopInc = canonical-iv-increment-for-part CanonicalIVIncrement
//   %ALM = active-lane-mask %InLoopInc, TC
//   %Negated = Not %ALM
//   branch-on-cond %Negated
//
static VPActiveLaneMaskPHIRecipe *
addVPLaneMaskPhiAndUpdateExitBranch(VPlan &Plan) {
  VPRegionBlock *TopRegion = Plan.getVectorLoopRegion();
  VPBasicBlock *EB = TopRegion->getExitingBasicBlock();
  VPValue *StartV = Plan.getZero(TopRegion->getCanonicalIVType());
  auto *CanonicalIVIncrement = TopRegion->getOrCreateCanonicalIVIncrement();
  // TODO: Check if dropping the flags is needed.
  TopRegion->clearCanonicalIVNUW(CanonicalIVIncrement);
  DebugLoc DL = CanonicalIVIncrement->getDebugLoc();
  // We can't use StartV directly in the ActiveLaneMask VPInstruction, since
  // we have to take unrolling into account. Each part needs to start at
  //   Part * VF
  auto *VecPreheader = Plan.getVectorPreheader();
  VPBuilder Builder(VecPreheader);

  // Create the ActiveLaneMask instruction using the correct start values.
  VPValue *TC = Plan.getTripCount();
  VPValue *VF = &Plan.getVF();

  auto *EntryIncrement =
      Builder.createOverflowingOp(VPInstruction::CanonicalIVIncrementForPart,
                                  {StartV, VF}, {}, DL, "index.part.next");

  // Create the active lane mask instruction in the VPlan preheader.
  VPValue *ALMMultiplier =
      Plan.getConstantInt(TopRegion->getCanonicalIVType(), 1);
  auto *EntryALM = Builder.createNaryOp(VPInstruction::ActiveLaneMask,
                                        {EntryIncrement, TC, ALMMultiplier}, DL,
                                        "active.lane.mask.entry");

  // Now create the ActiveLaneMaskPhi recipe in the main loop using the
  // preheader ActiveLaneMask instruction.
  auto *LaneMaskPhi =
      new VPActiveLaneMaskPHIRecipe(EntryALM, DebugLoc::getUnknown());
  auto *HeaderVPBB = TopRegion->getEntryBasicBlock();
  LaneMaskPhi->insertBefore(*HeaderVPBB, HeaderVPBB->begin());

  // Create the active lane mask for the next iteration of the loop before the
  // original terminator.
  VPRecipeBase *OriginalTerminator = EB->getTerminator();
  Builder.setInsertPoint(OriginalTerminator);
  auto *InLoopIncrement = Builder.createOverflowingOp(
      VPInstruction::CanonicalIVIncrementForPart,
      {CanonicalIVIncrement, &Plan.getVF()}, {}, DL);
  auto *ALM = Builder.createNaryOp(VPInstruction::ActiveLaneMask,
                                   {InLoopIncrement, TC, ALMMultiplier}, DL,
                                   "active.lane.mask.next");
  LaneMaskPhi->addBackedgeValue(ALM);

  // Replace the original terminator with BranchOnCond. We have to invert the
  // mask here because a true condition means jumping to the exit block.
  auto *NotMask = Builder.createNot(ALM, DL);
  Builder.createNaryOp(VPInstruction::BranchOnCond, {NotMask}, DL);
  OriginalTerminator->eraseFromParent();
  return LaneMaskPhi;
}

void VPlanTransforms::materializeHeaderMask(
    VPlan &Plan, bool UseActiveLaneMask, bool UseActiveLaneMaskForControlFlow) {
  VPRegionBlock *LoopRegion = Plan.getVectorLoopRegion();
  VPValue *HeaderMask = LoopRegion->getUsedHeaderMask();
  if (!HeaderMask)
    return;

  if (UseActiveLaneMaskForControlFlow) {
    HeaderMask->replaceAllUsesWith(addVPLaneMaskPhiAndUpdateExitBranch(Plan));
    return;
  }

  VPBasicBlock *Header = LoopRegion->getEntryBasicBlock();
  VPBuilder Builder(Header, Header->getFirstNonPhi());
  auto *WideCanonicalIV = Builder.insert(new VPWidenCanonicalIVRecipe(
      LoopRegion->getCanonicalIV(),
      VPIRFlags::WrapFlagsTy(/*HasNUW=*/true, /*HasNSW=*/false)));
  VPValue *Mask;
  if (UseActiveLaneMask) {
    VPValue *ALMMultiplier =
        Plan.getConstantInt(LoopRegion->getCanonicalIVType(), 1);
    Mask = Builder.createNaryOp(
        VPInstruction::ActiveLaneMask,
        {WideCanonicalIV, Plan.getTripCount(), ALMMultiplier}, nullptr,
        "active.lane.mask");
  } else {
    Mask = Builder.createICmp(CmpInst::ICMP_ULE, WideCanonicalIV,
                              Plan.getOrCreateBackedgeTakenCount());
  }
  HeaderMask->replaceAllUsesWith(Mask);
}

/// Expand a VPWidenIntOrFpInduction into executable recipes, for the initial
/// value, phi and backedge value. In the following example:
///
///  vector.ph:
///  Successor(s): vector loop
///
///  <x1> vector loop: {
///    vector.body:
///      WIDEN-INDUCTION %i = phi %start, %step, %vf
///      ...
///      EMIT branch-on-count ...
///    No successors
///  }
///
/// WIDEN-INDUCTION will get expanded to:
///
///  vector.ph:
///    ...
///    vp<%induction.start> = ...
///    vp<%induction.increment> = ...
///
///  Successor(s): vector loop
///
///  <x1> vector loop: {
///    vector.body:
///      ir<%i> = WIDEN-PHI vp<%induction.start>, vp<%vec.ind.next>
///      ...
///      vp<%vec.ind.next> = add ir<%i>, vp<%induction.increment>
///      EMIT branch-on-count ...
///    No successors
///  }
static void
expandVPWidenIntOrFpInduction(VPWidenIntOrFpInductionRecipe *WidenIVR) {
  VPlan *Plan = WidenIVR->getParent()->getPlan();
  VPValue *Start = WidenIVR->getStartValue();
  VPValue *Step = WidenIVR->getStepValue();
  VPValue *VF = WidenIVR->getVFValue();
  DebugLoc DL = WidenIVR->getDebugLoc();

  // The value from the original loop to which we are mapping the new induction
  // variable.
  Type *Ty = WidenIVR->getScalarType();

  const InductionDescriptor &ID = WidenIVR->getInductionDescriptor();
  Instruction::BinaryOps AddOp;
  Instruction::BinaryOps MulOp;
  VPIRFlags Flags = *WidenIVR;
  if (ID.getKind() == InductionDescriptor::IK_IntInduction) {
    AddOp = Instruction::Add;
    MulOp = Instruction::Mul;
  } else {
    AddOp = ID.getInductionOpcode();
    MulOp = Instruction::FMul;
  }

  // If the phi is truncated, truncate the start and step values.
  VPBuilder Builder(Plan->getVectorPreheader());
  Type *StepTy = Step->getScalarType();
  if (Ty->getScalarSizeInBits() < StepTy->getScalarSizeInBits()) {
    assert(StepTy->isIntegerTy() && "Truncation requires an integer type");
    Step = Builder.createScalarCast(Instruction::Trunc, Step, Ty, DL);
    Start = Builder.createScalarCast(Instruction::Trunc, Start, Ty, DL);
    StepTy = Ty;
  }

  // Construct the initial value of the vector IV in the vector loop preheader.
  Type *IVIntTy =
      IntegerType::get(Plan->getContext(), StepTy->getScalarSizeInBits());
  VPValue *Init = Builder.createNaryOp(VPInstruction::StepVector, {}, IVIntTy);
  if (StepTy->isFloatingPointTy())
    Init = Builder.createWidenCast(Instruction::UIToFP, Init, StepTy);

  VPValue *SplatStart = Builder.createNaryOp(VPInstruction::Broadcast, Start);
  VPValue *SplatStep = Builder.createNaryOp(VPInstruction::Broadcast, Step);

  Init = Builder.createNaryOp(MulOp, {Init, SplatStep}, Flags);
  Init = Builder.createNaryOp(AddOp, {SplatStart, Init}, Flags,
                              DebugLoc::getUnknown(), "induction");

  // Create the widened phi of the vector IV.
  auto *WidePHI = VPBuilder(WidenIVR).createWidenPhi(
      Init, WidenIVR->getDebugLoc(), "vec.ind");

  // Create the backedge value for the vector IV.
  VPValue *Inc;
  VPValue *Prev;
  // If unrolled, use the increment and prev value from the operands.
  if (auto *SplatVF = WidenIVR->getSplatVFValue()) {
    Inc = SplatVF;
    Prev = WidenIVR->getLastUnrolledPartOperand();
  } else {
    // Move the insertion point after the VF definition when the VF is defined
    // inside a loop, such as for EVL tail-folding.
    if (VPRecipeBase *R = VF->getDefiningRecipe())
      if (R->getParent()->getEnclosingLoopRegion())
        Builder.setInsertPoint(R->getParent(), std::next(R->getIterator()));

    // Multiply the vectorization factor by the step using integer or
    // floating-point arithmetic as appropriate.
    if (StepTy->isFloatingPointTy())
      VF = Builder.createScalarCast(Instruction::CastOps::UIToFP, VF, StepTy,
                                    DL);
    else
      VF = Builder.createScalarZExtOrTrunc(VF, StepTy, DL);

    Inc = Builder.createNaryOp(MulOp, {Step, VF}, Flags);
    Inc = Builder.createNaryOp(VPInstruction::Broadcast, Inc);
    Prev = WidePHI;
  }

  VPBasicBlock *ExitingBB = Plan->getVectorLoopRegion()->getExitingBasicBlock();
  Builder.setInsertPoint(ExitingBB, ExitingBB->getTerminator()->getIterator());
  auto *Next = Builder.createNaryOp(AddOp, {Prev, Inc}, Flags,
                                    WidenIVR->getDebugLoc(), "vec.ind.next");

  WidePHI->addIncoming(Next);

  WidenIVR->replaceAllUsesWith(WidePHI);
}

/// Expand a VPWidenPointerInductionRecipe into executable recipes, for the
/// initial value, phi and backedge value. In the following example:
///
///  <x1> vector loop: {
///    vector.body:
///      EMIT ir<%ptr.iv> = WIDEN-POINTER-INDUCTION %start, %step, %vf
///      ...
///      EMIT branch-on-count ...
///  }
///
/// WIDEN-POINTER-INDUCTION will get expanded to:
///
///  <x1> vector loop: {
///    vector.body:
///      EMIT-SCALAR %pointer.phi = phi %start, %ptr.ind
///      EMIT %mul = mul %stepvector, %step
///      EMIT %vector.gep = wide-ptradd %pointer.phi, %mul
///      ...
///      EMIT %ptr.ind = ptradd %pointer.phi, %vf
///      EMIT branch-on-count ...
///  }
static void expandVPWidenPointerInduction(VPWidenPointerInductionRecipe *R) {
  VPlan *Plan = R->getParent()->getPlan();
  VPValue *Start = R->getStartValue();
  VPValue *Step = R->getStepValue();
  VPValue *VF = R->getVFValue();

  assert(R->getInductionDescriptor().getKind() ==
             InductionDescriptor::IK_PtrInduction &&
         "Not a pointer induction according to InductionDescriptor!");
  assert(R->getScalarType()->isPointerTy() && "Unexpected type.");
  assert(!R->onlyScalarsGenerated(Plan->hasScalableVF()) &&
         "Recipe should have been replaced");

  VPBuilder Builder(R);
  DebugLoc DL = R->getDebugLoc();

  // Build a scalar pointer phi.
  VPPhi *ScalarPtrPhi = Builder.createScalarPhi(Start, DL, "pointer.phi");

  // Create actual address geps that use the pointer phi as base and a
  // vectorized version of the step value (<step*0, ..., step*N>) as offset.
  Builder.setInsertPoint(R->getParent(), R->getParent()->getFirstNonPhi());
  Type *StepTy = Step->getScalarType();
  VPValue *Offset = Builder.createNaryOp(VPInstruction::StepVector, {}, StepTy);
  Offset = Builder.createOverflowingOp(Instruction::Mul, {Offset, Step});
  VPValue *PtrAdd =
      Builder.createWidePtrAdd(ScalarPtrPhi, Offset, DL, "vector.gep");
  R->replaceAllUsesWith(PtrAdd);

  // Create the backedge value for the scalar pointer phi.
  VPBasicBlock *ExitingBB = Plan->getVectorLoopRegion()->getExitingBasicBlock();
  Builder.setInsertPoint(ExitingBB, ExitingBB->getTerminator()->getIterator());
  VF = Builder.createScalarZExtOrTrunc(VF, StepTy, DL);
  VPValue *Inc = Builder.createOverflowingOp(Instruction::Mul, {Step, VF});

  VPValue *InductionGEP =
      Builder.createPtrAdd(ScalarPtrPhi, Inc, DL, "ptr.ind");
  ScalarPtrPhi->addIncoming(InductionGEP);
}

/// Expand a VPDerivedIVRecipe into executable recipes.
static void expandVPDerivedIV(VPDerivedIVRecipe *R) {
  VPBuilder Builder(R);
  VPValue *Start = R->getStartValue();
  VPValue *Step = R->getStepValue();
  VPValue *Index = R->getIndex();
  Type *StepTy = Step->getScalarType();
  Index = StepTy->isIntegerTy()
              ? Builder.createScalarZExtOrTrunc(
                    Index, StepTy, DebugLoc::getCompilerGenerated())
              : Builder.createScalarCast(Instruction::SIToFP, Index, StepTy,
                                         DebugLoc::getCompilerGenerated());
  VPIRFlags::WrapFlagsTy Flags = R->getNoWrapFlags();
  switch (R->getInductionKind()) {
  case InductionDescriptor::IK_IntInduction: {
    assert(Index->getScalarType() == Start->getScalarType() &&
           "Index type does not match StartValue type");
    return R->replaceAllUsesWith(Builder.createAdd(
        Start,
        Builder.createOverflowingOp(Instruction::Mul, {Index, Step}, Flags),
        DebugLoc::getUnknown(), "", Flags));
  }
  case InductionDescriptor::IK_PtrInduction:
    return R->replaceAllUsesWith(Builder.createPtrAdd(
        Start,
        Builder.createOverflowingOp(Instruction::Mul, {Index, Step}, Flags)));
  case InductionDescriptor::IK_FpInduction: {
    assert(StepTy->isFloatingPointTy() && "Expected FP Step value");
    const FPMathOperator *FPBinOp = R->getFPBinOp();
    assert(FPBinOp &&
           (FPBinOp->getOpcode() == Instruction::FAdd ||
            FPBinOp->getOpcode() == Instruction::FSub) &&
           "Original BinOp should be defined for FP induction");
    FastMathFlags FMF = FPBinOp->getFastMathFlags();
    VPValue *FMul = Builder.createNaryOp(Instruction::FMul, {Step, Index}, FMF);
    return R->replaceAllUsesWith(
        Builder.createNaryOp(FPBinOp->getOpcode(), {Start, FMul}, FMF));
  }
  case InductionDescriptor::IK_NoInduction:
    return;
  }
  llvm_unreachable("Unhandled induction kind");
}

void VPlanTransforms::dissolveLoopRegions(VPlan &Plan) {
  // Replace loop regions with explicity CFG.
  SmallVector<VPRegionBlock *> LoopRegions;
  for (VPRegionBlock *R : VPBlockUtils::blocksOnly<VPRegionBlock>(
           vp_depth_first_deep(Plan.getEntry()))) {
    if (!R->isReplicator())
      LoopRegions.push_back(R);
  }
  for (VPRegionBlock *R : LoopRegions)
    R->dissolveToCFGLoop();
}

void VPlanTransforms::expandBranchOnTwoConds(VPlan &Plan) {
  SmallVector<VPInstruction *> WorkList;
  // The transform runs after dissolving loop regions, so all VPBasicBlocks
  // terminated with BranchOnTwoConds are reached via a shallow traversal.
  for (VPBasicBlock *VPBB : VPBlockUtils::blocksAs<VPBasicBlock>(
           vp_depth_first_shallow(Plan.getEntry()))) {
    if (!VPBB->empty() && match(&VPBB->back(), m_BranchOnTwoConds()))
      WorkList.push_back(cast<VPInstruction>(&VPBB->back()));
  }

  // Expand BranchOnTwoConds instructions into explicit CFG with two new
  // single-condition branches:
  // 1. A branch that replaces BranchOnTwoConds, jumps to the first successor if
  //    the first condition is true, and otherwise jumps to a new interim block.
  // 2. A branch that ends the interim block, jumps to the second successor if
  //    the second condition is true, and otherwise jumps to the third
  //    successor.
  for (VPInstruction *Br : WorkList) {
    assert(Br->getNumOperands() == 2 &&
           "BranchOnTwoConds must have exactly 2 conditions");
    DebugLoc DL = Br->getDebugLoc();
    VPBasicBlock *BrOnTwoCondsBB = Br->getParent();
    const auto Successors = to_vector(BrOnTwoCondsBB->getSuccessors());
    assert(Successors.size() == 3 &&
           "BranchOnTwoConds must have exactly 3 successors");

    for (VPBlockBase *Succ : Successors)
      VPBlockUtils::disconnectBlocks(BrOnTwoCondsBB, Succ);

    VPValue *Cond0 = Br->getOperand(0);
    VPValue *Cond1 = Br->getOperand(1);
    VPBlockBase *Succ0 = Successors[0];
    VPBlockBase *Succ1 = Successors[1];
    VPBlockBase *Succ2 = Successors[2];

    // If the successor block for both conditions is the same, then combine the
    // two conditions and plant a single conditional branch.
    if (Succ0 == Succ1) {
      VPBuilder Builder(Br);
      VPValue *Combined = Builder.createOr(Cond0, Cond1, DL);
      Builder.createNaryOp(VPInstruction::BranchOnCond, {Combined}, DL);
      VPBlockUtils::connectBlocks(BrOnTwoCondsBB, Succ0);
      VPBlockUtils::connectBlocks(BrOnTwoCondsBB, Succ2);
      Br->eraseFromParent();
      continue;
    }

    assert(!Succ0->getParent() && !Succ1->getParent() && !Succ2->getParent() &&
           !BrOnTwoCondsBB->getParent() && "regions must already be dissolved");

    VPBasicBlock *InterimBB =
        Plan.createVPBasicBlock(BrOnTwoCondsBB->getName() + ".interim");

    VPBuilder(BrOnTwoCondsBB)
        .createNaryOp(VPInstruction::BranchOnCond, {Cond0}, DL);
    VPBlockUtils::connectBlocks(BrOnTwoCondsBB, Succ0);
    VPBlockUtils::connectBlocks(BrOnTwoCondsBB, InterimBB);

    VPBuilder(InterimBB).createNaryOp(VPInstruction::BranchOnCond, {Cond1}, DL);
    VPBlockUtils::connectBlocks(InterimBB, Succ1);
    VPBlockUtils::connectBlocks(InterimBB, Succ2);
    Br->eraseFromParent();
  }
}

void VPlanTransforms::convertToConcreteRecipes(VPlan &Plan) {
  for (VPBasicBlock *VPBB : VPBlockUtils::blocksOnly<VPBasicBlock>(
           vp_depth_first_deep(Plan.getEntry()))) {
    for (VPRecipeBase &R : make_early_inc_range(*VPBB)) {
      VPBuilder Builder(&R);
      if (auto *WidenIVR = dyn_cast<VPWidenIntOrFpInductionRecipe>(&R)) {
        expandVPWidenIntOrFpInduction(WidenIVR);
        WidenIVR->eraseFromParent();
        continue;
      }

      if (auto *WidenIVR = dyn_cast<VPWidenPointerInductionRecipe>(&R)) {
        // If the recipe only generates scalars, scalarize it instead of
        // expanding it.
        if (WidenIVR->onlyScalarsGenerated(Plan.hasScalableVF())) {
          VPValue *PtrAdd = vputils::scalarizeVPWidenPointerInduction(
              WidenIVR, Plan, Builder);
          WidenIVR->replaceAllUsesWith(PtrAdd);
          WidenIVR->eraseFromParent();
          continue;
        }
        expandVPWidenPointerInduction(WidenIVR);
        WidenIVR->eraseFromParent();
        continue;
      }

      if (auto *DerivedIVR = dyn_cast<VPDerivedIVRecipe>(&R)) {
        expandVPDerivedIV(DerivedIVR);
        DerivedIVR->eraseFromParent();
        continue;
      }

      if (auto *WideCanIV = dyn_cast<VPWidenCanonicalIVRecipe>(&R)) {
        VPValue *CanIV = WideCanIV->getCanonicalIV();
        Type *CanIVTy = CanIV->getScalarType();
        VPValue *Step = WideCanIV->getStepValue();
        if (!Step) {
          assert(Plan.getConcreteUF() == 1 &&
                 "Expected unroller to have materialized step for UF != 1");
          Step = Plan.getZero(CanIVTy);
        }
        CanIV = Builder.createNaryOp(VPInstruction::Broadcast, CanIV);
        Step = Builder.createNaryOp(VPInstruction::Broadcast, Step);
        Step = Builder.createAdd(
            Step, Builder.createNaryOp(VPInstruction::StepVector, {}, CanIVTy));
        VPValue *CanVecIV =
            Builder.createAdd(CanIV, Step, WideCanIV->getDebugLoc(), "vec.iv",
                              WideCanIV->getNoWrapFlags());
        WideCanIV->replaceAllUsesWith(CanVecIV);
        WideCanIV->eraseFromParent();
        continue;
      }

      // Expand VPBlendRecipe into VPInstruction::Select.
      if (auto *Blend = dyn_cast<VPBlendRecipe>(&R)) {
        VPValue *Select = Blend->getIncomingValue(0);
        for (unsigned I = 1; I != Blend->getNumIncomingValues(); ++I)
          Select = Builder.createSelect(Blend->getMask(I),
                                        Blend->getIncomingValue(I), Select,
                                        R.getDebugLoc(), "predphi", *Blend);
        Blend->replaceAllUsesWith(Select);
        Blend->eraseFromParent();
        continue;
      }

      if (auto *VEPR = dyn_cast<VPVectorEndPointerRecipe>(&R)) {
        if (!VEPR->getOffset()) {
          assert(Plan.getConcreteUF() == 1 &&
                 "Expected unroller to have materialized offset for UF != 1");
          VEPR->materializeOffset();
        }
        continue;
      }

      if (auto *Expr = dyn_cast<VPExpressionRecipe>(&R)) {
        Expr->decompose();
        Expr->eraseFromParent();
        continue;
      }

      // Expand LastActiveLane into Not + FirstActiveLane + Sub.
      auto *LastActiveL = dyn_cast<VPInstruction>(&R);
      if (LastActiveL &&
          LastActiveL->getOpcode() == VPInstruction::LastActiveLane) {
        // Create Not(Mask) for all operands.
        SmallVector<VPValue *, 2> NotMasks;
        for (VPValue *Op : LastActiveL->operands()) {
          VPValue *NotMask = Builder.createNot(Op, LastActiveL->getDebugLoc());
          NotMasks.push_back(NotMask);
        }

        // Create FirstActiveLane on the inverted masks.
        VPValue *FirstInactiveLane = Builder.createFirstActiveLane(
            NotMasks, LastActiveL->getDebugLoc(), "first.inactive.lane");

        // Subtract 1 to get the last active lane.
        VPValue *One =
            Plan.getConstantInt(FirstInactiveLane->getScalarType(), 1);
        VPValue *LastLane =
            Builder.createSub(FirstInactiveLane, One,
                              LastActiveL->getDebugLoc(), "last.active.lane");

        LastActiveL->replaceAllUsesWith(LastLane);
        LastActiveL->eraseFromParent();
        continue;
      }

      // Lower MaskedCond with block mask to LogicalAnd.
      if (match(&R, m_VPInstruction<VPInstruction::MaskedCond>())) {
        auto *VPI = cast<VPInstruction>(&R);
        assert(VPI->isMasked() &&
               "Unmasked MaskedCond should be simplified earlier");
        VPI->replaceAllUsesWith(Builder.createNaryOp(
            VPInstruction::LogicalAnd, {VPI->getMask(), VPI->getOperand(0)}));
        VPI->eraseFromParent();
        continue;
      }

      // Lower CanonicalIVIncrementForPart to plain Add.
      if (match(
              &R,
              m_VPInstruction<VPInstruction::CanonicalIVIncrementForPart>())) {
        auto *VPI = cast<VPInstruction>(&R);
        VPValue *Add = Builder.createOverflowingOp(
            Instruction::Add, VPI->operands(), VPI->getNoWrapFlags(),
            VPI->getDebugLoc());
        VPI->replaceAllUsesWith(Add);
        VPI->eraseFromParent();
        continue;
      }

      // Lower BranchOnCount to ICmp + BranchOnCond.
      VPValue *IV, *TC;
      if (match(&R, m_BranchOnCount(m_VPValue(IV), m_VPValue(TC)))) {
        auto *BranchOnCountInst = cast<VPInstruction>(&R);
        DebugLoc DL = BranchOnCountInst->getDebugLoc();
        VPValue *Cond = Builder.createICmp(CmpInst::ICMP_EQ, IV, TC, DL);
        Builder.createNaryOp(VPInstruction::BranchOnCond, Cond, DL);
        BranchOnCountInst->eraseFromParent();
        continue;
      }

      VPValue *VectorStep;
      VPValue *ScalarStep;
      if (!match(&R, m_VPInstruction<VPInstruction::WideIVStep>(
                         m_VPValue(VectorStep), m_VPValue(ScalarStep))))
        continue;

      // Expand WideIVStep.
      auto *VPI = cast<VPInstruction>(&R);
      Type *IVTy = VPI->getScalarType();
      if (VectorStep->getScalarType() != IVTy) {
        Instruction::CastOps CastOp = IVTy->isFloatingPointTy()
                                          ? Instruction::UIToFP
                                          : Instruction::Trunc;
        VectorStep = Builder.createWidenCast(CastOp, VectorStep, IVTy);
      }

      assert(!match(ScalarStep, m_One()) && "Expected non-unit scalar-step");
      if (ScalarStep->getScalarType() != IVTy) {
        ScalarStep =
            Builder.createWidenCast(Instruction::Trunc, ScalarStep, IVTy);
      }

      VPIRFlags Flags;
      unsigned MulOpc;
      if (IVTy->isFloatingPointTy()) {
        MulOpc = Instruction::FMul;
        Flags = VPI->getFastMathFlagsOrNone();
      } else {
        MulOpc = Instruction::Mul;
        Flags = VPIRFlags::getDefaultFlags(MulOpc);
      }

      VPInstruction *Mul = Builder.createNaryOp(
          MulOpc, {VectorStep, ScalarStep}, Flags, R.getDebugLoc());
      VectorStep = Mul;
      VPI->replaceAllUsesWith(VectorStep);
      VPI->eraseFromParent();
    }
  }
}

void VPlanTransforms::materializeBroadcasts(VPlan &Plan) {
  if (Plan.hasScalarVFOnly())
    return;

#ifndef NDEBUG
  VPDominatorTree VPDT(Plan);
#endif

  SmallVector<VPValue *> VPValues;
  if (VPValue *BTC = Plan.getBackedgeTakenCount())
    VPValues.push_back(BTC);
  append_range(VPValues, Plan.getLiveIns());
  for (VPRecipeBase &R : *Plan.getEntry())
    append_range(VPValues, R.definedValues());

  auto *VectorPreheader = Plan.getVectorPreheader();
  for (VPValue *VPV : VPValues) {
    if (vputils::onlyScalarValuesUsed(VPV) || isa<VPConstant>(VPV))
      continue;

    // Add explicit broadcast at the insert point that dominates all users.
    VPBasicBlock *HoistBlock = VectorPreheader;
    VPBasicBlock::iterator HoistPoint = VectorPreheader->end();
    for (VPUser *User : VPV->users()) {
      if (User->usesScalars(VPV))
        continue;
      if (cast<VPRecipeBase>(User)->getParent() == VectorPreheader)
        HoistPoint = HoistBlock->begin();
      else
        assert(VPDT.dominates(VectorPreheader,
                              cast<VPRecipeBase>(User)->getParent()) &&
               "All users must be in the vector preheader or dominated by it");
    }

    VPBuilder Builder(cast<VPBasicBlock>(HoistBlock), HoistPoint);
    auto *Broadcast = Builder.createNaryOp(VPInstruction::Broadcast, {VPV});
    VPV->replaceUsesWithIf(Broadcast,
                           [VPV, Broadcast](VPUser &U, unsigned Idx) {
                             return Broadcast != &U && !U.usesScalars(VPV);
                           });
  }
}

void VPlanTransforms::materializeConstantVectorTripCount(
    VPlan &Plan, ElementCount BestVF, unsigned BestUF,
    PredicatedScalarEvolution &PSE) {
  assert(Plan.hasVF(BestVF) && "BestVF is not available in Plan");
  assert(Plan.hasUF(BestUF) && "BestUF is not available in Plan");

  VPValue *TC = Plan.getTripCount();
  if (TC->user_empty())
    return;

  // Skip cases for which the trip count may be non-trivial to materialize.
  // I.e., when a scalar tail is absent - due to tail folding, or when a scalar
  // tail is required.
  if (Plan.hasTailFolded() || !Plan.hasScalarTail() ||
      Plan.getMiddleBlock()->getSingleSuccessor() ==
          Plan.getScalarPreheader() ||
      !isa<VPIRValue>(TC))
    return;

  // Materialize vector trip counts for constants early if it can simply
  // be computed as (Original TC / VF * UF) * VF * UF.
  // TODO: Compute vector trip counts for loops requiring a scalar epilogue and
  // tail-folded loops.
  ScalarEvolution &SE = *PSE.getSE();
  auto *TCScev = SE.getSCEV(TC->getLiveInIRValue());
  if (!isa<SCEVConstant>(TCScev))
    return;
  const SCEV *VFxUF = SE.getElementCount(TCScev->getType(), BestVF * BestUF);
  auto VecTCScev = SE.getMulExpr(SE.getUDivExpr(TCScev, VFxUF), VFxUF);
  if (auto *ConstVecTC = dyn_cast<SCEVConstant>(VecTCScev))
    Plan.getVectorTripCount().setUnderlyingValue(ConstVecTC->getValue());
}

void VPlanTransforms::materializeBackedgeTakenCount(VPlan &Plan,
                                                    VPBasicBlock *VectorPH) {
  VPValue *BTC = Plan.getOrCreateBackedgeTakenCount();
  if (BTC->user_empty())
    return;

  VPBuilder Builder(VectorPH, VectorPH->begin());
  auto *TCTy = Plan.getTripCount()->getScalarType();
  auto *TCMO =
      Builder.createSub(Plan.getTripCount(), Plan.getConstantInt(TCTy, 1),
                        DebugLoc::getCompilerGenerated(), "trip.count.minus.1");
  BTC->replaceAllUsesWith(TCMO);
}

void VPlanTransforms::materializePacksAndUnpacks(VPlan &Plan) {
  if (Plan.hasScalarVFOnly())
    return;

  VPRegionBlock *LoopRegion = Plan.getVectorLoopRegion();
  auto VPBBsOutsideLoopRegion = VPBlockUtils::blocksOnly<VPBasicBlock>(
      vp_depth_first_shallow(Plan.getEntry()));
  auto VPBBsInsideLoopRegion = VPBlockUtils::blocksOnly<VPBasicBlock>(
      vp_depth_first_shallow(LoopRegion->getEntry()));
  // Materialize Build(Struct)Vector for all replicating VPReplicateRecipes,
  // VPScalarIVStepsRecipe and VPInstructions, excluding ones in replicate
  // regions. Those are not materialized explicitly yet.
  // TODO: materialize build vectors for replicating recipes in replicating
  // regions.
  for (VPBasicBlock *VPBB :
       concat<VPBasicBlock *>(VPBBsOutsideLoopRegion, VPBBsInsideLoopRegion)) {
    for (VPRecipeBase &R : make_early_inc_range(*VPBB)) {
      if (!vputils::doesGeneratePerAllLanes(&R))
        continue;
      auto *DefR = cast<VPSingleDefRecipe>(&R);
      auto UsesVectorOrInsideReplicateRegion = [DefR, LoopRegion](VPUser *U) {
        VPRegionBlock *ParentRegion = cast<VPRecipeBase>(U)->getRegion();
        return !U->usesScalars(DefR) || ParentRegion != LoopRegion;
      };
      if (none_of(DefR->users(), UsesVectorOrInsideReplicateRegion))
        continue;

      Type *ScalarTy = DefR->getScalarType();
      unsigned Opcode = ScalarTy->isStructTy()
                            ? VPInstruction::BuildStructVector
                            : VPInstruction::BuildVector;
      auto *BuildVector = new VPInstruction(Opcode, {DefR});
      BuildVector->insertAfter(DefR);

      DefR->replaceUsesWithIf(
          BuildVector, [BuildVector, &UsesVectorOrInsideReplicateRegion](
                           VPUser &U, unsigned) {
            return &U != BuildVector && UsesVectorOrInsideReplicateRegion(&U);
          });
    }
  }

  // Create explicit VPInstructions to convert vectors to scalars. The current
  // implementation is conservative - it may miss some cases that may or may not
  // be vector values. TODO: introduce Unpacks speculatively - remove them later
  // if they are known to operate on scalar values.
  for (VPBasicBlock *VPBB : VPBBsInsideLoopRegion) {
    for (VPRecipeBase &R : make_early_inc_range(*VPBB)) {
      if (isa<VPReplicateRecipe, VPInstruction, VPScalarIVStepsRecipe,
              VPDerivedIVRecipe>(&R))
        continue;
      for (VPValue *Def : R.definedValues()) {
        // Skip recipes that are single-scalar.
        // TODO: The Defs skipped here may or may not be vector values.
        // Introduce Unpacks, and remove them later, if they are guaranteed to
        // produce scalar values.
        if (vputils::isSingleScalar(Def))
          continue;

        // Only introduce an Unpack if some, but not all, users use the first
        // lane only.
        unsigned NumFirstLaneUsers = count_if(Def->users(), [&Def](VPUser *U) {
          return U->usesFirstLaneOnly(Def);
        });
        if (!NumFirstLaneUsers || NumFirstLaneUsers == Def->getNumUsers())
          continue;

        auto *Unpack = new VPInstruction(VPInstruction::Unpack, {Def});
        if (R.isPhi())
          Unpack->insertBefore(*VPBB, VPBB->getFirstNonPhi());
        else
          Unpack->insertAfter(&R);
        Def->replaceUsesWithIf(Unpack, [&Def](VPUser &U, unsigned) {
          return U.usesFirstLaneOnly(Def);
        });
      }
    }
  }
}

void VPlanTransforms::materializeVectorTripCount(
    VPlan &Plan, VPBasicBlock *VectorPHVPBB, bool TailByMasking,
    bool RequiresScalarEpilogue, VPValue *Step,
    std::optional<uint64_t> MaxRuntimeStep) {
  VPSymbolicValue &VectorTC = Plan.getVectorTripCount();
  // There's nothing to do if there are no users of the vector trip count or its
  // IR value has already been set.
  if (VectorTC.user_empty() || VectorTC.getUnderlyingValue())
    return;

  VPValue *TC = Plan.getTripCount();
  Type *TCTy = TC->getScalarType();
  VPBasicBlock::iterator InsertPt = VectorPHVPBB->begin();
  if (auto *StepR = Step->getDefiningRecipe()) {
    assert(VPDominatorTree(Plan).dominates(StepR->getParent(), VectorPHVPBB) &&
           "Step VPBB must dominate VectorPHVPBB");
    // Insert after Step's definition to maintain valid def-use ordering.
    InsertPt = std::next(StepR->getIterator());
  }
  VPBuilder Builder(VectorPHVPBB, InsertPt);

  // For scalable steps, if TC is a constant and is divisible by the maximum
  // possible runtime step, then TC % Step == 0 for all valid vscale values
  // and the vector trip count equals TC directly.
  const APInt *TCVal;
  if (!RequiresScalarEpilogue && match(TC, m_APInt(TCVal)) && MaxRuntimeStep &&
      TCVal->urem(*MaxRuntimeStep) == 0) {
    VectorTC.replaceAllUsesWith(TC);
    return;
  }

  // If the tail is to be folded by masking, round the number of iterations N
  // up to a multiple of Step instead of rounding down. This is done by first
  // adding Step-1 and then rounding down. Note that it's ok if this addition
  // overflows: the vector induction variable will eventually wrap to zero given
  // that it starts at zero and its Step is a power of two; the loop will then
  // exit, with the last early-exit vector comparison also producing all-true.
  if (TailByMasking) {
    TC = Builder.createAdd(
        TC, Builder.createSub(Step, Plan.getConstantInt(TCTy, 1)),
        DebugLoc::getCompilerGenerated(), "n.rnd.up");
  }

  // Now we need to generate the expression for the part of the loop that the
  // vectorized body will execute. This is equal to N - (N % Step) if scalar
  // iterations are not required for correctness, or N - Step, otherwise. Step
  // is equal to the vectorization factor (number of SIMD elements) times the
  // unroll factor (number of SIMD instructions).
  VPValue *R =
      Builder.createNaryOp(Instruction::URem, {TC, Step},
                           DebugLoc::getCompilerGenerated(), "n.mod.vf");

  // There are cases where we *must* run at least one iteration in the remainder
  // loop.  See the cost model for when this can happen.  If the step evenly
  // divides the trip count, we set the remainder to be equal to the step. If
  // the step does not evenly divide the trip count, no adjustment is necessary
  // since there will already be scalar iterations. Note that the minimum
  // iterations check ensures that N >= Step.
  if (RequiresScalarEpilogue) {
    assert(!TailByMasking &&
           "requiring scalar epilogue is not supported with fail folding");
    VPValue *IsZero =
        Builder.createICmp(CmpInst::ICMP_EQ, R, Plan.getZero(TCTy));
    R = Builder.createSelect(IsZero, Step, R);
  }

  VPValue *Res =
      Builder.createSub(TC, R, DebugLoc::getCompilerGenerated(), "n.vec");
  VectorTC.replaceAllUsesWith(Res);
}

void VPlanTransforms::materializeFactors(VPlan &Plan, VPBasicBlock *VectorPH,
                                         ElementCount VFEC) {
  // If VF and VFxUF have already been materialized (no remaining users),
  // there's nothing more to do.
  if (Plan.getVF().isMaterialized()) {
    assert(Plan.getVFxUF().isMaterialized() &&
           "VF and VFxUF must be materialized together");
    return;
  }

  VPBuilder Builder(VectorPH, VectorPH->begin());
  Type *TCTy = Plan.getTripCount()->getScalarType();
  VPValue &VF = Plan.getVF();
  VPValue &VFxUF = Plan.getVFxUF();
  // If there are no users of the runtime VF, compute VFxUF by constant folding
  // the multiplication of VF and UF.
  if (VF.user_empty()) {
    VPValue *RuntimeVFxUF =
        Builder.createElementCount(TCTy, VFEC * Plan.getConcreteUF());
    VFxUF.replaceAllUsesWith(RuntimeVFxUF);
    return;
  }

  // For users of the runtime VF, compute it as VF * vscale, and VFxUF as (VF *
  // vscale) * UF.
  VPValue *RuntimeVF = Builder.createElementCount(TCTy, VFEC);
  if (!vputils::onlyScalarValuesUsed(&VF)) {
    VPValue *BC = Builder.createNaryOp(VPInstruction::Broadcast, RuntimeVF);
    VF.replaceUsesWithIf(
        BC, [&VF](VPUser &U, unsigned) { return !U.usesScalars(&VF); });
  }
  VF.replaceAllUsesWith(RuntimeVF);

  VPValue *MulByUF = Builder.createOverflowingOp(
      Instruction::Mul,
      {RuntimeVF, Plan.getConstantInt(TCTy, Plan.getConcreteUF())},
      {true, false});
  VFxUF.replaceAllUsesWith(MulByUF);
}

VPValue *
VPlanTransforms::materializeAliasMask(VPlan &Plan, VPBasicBlock *AliasCheckVPBB,
                                      ArrayRef<PointerDiffInfo> DiffChecks) {
  VPBuilder Builder(AliasCheckVPBB);
  Type *I1Ty = IntegerType::getInt1Ty(Plan.getContext());

  VPValue *IncomingAliasMask = vputils::findIncomingAliasMask(Plan);
  assert(IncomingAliasMask && "Expected an alias mask!");

  VPValue *AliasMask = nullptr;
  for (const PointerDiffInfo &Check : DiffChecks) {
    VPValue *Src = vputils::getOrCreateVPValueForSCEVExpr(Plan, Check.SrcStart);
    VPValue *Sink =
        vputils::getOrCreateVPValueForSCEVExpr(Plan, Check.SinkStart);
    Type *AddrType = Src->getScalarType();

    // TODO: Only freeze the required pointer (not both src and sink).
    if (Check.NeedsFreeze) {
      Src = Builder.createScalarFreeze(Src, AddrType, DebugLoc::getUnknown());
      Sink = Builder.createScalarFreeze(Sink, AddrType, DebugLoc::getUnknown());
    }

    // TODO: Generate loop_dependence_raw_mask when there's a read-after-write
    // dependency between the source and the sink. This is not necessary for
    // correctness of the mask, but using the "raw" variant prevents loads
    // depending on the completion of stores.
    VPWidenIntrinsicRecipe *WARMask = Builder.insert(new VPWidenIntrinsicRecipe(
        Intrinsic::loop_dependence_war_mask,
        {Src, Sink, Plan.getConstantInt(AddrType, Check.AccessSize)}, I1Ty));

    if (AliasMask)
      AliasMask = Builder.createAnd(AliasMask, WARMask);
    else
      AliasMask = WARMask;
  }

  Type *IVTy = Plan.getVectorLoopRegion()->getCanonicalIVType();
  Type *IndexTy = Plan.getDataLayout().getIndexType(Plan.getContext(), 0);
  VPValue *NumActive = Builder.createNaryOp(
      VPInstruction::NumActiveLanes, {AliasMask}, nullptr, {}, {},
      DebugLoc::getUnknown(), "num.active.lanes", IndexTy);
  VPValue *ClampedVF = Builder.createScalarZExtOrTrunc(
      NumActive, IVTy, DebugLoc::getCompilerGenerated());

  IncomingAliasMask->replaceAllUsesWith(AliasMask);

  return ClampedVF;
}

void VPlanTransforms::materializeAliasMaskCheckBlock(
    VPlan &Plan, ArrayRef<PointerDiffInfo> DiffChecks, bool HasBranchWeights) {
  VPBasicBlock *ClampedVFCheck =
      Plan.createVPBasicBlock("vector.clamped.vf.check");

  VPValue *ClampedVF = materializeAliasMask(Plan, ClampedVFCheck, DiffChecks);
  VPBuilder Builder(ClampedVFCheck);
  DebugLoc DL = DebugLoc::getCompilerGenerated();
  Type *TCTy = Plan.getTripCount()->getScalarType();

  // Check the "ClampedVF" from the alias mask is larger than one.
  VPValue *IsScalar =
      Builder.createICmp(CmpInst::ICMP_ULE, ClampedVF,
                         Plan.getConstantInt(TCTy, 1), DL, "vf.is.scalar");

  VPValue *TripCount = Plan.getTripCount();
  VPValue *MaxUIntTripCount =
      Plan.getConstantInt(cast<IntegerType>(TCTy)->getMask());
  VPValue *DistanceToMax = Builder.createSub(MaxUIntTripCount, TripCount);

  // For tail-folding: Don't execute the vector loop if (UMax - n) < ClampedVF.
  // Note: The ClampedVF may not be a power-of-two. This means the loop exit
  // condition (index.next == n.vec) may not be correct in the case of an
  // overflow. The issue is `n.vec` could be zero due to an overflow, but
  // index.next is not guaranteed to overflow to zero as the ClampedVF is not a
  // power-of-two).
  VPValue *TripCountCheck = Builder.createICmp(
      ICmpInst::ICMP_ULT, DistanceToMax, ClampedVF, DL, "vf.step.overflow");

  VPValue *Cond = Builder.createOr(IsScalar, TripCountCheck, DL);
  attachVPCheckBlock(Plan, Cond, ClampedVFCheck, HasBranchWeights);

  // Materialize the trip count early as this will add a use of (VFxUF) that
  // needs to be replaced with the ClampedVF.
  materializeVectorTripCount(Plan, Plan.getVectorPreheader(),
                             /*TailByMasking=*/true,
                             /*RequiresScalarEpilogue=*/false,
                             &Plan.getVFxUF());

  assert(Plan.getConcreteUF() == 1 &&
         "Clamped VF not supported with interleaving");
  Plan.getVF().replaceAllUsesWith(ClampedVF);
  Plan.getVFxUF().replaceAllUsesWith(ClampedVF);
}

void VPlanTransforms::expandSCEVsToVPInstructions(VPlan &Plan,
                                                  ScalarEvolution &SE) {
  auto *Entry = Plan.getEntry();
  VPBuilder Builder(Entry, Entry->begin());
  DebugLoc DL = cast<VPIRBasicBlock>(Entry)
                    ->getIRBasicBlock()
                    ->getTerminator()
                    ->getDebugLoc();
  VPSCEVExpander Expander(Builder, SE, DL);

  // Expand VPExpandSCEVRecipes to VPInstructions using VPSCEVExpander. During
  // the transition, unsupported VPExpandSCEVRecipes are skipped and left for
  // late expansion.
  for (VPRecipeBase &R : make_early_inc_range(*Entry)) {
    auto *ExpSCEV = dyn_cast<VPExpandSCEVRecipe>(&R);
    if (!ExpSCEV || ExpSCEV->user_empty())
      continue;
    Builder.setInsertPoint(ExpSCEV);
    VPValue *Expanded = Expander.tryToExpand(ExpSCEV->getSCEV());
    if (!Expanded)
      continue;
    ExpSCEV->replaceAllUsesWith(Expanded);
    // TripCount should not be used after expansion to VPInstructions. Reset to
    // poison to avoid dangling references.
    if (Plan.getTripCount() == ExpSCEV)
      Plan.resetTripCount(Plan.getPoison(ExpSCEV->getScalarType()));
    ExpSCEV->eraseFromParent();
  }
}

DenseMap<const SCEV *, Value *>
VPlanTransforms::expandSCEVs(VPlan &Plan, ScalarEvolution &SE) {
  SCEVExpander Expander(SE, "induction", /*PreserveLCSSA=*/false);

  auto *Entry = cast<VPIRBasicBlock>(Plan.getEntry());
  BasicBlock *EntryBB = Entry->getIRBasicBlock();
  DenseMap<const SCEV *, Value *> ExpandedSCEVs;
  // Expand remaining VPExpandSCEVRecipes to IR instructions using SCEVExpander.
  for (VPRecipeBase &R : make_early_inc_range(*Entry)) {
    auto *ExpSCEV = dyn_cast<VPExpandSCEVRecipe>(&R);
    if (!ExpSCEV)
      continue;
    const SCEV *Expr = ExpSCEV->getSCEV();
    Value *Res =
        Expander.expandCodeFor(Expr, Expr->getType(), EntryBB->getTerminator());
    ExpandedSCEVs[Expr] = Res;
    VPValue *Exp = Plan.getOrAddLiveIn(Res);
    ExpSCEV->replaceAllUsesWith(Exp);
    if (Plan.getTripCount() == ExpSCEV)
      Plan.resetTripCount(Exp);
    ExpSCEV->eraseFromParent();
  }
  assert(none_of(*Entry, IsaPred<VPExpandSCEVRecipe>) &&
         "all VPExpandSCEVRecipes must have been expanded");
  // Add IR instructions in the entry basic block but not in the VPIRBasicBlock
  // to the VPIRBasicBlock.
  auto EI = Entry->begin();
  for (Instruction &I : drop_end(*EntryBB)) {
    if (EI != Entry->end() && isa<VPIRInstruction>(*EI) &&
        &cast<VPIRInstruction>(&*EI)->getInstruction() == &I) {
      EI++;
      continue;
    }
    VPIRInstruction::create(I)->insertBefore(*Entry, EI);
  }

  return ExpandedSCEVs;
}

/// The parts of a converted oracle plan its enclosing function is built from.
struct OraclePlanParts {
  /// The plan's single exit block.
  VPBasicBlock *ExitVPBB;
  /// The number of valid bytes, computed in \p ExitVPBB.
  VPValue *ByteCount;
  /// The replayed induction, whose start value is bound to the chunk start.
  VPPhi *IV;
};

/// Turn \p OraclePlan, a copy of the scalar loop taken before the vectorization
/// skeleton was added, into a speculative-load oracle: a scalar loop replaying
/// the original loop's exit conditions for the first VF lanes. The number of
/// valid bytes it computes is the number of replayed lanes times \p
/// EltStoreSize.
static OraclePlanParts convertToOraclePlan(VPlan &OraclePlan,
                                           uint64_t EltStoreSize) {
  // Route all exits (early exits and the latch's normal exit) into a single
  // shared exit block, replacing the original loop's VPIRBasicBlock exits, and
  // drop the recipes of those now-detached blocks. This must happen before the
  // dead-recipe removal below: their VPIRPhis keep values live that only the
  // original loop's live-outs used, which would then be replayed by the oracle
  // and turn their operands into oracle arguments.
  auto *ExitVPBB = OraclePlan.createVPBasicBlock("oracle.exit");
  for (VPIRBasicBlock *ExitVPIRBB : OraclePlan.getExitBlocks()) {
    for (VPBlockBase *Pred : to_vector(ExitVPIRBB->getPredecessors()))
      VPBlockUtils::connectBlocks(Pred, ExitVPBB, /*PredIdx=*/-1u,
                                  Pred->getIndexForSuccessor(ExitVPIRBB));
    for (VPRecipeBase &R : make_early_inc_range(*ExitVPIRBB))
      R.eraseFromParent();
  }

  // The oracle replays the loop body as-is into an optnone function, so drop
  // recipes not feeding the exit conditions. They are not merely redundant:
  // replaying them would emit their loads, and their operands would become
  // oracle arguments.
  VPlanTransforms::removeDeadRecipes(OraclePlan, /*RemovePhis=*/false);

  auto [HeaderVPBB, LatchVPBB] =
      VPBlockUtils::getPlainCFGHeaderAndLatch(OraclePlan);
  Type *IVTy = OraclePlan.getIndexType();

  // The replayed induction is the loop's only header phi, as ensured by
  // replaceUnsafeLoadsWithSpeculative. Capture it before adding the lane
  // counter below.
  auto *IV = cast<VPPhi>(&*HeaderVPBB->phis().begin());

  // Count the lanes replayed so far, starting at 1, and use the counter to
  // bound the replay to a single chunk. It replaces the original latch
  // condition, which cannot trigger inside a chunk as the vector loop only
  // executes full chunks. Deleting that condition also keeps the trip count
  // from becoming a spurious oracle argument.
  auto *LatchTerm = cast<VPInstruction>(LatchVPBB->getTerminator());
  assert(match(LatchTerm, m_BranchOnCond(m_VPValue())) &&
         "expected plain-CFG latch to be terminated by BranchOnCond");
  DebugLoc DL = LatchTerm->getDebugLoc();
  VPValue *One = OraclePlan.getConstantInt(IVTy, 1);
  auto *Lane = VPBuilder(HeaderVPBB, HeaderVPBB->getFirstNonPhi())
                   .createScalarPhi({One, One}, DL, "lane");
  VPBuilder LatchBuilder(LatchTerm);
  Lane->setOperand(1, LatchBuilder.createAdd(Lane, One, DL));
  LatchBuilder.createNaryOp(VPInstruction::BranchOnCount,
                            {Lane, &OraclePlan.getVF()}, DL);
  VPValue *OrigLatchCond = LatchTerm->getOperand(0);
  LatchTerm->eraseFromParent();
  vputils::recursivelyDeleteDeadRecipes(OrigLatchCond);

  // Replicate the memory, address and freeze recipes: the oracle is a scalar
  // loop, so they must generate a single scalar each rather than being widened.
  for (VPBasicBlock *VPBB : vp_rpo_plain_cfg_loop_body(HeaderVPBB)) {
    for (VPRecipeBase &R : make_early_inc_range(*VPBB)) {
      auto *VPI = dyn_cast<VPInstruction>(&R);
      if (!VPI || (VPI->getOpcode() != Instruction::Load &&
                   VPI->getOpcode() != Instruction::GetElementPtr &&
                   VPI->getOpcode() != Instruction::Freeze))
        continue;
      auto *Replicate = VPBuilder::createSingleScalarOp(
          VPI->getOpcode(), VPI->operands(), /*Mask=*/nullptr, *VPI, *VPI,
          VPI->getDebugLoc(), VPI->getUnderlyingInstr());
      Replicate->insertBefore(VPI);
      VPI->replaceAllUsesWith(Replicate);
      VPI->eraseFromParent();
    }
  }

  // Compute the byte count as an i64. Per @llvm.speculative.load semantics it
  // marks bytes [0, N) of the loaded vector as valid, so it is relative to the
  // start of the chunk.
  Type *I64Ty = Type::getInt64Ty(OraclePlan.getContext());
  VPBuilder ExitBuilder(ExitVPBB, ExitVPBB->begin());
  VPValue *Bytes = ExitBuilder.createScalarZExtOrTrunc(Lane, I64Ty, {});
  if (EltStoreSize != 1)
    Bytes = ExitBuilder.createNaryOp(
        Instruction::Mul,
        {Bytes, OraclePlan.getConstantInt(I64Ty, EltStoreSize)},
        VPIRFlags::getDefaultFlags(Instruction::Mul));

  VPlanTransforms::convertToConcreteRecipes(OraclePlan);
  return {ExitVPBB, Bytes, IV};
}

void VPlanTransforms::generateOracle(VPlan &Plan, VPlan &ScalarPlan,
                                     ElementCount VF,
                                     const TargetTransformInfo &TTI) {
  SmallVector<VPWidenIntrinsicRecipe *> SpeculativeLoads =
      vputils::collectSpeculativeLoads(Plan);
  if (SpeculativeLoads.empty())
    return;

  // Build the oracle plan from a fresh copy of the scalar loop. All speculative
  // loads share one element type, hence one byte count.
  VPlanPtr OraclePlanPtr(ScalarPlan.duplicate());
  VPlan &OraclePlan = *OraclePlanPtr;
  Type *EltTy = SpeculativeLoads.front()->getScalarType();
  auto [OracleExitVPBB, ByteCount, OracleIV] = convertToOraclePlan(
      OraclePlan, Plan.getDataLayout().getTypeStoreSize(EltTy).getFixedValue());

  // The oracle takes the index of the current chunk's first lane, passed by the
  // speculative loads as their first argument after the callee, followed by the
  // live-ins the replay reads. Only constants that cannot name memory are
  // materialized in the oracle function directly; everything else must be
  // passed in - in particular globals and constant expressions referring to
  // them - as the oracle is emitted with memory(argmem: read), as
  // @llvm.speculative.load requires.
  SmallVector<VPIRValue *> LiveIns;
  for (VPIRValue *LiveIn : OraclePlan.getLiveIns())
    if (!LiveIn->user_empty() &&
        !isa<ConstantInt, ConstantFP, UndefValue>(LiveIn->getValue()))
      LiveIns.push_back(LiveIn);
  SmallVector<Type *> ParamTys = {OraclePlan.getIndexType()};
  append_range(ParamTys, map_range(LiveIns, [](VPIRValue *LiveIn) {
                 return LiveIn->getScalarType();
               }));

  Type *I64Ty = Type::getInt64Ty(Plan.getContext());
  Function *OracleFn = Function::Create(
      FunctionType::get(I64Ty, ParamTys, false), GlobalValue::InternalLinkage,
      "speculativeLoadOracle", Plan.getModule());
  OracleFn->setMemoryEffects(MemoryEffects::argMemOnly(ModRefInfo::Ref));
  OracleFn->addFnAttr(Attribute::NoInline);
  // Keep the body exactly as emitted, so later passes cannot perturb the
  // precise per-lane replay.
  OracleFn->addFnAttr(Attribute::OptimizeNone);

  // Bind the oracle plan's live-ins to the matching arguments. Argument 0 is
  // the chunk's first lane, which the replayed induction starts at.
  Argument *ChunkStart = OracleFn->getArg(0);
  ChunkStart->setName("chunk.start");
  OracleIV->setOperand(0, OraclePlan.getOrAddLiveIn(ChunkStart));
  for (auto [LiveIn, Param] :
       zip_equal(LiveIns, drop_begin(OracleFn->args()))) {
    Param.setName(LiveIn->getValue()->getName());
    LiveIn->replaceAllUsesWith(OraclePlan.getOrAddLiveIn(&Param));
  }

  BasicBlock *EntryBB =
      BasicBlock::Create(Plan.getContext(), "oracle.entry", OracleFn);
  IRBuilder<> OracleBuilder(EntryBB);

  // Replace the VF placeholder with the runtime lane count, so the oracle
  // replays exactly the lanes the speculative loads produce.
  Value *RuntimeVF = getRuntimeVF(OracleBuilder, OraclePlan.getIndexType(), VF);
  OraclePlan.getVF().replaceAllUsesWith(OraclePlan.getOrAddLiveIn(RuntimeVF));
  // Placeholder terminator, replaced by VPlan::execute when stitching up the
  // plan's CFG.
  OracleBuilder.CreateUnreachable();

  // Make the materialized entry block the plan's entry, taking over the
  // original loop preheader's edge to the header.
  auto *NewEntry = OraclePlan.createEmptyVPIRBasicBlock(EntryBB);
  VPBlockUtils::reassociateBlocks(cast<VPBasicBlock>(OraclePlan.getEntry()),
                                  NewEntry);
  OraclePlan.setEntry(NewEntry);

  // Execute the oracle plan into the oracle function with VF=1. A LoopInfo lets
  // VPBasicBlock::execute register the loop; no AssumptionCache is needed as
  // the oracle only replicates Load/GEP/Freeze recipes. Use the generic
  // VPlan::execute rather than executeAndFinalize, which performs
  // main-loop-specific skeleton setup and cleanup.
  DominatorTree OracleDT(*OracleFn);
  LoopInfo OracleLI(OracleDT);
  VPTransformState OracleState(&TTI, ElementCount::getFixed(1), &OracleLI,
                               /*DT=*/nullptr, /*AC=*/nullptr, OracleBuilder,
                               &OraclePlan, /*CurrentParentLoop=*/nullptr);
  OracleState.CFG.PrevBB = EntryBB;
  OraclePlan.execute(&OracleState);
  OracleState.fixupHeaderPhis();

  // Return the byte count computed in the oracle exit block.
  BasicBlock *ExitBB = OracleState.CFG.VPBB2IRBB[OracleExitVPBB];
  ExitBB->getTerminator()->eraseFromParent();
  OracleBuilder.SetInsertPoint(ExitBB);
  OracleBuilder.CreateRet(OracleState.get(ByteCount, /*IsScalar=*/true));

  // Rebuild the speculative loads with the generated oracle function as callee,
  // replacing the poison placeholder, and the live-in arguments it reads
  // appended.
  VPValue *OracleCallee = Plan.getOrAddLiveIn(OracleFn);
  SmallVector<VPValue *> OracleLiveIns;
  for (VPIRValue *LiveIn : LiveIns)
    OracleLiveIns.push_back(Plan.getOrAddLiveIn(LiveIn->getValue()));
  for (VPWidenIntrinsicRecipe *R : SpeculativeLoads) {
    SmallVector<VPValue *> Ops = {R->getOperand(0), R->getOperand(1),
                                  OracleCallee, R->getOperand(3)};
    append_range(Ops, OracleLiveIns);
    auto *NewR = new VPWidenIntrinsicRecipe(Intrinsic::speculative_load, Ops,
                                            R->getScalarType(), *R, *R,
                                            R->getDebugLoc());
    NewR->insertBefore(R);
    R->replaceAllUsesWith(NewR);
    R->eraseFromParent();
  }
}

/// Add branch weight metadata, if the \p Plan's middle block is terminated by a
/// BranchOnCond recipe.
void VPlanTransforms::addBranchWeightToMiddleTerminator(
    VPlan &Plan, ElementCount VF, std::optional<unsigned> VScaleForTuning) {
  VPBasicBlock *MiddleVPBB = Plan.getMiddleBlock();
  auto *MiddleTerm =
      dyn_cast_or_null<VPInstruction>(MiddleVPBB->getTerminator());
  // Only add branch metadata if there is a (conditional) terminator.
  if (!MiddleTerm)
    return;

  assert(MiddleTerm->getOpcode() == VPInstruction::BranchOnCond &&
         "must have a BranchOnCond");
  // Assume that `TripCount % VectorStep ` is equally distributed.
  unsigned VectorStep = Plan.getConcreteUF() * VF.getKnownMinValue();
  if (VF.isScalable() && VScaleForTuning.has_value())
    VectorStep *= *VScaleForTuning;
  assert(VectorStep > 0 && "trip count should not be zero");
  MDBuilder MDB(Plan.getContext());
  MDNode *BranchWeights =
      MDB.createBranchWeights({1, VectorStep - 1}, /*IsExpected=*/false);
  MiddleTerm->setMetadata(LLVMContext::MD_prof, BranchWeights);
}
