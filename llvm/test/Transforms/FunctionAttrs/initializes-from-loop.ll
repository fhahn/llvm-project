; RUN: opt -passes=function-attrs -S < %s | FileCheck %s
;
; Soundness tests for the non-SCEV loop access range helper consumed by
; PostOrderFunctionAttrsPass::inferInitializes. Each test demonstrates a
; specific shape that the old helper mis-analysed (or asserted on); the
; helper now either reports the correct unsigned-domain range or bails
; out (no `initializes` attribute on the argument).

target datalayout = "e-m:o-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-n32:64-S128-Fn32"
target triple = "arm64-apple-macosx"

;-----------------------------------------------------------------------
; C1: zext IV cast must produce an unsigned-domain range.
;     Init = 0x80000000 (i32), TC = 5 -> bytes [0x80000000, 0x80000005).
;     Old helper produced [0xFFFFFFFF80000000, 0xFFFFFFFF80000006), a
;     disjoint over-claim that would let DSE clobber unrelated memory.
;-----------------------------------------------------------------------
define void @zext_init_high_bit(ptr noalias %dst) {
; CHECK-LABEL: define void @zext_init_high_bit(
; CHECK-SAME: ptr noalias writeonly captures(none) initializes((2147483648, 2147483653)) %dst)
entry:
  br label %loop
loop:
  %iv = phi i32 [ -2147483648, %entry ], [ %iv.next, %loop ]      ; 0x80000000
  %idx = zext i32 %iv to i64
  %p = getelementptr inbounds i8, ptr %dst, i64 %idx
  store i8 0, ptr %p, align 1
  %iv.next = add nuw nsw i32 %iv, 1
  %c = icmp ult i32 %iv.next, -2147483643                         ; 0x80000005
  br i1 %c, label %loop, label %exit
exit:
  ret void
}

;-----------------------------------------------------------------------
; M1: missing wrap flags on the IV add must inhibit inference. Without
;     `nuw` the i64 linear model the helper builds may diverge from the
;     IR's actual address evolution.
;-----------------------------------------------------------------------
define void @no_wrap_flags(ptr noalias %dst) {
; CHECK-LABEL: define void @no_wrap_flags(
; CHECK-SAME: ptr noalias writeonly captures(none) %dst)
entry:
  br label %loop
loop:
  %iv = phi i32 [ 0, %entry ], [ %iv.next, %loop ]
  %idx = zext i32 %iv to i64
  %p = getelementptr inbounds i8, ptr %dst, i64 %idx
  store i8 0, ptr %p, align 1
  %iv.next = add i32 %iv, 1                                       ; no nuw / nsw
  %c = icmp ult i32 %iv.next, 16
  br i1 %c, label %loop, label %exit
exit:
  ret void
}

;-----------------------------------------------------------------------
; M1b: sext consumer with only nuw on the IV must bail (signed consumer
;      requires nsw).
;-----------------------------------------------------------------------
define void @sext_consumer_nuw_only(ptr noalias %dst) {
; CHECK-LABEL: define void @sext_consumer_nuw_only(
; CHECK-SAME: ptr noalias writeonly captures(none) %dst)
entry:
  br label %loop
loop:
  %iv = phi i32 [ 0, %entry ], [ %iv.next, %loop ]
  %idx = sext i32 %iv to i64
  %p = getelementptr inbounds i8, ptr %dst, i64 %idx
  store i8 0, ptr %p, align 1
  %iv.next = add nuw i32 %iv, 1                                   ; nuw, no nsw
  %c = icmp slt i32 %iv.next, 16
  br i1 %c, label %loop, label %exit
exit:
  ret void
}

