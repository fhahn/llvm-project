; RUN: opt -p loop-vectorize -force-vector-width=2 -force-vector-interleave=1 -S %s | FileCheck %s

target datalayout = "e-m:o-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-n32:64-S128-Fn32"
target triple = "arm64-apple-macosx"

@g = global [202 x i64] zeroinitializer, align 16

; The interleave group's insert position is the first load in program order,
; here the member at offset 8 with align 16. The narrowed wide load starts at
; the group's base address, which %src makes provably 8-byte but not 16-byte
; aligned, so it must use the group's alignment (8) and not the insert
; position's.
define void @narrowed_load_uses_group_align(ptr noalias %dst) {
; CHECK-LABEL: define void @narrowed_load_uses_group_align(
; CHECK:       [[VECTOR_BODY:.*]]:
; CHECK:         [[WIDE_LOAD:%.*]] = load <2 x i64>, ptr {{.*}}, align 8
; CHECK:         store <2 x i64> [[WIDE_LOAD]], ptr {{.*}}, align 8
;
entry:
  %src = getelementptr inbounds i8, ptr @g, i64 8
  br label %loop

loop:
  %iv = phi i64 [ 0, %entry ], [ %iv.next, %loop ]
  %mul.2 = shl nsw i64 %iv, 1
  %add.1 = or disjoint i64 %mul.2, 1
  %src.1 = getelementptr inbounds i64, ptr %src, i64 %add.1
  %l.1 = load i64, ptr %src.1, align 16
  %src.0 = getelementptr inbounds i64, ptr %src, i64 %mul.2
  %l.0 = load i64, ptr %src.0, align 8
  %dst.0 = getelementptr inbounds i64, ptr %dst, i64 %mul.2
  store i64 %l.0, ptr %dst.0, align 8
  %dst.1 = getelementptr inbounds i64, ptr %dst, i64 %add.1
  store i64 %l.1, ptr %dst.1, align 8
  %iv.next = add nuw nsw i64 %iv, 1
  %ec = icmp eq i64 %iv.next, 100
  br i1 %ec, label %exit, label %loop

exit:
  ret void
}

; Same for the store side: the store group's insert position is the last store
; in program order, here the member at offset 8 with align 16.
define void @narrowed_store_uses_group_align(ptr noalias %src, ptr noalias %dst) {
; CHECK-LABEL: define void @narrowed_store_uses_group_align(
; CHECK:       [[VECTOR_BODY:.*]]:
; CHECK:         [[WIDE_LOAD:%.*]] = load <2 x i64>, ptr {{.*}}, align 8
; CHECK:         store <2 x i64> [[WIDE_LOAD]], ptr {{.*}}, align 8
;
entry:
  br label %loop

loop:
  %iv = phi i64 [ 0, %entry ], [ %iv.next, %loop ]
  %mul.2 = shl nsw i64 %iv, 1
  %add.1 = or disjoint i64 %mul.2, 1
  %src.0 = getelementptr inbounds i64, ptr %src, i64 %mul.2
  %l.0 = load i64, ptr %src.0, align 8
  %src.1 = getelementptr inbounds i64, ptr %src, i64 %add.1
  %l.1 = load i64, ptr %src.1, align 8
  %dst.0 = getelementptr inbounds i64, ptr %dst, i64 %mul.2
  store i64 %l.0, ptr %dst.0, align 8
  %dst.1 = getelementptr inbounds i64, ptr %dst, i64 %add.1
  store i64 %l.1, ptr %dst.1, align 16
  %iv.next = add nuw nsw i64 %iv, 1
  %ec = icmp eq i64 %iv.next, 100
  br i1 %ec, label %exit, label %loop

exit:
  ret void
}
