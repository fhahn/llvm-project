; RUN: opt -passes=lower-matrix-intrinsics -debug-only=lower-matrix-intrinsics -disable-output < %s 2>&1 | FileCheck %s --check-prefix=CHECK
; REQUIRES: asserts

define void @diag_3x3(ptr %in, ptr %out) {
  %inv = call <9 x float> @llvm.matrix.column.major.load(ptr %in, i64 3, i1 false, i32 3, i32 3)
  %diag = shufflevector <9 x float> %inv, <9 x float> poison, <3 x i32> <i32 0, i32 4, i32 8>
  store <3 x float> %diag, ptr %out
  ret void
}
; CHECK-LABEL: flattening a 3x3 matrix:
; CHECK-NEXT: %{{.*}} = call <9 x float> @llvm.matrix.column.major.load.v9f32.i64(ptr %{{.*}}, i64 3, i1 false, i32 3, i32 3)
; CHECK-NEXT: because we do not have a shape-aware lowering for its user:
; CHECK-NEXT: %{{.*}} = shufflevector <9 x float> %{{.*}}, <9 x float> poison, <3 x i32> <i32 0, i32 4, i32 8>

define void @reshape_conflicting_users(ptr %in, ptr %out) {
entry:
  %l = load <6 x double>, ptr %in, align 8
  call void @llvm.matrix.column.major.store.v6f64.i64(<6 x double> %l, ptr %out, i64 6, i1 false, i32 6, i32 1)
  call void @llvm.matrix.column.major.store.v6f64.i64(<6 x double> %l, ptr %out, i64 3, i1 false, i32 3, i32 2)
  ret void
}
; CHECK-LABEL: matrix reshape from 6x1 to 3x2 using at least 2 shuffles on behalf of:
; CHECK-NEXT: %{{.*}} = load <6 x double>, ptr %{{.*}}, align 8

define void @multiply_ntt(ptr %A, ptr %B, ptr %C, ptr %R) {
entry:
  %a = load <6 x double>, ptr %A, align 16
  %b = load <6 x double>, ptr %B, align 16
  %c = load <8 x double>, ptr %C, align 16
  %b_t = call <6 x double> @llvm.matrix.transpose.v6f64.v6f64(<6 x double> %b, i32 2, i32 3)
  %c_t = call <8 x double> @llvm.matrix.transpose.v8f64.v8f64(<8 x double> %c, i32 4, i32 2)
  %m1 = call <12 x double> @llvm.matrix.multiply.v12f64.v6f64.v8f64(<6 x double> %b_t, <8 x double> %c_t, i32 3, i32 2, i32 4)
  %m2 = call <8 x double> @llvm.matrix.multiply.v8f64.v6f64.v12f64(<6 x double> %a, <12 x double> %m1, i32 2, i32 3, i32 4)
  store <8 x double> %m2, ptr %R, align 16
  ret void
}
; CHECK-LABEL: flattening a 2x3 matrix:
; CHECK-NEXT: %{{.*}} = load <6 x double>, ptr %{{.*}}, align 16
; CHECK-NEXT: because we do not have a shape-aware lowering for its user:
; CHECK-NEXT: %{{.*}} = shufflevector <6 x double> %{{.*}}, <6 x double> poison, <2 x i32> <i32 4, i32 5>

; CHECK-LABEL: flattening a 4x3 matrix:
; CHECK-NEXT: %{{.*}} = call <12 x double> @llvm.matrix.multiply.v12f64.v8f64.v6f64(<8 x double> %{{.*}}, <6 x double> %{{.*}}, i32 4, i32 2, i32 3)
; CHECK-NEXT: because we do not have a shape-aware lowering for its user:
; CHECK-NEXT: %{{.*}} = shufflevector <12 x double> %{{.*}}, <12 x double> poison, <4 x i32> <i32 8, i32 9, i32 10, i32 11>


define void @split_shuffle(<4 x float> %a, <4 x float> %b, ptr %out) {
entry:
  %shuffle = shufflevector <4 x float> %a, <4 x float> %b, <4 x i32> <i32 0, i32 5, i32 2, i32 7>
  call void @llvm.matrix.column.major.store.v4f32.i64(<4 x float> %shuffle, ptr %out, i64 2, i1 false, i32 2, i32 2)
  ret void
}

; CHECK-LABEL: splitting a 2x2 matrix with 2 shuffles beacuse we do not have a shape-aware lowering for its def:
; CHECK-NEXT: %{{.*}} = shufflevector <4 x float> %{{.*}}, <4 x float> %{{.*}}, <4 x i32> <i32 0, i32 5, i32 2, i32 7>
