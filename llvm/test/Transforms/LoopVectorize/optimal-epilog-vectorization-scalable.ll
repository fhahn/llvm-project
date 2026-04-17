; REQUIRES: asserts
; RUN: opt < %s -passes='loop-vectorize' -force-target-supports-scalable-vectors=true -enable-epilogue-vectorization -epilogue-vectorization-force-VF=2 --debug-only=loop-vectorize --disable-output -scalable-vectorization=on -force-vector-interleave=1 2>&1 | FileCheck %s

target datalayout = "e-m:e-i64:64-n32:64-v256:256:256-v512:512:512"

; Currently we cannot handle scalable vectorization factors.
; CHECK: LV: Checking a loop in 'f1'
; CHECK: LEV: Epilogue vectorization factor is forced.
; CHECK: Epilogue Loop VF:2, Epilogue Loop UF:1

define void @f1(ptr %A) {
entry:
  br label %for.body

for.body:
  %iv = phi i64 [ 0, %entry ], [ %iv.next, %for.body ]
  %arrayidx = getelementptr inbounds i8, ptr %A, i64 %iv
  store i8 1, ptr %arrayidx, align 1
  %iv.next = add nuw nsw i64 %iv, 1
  %exitcond = icmp ne i64 %iv.next, 1024
  br i1 %exitcond, label %for.body, label %exit, !llvm.loop !0

exit:
  ret void
}

; Verify we handle invalid UserVF properly with forced epilogue vectorization.
; The UserVF should be ignored and the loop should still be vectorized with a
; profitable fixed-width VF.
; CHECK: LV: Checking a loop in 'scalable_uservf_invalid_cost_with_epilogue_vf'
; CHECK: LV: Loop hints: force=? width=vscale x 2 interleave=1
; CHECK: LV: Scalarizing and predicating:  store i32 %val, ptr %gep.b, align 4
; CHECK: Cost for VF vscale x 2: Invalid
; CHECK: LV: UserVF ignored because of invalid costs.
; CHECK: LV: Scalar loop costs: 8.
; CHECK: Cost for VF 2:
; CHECK: Cost for VF vscale x 1: Invalid
; CHECK: Cost for VF vscale x 2: Invalid
; CHECK: LV: Selecting VF: 2.
; CHECK: LV: Found a vectorizable loop (2)
define void @scalable_uservf_invalid_cost_with_epilogue_vf(ptr noalias %A, ptr noalias %B, i64 %N) {
entry:
  br label %loop

loop:
  %iv = phi i64 [ 0, %entry ], [ %iv.next, %loop.latch ]
  %gep.a = getelementptr inbounds i32, ptr %A, i64 %iv
  %val = load i32, ptr %gep.a, align 4
  %cond = icmp sgt i32 %val, 0
  br i1 %cond, label %if.then, label %loop.latch

if.then:
  %gep.b = getelementptr inbounds i32, ptr %B, i64 %iv
  store i32 %val, ptr %gep.b, align 4
  br label %loop.latch

loop.latch:
  %iv.next = add nuw nsw i64 %iv, 1
  %cmp = icmp ult i64 %iv.next, %N
  br i1 %cmp, label %loop, label %exit, !llvm.loop !2

exit:
  ret void
}

!0 = !{!0, !1}
!1 = !{!"llvm.loop.vectorize.scalable.enable", i1 true}
!2 = distinct !{!2, !3, !1}
!3 = !{!"llvm.loop.vectorize.width", i32 2}
