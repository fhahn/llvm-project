; RUN: opt -passes=loop-vectorize -force-vector-width=4 -enable-vplan-native-path -debug-only=loop-vectorize -disable-output %s 2>&1 | FileCheck %s

; The pair of accesses that could not be separated statically is registered as a
; predicate of the plan, which is materialized into a check block once the plan
; is selected for vectorization.

define void @store_may_alias_load(ptr %A, ptr %B, i64 %N, i64 %M) {
; CHECK-LABEL: LV: Checking a loop in 'store_may_alias_load'
; CHECK:      VPlan ' for VF={4},UF>=1' {
; CHECK:      Predicates:
; CHECK-NEXT:   No overlap: [%B, ((4 * %N) + %B)) and [(((4 * (-1 + %N) * %M) + %A) umin %A), ((4 * %M) + (((4 * (-1 + %N) * %M) + %A) umax %A)))
; CHECK-EMPTY:
; CHECK-NEXT: ir-bb<outer.ph>:
; CHECK-NEXT: Successor(s): scalar.ph, vector.ph
;
; CHECK:      VPlan 'Final VPlan for VF={4},UF={1}' {
; CHECK:      vector.memcheck:
; CHECK:        EMIT vp<[[BOUND0:%.+]]> = icmp ult ir<%B>, vp<{{.+}}>
; CHECK-NEXT:   EMIT vp<[[BOUND1:%.+]]> = icmp ult vp<{{.+}}>, vp<{{.+}}>
; CHECK-NEXT:   EMIT vp<[[CONFLICT:%.+]]> = and vp<[[BOUND0]]>, vp<[[BOUND1]]>
; CHECK-NEXT:   EMIT branch-on-cond vp<[[CONFLICT]]>
; CHECK-NEXT: Successor(s): ir-bb<scalar.ph>, vector.ph
entry:
  %cmp.outer = icmp sgt i64 %N, 0
  br i1 %cmp.outer, label %outer.ph, label %exit

outer.ph:
  br label %outer.header

outer.header:
  %i = phi i64 [ 0, %outer.ph ], [ %i.next, %outer.latch ]
  %i.mul.M = mul nsw i64 %i, %M
  br label %inner.body

inner.body:
  %j = phi i64 [ 0, %outer.header ], [ %j.next, %inner.body ]
  %sum = phi float [ 0.000000e+00, %outer.header ], [ %sum.next, %inner.body ]
  %idx = add nsw i64 %i.mul.M, %j
  %A.ptr = getelementptr inbounds float, ptr %A, i64 %idx
  %A.val = load float, ptr %A.ptr, align 4
  %sum.next = fadd float %sum, %A.val
  %j.next = add nuw nsw i64 %j, 1
  %j.cmp = icmp eq i64 %j.next, %M
  br i1 %j.cmp, label %outer.latch, label %inner.body

outer.latch:
  %B.ptr = getelementptr inbounds float, ptr %B, i64 %i
  store float %sum.next, ptr %B.ptr, align 4
  %i.next = add nuw nsw i64 %i, 1
  %i.cmp = icmp eq i64 %i.next, %N
  br i1 %i.cmp, label %exit, label %outer.header, !llvm.loop !0

exit:
  ret void
}

!0 = distinct !{!0, !1}
!1 = !{!"llvm.loop.vectorize.enable"}
