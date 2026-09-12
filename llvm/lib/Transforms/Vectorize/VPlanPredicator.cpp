//===-- VPlanPredicator.cpp - VPlan predicator ----------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
///
/// \file
/// This file implements predication for VPlans.
///
//===----------------------------------------------------------------------===//

#include "VPRecipeBuilder.h"
#include "VPlan.h"
#include "VPlanCFG.h"
#include "VPlanDominatorTree.h"
#include "VPlanPatternMatch.h"
#include "VPlanTransforms.h"
#include "VPlanUtils.h"
#include "llvm/ADT/PostOrderIterator.h"

using namespace llvm;
using namespace VPlanPatternMatch;

namespace {
class VPPredicator {
  VPlan &Plan;

  /// Builder to construct recipes to compute masks.
  VPBuilder Builder;

  /// Dominator tree for the VPlan.
  VPDominatorTree VPDT;

  /// Post-dominator tree for the VPlan.
  VPPostDominatorTree VPPDT;

  /// Post-dominator frontier for the VPlan.
  VPPostDominanceFrontier VPPDF;

  /// When we if-convert we need to create edge masks. We have to cache values
  /// so that we don't end up with exponential recursion/IR.
  using EdgeMaskCacheTy =
      DenseMap<std::pair<const VPBasicBlock *, const VPBasicBlock *>,
               VPValue *>;
  using BlockMaskCacheTy = DenseMap<const VPBasicBlock *, VPValue *>;
  EdgeMaskCacheTy EdgeMaskCache;

  BlockMaskCacheTy BlockMaskCache;

  /// Blocks terminated by a uniform branch that is kept as control flow
  /// instead of being if-converted, mapped to the block in which the branch's
  /// edges merge again. See getBlockToKeepUnderUniformBranch.
  DenseMap<const VPBasicBlock *, VPBasicBlock *> UniformBranchToMergeBlock;

  /// Create an edge mask for every destination of cases and/or default.
  void createSwitchEdgeMasks(const VPInstruction *SI);

  /// Computes and return the predicate of the edge between \p Src and \p Dst,
  /// possibly inserting new recipes at \p Dst (using Builder's insertion point)
  VPValue *createEdgeMask(const VPBasicBlock *Src, const VPBasicBlock *Dst);

  /// Record \p Mask as the *entry* mask of \p VPBB, which is expected to not
  /// already have a mask.
  void setBlockInMask(const VPBasicBlock *VPBB, VPValue *Mask) {
    // TODO: Include the masks as operands in the predicated VPlan directly to
    // avoid keeping the map of masks beyond the predication transform.
    assert(!getBlockInMask(VPBB) && "Mask already set");
    BlockMaskCache[VPBB] = Mask;
  }

  /// Record \p Mask as the mask of the edge from \p Src to \p Dst. The edge is
  /// expected to not have a mask already.
  VPValue *setEdgeMask(const VPBasicBlock *Src, const VPBasicBlock *Dst,
                       VPValue *Mask) {
    assert(Src != Dst && "Src and Dst must be different");
    assert(!getEdgeMask(Src, Dst) && "Mask already set");
    return EdgeMaskCache[{Src, Dst}] = Mask;
  }

  /// Returns where to insert new masks in \p VPBB.
  VPBasicBlock::iterator getMaskInsertPoint(VPBasicBlock *VPBB) {
    if (VPValue *Mask = getBlockInMask(VPBB))
      if (VPRecipeBase *MaskR = Mask->getDefiningRecipe())
        if (MaskR->getParent() == VPBB) // In-mask may be the IDom's.
          return std::next(MaskR->getIterator());
    return VPBB->getFirstNonPhi();
  }

  using EdgeTy = std::pair<const VPBasicBlock *, const VPBasicBlock *>;

  /// Compute the set of edges that are "furthest up" in the CFG for each
  /// incoming value of \p Phi.
  MapVector<EdgeTy, VPValue *> computeBlendEdges(VPPhi *Phi);

  /// Given a set of \p Edges that each can reach \p VPBB, return the OR of all
  /// edges, or an equivalent block in-mask.
  VPValue *createBlendMaskForEdges(ArrayRef<EdgeTy> Edges, VPBasicBlock *VPBB);

public:
  VPPredicator(VPlan &Plan)
      : Plan(Plan), VPDT(Plan), VPPDT(Plan), VPPDF(VPPDT) {}

