; RUN: opt -passes=loop-vectorize -mtriple=riscv64 -mattr=+v \
; RUN:     -vectorizer-maximize-bandwidth -scalable-vectorization=on \
; RUN:     -force-tail-folding-style=data-with-evl -tail-folding-policy=must-fold-tail \
; RUN:     -S %s | FileCheck %s

; Regression test for a crash in the cost model. VPInstruction::computeCost
; for LastActiveLane/FirstActiveLane builds an experimental_cttz_elts cost
; query whose argument type is a scalable predicate. When maximize-bandwidth
; widens the VF, the predicate type is not a legal MVT, so
; getValueType(..., AllowUnknown=true) yields a non-vector EVT. RISCV's
; shouldExpandCttzElements then called getVectorElementType() on that scalar
; EVT, tripping "isVector() && Invalid vector type!". The target now guards the
; query with isVector(); a non-vector type is simply expanded.

; CHECK-LABEL: define i32 @last_active_lane_cttz_cost
; CHECK: ret i32

define i32 @last_active_lane_cttz_cost(ptr readonly %a, i64 %n) {
entry:
  br label %loop
loop:
  %iv = phi i64 [ 0, %entry ], [ %iv.next, %loop ]
  %any = phi i1 [ false, %entry ], [ %any.next, %loop ]
  %g = getelementptr inbounds float, ptr %a, i64 %iv
  %l = load float, ptr %g, align 4
  %c = fcmp olt float %l, 0.0
  %any.next = select i1 %c, i1 true, i1 %any
  %iv.next = add nuw nsw i64 %iv, 1
  %ec = icmp eq i64 %iv.next, %n
  br i1 %ec, label %exit, label %loop
exit:
  %z = zext i1 %c to i32
  %r = select i1 %any.next, i32 2, i32 %z
  ret i32 %r
}
