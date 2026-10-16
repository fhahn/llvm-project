; RUN: opt < %s -passes=instcombine -S | FileCheck %s

; Extracting from a lane mask with a constant bound wider than 64 bits
; currently asserts in getZExtValue().

define <4 x i1> @extract_tc_i128(i128 %index) {
  %mask = call <8 x i1> @llvm.get.active.lane.mask.v8i1.i128(i128 %index, i128 18446744073709551620)
  %ext = call <4 x i1> @llvm.vector.extract.v4i1.v8i1(<8 x i1> %mask, i64 4)
  ret <4 x i1> %ext
}
