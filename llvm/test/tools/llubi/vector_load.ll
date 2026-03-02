; RUN: llubi --verbose < %s 2>&1 | FileCheck %s

target datalayout = "e-p:64:64:64:32"

define void @main() {
  %alloc = alloca [4 x i8], align 4
  %p = getelementptr [4 x i8], ptr %alloc, i64 0, i64 0

  ; Vector load from uninitialized memory yields poison for each element.
  %v_vec = load <4 x i8>, ptr %p

  ; Vector load of i32 elements.
  %alloc32 = alloca [2 x i32], align 4
  %v_vec32 = load <2 x i32>, ptr %alloc32

  ret void
}
; CHECK: Entering function: main
; CHECK-NEXT:   %alloc = alloca [4 x i8], align 4 => ptr 0x8 [alloc]
; CHECK-NEXT:   %p = getelementptr [4 x i8], ptr %alloc, i64 0, i64 0 => ptr 0x8 [alloc]
; CHECK-NEXT:   %v_vec = load <4 x i8>, ptr %p, align 4 => { poison, poison, poison, poison }
; CHECK-NEXT:   %alloc32 = alloca [2 x i32], align 4 => ptr 0xC [alloc32]
; CHECK-NEXT:   %v_vec32 = load <2 x i32>, ptr %alloc32, align 8 => { poison, poison }
; CHECK-NEXT:   ret void
; CHECK-NEXT: Exiting function: main
