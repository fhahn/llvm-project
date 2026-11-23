; RUN: opt -aa-pipeline=basic-aa -passes=aa-eval -print-all-alias-modref-info -disable-output %s 2>&1 | FileCheck %s

; Check that the mod/ref info of calls for locations based on phis and selects
; uses all objects the location may be based on.

declare void @g()
declare void @escape(ptr)
declare void @escape_read_provenance(ptr captures(address, read_provenance))
declare void @errno_writer() memory(errnomem: write)
declare noalias ptr @alloc()
declare i32 @setjmp(ptr) returns_twice
declare ptr @llvm.stacksave.p0()
declare void @llvm.stackrestore.p0(ptr)

@ext = external global i8

; CHECK-LABEL: Function: phi_iv_alloca
; CHECK: Both ModRef: Ptr: i8* %p <-> call void @g()
define void @phi_iv_alloca(i1 %c) {
entry:
  %a = alloca [16 x i8]
  br label %loop

loop:
  %p = phi ptr [ %a, %entry ], [ %p.next, %loop ]
  store i8 0, ptr %p
  call void @g()
  %p.next = getelementptr i8, ptr %p, i64 1
  br i1 %c, label %loop, label %exit

exit:
  ret void
}

; CHECK-LABEL: Function: select_allocas
; CHECK: Both ModRef: Ptr: i8* %p <-> call void @g()
define void @select_allocas(i1 %c) {
  %a = alloca i8
  %b = alloca i8
  %p = select i1 %c, ptr %a, ptr %b
  store i8 0, ptr %p
  call void @g()
  ret void
}

; %b escapes before the call.
; CHECK-LABEL: Function: select_alloca_escaped_alloca
; CHECK: Both ModRef: Ptr: i8* %p <-> call void @g()
define void @select_alloca_escaped_alloca(i1 %c) {
  %a = alloca i8
  %b = alloca i8
  call void @escape(ptr %b)
  %p = select i1 %c, ptr %a, ptr %b
  store i8 0, ptr %p
  call void @g()
  ret void
}

; Only the read provenance of %b escapes before the call.
; CHECK-LABEL: Function: select_alloca_read_provenance_alloca
; CHECK: Both ModRef: Ptr: i8* %p <-> call void @g()
define void @select_alloca_read_provenance_alloca(i1 %c) {
  %a = alloca i8
  %b = alloca i8
  call void @escape_read_provenance(ptr %b)
  %p = select i1 %c, ptr %a, ptr %b
  store i8 0, ptr %p
  call void @g()
  ret void
}

; CHECK-LABEL: Function: phi_alloca_arg
; CHECK: Both ModRef: Ptr: i8* %p <-> call void @g()
define void @phi_alloca_arg(ptr %arg, i1 %c) {
entry:
  %a = alloca i8
  br i1 %c, label %then, label %join

then:
  br label %join

join:
  %p = phi ptr [ %a, %entry ], [ %arg, %then ]
  store i8 0, ptr %p
  call void @g()
  ret void
}

; Tail calls cannot access allocas, even if they escaped.
; CHECK-LABEL: Function: tail_call_select_escaped_allocas
; CHECK: Both ModRef: Ptr: i8* %p <-> tail call void @g()
define void @tail_call_select_escaped_allocas(i1 %c) {
  %a = alloca i8
  %b = alloca i8
  call void @escape(ptr %a)
  call void @escape(ptr %b)
  %p = select i1 %c, ptr %a, ptr %b
  store i8 0, ptr %p
  tail call void @g()
  ret void
}

; CHECK-LABEL: Function: tail_call_select_escaped_alloca_arg
; CHECK: Both ModRef: Ptr: i8* %p <-> tail call void @g()
define void @tail_call_select_escaped_alloca_arg(ptr %arg, i1 %c) {
  %a = alloca i8
  call void @escape(ptr %a)
  %p = select i1 %c, ptr %a, ptr %arg
  store i8 0, ptr %p
  tail call void @g()
  ret void
}

