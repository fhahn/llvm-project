; Oracle functions of llvm.speculative.load must stay local. Make sure they are
; not promoted and functions referencing them are not imported.
; FIXME: Oracle functions are currently promoted.

; RUN: opt -module-summary %s -o %t1.bc
; RUN: opt -module-summary %p/Inputs/speculative-load-oracle.ll -o %t2.bc
; RUN: llvm-dis -o - %t1.bc | FileCheck --check-prefix=SUMMARY %s

; SUMMARY-DAG: name: "oracle", summaries: (function: (module: ^0, flags: (linkage: internal, visibility: default, notEligibleToImport: 0,
; SUMMARY-DAG: name: "f", summaries: (function: (module: ^0, flags: (linkage: external, visibility: default, notEligibleToImport: 0,

target datalayout = "e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-f80:128-n8:16:32:64-S128"
target triple = "x86_64-unknown-linux-gnu"

define internal i64 @oracle(ptr %p, i64 %n) memory(argmem: read) nounwind nosync willreturn "oracle-function" disable_sanitizer_instrumentation noprofile {
  ret i64 %n
}

define <4 x i32> @f(ptr %p, i64 %n) {
  %r = call <4 x i32> (ptr, i1, ...) @llvm.speculative.load.v4i32.p0(ptr %p, i1 false, ptr @oracle, ptr %p, i64 %n)
  ret <4 x i32> %r
}
