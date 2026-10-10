; RUN: not --crash opt -passes=loop-unroll -unroll-runtime -unroll-remainder -unroll-runtime-multi-exit -unroll-runtime-epilog=0 -disable-output %s 2>&1 | FileCheck %s

; FIXME: UnrollLoop captures the latch of %inner.header before calling
; UnrollRuntimeLoopRemainder. Unrolling the prolog remainder runs
; LoopSimplify on the loop nest, which erases that latch block, and UnrollLoop
; then reads the freed block (use-after-free, reported by valgrind). This
; should not crash.
; CHECK: Stack dump:
; CHECK: Running pass "loop-unroll<O2>" on function "test"

define i32 @test(i32 %n, i32 %seed, i1 %c.outer) {
entry:
  br label %outer

outer:
  %p = phi i32 [ %seed, %entry ], [ %seed, %inner.exit ]
  br label %inner.header

inner.header:
  %i = phi i32 [ 0, %outer ], [ %i.next, %inner.latch ]
  %i.next = add i32 %i, 1
  br i1 true, label %inner.latch, label %inner.exit

inner.latch:
  %c = icmp ult i32 %i, %n
  br i1 %c, label %inner.header, label %inner.exit

inner.exit:
  %lc = phi i32 [ %p, %inner.header ], [ %seed, %inner.latch ]
  br i1 %c.outer, label %outer, label %exit

exit:
  ret i32 %lc
}
