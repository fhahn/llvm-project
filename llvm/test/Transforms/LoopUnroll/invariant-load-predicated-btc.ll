; RUN: opt -p loop-unroll -S %s | FileCheck %s
;
; Verify that LoopUnroll does not attempt to fully unroll a loop that
; has an uncountable exit with a predicated BTC from an invariant load.

define void @no_unroll_invariant_load_exit(ptr %ptr, ptr %dst) {
; CHECK-LABEL: define void @no_unroll_invariant_load_exit(
; CHECK:       loop:
; CHECK:         %limit = load i32, ptr %ptr
; CHECK:         %cmp = icmp slt i32 %iv.next, %limit
; CHECK:         br i1 %cmp, label %loop, label %exit
entry:
  br label %loop

loop:
  %iv = phi i32 [ 0, %entry ], [ %iv.next, %loop ]
  %p = getelementptr i8, ptr %dst, i32 %iv
  store i8 42, ptr %p
  %iv.next = add nuw nsw i32 %iv, 1
  %limit = load i32, ptr %ptr
  %cmp = icmp slt i32 %iv.next, %limit
  br i1 %cmp, label %loop, label %exit

exit:
  ret void
}
