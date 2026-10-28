; REQUIRES: asserts
; RUN: opt -passes=loop-vectorize -debug-only=loop-vectorize -disable-output %s 2>&1 | FileCheck %s

target triple = "x86_64-unknown-linux-gnu"

; The SCEV checks are costed as the recipes they are generated as.
define void @wrap_check(ptr %dst, i64 %n) {
; CHECK-LABEL: LV: Checking a loop in 'wrap_check'
; CHECK:      Calculating cost of runtime checks:
; CHECK-NEXT: Cost of 1 for VF 1: EMIT {{.+}} = add ir<%n>, ir<-1>
; CHECK-NEXT: Cost of 0 for VF 1: EMIT-SCALAR {{.+}} = trunc {{.+}} to i32
; CHECK-NEXT: Cost of 1 for VF 1: EMIT {{.+}} = icmp ult {{.+}}, ir<0>
; CHECK-NEXT: Cost of 1 for VF 1: EMIT {{.+}} = icmp ugt {{.+}}, ir<4294967295>
; CHECK-NEXT: Cost of 1 for VF 1: EMIT {{.+}} = or
; CHECK-NEXT: Cost of 0 for VF 1: EMIT branch-on-cond
; CHECK-NEXT: Total cost of runtime checks: 4
; CHECK:      LV: Minimum required TC for runtime checks to be profitable:12
entry:
  br label %loop

loop:
  %iv = phi i64 [ 0, %entry ], [ %iv.next, %loop ]
  %iv.32 = phi i32 [ 0, %entry ], [ %iv.32.next, %loop ]
  %ext = zext i32 %iv.32 to i64
  %gep = getelementptr i32, ptr %dst, i64 %ext
  store i32 0, ptr %gep, align 4
  %iv.32.next = add i32 %iv.32, 1
  %iv.next = add i64 %iv, 1
  %ec = icmp eq i64 %iv.next, %n
  br i1 %ec, label %exit, label %loop

exit:
  ret void
}

; The checks of the nssw and nusw predicates of the same AddRec share the
; computation of its last value and the compare of the trip count against the
; maximum i8 value, which are costed once.
define void @shared_subexpression(i8 %start, i32 %n, ptr %dst) {
; CHECK-LABEL: LV: Checking a loop in 'shared_subexpression'
; CHECK:      Calculating cost of runtime checks:
; CHECK-NEXT: Cost of 0 for VF 1: EMIT-SCALAR {{.+}} = trunc ir<%n> to i8
; CHECK-NEXT: Cost of 1 for VF 1: EMIT {{.+}} = add ir<%start>,
; CHECK-NEXT: Cost of 1 for VF 1: EMIT {{.+}} = icmp slt {{.+}}, ir<%start>
; CHECK-NEXT: Cost of 1 for VF 1: EMIT {{.+}} = icmp ugt ir<%n>, ir<255>
; CHECK-NEXT: Cost of 1 for VF 1: EMIT {{.+}} = or
; CHECK-NEXT: Cost of 1 for VF 1: EMIT {{.+}} = icmp ult {{.+}}, ir<%start>
; CHECK-NEXT: Cost of 1 for VF 1: EMIT {{.+}} = or
; CHECK-NEXT: Cost of 1 for VF 1: EMIT {{.+}} = or
; CHECK-NEXT: Cost of 0 for VF 1: EMIT branch-on-cond
; CHECK-NEXT: Total cost of runtime checks: 7
; CHECK:      LV: Minimum required TC for runtime checks to be profitable:16
entry:
  %start.ext = zext i8 %start to i32
  br label %loop

loop:
  %iv = phi i32 [ 0, %entry ], [ %iv.next, %loop ]
  %iv.8 = phi i8 [ %start, %entry ], [ %iv.8.next, %loop ]
  %iv.ext = phi i32 [ %start.ext, %entry ], [ %iv.8.next.ext, %loop ]
  %gep = getelementptr inbounds i32, ptr %dst, i8 %iv.8
  store i32 %iv.ext, ptr %gep, align 4
  %iv.8.next = add i8 %iv.8, 1
  %iv.8.next.ext = zext i8 %iv.8.next to i32
  %iv.next = add i32 %iv, 1
  %ec = icmp ult i32 %iv, %n
  br i1 %ec, label %loop, label %exit

exit:
  ret void
}