  /// Returns the *entry* mask for \p VPBB.
  VPValue *getBlockInMask(const VPBasicBlock *VPBB) const {
    return BlockMaskCache.lookup(VPBB);
  }

  /// Returns the precomputed predicate of the edge from \p Src to \p Dst.
  VPValue *getEdgeMask(const VPBasicBlock *Src, const VPBasicBlock *Dst) const {
    return EdgeMaskCache.lookup({Src, Dst});
  }

  /// Compute the predicate of \p VPBB.
  void createBlockInMask(VPBasicBlock *VPBB);

  /// Convert phi recipes in \p VPBB to VPBlendRecipes.
  void convertPhisToBlends(VPBasicBlock *VPBB);

  /// Predicate and linearize the plan.
  void run();
};
} // namespace

VPValue *VPPredicator::createEdgeMask(const VPBasicBlock *Src,
                                      const VPBasicBlock *Dst) {
  assert(is_contained(Dst->getPredecessors(), Src) && "Invalid edge");

  // Look for cached value.
  VPValue *EdgeMask = getEdgeMask(Src, Dst);
  if (EdgeMask)
    return EdgeMask;

  VPValue *SrcMask = getBlockInMask(Src);

  // If there's a single successor, there's no terminator recipe.
  if (Src->getNumSuccessors() == 1)
    return setEdgeMask(Src, Dst, SrcMask);

  auto *Term = cast<VPInstruction>(Src->getTerminator());
  if (Term->getOpcode() == Instruction::Switch) {
    createSwitchEdgeMasks(Term);
    return getEdgeMask(Src, Dst);
  }

  assert(Term->getOpcode() == VPInstruction::BranchOnCond &&
         "Unsupported terminator");
  if (Src->getSuccessors()[0] == Src->getSuccessors()[1])
    return setEdgeMask(Src, Dst, SrcMask);

  // A uniform branch that is kept remains actual control flow. All lanes take
  // the same edge, so every lane reaching Src also reaches Dst if that edge is
  // taken.
  if (UniformBranchToMergeBlock.contains(Src))
    return setEdgeMask(Src, Dst, SrcMask);

  EdgeMask = Term->getOperand(0);
  assert(EdgeMask && "No Edge Mask found for condition");

  if (Src->getSuccessors()[0] != Dst)
    EdgeMask = Builder.createNot(EdgeMask, Term->getDebugLoc());

  if (SrcMask) { // Otherwise block in-mask is all-one, no need to AND.
    // The bitwise 'And' of SrcMask and EdgeMask introduces new UB if SrcMask
    // is false and EdgeMask is poison. Avoid that by using 'LogicalAnd'
    // instead which generates 'select i1 SrcMask, i1 EdgeMask, i1 false'.
    EdgeMask = Builder.createLogicalAnd(SrcMask, EdgeMask, Term->getDebugLoc());
  }

  return setEdgeMask(Src, Dst, EdgeMask);
}

void VPPredicator::createBlockInMask(VPBasicBlock *VPBB) {
  // Start inserting after the block's phis, which be replaced by blends later.
  Builder.setInsertPoint(VPBB, VPBB->getFirstNonPhi());

  // Reuse the mask of the immediate dominator if the VPBB post-dominates the
  // immediate dominator.
  auto *IDom = VPDT.getNode(VPBB)->getIDom();
  assert(IDom && "Block in loop must have immediate dominator");
  auto *IDomBB = cast<VPBasicBlock>(IDom->getBlock());
  if (VPPDT.properlyDominates(VPBB, IDomBB)) {
    setBlockInMask(VPBB, getBlockInMask(IDomBB));
    return;
  }
  // All-one mask is modelled as no-mask following the convention for masked
  // load/store/gather/scatter. Initialize BlockMask to no-mask.
  VPValue *BlockMask = nullptr;
  // This is the block mask. We OR all unique incoming edges.
  for (auto *Predecessor : SetVector<VPBlockBase *>(
           VPBB->getPredecessors().begin(), VPBB->getPredecessors().end())) {
    VPValue *EdgeMask = createEdgeMask(cast<VPBasicBlock>(Predecessor), VPBB);
    if (!EdgeMask) { // Mask of predecessor is all-one so mask of block is
                     // too.
      setBlockInMask(VPBB, EdgeMask);
      return;
    }

    if (!BlockMask) { // BlockMask has its initial nullptr value.
      BlockMask = EdgeMask;
      continue;
    }

    BlockMask = Builder.createOr(BlockMask, EdgeMask, {});
  }

  setBlockInMask(VPBB, BlockMask);
}

