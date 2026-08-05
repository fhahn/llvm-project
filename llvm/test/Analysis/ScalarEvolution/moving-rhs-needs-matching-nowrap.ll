; RUN: opt -passes='print<scalar-evolution>' -disable-output %s 2>&1 | FileCheck %s

; The closed form for a comparison of two AddRecs is their meeting point over
; the integers, so it only gives the machine iteration count if the right hand
; side does not wrap in the signedness of the comparison. %r is {1,+,-2}<nsw>
; and the comparison is unsigned: %r dips below zero, becomes a large unsigned
; value, and the exit is never taken, so no count may be derived from it.
define void @ult_with_nsw_moving_rhs(ptr %A, ptr %B) {
; CHECK-LABEL: 'ult_with_nsw_moving_rhs'
; CHECK:         Loop %loop: <multiple exits> Unpredictable backedge-taken count.
; CHECK:         Loop %loop: constant max backedge-taken count is i8 29
; CHECK-NOT:     backedge-taken count is i8 1
;
entry:
  br label %loop

loop:
  %k = phi i8 [ 0, %entry ], [ %k.next, %latch ]
  %l = phi i8 [ 0, %entry ], [ %l.next, %latch ]
  %r = phi i8 [ 1, %entry ], [ %r.next, %latch ]
  %idx = zext i8 %k to i64
  %gA = getelementptr inbounds i32, ptr %A, i64 %idx
  %v = load i32, ptr %gA, align 4
  %gB = getelementptr inbounds i32, ptr %B, i64 %idx
  store i32 %v, ptr %gB, align 4
  %c1 = icmp ult i8 %l, %r
  br i1 %c1, label %latch, label %ret

latch:
  %l.next = add i8 %l, 1
  %r.next = add i8 %r, -2
  %k.next = add nuw nsw i8 %k, 1
  %c2 = icmp ult i8 %k.next, 30
  br i1 %c2, label %loop, label %ret

ret:
  ret void
}
