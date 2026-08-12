; RUN: opt -passes=loop-vectorize -force-vector-width=4 -force-vector-interleave=1 \
; RUN:   -vplan-print-after=VPlanTransforms::makeScalarizationDecisions \
; RUN:   -disable-output %s 2>&1 | FileCheck %s

; Intrinsics whose side-effects are invariant, and extractvalue from an
; aggregate defined outside the loop, are executed as a single scalar. The
; decision is made in VPlan by makeScalarizationDecisions, so they show up as
; CLONE (single-scalar) rather than as unscalarized EMIT ... = call recipes.

define void @invariant_side_effects(ptr noalias %dst, i64 %n, i1 %c) {
; CHECK-LABEL: VPlan for loop in 'invariant_side_effects'
; CHECK:      vector.body:
; CHECK-NEXT:   ir<%iv> = WIDEN-INDUCTION ir<0>, ir<1>, vp<%0>
; CHECK-NEXT:   CLONE call @llvm.experimental.noalias.scope.decl(ir<!2>)
; CHECK-NEXT:   CLONE call @llvm.lifetime.start.p0(ir<%obj>)
; CHECK-NEXT:   CLONE call @llvm.assume(ir<%c>)
; CHECK-NEXT:   CLONE ir<%gep> = getelementptr ir<%dst>, ir<%iv>
; CHECK-NEXT:   vp<%4> = vector-pointer i64, ir<%gep>, ir<1>
; CHECK-NEXT:   WIDEN store vp<%4>, ir<%iv>
; CHECK-NEXT:   CLONE call @llvm.lifetime.end.p0(ir<%obj>)
;
entry:
  %obj = alloca i64, align 8
  br label %loop

loop:
  %iv = phi i64 [ 0, %entry ], [ %iv.next, %loop ]
  call void @llvm.experimental.noalias.scope.decl(metadata !0)
  call void @llvm.lifetime.start.p0(ptr %obj)
  tail call void @llvm.assume(i1 %c)
  %gep = getelementptr i64, ptr %dst, i64 %iv
  store i64 %iv, ptr %gep, align 8
  call void @llvm.lifetime.end.p0(ptr %obj)
  %iv.next = add i64 %iv, 1
  %ec = icmp eq i64 %iv.next, %n
  br i1 %ec, label %exit, label %loop

exit:
  ret void
}

; An extractvalue whose aggregate is defined outside the loop produces the same
; value in every lane, so a single scalar instance is enough even though its
; result is used by a widened recipe.

define void @extractvalue_from_invariant_aggregate(ptr noalias %dst, i64 %n,
                                                   {i64, i64} %sv) {
; CHECK-LABEL: VPlan for loop in 'extractvalue_from_invariant_aggregate'
; CHECK:      vector.body:
; CHECK-NEXT:   ir<%iv> = WIDEN-INDUCTION ir<0>, ir<1>, vp<%0>
; CHECK-NEXT:   CLONE ir<%a> = extractvalue ir<%sv>
; CHECK-NEXT:   EMIT ir<%add> = add ir<%iv>, ir<%a>
;
entry:
  br label %loop

loop:
  %iv = phi i64 [ 0, %entry ], [ %iv.next, %loop ]
  %a = extractvalue { i64, i64 } %sv, 0
  %add = add i64 %iv, %a
  %gep = getelementptr i64, ptr %dst, i64 %iv
  store i64 %add, ptr %gep, align 8
  %iv.next = add i64 %iv, 1
  %ec = icmp eq i64 %iv.next, %n
  br i1 %ec, label %exit, label %loop

exit:
  ret void
}

declare void @llvm.experimental.noalias.scope.decl(metadata)
declare void @llvm.lifetime.start.p0(ptr captures(none))
declare void @llvm.lifetime.end.p0(ptr captures(none))
declare void @llvm.assume(i1)

!0 = !{!1}
!1 = distinct !{!1, !2, !"s"}
!2 = distinct !{!2, !"s"}
