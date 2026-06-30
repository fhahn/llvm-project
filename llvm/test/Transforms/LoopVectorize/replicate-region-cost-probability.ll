; REQUIRES: asserts
; RUN: opt < %s -passes=loop-vectorize -force-vector-width=2 -force-vector-interleave=1 \
; RUN:   -disable-output -debug-only=loop-vectorize 2>&1 | FileCheck %s

; Tests that the VPlan cost model scales the cost of a recipe in a replicate
; region by the reciprocal entry probability of the region. The reciprocal is
; recovered from the branch_weights recorded on the region's branch-on-mask.
; Those weights are estimated during initial VPlan construction (from the
; original loop's block frequency info, which incorporates branch weights when
; present) and seeded onto the block, attached to the masked recipes by the
; predicator, and moved to the branch-on-mask when the region is created; so the
; cost model reads them directly from the VPlan rather than re-deriving them
; from the original IR on each query. The scalarized cost of the predicated udiv
; (5 per lane * 2 lanes = 10) is divided by the recovered reciprocal, so a less
; likely predicate yields a smaller per-iteration cost.

target datalayout = "e-m:o-i64:64-f80:128-n8:16:32:64-S128"

; The predicate is taken 1 in 4 iterations (reciprocal 4): cost 10 / 4 -> 2.5.
; CHECK-LABEL: LV: Checking a loop in 'predicated_udiv_p25'
; CHECK: Cost of 2.5 for VF 2: REPLICATE ir<%d> = udiv ir<%val>, ir<7>
define void @predicated_udiv_p25(ptr %a, i32 %n) {
entry:
  br label %loop

loop:
  %iv = phi i32 [ 0, %entry ], [ %iv.next, %latch ]
  %gep = getelementptr inbounds i32, ptr %a, i32 %iv
  %val = load i32, ptr %gep, align 4
  %cmp = icmp sgt i32 %val, 0
  br i1 %cmp, label %if.then, label %latch, !prof !1

if.then:
  %d = udiv i32 %val, 7
  store i32 %d, ptr %gep, align 4
  br label %latch

latch:
  %iv.next = add nuw nsw i32 %iv, 1
  %exitcond = icmp eq i32 %iv.next, %n
  br i1 %exitcond, label %exit, label %loop, !prof !0

exit:
  ret void
}

; The predicate is taken 1 in 8 iterations (reciprocal 8): cost 10 / 8 -> 1.25.
; A less likely predicate makes the predicated udiv look cheaper than at 1/4.
; CHECK-LABEL: LV: Checking a loop in 'predicated_udiv_p12'
; CHECK: Cost of 1.25 for VF 2: REPLICATE ir<%d> = udiv ir<%val>, ir<7>
define void @predicated_udiv_p12(ptr %a, i32 %n) {
entry:
  br label %loop

loop:
  %iv = phi i32 [ 0, %entry ], [ %iv.next, %latch ]
  %gep = getelementptr inbounds i32, ptr %a, i32 %iv
  %val = load i32, ptr %gep, align 4
  %cmp = icmp sgt i32 %val, 0
  br i1 %cmp, label %if.then, label %latch, !prof !2

if.then:
  %d = udiv i32 %val, 7
  store i32 %d, ptr %gep, align 4
  br label %latch

latch:
  %iv.next = add nuw nsw i32 %iv, 1
  %exitcond = icmp eq i32 %iv.next, %n
  br i1 %exitcond, label %exit, label %loop, !prof !0

exit:
  ret void
}

; Without profile data the cost model is still self-contained: the recorded
; reciprocal comes from block frequency info, which defaults to an even 50/50
; split for the unprofiled predicate (reciprocal 2), so cost 10 / 2 -> 5.
; CHECK-LABEL: LV: Checking a loop in 'predicated_udiv_no_profile'
; CHECK: Cost of 5 for VF 2: REPLICATE ir<%d> = udiv ir<%val>, ir<7>
define void @predicated_udiv_no_profile(ptr %a, i32 %n) {
entry:
  br label %loop

loop:
  %iv = phi i32 [ 0, %entry ], [ %iv.next, %latch ]
  %gep = getelementptr inbounds i32, ptr %a, i32 %iv
  %val = load i32, ptr %gep, align 4
  %cmp = icmp sgt i32 %val, 0
  br i1 %cmp, label %if.then, label %latch

if.then:
  %d = udiv i32 %val, 7
  store i32 %d, ptr %gep, align 4
  br label %latch

latch:
  %iv.next = add nuw nsw i32 %iv, 1
  %exitcond = icmp eq i32 %iv.next, %n
  br i1 %exitcond, label %exit, label %loop

exit:
  ret void
}

!0 = !{!"branch_weights", i32 1, i32 1000}
!1 = !{!"branch_weights", i32 1, i32 3}
!2 = !{!"branch_weights", i32 1, i32 7}