;-----------------------------------------------------------------------
; M2: ULT bound 0xC0000000 with i32 Init=0. The bound has the high bit
;     set; the old code read it via getSExtValue() which produced a tiny
;     negative i64. The correct unsigned trip count is 0xC0000000.
;-----------------------------------------------------------------------
define void @ult_bound_high_bit(ptr noalias %dst) {
; CHECK-LABEL: define void @ult_bound_high_bit(
; CHECK-SAME: ptr noalias writeonly captures(none) initializes((0, 3221225472)) %dst)
entry:
  br label %loop
loop:
  %iv = phi i32 [ 0, %entry ], [ %iv.next, %loop ]
  %idx = zext i32 %iv to i64
  %p = getelementptr inbounds i8, ptr %dst, i64 %idx
  store i8 0, ptr %p, align 1
  %iv.next = add nuw i32 %iv, 1
  %c = icmp ult i32 %iv.next, -1073741824                         ; 0xC0000000
  br i1 %c, label %loop, label %exit
exit:
  ret void
}

;-----------------------------------------------------------------------
; M3: i128 IV must bail (no truncation cast supported, and the bound
;     wider than i64 would assert on getSExtValue() in old code).
;-----------------------------------------------------------------------
define void @i128_iv(ptr noalias %dst) {
; CHECK-LABEL: define void @i128_iv(
; CHECK-SAME: ptr noalias writeonly captures(none) %dst)
entry:
  br label %loop
loop:
  %iv = phi i128 [ 0, %entry ], [ %iv.next, %loop ]
  %idx = trunc i128 %iv to i64
  %p = getelementptr inbounds i8, ptr %dst, i64 %idx
  store i8 0, ptr %p, align 1
  %iv.next = add nuw nsw i128 %iv, 1
  %c = icmp ult i128 %iv.next, 16
  br i1 %c, label %loop, label %exit
exit:
  ret void
}

;-----------------------------------------------------------------------
; M3b: i128 IV used directly with a wide bound that doesn't fit in i64
;      must bail rather than asserting in getSExtValue().
;-----------------------------------------------------------------------
define void @i128_iv_wide_bound(ptr noalias %dst) {
; CHECK-LABEL: define void @i128_iv_wide_bound(
; CHECK-SAME: ptr noalias writeonly captures(none) %dst)
entry:
  br label %loop
loop:
  %iv = phi i128 [ 0, %entry ], [ %iv.next, %loop ]
  %p = getelementptr inbounds i8, ptr %dst, i128 %iv
  store i8 0, ptr %p, align 1
  %iv.next = add nuw i128 %iv, 1
  %c = icmp ult i128 %iv.next, 18446744073709551616               ; 2^64
  br i1 %c, label %loop, label %exit
exit:
  ret void
}

;-----------------------------------------------------------------------
; M4: GEP in non-default address space must bail. AS 270 has a 32-bit
;     index width per the data layout above; the old helper would have
;     sign-extended an AS-modular offset (with the AS high bit set) into
;     a wrong-sign i64 ConstOff.
;-----------------------------------------------------------------------
define void @nondefault_as(ptr addrspace(270) noalias %dst) {
; CHECK-LABEL: define void @nondefault_as(
; CHECK-SAME: ptr addrspace(270) noalias writeonly captures(none) %dst)
entry:
  br label %loop
loop:
  %iv = phi i32 [ 0, %entry ], [ %iv.next, %loop ]
  %p = getelementptr inbounds i8, ptr addrspace(270) %dst, i32 %iv
  store i8 0, ptr addrspace(270) %p, align 1
  %iv.next = add nuw nsw i32 %iv, 1
  %c = icmp ult i32 %iv.next, 16
  br i1 %c, label %loop, label %exit
exit:
  ret void
}

;-----------------------------------------------------------------------
; M5: scalable vector store: type alloc size is scalable, must bail.
;-----------------------------------------------------------------------
define void @scalable_vec_store(ptr noalias %dst, <vscale x 4 x i32> %v) {
; CHECK-LABEL: define void @scalable_vec_store(
; CHECK-SAME: ptr noalias writeonly captures(none) %dst, <vscale x 4 x i32> %v)
entry:
  br label %loop
loop:
  %iv = phi i64 [ 0, %entry ], [ %iv.next, %loop ]
  %p = getelementptr inbounds <vscale x 4 x i32>, ptr %dst, i64 %iv
  store <vscale x 4 x i32> %v, ptr %p, align 16
  %iv.next = add nuw nsw i64 %iv, 1
  %c = icmp ult i64 %iv.next, 16
  br i1 %c, label %loop, label %exit
exit:
  ret void
}

