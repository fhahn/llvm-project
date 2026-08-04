; RUN: opt -p loop-vectorize -force-vector-width=2 -force-vector-interleave=1 -S %s | FileCheck %s

target datalayout = "e-m:o-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-n32:64-S128-Fn32"
target triple = "arm64-apple-macosx"

; The address of a reverse interleave group points at the end of the accessed
; range, so the group must not be narrowed to a single wide access using it as
; the base address.
define void @reverse_interleave_group_not_narrowed(ptr noalias %data) {
; CHECK-LABEL: define void @reverse_interleave_group_not_narrowed(
; CHECK:       [[VECTOR_BODY:.*]]:
; CHECK:         [[WIDE_VEC:%.*]] = load <4 x i64>, ptr {{.*}}, align 8
; CHECK:         shufflevector <4 x i64> [[WIDE_VEC]], <4 x i64> poison, <2 x i32> <i32 0, i32 2>
; CHECK:         store <4 x i64> {{.*}}, align 8
;
entry:
  br label %loop

loop:
  %iv = phi i64 [ 99, %entry ], [ %iv.next, %loop ]
  %mul.2 = shl nsw i64 %iv, 1
  %data.0 = getelementptr inbounds i64, ptr %data, i64 %mul.2
  %l.0 = load i64, ptr %data.0, align 8
  %data.1 = getelementptr inbounds i64, ptr %data.0, i64 1
  %l.1 = load i64, ptr %data.1, align 8
  %a.0 = add i64 %l.0, 10
  %a.1 = add i64 %l.1, 20
  store i64 %a.0, ptr %data.0, align 8
  store i64 %a.1, ptr %data.1, align 8
  %iv.next = add nsw i64 %iv, -1
  %ec = icmp eq i64 %iv.next, -1
  br i1 %ec, label %exit, label %loop

exit:
  ret void
}
