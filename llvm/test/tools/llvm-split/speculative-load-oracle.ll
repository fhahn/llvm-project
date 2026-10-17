; Oracle functions of llvm.speculative.load must stay local and must be placed
; in the same partition as their users. Other partitions must not contain
; declarations of them.

; RUN: llvm-split -j2 -o %t %s
; RUN: llvm-dis -o - %t0 | FileCheck --check-prefix=CHECK0 %s
; RUN: llvm-dis -o - %t1 | FileCheck --check-prefix=CHECK1 %s
; RUN: llvm-split -j2 -preserve-locals -o %t.pl %s
; RUN: llvm-dis -o - %t.pl0 | FileCheck --check-prefix=CHECK0 %s
; RUN: llvm-dis -o - %t.pl1 | FileCheck --check-prefix=CHECK1 %s

define internal i64 @oracle_a(ptr %p, i64 %n) memory(argmem: read) nounwind nosync willreturn "oracle-function" disable_sanitizer_instrumentation noprofile {
  ret i64 %n
}

define internal i64 @oracle_b(ptr %p, i64 %n) memory(argmem: read) nounwind nosync willreturn "oracle-function" disable_sanitizer_instrumentation noprofile {
  ret i64 %n
}

define <4 x i32> @user_a(ptr %p, i64 %n) {
  %r = call <4 x i32> (ptr, i1, ...) @llvm.speculative.load.v4i32.p0(ptr %p, i1 false, ptr @oracle_a, ptr %p, i64 %n)
  ret <4 x i32> %r
}

define <4 x i32> @user_a2(ptr %p, i64 %n) {
  %r = call <4 x i32> (ptr, i1, ...) @llvm.speculative.load.v4i32.p0(ptr %p, i1 false, ptr @oracle_a, ptr %p, i64 %n)
  ret <4 x i32> %r
}

define <4 x i32> @user_b(ptr %p, i64 %n) {
  %r = call <4 x i32> (ptr, i1, ...) @llvm.speculative.load.v4i32.p0(ptr %p, i1 false, ptr @oracle_b, ptr %p, i64 %n)
  ret <4 x i32> %r
}

; CHECK0-NOT:   oracle_b
; CHECK0:       define internal i64 @oracle_a(
; CHECK0-NOT:   oracle_b
; CHECK0:       define <4 x i32> @user_a(
; CHECK0:       define <4 x i32> @user_a2(
; CHECK0-NOT:   oracle_b

; CHECK1-NOT:   oracle_a
; CHECK1:       define internal i64 @oracle_b(
; CHECK1-NOT:   oracle_a
; CHECK1:       define <4 x i32> @user_b(
; CHECK1-NOT:   oracle_a
