; RUN: opt -passes=loop-vectorize -mtriple=riscv64 -mattr=+v -S %s 2>&1 | FileCheck %s

; Regression test for a 32-bit unsigned overflow in estimateElementCount. The
; runtime element count of a scalable VF is its known minimum value scaled by
; the estimated vscale. With a huge vscale_range the product overflowed the
; 32-bit `unsigned` result to 0, tripping the assert that the estimated VF is at
; least 1. The product is now computed in 64 bits and saturated.

; CHECK-LABEL: define void @huge_vscale_range(
define void @huge_vscale_range(ptr %a, ptr %b) vscale_range(1073741824,1073741824) {
entry:
  br label %loop

loop:
  %iv = phi i64 [ 0, %entry ], [ %iv.next, %loop ]
  %g = getelementptr i8, ptr %a, i64 %iv
  %l = load i8, ptr %g
  %gb = getelementptr i8, ptr %b, i64 %iv
  store i8 %l, ptr %gb
  %iv.next = add i64 %iv, 1
  %ec = icmp eq i64 %iv.next, 16
  br i1 %ec, label %exit, label %loop, !llvm.loop !0

exit:
  ret void
}

!0 = distinct !{!0, !1, !2, !3}
!1 = !{!"llvm.loop.vectorize.width", i32 16}
!2 = !{!"llvm.loop.vectorize.scalable.enable"}
!3 = !{!"llvm.loop.interleave.count", i32 16}
