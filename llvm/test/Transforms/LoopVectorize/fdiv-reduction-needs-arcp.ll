; RUN: opt -passes=loop-vectorize -force-vector-width=2 -force-vector-interleave=1 -S %s | FileCheck %s

; An fdiv recurrence is lowered as an FMul recurrence, i.e. the partial results
; are combined with fmul starting from 1.0, which turns each fdiv into a
; multiplication by 1.0 / x. That needs arcp, so reassoc alone must not be
; enough. For x = 1e-320, 1.0 / x overflows to +Inf while the division does not.
define double @fdiv_reduction_reassoc_only(ptr %x, double %s, i64 %n) {
; CHECK-LABEL: define double @fdiv_reduction_reassoc_only(
; CHECK-NOT:     vector.body
;
entry:
  br label %loop

loop:
  %iv = phi i64 [ 0, %entry ], [ %iv.next, %loop ]
  %r = phi double [ %s, %entry ], [ %d, %loop ]
  %g = getelementptr inbounds double, ptr %x, i64 %iv
  %v = load double, ptr %g, align 8
  %d = fdiv reassoc double %r, %v
  %iv.next = add i64 %iv, 1
  %ec = icmp eq i64 %iv.next, %n
  br i1 %ec, label %exit, label %loop

exit:
  ret double %d
}

; With arcp the reciprocal is licensed and the reduction is vectorized.
define double @fdiv_reduction_reassoc_arcp(ptr %x, double %s, i64 %n) {
; CHECK-LABEL: define double @fdiv_reduction_reassoc_arcp(
; CHECK:         vector.body
; CHECK:         call reassoc arcp double @llvm.vector.reduce.fmul.v2f64(
;
entry:
  br label %loop

loop:
  %iv = phi i64 [ 0, %entry ], [ %iv.next, %loop ]
  %r = phi double [ %s, %entry ], [ %d, %loop ]
  %g = getelementptr inbounds double, ptr %x, i64 %iv
  %v = load double, ptr %g, align 8
  %d = fdiv reassoc arcp double %r, %v
  %iv.next = add i64 %iv, 1
  %ec = icmp eq i64 %iv.next, %n
  br i1 %ec, label %exit, label %loop

exit:
  ret double %d
}
