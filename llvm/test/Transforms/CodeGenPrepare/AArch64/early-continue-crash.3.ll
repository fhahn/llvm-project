target datalayout = "e-m:o-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-n32:64-S128-Fn32"
target triple = "arm64-apple-macosx15.0.0"

define i32 @ff_aac_ac_get_pk(i32 %c) {
entry:
  br label %while.body

while.body:                                       ; preds = %if.end7, %entry
  %sub26 = phi i32 [ 0, %entry ], [ %sub, %if.end7 ]
  %cmp2.not = icmp eq i32 %sub26, 0
  br i1 %cmp2.not, label %if.else, label %if.end7

if.else:                                          ; preds = %while.body
  %cmp4.not = icmp eq i32 %c, 0
  br i1 %cmp4.not, label %if.else6, label %if.end7

if.else6:                                         ; preds = %if.else
  ret i32 0

if.end7:                                          ; preds = %if.else, %while.body
  %i_min.1 = phi i32 [ 0, %while.body ], [ %c, %if.else ]
  %sub = sub i32 0, %i_min.1
  br label %while.body
}
