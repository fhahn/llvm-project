//===- VPlanCFG.h - GraphTraits for VP blocks -------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
/// Specializations of GraphTraits that allow VPBlockBase graphs to be
/// treated as proper graphs for generic algorithms;
//===----------------------------------------------------------------------===//

#ifndef LLVM_TRANSFORMS_VECTORIZE_VPLANCFG_H
#define LLVM_TRANSFORMS_VECTORIZE_VPLANCFG_H

#include "VPlan.h"
#include "VPlanUtils.h"
#include "llvm/ADT/DepthFirstIterator.h"
#include "llvm/ADT/GraphTraits.h"
#include "llvm/ADT/PostOrderIterator.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"

namespace llvm {

//===----------------------------------------------------------------------===//
// GraphTraits specializations for VPlan Hierarchical Control-Flow Graphs     //
//===----------------------------------------------------------------------===//

/// Iterator to traverse all successors of a VPBlockBase node. This includes the
/// entry node of VPRegionBlocks. Exit blocks of a region implicitly have their
/// parent region's successors. This ensures all blocks in a region are visited
/// before any blocks in a successor region when doing a reverse post-order
// traversal of the graph. Region blocks themselves traverse only their entries
// directly and not their successors. Those will be traversed when a region's
// exiting block is traversed
template <typename BlockPtrTy, typename NodeTy = BlockPtrTy>
class VPAllSuccessorsIterator
    : public iterator_facade_base<VPAllSuccessorsIterator<BlockPtrTy, NodeTy>,
                                  std::bidirectional_iterator_tag,
                                  VPBlockBase> {
  using RegionPtrTy = decltype(std::declval<BlockPtrTy>()->getParent());
  BlockPtrTy Block;
  /// Index of the current successor. For VPBasicBlock nodes, this simply is the
  /// index for the successor array. For VPRegionBlock, SuccessorIdx == 0 is
  /// used for the region's entry block, and SuccessorIdx - 1 are the indices
  /// for the successor array.
  size_t SuccessorIdx;
  /// Optional filter region. If set, only successors within this region
  /// (including nested regions) are returned.
  RegionPtrTy FilterRegion;

  static BlockPtrTy getBlockWithSuccs(BlockPtrTy Current) {
    while (Current && Current->getNumSuccessors() == 0)
      Current = Current->getParent();
    return Current;
  }

  bool isInFilterRegion(BlockPtrTy B) const {
    if (!FilterRegion)
      return true;
    for (auto *P = B->getParent(); P; P = P->getParent())
      if (P == FilterRegion)
        return true;
    return false;
  }

  /// Templated helper to dereference successor \p SuccIdx of \p Block. Used by
  /// both the const and non-const operator* implementations.
  template <typename T1> static T1 deref(T1 Block, unsigned SuccIdx) {
    if (auto *R = dyn_cast<VPRegionBlock>(Block)) {
      assert(SuccIdx == 0);
      return R->getEntry();
    }

    // For exit blocks, use the next parent region with successors.
    return getBlockWithSuccs(Block)->getSuccessors()[SuccIdx];
  }

  static size_t getEndIdx(BlockPtrTy Block) {
    if (auto *R = dyn_cast<VPRegionBlock>(Block))
      return 1;
    BlockPtrTy ParentWithSuccs = getBlockWithSuccs(Block);
    return ParentWithSuccs ? ParentWithSuccs->getNumSuccessors() : 0;
  }

public:
  /// Used by iterator_facade_base with bidirectional_iterator_tag.
  using reference = NodeTy;

  VPAllSuccessorsIterator(BlockPtrTy Block, size_t Idx = 0,
                          RegionPtrTy FilterRegion = nullptr,
                          bool SkipToValid = true)
      : Block(Block), SuccessorIdx(Idx), FilterRegion(FilterRegion) {
    if (SkipToValid) {
      while (SuccessorIdx < getEndIdx(Block) &&
             !isInFilterRegion(deref(Block, SuccessorIdx)))
        ++SuccessorIdx;
    }
  }

  static VPAllSuccessorsIterator end(BlockPtrTy Block,
                                     RegionPtrTy FilterRegion = nullptr) {
    return {Block, getEndIdx(Block), FilterRegion, false};
  }

  bool operator==(const VPAllSuccessorsIterator &R) const {
    return Block == R.Block && SuccessorIdx == R.SuccessorIdx;
  }

  NodeTy operator*() const {
    if constexpr (std::is_same_v<NodeTy, BlockPtrTy>)
      return deref(Block, SuccessorIdx);
    else
      return {deref(Block, SuccessorIdx), FilterRegion};
  }

  VPAllSuccessorsIterator &operator++() {
    do {
      ++SuccessorIdx;
    } while (SuccessorIdx < getEndIdx(Block) &&
             !isInFilterRegion(deref(Block, SuccessorIdx)));
    return *this;
  }

  VPAllSuccessorsIterator &operator--() {
    do {
      --SuccessorIdx;
    } while (SuccessorIdx > 0 &&
             !isInFilterRegion(deref(Block, SuccessorIdx)));
    return *this;
  }

  VPAllSuccessorsIterator operator++(int) {
    VPAllSuccessorsIterator Orig = *this;
    ++(*this);
    return Orig;
  }
};

/// Pair of a VPBlockBase pointer and filter region for traversal.
template <typename BlockTy> struct VPBlockWithRegion {
  using RegionPtrTy = decltype(std::declval<BlockTy>()->getParent());
  BlockTy Block;
  RegionPtrTy FilterRegion;

  operator BlockTy() const { return Block; }
  bool operator==(const VPBlockWithRegion &R) const { return Block == R.Block; }
};

/// Helper for GraphTraits specialization that traverses through VPRegionBlocks.
template <typename BlockTy> class VPBlockDeepTraversalWrapper {
  using RegionPtrTy = decltype(std::declval<BlockTy>()->getParent());
  BlockTy Entry;
  RegionPtrTy FilterRegion;

public:
  VPBlockDeepTraversalWrapper(BlockTy Entry,
                              RegionPtrTy FilterRegion = nullptr)
      : Entry(Entry),
        FilterRegion(FilterRegion ? FilterRegion : Entry->getParent()) {}
  BlockTy getEntry() const { return Entry; }
  RegionPtrTy getFilterRegion() const { return FilterRegion; }
};

/// GraphTraits specialization to recursively traverse VPBlockBase nodes,
/// including traversing through VPRegionBlocks. Exit blocks of a region
/// implicitly have their parent region's successors. This ensures all blocks in
/// a region are visited before any blocks in a successor region when doing a
/// reverse post-order traversal of the graph.
template <> struct GraphTraits<VPBlockDeepTraversalWrapper<VPBlockBase *>> {
  using NodeRef = VPBlockWithRegion<VPBlockBase *>;
  using ChildIteratorType =
      VPAllSuccessorsIterator<VPBlockBase *, VPBlockWithRegion<VPBlockBase *>>;

  static NodeRef getEntryNode(VPBlockDeepTraversalWrapper<VPBlockBase *> N) {
    return {N.getEntry(), N.getFilterRegion()};
  }

  static ChildIteratorType child_begin(NodeRef N) {
    return ChildIteratorType(N.Block, 0, N.FilterRegion);
  }

  static ChildIteratorType child_end(NodeRef N) {
    return ChildIteratorType::end(N.Block, N.FilterRegion);
  }
};

template <>
struct GraphTraits<VPBlockDeepTraversalWrapper<const VPBlockBase *>> {
  using NodeRef = VPBlockWithRegion<const VPBlockBase *>;
  using ChildIteratorType =
      VPAllSuccessorsIterator<const VPBlockBase *,
                              VPBlockWithRegion<const VPBlockBase *>>;

