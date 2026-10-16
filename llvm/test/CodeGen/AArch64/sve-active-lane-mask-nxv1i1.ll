; RUN: llc -mtriple=aarch64-linux-gnu -mattr=+sve < %s | FileCheck %s

; A constant nxv1i1 lane mask is currently combined to a PTRUE, which cannot
; be selected for nxv1i1.

define void @lane_mask_nxv1i1_const(ptr %dst) {
  %active.lane.mask = call <vscale x 1 x i1> @llvm.get.active.lane.mask.nxv1i1.i64(i64 0, i64 1)
  %ext = zext <vscale x 1 x i1> %active.lane.mask to <vscale x 1 x i8>
  store <vscale x 1 x i8> %ext, ptr %dst
  ret void
}
