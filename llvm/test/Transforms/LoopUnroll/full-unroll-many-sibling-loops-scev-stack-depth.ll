; RUN: opt -passes='loop(loop-unroll-full)' -S < %s | FileCheck %s

; Fully unrolling the outer loops creates many sibling copies of the inner
; loops. Each copy is entered through the exit of the previous copy, so proving
; its entry guards builds the SCEV of the previous copy's IV, whose wrap flags
; need the backedge-taken count of that copy, and so on. Make sure SCEV does not
; recurse once per copy, which overflows the stack for these copy counts.

; CHECK-LABEL: define void @post_inc(
; CHECK:       inner.1499:
define void @post_inc(i64 %s) {
entry:
  %entered = icmp slt i64 %s, 0
  br i1 %entered, label %outer, label %exit

outer:
  %k = phi i32 [ %k.next, %outer.latch ], [ 0, %entry ]
  br label %inner

inner:
  %j = phi i64 [ %s, %outer ], [ %j.next, %inner ]
  call void @sink()
  %j.next = add nsw i64 %j, 3
  %more = icmp slt i64 %j.next, 0
  br i1 %more, label %inner, label %outer.latch

outer.latch:
  %k.next = add nuw nsw i32 %k, 1
  %again = icmp samesign ult i32 %k, 1499
  br i1 %again, label %outer, label %exit, !llvm.loop !0

exit:
  ret void
}

; The previous copy's compare is only reachable through a logical or.
; CHECK-LABEL: define void @or_exit(
; CHECK:       inner.1299:
define void @or_exit(i64 %s, i1 %c) {
entry:
  %entered = icmp slt i64 %s, 0
  br i1 %entered, label %outer, label %exit

outer:
  %k = phi i32 [ %k.next, %outer.latch ], [ 0, %entry ]
  br label %inner

inner:
  %j = phi i64 [ %s, %outer ], [ %j.next, %inner ]
  call void @sink()
  %j.next = add nsw i64 %j, 3
  %cmp = icmp slt i64 %j.next, 0
  %more = select i1 %cmp, i1 true, i1 %c
  br i1 %more, label %inner, label %outer.latch

outer.latch:
  %k.next = add nuw nsw i32 %k, 1
  %again = icmp samesign ult i32 %k, 1299
  br i1 %again, label %outer, label %exit, !llvm.loop !1

exit:
  ret void
}

declare void @sink()

!0 = distinct !{!0, !2}
!1 = distinct !{!1, !2}
!2 = !{!"llvm.loop.unroll.enable"}
