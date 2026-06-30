; RUN: opt -S -passes='default<O2>' < %s | FileCheck %s
;
; Test that the smin reduction loop is runtime-unrolled. The loop's induction
; variable needs nowrap flags for the unroller to compute the trip count; those
; flags are now inferred independently of SCEV query order (see the nowrap-flag
; re-inference in ScalarEvolution::getBackedgeTakenInfo), so the loop is
; reliably unrolled with a reduction tree (llvm.smin.i32 on rdx.minmax values).

target datalayout = "e-m:o-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-n32:64-S128-Fn32"

define i32 @reduce_smin(i32 %dir) {
entry:
  switch i32 %dir, label %common.ret [
    i32 0, label %sw.epilog
    i32 2, label %sw.bb2
    i32 1, label %sw.epilog
  ]

sw.bb2:
  br label %sw.epilog

common.ret:
  %common.ret.op = phi i32 [ 0, %entry ], [ %my_min.0, %for.cond ]
  ret i32 %common.ret.op

sw.epilog:
  %mvs.0 = phi i32 [ 1, %entry ], [ 2, %sw.bb2 ], [ 1, %entry ]
  br label %for.cond

for.cond:
  %i.0 = phi i32 [ 0, %sw.epilog ], [ %inc, %for.body ]
  %my_min.0 = phi i32 [ 0, %sw.epilog ], [ %cond13, %for.body ]
  %cmp4 = icmp slt i32 %i.0, %mvs.0
  br i1 %cmp4, label %for.body, label %common.ret

for.body:
  %idxprom5 = zext i32 %i.0 to i64
  %arrayidx6 = getelementptr [8 x i8], ptr null, i64 %idxprom5
  %arrayidx7 = getelementptr i8, ptr %arrayidx6, i64 4
  %v = load i32, ptr %arrayidx7, align 4
  %cond13 = call i32 @llvm.smin.i32(i32 %my_min.0, i32 %v)
  %inc = add i32 %i.0, 1
  br label %for.cond
}

declare i32 @llvm.smin.i32(i32, i32)

; CHECK-LABEL: define i32 @reduce_smin(
; CHECK:         call i32 @llvm.smin.i32(i32 [[RDX1:%.*]], i32 [[RDX2:%.*]])
