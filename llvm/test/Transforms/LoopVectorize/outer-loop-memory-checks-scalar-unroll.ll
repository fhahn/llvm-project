; RUN: opt -passes=loop-vectorize -enable-vplan-native-path -vplan-outer-loop-legality-checks -force-vector-width=4 -S %s | FileCheck %s

; The scalar loop runs if the memory runtime checks fail, so runtime unrolling
; is not disabled for it.
define void @may_alias_load(ptr %A, ptr %B, i64 %N, i64 %M) {
; CHECK-LABEL: define void @may_alias_load(
; CHECK:       vector.memcheck:
; CHECK:         br i1 %outer.iv.cmp, label %exit, label %outer.header, !llvm.loop [[SCALAR_LOOP:![0-9]+]]
entry:
  br label %outer.header

outer.header:
  %outer.iv = phi i64 [ 0, %entry ], [ %outer.iv.next, %outer.latch ]
  %outer.iv.mul.M = mul nsw i64 %outer.iv, %M
  br label %inner.body

inner.body:
  %inner.iv = phi i64 [ 0, %outer.header ], [ %inner.iv.next, %inner.body ]
  %sum = phi float [ 0.000000e+00, %outer.header ], [ %sum.next, %inner.body ]
  %idx = add nsw i64 %outer.iv.mul.M, %inner.iv
  %A.ptr = getelementptr inbounds float, ptr %A, i64 %idx
  %A.val = load float, ptr %A.ptr, align 4
  %sum.next = fadd float %sum, %A.val
  %inner.iv.next = add nuw nsw i64 %inner.iv, 1
  %inner.iv.cmp = icmp eq i64 %inner.iv.next, %M
  br i1 %inner.iv.cmp, label %outer.latch, label %inner.body

outer.latch:
  %B.ptr = getelementptr inbounds float, ptr %B, i64 %outer.iv
  store float %sum.next, ptr %B.ptr, align 4
  %outer.iv.next = add nuw nsw i64 %outer.iv, 1
  %outer.iv.cmp = icmp eq i64 %outer.iv.next, %N
  br i1 %outer.iv.cmp, label %exit, label %outer.header, !llvm.loop !0

exit:
  ret void
}

!0 = distinct !{!0, !1}
!1 = !{!"llvm.loop.vectorize.enable"}

; CHECK: [[IS_VECTORIZED:![0-9]+]] = !{!"llvm.loop.isvectorized", i32 1}
; CHECK: [[SCALAR_LOOP]] = distinct !{[[SCALAR_LOOP]], [[IS_VECTORIZED]]}
