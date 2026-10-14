; RUN: not opt -passes='loop(loop-unroll-full<unknown>)' -disable-output %s 2>&1 | FileCheck -check-prefix=UNKNOWN %s
; RUN: not opt -passes='loop(loop-unroll-full<defer-vectorizable;only-deferred>)' -disable-output %s 2>&1 | FileCheck -check-prefix=EXCLUSIVE %s

; Parsing of the loop-unroll-full pass parameters: unknown parameters are
; rejected, and defer-vectorizable and only-deferred are mutually exclusive.

; UNKNOWN: invalid LoopFullUnrollPass parameter 'unknown'
; EXCLUSIVE: LoopFullUnrollPass parameters 'defer-vectorizable' and 'only-deferred' are mutually exclusive

define void @f() {
  ret void
}
