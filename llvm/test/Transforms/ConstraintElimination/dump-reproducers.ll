; RUN: opt -passes=constraint-elimination -constraint-elimination-dump-reproducers \
; RUN:   -disable-output %s
;
; Check that generating reproducers does not crash.

@g = global [16 x i8] zeroinitializer

; Facts added while checking the operands of an and/or must also be recorded
; for the reproducer.
define i1 @and_of_compares(i8 %a, i8 %b, i8 %c) {
entry:
  %c1 = icmp ule i8 %a, %b
  %c2 = icmp ule i8 %a, %c
  %and = and i1 %c1, %c2
  ret i1 %and
}

; The reproducer function must not reference globals of the original module.
define i1 @condition_using_global(i64 %i) {
entry:
  %p = getelementptr inbounds nuw i8, ptr @g, i64 %i
  %c = icmp uge ptr %p, @g
  ret i1 %c
}
