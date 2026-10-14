; RUN: opt -passes='function(mark-loops-deferred-for-vectorization,mark-late-unroll-cleanup)' \
; RUN:     -debug-pass-manager -disable-output %s 2>&1 | FileCheck --check-prefix=PM %s
; RUN: opt -passes='function(mark-loops-deferred-for-vectorization,mark-late-unroll-cleanup)' \
; RUN:     -S %s | FileCheck --check-prefix=IR %s

; MarkLoopsDeferredForVectorizationPass computes the
; ShouldRunExtraUnrollAfterVectorize marker analysis only for functions with a
; loop marked llvm.loop.unroll.deferred_for_vectorization.
; MarkLateUnrollCleanupPass computes the ShouldRunLateUnrollCleanup marker
; analysis only for functions with the late-unroll-performed attribute, and
; consumes the attribute.

; PM-LABEL: Running pass: MarkLoopsDeferredForVectorizationPass on marked_loop
; PM-NEXT:  Running analysis: LoopAnalysis on marked_loop
; PM-NEXT:  Running analysis: ShouldRunExtraUnrollAfterVectorize on marked_loop
; PM-NEXT:  Running pass: MarkLateUnrollCleanupPass on marked_loop
; PM-NEXT:  Running pass: MarkLoopsDeferredForVectorizationPass on unmarked_loop
define void @marked_loop(ptr %dst) {
entry:
  br label %loop

loop:
  %iv = phi i64 [ 0, %entry ], [ %iv.next, %loop ]
  %gep = getelementptr inbounds i8, ptr %dst, i64 %iv
  store i8 0, ptr %gep, align 1
  %iv.next = add nuw nsw i64 %iv, 1
  %ec = icmp eq i64 %iv.next, 16
  br i1 %ec, label %exit, label %loop, !llvm.loop !0

exit:
  ret void
}

; PM-NEXT:  Running analysis: LoopAnalysis on unmarked_loop
; PM-NEXT:  Running pass: MarkLateUnrollCleanupPass on unmarked_loop
; PM-NEXT:  Running pass: MarkLoopsDeferredForVectorizationPass on late_unroll_performed
define void @unmarked_loop(ptr %dst) {
entry:
  br label %loop

loop:
  %iv = phi i64 [ 0, %entry ], [ %iv.next, %loop ]
  %gep = getelementptr inbounds i8, ptr %dst, i64 %iv
  store i8 0, ptr %gep, align 1
  %iv.next = add nuw nsw i64 %iv, 1
  %ec = icmp eq i64 %iv.next, 16
  br i1 %ec, label %exit, label %loop

exit:
  ret void
}

; PM-NEXT:  Running analysis: LoopAnalysis on late_unroll_performed
; PM-NEXT:  Running pass: MarkLateUnrollCleanupPass on late_unroll_performed
; PM-NEXT:  Running analysis: ShouldRunLateUnrollCleanup on late_unroll_performed
; PM-NOT:   ShouldRun

; The attribute is consumed.
; IR-LABEL: define void @late_unroll_performed() {
; IR-NOT:   late-unroll-performed
define void @late_unroll_performed() "late-unroll-performed" {
  ret void
}

!0 = distinct !{!0, !1}
!1 = !{!"llvm.loop.unroll.deferred_for_vectorization", i32 150}
