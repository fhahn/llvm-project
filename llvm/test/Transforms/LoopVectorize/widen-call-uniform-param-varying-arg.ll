; RUN: opt -passes=loop-vectorize -force-vector-width=4 -force-vector-interleave=1 -S %s | FileCheck %s

; Regression test for a crash when deciding call widening. A vector-function-abi
; variant with an OMP_Uniform ('u') parameter is validated by areVFParamsOk via
; ScalarEvolution::isLoopInvariant. When the argument mapped to the uniform
; parameter is loop-varying and has no computable SCEV (here a widened load),
; getSCEVExprForVPValue returns SCEVCouldNotCompute, on which isLoopInvariant
; asserts ("Attempt to use a SCEVCouldNotCompute object!"). The uniform check
; now guards against a non-computable SCEV and rejects the variant, so the call
; is scalarized.

; CHECK-LABEL: define void @uniform_param_varying_arg(
; CHECK:       vector.body:
; A loop-varying argument cannot satisfy the uniform ('u') contract, so the
; variant is rejected and the call is scalarized (no foo_u call).
; CHECK-NOT:     call <4 x i64> @foo_u
; CHECK:         call i64 @foo(

define void @uniform_param_varying_arg(ptr noalias %a, ptr %b) {
entry:
  br label %loop
loop:
  %iv = phi i64 [ 0, %entry ], [ %iv.next, %loop ]
  %gep = getelementptr i64, ptr %b, i64 %iv
  %ld = load i64, ptr %gep
  %r = call i64 @foo(i64 %ld) #0
  %sa = getelementptr i64, ptr %a, i64 %iv
  store i64 %r, ptr %sa
  %iv.next = add i64 %iv, 1
  %ec = icmp eq i64 %iv.next, 1024
  br i1 %ec, label %exit, label %loop
exit:
  ret void
}

; A loop-invariant argument does satisfy the uniform contract, so the variant
; is used.
; CHECK-LABEL: define void @uniform_param_invariant_arg(
; CHECK:       vector.body:
; CHECK:         call <4 x i64> @foo_u(

define void @uniform_param_invariant_arg(ptr noalias %a, i64 %inv) {
entry:
  br label %loop
loop:
  %iv = phi i64 [ 0, %entry ], [ %iv.next, %loop ]
  %r = call i64 @foo(i64 %inv) #0
  %sa = getelementptr i64, ptr %a, i64 %iv
  store i64 %r, ptr %sa
  %iv.next = add i64 %iv, 1
  %ec = icmp eq i64 %iv.next, 1024
  br i1 %ec, label %exit, label %loop
exit:
  ret void
}

declare i64 @foo(i64)
declare <4 x i64> @foo_u(i64, <4 x i1>)

attributes #0 = { nounwind "vector-function-abi-variant"="_ZGV_LLVM_M4u_foo(foo_u)" }
