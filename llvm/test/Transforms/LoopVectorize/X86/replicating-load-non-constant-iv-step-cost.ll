; REQUIRES: asserts
; RUN: opt -p loop-vectorize -mtriple=x86_64-linux-gnu -debug-only=loop-vectorize -disable-output %s 2>&1 | FileCheck %s

; The hoisted loads from %gep.src use an induction with the non-constant step
; %s * %s, which cannot be versioned.

; CHECK-LABEL: LV: Checking a loop in 'hoisted_loads_non_constant_iv_step'
; CHECK: LV: Scalar loop costs: 7.
; CHECK: Cost of 6 for VF 2: REPLICATE ir<%l.else> = load ir<%gep.src>
; CHECK: Cost for VF 2: 17
; CHECK: Cost of 12 for VF 4: REPLICATE ir<%l.else> = load ir<%gep.src>
; CHECK: Cost for VF 4: 23
; CHECK: Cost of 25 for VF 8: REPLICATE ir<%l.else> = load ir<%gep.src>
; CHECK: Cost for VF 8: 36
; CHECK: LV: Selecting VF: 8.
define void @hoisted_loads_non_constant_iv_step(ptr noalias %dst, ptr noalias %src, ptr noalias %cond, i64 %s, i64 %n) #0 {
entry:
  %step = mul i64 %s, %s
  br label %loop

loop:
  %iv = phi i64 [ 0, %entry ], [ %iv.next, %loop.latch ]
  %iv.2 = phi i64 [ 0, %entry ], [ %iv.2.next, %loop.latch ]
  %gep.cond = getelementptr i32, ptr %cond, i64 %iv
  %c = load i32, ptr %gep.cond, align 4
  %c.0 = icmp eq i32 %c, 0
  %gep.src = getelementptr i32, ptr %src, i64 %iv.2
  br i1 %c.0, label %then, label %else

then:
  %l.then = load i32, ptr %gep.src, align 4
  %add = add i32 %l.then, 1
  br label %loop.latch

else:
  %l.else = load i32, ptr %gep.src, align 4
  br label %loop.latch

loop.latch:
  %merge = phi i32 [ %add, %then ], [ %l.else, %else ]
  %gep.dst = getelementptr i32, ptr %dst, i64 %iv
  store i32 %merge, ptr %gep.dst, align 4
  %iv.next = add i64 %iv, 1
  %iv.2.next = add i64 %iv.2, %step
  %ec = icmp eq i64 %iv.next, %n
  br i1 %ec, label %exit, label %loop

exit:
  ret void
}

attributes #0 = { "target-cpu"="znver2" }
