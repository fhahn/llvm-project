//===- LoopRemarkExtractor.cpp - Loop Remark Extraction ---------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This pass extracts loops into remarks.
//
//===----------------------------------------------------------------------===//

#include "llvm/Analysis/AssumptionCache.h"
#include "llvm/Analysis/BlockFrequencyInfo.h"
#include "llvm/Analysis/BranchProbabilityInfo.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/OptimizationRemarkEmitter.h"
#include "llvm/Analysis/ScalarEvolutionExpressions.h"
#include "llvm/Analysis/TargetLibraryInfo.h"
#include "llvm/Analysis/TargetTransformInfo.h"
#include "llvm/CodeGen/PseudoSourceValue.h"
#include "llvm/IR/DebugInfo.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/PassInstrumentation.h"
#include "llvm/Support/Debug.h"
#include "llvm/Transforms/IPO.h"
#include "llvm/Transforms/IPO/ExtractGV.h"
#include "llvm/Transforms/IPO/LoopRemarkExtractor.h"
#include "llvm/Transforms/IPO/StripDeadPrototypes.h"
#include "llvm/Transforms/IPO/StripSymbols.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include "llvm/Transforms/Utils/CodeExtractor.h"
#include "llvm/Transforms/Utils/LoopSimplify.h"
#include <cassert>

using namespace llvm;

#define DEBUG_TYPE "loop-remark-extract"

namespace {
struct LoopExtractionAnalyzer {
  explicit LoopExtractionAnalyzer() {}

  bool runOnModule(Module &M);
  bool runOnFunction(Function &F);

private:
  bool runOnFunctionImpl(Function &F);

  Function *extractLoop(Loop *L, LoopInfo &LI, DominatorTree &DT);
  SmallVector<Function *, 16> ExtractedLoopFuncs;
};
} // namespace

Function *LoopExtractionAnalyzer::extractLoop(Loop *L, LoopInfo &LI,
                                              DominatorTree &DT) {
  Function &Func = *L->getHeader()->getParent();
  CodeExtractorAnalysisCache CEAC(Func);
  /*BranchProbabilityInfo BPI(Func, LI);*/
  /*BlockFrequencyInfo BFI(Func, BPI, LI);*/
  CodeExtractor Extractor(L->getBlocks(), &DT, false, nullptr, nullptr, nullptr);

  return Extractor.extractCodeRegion(CEAC);
}

bool LoopExtractionAnalyzer::runOnFunctionImpl(Function &F) {
  size_t NumSimplified = 0;
  size_t NumIsNotSimplified = 0;
  size_t NumExtracted = 0;
  size_t NumNotExtracted = 0;

  if (F.empty())
    return false;

  DominatorTree DT(F);
  LoopInfo LI(DT);
  if (LI.empty())
    return false;

  AssumptionCache AC(F);
  TargetLibraryInfoImpl TLII(F.getParent()->getTargetTriple());
  TargetLibraryInfo TLI(TLII);
  ScalarEvolution SE(F, TLI, AC, DT, LI);
  OptimizationRemarkEmitter ORE(&F);

  for (Loop *L : LI) {
    assert(!L->getParentLoop());

    bool Simplified = simplifyLoop(L, &DT, &LI, &SE, &AC, nullptr, false);
    if (Simplified) {
      ++NumSimplified;
      LLVM_DEBUG(dbgs() << "Simplified loop!\n");
    }
    LLVM_DEBUG(dbgs() << "Loop dump in func "
                      << L->getHeader()->getParent()->getName() << ":\n");
    LLVM_DEBUG(L->dump());
    if (!L->isLoopSimplifyForm()) {
      LLVM_DEBUG(errs() << "Loop is not in Loop Simply Form!\n");
      ++NumIsNotSimplified;
    }
    if (Function *ExtractedFunc = extractLoop(L, LI, DT)) {
      LLVM_DEBUG(dbgs() << "Loop was extracted\n");
      ExtractedLoopFuncs.push_back(ExtractedFunc);
      NumExtracted++;
      LLVM_DEBUG(dbgs() << "Extracted Function: " << ExtractedFunc->getName()
                        << "\n");
    } else {
      LLVM_DEBUG(dbgs() << "Loop could not be extracted!!\n");
      NumNotExtracted++;
    }
  }

  ORE.emit([&]() {
    using namespace ore;
    return OptimizationRemarkAnalysis(DEBUG_TYPE, "FuncStats", &F)
           << NV("NumExtracted", NumExtracted)
           << NV("NumNotExtracted", NumNotExtracted)
           << NV("NumSimplified", NumSimplified)
           << NV("NumIsNotSimplified", NumIsNotSimplified);
  });

  return false;
}

