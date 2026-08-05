; RUN: opt -S -passes=loop-vectorize -enable-vplan-native-path -force-vector-width=4 < %s | FileCheck %s

; A contiguous classification widens VF consecutive scalar accesses to a packed
; <VF x ElemTy> access. For element types that are bit-packed in vectors, the
; two are not byte-equivalent: 4 consecutive i1 scalars span 4 bytes, while a
; <4 x i1> access covers a single byte. Such accesses must stay
; gathers/scatters.

; for (i = 0; i < N; i++) {
;   bool b = A[i];
;   for (j = 0; j < M; j++)
;     ;
;   A[i] = !b;
; }
define void @sub_byte_i1(ptr noalias %A, i64 %N, i64 %M) {
; CHECK-LABEL: define void @sub_byte_i1(
; CHECK:       vector.body:
; CHECK:         call <4 x i1> @llvm.masked.gather.v4i1.v4p0(
; CHECK:         call void @llvm.masked.scatter.v4i1.v4p0(
;
entry:
  br label %outer.header

outer.header:
  %i = phi i64 [ 0, %entry ], [ %i.next, %outer.latch ]
  %gep.A = getelementptr inbounds i1, ptr %A, i64 %i
  %l = load i1, ptr %gep.A, align 1
  br label %inner.header

inner.header:
  %j = phi i64 [ 0, %outer.header ], [ %j.next, %inner.header ]
  %j.next = add nuw nsw i64 %j, 1
  %ec.inner = icmp eq i64 %j.next, %M
  br i1 %ec.inner, label %outer.latch, label %inner.header

outer.latch:
  %x = xor i1 %l, true
  store i1 %x, ptr %gep.A, align 1
  %i.next = add nuw nsw i64 %i, 1
  %ec = icmp eq i64 %i.next, %N
  br i1 %ec, label %exit, label %outer.header, !llvm.loop !0

exit:
  ret void
}

define void @sub_byte_i4(ptr noalias %A, i64 %N, i64 %M) {
; CHECK-LABEL: define void @sub_byte_i4(
; CHECK:       vector.body:
; CHECK:         call <4 x i4> @llvm.masked.gather.v4i4.v4p0(
; CHECK:         call void @llvm.masked.scatter.v4i4.v4p0(
;
entry:
  br label %outer.header

outer.header:
  %i = phi i64 [ 0, %entry ], [ %i.next, %outer.latch ]
  %gep.A = getelementptr inbounds i4, ptr %A, i64 %i
  %l = load i4, ptr %gep.A, align 1
  br label %inner.header

inner.header:
  %j = phi i64 [ 0, %outer.header ], [ %j.next, %inner.header ]
  %j.next = add nuw nsw i64 %j, 1
  %ec.inner = icmp eq i64 %j.next, %M
  br i1 %ec.inner, label %outer.latch, label %inner.header

outer.latch:
  %x = add i4 %l, 1
  store i4 %x, ptr %gep.A, align 1
  %i.next = add nuw nsw i64 %i, 1
  %ec = icmp eq i64 %i.next, %N
  br i1 %ec, label %exit, label %outer.header, !llvm.loop !0

exit:
  ret void
}

; i8 is not bit-packed in vectors, so the same access pattern is contiguous.
define void @byte_sized_i8(ptr noalias %A, i64 %N, i64 %M) {
; CHECK-LABEL: define void @byte_sized_i8(
; CHECK:       vector.body:
; CHECK:         load <4 x i8>, ptr
; CHECK:         store <4 x i8> {{.*}}, ptr
;
entry:
  br label %outer.header

outer.header:
  %i = phi i64 [ 0, %entry ], [ %i.next, %outer.latch ]
  %gep.A = getelementptr inbounds i8, ptr %A, i64 %i
  %l = load i8, ptr %gep.A, align 1
  br label %inner.header

inner.header:
  %j = phi i64 [ 0, %outer.header ], [ %j.next, %inner.header ]
  %j.next = add nuw nsw i64 %j, 1
  %ec.inner = icmp eq i64 %j.next, %M
  br i1 %ec.inner, label %outer.latch, label %inner.header

outer.latch:
  %x = add i8 %l, 1
  store i8 %x, ptr %gep.A, align 1
  %i.next = add nuw nsw i64 %i, 1
  %ec = icmp eq i64 %i.next, %N
  br i1 %ec, label %exit, label %outer.header, !llvm.loop !0

exit:
  ret void
}

!0 = distinct !{!0, !1}
!1 = !{!"llvm.loop.vectorize.enable"}