; The checks for an i1 induction with a runtime trip count.
define void @i1_induction(ptr noalias %src, ptr %dst, i32 %step, i64 %n) {
; CHECK-LABEL: LV: Checking a loop in 'i1_induction'
; CHECK:      Calculating cost of runtime checks:
; CHECK-NEXT: Cost of 0 for VF 1: EMIT-SCALAR {{.+}} = trunc ir<%step> to i1
; CHECK-NEXT: Cost of 1 for VF 1: EMIT {{.+}} = icmp slt {{.+}}, ir<false>
; CHECK-NEXT: Cost of 0 for VF 1: EMIT-SCALAR {{.+}} = trunc ir<%n> to i1
; CHECK-NEXT: Cost of 6 for VF 1: EMIT-SCALAR {{.+}} = call { i1, i1 } @llvm.umul.with.overflow(
; CHECK-NEXT: Cost of 0 for VF 1: EMIT {{.+}} = extractvalue {{.+}}, ir<0>
; CHECK-NEXT: Cost of 0 for VF 1: EMIT {{.+}} = extractvalue {{.+}}, ir<1>
; CHECK-NEXT: Cost of 1 for VF 1: EMIT {{.+}} = sub ir<false>,
; CHECK-NEXT: Cost of 1 for VF 1: EMIT {{.+}} = icmp ult {{.+}}, ir<false>
; CHECK-NEXT: Cost of 1 for VF 1: EMIT {{.+}} = icmp ugt {{.+}}, ir<false>
; CHECK-NEXT: Cost of 1 for VF 1: EMIT {{.+}} = select
; CHECK-NEXT: Cost of 1 for VF 1: EMIT {{.+}} = or
; CHECK-NEXT: Cost of 1 for VF 1: EMIT {{.+}} = icmp ugt ir<%n>, ir<1>
; CHECK-NEXT: Cost of 1 for VF 1: EMIT {{.+}} = and
; CHECK-NEXT: Cost of 1 for VF 1: EMIT {{.+}} = or
; CHECK-NEXT: Cost of 1 for VF 1: EMIT-SCALAR {{.+}} = sext {{.+}} to i32
; CHECK-NEXT: Cost of 1 for VF 1: EMIT {{.+}} = icmp ne ir<%step>,
; CHECK-NEXT: Cost of 1 for VF 1: EMIT {{.+}} = or
; CHECK-NEXT: Cost of 0 for VF 1: EMIT branch-on-cond
; CHECK-NEXT: Total cost of runtime checks: 18
; CHECK:      LV: Minimum required TC for runtime checks to be profitable:32
entry:
  br label %loop

loop:
  %iv.0 = phi i64 [ 0, %entry ], [ %iv.0.next, %loop ]
  %iv.1 = phi i32 [ 0, %entry ], [ %iv.1.next, %loop ]
  %iv.1.and = and i32 %iv.1, 1
  %gep.src = getelementptr float, ptr %src, i32 %iv.1.and
  %l = load float, ptr %gep.src, align 4
  %gep.dst = getelementptr float, ptr %dst, i64 %iv.0
  store float %l, ptr %gep.dst, align 4
  %iv.1.next = add i32 %iv.1.and, %step
  %iv.0.next = add i64 %iv.0, 1
  %ec = icmp eq i64 %iv.0, %n
  br i1 %ec, label %exit, label %loop

exit:
  ret void
}

; The parts of the checks of an inner loop invariant in the outer loop, like the
; backedge-taken count, are expanded outside of it and not costed.
define void @inner_loop(ptr %dst, i64 %n, i64 %m) {
; CHECK-LABEL: LV: Checking a loop in 'inner_loop'
; CHECK:      Calculating cost of runtime checks:
; CHECK-NEXT: Cost of 0 for VF 1: EMIT-SCALAR {{.+}} = trunc [[BTC:vp<%.+>]] to i32
; CHECK-NEXT: Cost of 1 for VF 1: EMIT {{.+}} = icmp ult {{.+}}, ir<0>
; CHECK-NEXT: Cost of 1 for VF 1: EMIT {{.+}} = icmp ugt [[BTC]], ir<4294967295>
; CHECK-NEXT: Cost of 1 for VF 1: EMIT {{.+}} = or
; CHECK-NEXT: Cost of 0 for VF 1: EMIT branch-on-cond
; CHECK-NEXT: Total cost of runtime checks: 3
; CHECK:      LV: Minimum required TC for runtime checks to be profitable:8
entry:
  br label %outer

outer:
  %outer.iv = phi i64 [ 0, %entry ], [ %outer.iv.next, %outer.latch ]
  br label %loop

loop:
  %iv = phi i64 [ 0, %outer ], [ %iv.next, %loop ]
  %iv.32 = phi i32 [ 0, %outer ], [ %iv.32.next, %loop ]
  %ext = zext i32 %iv.32 to i64
  %gep = getelementptr i32, ptr %dst, i64 %ext
  store i32 0, ptr %gep, align 4
  %iv.32.next = add i32 %iv.32, 1
  %iv.next = add i64 %iv, 1
  %ec = icmp eq i64 %iv.next, %n
  br i1 %ec, label %outer.latch, label %loop

outer.latch:
  %outer.iv.next = add i64 %outer.iv, 1
  %outer.ec = icmp eq i64 %outer.iv.next, %m
  br i1 %outer.ec, label %exit, label %outer

exit:
  ret void
}

; The checks fold to false, so no checks are costed.
define void @checks_fold_to_false(ptr %dst) {
; CHECK-LABEL: LV: Checking a loop in 'checks_fold_to_false'
; CHECK:      Calculating cost of runtime checks:
; CHECK-NEXT: Total cost of runtime checks: 0
; CHECK:      LV: Minimum required TC for runtime checks to be profitable:4
entry:
  br label %loop

loop:
  %iv = phi i32 [ 0, %entry ], [ %iv.next, %loop ]
  %iv.8 = phi i8 [ 0, %entry ], [ %iv.8.next, %loop ]
  %iv.32 = phi i32 [ 0, %entry ], [ %iv.32.next, %loop ]
  %trunc = trunc i32 %iv.32 to i8
  %iv.8.next = add i8 %iv.8, %trunc
  %gep = getelementptr inbounds i32, ptr %dst, i32 %iv
  store i32 %iv.32, ptr %gep, align 4
  %and = and i32 %iv.32, 255
  %iv.32.next = add nuw nsw i32 %and, 9
  %iv.next = add i32 %iv, 1
  %ec = icmp eq i32 %iv.next, 16
  br i1 %ec, label %exit, label %loop

exit:
  store i8 %iv.8.next, ptr %dst, align 1
  ret void
}