bool LoopExtractionAnalyzer::runOnFunction(Function &F) {
  if (F.empty())
    return false;
  if (!F.getParent())
    return false;

  auto ShouldCloneDef = [&](const GlobalValue *GV) {
    if (auto *GVF = dyn_cast<Function>(GV)) {
      if (!GVF->empty() && GVF != &F)
        return false;
    }
    return true;
  };

  ValueToValueMapTy VMap;
  std::unique_ptr<Module> ClonedModPtr = CloneModule(*F.getParent(), VMap, ShouldCloneDef);
  Module &ClonedM = *ClonedModPtr;

  runOnFunctionImpl(*ClonedM.getFunction(F.getName()));

  if (ExtractedLoopFuncs.empty())
    return false;

  std::vector<GlobalValue *> GVs(ExtractedLoopFuncs.begin(),
                                 ExtractedLoopFuncs.end());

  ModulePassManager MPM;
  MPM.addPass(ExtractGVPass(GVs, false));
  MPM.addPass(StripDeadPrototypesPass());
  ModuleAnalysisManager MAM;
  MAM.registerPass([&] { return PassInstrumentationAnalysis(); });
  MPM.run(ClonedM, MAM);

  /*M.getContext().setDiagnosticsHotnessRequested(true);*/

  StripDebugInfo(ClonedM);
  OptimizationRemarkEmitter ORE(&F);
  ORE.emit([&]() {
    return OptimizationRemarkAnalysis(DEBUG_TYPE, "ModuleDump", &F)
           << ore::NV("LoopIR", ClonedModPtr->getName().str(), &ClonedM);
  });

  return false;
}

bool LoopExtractionAnalyzer::runOnModule(Module &M) {
  if (M.empty())
    return false;

  std::unique_ptr<Module> ClonedModPtr = CloneModule(M);
  Module &ClonedM = *ClonedModPtr;

  SmallVector<Function *, 16> OriginalFunctions;
  for (auto &F : ClonedM) {
    OriginalFunctions.push_back(&F);
  }

  for (auto *F : OriginalFunctions) {
    runOnFunctionImpl(*F);
  }

  if (ExtractedLoopFuncs.empty())
    return false;

  std::vector<GlobalValue *> GVs(ExtractedLoopFuncs.begin(),
                                 ExtractedLoopFuncs.end());

  ModulePassManager MPM;
  MPM.addPass(ExtractGVPass(GVs, false));
  MPM.addPass(StripDeadPrototypesPass());
  ModuleAnalysisManager MAM;
  MAM.registerPass([&] { return PassInstrumentationAnalysis(); });
  MPM.run(ClonedM, MAM);

  Function *FirstF = nullptr;
  for (auto &ExtractedF : ClonedM) {
    if (ExtractedF.empty())
      continue;
    FirstF = &ExtractedF;
  }
  if (!FirstF)
    return false;

  /*M.getContext().setDiagnosticsHotnessRequested(true);*/

  StripDebugInfo(ClonedM);
  OptimizationRemarkEmitter ORE(FirstF);
  ORE.emit([&]() {
    std::string ModuleStr;
    raw_string_ostream ModuleStrS(ModuleStr);
    ClonedModPtr->print(ModuleStrS, nullptr);
    return OptimizationRemarkAnalysis(DEBUG_TYPE, "ModuleDump", FirstF)
           << ModuleStr;
  });

  return false;
}

PreservedAnalyses LoopRemarkExtractorPass::run(Module &M,
                                               ModuleAnalysisManager &AM) {
  LoopExtractionAnalyzer().runOnModule(M);

  return PreservedAnalyses::all();
}

PreservedAnalyses LoopRemarkExtractorPass::run(Function &F,
                                               FunctionAnalysisManager &FAM) {
  LoopExtractionAnalyzer().runOnFunction(F);

  return PreservedAnalyses::all();
}
