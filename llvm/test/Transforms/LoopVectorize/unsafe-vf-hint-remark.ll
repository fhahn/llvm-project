; REQUIRES: asserts
; RUN: opt -passes=loop-vectorize -pass-remarks-analysis=loop-vectorize -debug-only=loop-vectorize -S < %s 2>&1 | FileCheck %s
; RUN: opt -passes=loop-vectorize -enable-epilogue-vectorization -epilogue-vectorization-force-VF=2 -pass-remarks-analysis=loop-vectorize -S < %s 2>&1 | FileCheck --check-prefix=CHECK-EPILOGUE %s

; Make sure the unsafe user specified vectorization factor is clamped.

target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v64:64:64-v128:128:128-a0:0:64-s0:64:64-f80:128:128-n8:16:32:64-S128"

; void foo(int *a, int *b) {
;   #pragma clang loop vectorize(enable) vectorize_width(4)
;   for (int i=0; i < 1024; ++i) {
;     a[i + 2] = a[i] + b[i];
;   }
; }

; CHECK: LV: User VF=4 is unsafe, clamping to max safe VF=2.
; CHECK: remark: <unknown>:0:0: User-specified vectorization factor 4 is unsafe, clamping to maximum safe vectorization factor 2
; CHECK: remark: <unknown>:0:0: UserVF ignored because it may be larger than the maximal safe VF
; CHECK-LABEL: @foo
; CHECK: <2 x i32>
; CHECK-EPILOGUE: remark: <unknown>:0:0: UserVF ignored because it may be larger than the maximal safe VF
; CHECK-EPILOGUE-LABEL: @foo
; CHECK-EPILOGUE: <2 x i32>
define void @foo(ptr %a, ptr %b) {
entry:
  br label %loop.ph

loop.ph:
  br label %loop

loop:
  %iv = phi i64 [ 0, %loop.ph ], [ %iv.next, %loop ]
  %arrayidx = getelementptr inbounds i32, ptr %a, i64 %iv
  %0 = load i32, ptr %arrayidx, align 4
  %arrayidx2 = getelementptr inbounds i32, ptr %b, i64 %iv
  %1 = load i32, ptr %arrayidx2, align 4
  %add = add nsw i32 %1, %0
  %2 = add nuw nsw i64 %iv, 2
  %arrayidx5 = getelementptr inbounds i32, ptr %a, i64 %2
  store i32 %add, ptr %arrayidx5, align 4
  %iv.next = add nuw nsw i64 %iv, 1
  %exitcond.not = icmp eq i64 %iv.next, 1024
  br i1 %exitcond.not, label %exit, label %loop, !llvm.loop !0

exit:
  ret void
}

; The dependence distance is 3 iterations with types whose alloc size is larger
; than their type size.
; FIXME: User VF 4 is unsafe, should be clamped to 2.
; CHECK-LABEL: @foo_i24
; CHECK: <4 x i64>
define void @foo_i24(ptr %a) {
entry:
  br label %loop

loop:
  %iv = phi i64 [ 0, %entry ], [ %iv.next, %loop ]
  %gep = getelementptr inbounds i24, ptr %a, i64 %iv
  %t = trunc i64 %iv to i24
  store i24 %t, ptr %gep, align 4
  %iv.3 = add nuw nsw i64 %iv, 3
  %gep.3 = getelementptr inbounds i24, ptr %a, i64 %iv.3
  store i24 %t, ptr %gep.3, align 4
  %iv.next = add nuw nsw i64 %iv, 1
  %exitcond.not = icmp eq i64 %iv.next, 1024
  br i1 %exitcond.not, label %exit, label %loop, !llvm.loop !3

exit:
  ret void
}

; CHECK-LABEL: @foo_x86_fp80
; CHECK: <4 x i64>
define void @foo_x86_fp80(ptr %a) {
entry:
  br label %loop

loop:
  %iv = phi i64 [ 0, %entry ], [ %iv.next, %loop ]
  %gep = getelementptr inbounds x86_fp80, ptr %a, i64 %iv
  %t = uitofp i64 %iv to x86_fp80
  store x86_fp80 %t, ptr %gep, align 16
  %iv.3 = add nuw nsw i64 %iv, 3
  %gep.3 = getelementptr inbounds x86_fp80, ptr %a, i64 %iv.3
  store x86_fp80 %t, ptr %gep.3, align 16
  %iv.next = add nuw nsw i64 %iv, 1
  %exitcond.not = icmp eq i64 %iv.next, 1024
  br i1 %exitcond.not, label %exit, label %loop, !llvm.loop !4

exit:
  ret void
}

!0 = !{!0, !1, !2}
!1 = !{!"llvm.loop.vectorize.width", i32 4}
!2 = !{!"llvm.loop.vectorize.enable"}
!3 = !{!3, !1, !2}
!4 = !{!4, !1, !2}
