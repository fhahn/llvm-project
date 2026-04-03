; REQUIRES: asserts
; RUN: opt -passes=loop-vectorize -force-vector-interleave=1 \
; RUN:   -enable-cost-based-tail-folding -debug-only=loop-vectorize \
; RUN:   -disable-output %s 2>&1 | FileCheck %s

; Cost-based tail folding builds tail-folded VPlans that are then costed by
; computeBestVF() with the primary (scalar-epilogue) cost model. That is only
; reliable when tail folding does not scalarize an op the primary model would
; widen: a tail-fold-scalarized op becomes scalar-with-predication, whose cost
; the primary model splits across precomputeCosts' scalarization set and a
; skipped recipe (i.e. mis-costs it). The planner therefore declines to offer
; tail-folded plans for loops where tail folding scalarizes.

target datalayout = "e-m:e-p:64:64-i64:64-i128:128-n32:64-S128"
target triple = "aarch64-linux-gnu"

; Store to a non-affine address: scalarized under tail folding, so the
; tail-folded plans are not built.
; CHECK-LABEL: LV: Checking a loop in 'scalarized_store'
; CHECK: LV: Not building tail-folded plans: tail folding scalarizes an op
define void @scalarized_store(ptr noalias %dst) {
entry:
  br label %loop
loop:
  %iv = phi i64 [ 0, %entry ], [ %iv.next, %loop ]
  %sq = mul nuw nsw i64 %iv, %iv
  %gd = getelementptr inbounds i32, ptr %dst, i64 %sq
  store i32 42, ptr %gd, align 4
  %iv.next = add nuw nsw i64 %iv, 1
  %ec = icmp eq i64 %iv.next, 19
  br i1 %ec, label %exit, label %loop
exit:
  ret void
}

; Consecutive store on SVE: widened-and-masked (not scalarized) under tail
; folding, so tail-folded plans are built and the gate stays out of the way.
; CHECK-LABEL: LV: Checking a loop in 'widened_masked_store'
; CHECK-NOT: LV: Not building tail-folded plans
define void @widened_masked_store(ptr noalias %dst) #0 {
entry:
  br label %loop
loop:
  %iv = phi i64 [ 0, %entry ], [ %iv.next, %loop ]
  %gd = getelementptr inbounds i32, ptr %dst, i64 %iv
  store i32 42, ptr %gd, align 4
  %iv.next = add nuw nsw i64 %iv, 1
  %ec = icmp eq i64 %iv.next, 1024
  br i1 %ec, label %exit, label %loop
exit:
  ret void
}

attributes #0 = { "target-features"="+sve" }
