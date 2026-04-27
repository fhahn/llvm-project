; RUN: opt -passes="inline,function(mark-loops-deferred-for-vectorization)" \
; RUN:   -debug-pass-manager %s -S 2>&1 | FileCheck %s

; PhaseOrdering regression guard for the inlining + deferred-unroll
; gating interaction.
;
; The early LoopFullUnrollPass(SkipVectorizableLoops=true) tags
; likely-vectorizable inner loops with the loop metadata
; "llvm.loop.unroll.deferred_for_vectorization". That metadata travels
; with the IR through subsequent inlining (it lives on the loop's
; latch branch). If MarkLoopsDeferredForVectorizationPass were to gate
; the late-unroll-after-vectorize ExtraFunctionPassManager on a
; function-level attribute alongside, the attribute would be erased
; when @callee is inlined into @caller, the gate would not fire on
; @caller, and the deferred loop would never receive the late on-demand
; unroll + cleanup.
;
; This test feeds in IR that already carries the deferred loop
; metadata on @callee (as if the early pipeline already ran). It asks
; only "after inlining @callee into @caller, does
; MarkLoopsDeferredForVectorizationPass fire the
; ShouldRunExtraUnrollAfterVectorize analysis on @caller?". The
; -debug-pass-manager output makes that observable directly.

target datalayout = "e-m:o-i64:64-i128:128-n32:64-S128"
target triple = "arm64-apple-macosx14"

define internal void @callee(ptr noundef %p) {
entry:
  br label %loop

loop:
  %iv = phi i64 [ 0, %entry ], [ %iv.next, %loop ]
  %v32 = trunc i64 %iv to i32
  %gep = getelementptr inbounds i32, ptr %p, i64 %iv
  store i32 %v32, ptr %gep, align 4
  %iv.next = add nuw nsw i64 %iv, 1
  %done = icmp eq i64 %iv.next, 64
  br i1 %done, label %exit, label %loop, !llvm.loop !0

exit:
  ret void
}

define void @caller(ptr noundef %p) {
entry:
  call void @callee(ptr noundef %p)
  ret void
}

!0 = distinct !{!0, !1}
!1 = !{!"llvm.loop.unroll.deferred_for_vectorization", i32 1}

; The analysis is registered on @caller after @callee was inlined into it,
; even though no function-level attribute travelled across the inlining.
; CHECK: Running pass: InlinerPass on (callee)
; CHECK: Running pass: InlinerPass on (caller)
; CHECK: Running pass: MarkLoopsDeferredForVectorizationPass on caller
; CHECK: Running analysis: ShouldRunExtraUnrollAfterVectorize on caller
