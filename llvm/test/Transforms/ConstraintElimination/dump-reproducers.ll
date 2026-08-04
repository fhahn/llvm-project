; RUN: opt -passes=constraint-elimination -constraint-elimination-dump-reproducers \
; RUN:   -disable-output %s
;
; Check that generating reproducers does not crash.

; Facts added while checking the operands of an and/or must be tracked for the
; reproducer stack as well, otherwise removing them underflows it.
define i1 @and_of_equal_conditions(i8 %a, i8 %b) {
entry:
  %c1 = icmp ule i8 %a, %b
  %c2 = icmp ule i8 %a, %b
  %and = and i1 %c1, %c2
  ret i1 %and
}
