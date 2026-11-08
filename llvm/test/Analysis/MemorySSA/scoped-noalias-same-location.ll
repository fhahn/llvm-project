; RUN: opt -aa-pipeline=basic-aa,scoped-noalias-aa -passes='print<memoryssa>,verify<memoryssa>' -disable-output < %s 2>&1 | FileCheck %s

; %l0 executes whenever %l1 does and accesses the same location. Its !noalias
; metadata shows that the store does not alias that location, so the store
; does not clobber %l1 either.
define float @earlier_access(ptr %p, ptr %q) {
; CHECK-LABEL: define float @earlier_access(
; CHECK:       MemoryUse(liveOnEntry)
; CHECK-NEXT:  %l0 = load float, ptr %p
; CHECK:       1 = MemoryDef(liveOnEntry)
; CHECK-NEXT:  store i8 0, ptr %q
; CHECK:       MemoryUse(1)
; CHECK-NEXT:  %l1 = load float, ptr %p
;
  %l0 = load float, ptr %p, align 4, !noalias !0
  store i8 0, ptr %q, align 1, !alias.scope !0
  %l1 = load float, ptr %p, align 4, !noalias !3
  %r = fadd float %l0, %l1
  ret float %r
}

; The access with the !noalias metadata comes after %l0, so it does not need to
; execute whenever %l0 does.
define float @later_access(ptr %p, ptr %q) {
; CHECK-LABEL: define float @later_access(
; CHECK:       1 = MemoryDef(liveOnEntry)
; CHECK-NEXT:  store i8 0, ptr %q
; CHECK:       MemoryUse(1)
; CHECK-NEXT:  %l0 = load float, ptr %p
; CHECK:       MemoryUse(liveOnEntry)
; CHECK-NEXT:  %l1 = load float, ptr %p
;
  store i8 0, ptr %q, align 1, !alias.scope !0
  %l0 = load float, ptr %p, align 4, !noalias !3
  %l1 = load float, ptr %p, align 4, !noalias !0
  %r = fadd float %l0, %l1
  ret float %r
}

; The earlier access is smaller than %l1.
define i32 @earlier_access_smaller(ptr %p, ptr %q) {
; CHECK-LABEL: define i32 @earlier_access_smaller(
; CHECK:       1 = MemoryDef(liveOnEntry)
; CHECK-NEXT:  store i8 0, ptr %q
; CHECK:       MemoryUse(1)
; CHECK-NEXT:  %l1 = load i32, ptr %p
;
  %l0 = load i16, ptr %p, align 4, !noalias !0
  store i8 0, ptr %q, align 1, !alias.scope !0
  %l1 = load i32, ptr %p, align 4, !noalias !3
  ret i32 %l1
}

; The earlier access is in a different block.
define float @earlier_access_other_block(ptr %p, ptr %q, i1 %c) {
; CHECK-LABEL: define float @earlier_access_other_block(
; CHECK:       1 = MemoryDef(liveOnEntry)
; CHECK-NEXT:  store i8 0, ptr %q
; CHECK:       MemoryUse(1)
; CHECK-NEXT:  %l1 = load float, ptr %p
;
entry:
  %l0 = load float, ptr %p, align 4, !noalias !0
  br label %next

next:
  store i8 0, ptr %q, align 1, !alias.scope !0
  %l1 = load float, ptr %p, align 4, !noalias !3
  %r = fadd float %l0, %l1
  ret float %r
}

; The earlier access accesses a different pointer value.
define float @earlier_access_other_ptr(ptr %p, ptr %q) {
; CHECK-LABEL: define float @earlier_access_other_ptr(
; CHECK:       1 = MemoryDef(liveOnEntry)
; CHECK-NEXT:  store i8 0, ptr %q
; CHECK:       MemoryUse(1)
; CHECK-NEXT:  %l1 = load float, ptr %p
;
  %gep = getelementptr i8, ptr %p, i64 0
  %l0 = load float, ptr %gep, align 4, !noalias !0
  store i8 0, ptr %q, align 1, !alias.scope !0
  %l1 = load float, ptr %p, align 4, !noalias !3
  %r = fadd float %l0, %l1
  ret float %r
}

!0 = !{!1}
!1 = distinct !{!1, !2, !"scope0"}
!2 = distinct !{!2, i1 false, !"domain"}
!3 = !{!4}
!4 = distinct !{!4, !2, !"scope1"}
