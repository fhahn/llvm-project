; RUN: opt -passes=loop-vectorize -force-vector-width=4 -enable-vplan-native-path -debug-only=loop-vectorize -disable-output %S/../outer_loop_memory_safety.ll 2>&1 | FileCheck %s

; Baseline: outer-loop vectorization widens this loop directly via a masked
; gather. There is no proof that the two accesses do not overlap and no
; memory-check block is materialized.

; CHECK-LABEL: LV: Checking a loop in 'store_may_alias_load'
; CHECK:      VPlan ' for VF={4},UF>=1' {
; CHECK:      ir-bb<outer.ph>:
; CHECK-NEXT: Successor(s): scalar.ph, vector.ph
;
; CHECK:      VPlan 'Final VPlan for VF={4},UF={1}' {
; CHECK:      ir-bb<outer.ph>:
; CHECK-NEXT:   EMIT vp<%min.iters.check> = icmp ult ir<%N>, ir<4>
; CHECK-NEXT:   EMIT branch-on-cond vp<%min.iters.check>
; CHECK-NEXT: Successor(s): ir-bb<scalar.ph>, vector.ph
