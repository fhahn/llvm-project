; RUN: opt -passes='print<access-info>' -disable-output %s 2>&1 | FileCheck %s

; target("aarch64.svcount") has a scalable layout type, so its alloc size has no
; fixed value and no element stride can be computed for accesses of that type.
define void @scalable_target_ext_type(ptr %A, ptr %B, i64 %n) {
; CHECK-LABEL: 'scalable_target_ext_type'
; CHECK:         Memory dependences are safe with run-time checks
;
entry:
  br label %loop

loop:
  %iv = phi i64 [ 0, %entry ], [ %iv.next, %loop ]
  %off = mul i64 %iv, 32
  %gep.A = getelementptr i8, ptr %A, i64 %off
  %l = load target("aarch64.svcount"), ptr %gep.A, align 2
  %gep.B = getelementptr i8, ptr %B, i64 %off
  store target("aarch64.svcount") %l, ptr %gep.B, align 2
  %iv.next = add i64 %iv, 1
  %ec = icmp eq i64 %iv.next, %n
  br i1 %ec, label %exit, label %loop

exit:
  ret void
}
