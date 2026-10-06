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
// The aligned operators are more expensive on common C++ runtimes (libstdc++
// implements them via posix_memalign), so avoid them where the plain ones
// guarantee Alignment. Those return storage suitable for any object of the
// requested size without new-extended alignment. An object's size is a
// non-zero multiple of its alignment, so this covers Alignment if it is at
// most __STDCPP_DEFAULT_NEW_ALIGNMENT__ and Size is a non-zero multiple of it.
// allocate_buffer and deallocate_buffer make the same choice for the same Size
// and Alignment, so new and delete always pair up.
static bool needsAlignedNew(size_t Size, size_t Alignment) {
  // Alignment is a power of two, as the aligned operators require.
  return Alignment > __STDCPP_DEFAULT_NEW_ALIGNMENT__ || Size == 0 ||
         (Size & (Alignment - 1)) != 0;
}
#endif

static void *allocateNoThrow(size_t Size, size_t Alignment) {
#ifdef __cpp_aligned_new
  if (needsAlignedNew(Size, Alignment))
    return ::operator new(Size, std::align_val_t(Alignment), std::nothrow);
#endif
  return ::operator new(Size, std::nothrow);
}

// These are out of line to have __cpp_aligned_new not affect ABI.

LLVM_ATTRIBUTE_RETURNS_NONNULL LLVM_ATTRIBUTE_RETURNS_NOALIAS void *
llvm::allocate_buffer(size_t Size, size_t Alignment) {
  void *Result = allocateNoThrow(Size, Alignment);
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
