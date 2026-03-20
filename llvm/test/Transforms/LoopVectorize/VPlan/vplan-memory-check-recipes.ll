; RUN: opt -passes=loop-vectorize -force-vector-width=4 -force-vector-interleave=1 -disable-output -vplan-print-after="printFinalVPlan$" -S %s 2>&1 | FileCheck --check-prefixes=CHECK %s

; Verify that the VPlan-based memory runtime check path expands SCEV
; bounds directly into VPlan recipes in the memcheck block and generates
; bound0/bound1/found.conflict recipes there.
; The read-modify-write on %a prevents diff checks, forcing the
; overlap check (bound) path via VPlan recipes.

define void @bound_checks_vplan(ptr %a, ptr %b, ptr %c, i64 %n) {
; CHECK-LABEL: VPlan for loop in 'bound_checks_vplan'
; CHECK:       ir-bb<entry>:
; CHECK:         EMIT vp<%min.iters.check> = icmp ult ir<%umax>, ir<4>
; CHECK-NEXT:    EMIT branch-on-cond vp<%min.iters.check>
; CHECK-NEXT:  Successor(s): ir-bb<scalar.ph>, vector.memcheck
; CHECK-EMPTY:
; CHECK-NEXT:  vector.memcheck:
; CHECK-NEXT:    EMIT vp<[[SCALED:%.+]]> = shl ir<%umax>, ir<2>
; CHECK-NEXT:    EMIT vp<[[BOUND_C_HI:%.+]]> = ptradd ir<%c>, vp<[[SCALED]]>
; CHECK-NEXT:    EMIT vp<[[BOUND_A_HI:%.+]]> = ptradd ir<%a>, vp<[[SCALED]]>
; CHECK-NEXT:    EMIT vp<[[BOUND_B_HI:%.+]]> = ptradd ir<%b>, vp<[[SCALED]]>
; CHECK-NEXT:    EMIT vp<%bound0> = icmp ult ir<%c>, vp<[[BOUND_A_HI]]>
; CHECK-NEXT:    EMIT vp<%bound1> = icmp ult ir<%a>, vp<[[BOUND_C_HI]]>
; CHECK-NEXT:    EMIT vp<%found.conflict> = and vp<%bound0>, vp<%bound1>
; CHECK-NEXT:    EMIT vp<%bound0>.1 = icmp ult ir<%c>, vp<[[BOUND_B_HI]]>
; CHECK-NEXT:    EMIT vp<%bound1>.1 = icmp ult ir<%b>, vp<[[BOUND_C_HI]]>
; CHECK-NEXT:    EMIT vp<%found.conflict>.1 = and vp<%bound0>.1, vp<%bound1>.1
; CHECK-NEXT:    EMIT vp<%conflict.rdx> = or vp<%found.conflict>, vp<%found.conflict>.1
; CHECK-NEXT:    EMIT vp<%bound0>.2 = icmp ult ir<%a>, vp<[[BOUND_B_HI]]>
; CHECK-NEXT:    EMIT vp<%bound1>.2 = icmp ult ir<%b>, vp<[[BOUND_A_HI]]>
; CHECK-NEXT:    EMIT vp<%found.conflict>.2 = and vp<%bound0>.2, vp<%bound1>.2
; CHECK-NEXT:    EMIT vp<%conflict.rdx>.1 = or vp<%conflict.rdx>, vp<%found.conflict>.2
; CHECK-NEXT:    EMIT branch-on-cond vp<%conflict.rdx>.1
; CHECK-NEXT:  Successor(s): ir-bb<scalar.ph>, vector.ph
;
entry:
  br label %loop

loop:
  %iv = phi i64 [ 0, %entry ], [ %iv.next, %loop ]
  %gep.a.read = getelementptr inbounds i32, ptr %a, i64 %iv
  %load.a = load i32, ptr %gep.a.read, align 4
  %gep.b = getelementptr inbounds i32, ptr %b, i64 %iv
  %load.b = load i32, ptr %gep.b, align 4
  %sum = add i32 %load.a, %load.b
  %gep.c = getelementptr inbounds i32, ptr %c, i64 %iv
  store i32 %sum, ptr %gep.c, align 4
  store i32 %load.b, ptr %gep.a.read, align 4
  %iv.next = add nuw nsw i64 %iv, 1
  %cmp = icmp ult i64 %iv.next, %n
  br i1 %cmp, label %loop, label %exit

exit:
  ret void
}
