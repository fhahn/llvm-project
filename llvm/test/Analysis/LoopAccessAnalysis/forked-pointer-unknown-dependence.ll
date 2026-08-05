; RUN: opt -passes='print<access-info>' -disable-output %s 2>&1 | FileCheck %s

; Accesses through non-header pointer phis are added for the phis' incoming
; pointers. An unknown dependence between two such accesses must still lead to
; a runtime check between them.
define void @unknown_dependence_between_pointer_phis(ptr %a, i64 %n, i64 %k, i1 %c) {
; CHECK-LABEL: 'unknown_dependence_between_pointer_phis'
; CHECK:         Memory dependences are safe with run-time checks
; CHECK:         Run-time memory checks:
; CHECK-NEXT:    Check 0:
; CHECK-NEXT:      Comparing group GRP0:
; CHECK-NEXT:        %gep = getelementptr i32, ptr %a, i64 %iv
; CHECK-NEXT:      Against group GRP1:
; CHECK-NEXT:        %gep.k = getelementptr i32, ptr %a, i64 %iv.k
;
entry:
  br label %loop.header

loop.header:
  %iv = phi i64 [ 0, %entry ], [ %iv.next, %loop.latch ]
  %gep = getelementptr i32, ptr %a, i64 %iv
  %iv.k = add i64 %iv, %k
  %gep.k = getelementptr i32, ptr %a, i64 %iv.k
  br i1 %c, label %then, label %loop.latch

then:
  br label %loop.latch

loop.latch:
  %ptr.st = phi ptr [ %gep, %then ], [ %gep, %loop.header ]
  %ptr.ld = phi ptr [ %gep.k, %then ], [ %gep.k, %loop.header ]
  store i32 0, ptr %ptr.st, align 4
  %l = load i32, ptr %ptr.ld, align 4
  %iv.next = add i64 %iv, 1
  %ec = icmp eq i64 %iv.next, %n
  br i1 %ec, label %exit, label %loop.header

exit:
  ret void
}
