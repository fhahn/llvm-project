//===- LowerMatrixIntrinsics.h - Lower matrix intrinsics. -------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This pass lowers matrix intrinsics down to vector operations.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TRANSFORMS_SCALAR_LOWERMATRIXINTRINSICS_H
#define LLVM_TRANSFORMS_SCALAR_LOWERMATRIXINTRINSICS_H

#include "llvm/IR/PassManager.h"

namespace llvm {
class LLVMContext;

/// Selects the phases LowerMatrixIntrinsicsPass runs.
struct LowerMatrixIntrinsicsOptions {
  /// Sink and lift transposes to fold them away or into multiplies.
  bool TransposeOpts = true;
  /// Fuse multiplies with transposed operands or with loads and stores.
  bool Fuse = true;
  /// Lower the matrix intrinsics to vector operations.
  bool Lower = true;
};

class LowerMatrixIntrinsicsPass
    : public RequiredPassInfoMixin<LowerMatrixIntrinsicsPass> {
  LowerMatrixIntrinsicsOptions Options;

public:
  LowerMatrixIntrinsicsPass(LowerMatrixIntrinsicsOptions Options = {})
      : Options(Options) {}
  LLVM_ABI PreservedAnalyses run(Function &F, FunctionAnalysisManager &AM);
  LLVM_ABI void
  printPipeline(raw_ostream &OS,
                function_ref<StringRef(StringRef)> MapClassName2PassName);
};

/// Returns true if matrices in \p Ctx use column-major layout, as selected by
/// -matrix-default-layout.
LLVM_ABI bool isMatrixLayoutColumnMajor(const LLVMContext &Ctx);
} // namespace llvm

#endif
