; RUN: opt -p loop-vectorize -S %s | FileCheck %s

target datalayout = "e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-unknown-linux"

@a = dso_local local_unnamed_addr global i8 0, align 1
@f = dso_local local_unnamed_addr global i32 0, align 4
@e = dso_local local_unnamed_addr global i32 0, align 4

; Function Attrs: mustprogress nofree norecurse nosync nounwind willreturn memory(read, argmem: none, inaccessiblemem: none) uwtable
define dso_local signext i8 @g(i32 noundef %h) local_unnamed_addr #0 {
entry:
  %0 = load i8, ptr @a, align 1, !tbaa !5
  %conv = zext i8 %0 to i32
  %shl = shl i32 %conv, %h
  %conv1 = trunc i32 %shl to i8
  ret i8 %conv1
}

; Function Attrs: nofree norecurse nosync nounwind memory(readwrite, argmem: read, inaccessiblemem: none) uwtable
define dso_local noundef i32 @main() local_unnamed_addr #1 {
entry:
  %.pr = load i32, ptr @f, align 4, !tbaa !8
  %cmp3 = icmp ult i32 %.pr, 3
  br i1 %cmp3, label %for.body.lr.ph, label %for.end

for.body.lr.ph:                                   ; preds = %entry
  %0 = load i8, ptr @a, align 1, !tbaa !5
  %conv.i = zext i8 %0 to i32
  br label %for.body

for.body:                                         ; preds = %for.body.lr.ph, %for.body
  %1 = phi i32 [ %.pr, %for.body.lr.ph ], [ %inc, %for.body ]
  %conv2 = zext nneg i8 0 to i32
  %shl.i = shl i32 %conv.i, %conv2
  %sext = shl i32 %shl.i, 24
  %conv1 = ashr exact i32 %sext, 24
  store i32 %conv1, ptr @e, align 4, !tbaa !8
  %inc = add i32 %1, 1
  store i32 %inc, ptr @f, align 4, !tbaa !8
  %exitcond.not = icmp eq i32 %inc, 3
  br i1 %exitcond.not, label %for.end.loopexit, label %for.body, !llvm.loop !10

for.end.loopexit:                                 ; preds = %for.body
  br label %for.end

for.end:                                          ; preds = %for.end.loopexit, %entry
  ret i32 0
}

attributes #0 = { mustprogress nofree norecurse nosync nounwind willreturn memory(read, argmem: none, inaccessiblemem: none) uwtable "min-legal-vector-width"="0" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "target-cpu"="x86-64" "target-features"="+cmov,+cx8,+fxsr,+mmx,+sse,+sse2,+x87" "tune-cpu"="generic" }
attributes #1 = { nofree norecurse nosync nounwind memory(readwrite, argmem: read, inaccessiblemem: none) uwtable "min-legal-vector-width"="0" "no-trapping-math"="true" "stack-protector-buffer-size"="8" "target-cpu"="x86-64" "target-features"="+cmov,+cx8,+fxsr,+mmx,+sse,+sse2,+x87" "tune-cpu"="generic" }

!llvm.module.flags = !{!0, !1, !2, !3}
!llvm.ident = !{!4}

!0 = !{i32 1, !"wchar_size", i32 4}
!1 = !{i32 8, !"PIC Level", i32 2}
!2 = !{i32 7, !"PIE Level", i32 2}
!3 = !{i32 7, !"uwtable", i32 2}
!4 = !{!"clang version 20.0.0git (git@github.com:llvm/llvm-project.git ff81f9fbaeb32e9967ecd01f58a1cd9c7196e948)"}
!5 = !{!6, !6, i64 0}
!6 = !{!"omnipotent char", !7, i64 0}
!7 = !{!"Simple C/C++ TBAA"}
!8 = !{!9, !9, i64 0}
!9 = !{!"int", !6, i64 0}
!10 = distinct !{!10, !11}
!11 = !{!"llvm.loop.mustprogress"}
