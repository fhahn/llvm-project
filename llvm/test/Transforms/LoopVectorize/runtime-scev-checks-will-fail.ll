; RUN: opt -passes=loop-vectorize -force-vector-width=4 -S -pass-remarks-analysis=loop-vectorize < %s 2>&1 | FileCheck %s

; The i8 IV wraps after 256 of the 1000 iterations, so the SCEV check that it
; does not wrap is known to fail and the vector loop would never be entered.

; CHECK: remark: <unknown>:0:0: loop not vectorized: runtime checks are known to fail, so we will never enter the vector loop
; CHECK-LABEL: define void @narrow_iv_known_to_wrap(
; CHECK-NOT:     vector.body
define void @narrow_iv_known_to_wrap(ptr %a) {
entry:
  br label %loop

loop:
  %iv = phi i64 [ 0, %entry ], [ %iv.next, %loop ]
  %iv.8 = phi i8 [ 0, %entry ], [ %iv.8.next, %loop ]
  %idx = zext i8 %iv.8 to i64
  %gep = getelementptr i32, ptr %a, i64 %idx
  %l = load i32, ptr %gep
  %add = add i32 %l, 1
  store i32 %add, ptr %gep
  %iv.next = add i64 %iv, 1
  %iv.8.next = add i8 %iv.8, 1
  %ec = icmp eq i64 %iv.next, 1000
  br i1 %ec, label %exit, label %loop

exit:
  ret void
}
