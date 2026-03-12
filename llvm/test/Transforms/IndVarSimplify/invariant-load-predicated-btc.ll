; RUN: opt -p indvars -S %s | FileCheck %s
;
; Verify that IndVarSimplify does not use the predicated BTC containing
; SCEVLoopInvariantLoad to rewrite IVs or exit values. The load in
; the loop is not provably invariant without runtime checks, so
; IndVarSimplify must leave the loop unchanged.

; The loop exits when iv.next >= load(ptr). IndVarSimplify should NOT
; rewrite the exit condition using the predicated BTC because it
; cannot verify the no-alias load predicate.
define i32 @indvars_no_rewrite_invariant_load_exit(ptr %ptr, ptr %dst) {
; CHECK-LABEL: define i32 @indvars_no_rewrite_invariant_load_exit(
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
  %result = phi i32 [ %iv.next, %loop ]
  ret i32 %result
}

; The loop has a countable exit (constant bound) but also uses an
; invariant-address load in the body. IndVarSimplify should handle
; this normally since the exit doesn't depend on the invariant load.
define i32 @indvars_countable_with_invariant_load_body(ptr %ptr, ptr %dst) {
; CHECK-LABEL: define i32 @indvars_countable_with_invariant_load_body(
; CHECK:       loop:
; CHECK:         %limit = load i32, ptr %ptr
entry:
  br label %loop

loop:
  %iv = phi i32 [ 0, %entry ], [ %iv.next, %loop ]
  %sum = phi i32 [ 0, %entry ], [ %sum.next, %loop ]
  %limit = load i32, ptr %ptr
  %sum.next = add i32 %sum, %limit
  %p = getelementptr i8, ptr %dst, i32 %iv
  store i8 42, ptr %p
  %iv.next = add nuw nsw i32 %iv, 1
  %cmp = icmp slt i32 %iv.next, 100
  br i1 %cmp, label %loop, label %exit

exit:
  ret i32 %sum.next
}
