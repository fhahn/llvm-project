; RUN: opt -passes=loop-vectorize -force-vector-width=16 -force-vector-interleave=1 -S < %s | FileCheck %s
; RUN: opt -passes=loop-vectorize -force-vector-width="vscale x 16" -force-vector-interleave=1 -S < %s | FileCheck %s --check-prefix=SVE

define i64 @test(ptr noalias readonly %addr) {
; CHECK-LABEL: @test(
; CHECK:       vector.body:
; CHECK:       [[L:%.*]] = load <16 x i64>
; CHECK:       middle.block:
; CHECK-NEXT:  [[E:%.*]] = extractelement <16 x i64> [[L]], i64 15
; CHECK-NEXT:  freeze i64 [[E]]

; SVE-LABEL: @test(
; SVE:       vector.body:
; SVE:       [[L:%.*]] = load <vscale x 16 x i64>
; SVE:       middle.block:
; SVE:       [[E:%.*]] = extractelement <vscale x 16 x i64> [[L]], i32
; SVE-NEXT:  freeze i64 [[E]]

entry:
  br label %loop

exit:
  ret i64 %tmp4

loop:
  %tmp3 = phi ptr [ %tmp6, %loop ], [ %addr, %entry ]
  %l = load i64, ptr %tmp3
  %tmp4 = freeze i64 %l
  %tmp5 = add i64 0, 0
  %tmp6 = getelementptr inbounds ptr, ptr %tmp3, i64 1
  %tmp7 = icmp eq ptr %tmp6, null
  br i1 %tmp7, label %exit, label %loop
}
