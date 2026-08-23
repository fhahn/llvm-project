; RUN: opt -passes=loop-vectorize -force-vector-width=4 -enable-epilogue-vectorization \
; RUN:     -epilogue-vectorization-force-VF=2 -epilogue-vectorization-minimum-VF=0 -S %s \
; RUN:     | FileCheck %s

; Tests for modeling the blocks generated for the main vector loop in the
; epilogue VPlan. In all cases below the main vector loop does not require a
; scalar epilogue while the epilogue vector loop does, so the loop's exit block
; is a successor of the main loop's middle block, but is not an exit block of the
; epilogue plan. Only the chain of blocks branching into the epilogue plan must
; be modeled; anything reachable from the main vector loop must be left alone.

; The exit block is terminated by a switch with 3 successors, which cannot be
; modeled in VPlan.
define void @exit_block_with_switch(ptr noalias %p, ptr noalias %end, i32 %s) {
; CHECK-LABEL: define void @exit_block_with_switch(
; CHECK:       vec.epilog.vector.body:
; CHECK:       exit:
; CHECK-NEXT:    switch i32 %s, label %d [
entry:
  br label %loop

loop:
  %q = phi ptr [ %p, %entry ], [ %q.next, %loop ]
  %l = load i32, ptr %q, align 4
  store i32 1, ptr %q, align 4
  %q.next = getelementptr inbounds nuw i8, ptr %q, i64 24
  %ec = icmp eq ptr %q.next, %end
  br i1 %ec, label %exit, label %loop

exit:
  switch i32 %s, label %d [
    i32 0, label %a
    i32 1, label %b
  ]

a:
  ret void

b:
  ret void

d:
  ret void
}

; The exit block is terminated by a switch with 2 successors, i.e. it has a
; number of successors that can be modeled in VPlan, but not a branch.
define void @exit_block_with_two_case_switch(ptr noalias %p, ptr noalias %end, i32 %s) {
; CHECK-LABEL: define void @exit_block_with_two_case_switch(
; CHECK:       vec.epilog.vector.body:
; CHECK:       exit:
; CHECK-NEXT:    switch i32 %s, label %d [
entry:
  br label %loop

loop:
  %q = phi ptr [ %p, %entry ], [ %q.next, %loop ]
  %l = load i32, ptr %q, align 4
  store i32 1, ptr %q, align 4
  %q.next = getelementptr inbounds nuw i8, ptr %q, i64 24
  %ec = icmp eq ptr %q.next, %end
  br i1 %ec, label %exit, label %loop

exit:
  switch i32 %s, label %d [
    i32 0, label %a
  ]

a:
  ret void

d:
  ret void
}

; The vectorized loop is nested, so blocks reachable from the main vector loop
; branch back to the block the epilogue plan is entered from.
define i32 @nested_loop(ptr noalias %p, ptr noalias %end, ptr noalias %dst, i64 %m) {
; CHECK-LABEL: define i32 @nested_loop(
; CHECK:       vec.epilog.vector.body:
; CHECK:       outer.latch:
entry:
  br label %outer

outer:
  %j = phi i64 [ 0, %entry ], [ %j.next, %outer.latch ]
  br label %loop

loop:
  %q = phi ptr [ %p, %outer ], [ %q.next, %loop ]
  %l = load i32, ptr %q, align 8
  store i32 %l, ptr %dst, align 4
  %q.next = getelementptr inbounds nuw i8, ptr %q, i64 24
  %ec = icmp eq ptr %q.next, %end
  br i1 %ec, label %inner.exit, label %loop

inner.exit:
  %cc = icmp ult i64 %j, 3
  br i1 %cc, label %outer.latch, label %bail

outer.latch:
  %j.next = add nuw nsw i64 %j, 1
  %oc = icmp eq i64 %j.next, %m
  br i1 %oc, label %done, label %outer

bail:
  ret i32 -1

done:
  ret i32 0
}
