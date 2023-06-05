; ModuleID = 'reduced.ll'
source_filename = "reduced.ll"
target datalayout = "e-m:o-i64:64-i128:128-n32:64-S128"
target triple = "arm64-apple-macosx13.0.0"

define weak_odr ptr @widget(i32 %arg) {
bb:
  %add = add i32 %arg, 1
  br label %bb1

bb1:                                              ; preds = %bb1, %bb
  %phi = phi i64 [ %add2, %bb1 ], [ 0, %bb ]
  %trunc = trunc i64 %phi to i32
  %mul = mul i32 %add, %trunc
  %zext = zext i32 %mul to i64
  %getelementptr = getelementptr double, ptr null, i64 %zext
  store double 0.000000e+00, ptr %getelementptr, align 8
  %add2 = add i64 %phi, 1
  %icmp = icmp eq i64 %add2, 0
  br i1 %icmp, label %bb3, label %bb1

bb3:                                              ; preds = %bb1
  ret ptr null
}
