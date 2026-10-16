; RUN: llc -mtriple=aarch64-linux-gnu -mattr=+sve < %s | FileCheck %s

; A scalable lane mask with an i128 operand is currently expanded in
; SelectionDAGBuilder via an illegal scalable i128 vector, which fails with
; "scalarization of scalable vectors is not supported".

define <vscale x 4 x i1> @lane_mask_nxv4i1_i128(i128 %index, i128 %TC) {
  %active.lane.mask = call <vscale x 4 x i1> @llvm.get.active.lane.mask.nxv4i1.i128(i128 %index, i128 %TC)
  ret <vscale x 4 x i1> %active.lane.mask
}
