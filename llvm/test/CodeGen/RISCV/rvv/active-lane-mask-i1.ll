; RUN: llc -mtriple=riscv64 -mattr=+m,+v -verify-machineinstrs < %s | FileCheck %s

; The step vector for a lane mask with an i1 operand is currently built as an
; i1 step_vector, which cannot be selected (and wraps for more than 2 lanes).

define <vscale x 4 x i1> @get_lane_mask_i1(i1 %index, i1 %tc) {
  %mask = call <vscale x 4 x i1> @llvm.get.active.lane.mask.nxv4i1.i1(i1 %index, i1 %tc)
  ret <vscale x 4 x i1> %mask
}
