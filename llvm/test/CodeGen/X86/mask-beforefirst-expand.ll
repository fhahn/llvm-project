; RUN: llc < %s -mtriple=x86_64 | FileCheck %s --check-prefix=SSE2
; RUN: llc < %s -mtriple=x86_64 -mattr=+avx2 | FileCheck %s --check-prefix=AVX2

; Expanding MASK_BEFOREFIRST via GET_ACTIVE_LANE_MASK after type legalization
; currently splats the i64 end into a narrower vector, which crashes isel.

define <4 x i1> @v4i1(<4 x i1> %m) {
  %x = call <4 x i1> @llvm.mask.beforefirst(<4 x i1> %m)
  ret <4 x i1> %x
}

define <8 x i1> @v8i1(<8 x i1> %m) {
  %x = call <8 x i1> @llvm.mask.beforefirst(<8 x i1> %m)
  ret <8 x i1> %x
}

define <32 x i1> @v32i1(<32 x i1> %m) {
  %x = call <32 x i1> @llvm.mask.beforefirst(<32 x i1> %m)
  ret <32 x i1> %x
}
