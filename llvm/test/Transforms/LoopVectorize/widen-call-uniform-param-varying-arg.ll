; RUN: opt -passes=loop-vectorize -force-vector-width=4 -force-vector-interleave=1 -S %s | FileCheck %s

; A vector variant with a uniform ('u') parameter is only usable if the
; argument passed for it is loop-invariant.

; A loaded argument varies across the loop, so the call is scalarized.
; CHECK-LABEL: define void @uniform_param_loaded_arg(
; CHECK:       vector.body:
; CHECK-NOT:     call <4 x i64> @foo_u(
; CHECK:         call i64 @foo(
define void @uniform_param_loaded_arg(ptr noalias %dst, ptr %src, i64 %n) {
entry:
  br label %loop

loop:
  %iv = phi i64 [ 0, %entry ], [ %iv.next, %loop ]
  %gep.src = getelementptr i64, ptr %src, i64 %iv
  %l = load i64, ptr %gep.src
  %r = call i64 @foo(i64 %l) #0
  %gep.dst = getelementptr i64, ptr %dst, i64 %iv
  store i64 %r, ptr %gep.dst
  %iv.next = add i64 %iv, 1
  %ec = icmp eq i64 %iv.next, %n
  br i1 %ec, label %exit, label %loop

exit:
  ret void
}

; A loop-invariant argument can use the vector variant.
; CHECK-LABEL: define void @uniform_param_invariant_arg(
; CHECK:       vector.body:
; CHECK:         call <4 x i64> @foo_u(i64 %inv)
define void @uniform_param_invariant_arg(ptr %dst, i64 %inv, i64 %n) {
entry:
  br label %loop

loop:
  %iv = phi i64 [ 0, %entry ], [ %iv.next, %loop ]
  %r = call i64 @foo(i64 %inv) #0
  %gep.dst = getelementptr i64, ptr %dst, i64 %iv
  store i64 %r, ptr %gep.dst
  %iv.next = add i64 %iv, 1
  %ec = icmp eq i64 %iv.next, %n
  br i1 %ec, label %exit, label %loop

exit:
  ret void
}

declare i64 @foo(i64)
declare <4 x i64> @foo_u(i64)

attributes #0 = { "vector-function-abi-variant"="_ZGV_LLVM_N4u_foo(foo_u)" }
