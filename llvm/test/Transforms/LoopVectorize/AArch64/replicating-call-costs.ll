; REQUIRES: asserts
; RUN: opt -p loop-vectorize -debug-only=loop-vectorize -disable-output %s 2>&1 | FileCheck %s

target triple = "arm64-apple-macosx"

; Only a VF 4 variant of @foo exists, so the call is replicated at VF 2. Its
; operand is a SCALAR-STEPS recipe, which already provides a scalar per lane.
define void @replicated_call_scalar_steps_operand(ptr noalias %dst) {
; CHECK-LABEL: LV: Checking a loop in 'replicated_call_scalar_steps_operand'
; CHECK: Cost of 1 for VF 2: [[STEPS:vp<%.+>]] = SCALAR-STEPS vp<%{{.+}}>, ir<1>, vp<%0>
; CHECK: Cost of 24 for VF 2: REPLICATE ir<%r> = call @foo([[STEPS]])
; CHECK: Cost of 10 for VF 4: WIDEN-CALL ir<%r> = call @foo(ir<%iv>) (using library function: foo_v4)
entry:
  br label %loop

loop:
  %iv = phi i64 [ 0, %entry ], [ %iv.next, %loop ]
  %iv.trunc = trunc i64 %iv to i32
  %r = call i32 @foo(i32 %iv.trunc) #0
  %gep.dst = getelementptr i32, ptr %dst, i64 %iv
  store i32 %r, ptr %gep.dst
  %iv.next = add nuw nsw i64 %iv, 1
  %ec = icmp eq i64 %iv.next, 1000
  br i1 %ec, label %exit, label %loop

exit:
  ret void
}

; Each lane of a scalarized umul.with.overflow is a cheap scalar intrinsic, so
; replicating should be cheaper than widening at VF 2.
define void @replicated_umul_with_overflow(ptr noalias %dst) {
; CHECK-LABEL: LV: Checking a loop in 'replicated_umul_with_overflow'
; CHECK: Cost of 14 for VF 2: REPLICATE ir<%res> = call @llvm.umul.with.overflow.i64(vp<%{{.+}}>, ir<3>)
entry:
  br label %loop

loop:
  %iv = phi i64 [ 0, %entry ], [ %iv.next, %loop ]
  %res = call { i64, i1 } @llvm.umul.with.overflow.i64(i64 %iv, i64 3)
  %mul = extractvalue { i64, i1 } %res, 0
  %gep.dst = getelementptr i64, ptr %dst, i64 %iv
  store i64 %mul, ptr %gep.dst
  %iv.next = add nuw nsw i64 %iv, 1
  %ec = icmp eq i64 %iv.next, 1000
  br i1 %ec, label %exit, label %loop

exit:
  ret void
}

declare i32 @foo(i32)
declare <4 x i32> @foo_v4(<4 x i32>)

attributes #0 = { nounwind memory(none) "vector-function-abi-variant"="_ZGVnN4v_foo(foo_v4)" }
