; RUN: opt -disable-verify -verify-analysis-invalidation=0 \
; RUN:     -eagerly-invalidate-analyses=0 -debug-pass-manager \
; RUN:     -passes='default<O2>' -S %s 2>&1 | FileCheck %s

target datalayout = "e-m:o-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-apple-macosx"

; @deferred_loop_unrolled has an innermost loop without vectorize-disable
; metadata, so the early LoopFullUnrollPass defers it and
; MarkLoopsDeferredForVectorizationPass sets the marker. LoopVectorize cannot
; vectorize it (loop-carried dependence through memory), so the late
; LoopFullUnrollPass fully unrolls it and the cleanup chain runs.
;
; CHECK:      Running pass: MarkLoopsDeferredForVectorizationPass on deferred_loop_unrolled
; CHECK-NEXT: Running analysis: ShouldRunExtraUnrollAfterVectorize on deferred_loop_unrolled
; CHECK:      Running pass: LoopVectorizePass on deferred_loop_unrolled
;
; The late unroll cleanup runs:
; CHECK:      Running pass: LoopFullUnrollPass on loop {{.*}} in function deferred_loop_unrolled
; CHECK:      Running pass: SROAPass on deferred_loop_unrolled
; CHECK:      Invalidating analysis: ShouldRunExtraUnrollAfterVectorize on deferred_loop_unrolled
; CHECK:      Running pass: MarkLateUnrollCleanupPass on deferred_loop_unrolled
; CHECK-NEXT: Running analysis: ShouldRunLateUnrollCleanup on deferred_loop_unrolled
; CHECK:      Running pass: InstCombinePass on deferred_loop_unrolled
; CHECK:      Invalidating analysis: ShouldRunLateUnrollCleanup on deferred_loop_unrolled

define void @deferred_loop_unrolled(ptr noalias %a, ptr noalias %b) {
entry:
  br label %loop

loop:
  %iv = phi i64 [ 0, %entry ], [ %iv.next, %loop ]
  %gep.a = getelementptr inbounds i32, ptr %a, i64 %iv
  %gep.b = getelementptr inbounds i32, ptr %b, i64 %iv
  %iv.next = add nuw nsw i64 %iv, 1
  %gep.a.next = getelementptr inbounds i32, ptr %a, i64 %iv.next
  %va = load i32, ptr %gep.a, align 4
  %vb = load i32, ptr %gep.b, align 4
  %add = add nsw i32 %va, %vb
  store i32 %add, ptr %gep.a.next, align 4
  %cmp = icmp ult i64 %iv.next, 16
  br i1 %cmp, label %loop, label %exit

exit:
  ret void
}

; @deferred_loop_vectorized has a deferred loop that LoopVectorize vectorizes.
; The late LoopFullUnrollPass runs, but does not unroll anything, so the
; cleanup chain does not run.
;
; CHECK:      Running pass: MarkLoopsDeferredForVectorizationPass on deferred_loop_vectorized
; CHECK-NEXT: Running analysis: ShouldRunExtraUnrollAfterVectorize on deferred_loop_vectorized
; CHECK:      Running pass: LoopVectorizePass on deferred_loop_vectorized
; CHECK:      Running pass: LoopFullUnrollPass on loop {{.*}} in function deferred_loop_vectorized
; CHECK:      Running pass: MarkLateUnrollCleanupPass on deferred_loop_vectorized
; CHECK-NOT:  Running analysis: ShouldRunLateUnrollCleanup on deferred_loop_vectorized
; CHECK:      Running pass: SLPVectorizerPass on deferred_loop_vectorized

define void @deferred_loop_vectorized(ptr noalias %dst, ptr noalias %a, ptr noalias %b) {
entry:
  br label %loop

loop:
  %iv = phi i64 [ 0, %entry ], [ %iv.next, %loop ]
  %gep.a = getelementptr inbounds i32, ptr %a, i64 %iv
  %gep.b = getelementptr inbounds i32, ptr %b, i64 %iv
  %gep.dst = getelementptr inbounds i32, ptr %dst, i64 %iv
  %va = load i32, ptr %gep.a, align 4
  %vb = load i32, ptr %gep.b, align 4
  %add = add nsw i32 %va, %vb
  store i32 %add, ptr %gep.dst, align 4
  %iv.next = add nuw nsw i64 %iv, 1
  %cmp = icmp ult i64 %iv.next, 16
  br i1 %cmp, label %loop, label %exit

exit:
  ret void
}

; @no_deferred_loop has vectorization disabled on its only loop, so the early
; LoopFullUnrollPass unrolls it and MarkLoopsDeferredForVectorizationPass does
; NOT set the marker. The late LoopFullUnrollPass + SROAPass + InstCombinePass
; must NOT run for this function, and the marker must NOT be invalidated for it
; (because it was never created).
;
; CHECK:      Running pass: MarkLoopsDeferredForVectorizationPass on no_deferred_loop
; CHECK-NOT:  Running analysis: ShouldRunExtraUnrollAfterVectorize on no_deferred_loop
; CHECK:      Running pass: LoopVectorizePass on no_deferred_loop
; CHECK-NOT:  Running pass: LoopFullUnrollPass on loop {{.*}} in function no_deferred_loop
; CHECK-NOT:  Running analysis: ShouldRunLateUnrollCleanup on no_deferred_loop
;
; Sentinel to terminate the negative checks above:
; CHECK:      Running pass: SLPVectorizerPass on no_deferred_loop

define void @no_deferred_loop(ptr noalias %dst, ptr noalias %a, ptr noalias %b) {
entry:
  br label %loop

loop:
  %iv = phi i64 [ 0, %entry ], [ %iv.next, %loop ]
  %gep.a = getelementptr inbounds i32, ptr %a, i64 %iv
  %gep.b = getelementptr inbounds i32, ptr %b, i64 %iv
  %gep.dst = getelementptr inbounds i32, ptr %dst, i64 %iv
  %va = load i32, ptr %gep.a, align 4
  %vb = load i32, ptr %gep.b, align 4
  %add = add nsw i32 %va, %vb
  store i32 %add, ptr %gep.dst, align 4
  %iv.next = add nuw nsw i64 %iv, 1
  %cmp = icmp ult i64 %iv.next, 4
  br i1 %cmp, label %loop, label %exit, !llvm.loop !0

exit:
  ret void
}

!0 = distinct !{!0, !1}
!1 = !{!"llvm.loop.vectorize.enable", i1 false}