  static NodeRef
  getEntryNode(VPBlockDeepTraversalWrapper<const VPBlockBase *> N) {
    return {N.getEntry(), N.getFilterRegion()};
  }

  static ChildIteratorType child_begin(NodeRef N) {
    return ChildIteratorType(N.Block, 0, N.FilterRegion);
  }

  static ChildIteratorType child_end(NodeRef N) {
    return ChildIteratorType::end(N.Block, N.FilterRegion);
  }
};

/// Helper for GraphTraits specialization that does not traverses through
/// VPRegionBlocks.
template <typename BlockTy> class VPBlockShallowTraversalWrapper {
  BlockTy Entry;

public:
  VPBlockShallowTraversalWrapper(BlockTy Entry) : Entry(Entry) {}
  BlockTy getEntry() const { return Entry; }
};

template <> struct GraphTraits<VPBlockShallowTraversalWrapper<VPBlockBase *>> {
  using NodeRef = VPBlockBase *;
  using ChildIteratorType = SmallVectorImpl<VPBlockBase *>::iterator;

  static NodeRef getEntryNode(VPBlockShallowTraversalWrapper<VPBlockBase *> N) {
    return N.getEntry();
  }

  static inline ChildIteratorType child_begin(NodeRef N) {
    return N->getSuccessors().begin();
  }

  static inline ChildIteratorType child_end(NodeRef N) {
    return N->getSuccessors().end();
  }
};

template <>
struct GraphTraits<VPBlockShallowTraversalWrapper<const VPBlockBase *>> {
  using NodeRef = const VPBlockBase *;
  using ChildIteratorType = SmallVectorImpl<VPBlockBase *>::const_iterator;

  static NodeRef
  getEntryNode(VPBlockShallowTraversalWrapper<const VPBlockBase *> N) {
    return N.getEntry();
  }

  static inline ChildIteratorType child_begin(NodeRef N) {
    return N->getSuccessors().begin();
  }

