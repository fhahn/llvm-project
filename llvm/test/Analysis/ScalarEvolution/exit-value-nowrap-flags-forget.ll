; RUN: opt -passes='print<scalar-evolution>,loop(indvars),print<scalar-evolution>' -disable-output %s 2>&1 | FileCheck %s

; %j starts at the exit value of %x, (9 + %pn)<u nuw>, whose range is memoized
; for the constant max backedge-taken count of %l3. IndVars rewrites the exit
; value of %l1 and forgets %pn. The range of the use must be forgotten with it,
; so that the recomputed count uses the new range of %pn.

; CHECK: Printing analysis 'Scalar Evolution Analysis' for function 'f':
; CHECK: Loop %l3: constant max backedge-taken count is i64 9223372036854775798
; CHECK: Printing analysis 'Scalar Evolution Analysis' for function 'f':
; CHECK: Loop %l3: constant max backedge-taken count is i64 -10
define void @f(i1 %skip, i64 %lim, i64 %m, ptr %p) {
entry:
  br i1 %skip, label %l1.exit, label %l1

l1:
  %iv = phi i64 [ -1, %entry ], [ %iv.next, %l1 ]
  %iv.next = add nsw i64 %iv, -1
  %c2 = icmp ne i64 %iv.next, %lim
  br i1 %c2, label %l1, label %l1.exit

l1.exit:
  %pn = phi i64 [ -7, %entry ], [ %iv.next, %l1 ]
  br label %l2

l2:
  %x = phi i64 [ %pn, %l1.exit ], [ %x.next, %l2 ]
  %i = phi i64 [ 0, %l1.exit ], [ %i.next, %l2 ]
  store volatile i64 %x, ptr %p
  %x.next = add nuw i64 %x, 1
  %i.next = add i64 %i, 1
  %c = icmp ult i64 %i.next, 10
  br i1 %c, label %l2, label %l3

l3:
  %j = phi i64 [ %x, %l2 ], [ %j.next, %l3 ]
  store volatile i64 %j, ptr %p
  %j.next = add i64 %j, 1
  %ec = icmp ult i64 %j, %m
  br i1 %ec, label %l3, label %exit

exit:
  ret void
}
