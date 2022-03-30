//===- TypeBasedAliasAnalysisVerifier.cpp - TBAA Verifier -----------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
//===----------------------------------------------------------------------===//

#include "llvm/Analysis/TypeBasedAliasAnalysisVerifier.h"
#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/Analysis/MemoryLocation.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/Analysis/ScalarEvolutionExpressions.h"
#include "llvm/Analysis/TargetLibraryInfo.h"
#include "llvm/Analysis/TypeBasedAliasAnalysis.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DebugLoc.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Metadata.h"

#include "llvm/InitializePasses.h"
#include "llvm/Pass.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/ErrorHandling.h"
#include <cassert>
#include <cstdint>

using namespace llvm;

extern cl::opt<bool> EnableTBAA;

static void verifyTBAA(Function &F, DominatorTree &DT, ScalarEvolution &SE,
                       AAResults &AA) {
  EnableTBAA = true;

  SmallVector<Instruction *> MemoryInsts;
  for (Instruction &I : instructions(F)) {
    if (auto *LI = dyn_cast<LoadInst>(&I))
      MemoryInsts.push_back(LI);
    if (auto *SI = dyn_cast<StoreInst>(&I))
      MemoryInsts.push_back(SI);
  }

  for (unsigned I = 0; I < MemoryInsts.size(); I++) {
    for (unsigned J = I + 1; J < MemoryInsts.size(); J++) {
      auto *Src = MemoryInsts[I];
      auto *Dst = MemoryInsts[J];
      auto L1 = MemoryLocation::get(Src);
      auto L2 = MemoryLocation::get(Dst);

      if (DT.dominates(Dst, Src))
        std::swap(Src, Dst);
      else if (!DT.dominates(Src, Dst))
        continue;

      if (AA.alias(L1, L2) != AliasResult::NoAlias)
        continue;

      auto CheckPointers = [&SE, Src, Dst, &F](const SCEV *Ptr1Expr,
                                               const SCEV *Ptr2Expr) {
        if (SE.getPointerBase(Ptr1Expr) != SE.getPointerBase(Ptr2Expr))
          return false;

        const SCEV *Off1 = SE.removePointerBase(Ptr1Expr);
        const SCEV *Off2 = SE.removePointerBase(Ptr2Expr);
        if (Off1 == Off2) {
          dbgs() << "Possible strict aliasing violation in " << F.getName()
                 << " between accesses on ";
          Src->getDebugLoc().dump();
          dbgs() << " and ";
          Dst->getDebugLoc().dump();
          dbgs() << "\n";
          return true;

          // report_fatal_error("Strict aliasing violation");
        }

        auto *Ptr2AddRec = dyn_cast<SCEVAddRecExpr>(Ptr2Expr);
        if (Ptr2AddRec &&
            SE.isKnownPredicate(CmpInst::ICMP_EQ, Ptr2AddRec->getStart(),
                                Ptr1Expr)) {

          dbgs() << "Possible strict aliasing violation in " << F.getName()
                 << " between accesses on ";
          Src->getDebugLoc().dump();
          dbgs() << " and ";
          Dst->getDebugLoc().dump();
          dbgs() << "\n";
          return true;
        }
        return false;
      };

      const SCEV *Ptr1Expr = SE.getSCEV(const_cast<Value *>(L1.Ptr));
      Value *Ptr2 = const_cast<Value *>(L2.Ptr);
      const SCEV *Ptr2Expr = SE.getSCEV(Ptr2);
      if (CheckPointers(Ptr1Expr, Ptr2Expr))
        break;
      if (auto *Unknown = dyn_cast<SCEVUnknown>(Ptr2Expr)) {
        if (auto *PN = dyn_cast<PHINode>(Unknown->getValue())) {
          if (any_of(PN->incoming_values(),
                     [&SE, Ptr1Expr, CheckPointers](Value *Inc) {
                       return CheckPointers(Ptr1Expr, SE.getSCEV(Inc));
                     }))
            break;
        }
      }
    }
  }

  EnableTBAA = false;
}

PreservedAnalyses TypeBasedAAVerifierPass::run(Function &F,
                                               FunctionAnalysisManager &AM) {
  auto &TBAA = AM.getResult<TypeBasedAA>(F);
  auto &DT = AM.getResult<DominatorTreeAnalysis>(F);
  auto &SE = AM.getResult<ScalarEvolutionAnalysis>(F);
  auto &TLI = AM.getResult<TargetLibraryAnalysis>(F);

  AAResults AA(TLI);
  AA.addAAResult(TBAA);
  verifyTBAA(F, DT, SE, AA);
  return PreservedAnalyses::all();
}

namespace {

/// Legacy wrapper pass to provide the TypeBasedAAResult object.
struct TypeBasedAAVerifierLegacyPass : public FunctionPass {
  static char ID;

  TypeBasedAAVerifierLegacyPass() : FunctionPass(ID) {
    initializeTypeBasedAAVerifierLegacyPassPass(
        *PassRegistry::getPassRegistry());
  }

  bool runOnFunction(Function &F) override { return false; }

  void getAnalysisUsage(AnalysisUsage &AU) const override {}
};

} // namespace

char TypeBasedAAVerifierLegacyPass::ID = 0;
INITIALIZE_PASS_BEGIN(TypeBasedAAVerifierLegacyPass, "tbaa-verify",
                      "Type-Based Alias Analysis Verifier", false, false)
INITIALIZE_PASS_END(TypeBasedAAVerifierLegacyPass, "tbaa-verify",
                    "Type-Based Alias Analysis Verifier", false, false)

FunctionPass *llvm::createTypeBasedAAVerifierLegacyPass() {
  return new TypeBasedAAVerifierLegacyPass();
}
