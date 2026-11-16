//===- CFGUpdate.h - Encode a CFG Edge Update. ------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file defines a CFG Edge Update: Insert or Delete, and two Nodes as the
// Edge ends.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_SUPPORT_CFGUPDATE_H
#define LLVM_SUPPORT_CFGUPDATE_H

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/PointerIntPair.h"
#include "llvm/Support/Compiler.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

namespace llvm {
namespace cfg {
enum class UpdateKind : unsigned char { Insert, Delete };

template <typename NodePtr> class Update {
  using NodeKindPair = PointerIntPair<NodePtr, 1, UpdateKind>;
  NodePtr From;
  NodeKindPair ToAndKind;

public:
  Update(UpdateKind Kind, NodePtr From, NodePtr To)
      : From(From), ToAndKind(To, Kind) {}

  UpdateKind getKind() const { return ToAndKind.getInt(); }
  NodePtr getFrom() const { return From; }
  NodePtr getTo() const { return ToAndKind.getPointer(); }
  bool operator==(const Update &RHS) const {
    return From == RHS.From && ToAndKind == RHS.ToAndKind;
  }

  void print(raw_ostream &OS) const {
    OS << (getKind() == UpdateKind::Insert ? "Insert " : "Delete ");
    getFrom()->printAsOperand(OS, false);
    OS << " -> ";
    getTo()->printAsOperand(OS, false);
  }

#if !defined(NDEBUG) || defined(LLVM_ENABLE_DUMP)
  LLVM_DUMP_METHOD void dump() const { print(dbgs()); }
#endif
};

// LegalizeUpdates function simplifies updates assuming a graph structure.
// This function serves double purpose:
// a) It removes redundant updates, which makes it easier to reverse-apply
//    them when traversing CFG.
// b) It optimizes away updates that cancel each other out, as the end result
//    is the same.
template <typename NodePtr>
void LegalizeUpdates(ArrayRef<Update<NodePtr>> AllUpdates,
                     SmallVectorImpl<Update<NodePtr>> &Result,
                     bool InverseGraph, bool ReverseResultOrder = false) {
  // Count the total number of inserions of each edge.
  // Each insertion adds 1 and deletion subtracts 1. The end number should be
  // one of {-1 (deletion), 0 (NOP), +1 (insertion)}. Otherwise, the sequence
  // of updates contains multiple updates of the same kind and we assert for
  // that case.
  SmallDenseMap<std::pair<NodePtr, NodePtr>, int, 4> Operations;
  Operations.reserve(AllUpdates.size());

  for (const auto &U : AllUpdates) {
    NodePtr From = U.getFrom();
    NodePtr To = U.getTo();
    if (InverseGraph)
      std::swap(From, To); // Reverse edge for postdominators.

    Operations[{From, To}] += (U.getKind() == UpdateKind::Insert ? 1 : -1);
  }

  // Emit each edge at its last update, which makes the order consistent
  // without relying on pointer values within the set. Zeroing the count skips
  // the edge's earlier updates.
  // In the future, we should order by something else to minimize the amount
  // of work needed to perform the series of updates.
  Result.clear();
  Result.reserve(Operations.size());
  for (const auto &U : reverse(AllUpdates)) {
    NodePtr From = U.getFrom();
    NodePtr To = U.getTo();
    if (InverseGraph)
      std::swap(From, To); // Reverse edge for postdominators.

    const int NumInsertions = std::exchange(Operations[{From, To}], 0);
    assert(std::abs(NumInsertions) <= 1 && "Unbalanced operations!");
    if (NumInsertions == 0)
      continue;
    const UpdateKind UK =
        NumInsertions > 0 ? UpdateKind::Insert : UpdateKind::Delete;
    Result.push_back({UK, From, To});
  }
  if (ReverseResultOrder)
    std::reverse(Result.begin(), Result.end());
}

} // end namespace cfg
} // end namespace llvm

#endif // LLVM_SUPPORT_CFGUPDATE_H
