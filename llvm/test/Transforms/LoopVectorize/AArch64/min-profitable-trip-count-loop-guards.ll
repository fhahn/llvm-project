; RUN: opt -passes=loop-vectorize -mcpu=apple-m1 -pass-remarks=loop-vectorize -pass-remarks-analysis=loop-vectorize -disable-output %s 2>&1 | FileCheck %s

; Test that loops whose trip count is bounded by a loop guard below the minimum
; profitable trip count required by the runtime checks are not vectorized. The
; minimum iteration check would always branch to the scalar loop.

target triple = "arm64-apple-macosx"

; The trip count is at most 9, and the minimum profitable trip count is 10.
; CHECK: remark: <unknown>:0:0: vectorized loop (vectorization width: 2, interleaved count: 4)
; CHECK-NEXT: remark: <unknown>:0:0: Created vector loop never executes due to insufficient trip count.
define void @tc_below_min_profitable_tc(ptr %dst, ptr %a, ptr %b, ptr %c, i64 %n) {
entry:
  %g = icmp ult i64 %n, 10
  br i1 %g, label %loop, label %exit

loop:
  %iv = phi i64 [ 0, %entry ], [ %iv.next, %loop ]
  %gep.a = getelementptr inbounds i64, ptr %a, i64 %iv
  %la = load i64, ptr %gep.a, align 8
  %gep.b = getelementptr inbounds i64, ptr %b, i64 %iv
  %lb = load i64, ptr %gep.b, align 8
  %gep.c = getelementptr inbounds i64, ptr %c, i64 %iv
  %lc = load i64, ptr %gep.c, align 8
  %add = add i64 %la, %lb
  %add2 = add i64 %add, %lc
  %gep.dst = getelementptr inbounds i64, ptr %dst, i64 %iv
  store i64 %add2, ptr %gep.dst, align 8
  %iv.next = add nuw nsw i64 %iv, 1
  %ec = icmp eq i64 %iv.next, %n
  br i1 %ec, label %exit, label %loop

exit:
  ret void
}

; The trip count is at most 10, and the minimum profitable trip count is 10.
; CHECK: remark: <unknown>:0:0: vectorized loop (vectorization width: 2, interleaved count: 4)
define void @tc_may_be_min_profitable_tc(ptr %dst, ptr %a, ptr %b, ptr %c, i64 %n) {
entry:
  %g = icmp ult i64 %n, 11
  br i1 %g, label %loop, label %exit

loop:
  %iv = phi i64 [ 0, %entry ], [ %iv.next, %loop ]
  %gep.a = getelementptr inbounds i64, ptr %a, i64 %iv
  %la = load i64, ptr %gep.a, align 8
  %gep.b = getelementptr inbounds i64, ptr %b, i64 %iv
  %lb = load i64, ptr %gep.b, align 8
  %gep.c = getelementptr inbounds i64, ptr %c, i64 %iv
  %lc = load i64, ptr %gep.c, align 8
  %add = add i64 %la, %lb
  %add2 = add i64 %add, %lc
  %gep.dst = getelementptr inbounds i64, ptr %dst, i64 %iv
  store i64 %add2, ptr %gep.dst, align 8
  %iv.next = add nuw nsw i64 %iv, 1
  %ec = icmp eq i64 %iv.next, %n
  br i1 %ec, label %exit, label %loop

exit:
  ret void
}

; A scalar epilogue is required by the interleave group for the strided load of
; %a. The trip count is at most 12, and the minimum profitable trip count is 12.
; CHECK: remark: <unknown>:0:0: vectorized loop (vectorization width: 2, interleaved count: 4)
; CHECK-NEXT: remark: <unknown>:0:0: Created vector loop never executes due to insufficient trip count.
define void @tc_equal_min_profitable_tc_requires_scalar_epilogue(ptr %dst, ptr %a, ptr %b, ptr %c, i64 %n) {
entry:
  %g = icmp ult i64 %n, 13
  br i1 %g, label %loop, label %exit

loop:
  %iv = phi i64 [ 0, %entry ], [ %iv.next, %loop ]
  %iv2 = shl nuw nsw i64 %iv, 1
  %gep.a = getelementptr inbounds i64, ptr %a, i64 %iv2
  %la = load i64, ptr %gep.a, align 8
  %gep.b = getelementptr inbounds i64, ptr %b, i64 %iv
  %lb = load i64, ptr %gep.b, align 8
  %gep.c = getelementptr inbounds i64, ptr %c, i64 %iv
  %lc = load i64, ptr %gep.c, align 8
  %add = add i64 %la, %lb
  %add2 = add i64 %add, %lc
  %gep.dst = getelementptr inbounds i64, ptr %dst, i64 %iv
  store i64 %add2, ptr %gep.dst, align 8
  %iv.next = add nuw nsw i64 %iv, 1
  %ec = icmp eq i64 %iv.next, %n
  br i1 %ec, label %exit, label %loop

exit:
  ret void
}

; CHECK-NOT: Created vector loop never executes
