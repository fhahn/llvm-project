; RUN: llc < %s -mtriple=x86_64 -mattr=+avx512f | FileCheck %s

; With a legal vNi1 mask but a step vector that needs widening, expanding
; GET_ACTIVE_LANE_MASK currently asserts "Step vector was widened".

define <2 x i1> @v2i1(<2 x i1> %m) {
  %x = call <2 x i1> @llvm.mask.beforefirst(<2 x i1> %m)
  ret <2 x i1> %x
}

define <4 x i1> @v4i1(<4 x i1> %m) {
  %x = call <4 x i1> @llvm.mask.beforefirst(<4 x i1> %m)
  ret <4 x i1> %x
}

define <8 x i1> @v8i1(<8 x i1> %m) {
  %x = call <8 x i1> @llvm.mask.beforefirst(<8 x i1> %m)
  ret <8 x i1> %x
}
