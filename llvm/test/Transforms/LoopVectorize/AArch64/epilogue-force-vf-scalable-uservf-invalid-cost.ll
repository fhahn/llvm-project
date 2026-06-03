; RUN: opt -passes=loop-vectorize -mtriple=aarch64 -mattr=+sve \
; RUN:   -enable-epilogue-vectorization -epilogue-vectorization-force-VF=2 -S %s \
; RUN:   | FileCheck %s

target triple = "aarch64-unknown-linux-gnu"

; Regression test for an assertion in computeBestVF. With a scalable UserVF
; (loop hint width=2 + scalable.enable) and a forced epilogue VF, the call to
; llvm.sin.f32 has no scalable mapping, so the cost of the UserVF plan is
; invalid. The planner then clears its plans and rebuilds the generic candidate
; plans. The fast path for the forced epilogue VF previously fired solely
; because a generic plan happened to cover the scalable UserVF, asserting that
; exactly two plans were built. computeBestVF now only takes the fast path when
; the expected two-plan forced-epilogue state is actually present and otherwise
; falls back to the normal cost-based selection.

; CHECK-LABEL: define void @vec_sin_no_scalable_mapping(
; CHECK:       vector.body:
; CHECK:         call fast <2 x float> @llvm.sin.v2f32(
define void @vec_sin_no_scalable_mapping(ptr noalias nocapture %dst, ptr noalias nocapture readonly %src, i64 %n) {
entry:
  br label %for.body

for.body:
  %i.07 = phi i64 [ %inc, %for.body ], [ 0, %entry ]
  %arrayidx = getelementptr inbounds float, ptr %src, i64 %i.07
  %0 = load float, ptr %arrayidx, align 4
  %1 = tail call fast float @llvm.sin.f32(float %0)
  %arrayidx1 = getelementptr inbounds float, ptr %dst, i64 %i.07
  store float %1, ptr %arrayidx1, align 4
  %inc = add nuw nsw i64 %i.07, 1
  %exitcond.not = icmp eq i64 %inc, %n
  br i1 %exitcond.not, label %for.cond.cleanup, label %for.body, !llvm.loop !1

for.cond.cleanup:
  ret void
}

declare float @llvm.sin.f32(float)

!1 = distinct !{!1, !2, !3}
!2 = !{!"llvm.loop.vectorize.width", i32 2}
!3 = !{!"llvm.loop.vectorize.scalable.enable", i1 true}