;-----------------------------------------------------------------------
; M6: stride * constant-index in a GEP must overflow-check. Here the
;     element type's alloc size is INT64_MAX bytes; multiplying by the
;     constant first index of 2 overflows i64.
;-----------------------------------------------------------------------
%huge = type [9223372036854775807 x i8]   ; INT64_MAX

define void @stride_const_overflow(ptr noalias %dst) {
; CHECK-LABEL: define void @stride_const_overflow(
; CHECK-SAME: ptr noalias writeonly captures(none) %dst)
entry:
  br label %loop
loop:
  %iv = phi i64 [ 0, %entry ], [ %iv.next, %loop ]
  %p = getelementptr inbounds %huge, ptr %dst, i64 2, i64 %iv
  store i8 0, ptr %p, align 1
  %iv.next = add nuw nsw i64 %iv, 1
  %c = icmp ult i64 %iv.next, 16
  br i1 %c, label %loop, label %exit
exit:
  ret void
}

;-----------------------------------------------------------------------
; M7: stride > INT64_MAX as a uint64_t. Old code reinterpreted as a
;     negative int64_t and then called std::abs on the (potentially
;     INT64_MIN) IterStrideBytes. New code bails on stride > INT64_MAX.
;-----------------------------------------------------------------------
%hugeplus = type { [9223372036854775807 x i8], i8 }  ; 2^63 bytes

define void @stride_above_int64_max(ptr noalias %dst) {
; CHECK-LABEL: define void @stride_above_int64_max(
; CHECK-SAME: ptr noalias writeonly captures(none) %dst)
entry:
  br label %loop
loop:
  %iv = phi i64 [ 0, %entry ], [ %iv.next, %loop ]
  %p = getelementptr inbounds %hugeplus, ptr %dst, i64 %iv
  store i8 0, ptr %p, align 1
  %iv.next = add nuw nsw i64 %iv, 1
  %c = icmp ult i64 %iv.next, 16
  br i1 %c, label %loop, label %exit
exit:
  ret void
}

;-----------------------------------------------------------------------
; Positive control: a clean 8-element i32 store loop (i64 IV, no casts)
; should still produce initializes((0, 32)).
;-----------------------------------------------------------------------
define void @clean_loop(ptr noalias %dst) {
; CHECK-LABEL: define void @clean_loop(
; CHECK-SAME: ptr noalias writeonly captures(none) initializes((0, 32)) %dst)
entry:
  br label %loop
loop:
  %iv = phi i64 [ 0, %entry ], [ %iv.next, %loop ]
  %p = getelementptr inbounds i32, ptr %dst, i64 %iv
  store i32 0, ptr %p, align 4
  %iv.next = add nuw nsw i64 %iv, 1
  %c = icmp ult i64 %iv.next, 8
  br i1 %c, label %loop, label %exit
exit:
  ret void
}

;-----------------------------------------------------------------------
; Positive control: sext IV with a signed-domain cmp (slt) and nsw.
;-----------------------------------------------------------------------
define void @sext_signed_cmp(ptr noalias %dst) {
; CHECK-LABEL: define void @sext_signed_cmp(
; CHECK-SAME: ptr noalias writeonly captures(none) initializes((0, 16)) %dst)
entry:
  br label %loop
loop:
  %iv = phi i32 [ 0, %entry ], [ %iv.next, %loop ]
  %idx = sext i32 %iv to i64
  %p = getelementptr inbounds i8, ptr %dst, i64 %idx
  store i8 0, ptr %p, align 1
  %iv.next = add nsw i32 %iv, 1
  %c = icmp slt i32 %iv.next, 16
  br i1 %c, label %loop, label %exit
exit:
  ret void
}