void VPPredicator::createSwitchEdgeMasks(const VPInstruction *SI) {
  const VPBasicBlock *Src = SI->getParent();

  // Create masks where SI is a switch. We create masks for all edges from SI's
  // parent block at the same time. This is more efficient, as we can create and
  // collect compares for all cases once.
  VPValue *Cond = SI->getOperand(0);
  VPBasicBlock *DefaultDst = cast<VPBasicBlock>(Src->getSuccessors()[0]);
  MapVector<VPBasicBlock *, SmallVector<VPValue *>> Dst2Compares;
  for (const auto &[Idx, Succ] : enumerate(drop_begin(Src->getSuccessors()))) {
    VPBasicBlock *Dst = cast<VPBasicBlock>(Succ);
    assert(!getEdgeMask(Src, Dst) && "Edge masks already created");
    //  Cases whose destination is the same as default are redundant and can
    //  be ignored - they will get there anyhow.
    if (Dst == DefaultDst)
      continue;
    auto &Compares = Dst2Compares[Dst];
    VPValue *V = SI->getOperand(Idx + 1);
    Compares.push_back(Builder.createICmp(CmpInst::ICMP_EQ, Cond, V));
  }

  // We need to handle 2 separate cases below for all entries in Dst2Compares,
  // which excludes destinations matching the default destination.
  VPValue *SrcMask = getBlockInMask(Src);
  VPValue *DefaultMask = nullptr;
  for (const auto &[Dst, Conds] : Dst2Compares) {
    // 1. Dst is not the default destination. Dst is reached if any of the
    // cases with destination == Dst are taken. Join the conditions for each
    // case whose destination == Dst using an OR.
    VPValue *Mask = Conds[0];
    for (VPValue *V : drop_begin(Conds))
      Mask = Builder.createOr(Mask, V);
    if (SrcMask)
      Mask = Builder.createLogicalAnd(SrcMask, Mask);
    setEdgeMask(Src, Dst, Mask);

    // 2. Create the mask for the default destination, which is reached if
    // none of the cases with destination != default destination are taken.
    // Join the conditions for each case where the destination is != Dst using
    // an OR and negate it.
    DefaultMask = DefaultMask ? Builder.createOr(DefaultMask, Mask) : Mask;
  }

  if (DefaultMask) {
    DefaultMask = Builder.createNot(DefaultMask);
    if (SrcMask)
      DefaultMask = Builder.createLogicalAnd(SrcMask, DefaultMask);
  } else {
    // There are no destinations other than the default destination, so this is
    // an unconditional branch.
    DefaultMask = SrcMask;
  }
  setEdgeMask(Src, DefaultDst, DefaultMask);
}

// Start by keeping track of what edges lead to which value. Then see if any
// node has the same value for all outgoing edges. If so then propagate that
// value up to every node it postdominates. E.g:
//
//    Entry      Edges =  {C->ɸ : %x, D->ɸ : %x, F->ɸ : %y}
//    /   \            [C,D,F all outgoing edges equal: go up postdom frontier]
//   A     B           ~> {A->C : %x, A->D : %x, Entry->B : %y}
//  / \    |\          [A all outgoing edges equal: go up postdom frontier]
// C   D   | E         ~> {Entry->A : %x, Entry->B : %y}
//  \   \  |/
//   \  |  F
//    \ | /
//      ɸ = phi [%x, C], [%x, D], [%y, F]
MapVector<VPPredicator::EdgeTy, VPValue *>
VPPredicator::computeBlendEdges(VPPhi *Phi) {
  MapVector<EdgeTy, VPValue *> Edges;

  // Mark the given edge as providing the value \p V.
  auto AddEdge = [&Edges](const VPBlockBase *From, const VPBlockBase *To,
                          VPValue *V) {
    EdgeTy Edge = {cast<VPBasicBlock>(From), cast<VPBasicBlock>(To)};
    assert((!Edges.contains(Edge) || Edges.lookup(Edge) == V) &&
           "Clobbering an edge?");
    Edges[Edge] = V;
  };

  for (auto [InVal, InVPBB] : Phi->incoming_values_and_blocks())
    AddEdge(InVPBB, Phi->getParent(), InVal);

  SetVector<const VPBlockBase *> Worklist(from_range, Phi->incoming_blocks());
  while (!Worklist.empty()) {
    auto *VPBB = cast<VPBasicBlock>(Worklist.pop_back_val());

    // Check that all outgoing edges from VPBB have the same value.
    SmallVector<EdgeTy> OutEdges;
    for (const VPBlockBase *Succ : VPBB->getSuccessors())
      OutEdges.emplace_back(VPBB, cast<VPBasicBlock>(Succ));
    auto OutVals =
        map_range(OutEdges, [&Edges](EdgeTy E) { return Edges.lookup(E); });
    VPValue *Common = *OutVals.begin();
    if (!Common || !all_equal(OutVals))
      continue;

    // They have the same value: we can move the edges up.
    for (EdgeTy Edge : OutEdges)
      Edges.erase(Edge);

    // Iterate up through the post dominance frontier.
    assert(VPPDF.find(VPBB) != VPPDF.end() &&
           "VPBB must have a post-dominance frontier entry");
    for (const VPBlockBase *Frontier : VPPDF.find(VPBB)->second) {
      for (const VPBlockBase *FrontierSucc : Frontier->getSuccessors())
        if (VPPDT.dominates(VPBB, FrontierSucc))
          AddEdge(Frontier, FrontierSucc, Common);
      Worklist.insert(cast<VPBasicBlock>(Frontier));
    }
  }

  return Edges;
}

