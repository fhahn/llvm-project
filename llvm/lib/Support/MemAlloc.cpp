//===- MemAlloc.cpp - Memory allocation functions -------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/Support/MemAlloc.h"
#include <new>

#ifdef __cpp_aligned_new
// Plain operator new returns storage aligned for any object of the requested
// size without new-extended alignment, and an object's size is a multiple of
// its alignment. Use it over the slower aligned operator new when that covers
// Alignment. allocate_buffer and deallocate_buffer must make the same choice.
static bool needsAlignedNew(size_t Size, size_t Alignment) {
  return Alignment > __STDCPP_DEFAULT_NEW_ALIGNMENT__ || Size == 0 ||
         (Size & (Alignment - 1)) != 0;
}
#endif

// These are out of line to have __cpp_aligned_new not affect ABI.

LLVM_ATTRIBUTE_RETURNS_NONNULL LLVM_ATTRIBUTE_RETURNS_NOALIAS void *
llvm::allocate_buffer(size_t Size, size_t Alignment) {
#ifdef __cpp_aligned_new
  void *Result =
      needsAlignedNew(Size, Alignment)
          ? ::operator new(Size, std::align_val_t(Alignment), std::nothrow)
          : ::operator new(Size, std::nothrow);
#else
  void *Result = ::operator new(Size, std::nothrow);
#endif
  if (Result == nullptr) {
    report_bad_alloc_error("Buffer allocation failed");
  }
  return Result;
}

void llvm::deallocate_buffer(void *Ptr, size_t Size, size_t Alignment) {
#ifdef __cpp_aligned_new
  if (needsAlignedNew(Size, Alignment)) {
    ::operator delete(Ptr,
#ifdef __cpp_sized_deallocation
                      Size,
#endif
                      std::align_val_t(Alignment));
    return;
  }
#endif
  ::operator delete(Ptr
#ifdef __cpp_sized_deallocation
                    ,
                    Size
#endif
  );
}
