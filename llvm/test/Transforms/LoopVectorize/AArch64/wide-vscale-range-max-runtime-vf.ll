; RUN: opt -passes=loop-vectorize -mtriple=aarch64-unknown-linux-gnu -mattr=+sve -S %s | FileCheck %s

; The maximum runtime VF is VF's minimum value multiplied by the maximum value
; of vscale. Check that this product is computed in 64 bits: for
; vscale_range(1, 1073741824) and a VF of vscale x 4 it is 2^32, which does not
; divide the trip count of 8, so the tail must be folded. Computing the product
; in 32 bits truncates it to 0, which makes the fixed VF of 4 win instead and
; wrongly concludes that no tail is needed for any chosen VF.
define void @wide_vscale_range_needs_tail_folding(ptr noalias %A) #0 {
; CHECK-LABEL: define void @wide_vscale_range_needs_tail_folding(
; CHECK-NOT:     scalar.ph
; CHECK:         call <vscale x 4 x i1> @llvm.get.active.lane.mask
; CHECK-NOT:     scalar.ph
;
entry:
  br label %loop

loop:
  %iv = phi i64 [ 0, %entry ], [ %iv.next, %loop ]
  %gep = getelementptr inbounds i32, ptr %A, i64 %iv
  %l = load i32, ptr %gep, align 4
  %add = add i32 %l, 1
  store i32 %add, ptr %gep, align 4
  %iv.next = add i64 %iv, 1
  %ec = icmp eq i64 %iv.next, 8
  br i1 %ec, label %exit, label %loop

exit:
  ret void
}

attributes #0 = { vscale_range(1,1073741824) }
