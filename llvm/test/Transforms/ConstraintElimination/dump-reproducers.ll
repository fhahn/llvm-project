; RUN: opt -passes=constraint-elimination -constraint-elimination-dump-reproducers \
; RUN:   -disable-output %s
;
; Check that generating reproducers does not crash.

; Facts added while checking the operands of an and/or must also be recorded
; for the reproducer.
define i1 @and_of_compares(i8 %a, i8 %b, i8 %c) {
entry:
  %c1 = icmp ule i8 %a, %b
  %c2 = icmp ule i8 %a, %c
  %and = and i1 %c1, %c2
  ret i1 %and
}
