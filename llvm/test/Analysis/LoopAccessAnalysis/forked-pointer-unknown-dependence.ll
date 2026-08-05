; RUN: opt -passes='print<access-info>' -disable-output %s 2>&1 | FileCheck %s

; Accesses through a pointer phi in the loop are added for the pointers the phi
; is expanded to, not for the phi itself. An unknown dependence between two such
; accesses must still remove them from the same dependency set, so that a
; runtime check is generated between them.
define i32 @unknown_dependence_between_forked_pointers(ptr %A, i64 %n, i64 %k, i1 %c) {
; CHECK-LABEL: 'unknown_dependence_between_forked_pointers'
; CHECK:         Memory dependences are safe with run-time checks
; CHECK:         Run-time memory checks:
; CHECK-NEXT:    Check 0:
;
entry:
  br label %loop

loop:
  %iv = phi i64 [ 0, %entry ], [ %iv.next, %latch ]
  %acc = phi i32 [ 0, %entry ], [ %acc.next, %latch ]
  %ivk = add nsw i64 %iv, %k
  br i1 %c, label %t1, label %f1

t1:
  %p1 = getelementptr inbounds i32, ptr %A, i64 %iv
  br label %m1

f1:
  %p2 = getelementptr inbounds i32, ptr %A, i64 %iv
  br label %m1

m1:
  %ph = phi ptr [ %p1, %t1 ], [ %p2, %f1 ]
  store i32 7, ptr %ph, align 4
  br i1 %c, label %t2, label %f2

t2:
  %q1 = getelementptr inbounds i32, ptr %A, i64 %ivk
  br label %m2

f2:
  %q2 = getelementptr inbounds i32, ptr %A, i64 %ivk
  br label %m2

m2:
  %qh = phi ptr [ %q1, %t2 ], [ %q2, %f2 ]
  %l = load i32, ptr %qh, align 4
  %acc.next = add i32 %acc, %l
  br label %latch

latch:
  %iv.next = add nuw nsw i64 %iv, 1
  %ec = icmp eq i64 %iv.next, %n
  br i1 %ec, label %exit, label %loop

exit:
  ret i32 %acc.next
}
