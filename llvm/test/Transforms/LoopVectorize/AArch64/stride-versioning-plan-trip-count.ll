; RUN: opt -passes=loop-vectorize -mtriple=arm64-apple-macosx -mcpu=apple-m1 \
; RUN:     -pass-remarks=loop-vectorize -pass-remarks-analysis=loop-vectorize \
; RUN:     -S %s -o /dev/null 2>&1 | \
; RUN:   FileCheck %s --implicit-check-not="vector loop never executes"

; The trip counts of the loops below depend on %stride. After versioning
; %stride == 1, the plan's trip count is a small constant, which is smaller
; than the trip count of the original loop. VF and IC must be chosen such that
; the vector loop can execute with the plan's trip count.

; Trip count is 1 with %stride == 1, the vector loop can never execute.
; CHECK-NOT: vectorized loop
; CHECK-NOT: interleaved loop
define void @tc_1(ptr noalias %dst, ptr %src, i64 %stride) {
entry:
  br label %loop

loop:
  %iv = phi i64 [ 0, %entry ], [ %iv.next, %loop ]
  %iv.next = add nsw i64 %iv, 1
  %idx = mul i64 %iv, %stride
  %gep.src = getelementptr i64, ptr %src, i64 %idx
  %l = load i64, ptr %gep.src, align 8
  %gep.dst = getelementptr i64, ptr %dst, i64 %iv
  store i64 %l, ptr %gep.dst, align 8
  %ec = icmp slt i64 %iv.next, %stride
  br i1 %ec, label %loop, label %exit

exit:
  ret void
}

; Trip count is 2 with %stride == 1.
; CHECK: remark: {{.*}} vectorized loop (vectorization width: 2, interleaved count: 1)
define void @tc_2(ptr noalias %dst, ptr %src, i64 %stride) {
entry:
  %n = add i64 %stride, 1
  br label %loop

loop:
  %iv = phi i64 [ 0, %entry ], [ %iv.next, %loop ]
  %iv.next = add nsw i64 %iv, 1
  %idx = mul i64 %iv, %stride
  %gep.src = getelementptr i64, ptr %src, i64 %idx
  %l = load i64, ptr %gep.src, align 8
  %gep.dst = getelementptr i64, ptr %dst, i64 %iv
  store i64 %l, ptr %gep.dst, align 8
  %ec = icmp slt i64 %iv.next, %n
  br i1 %ec, label %loop, label %exit

exit:
  ret void
}

; Trip count is 4 with %stride == 1.
; CHECK: remark: {{.*}} vectorized loop (vectorization width: 2, interleaved count: 2)
define void @tc_4(ptr noalias %dst, ptr %src, i64 %stride) {
entry:
  %n = shl i64 %stride, 2
  br label %loop

loop:
  %iv = phi i64 [ 0, %entry ], [ %iv.next, %loop ]
  %iv.next = add nsw i64 %iv, 1
  %idx = mul i64 %iv, %stride
  %gep.src = getelementptr i64, ptr %src, i64 %idx
  %l = load i64, ptr %gep.src, align 8
  %gep.dst = getelementptr i64, ptr %dst, i64 %iv
  store i64 %l, ptr %gep.dst, align 8
  %ec = icmp slt i64 %iv.next, %n
  br i1 %ec, label %loop, label %exit

exit:
  ret void
}
