; Oracle functions of llvm.speculative.load must stay local and must be placed
; in the same partition as their users. Other partitions must not contain
; declarations of them.
; FIXME: Oracle functions are currently externalized.

; RUN: not llvm-split -j2 -o %t %s 2>&1 | FileCheck --check-prefix=ERR %s
; RUN: not llvm-split -j2 -preserve-locals -o %t.pl %s 2>&1 | FileCheck --check-prefix=ERR %s

; ERR: oracle function must have local linkage

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

