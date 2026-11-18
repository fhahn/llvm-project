; REQUIRES: asserts
; RUN: opt -p loop-vectorize -force-vector-width=2 -force-vector-interleave=1 -force-target-supports-masked-memory-ops -debug-only=loop-vectorize -disable-output %s 2>&1 | FileCheck %s

; The speculated load from the loop-invariant %a loads the same value for all
; lanes and is executed as a single load. Check its cost.
define void @speculated_load_loop_invariant_addr(ptr noalias noundef %a, ptr noalias %b, ptr noalias %c) nofree nosync {
; CHECK-LABEL: LV: Checking a loop in 'speculated_load_loop_invariant_addr'
; CHECK: Cost of 1 for VF 2: CLONE ir<%l.a> = load ir<%a>
; CHECK: Cost for VF 2: 8
;
entry:
  call void @llvm.assume(i1 true) [ "align"(ptr %a, i64 4), "dereferenceable"(ptr %a, i64 4) ]
  br label %loop.header

loop.header:
  %iv = phi i64 [ 0, %entry ], [ %iv.next, %loop.latch ]
  %gep.b = getelementptr inbounds i32, ptr %b, i64 %iv
  %l.b = load i32, ptr %gep.b, align 4
  %c.1 = icmp sge i32 %l.b, 0
  br i1 %c.1, label %loop.latch, label %loop.then

loop.then:
  %l.a = load i32, ptr %a, align 4
  br label %loop.latch

loop.latch:
  %merge = phi i32 [ %l.a, %loop.then ], [ %l.b, %loop.header ]
  %gep.c = getelementptr inbounds i32, ptr %c, i64 %iv
  store i32 %merge, ptr %gep.c, align 4
  %iv.next = add nuw nsw i64 %iv, 1
  %ec = icmp eq i64 %iv.next, 1000
  br i1 %ec, label %exit, label %loop.header

exit:
  ret void
}
