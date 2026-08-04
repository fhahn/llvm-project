; RUN: opt -passes=constraint-elimination -S %s | FileCheck %s

@g = global [16 x i8] zeroinitializer

; The two indices scale the same variable by 2^64-1 and 1, so it does not
; contribute to the offset at all. No bound on %i can be derived from the
; access, and in particular the total scale must not be used as a divisor.
define void @gep_index_with_zero_total_scale(i64 %i) {
; CHECK-LABEL: define void @gep_index_with_zero_total_scale(
; CHECK-NEXT:  [[ENTRY:.*:]]
; CHECK-NEXT:    [[P:%.*]] = getelementptr nuw [18446744073709551615 x i8], ptr @g, i64 [[I:%.*]], i64 [[I]]
; CHECK-NEXT:    store i8 0, ptr [[P]], align 1
; CHECK-NEXT:    ret void
;
entry:
  %p = getelementptr nuw [18446744073709551615 x i8], ptr @g, i64 %i, i64 %i
  store i8 0, ptr %p
  ret void
}
