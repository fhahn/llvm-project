; This is a known-crash regression input: computeVPlanOuterloopVF computes
; VF = max(1, RegSize / WidestType) for the VPlan-native outer-loop path with
; UserVF == 0, WITHOUT rounding down to a power of two (the inner-loop path uses
; llvm::bit_floor). With RegSize=128 (x86 SSE) and WidestType=i20, 128/20 = 6,
; a non-power-of-two VF, which trips `assert(isPowerOf2_32(VF))` in
; computeVPlanOuterloopVF (LoopVectorizationPlanner.cpp). In a release (NDEBUG)
; build the assert is gone and an illegal non-power-of-two fixed-width VPlan is
; built and executed instead. XFAIL until the VF is clamped with bit_floor.
;
; XFAIL: *
; RUN: opt -passes=loop-vectorize -enable-vplan-native-path -S < %s | FileCheck %s

target datalayout = "e-m:e-i64:64-f80:128-n8:16:32:64-S128"
target triple = "x86_64-unknown-linux-gnu"

; CHECK-LABEL: @outer(
define void @outer(ptr %a, i64 %n, i64 %m) {
entry:
  br label %outer

outer:
  %i = phi i64 [ 0, %entry ], [ %i.next, %outer.latch ]
  br label %inner

inner:
  %j = phi i64 [ 0, %outer ], [ %j.next, %inner ]
  %idx = mul i64 %i, %m
  %idx2 = add i64 %idx, %j
  %g = getelementptr i20, ptr %a, i64 %idx2
  %l = load i20, ptr %g
  %add = add i20 %l, %l
  store i20 %add, ptr %g
  %j.next = add i64 %j, 1
  %ec.inner = icmp eq i64 %j.next, %m
  br i1 %ec.inner, label %outer.latch, label %inner

outer.latch:
  %i.next = add i64 %i, 1
  %ec.outer = icmp eq i64 %i.next, %n
  br i1 %ec.outer, label %exit, label %outer, !llvm.loop !0

exit:
  ret void
}

!0 = distinct !{!0, !1}
!1 = !{!"llvm.loop.vectorize.enable", i1 true}
