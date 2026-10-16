; RUN: llc < %s -mtriple aarch64 -mattr=+sve | FileCheck %s

; nxv1i1 is expanded via CTTZ_ELTS, which currently fails with "Possible
; incorrect use of EVT::getVectorNumElements() for scalable vector".

define <vscale x 1 x i1> @nxv1i1(<vscale x 1 x i1> %m) {
  %x = call <vscale x 1 x i1> @llvm.mask.beforefirst(<vscale x 1 x i1> %m)
  ret <vscale x 1 x i1> %x
}
