; Oracle functions of llvm.speculative.load must stay local and must be kept
; together with their users.
; FIXME: Oracle functions are currently externalized.

; RUN: llvm-extract -func=user -S %s | FileCheck --check-prefix=EXTRACT %s
; RUN: llvm-extract --delete -func=other -S %s | FileCheck --check-prefix=DELETE %s
; RUN: llvm-extract -func=other -S %s | FileCheck --check-prefix=OTHER %s

; EXTRACT: declare hidden i64 @oracle(
; EXTRACT: define <4 x i32> @user(
; EXTRACT: call <4 x i32> (ptr, i1, ...) @llvm.speculative.load.v4i32.p0(ptr %p, i1 false, ptr @oracle, ptr %p, i64 %n)

; DELETE: define hidden i64 @oracle(
; DELETE: define <4 x i32> @user(
; DELETE: call <4 x i32> (ptr, i1, ...) @llvm.speculative.load.v4i32.p0(ptr %p, i1 false, ptr @oracle, ptr %p, i64 %n)

; OTHER-NOT: @oracle
; OTHER:     define void @other(
; OTHER-NOT: @oracle

define internal i64 @oracle(ptr %p, i64 %n) memory(argmem: read) nounwind nosync willreturn "oracle-function" disable_sanitizer_instrumentation noprofile {
  ret i64 %n
}

define <4 x i32> @user(ptr %p, i64 %n) {
  %r = call <4 x i32> (ptr, i1, ...) @llvm.speculative.load.v4i32.p0(ptr %p, i1 false, ptr @oracle, ptr %p, i64 %n)
  ret <4 x i32> %r
}

define void @other() {
  ret void
}
