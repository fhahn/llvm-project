; RUN: opt -passes=loop-vectorize -force-vector-width=2 -force-vector-interleave=1 -S %s | FileCheck %s

; Vectorizing an fdiv reduction combines the per-lane partial quotients with
; fmul, replacing divisions by multiplications with reciprocals. That needs arcp,
; reassoc alone is not enough.

define double @fdiv_reduction_reassoc_only(ptr %src, double %start, i64 %n) {
; CHECK-LABEL: define double @fdiv_reduction_reassoc_only(
; CHECK:         vector.body
; CHECK:         call reassoc double @llvm.vector.reduce.fmul.v2f64(
;
entry:
  br label %loop

loop:
  %iv = phi i64 [ 0, %entry ], [ %iv.next, %loop ]
  %red = phi double [ %start, %entry ], [ %red.next, %loop ]
  %gep.src = getelementptr inbounds double, ptr %src, i64 %iv
  %l = load double, ptr %gep.src, align 8
  %red.next = fdiv reassoc double %red, %l
  %iv.next = add i64 %iv, 1
  %ec = icmp eq i64 %iv.next, %n
  br i1 %ec, label %exit, label %loop

exit:
  ret double %red.next
}

define double @fdiv_reduction_reassoc_arcp(ptr %src, double %start, i64 %n) {
; CHECK-LABEL: define double @fdiv_reduction_reassoc_arcp(
; CHECK:         vector.body
; CHECK:         call reassoc arcp double @llvm.vector.reduce.fmul.v2f64(
;
entry:
  br label %loop

loop:
  %iv = phi i64 [ 0, %entry ], [ %iv.next, %loop ]
  %red = phi double [ %start, %entry ], [ %red.next, %loop ]
  %gep.src = getelementptr inbounds double, ptr %src, i64 %iv
  %l = load double, ptr %gep.src, align 8
  %red.next = fdiv reassoc arcp double %red, %l
  %iv.next = add i64 %iv, 1
  %ec = icmp eq i64 %iv.next, %n
  br i1 %ec, label %exit, label %loop

exit:
  ret double %red.next
}
