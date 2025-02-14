target datalayout = "e-m:o-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-n32:64-S128-Fn32"
target triple = "arm64-apple-macosx15.0.0"

define i32 @main(i32 %call14) {
entry:
  br label %for.cond.outer

for.cond:                                         ; preds = %for.cond.outer, %for.cond
  %call141 = load volatile i32, ptr null, align 4
  switch i32 %call14, label %for.cond.outer [
    i32 1, label %for.end
    i32 0, label %for.cond
  ]

for.cond.outer:                                   ; preds = %for.cond, %entry
  %M.0.ph = phi i64 [ 0, %entry ], [ 1, %for.cond ]
  br label %for.cond

for.end:                                          ; preds = %for.cond
  %call20 = call ptr @malloc(i64 %M.0.ph)
  unreachable
}

declare ptr @malloc(i64)
