@aa = dso_local local_unnamed_addr global [256 x [256 x i8]] zeroinitializer, align 4
@bb = dso_local local_unnamed_addr global [256 x [256 x i8]] zeroinitializer, align 4

define dso_local void @s114_simplified() local_unnamed_addr {
entry:
  br label %for.cond1.preheader

for.cond1.preheader:
  %indvars.iv25 = phi i64 [ 0, %entry ], [ %indvars.iv.next26, %for.cond.cleanup3 ]
  %cmp221.not = icmp eq i64 %indvars.iv25, 0
  br i1 %cmp221.not, label %for.cond.cleanup3, label %for.body4

for.cond.cleanup:
  ret void


for.body4:
  %indvars.iv = phi i64 [ %indvars.iv.next, %for.body4 ], [ 0, %for.cond1.preheader ]
  %arrayidx6 = getelementptr inbounds [256 x [256 x i8]], ptr @aa, i64 0, i64 %indvars.iv, i64 %indvars.iv25
  %0 = load i8, ptr %arrayidx6, align 4
  %arrayidx10 = getelementptr inbounds [256 x [256 x i8]], ptr @aa, i64 0, i64 %indvars.iv25, i64 %indvars.iv
  store i8 %0, ptr %arrayidx10, align 4
  %indvars.iv.next = add nuw nsw i64 %indvars.iv, 1
  %exitcond.not = icmp eq i64 %indvars.iv.next, %indvars.iv25
  br i1 %exitcond.not, label %for.cond.cleanup3, label %for.body4

  for.cond.cleanup3:
  %indvars.iv.next26 = add nuw nsw i64 %indvars.iv25, 1
  %exitcond28.not = icmp eq i64 %indvars.iv.next26, 256
  br i1 %exitcond28.not, label %for.cond.cleanup, label %for.cond1.preheader

}

