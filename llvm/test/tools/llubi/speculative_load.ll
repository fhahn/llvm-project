; RUN: llubi --verbose < %s 2>&1 | FileCheck %s

target datalayout = "e-p:64:64:64:32"

define void @test_speculative(ptr %p) {
  ; can.load.speculatively always returns true
  %can = call i1 @llvm.can.load.speculatively.p0(ptr %p, i64 4)

  ; 4 bytes valid: all elements loaded as concrete values
  %v4 = call <4 x i8> @llvm.speculative.load.v4i8.p0(ptr %p, i64 4)

  ; 2 bytes valid: first 2 concrete, last 2 poison
  %v2 = call <4 x i8> @llvm.speculative.load.v4i8.p0(ptr %p, i64 2)

  ; 0 bytes valid: all poison
  %v0 = call <4 x i8> @llvm.speculative.load.v4i8.p0(ptr %p, i64 0)
  ret void
}

define void @main() {
  %alloc = alloca [4 x i8], align 4
  %p = getelementptr [4 x i8], ptr %alloc, i64 0, i64 0
  store i8 10, ptr %p
  %p1 = getelementptr i8, ptr %p, i64 1
  store i8 20, ptr %p1
  %p2 = getelementptr i8, ptr %p, i64 2
  store i8 30, ptr %p2
  %p3 = getelementptr i8, ptr %p, i64 3
  store i8 40, ptr %p3
  call void @test_speculative(ptr %p)
  ret void
}
; CHECK: Entering function: main
; CHECK-NEXT:   %alloc = alloca [4 x i8], align 4 => ptr 0x8 [alloc]
; CHECK-NEXT:   %p = getelementptr [4 x i8], ptr %alloc, i64 0, i64 0 => ptr 0x8 [alloc]
; CHECK-NEXT:   store i8 10, ptr %p, align 1
; CHECK-NEXT:   %p1 = getelementptr i8, ptr %p, i64 1 => ptr 0x9 [alloc + 1]
; CHECK-NEXT:   store i8 20, ptr %p1, align 1
; CHECK-NEXT:   %p2 = getelementptr i8, ptr %p, i64 2 => ptr 0xA [alloc + 2]
; CHECK-NEXT:   store i8 30, ptr %p2, align 1
; CHECK-NEXT:   %p3 = getelementptr i8, ptr %p, i64 3 => ptr 0xB [alloc + 3]
; CHECK-NEXT:   store i8 40, ptr %p3, align 1
; CHECK-NEXT: Entering function: test_speculative
; CHECK-NEXT:   ptr %p = ptr 0x8 [alloc]
; CHECK-NEXT:   %can = call i1 @llvm.can.load.speculatively.p0(ptr %p, i64 4) => T
; CHECK-NEXT:   %v4 = call <4 x i8> @llvm.speculative.load.v4i8.p0(ptr %p, i64 4) => { i8 10, i8 20, i8 30, i8 40 }
; CHECK-NEXT:   %v2 = call <4 x i8> @llvm.speculative.load.v4i8.p0(ptr %p, i64 2) => { i8 10, i8 20, poison, poison }
; CHECK-NEXT:   %v0 = call <4 x i8> @llvm.speculative.load.v4i8.p0(ptr %p, i64 0) => { poison, poison, poison, poison }
; CHECK-NEXT:   ret void
; CHECK-NEXT: Exiting function: test_speculative
; CHECK-NEXT:   call void @test_speculative(ptr %p)
; CHECK-NEXT:   ret void
; CHECK-NEXT: Exiting function: main
