; RUN: opt -passes=inline -S %s | FileCheck %s

; A loop whose full unrolling was deferred until after vectorization carries
; the extra size of the eventual unroll in its loop metadata. The inliner must
; charge that cost while the loop is still rolled, but only for loops that are
; live after inlining.

; CHECK-LABEL: define void @caller_large_cost(
; CHECK:         call void @deferred_large_cost(
define void @caller_large_cost(ptr %p) {
  call void @deferred_large_cost(ptr %p)
  ret void
}

; CHECK-LABEL: define void @caller_small_cost(
; CHECK-NOT:     call void @deferred_small_cost(
define void @caller_small_cost(ptr %p) {
  call void @deferred_small_cost(ptr %p)
  ret void
}

; The deferred loop is dead after inlining with %skip = true.
; CHECK-LABEL: define void @caller_dead_loop(
; CHECK-NOT:     call void @deferred_maybe_dead(
define void @caller_dead_loop(ptr %p) {
  call void @deferred_maybe_dead(ptr %p, i1 true)
  ret void
}

; CHECK-LABEL: define void @caller_live_loop(
; CHECK:         call void @deferred_maybe_dead(
define void @caller_live_loop(ptr %p, i1 %c) {
  call void @deferred_maybe_dead(ptr %p, i1 %c)
  ret void
}

; The marker travels with the loop through inlining: @wrapper_large_cost
; contains the deferred loop of @deferred_large_cost_always_inline after it
; is inlined, so inlining @wrapper_large_cost into its caller is charged the
; extra cost too.
; CHECK-LABEL: define void @wrapper_large_cost(
; CHECK-NOT:     call void @deferred_large_cost_always_inline(
; CHECK:         br i1 {{.*}}, !llvm.loop
; CHECK-LABEL: define void @caller_wrapper_large_cost(
; CHECK:         call void @wrapper_large_cost(
define void @wrapper_large_cost(ptr %p) {
  call void @deferred_large_cost_always_inline(ptr %p)
  ret void
}

define void @caller_wrapper_large_cost(ptr %p) {
  call void @wrapper_large_cost(ptr %p)
  ret void
}

; Same with a small extra cost: both calls are inlined.
; CHECK-LABEL: define void @caller_wrapper_small_cost(
; CHECK-NOT:     call void @wrapper_small_cost(
; CHECK-NOT:     call void @deferred_small_cost_always_inline(
; CHECK:         ret void
define void @wrapper_small_cost(ptr %p) {
  call void @deferred_small_cost_always_inline(ptr %p)
  ret void
}

define void @caller_wrapper_small_cost(ptr %p) {
  call void @wrapper_small_cost(ptr %p)
  ret void
}

define void @deferred_large_cost_always_inline(ptr %p) alwaysinline {
entry:
  br label %loop

loop:
  %iv = phi i64 [ 0, %entry ], [ %iv.next, %loop ]
  %gep = getelementptr inbounds i32, ptr %p, i64 %iv
  store i32 0, ptr %gep, align 4
  %iv.next = add nuw nsw i64 %iv, 1
  %ec = icmp eq i64 %iv.next, 64
  br i1 %ec, label %exit, label %loop, !llvm.loop !5

exit:
  ret void
}

define void @deferred_small_cost_always_inline(ptr %p) alwaysinline {
entry:
  br label %loop

loop:
  %iv = phi i64 [ 0, %entry ], [ %iv.next, %loop ]
  %gep = getelementptr inbounds i32, ptr %p, i64 %iv
  store i32 0, ptr %gep, align 4
  %iv.next = add nuw nsw i64 %iv, 1
  %ec = icmp eq i64 %iv.next, 64
  br i1 %ec, label %exit, label %loop, !llvm.loop !6

exit:
  ret void
}

define void @deferred_large_cost(ptr %p) {
entry:
  br label %loop

loop:
  %iv = phi i64 [ 0, %entry ], [ %iv.next, %loop ]
  %gep = getelementptr inbounds i32, ptr %p, i64 %iv
  store i32 0, ptr %gep, align 4
  %iv.next = add nuw nsw i64 %iv, 1
  %ec = icmp eq i64 %iv.next, 64
  br i1 %ec, label %exit, label %loop, !llvm.loop !0

exit:
  ret void
}

define void @deferred_small_cost(ptr %p) {
entry:
  br label %loop

loop:
  %iv = phi i64 [ 0, %entry ], [ %iv.next, %loop ]
  %gep = getelementptr inbounds i32, ptr %p, i64 %iv
  store i32 0, ptr %gep, align 4
  %iv.next = add nuw nsw i64 %iv, 1
  %ec = icmp eq i64 %iv.next, 64
  br i1 %ec, label %exit, label %loop, !llvm.loop !2

exit:
  ret void
}

define void @deferred_maybe_dead(ptr %p, i1 %skip) {
entry:
  br i1 %skip, label %exit, label %loop

loop:
  %iv = phi i64 [ 0, %entry ], [ %iv.next, %loop ]
  %gep = getelementptr inbounds i32, ptr %p, i64 %iv
  store i32 0, ptr %gep, align 4
  %iv.next = add nuw nsw i64 %iv, 1
  %ec = icmp eq i64 %iv.next, 64
  br i1 %ec, label %exit, label %loop, !llvm.loop !4

exit:
  ret void
}

!0 = distinct !{!0, !1}
!1 = !{!"llvm.loop.unroll.deferred_for_vectorization", i32 1000}
!2 = distinct !{!2, !3}
!3 = !{!"llvm.loop.unroll.deferred_for_vectorization", i32 1}
!4 = distinct !{!4, !1}
!5 = distinct !{!5, !1}
!6 = distinct !{!6, !3}
