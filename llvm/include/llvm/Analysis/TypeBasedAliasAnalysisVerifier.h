//===- TypeBasedAliasAnalysis.h - Type-Based Alias Analysis -----*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
/// \file
/// This is the interface for a metadata-based TBAA. See the source file for
/// details on the algorithm.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_ANALYSIS_TYPEBASEDALIASANALYSISVERIFIER_H
#define LLVM_ANALYSIS_TYPEBASEDALIASANALYSISVERIFIER_H

#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/IR/PassManager.h"
#include "llvm/Pass.h"
#include <memory>

namespace llvm {

class CallBase;
class Function;
class MDNode;
class MemoryLocation;

/// Analysis pass providing a never-invalidated alias analysis result.
class TypeBasedAAVerifierPass
    : public AnalysisInfoMixin<TypeBasedAAVerifierPass> {
  friend AnalysisInfoMixin<TypeBasedAAVerifierPass>;

  static AnalysisKey Key;

public:
  PreservedAnalyses run(Function &F, FunctionAnalysisManager &AM);
};

//===--------------------------------------------------------------------===//
//
//
FunctionPass *createTypeBasedAAVerifierLegacyPass();

} // end namespace llvm

#endif // LLVM_ANALYSIS_TYPEBASEDALIASANALYSISVERIFIER_H
