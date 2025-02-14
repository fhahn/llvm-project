target datalayout = "e-m:o-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-n32:64-S128-Fn32"
target triple = "arm64-apple-macosx15.0.0"

define ptr @av_opt_find2(ptr %obj, i1 %tobool.not.i100, i1 %tobool18.not) {
entry:
  br label %while.cond16

while.cond16:                                     ; preds = %while.body19, %entry
  br i1 %tobool.not.i100, label %if.end29.loopexit, label %av_opt_child_next.exit

av_opt_child_next.exit:                           ; preds = %while.cond16
  %0 = load ptr, ptr %obj, align 8
  %call.i1022 = tail call ptr %0(ptr null, ptr null)
  br i1 %tobool18.not, label %if.end29.loopexit, label %while.body19

while.body19:                                     ; preds = %av_opt_child_next.exit
  %call204 = call ptr @av_opt_find2(ptr %0, i1 false, i1 false)
  br label %while.cond16

if.end29.loopexit:                                ; preds = %av_opt_child_next.exit, %while.cond16
  ret ptr null
}