VPValue *VPPredicator::createBlendMaskForEdges(ArrayRef<EdgeTy> Edges,
                                               VPBasicBlock *VPBB) {
  // If the nearest common postdominator to all of Edges destinations isn't VPBB
  // then we can use its block in-mask. E.g:
  //
  //  A  ...  B
  //   \   \ /
  //    \   C
  //     \ /
  // ...  D   ...
  //    \ |  /
  //     VPBB
  //
  // If the edges are A->D and B->C, PostDom will be D. We can reuse Ds block
  // in-mask.
  const VPBasicBlock *PostDom = Edges[0].second;
  for (auto [_, DstVPBB] : drop_begin(Edges))
    PostDom =
        cast<VPBasicBlock>(VPPDT.findNearestCommonDominator(PostDom, DstVPBB));
  assert(VPPDT.dominates(VPBB, PostDom) && "VPBB doesn't postdominate edges");
  if (PostDom != VPBB)
    return getBlockInMask(PostDom);

  // Otherwise, compute the disjunction of edges.
  VPValue *Mask = nullptr;
  for (auto [Src, ConstDst] : Edges) {
    auto *Dst = const_cast<VPBasicBlock *>(ConstDst);
    VPValue *EdgeMask;
    {
      VPBuilder::InsertPointGuard Guard(Builder);
      Builder.setInsertPoint(Dst, getMaskInsertPoint(Dst));
      EdgeMask = createEdgeMask(Src, Dst);
    }
    Mask = Mask ? Builder.createOr(Mask, EdgeMask) : EdgeMask;
  }
  return Mask;
}

void VPPredicator::convertPhisToBlends(VPBasicBlock *VPBB) {
  Builder.setInsertPoint(VPBB, getMaskInsertPoint(VPBB));

  SmallVector<VPPhi *> Phis;
  for (VPRecipeBase &R : VPBB->phis())
    Phis.push_back(cast<VPPhi>(&R));
  for (VPPhi *PhiR : Phis) {
    // The non-header Phi is converted into a Blend recipe below,
    // so we don't have to worry about the insertion order and we can just use
    // the builder. At this point we generate the predication tree. There may
    // be duplications since this is a simple recursive scan, but future
    // optimizations will clean it up.

    auto NotPoison = make_filter_range(PhiR->incoming_values(), [](VPValue *V) {
      return !match(V, m_Poison());
    });
    if (all_equal(NotPoison)) {
      PhiR->replaceAllUsesWith(NotPoison.empty() ? PhiR->getIncomingValue(0)
                                                 : *NotPoison.begin());
      PhiR->eraseFromParent();
      continue;
    }

    MapVector<VPValue *, SmallVector<EdgeTy>> InValEdgesMap;
    for (auto [Edge, Val] : computeBlendEdges(PhiR))
      InValEdgesMap[Val].push_back(Edge);
    auto InValEdges = InValEdgesMap.takeVector();

    // Sort the incoming value order to match PhiR as much as possible.
    llvm::stable_sort(InValEdges, [&PhiR](auto &L, auto &R) {
      auto InVs = PhiR->incoming_values();
      return std::distance(InVs.begin(), find(InVs, L.first)) <
             std::distance(InVs.begin(), find(InVs, R.first));
    });

    SmallVector<VPValue *, 2> OperandsWithMask;
    for (const auto &[InVPV, Edges] : InValEdges) {
      OperandsWithMask.push_back(InVPV);
      OperandsWithMask.push_back(createBlendMaskForEdges(Edges, VPBB));
    }
    PHINode *IRPhi = cast_or_null<PHINode>(PhiR->getUnderlyingValue());
    auto *Blend =
        new VPBlendRecipe(IRPhi, OperandsWithMask, *PhiR, PhiR->getDebugLoc());
    Builder.insert(Blend);
    PhiR->replaceAllUsesWith(Blend);
    PhiR->eraseFromParent();
  }
}

