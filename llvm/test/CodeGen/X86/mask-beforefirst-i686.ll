; RUN: llc < %s -mtriple=i686 -mattr=+sse2 | FileCheck %s

; Expanding GET_ACTIVE_LANE_MASK currently asserts building an all-ones i64
; constant in the i32 vector index type.

define <2 x i1> @v2i1(<2 x i1> %m) {
  %x = call <2 x i1> @llvm.mask.beforefirst(<2 x i1> %m)
  ret <2 x i1> %x
}
