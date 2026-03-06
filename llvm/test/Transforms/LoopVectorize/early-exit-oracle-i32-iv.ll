; RUN: opt -p loop-vectorize -force-vector-width=4 -S %s | FileCheck %s

; Test that the oracle correctly handles an i32 induction variable.
; The oracle trip count must match the IV type.
define i32 @oracle_i32_iv(ptr %A, ptr %B, i32 %n) {
entry:
  %cmp.entry = icmp sgt i32 %n, 0
  br i1 %cmp.entry, label %loop, label %exit

loop:
  %iv = phi i32 [ 0, %entry ], [ %iv.next, %loop.latch ]
  %gep.A = getelementptr inbounds i8, ptr %A, i32 %iv
  %ld.A = load i8, ptr %gep.A, align 1
  %gep.B = getelementptr inbounds i8, ptr %B, i32 %iv
  %ld.B = load i8, ptr %gep.B, align 1
  %cmp = icmp ne i8 %ld.A, %ld.B
  br i1 %cmp, label %early.exit, label %loop.latch

loop.latch:
  %iv.next = add nuw nsw i32 %iv, 1
  %ec = icmp ne i32 %iv.next, %n
  br i1 %ec, label %loop, label %exit

early.exit:
  ret i32 %iv

exit:
  ret i32 -1
}
