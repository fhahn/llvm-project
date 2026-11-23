; RUN: opt < %s -passes=aa-eval -print-all-alias-modref-info 2>&1 | FileCheck %s

@c = constant [8 x i32] zeroinitializer

declare void @dummy()

declare void @foo(ptr)

; CHECK-LABEL: Function: basic
; CHECK: NoModRef: Ptr: i32* @c	<->  call void @dummy()
define void @basic(ptr %p) {
  call void @dummy()
  load i32, ptr @c
  ret void
}

; CHECK-LABEL: Function: recphi
; CHECK: NoModRef: Ptr: i32* %p	<->  call void @dummy()
define void @recphi() {
entry:
  br label %loop

loop:
  %p = phi ptr [ @c, %entry ], [ %p.next, %loop ]
  call void @dummy()
  load i32, ptr %p
  %p.next = getelementptr i32, ptr %p, i64 1
  %c = icmp ne ptr %p.next, getelementptr (i32, ptr @c, i64 8)
  br i1 %c, label %loop, label %exit

exit:
  ret void
}

; Tests that readonly noalias implies !Mod.
;
; CHECK-LABEL: Function: readonly_noalias
; CHECK: Just Ref: Ptr: i32* %p <->  call void @foo(ptr %p)
define void @readonly_noalias(ptr readonly noalias %p) {
    call void @foo(ptr %p)
    load i32, ptr %p
    ret void
}

; Tests that readnone noalias implies !Mod.
;
; CHECK-LABEL: Function: readnone_noalias
; CHECK: Just Ref: Ptr: i32* %p <->  call void @foo(ptr %p)
define void @readnone_noalias(ptr readnone noalias %p) {
    call void @foo(ptr %p)
    load i32, ptr %p
    ret void
}

; Tests that writeonly noalias doesn't imply !Ref (since it's still possible
; to read from the object through other pointers if the pointer wasn't
; written).
;
; CHECK-LABEL: Function: writeonly_noalias
; CHECK: Both ModRef: Ptr: i32* %p <->  call void @foo(ptr %p)
define void @writeonly_noalias(ptr writeonly noalias %p) {
    call void @foo(ptr %p)
    load i32, ptr %p
    ret void
}

; Tests that readonly doesn't imply !Mod without noalias.
;
; CHECK-LABEL: Function: just_readonly
; CHECK: Both ModRef: Ptr: i32* %p <->  call void @foo(ptr %p)
define void @just_readonly(ptr readonly %p) {
    call void @foo(ptr %p)
    load i32, ptr %p
    ret void
}

; Tests that readnone doesn't imply !Mod without noalias.
;
; CHECK-LABEL: Function: just_readnone
; CHECK: Both ModRef: Ptr: i32* %p <->  call void @foo(ptr %p)
define void @just_readnone(ptr readnone %p) {
    call void @foo(ptr %p)
    load i32, ptr %p
    ret void
}

; Tests that writeonly doesn't imply !Ref.
;
; CHECK-LABEL: Function: just_writeonly
; CHECK: Both ModRef: Ptr: i32* %p <->  call void @foo(ptr %p)
define void @just_writeonly(ptr writeonly %p) {
    call void @foo(ptr %p)
    load i32, ptr %p
    ret void
}

; %p has 8 incoming values, which are all @c.
; CHECK-LABEL: Function: phi_same_constant_incoming
; CHECK: NoModRef: Ptr: i32* %p <->  call void @dummy()
define void @phi_same_constant_incoming(i32 %x) {
entry:
  switch i32 %x, label %join [
    i32 1, label %bb1
    i32 2, label %bb2
    i32 3, label %bb3
    i32 4, label %bb4
    i32 5, label %bb5
    i32 6, label %bb6
    i32 7, label %bb7
  ]

bb1:
  br label %join

bb2:
  br label %join

bb3:
  br label %join

bb4:
  br label %join

bb5:
  br label %join

bb6:
  br label %join

bb7:
  br label %join

join:
  %p = phi ptr [ @c, %entry ], [ @c, %bb1 ], [ @c, %bb2 ], [ @c, %bb3 ], [ @c, %bb4 ], [ @c, %bb5 ], [ @c, %bb6 ], [ @c, %bb7 ]
  call void @dummy()
  load i32, ptr %p
  ret void
}

@c0 = constant i32 0
@c1 = constant i32 0
@c2 = constant i32 0
@c3 = constant i32 0
@c4 = constant i32 0
@c5 = constant i32 0
@c6 = constant i32 0
@c7 = constant i32 0
@c8 = constant i32 0

; %s8 is based on more constant globals than are looked at.
; CHECK-LABEL: Function: select_too_many_constants
; CHECK: Both ModRef: Ptr: i32* %s8 <->  call void @dummy()
define void @select_too_many_constants(i1 %b) {
  %s1 = select i1 %b, ptr @c0, ptr @c1
  %s2 = select i1 %b, ptr %s1, ptr @c2
  %s3 = select i1 %b, ptr %s2, ptr @c3
  %s4 = select i1 %b, ptr %s3, ptr @c4
  %s5 = select i1 %b, ptr %s4, ptr @c5
  %s6 = select i1 %b, ptr %s5, ptr @c6
  %s7 = select i1 %b, ptr %s6, ptr @c7
  %s8 = select i1 %b, ptr %s7, ptr @c8
  call void @dummy()
  load i32, ptr %s8
  ret void
}
