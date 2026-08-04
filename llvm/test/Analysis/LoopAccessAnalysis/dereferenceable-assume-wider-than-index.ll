; RUN: opt -passes='print<access-info>' -disable-output %s 2>&1 | FileCheck %s

; The size operand of a dereferenceable assume bundle is an i64, which is wider
; than the 32-bit index type used for the pointer bounds computation. All SCEVs
; used to prove the AddRec does not wrap must use the same type; otherwise the
; predicate below compares SCEVs of different widths.
target datalayout = "p:32:32"

define void @deref_assume_wider_than_index_type(ptr %A, ptr %B) {
; CHECK-LABEL: 'deref_assume_wider_than_index_type'
; CHECK:         Memory dependences are safe with run-time checks
;
entry:
  call void @llvm.assume(i1 true) ["dereferenceable"(ptr %A, i64 2000)]
  br label %loop

loop:
  %iv = phi i32 [ 0, %entry ], [ %iv.next, %latch ]
  %gep.A = getelementptr i32, ptr %A, i32 %iv
  %l = load i32, ptr %gep.A, align 4
  store i32 %l, ptr %B, align 4
  %c = icmp eq i32 %l, 0
  %iv.next = add i32 %iv, 1
  br i1 %c, label %exit, label %latch

latch:
  %ec = icmp eq i32 %iv.next, 500
  br i1 %ec, label %exit, label %loop

exit:
  ret void
}

declare void @llvm.assume(i1)
