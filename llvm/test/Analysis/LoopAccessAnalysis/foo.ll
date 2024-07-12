@aa = dso_local local_unnamed_addr global [256 x [256 x i8]] zeroinitializer, align 4
@bb = dso_local local_unnamed_addr global [256 x [256 x i8]] zeroinitializer, align 4


declare void @llvm.assume(i1)

define void @s114_simplified(i64 %x) {
for.cond1.preheader:
  %pre = icmp ult i64 %x, 255
  call void @llvm.assume(i1 %pre)
  %cmp221.not = icmp eq i64 %x, 0
  br i1 %cmp221.not, label %for.cond.cleanup3, label %for.body4

for.body4:
  %indvars.iv = phi i64 [ %indvars.iv.next, %for.body4 ], [ 0, %for.cond1.preheader ]
  %arrayidx6 = getelementptr inbounds [256 x [256 x i8]], ptr @aa, i64 0, i64 %indvars.iv, i64 %x
  %0 = load i8, ptr %arrayidx6, align 4
  %arrayidx10 = getelementptr inbounds [256 x [256 x i8]], ptr @aa, i64 0, i64 %x, i64 %indvars.iv
  store i8 %0, ptr %arrayidx10, align 4
  %indvars.iv.next = add nuw nsw i64 %indvars.iv, 1
  %exitcond.not = icmp eq i64 %indvars.iv.next, %x
  br i1 %exitcond.not, label %for.cond.cleanup3, label %for.body4

for.cond.cleanup3:
  ret void
}

