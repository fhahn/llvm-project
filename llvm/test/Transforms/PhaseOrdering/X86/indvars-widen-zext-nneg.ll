; RUN: opt -passes='default<O3>' -S %s | FileCheck %s

target datalayout = "e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-unknown-linux-gnu"

; End-to-end test for IV widening of an IV extended by a `zext nneg`.
;
; Reduced from icu's UVector64::insertElementAt. ConstraintElimination proves
; %iv non-negative from `%iv > %idx` and `%idx >= 0` and rewrites the address
; sext to `zext nneg`. IndVarSimplify must still widen the count-down IV to i64,
; otherwise the vectorizer needs a runtime overflow check for the i32 IV and
; raises the minimum trip count for vectorizing from 4 to 14.
;
; With the wide IV in place ScalarEvolution separates the invariant part of the
; address, and the loop is recognised as the memmove it is.

define void @insert_element_at(i32 %idx, ptr %len_ptr, ptr %data) {
; CHECK-LABEL: define void @insert_element_at(
; CHECK-NOT:     vector.scevcheck
; CHECK-NOT:     vector.body
; CHECK:         call void @llvm.memmove
; CHECK:         ret void
;
entry:
  %idx.nonneg = icmp sle i32 0, %idx
  br i1 %idx.nonneg, label %preheader, label %exit

preheader:
  %len = load i32, ptr %len_ptr, align 8
  br label %loop.header

loop.header:
  %iv = phi i32 [ %len, %preheader ], [ %iv.next, %loop.latch ]
  %cont = icmp sgt i32 %iv, %idx
  br i1 %cont, label %loop.latch, label %exit

exit:
  ret void

loop.latch:
  %iv.m1 = sub i32 %iv, 1
  %iv.m1.ext = sext i32 %iv.m1 to i64
  %src = getelementptr i64, ptr %data, i64 %iv.m1.ext
  %val = load i64, ptr %src, align 8
  %iv.ext = sext i32 %iv to i64
  %dst = getelementptr i64, ptr %data, i64 %iv.ext
  store i64 %val, ptr %dst, align 8
  %iv.next = add i32 %iv, -1
  br label %loop.header
}
