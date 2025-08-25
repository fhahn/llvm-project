target datalayout = "e-m:o-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-n32:64-S128-Fn32"
target triple = "arm64-apple-macosx15.0.0"

define fastcc i32 @_ov_d_seek_lap(i64 %conv, ptr %0) {
entry:
  br label %for.body

for.body:                                         ; preds = %for.body, %entry
  %indvars.iv = phi i64 [ 0, %entry ], [ %indvars.iv.next, %for.body ]
  %1 = alloca [18446744073709551612 x i8], align 8
  %arrayidx = getelementptr ptr, ptr %0, i64 %indvars.iv
  store ptr %1, ptr %arrayidx, align 8
  %indvars.iv.next = add i64 %indvars.iv, 1
  %exitcond.not = icmp eq i64 %indvars.iv, %conv
  br i1 %exitcond.not, label %for.end, label %for.body

for.end:                                          ; preds = %for.body
  ret i32 0
}
