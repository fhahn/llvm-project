; RUN: opt < %s -disable-output -passes='print<scalar-evolution>' 2>&1 | FileCheck %s

target datalayout = "e-m:e-i64:64-f80:128-n8:16:32:64-S128"

declare void @use(i64)

; `zext nneg X` computes the same value as `sext X`. Zero extension does not
; distribute over an AddRec that only carries nsw, so the sign-extended
; expression is used instead: it separates the loop-invariant part of the
; address, which can then be hoisted out of the loop.
define void @distributes(ptr %p, i32 %n, i32 %k) {
; CHECK-LABEL: 'distributes'
; CHECK:         %ext = zext nneg i32 %off to i64
; CHECK-NEXT:    -->  {(sext i32 (-1 * %k) to i64),+,1}<nsw><%loop>
entry:
  br label %loop

loop:
  %iv = phi i32 [ 0, %entry ], [ %iv.next, %loop ]
  %off = sub nsw i32 %iv, %k
  %ext = zext nneg i32 %off to i64
  %gep = getelementptr i8, ptr %p, i64 %ext
  store i8 1, ptr %gep
  %iv.next = add nsw i32 %iv, 1
  %ec = icmp slt i32 %iv.next, %n
  br i1 %ec, label %loop, label %exit

exit:
  ret void
}

; nneg is poison-generating, so it may only be used when poison from the zext
; causes UB. Here the result only feeds a call argument that is not noundef.
define void @poison_does_not_cause_ub(i32 %n, i32 %k) {
; CHECK-LABEL: 'poison_does_not_cause_ub'
; CHECK:         %ext = zext nneg i32 %off to i64
; CHECK-NEXT:    -->  (zext i32 {(-1 * %k),+,1}<nw><%loop> to i64)
entry:
  br label %loop

loop:
  %iv = phi i32 [ 0, %entry ], [ %iv.next, %loop ]
  %off = sub nsw i32 %iv, %k
  %ext = zext nneg i32 %off to i64
  call void @use(i64 %ext)
  %iv.next = add nsw i32 %iv, 1
  %ec = icmp slt i32 %iv.next, %n
  br i1 %ec, label %loop, label %exit

exit:
  ret void
}

; The zext is not executed on every iteration, so other instructions mapping to
; the same expression cannot rely on the operand being non-negative.
define void @not_executed_every_iteration(ptr %p, i32 %n, i32 %k, i1 %c) {
; CHECK-LABEL: 'not_executed_every_iteration'
; CHECK:         %ext = zext nneg i32 %off to i64
; CHECK-NEXT:    -->  (zext i32 {(-1 * %k),+,1}<nw><%loop> to i64)
entry:
  br label %loop

loop:
  %iv = phi i32 [ 0, %entry ], [ %iv.next, %latch ]
  br i1 %c, label %then, label %latch

then:
  %off = sub nsw i32 %iv, %k
  %ext = zext nneg i32 %off to i64
  %gep = getelementptr i8, ptr %p, i64 %ext
  store i8 1, ptr %gep
  br label %latch

latch:
  %iv.next = add nsw i32 %iv, 1
  %ec = icmp slt i32 %iv.next, %n
  br i1 %ec, label %loop, label %exit

exit:
  ret void
}

; Without nneg nothing changes.
define void @plain_zext(ptr %p, i32 %n, i32 %k) {
; CHECK-LABEL: 'plain_zext'
; CHECK:         %ext = zext i32 %off to i64
; CHECK-NEXT:    -->  (zext i32 {(-1 * %k),+,1}<nsw><%loop> to i64)
entry:
  br label %loop

loop:
  %iv = phi i32 [ 0, %entry ], [ %iv.next, %loop ]
  %off = sub nsw i32 %iv, %k
  %ext = zext i32 %off to i64
  %gep = getelementptr i8, ptr %p, i64 %ext
  store i8 1, ptr %gep
  %iv.next = add nsw i32 %iv, 1
  %ec = icmp slt i32 %iv.next, %n
  br i1 %ec, label %loop, label %exit

exit:
  ret void
}

; The operand is loop-invariant, so nothing is separated from an AddRec here,
; but the sign-extended expression is still used: it relates this value to the
; extends of the other terms, which lets an already materialized `zext nneg` be
; reused where the opaque form would have to be recomputed.
define void @invariant_operand(ptr %p, i32 %n, i32 %k) {
; CHECK-LABEL: 'invariant_operand'
; CHECK:         %ext = zext nneg i32 %off to i64
; CHECK-NEXT:    -->  (-1 + (sext i32 %k to i64))<nsw>
entry:
  %off = add nsw i32 %k, -1
  br label %loop

loop:
  %iv = phi i32 [ 0, %entry ], [ %iv.next, %loop ]
  %ext = zext nneg i32 %off to i64
  %gep = getelementptr i8, ptr %p, i64 %ext
  store i8 1, ptr %gep
  %iv.next = add nsw i32 %iv, 1
  %ec = icmp slt i32 %iv.next, %n
  br i1 %ec, label %loop, label %exit

exit:
  ret void
}

; Sign extension does not distribute over `mul nsw`, so there is nothing to
; gain and the zero-extended expression is kept.
define void @invariant_mul_operand(ptr %p, i32 %n, i32 %k) {
; CHECK-LABEL: 'invariant_mul_operand'
; CHECK:         %ext = zext nneg i32 %off to i64
; CHECK-NEXT:    -->  (zext i32 (3 * %k)<nsw> to i64)
entry:
  %off = mul nsw i32 %k, 3
  br label %loop

loop:
  %iv = phi i32 [ 0, %entry ], [ %iv.next, %loop ]
  %ext = zext nneg i32 %off to i64
  %gep = getelementptr i8, ptr %p, i64 %ext
  store i8 1, ptr %gep
  %iv.next = add nsw i32 %iv, 1
  %ec = icmp slt i32 %iv.next, %n
  br i1 %ec, label %loop, label %exit

exit:
  ret void
}

; There is nothing for the sign extension to distribute over, so the
; zero-extended expression is kept.
define void @invariant_opaque_operand(ptr %p, i32 %n, i32 %k) {
; CHECK-LABEL: 'invariant_opaque_operand'
; CHECK:         %ext = zext nneg i32 %k to i64
; CHECK-NEXT:    -->  (zext i32 %k to i64)
entry:
  br label %loop

loop:
  %iv = phi i32 [ 0, %entry ], [ %iv.next, %loop ]
  %ext = zext nneg i32 %k to i64
  %gep = getelementptr i8, ptr %p, i64 %ext
  store i8 1, ptr %gep
  %iv.next = add nsw i32 %iv, 1
  %ec = icmp slt i32 %iv.next, %n
  br i1 %ec, label %loop, label %exit

exit:
  ret void
}
