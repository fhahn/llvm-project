; RUN: llubi --verbose < %s 2>&1 | FileCheck %s

target datalayout = "e-p:64:64:64:32"

define void @main() {
  %alloc = alloca [4 x i8], align 4
  %p = getelementptr [4 x i8], ptr %alloc, i64 0, i64 0

  ; Store and load back scalar values.
  store i8 42, ptr %p
  %v0 = load i8, ptr %p

  %p1 = getelementptr i8, ptr %p, i64 1
  store i8 99, ptr %p1
  %v1 = load i8, ptr %p1

  ; Store and load an i32.
  %alloc32 = alloca i32, align 4
  store i32 305419896, ptr %alloc32
  %v32 = load i32, ptr %alloc32

  ; Store poison, load back yields poison.
  store i8 poison, ptr %p
  %v_poison = load i8, ptr %p

  ret void
}
; CHECK: Entering function: main
; CHECK-NEXT:   %alloc = alloca [4 x i8], align 4 => ptr 0x8 [alloc]
; CHECK-NEXT:   %p = getelementptr [4 x i8], ptr %alloc, i64 0, i64 0 => ptr 0x8 [alloc]
; CHECK-NEXT:   store i8 42, ptr %p, align 1
; CHECK-NEXT:   %v0 = load i8, ptr %p, align 1 => i8 42
; CHECK-NEXT:   %p1 = getelementptr i8, ptr %p, i64 1 => ptr 0x9 [alloc + 1]
; CHECK-NEXT:   store i8 99, ptr %p1, align 1
; CHECK-NEXT:   %v1 = load i8, ptr %p1, align 1 => i8 99
; CHECK-NEXT:   %alloc32 = alloca i32, align 4 => ptr 0xC [alloc32]
; CHECK-NEXT:   store i32 305419896, ptr %alloc32, align 4
; CHECK-NEXT:   %v32 = load i32, ptr %alloc32, align 4 => i32 305419896
; CHECK-NEXT:   store i8 poison, ptr %p, align 1
; CHECK-NEXT:   %v_poison = load i8, ptr %p, align 1 => poison
; CHECK-NEXT:   ret void
; CHECK-NEXT: Exiting function: main
