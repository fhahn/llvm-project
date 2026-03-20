//===- RuntimeCheckExpansion.h - Expand runtime memory checks ---*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
/// \file
/// Shared runtime memory check generation, parameterized over a builder type.
//===----------------------------------------------------------------------===//

#ifndef LLVM_TRANSFORMS_UTILS_RUNTIMECHECKEXPANSION_H
#define LLVM_TRANSFORMS_UTILS_RUNTIMECHECKEXPANSION_H

#include "llvm/Analysis/LoopAccessAnalysis.h"
#include "llvm/Support/Debug.h"
#include "llvm/Transforms/Utils/LoopUtils.h"

namespace llvm {

/// Generate runtime memory overlap checks for \p Checks using \p Builder.
/// BuilderT must provide: VT (value type), expandSCEV, createFreeze,
/// createICmp, createAnd, createOr, getZero. The create methods take a
/// DebugLoc parameter after the operands (matching VPBuilder's convention).
template <typename BuilderT>
typename BuilderT::VT
generateRuntimeChecks(BuilderT &Builder, DebugLoc DL,
                      const SmallVectorImpl<RuntimePointerCheck> &Checks,
                      Loop *TheLoop, ScalarEvolution &SE,
                      bool HoistRuntimeChecks) {
  using VT = typename BuilderT::VT;
  struct Bounds {
    VT Start, End, Stride;
  };
  DenseMap<const RuntimeCheckingPtrGroup *, Bounds> Cache;

  auto expandBounds = [&](const RuntimeCheckingPtrGroup *CG) {
    auto [It, Inserted] = Cache.try_emplace(CG);
    if (!Inserted)
      return;
    DEBUG_WITH_TYPE("loop-utils",
                    dbgs() << "LAA: Adding RT check for range:\n");
    AdjustedBounds Adj =
        adjustBoundsForHoisting(CG, TheLoop, SE, HoistRuntimeChecks);
    Type *PtrTy = PointerType::get(SE.getContext(), CG->AddressSpace);
    VT Start = Builder.expandSCEV(Adj.Low, PtrTy);
    VT End = Builder.expandSCEV(Adj.High, PtrTy);
    if (CG->NeedsFreeze) {
      Start = Builder.createFreeze(Start, DL);
      End = Builder.createFreeze(End, DL);
    }
    VT Stride{};
    if (Adj.Stride)
      Stride = Builder.expandSCEV(Adj.Stride, Adj.Stride->getType());
    DEBUG_WITH_TYPE("loop-utils",
                    dbgs() << "Start: " << *Adj.Low << " End: " << *Adj.High
                           << "\n");
    It->second = {Start, End, Stride};
  };

  for (const auto &[First, Second] : Checks) {
    expandBounds(First);
    expandBounds(Second);
  }

  VT MemRuntimeCheck{};
  for (const auto &[First, Second] : Checks) {
    const auto &A = Cache[First], &B = Cache[Second];
    VT IsConflict = Builder.createAnd(
        Builder.createICmp(ICmpInst::ICMP_ULT, A.Start, B.End, DL, "bound0"),
        Builder.createICmp(ICmpInst::ICMP_ULT, B.Start, A.End, DL, "bound1"),
        DL, "found.conflict");
    for (VT Stride : {A.Stride, B.Stride})
      if (Stride)
        IsConflict = Builder.createOr(
            IsConflict,
            Builder.createICmp(ICmpInst::ICMP_SLT, Stride,
                               Builder.getZero(Stride), DL, "stride.check"),
            DL, "");
    if (MemRuntimeCheck)
      IsConflict =
          Builder.createOr(MemRuntimeCheck, IsConflict, DL, "conflict.rdx");
    MemRuntimeCheck = IsConflict;
  }
  return MemRuntimeCheck;
}

} // namespace llvm

#endif // LLVM_TRANSFORMS_UTILS_RUNTIMECHECKEXPANSION_H