; stackrestore may modify the dynamic alloca %d, even though it does not
; escape. Other objects keep the regular rules.
; CHECK-LABEL: Function: stackrestore_select_static_and_dynamic_alloca
; CHECK: Both ModRef: Ptr: i8* %p <-> call void @llvm.stackrestore.p0(ptr %ss)
define void @stackrestore_select_static_and_dynamic_alloca(i64 %n, i1 %c) {
  %a = alloca i8
  %ss = call ptr @llvm.stacksave.p0()
  %d = alloca i8, i64 %n
  %p = select i1 %c, ptr %a, ptr %d
  store i8 0, ptr %p
  call void @llvm.stackrestore.p0(ptr %ss)
  ret void
}

; stackrestore may modify both dynamic allocas, even though they do not escape.
; CHECK-LABEL: Function: stackrestore_select_dynamic_allocas
; CHECK: Both ModRef: Ptr: i8* %p <-> call void @llvm.stackrestore.p0(ptr %ss)
define void @stackrestore_select_dynamic_allocas(i64 %n, i1 %c) {
  %ss = call ptr @llvm.stacksave.p0()
  %d1 = alloca i8, i64 %n
  %d2 = alloca i8, i64 %n
  %p = select i1 %c, ptr %d1, ptr %d2
  store i8 0, ptr %p
  call void @llvm.stackrestore.p0(ptr %ss)
  ret void
}

; Calls returning twice may access non-escaping objects other than allocas.
; CHECK-LABEL: Function: returns_twice_select_alloca_noalias_call
; CHECK: Both ModRef: Ptr: i8* %p <-> %r = call i32 @setjmp(ptr %buf)
define void @returns_twice_select_alloca_noalias_call(ptr %buf, i1 %c) {
  %a = alloca i8
  %m = call ptr @alloc()
  %p = select i1 %c, ptr %a, ptr %m
  store i8 0, ptr %p
  %r = call i32 @setjmp(ptr %buf)
  ret void
}

; %p may be the object returned by the call itself.
; CHECK-LABEL: Function: select_alloca_call_result
; CHECK: Both ModRef: Ptr: i8* %p <-> %m = call ptr @alloc()
define void @select_alloca_call_result(i1 %c) {
  %a = alloca i8
  %m = call ptr @alloc()
  %p = select i1 %c, ptr %a, ptr %m
  store i8 0, ptr %p
  ret void
}

; CHECK-LABEL: Function: errno_select_allocas
; CHECK: Just Mod: Ptr: i8* %p <-> call void @errno_writer()
define void @errno_select_allocas(i1 %c) {
  %a = alloca i8
  %b = alloca i8
  %p = select i1 %c, ptr %a, ptr %b
  store i8 0, ptr %p
  call void @errno_writer()
  ret void
}

; @ext may be errno.
; CHECK-LABEL: Function: errno_select_alloca_global
; CHECK: Just Mod: Ptr: i8* %p <-> call void @errno_writer()
define void @errno_select_alloca_global(i1 %c) {
  %a = alloca i8
  %p = select i1 %c, ptr %a, ptr @ext
  store i8 0, ptr %p
  call void @errno_writer()
  ret void
}

; %s9 is based on more objects than are looked at.
; CHECK-LABEL: Function: select_too_many_allocas
; CHECK: Both ModRef: Ptr: i8* %s9 <-> call void @g()
define void @select_too_many_allocas(i1 %c) {
  %a0 = alloca i8
  %a1 = alloca i8
  %a2 = alloca i8
  %a3 = alloca i8
  %a4 = alloca i8
  %a5 = alloca i8
  %a6 = alloca i8
  %a7 = alloca i8
  %a8 = alloca i8
  %a9 = alloca i8
  %s1 = select i1 %c, ptr %a0, ptr %a1
  %s2 = select i1 %c, ptr %s1, ptr %a2
  %s3 = select i1 %c, ptr %s2, ptr %a3
  %s4 = select i1 %c, ptr %s3, ptr %a4
  %s5 = select i1 %c, ptr %s4, ptr %a5
  %s6 = select i1 %c, ptr %s5, ptr %a6
  %s7 = select i1 %c, ptr %s6, ptr %a7
  %s8 = select i1 %c, ptr %s7, ptr %a8
  %s9 = select i1 %c, ptr %s8, ptr %a9
  store i8 0, ptr %s9
  call void @g()
  ret void
}
