; RUN: opt -passes=loop-vectorize -force-vector-width=4 -force-vector-interleave=1 -S %s | FileCheck %s
; RUN: opt -passes=loop-vectorize -force-vector-width=1 -force-vector-interleave=4 -S %s | FileCheck %s

; Each execution of an alloca returns a new object, distinct from the objects of
; all other executions, and those objects stay live until the function returns.
; A vector iteration executes one alloca for all of its lanes, so the loop must
; not be vectorized: the lanes would share a single object and its address.
define void @alloca_address_stored(ptr noalias %a, i64 %n) {
; CHECK-LABEL: define void @alloca_address_stored(
; CHECK-NOT:     vector.body
;
entry:
  br label %loop

loop:
  %iv = phi i64 [ 0, %entry ], [ %iv.next, %loop ]
  %p = alloca i64, align 8
  %gep = getelementptr inbounds ptr, ptr %a, i64 %iv
  store ptr %p, ptr %gep, align 8
  %iv.next = add nuw nsw i64 %iv, 1
  %ec = icmp eq i64 %iv.next, %n
  br i1 %ec, label %exit, label %loop

exit:
  ret void
}

; Same for an alloca in a predicated block.
define void @alloca_in_predicated_block(ptr noalias %a, ptr noalias readonly %c, i64 %n) {
; CHECK-LABEL: define void @alloca_in_predicated_block(
; CHECK-NOT:     vector.body
;
entry:
  br label %loop

loop:
  %iv = phi i64 [ 0, %entry ], [ %iv.next, %latch ]
  %gep.c = getelementptr inbounds i8, ptr %c, i64 %iv
  %cv = load i8, ptr %gep.c, align 1
  %tst = icmp eq i8 %cv, 0
  br i1 %tst, label %latch, label %if.then

if.then:
  %p = alloca i64, align 8
  br label %latch

latch:
  %addr = phi ptr [ null, %loop ], [ %p, %if.then ]
  %gep = getelementptr inbounds ptr, ptr %a, i64 %iv
  store ptr %addr, ptr %gep, align 8
  %iv.next = add nuw nsw i64 %iv, 1
  %ec = icmp eq i64 %iv.next, %n
  br i1 %ec, label %exit, label %loop

exit:
  ret void
}

; An alloca outside the loop is fine.
define void @alloca_outside_loop(ptr noalias %a, i64 %n) {
; CHECK-LABEL: define void @alloca_outside_loop(
; CHECK:         vector.body
;
entry:
  %p = alloca i64, align 8
  br label %loop

loop:
  %iv = phi i64 [ 0, %entry ], [ %iv.next, %loop ]
  %gep = getelementptr inbounds ptr, ptr %a, i64 %iv
  store ptr %p, ptr %gep, align 8
  %iv.next = add nuw nsw i64 %iv, 1
  %ec = icmp eq i64 %iv.next, %n
  br i1 %ec, label %exit, label %loop

exit:
  ret void
}
