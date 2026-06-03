; RUN: opt -passes='print<access-info>' -disable-output %s 2>&1 | FileCheck %s

; Regression test for a division-by-zero in getStrideFromAddRec. The access
; type is zero-sized (an empty array), so DL.getTypeAllocSize(AccessTy) is 0,
; while the pointer advances by a non-zero constant byte stride. Computing the
; element stride as StepVal % Size / StepVal / Size divided by zero (SIGFPE on
; x86; silently mis-analyzed elsewhere). getStrideFromAddRec now bails out for
; zero-sized access types.

target datalayout = "e-i64:64-n32:64"

define void @zero_sized_access(ptr %a, i64 %n) {
; CHECK-LABEL: 'zero_sized_access'
; CHECK-NEXT:    loop:
; CHECK-NEXT:      Report: unsafe dependent memory operations in loop.
entry:
  br label %loop

loop:
  %iv = phi i64 [ 0, %entry ], [ %iv.next, %loop ]
  %off = mul i64 %iv, 16
  %gep.r = getelementptr i8, ptr %a, i64 %off
  %v = load [0 x i8], ptr %gep.r, align 1
  %off2 = add i64 %off, 8
  %gep.w = getelementptr i8, ptr %a, i64 %off2
  store [0 x i8] %v, ptr %gep.w, align 1
  %iv.next = add i64 %iv, 1
  %cmp = icmp slt i64 %iv.next, %n
  br i1 %cmp, label %loop, label %exit

exit:
  ret void
}
