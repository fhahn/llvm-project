; RUN: opt -passes=loop-vectorize -force-vector-width=4 -enable-vplan-native-path -debug-only=loop-vectorize -disable-output %S/../outer_loop_memory_safety.ll 2>&1 | FileCheck %s

; The pair of accesses that could not be separated statically is registered as a
; predicate of the plan, which is materialized into a check block once the plan
; is selected for vectorization.

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