  static inline ChildIteratorType child_end(NodeRef N) {
    return N->getSuccessors().end();
  }
};

/// Returns an iterator range to traverse the graph starting at \p G in
/// depth-first order. The iterator won't traverse through region blocks.
inline iterator_range<
    df_iterator<VPBlockShallowTraversalWrapper<VPBlockBase *>>>
vp_depth_first_shallow(VPBlockBase *G) {
  return depth_first(VPBlockShallowTraversalWrapper<VPBlockBase *>(G));
}
inline iterator_range<
    df_iterator<VPBlockShallowTraversalWrapper<const VPBlockBase *>>>
vp_depth_first_shallow(const VPBlockBase *G) {
  return depth_first(VPBlockShallowTraversalWrapper<const VPBlockBase *>(G));
}

/// Returns an iterator range to traverse the graph starting at \p G in
/// post order. The iterator won't traverse through region blocks.
inline iterator_range<
    po_iterator<VPBlockShallowTraversalWrapper<VPBlockBase *>>>
vp_post_order_shallow(VPBlockBase *G) {
  return post_order(VPBlockShallowTraversalWrapper<VPBlockBase *>(G));
}

/// Returns an iterator range to traverse the graph starting at \p G in
/// post order while traversing through region blocks.
inline iterator_range<po_iterator<VPBlockDeepTraversalWrapper<VPBlockBase *>>>
vp_post_order_deep(VPBlockBase *G) {
  return post_order(VPBlockDeepTraversalWrapper<VPBlockBase *>(G));
}

/// Returns an iterator range to traverse the graph starting at \p G in
/// depth-first order while traversing through region blocks.
inline iterator_range<df_iterator<VPBlockDeepTraversalWrapper<VPBlockBase *>>>
vp_depth_first_deep(VPBlockBase *G) {
  return depth_first(VPBlockDeepTraversalWrapper<VPBlockBase *>(G));
}
inline iterator_range<
    df_iterator<VPBlockDeepTraversalWrapper<const VPBlockBase *>>>
vp_depth_first_deep(const VPBlockBase *G) {
  return depth_first(VPBlockDeepTraversalWrapper<const VPBlockBase *>(G));
}

// Specialize PointerLikeTypeTraits for VPBlockWithRegion.
template <typename BlockTy>
struct PointerLikeTypeTraits<VPBlockWithRegion<BlockTy>> {
  using Wrapped = VPBlockWithRegion<BlockTy>;
  using PtrTraits = PointerLikeTypeTraits<BlockTy>;

  static void *getAsVoidPointer(const Wrapped &P) {
    return const_cast<void *>(PtrTraits::getAsVoidPointer(P.Block));
  }
  static Wrapped getFromVoidPointer(void *P) {
    return {PtrTraits::getFromVoidPointer(P), nullptr};
  }
  static constexpr int NumLowBitsAvailable = PtrTraits::NumLowBitsAvailable;
};

// The following set of template specializations implement GraphTraits to treat
// any VPBlockBase as a node in a graph of VPBlockBases. It's important to note
// that VPBlockBase traits don't recurse into VPRegioBlocks, i.e., if the
// VPBlockBase is a VPRegionBlock, this specialization provides access to its
// successors/predecessors but not to the blocks inside the region.

template <> struct GraphTraits<VPBlockBase *> {
  using NodeRef = VPBlockBase *;
  using ChildIteratorType = VPAllSuccessorsIterator<VPBlockBase *>;

  static NodeRef getEntryNode(NodeRef N) { return N; }

  static inline ChildIteratorType child_begin(NodeRef N) {
    return ChildIteratorType(N);
  }

  static inline ChildIteratorType child_end(NodeRef N) {
    return ChildIteratorType::end(N);
  }
};

template <> struct GraphTraits<const VPBlockBase *> {
  using NodeRef = const VPBlockBase *;
  using ChildIteratorType = VPAllSuccessorsIterator<const VPBlockBase *>;

  static NodeRef getEntryNode(NodeRef N) { return N; }

  static inline ChildIteratorType child_begin(NodeRef N) {
    return ChildIteratorType(N);
  }

  static inline ChildIteratorType child_end(NodeRef N) {
    return ChildIteratorType::end(N);
  }
};

/// Inverse graph traits are not implemented yet.
/// TODO: Implement a version of VPBlockNonRecursiveTraversalWrapper to traverse
/// predecessors recursively through regions.
template <> struct GraphTraits<Inverse<VPBlockBase *>> {
  using NodeRef = VPBlockBase *;
  using ChildIteratorType = SmallVectorImpl<VPBlockBase *>::iterator;

  static NodeRef getEntryNode(Inverse<NodeRef> B) {
    llvm_unreachable("not implemented");
  }

  static inline ChildIteratorType child_begin(NodeRef N) {
    llvm_unreachable("not implemented");
  }

  static inline ChildIteratorType child_end(NodeRef N) {
    llvm_unreachable("not implemented");
  }
};

template <> struct GraphTraits<VPlan *> {
  using GraphRef = VPlan *;
  using NodeRef = VPBlockBase *;
  using nodes_iterator = df_iterator<NodeRef>;

  static NodeRef getEntryNode(GraphRef N) { return N->getEntry(); }

  static nodes_iterator nodes_begin(GraphRef N) {
    return nodes_iterator::begin(N->getEntry());
  }

  static nodes_iterator nodes_end(GraphRef N) {
    // df_iterator::end() returns an empty iterator so the node used doesn't
    // matter.
    return nodes_iterator::end(N->getEntry());
  }
};

} // namespace llvm

#endif // LLVM_TRANSFORMS_VECTORIZE_VPLANCFG_H