/// Returns true if \p V produces the same value for all lanes of a vector
/// iteration, and for all parts once the plan is unrolled.
///
/// Note that vputils::isSingleScalar must not be used for this: it also
/// returns true for values of which only the first lane is used, and it
/// classifies the loads, casts and phis of the initial VPlan as single-scalar
/// even though they are widened later.
static bool isUniformAcrossLanes(const VPValue *V) {
  // AnyOf reduces a vector to a single i1 that all lanes share, and the
  // unroller combines it across all parts, so a single use of it is valid for
  // the whole unrolled iteration.
  // TODO: Generalize to the other vector-to-scalar opcodes that
  // VPlanTransforms::unrollByUF combines across parts, once one of them can
  // be a branch condition.
  if (match(V, m_AnyOf(m_VPValue())))
    return true;
  // Uniform across all VFs and UFs implies uniform across the lanes of a
  // single vector iteration.
  return vputils::isUniformAcrossVFsAndUFs(V);
}

/// Returns the block guarded by the terminator of \p VPBB, if that terminator
/// is a branch that is better kept as control flow than if-converted, and
/// nullptr otherwise. Note that this also decides whether keeping the branch
/// pays off, it is not a purely structural query.
///
/// A branch can be kept if its condition is uniform across all lanes: they
/// then all take the same edge, so the guarded block can execute unmasked
/// under a real branch. Only the simplest shape is handled, where \p VPBB
/// branches to a guarded block, which has \p VPBB as its only predecessor, and
/// to a merge block, which is the guarded block's only successor and has no
/// other predecessors:
///
///     VPBB
///     |   \
///     |  Guarded
///     |   /
///     Merge
///
/// Merge must not contain any phis. Those could stay phis, as the branch stays
/// control flow, but convertPhisToBlends turns all of them into
/// VPBlendRecipes, and a blend cannot tell the two edges apart because they
/// have the same mask.
/// TODO: Keep the phis of Merge instead of blending them.
static VPBasicBlock *getBlockToKeepUnderUniformBranch(VPBasicBlock *VPBB) {
  if (VPBB->getNumSuccessors() != 2)
    return nullptr;
  VPValue *Cond;
  if (!match(VPBB->getTerminator(), m_BranchOnCond(m_VPValue(Cond))) ||
      !isUniformAcrossLanes(Cond))
    return nullptr;

  auto *Succ0 = cast<VPBasicBlock>(VPBB->getSuccessors()[0]);
  auto *Succ1 = cast<VPBasicBlock>(VPBB->getSuccessors()[1]);
  if (Succ0 == Succ1)
    return nullptr;
  auto *Guarded = Succ0->getSingleSuccessor() == Succ1 ? Succ0 : Succ1;
  auto *Merge = Guarded == Succ0 ? Succ1 : Succ0;
  if (Guarded->getSinglePredecessor() != VPBB ||
      Guarded->getSingleSuccessor() != Merge ||
      Merge->getNumPredecessors() != 2 ||
      Merge->getFirstNonPhi() != Merge->begin())
    return nullptr;

  // Merge having no phis, together with Guarded not dominating any other
  // block, means no value defined in Guarded is used outside of it. That is
  // what keeps the transforms that run on the kept sub-CFG correct.
  assert(none_of(*Guarded,
                 [Guarded](VPRecipeBase &R) {
                   return any_of(R.definedValues(), [Guarded](VPValue *Def) {
                     return any_of(Def->users(), [Guarded](VPUser *U) {
                       return cast<VPRecipeBase>(U)->getParent() != Guarded;
                     });
                   });
                 }) &&
         "a value defined in the guarded block escapes it");

  // Keeping the branch only pays off if it skips an operation that would
  // otherwise have to be masked; if-converting a block of plain arithmetic is
  // cheaper, as it avoids the branch.
  // TODO: This runs before a VF is chosen, so the cost of both alternatives
  // cannot be compared here. Model both and decide per VF instead.
  if (none_of(*Guarded, [](VPRecipeBase &R) {
        return R.mayReadOrWriteMemory() || R.mayHaveSideEffects();
      }))
    return nullptr;
  return Guarded;
}

