; RUN: opt -passes=constraint-elimination -constraint-elimination-dump-reproducers \
; RUN:   -disable-output %s
;
; Check that generating reproducers does not crash.

@g = global [16 x i8] zeroinitializer

; Facts added while checking the operands of an and/or must be tracked for the
; reproducer stack as well, otherwise removing them underflows it.
define i1 @and_of_equal_conditions(i8 %a, i8 %b) {
entry:
  %c1 = icmp ule i8 %a, %b
  %c2 = icmp ule i8 %a, %b
  %and = and i1 %c1, %c2
  ret i1 %and
}

; The reproducer function must not reference globals of the original module.
define i1 @condition_using_global(i64 %i) {
entry:
  %p = getelementptr inbounds i8, ptr @g, i64 %i
  %c = icmp ugt ptr %p, @g
  br i1 %c, label %then, label %else

then:
  %c2 = icmp ugt ptr %p, @g
  ret i1 %c2

else:
  ret i1 false
}
