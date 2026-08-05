; RUN: opt -passes=loop-vectorize -force-vector-width=4 -S %s | FileCheck %s

; llvm.loop.parallel_accesses only asserts the absence of cross-iteration
; dependences. It does not make a volatile or atomic access safe to widen, as
; that would change the number and the atomicity of the accesses.
define void @volatile_load_in_parallel_loop(ptr %q, ptr %p) {
; CHECK-LABEL: define void @volatile_load_in_parallel_loop(
; CHECK-NOT:     vector.body
;
entry:
  br label %loop

loop:
  %iv = phi i64 [ 0, %entry ], [ %iv.next, %loop ]
  %gp = getelementptr inbounds i32, ptr %p, i64 %iv
  %v = load volatile i32, ptr %gp, align 4, !llvm.access.group !1
  %gq = getelementptr inbounds i32, ptr %q, i64 %iv
  store i32 %v, ptr %gq, align 4, !llvm.access.group !1
  %iv.next = add nuw nsw i64 %iv, 1
  %ec = icmp eq i64 %iv.next, 1024
  br i1 %ec, label %exit, label %loop, !llvm.loop !0

exit:
  ret void
}

define void @monotonic_load_in_parallel_loop(ptr %q, ptr %p) {
; CHECK-LABEL: define void @monotonic_load_in_parallel_loop(
; CHECK-NOT:     vector.body
;
entry:
  br label %loop

loop:
  %iv = phi i64 [ 0, %entry ], [ %iv.next, %loop ]
  %gp = getelementptr inbounds i32, ptr %p, i64 %iv
  %v = load atomic i32, ptr %gp monotonic, align 4, !llvm.access.group !1
  %gq = getelementptr inbounds i32, ptr %q, i64 %iv
  store i32 %v, ptr %gq, align 4, !llvm.access.group !1
  %iv.next = add nuw nsw i64 %iv, 1
  %ec = icmp eq i64 %iv.next, 1024
  br i1 %ec, label %exit, label %loop, !llvm.loop !0

exit:
  ret void
}

; Unordered atomics do not trip any assertion, but widening them to plain
; vector accesses drops their atomicity.
define void @unordered_atomics_in_parallel_loop(ptr %q, ptr %p) {
; CHECK-LABEL: define void @unordered_atomics_in_parallel_loop(
; CHECK-NOT:     vector.body
;
entry:
  br label %loop

loop:
  %iv = phi i64 [ 0, %entry ], [ %iv.next, %loop ]
  %gp = getelementptr inbounds i32, ptr %p, i64 %iv
  %v = load atomic i32, ptr %gp unordered, align 4, !llvm.access.group !1
  %add = add i32 %v, 1
  %gq = getelementptr inbounds i32, ptr %q, i64 %iv
  store atomic i32 %add, ptr %gq unordered, align 4, !llvm.access.group !1
  %iv.next = add nuw nsw i64 %iv, 1
  %ec = icmp eq i64 %iv.next, 1024
  br i1 %ec, label %exit, label %loop, !llvm.loop !0

exit:
  ret void
}

; A volatile load from a loop-invariant address would additionally be treated as
; uniform, performing one load per vector iteration instead of one per original
; iteration.
define void @uniform_volatile_load_in_parallel_loop(ptr noalias %q, ptr noalias %p, i64 %n) {
; CHECK-LABEL: define void @uniform_volatile_load_in_parallel_loop(
; CHECK-NOT:     vector.body
;
entry:
  br label %loop

loop:
  %iv = phi i64 [ 0, %entry ], [ %iv.next, %loop ]
  %v = load volatile i32, ptr %p, align 4, !llvm.access.group !1
  %gq = getelementptr inbounds i32, ptr %q, i64 %iv
  store i32 %v, ptr %gq, align 4, !llvm.access.group !1
  %iv.next = add nuw nsw i64 %iv, 1
  %ec = icmp eq i64 %iv.next, %n
  br i1 %ec, label %exit, label %loop, !llvm.loop !0

exit:
  ret void
}

!0 = distinct !{!0, !2}
!1 = distinct !{}
!2 = !{!"llvm.loop.parallel_accesses", !1}
