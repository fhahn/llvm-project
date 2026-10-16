; RUN: opt -passes=instsimplify -S < %s | FileCheck %s

; Lanes whose index does not fit the operand type are inactive. Constant
; folding currently asserts building the lane index in the operand width.

define <4 x i1> @v4i1_i1() {
  %mask = call <4 x i1> @llvm.get.active.lane.mask.v4i1.i1(i1 0, i1 1)
  ret <4 x i1> %mask
}

define <512 x i1> @v512i1_i8() {
  %mask = call <512 x i1> @llvm.get.active.lane.mask.v512i1.i8(i8 0, i8 255)
  ret <512 x i1> %mask
}
