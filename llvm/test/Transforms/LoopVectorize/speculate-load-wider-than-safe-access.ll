; RUN: opt -passes=loop-vectorize -force-vector-width=4 -force-vector-interleave=1 -S %s | FileCheck %s

; A conditional load may only be speculated if the address is known to be safe
; to access for this load's size and alignment. Here the unconditional i8 load
; only makes 1 byte at %gep safe, so the conditional i64 load must be masked.
define i64 @speculate_load_wider_than_unconditional_load(ptr noalias readonly %base, i64 %n) {
; CHECK-LABEL: define i64 @speculate_load_wider_than_unconditional_load(
; CHECK:       [[VECTOR_BODY:.*]]:
; CHECK:         [[WIDE_LOAD:%.*]] = load <4 x i8>, ptr {{.*}}, align 1
; CHECK:         br i1 {{.*}}, label %[[PRED_LOAD_IF:.*]], label %{{.*}}
; CHECK:       [[PRED_LOAD_IF]]:
; CHECK-NEXT:    load i64
;
entry:
  br label %loop

loop:
  %iv = phi i64 [ 0, %entry ], [ %iv.next, %latch ]
  %acc = phi i64 [ 0, %entry ], [ %acc.next, %latch ]
  %gep = getelementptr inbounds i8, ptr %base, i64 %iv
  %small = load i8, ptr %gep, align 1
  %tst = icmp eq i8 %small, 0
  br i1 %tst, label %latch, label %if.then

if.then:
  %v = load i64, ptr %gep, align 1
  br label %latch

latch:
  %m = phi i64 [ 0, %loop ], [ %v, %if.then ]
  %acc.next = add i64 %acc, %m
  %iv.next = add nuw nsw i64 %iv, 1
  %ec = icmp eq i64 %iv.next, %n
  br i1 %ec, label %exit, label %loop

exit:
  ret i64 %acc.next
}

; A load of the same size as the unconditional access can still be speculated.
define i64 @speculate_load_same_size(ptr noalias readonly %base, i64 %n) {
; CHECK-LABEL: define i64 @speculate_load_same_size(
; CHECK:       [[VECTOR_BODY1:.*]]:
; CHECK-NOT:     pred.load.if
; CHECK:         [[WIDE_LOAD1:%.*]] = load <4 x i64>, ptr {{.*}}, align 8
;
entry:
  br label %loop

loop:
  %iv = phi i64 [ 0, %entry ], [ %iv.next, %latch ]
  %acc = phi i64 [ 0, %entry ], [ %acc.next, %latch ]
  %gep = getelementptr inbounds i64, ptr %base, i64 %iv
  %small = load i64, ptr %gep, align 8
  %tst = icmp eq i64 %small, 0
  br i1 %tst, label %latch, label %if.then

if.then:
  %v = load i64, ptr %gep, align 8
  br label %latch

latch:
  %m = phi i64 [ 0, %loop ], [ %v, %if.then ]
  %acc.next = add i64 %acc, %m
  %iv.next = add nuw nsw i64 %iv, 1
  %ec = icmp eq i64 %iv.next, %n
  br i1 %ec, label %exit, label %loop

exit:
  ret i64 %acc.next
}
