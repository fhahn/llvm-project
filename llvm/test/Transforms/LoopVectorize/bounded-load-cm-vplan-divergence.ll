; RUN: not --crash opt -passes=loop-vectorize -S %s 2>&1 | FileCheck --check-prefix=ASSERT %s
; RUN: not --crash opt -passes=loop-vectorize -force-vector-width=4 -S %s 2>&1 | FileCheck --check-prefix=ASSERT %s
; REQUIRES: asserts

target datalayout = "e-m:o-i64:64-i128:128-n32:64-S128"
target triple = "arm64-apple-macosx"

; Tests for the bound-recognition divergence between the legacy cost model and
; VPlan.
;
; The bound is recognised twice, by two different mechanisms:
;   - legacy: LoopVectorizationLegality::getBoundForConsecutiveLoad, which asks
;     PSE.getSCEV() about the IR;
;   - VPlan: canWidenBoundedLoad, which asks vputils::getSCEVExprForVPValue
;     about the recipes.
;
; getSCEVExprForVPValue handles a fixed opcode list, while SCEV-on-IR sees
; through far more. When an address chain contains an opcode the former does
; not model, the legacy CM reports CM_Widen but VPlan declines, and the load
; falls through to the generic consecutive path. That path addresses part P as
; `part0 + P*VF`, which is exactly the formula a bounded load must not use --
; the correct one re-derives A[(i + P*VF) % 2^N] per part. For UF > 1 the
; access then runs past the 2^N window.
;
; In an assertions build the divergence trips the guard in tryToWidenMemory,
; which is what this file pins. Without assertions the guard is compiled out
; and the out-of-bounds addressing is emitted instead -- see
; bounded-load-cm-vplan-divergence-codegen.ll for that half, and note it has
; been confirmed to fault against a guard page.
;
; FIXME: Neither function should reach this assert.

; ASSERT: unpredicated bounded loads must be widened in

; `ashr X, 0` is a no-op SCEV-wise but has no case in getSCEVExprForVPValue.
; Needs no -force-* flags: the cost model picks UF=4 on its own, so with a bound
; of 8 the release build reads A[0..15].
define i32 @bounded_load_bound8_ashr_divergence(ptr %A, i64 %n) {
entry:
  br label %loop

loop:
  %iv = phi i64 [ 0, %entry ], [ %iv.next, %loop ]
  %sum = phi i32 [ 0, %entry ], [ %sum.next, %loop ]
  %m = and i64 %iv, 7
  %b = ashr i64 %m, 0
  %gep = getelementptr inbounds i32, ptr %A, i64 %b
  %lv = load i32, ptr %gep, align 4
  %sum.next = add i32 %sum, %lv
  %iv.next = add nuw nsw i64 %iv, 1
  %c = icmp eq i64 %iv.next, %n
  br i1 %c, label %exit, label %loop

exit:
  ret i32 %sum.next
}
