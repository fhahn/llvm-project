; RUN: llubi --verbose < %s 2>&1 | FileCheck %s

target datalayout = "e-p:64:64:64:32"

define void @main() {
  %alloc = alloca [4 x i8], align 4
  %p = getelementptr [4 x i8], ptr %alloc, i64 0, i64 0

  ; Load from uninitialized memory yields poison.
  %v_undef = load i8, ptr %p

  ; Load i32 from uninitialized memory.
  %v_undef32 = load i32, ptr %p

  ret void
}
; CHECK: Entering function: main
; CHECK-NEXT:   %alloc = alloca [4 x i8], align 4 => ptr 0x8 [alloc]
; CHECK-NEXT:   %p = getelementptr [4 x i8], ptr %alloc, i64 0, i64 0 => ptr 0x8 [alloc]
; CHECK-NEXT:   %v_undef = load i8, ptr %p, align 1 => poison
; CHECK-NEXT:   %v_undef32 = load i32, ptr %p, align 4 => poison
; CHECK-NEXT:   ret void
; CHECK-NEXT: Exiting function: main
