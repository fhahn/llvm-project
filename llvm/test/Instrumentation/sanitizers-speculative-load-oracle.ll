; Oracle functions of llvm.speculative.load are never executed. Make sure they
; are not instrumented, their attributes are preserved and they are not
; referenced other than by llvm.speculative.load.

; RUN: opt < %s -passes=asan -S | FileCheck %s
; RUN: opt < %s -passes=msan -S | FileCheck %s
; RUN: opt < %s -passes=tsan -S | FileCheck %s
; RUN: opt < %s -passes='module(sancov-module)' -sanitizer-coverage-level=3 -sanitizer-coverage-trace-pc -sanitizer-coverage-pc-table -S | FileCheck %s
; RUN: opt < %s -passes='pgo-instr-gen,instrprof' -S | FileCheck %s
; RUN: opt < %s -passes=ctx-instr-gen -S | FileCheck %s

; REQUIRES: x86-registered-target

target datalayout = "e-m:e-i64:64-f80:128-n8:16:32:64-S128"
target triple = "x86_64-unknown-linux-gnu"

; CHECK-LABEL: define internal i64 @oracle(
; CHECK-SAME:    ptr [[P:%.*]], i64 [[N:%.*]]) #[[ATTR:[0-9]+]] {
; CHECK-NEXT:    [[V:%.*]] = load i64, ptr [[P]], align 8
; CHECK-NEXT:    [[C:%.*]] = icmp ult i64 [[V]], [[N]]
; CHECK-NEXT:    [[S:%.*]] = select i1 [[C]], i64 [[V]], i64 [[N]]
; CHECK-NEXT:    ret i64 [[S]]
; CHECK-NEXT:  }
define internal i64 @oracle(ptr %p, i64 %n) #0 {
  %v = load i64, ptr %p, align 8
  %c = icmp ult i64 %v, %n
  %s = select i1 %c, i64 %v, i64 %n
  ret i64 %s
}

define <4 x i32> @f(ptr %p, i64 %n) #1 {
  %r = call <4 x i32> (ptr, i1, ...) @llvm.speculative.load.v4i32.p0(ptr %p, i1 false, ptr @oracle, ptr %p, i64 %n)
  ret <4 x i32> %r
}

; CHECK: attributes #[[ATTR]] = { {{.*}}nosync nounwind{{.*}}willreturn memory(argmem: read){{.*}}"oracle-function"
attributes #0 = { memory(argmem: read) nounwind nosync willreturn "oracle-function" disable_sanitizer_instrumentation noprofile sanitize_address sanitize_hwaddress sanitize_memory sanitize_thread sanitize_type sanitize_realtime }
attributes #1 = { sanitize_address sanitize_hwaddress sanitize_memory sanitize_thread sanitize_type sanitize_realtime }
