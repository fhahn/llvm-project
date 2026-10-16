; RUN: llc -mtriple=aarch64-linux-gnu -mattr=+sve < %s | FileCheck %s

; Splitting a lane mask whose lane count exceeds the range of the operand type
; currently asserts computing the high half's start in the unpromoted type.

define <512 x i1> @lane_mask_v512i1_i8(i8 %index, i8 %TC) {
  %active.lane.mask = call <512 x i1> @llvm.get.active.lane.mask.v512i1.i8(i8 %index, i8 %TC)
  ret <512 x i1> %active.lane.mask
}

define <vscale x 32 x i1> @lane_mask_nxv32i1_i1(i1 %index, i1 %TC) {
  %active.lane.mask = call <vscale x 32 x i1> @llvm.get.active.lane.mask.nxv32i1.i1(i1 %index, i1 %TC)
  ret <vscale x 32 x i1> %active.lane.mask
}
