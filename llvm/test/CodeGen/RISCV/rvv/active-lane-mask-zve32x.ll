; RUN: llc -mtriple=riscv64 -mattr=+m,+zve32x -verify-machineinstrs < %s | FileCheck %s

; Without 64-bit vector elements, a lane mask with an i64 operand is currently
; expanded via an illegal scalable i64 vector, which fails with "scalarization
; of scalable vectors is not supported".

define <vscale x 4 x i1> @get_lane_mask_i64(i64 %index, i64 %tc) {
  %mask = call <vscale x 4 x i1> @llvm.get.active.lane.mask.nxv4i1.i64(i64 %index, i64 %tc)
  ret <vscale x 4 x i1> %mask
}
