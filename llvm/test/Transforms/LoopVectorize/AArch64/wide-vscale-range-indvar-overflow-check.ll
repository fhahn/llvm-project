; RUN: opt -passes=loop-vectorize -mtriple=aarch64-unknown-linux-gnu -mattr=+sve -force-tail-folding-style=data -S %s | FileCheck %s

; The maximum runtime VF (VF's minimum value times the maximum value of vscale)
; bounds the step of the canonical IV, and is compared against the headroom left
; in the induction variable's type to decide whether the IV increment can be
; marked nuw. Both loops below are tail-folded, so the flag depends on that
; comparison rather than being implied by the absence of tail folding.

; vscale_range(1, 16) gives a maximum runtime VF of 64, which leaves plenty of
; headroom in i32, so the increment gets nuw.
define void @narrow_vscale_range_iv_no_overflow(ptr noalias %A) #0 {
; CHECK-LABEL: define void @narrow_vscale_range_iv_no_overflow(
; CHECK:       vector.body:
; CHECK:         %index.next = add nuw i32 %index, %{{.*}}
;
entry:
  br label %loop

loop:
  %iv = phi i32 [ 0, %entry ], [ %iv.next, %loop ]
  %gep = getelementptr inbounds i32, ptr %A, i32 %iv
  %l = load i32, ptr %gep, align 4
  %add = add i32 %l, 1
  store i32 %add, ptr %gep, align 4
  %iv.next = add i32 %iv, 1
  %ec = icmp eq i32 %iv.next, 21
  br i1 %ec, label %exit, label %loop

exit:
  ret void
}

; vscale_range(1, 2147483648) gives a maximum runtime VF of 2^33, which exceeds
; the range of the i32 induction variable. The step is then computed modulo i32
; in the vectorized loop and no useful bound on the truncated value is
; available, so the increment must not be marked nuw.
define void @wide_vscale_range_iv_may_overflow(ptr noalias %A) #1 {
; CHECK-LABEL: define void @wide_vscale_range_iv_may_overflow(
; CHECK:       vector.body:
; CHECK:         %index.next = add i32 %index, %{{.*}}
;
entry:
  br label %loop

loop:
  %iv = phi i32 [ 0, %entry ], [ %iv.next, %loop ]
  %gep = getelementptr inbounds i32, ptr %A, i32 %iv
  %l = load i32, ptr %gep, align 4
  %add = add i32 %l, 1
  store i32 %add, ptr %gep, align 4
  %iv.next = add i32 %iv, 1
  %ec = icmp eq i32 %iv.next, 21
  br i1 %ec, label %exit, label %loop

exit:
  ret void
}

attributes #0 = { optsize vscale_range(1,16) }
attributes #1 = { optsize vscale_range(1,2147483648) }
