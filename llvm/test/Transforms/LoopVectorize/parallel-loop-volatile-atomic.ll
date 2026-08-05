; RUN: opt -passes=loop-vectorize -force-vector-width=4 -S %s | FileCheck %s

; llvm.loop.parallel_accesses only asserts the absence of cross-iteration
; dependences. It does not make a volatile or atomic access safe to widen, as
; that would change the number and the atomicity of the accesses.

define void @volatile_load(ptr %src, ptr %dst, i64 %n) {
; CHECK-LABEL: define void @volatile_load(
; CHECK-NOT:     vector.body
;
entry:
  br label %loop

loop:
  %iv = phi i64 [ 0, %entry ], [ %iv.next, %loop ]
  %gep.src = getelementptr inbounds i32, ptr %src, i64 %iv
  %l = load volatile i32, ptr %gep.src, align 4, !llvm.access.group !1
  %gep.dst = getelementptr inbounds i32, ptr %dst, i64 %iv
  store i32 %l, ptr %gep.dst, align 4, !llvm.access.group !1
  %iv.next = add nuw nsw i64 %iv, 1
  %ec = icmp eq i64 %iv.next, %n
  br i1 %ec, label %exit, label %loop, !llvm.loop !0

exit:
  ret void
}

define void @monotonic_load(ptr %src, ptr %dst, i64 %n) {
; CHECK-LABEL: define void @monotonic_load(
; CHECK-NOT:     vector.body
;
entry:
  br label %loop

loop:
  %iv = phi i64 [ 0, %entry ], [ %iv.next, %loop ]
  %gep.src = getelementptr inbounds i32, ptr %src, i64 %iv
  %l = load atomic i32, ptr %gep.src monotonic, align 4, !llvm.access.group !1
  %gep.dst = getelementptr inbounds i32, ptr %dst, i64 %iv
  store i32 %l, ptr %gep.dst, align 4, !llvm.access.group !1
  %iv.next = add nuw nsw i64 %iv, 1
  %ec = icmp eq i64 %iv.next, %n
  br i1 %ec, label %exit, label %loop, !llvm.loop !0

exit:
  ret void
}

; Unordered atomics hit no assertion, but widening them drops their atomicity.
define void @unordered_load(ptr %src, ptr %dst, i64 %n) {
; CHECK-LABEL: define void @unordered_load(
; CHECK-NOT:     vector.body
;
entry:
  br label %loop

loop:
  %iv = phi i64 [ 0, %entry ], [ %iv.next, %loop ]
  %gep.src = getelementptr inbounds i32, ptr %src, i64 %iv
  %l = load atomic i32, ptr %gep.src unordered, align 4, !llvm.access.group !1
  %gep.dst = getelementptr inbounds i32, ptr %dst, i64 %iv
  store i32 %l, ptr %gep.dst, align 4, !llvm.access.group !1
  %iv.next = add nuw nsw i64 %iv, 1
  %ec = icmp eq i64 %iv.next, %n
  br i1 %ec, label %exit, label %loop, !llvm.loop !0

exit:
  ret void
}

define void @unordered_store(ptr %src, ptr %dst, i64 %n) {
; CHECK-LABEL: define void @unordered_store(
; CHECK-NOT:     vector.body
;
entry:
  br label %loop

loop:
  %iv = phi i64 [ 0, %entry ], [ %iv.next, %loop ]
  %gep.src = getelementptr inbounds i32, ptr %src, i64 %iv
  %l = load i32, ptr %gep.src, align 4, !llvm.access.group !1
  %gep.dst = getelementptr inbounds i32, ptr %dst, i64 %iv
  store atomic i32 %l, ptr %gep.dst unordered, align 4, !llvm.access.group !1
  %iv.next = add nuw nsw i64 %iv, 1
  %ec = icmp eq i64 %iv.next, %n
  br i1 %ec, label %exit, label %loop, !llvm.loop !0

exit:
  ret void
}

; A volatile load from a loop-invariant address must not become one load per
; vector iteration.
define void @volatile_load_invariant_address(ptr %src, ptr %dst, i64 %n) {
; CHECK-LABEL: define void @volatile_load_invariant_address(
; CHECK-NOT:     vector.body
;
entry:
  br label %loop

loop:
  %iv = phi i64 [ 0, %entry ], [ %iv.next, %loop ]
  %l = load volatile i32, ptr %src, align 4, !llvm.access.group !1
  %gep.dst = getelementptr inbounds i32, ptr %dst, i64 %iv
  store i32 %l, ptr %gep.dst, align 4, !llvm.access.group !1
  %iv.next = add nuw nsw i64 %iv, 1
  %ec = icmp eq i64 %iv.next, %n
  br i1 %ec, label %exit, label %loop, !llvm.loop !0

exit:
  ret void
}

!0 = distinct !{!0, !2}
!1 = distinct !{}
!2 = !{!"llvm.loop.parallel_accesses", !1}