void VPPredicator::run() {
  VPBasicBlock *Header = Plan.getVectorLoopRegion()->getEntryBasicBlock();
  // Scan the body of the loop in a topological order to visit each basic
  // block after having visited its predecessor basic blocks.
  ReversePostOrderTraversal<VPBlockShallowTraversalWrapper<VPBlockBase *>> RPOT(
      Header);
  // Non-outer regions with VPBBs only are supported at the moment.
  auto Blocks = to_vector(VPBlockUtils::blocksAs<VPBasicBlock>(RPOT));
  DenseMap<const VPBasicBlock *, std::optional<VPExecutionFrequency>>
      Frequencies = vputils::computeExecutionFrequencies(Blocks);

  // Collect the uniform branches to keep as control flow, together with the
  // blocks of the sub-CFG they guard.
  SmallPtrSet<const VPBasicBlock *, 4> BlocksInKeptSubCFG;
  for (VPBasicBlock *VPBB : Blocks) {
    VPBasicBlock *Guarded = getBlockToKeepUnderUniformBranch(VPBB);
    if (!Guarded)
      continue;
    auto *Merge = cast<VPBasicBlock>(Guarded->getSingleSuccessor());
    UniformBranchToMergeBlock[VPBB] = Merge;
    BlocksInKeptSubCFG.insert(Guarded);
    BlocksInKeptSubCFG.insert(Merge);
  }

  for (VPBasicBlock *VPBB : Blocks) {
    // Introduce the mask for VPBB, which may introduce needed edge masks, and
    // convert all phi recipes of VPBB to blend recipes unless VPBB is the
    // header.
    if (VPBB != Header)
      createBlockInMask(VPBB);

    VPValue *BlockMask = getBlockInMask(VPBB);
    if (!BlockMask)
      continue;

    // Mask all VPInstructions in the block and record the frequency with
    // which the masked recipes execute.
    std::optional<VPExecutionFrequency> Freq = Frequencies.lookup(VPBB);
    for (VPRecipeBase &R : *VPBB) {
      auto *VPI = dyn_cast<VPInstruction>(&R);
      if (!VPI)
        continue;
      VPI->addMask(BlockMask);
      if (VPI->isMasked())
        VPI->setExecutionFrequency(Freq, Plan.getContext());
    }
  }

  for (VPBasicBlock *VPBB : reverse(Blocks))
    if (VPBB != Header)
      convertPhisToBlends(VPBB);

  // Linearize the blocks of the loop into one serial chain. A uniform branch
  // that is kept together with the block it guards forms a single-entry
  // single-exit sub-CFG, which is spliced into the chain as a unit: the chain
  // enters at the branch's block and leaves at its merge block, keeping the
  // edges in between.
  VPBlockBase *PrevVPBB = nullptr;
  for (VPBasicBlock *VPBB : Blocks) {
    // Skip blocks of a kept sub-CFG; they are connected via its entry block.
    if (BlocksInKeptSubCFG.contains(VPBB))
      continue;
    // Find the block the chain continues at. Sub-CFGs can be adjacent, if the
    // merge block of one is the entry block of the next.
    VPBasicBlock *SubCFGExit = VPBB;
    while (VPBasicBlock *Merge = UniformBranchToMergeBlock.lookup(SubCFGExit))
      SubCFGExit = Merge;

    auto Successors = to_vector(SubCFGExit->getSuccessors());
    if (Successors.size() > 1)
      SubCFGExit->getTerminator()->eraseFromParent();

    // Flatten the CFG in the loop. To do so, first disconnect the sub-CFG's
    // exit from its successors. Then connect VPBB to the previously visited
    // VPBB.
    for (auto *Succ : Successors)
      VPBlockUtils::disconnectBlocks(SubCFGExit, Succ);
    if (PrevVPBB)
      VPBlockUtils::connectBlocks(PrevVPBB, VPBB);

    PrevVPBB = SubCFGExit;
  }
}

void VPlanTransforms::introduceMasksAndLinearize(VPlan &Plan) {
  // Nested loop regions (outer-loop vectorization) are not supported yet.
  if (Plan.isOuterLoop())
    return;
  VPPredicator(Plan).run();
}
