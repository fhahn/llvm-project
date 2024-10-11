; RUN: opt < %s -scalable-vectorization=on -force-target-supports-scalable-vectors=true -passes=loop-vectorize -force-vector-width=2 -force-vector-interleave=2  -S | FileCheck %s

declare void @llvm.assume(i1) #0

attributes #0 = { nounwind willreturn }

%struct.data = type { ptr, ptr }

define void @test2(ptr %a, ptr %b) {
; CHECK-LABEL: @test2(
; CHECK:       entry:
; CHECK:         [[MASKCOND:%.*]] = icmp eq i64 %ptrint1, 0
; CHECK:         [[MASKCOND4:%.*]] = icmp eq i64 %ptrint2, 0
; CHECK:       vector.body:
; CHECK:         tail call void @llvm.assume(i1 [[MASKCOND]])
; CHECK-NEXT:    tail call void @llvm.assume(i1 [[MASKCOND]])
; CHECK:         tail call void @llvm.assume(i1 [[MASKCOND4]])
; CHECK-NEXT:    tail call void @llvm.assume(i1 [[MASKCOND4]])
entry:
  %ptrint1 = ptrtoint ptr %a to i64
  %maskcond = icmp eq i64 %ptrint1, 0
  %ptrint2 = ptrtoint ptr %b to i64
  %maskcond4 = icmp eq i64 %ptrint2, 0
  br label %for.body


for.body:                                         ; preds = %for.body, %entry
  %indvars.iv = phi i64 [ 0, %entry ], [ %indvars.iv.next, %for.body ]
  tail call void @llvm.assume(i1 %maskcond)
  %arrayidx = getelementptr inbounds float, ptr %a, i64 %indvars.iv
  %0 = load float, ptr %arrayidx, align 4
  %add = fadd float %0, 1.000000e+00
  tail call void @llvm.assume(i1 %maskcond4)
  %arrayidx5 = getelementptr inbounds float, ptr %b, i64 %indvars.iv
  store float %add, ptr %arrayidx5, align 4
  %indvars.iv.next = add nuw nsw i64 %indvars.iv, 1
  %exitcond = icmp eq i64 %indvars.iv, 1599
  br i1 %exitcond, label %for.end, label %for.body, !llvm.loop !0

for.end:                                          ; preds = %for.body
  ret void
}


!0 = distinct !{!0, !1}
!1 = !{!"llvm.loop.vectorize.scalable.enable", i1 true}
