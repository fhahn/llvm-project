target datalayout = "e-m:o-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-n32:64-S128-Fn32"
target triple = "arm64-apple-macosx15.0.0"

define ptr @avpriv_fopen_utf8(i8 %0) {
entry:
  br label %while.cond.outer

while.cond:                                       ; preds = %while.cond.outer, %while.cond
  switch i8 %0, label %if.then11 [
    i8 1, label %while.cond
    i8 0, label %if.then
  ]

if.then:                                          ; preds = %while.cond
  %and = and i32 %access.1.ph37, 0
  br label %while.cond.outer

while.cond.outer:                                 ; preds = %if.then, %entry
  %access.1.ph37 = phi i32 [ 0, %if.then ], [ 0, %entry ]
  br label %while.cond

if.then11:                                        ; preds = %while.cond
  ret ptr null
}
