; RUN: llubi --verbose < %s 2>&1 | FileCheck %s

target datalayout = "e-p:64:64:64:32"

define void @main() {
  ; Vector store and load back.
  %vec_alloc = alloca <4 x i8>, align 4
  store <4 x i8> <i8 10, i8 20, i8 30, i8 40>, ptr %vec_alloc
  %v_vec = load <4 x i8>, ptr %vec_alloc

  ; Vector store with poison element, load back.
  store <4 x i8> <i8 1, i8 poison, i8 3, i8 4>, ptr %vec_alloc
  %v_mixed = load <4 x i8>, ptr %vec_alloc

  ret void
}
; CHECK: Entering function: main
; CHECK-NEXT:   %vec_alloc = alloca <4 x i8>, align 4 => ptr 0x8 [vec_alloc]
; CHECK-NEXT:   store <4 x i8> <i8 10, i8 20, i8 30, i8 40>, ptr %vec_alloc, align 4
; CHECK-NEXT:   %v_vec = load <4 x i8>, ptr %vec_alloc, align 4 => { i8 10, i8 20, i8 30, i8 40 }
; CHECK-NEXT:   store <4 x i8> <i8 1, i8 poison, i8 3, i8 4>, ptr %vec_alloc, align 4
; CHECK-NEXT:   %v_mixed = load <4 x i8>, ptr %vec_alloc, align 4 => { i8 1, poison, i8 3, i8 4 }
; CHECK-NEXT:   ret void
; CHECK-NEXT: Exiting function: main
