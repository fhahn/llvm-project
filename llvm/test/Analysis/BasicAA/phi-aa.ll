; RUN: opt < %s -aa-pipeline=basic-aa -passes=aa-eval -print-all-alias-modref-info -disable-output 2>&1 | FileCheck %s
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v64:64:64-v128:128:128-a0:0:64-s0:64:64-f80:128:128-n8:16:32:64-S128"
target triple = "x86_64-unknown-linux-gnu"

; rdar://7282591

@X = common global i32 0
@Y = common global i32 0
@Z = common global i32 0

; CHECK-LABEL: foo
; CHECK:  NoAlias: i32* %P, i32* @Z

define void @foo(i32 %cond) nounwind {
entry:
  %"alloca point" = bitcast i32 0 to i32
  %tmp = icmp ne i32 %cond, 0
  br i1 %tmp, label %bb, label %bb1

bb:
  br label %bb2

bb1:
  br label %bb2

bb2:
  %P = phi ptr [ @X, %bb ], [ @Y, %bb1 ]
  %tmp1 = load i32, ptr @Z, align 4
  store i32 123, ptr %P, align 4
  %tmp2 = load i32, ptr @Z, align 4
  br label %return

return:
  ret void
}

; Pointers can vary in between iterations of loops.
; PR18068

; CHECK-LABEL: pr18068
; CHECK: MayAlias: i32* %0, i32* %arrayidx5
; CHECK: NoAlias: i32* %arrayidx13, i32* %arrayidx5

define i32 @pr18068(ptr %jj7, ptr %j) {
entry:
  %oa5 = alloca [100 x i32], align 16
  br label %codeRepl

codeRepl:
  %0 = phi ptr [ %arrayidx13, %for.body ], [ %j, %entry ]
  %targetBlock = call i1 @cond(ptr %jj7)
  br i1 %targetBlock, label %for.body, label %bye

for.body:
  %1 = load i32, ptr %jj7, align 4
  %idxprom4 = zext i32 %1 to i64
  %arrayidx5 = getelementptr inbounds [100 x i32], ptr %oa5, i64 0, i64 %idxprom4
  %2 = load i32, ptr %arrayidx5, align 4
  %sub6 = sub i32 %2, 6
  store i32 %sub6, ptr %arrayidx5, align 4
  ; %0 and %arrayidx5 can alias! It is not safe to DSE the above store.
  %3 = load i32, ptr %0, align 4
  store i32 %3, ptr %arrayidx5, align 4
  %sub11 = add i32 %1, -1
  %idxprom12 = zext i32 %sub11 to i64
  %arrayidx13 = getelementptr inbounds [100 x i32], ptr %oa5, i64 0, i64 %idxprom12
  load i32, ptr %arrayidx13
  call void @inc(ptr %jj7)
  br label %codeRepl

bye:
  %.reload = load i32, ptr %jj7, align 4
  ret i32 %.reload
}

declare i1 @cond(ptr)

declare void @inc(ptr)


; When we have a chain of phis in nested loops we should recognise if there's
; actually only one underlying value.
; FIXME: All of these could be NoAlias.
; CHECK-LABEL: loop_phi_chain
; CHECK: MayAlias: i32* %val1, i32* @Y
; CHECK: MayAlias: i32* %val2, i32* @Y
; CHECK: MayAlias: i32* %val3, i32* @Y
define void @loop_phi_chain(i32 %a, i32 %b, i32 %c) {
entry:
  br label %loop1

loop1:
  %n1 = phi i32 [ 0, %entry ], [ %add1, %loop2 ]
  %val1 = phi ptr [ @X, %entry ], [ %val2, %loop2 ]
  load i32, ptr %val1
  %add1 = add i32 %n1, 1
  %cmp1 = icmp ne i32 %n1, 32
  br i1 %cmp1, label %loop2, label %end

loop2:
  %n2 = phi i32 [ 0, %loop1 ], [ %add2, %loop3 ]
  %val2 = phi ptr [ %val1, %loop1 ], [ %val3, %loop3 ]
  load i32, ptr %val2
  %add2 = add i32 %n2, 1
  %cmp2 = icmp ne i32 %n2, 32
  br i1 %cmp2, label %loop3, label %loop1

loop3:
  %n3 = phi i32 [ 0, %loop2 ], [ %add3, %loop3 ]
  %val3 = phi ptr [ %val2, %loop2 ], [ %val3, %loop3 ]
  store i32 0, ptr %val3, align 4
  store i32 0, ptr @Y, align 4
  %add3 = add i32 %n3, 1
  %cmp3 = icmp ne i32 %n3, 32
  br i1 %cmp3, label %loop3, label %loop2

end:
  ret void
}

; CHECK-LABEL: phi_and_select
; CHECK: MustAlias: i32* %p, i32* %s
define void @phi_and_select(i1 %c, i1 %c2, ptr %x, ptr %y) {
entry:
  br i1 %c, label %true, label %false

true:
  br label %exit

false:
  br label %exit

exit:
  %p = phi ptr [ %x, %true ], [ %y, %false ]
  %s = select i1 %c2, ptr %p, ptr %p
  store i32 0, ptr %p
  store i32 0, ptr %s
  ret void
}

; CHECK-LABEL: phi_and_phi_cycle
; CHECK: NoAlias: i32* %p1, i32* %p2
define void @phi_and_phi_cycle(ptr noalias %x, ptr noalias %y) {
entry:
  br label %loop

loop:
  %p1 = phi ptr [ %x, %entry ], [ %p1.next, %loop ]
  %p2 = phi ptr [ %y, %entry ], [ %p2.next, %loop ]
  %p1.next = getelementptr i32, ptr %p1, i64 1
  %p2.next = getelementptr i32, ptr %p1, i64 2
  store i32 0, ptr %p1
  store i32 0, ptr %p2
  br label %loop
}

; CHECK-LABEL: phi_and_gep_unknown_size
; CHECK: NoModRef:   call void @llvm.memset.p0.i32(ptr %g, i8 0, i32 %size, i1 false) <->   call void @llvm.memset.p0.i32(ptr %z, i8 0, i32 %size, i1 false)
define void @phi_and_gep_unknown_size(i1 %c, ptr %x, ptr %y, ptr noalias %z, i32 %size) {
entry:
  br i1 %c, label %true, label %false

true:
  br label %exit

false:
  br label %exit

exit:
  %p = phi ptr [ %x, %true ], [ %y, %false ]
  %g = getelementptr inbounds i8, ptr %p, i64 1
  call void @llvm.memset.p0.i32(ptr %g, i8 0, i32 %size, i1 false)
  call void @llvm.memset.p0.i32(ptr %z, i8 0, i32 %size, i1 false)
  ret void
}

; If a phi arm is the other pointer, the gep may alias it.
; CHECK-LABEL: phi_and_gep_unknown_size_may
; CHECK: MayAlias:	i8* %g, i8* %z
define void @phi_and_gep_unknown_size_may(i1 %c, ptr %x, ptr %z) {
entry:
  br i1 %c, label %true, label %false

true:
  br label %exit

false:
  br label %exit

exit:
  %p = phi ptr [ %x, %true ], [ %z, %false ]
  %g = getelementptr inbounds i8, ptr %p, i64 1
  store i8 0, ptr %g
  store i8 0, ptr %z
  ret void
}

declare void @llvm.memset.p0.i32(ptr, i8, i32, i1)

; CHECK-LABEL: unsound_inequality
; CHECK: MayAlias:  i32* %arrayidx5, i32* %phi
; CHECK: MayAlias:  i32* %arrayidx13, i32* %phi
; CHECK: NoAlias:   i32* %arrayidx13, i32* %arrayidx5

; When recursively reasoning about phis, we can't use predicates between
; two values as we might be comparing the two from different iterations.
define i32 @unsound_inequality(ptr %jj7, ptr %j) {
entry:
  %oa5 = alloca [100 x i32], align 16
  br label %for.body

for.body:                                         ; preds = %for.body, %entry
  %phi = phi ptr [ %arrayidx13, %for.body ], [ %j, %entry ]
  load i32, ptr %phi
  %idx = load i32, ptr %jj7, align 4
  %arrayidx5 = getelementptr inbounds [100 x i32], ptr %oa5, i64 0, i32 %idx
  store i32 0, ptr %arrayidx5, align 4
  store i32 0, ptr %phi, align 4
  %notequal = add i32 %idx, 1
  %arrayidx13 = getelementptr inbounds [100 x i32], ptr %oa5, i64 0, i32 %notequal
  store i32 0, ptr %arrayidx13, align 4
  br label %for.body
} 

; CHECK-LABEL: single_arg_phi
; CHECK: NoAlias: i32* %ptr, i32* %ptr.next
; CHECK: MustAlias: i32* %ptr, i32* %ptr.phi
; CHECK: MustAlias: i32* %ptr.next, i32* %ptr.next.phi
define void @single_arg_phi(ptr %ptr.base) {
entry:
  br label %loop

loop:
  %ptr = phi ptr [ %ptr.base, %entry ], [ %ptr.next, %split ]
  %ptr.next = getelementptr inbounds i32, ptr %ptr, i64 1
  load i32, ptr %ptr
  load i32, ptr %ptr.next
  br label %split

split:
  %ptr.phi = phi ptr [ %ptr, %loop ]
  %ptr.next.phi = phi ptr [ %ptr.next, %loop ]
  load i32, ptr %ptr.phi
  load i32, ptr %ptr.next.phi
  br label %loop
}

; CHECK-LABEL: phi_of_geps_based_on_alloca
; CHECK: NoAlias:	i32* %gep3, i32* %phi
define void @phi_of_geps_based_on_alloca(i1 %c) {
  %a = alloca [3 x i32]
  br i1 %c, label %if, label %else

if:
  %gep1 = getelementptr i32, ptr %a, i64 1
  br label %join

else:
  %gep2 = getelementptr i32, ptr %a, i64 2
  br label %join

join:
  %phi = phi ptr [ %gep1, %if ], [ %gep2, %else ]
  %gep3 = getelementptr i32, ptr %a, i64 3
  load i32, ptr %phi
  load i32, ptr %gep3
  ret void
}

; Don't crash with an unreachable predecessor.
; CHECK-LABEL: pr59360
; CHECK: MayAlias: i32* %loaded.ptr, i32* %phi
define void @pr59360() {
entry:
  br label %outer

loopexit:                                         ; No predecessors!
  br label %outer

outer:                                            ; preds = %loopexit, %entry
  %phi = phi ptr [ %loaded.ptr, %loopexit ], [ null, %entry ]
  store i32 0, ptr %phi, align 4
  %loaded.ptr = load ptr, ptr null, align 8
  %0 = load i32, ptr %loaded.ptr, align 4
  ret void
}

; Tests for pointers carried through nested loop phis and LCSSA phis.

; %ptr.inner is based on argument %start, which may alias argument %q.
; CHECK-LABEL: nested_loop_arg
; CHECK: MayAlias: i8* %ptr.inner, i8* %q
; CHECK: MayAlias: i8* %buf, i8* %ptr.inner
define void @nested_loop_arg(ptr %start, ptr %q, i1 %c) {
entry:
  %buf = alloca [16 x i8]
  %lq = load i8, ptr %q
  br label %outer

outer:
  %ptr.outer = phi ptr [ %start, %entry ], [ %ptr.next.lcssa, %outer.latch ]
  br label %inner

inner:
  %ptr.inner = phi ptr [ %ptr.outer, %outer ], [ %ptr.next, %inner ]
  %l = load i8, ptr %buf
  store i8 %l, ptr %ptr.inner
  %ptr.next = getelementptr inbounds i8, ptr %ptr.inner, i64 1
  br i1 %c, label %outer.latch, label %inner

outer.latch:
  %ptr.next.lcssa = phi ptr [ %ptr.next, %inner ]
  br i1 %c, label %exit, label %outer

exit:
  ret void
}

; %ptr.inner is based on a load and %buf is never captured.
; CHECK-LABEL: nested_loop_load_not_captured
; CHECK: MayAlias: i8* %buf, i8* %ptr.inner
define void @nested_loop_load_not_captured(ptr %p, i1 %c) {
entry:
  %buf = alloca [16 x i8]
  %start = load ptr, ptr %p
  br label %outer

outer:
  %ptr.outer = phi ptr [ %start, %entry ], [ %ptr.next.lcssa, %outer.latch ]
  br label %inner

inner:
  %ptr.inner = phi ptr [ %ptr.outer, %outer ], [ %ptr.next, %inner ]
  %l = load i8, ptr %buf
  store i8 %l, ptr %ptr.inner
  %ptr.next = getelementptr inbounds i8, ptr %ptr.inner, i64 1
  br i1 %c, label %outer.latch, label %inner

outer.latch:
  %ptr.next.lcssa = phi ptr [ %ptr.next, %inner ]
  br i1 %c, label %exit, label %outer

exit:
  ret void
}

; Both pointers are phis carried through nested loops, in different blocks, and
; based on distinct allocas.
; CHECK-LABEL: nested_loop_phis_both_sides
; CHECK: MayAlias: i8* %pa.inner, i8* %pb.inner
define void @nested_loop_phis_both_sides(i1 %c) {
entry:
  %a = alloca [16 x i8]
  %b = alloca [16 x i8]
  br label %outer

outer:
  %pa.outer = phi ptr [ %a, %entry ], [ %pa.inner, %latch ]
  %pb.outer = phi ptr [ %b, %entry ], [ %pb.inner, %latch ]
  br label %inner

inner:
  %pa.inner = phi ptr [ %pa.outer, %outer ], [ %pa.next, %inner ]
  %pa.next = getelementptr i8, ptr %pa.inner, i64 1
  br i1 %c, label %inner, label %inner.b.ph

inner.b.ph:
  br label %inner.b

inner.b:
  %pb.inner = phi ptr [ %pb.outer, %inner.b.ph ], [ %pb.next, %inner.b ]
  load i8, ptr %pa.inner
  load i8, ptr %pb.inner
  %pb.next = getelementptr i8, ptr %pb.inner, i64 1
  br i1 %c, label %inner.b, label %latch

latch:
  br i1 %c, label %outer, label %exit

exit:
  ret void
}

; %ptr.inner is based on a load and %buf is captured before the load.
; CHECK-LABEL: nested_loop_load_captured
; CHECK: MayAlias: i8* %buf, i8* %ptr.inner
define void @nested_loop_load_captured(ptr %p, i1 %c) {
entry:
  %buf = alloca [16 x i8]
  call void @inc(ptr %buf)
  %start = load ptr, ptr %p
  br label %outer

outer:
  %ptr.outer = phi ptr [ %start, %entry ], [ %ptr.next.lcssa, %outer.latch ]
  br label %inner

inner:
  %ptr.inner = phi ptr [ %ptr.outer, %outer ], [ %ptr.next, %inner ]
  %l = load i8, ptr %buf
  store i8 %l, ptr %ptr.inner
  %ptr.next = getelementptr inbounds i8, ptr %ptr.inner, i64 1
  br i1 %c, label %outer.latch, label %inner

outer.latch:
  %ptr.next.lcssa = phi ptr [ %ptr.next, %inner ]
  br i1 %c, label %exit, label %outer

exit:
  ret void
}

; %start is a phi with many incoming values, which are all %a.
; CHECK-LABEL: nested_loop_phi_same_incoming
; CHECK: MayAlias: i8* %buf, i8* %ptr.inner
define void @nested_loop_phi_same_incoming(ptr %a, i32 %x, i1 %c) {
entry:
  %buf = alloca [16 x i8]
  switch i32 %x, label %join [ i32 0, label %bb0
                                i32 1, label %bb1
                                i32 2, label %bb2
                                i32 3, label %bb3
                                i32 4, label %bb4
                                i32 5, label %bb5
                                i32 6, label %bb6
                                i32 7, label %bb7 ]

bb0:
  br label %join

bb1:
  br label %join

bb2:
  br label %join

bb3:
  br label %join

bb4:
  br label %join

bb5:
  br label %join

bb6:
  br label %join

bb7:
  br label %join

join:
  %start = phi ptr [ %a, %entry ], [ %a, %bb0 ], [ %a, %bb1 ], [ %a, %bb2 ], [ %a, %bb3 ], [ %a, %bb4 ], [ %a, %bb5 ], [ %a, %bb6 ], [ %a, %bb7 ]
  br label %outer

outer:
  %ptr.outer = phi ptr [ %start, %join ], [ %ptr.next.lcssa, %outer.latch ]
  br label %inner

inner:
  %ptr.inner = phi ptr [ %ptr.outer, %outer ], [ %ptr.next, %inner ]
  %l = load i8, ptr %buf
  store i8 %l, ptr %ptr.inner
  %ptr.next = getelementptr inbounds i8, ptr %ptr.inner, i64 1
  br i1 %c, label %outer.latch, label %inner

outer.latch:
  %ptr.next.lcssa = phi ptr [ %ptr.next, %inner ]
  br i1 %c, label %exit, label %outer

exit:
  ret void
}

; %start is a chain of selects of arguments.
; CHECK-LABEL: nested_loop_select_chain_of_args
; CHECK: MayAlias: i8* %buf, i8* %ptr.inner
define void @nested_loop_select_chain_of_args(ptr %a, ptr %b, ptr %d, ptr %e, i1 %c) {
entry:
  %buf = alloca [16 x i8]
  %s1 = select i1 %c, ptr %a, ptr %b
  %s2 = select i1 %c, ptr %s1, ptr %d
  %start = select i1 %c, ptr %s2, ptr %e
  br label %outer

outer:
  %ptr.outer = phi ptr [ %start, %entry ], [ %ptr.next.lcssa, %outer.latch ]
  br label %inner

inner:
  %ptr.inner = phi ptr [ %ptr.outer, %outer ], [ %ptr.next, %inner ]
  %l = load i8, ptr %buf
  store i8 %l, ptr %ptr.inner
  %ptr.next = getelementptr inbounds i8, ptr %ptr.inner, i64 1
  br i1 %c, label %outer.latch, label %inner

outer.latch:
  %ptr.next.lcssa = phi ptr [ %ptr.next, %inner ]
  br i1 %c, label %exit, label %outer

exit:
  ret void
}

; %ptr.inner may be based on %buf.
; CHECK-LABEL: nested_loop_select_of_arg_and_alloca
; CHECK: MayAlias: i8* %buf, i8* %ptr.inner
define void @nested_loop_select_of_arg_and_alloca(ptr %a, i1 %c) {
entry:
  %buf = alloca [16 x i8]
  %start = select i1 %c, ptr %a, ptr %buf
  br label %outer

outer:
  %ptr.outer = phi ptr [ %start, %entry ], [ %ptr.next.lcssa, %outer.latch ]
  br label %inner

inner:
  %ptr.inner = phi ptr [ %ptr.outer, %outer ], [ %ptr.next, %inner ]
  %l = load i8, ptr %buf
  store i8 %l, ptr %ptr.inner
  %ptr.next = getelementptr inbounds i8, ptr %ptr.inner, i64 1
  br i1 %c, label %outer.latch, label %inner

outer.latch:
  %ptr.next.lcssa = phi ptr [ %ptr.next, %inner ]
  br i1 %c, label %exit, label %outer

exit:
  ret void
}

; %ptr.outer has two arguments and a recursive LCSSA phi as incoming values.
; CHECK-LABEL: recursive_lcssa_phi_and_two_args
; CHECK: NoAlias: i8* %buf, i8* %ptr.outer
define void @recursive_lcssa_phi_and_two_args(ptr %a, ptr %b, i1 %c) {
entry:
  %buf = alloca [16 x i8]
  br i1 %c, label %then, label %outer

then:
  br label %outer

outer:
  %ptr.outer = phi ptr [ %a, %entry ], [ %b, %then ], [ %ptr.next.lcssa, %outer.latch ]
  br label %inner

inner:
  %iv = phi i64 [ 0, %outer ], [ %iv.next, %inner ]
  %ptr.next = getelementptr inbounds i8, ptr %ptr.outer, i64 %iv
  %l = load i8, ptr %buf
  store i8 %l, ptr %ptr.outer
  %iv.next = add i64 %iv, 1
  br i1 %c, label %outer.latch, label %inner

outer.latch:
  %ptr.next.lcssa = phi ptr [ %ptr.next, %inner ]
  br i1 %c, label %exit, label %outer

exit:
  ret void
}

; %ptr.outer has a recursive LCSSA phi and another loop's LCSSA phi as inputs.
; CHECK-LABEL: recursive_lcssa_phi_and_phi
; CHECK: NoAlias: i8* %buf, i8* %ptr.outer
define void @recursive_lcssa_phi_and_phi(ptr %a, i1 %c) {
entry:
  %buf = alloca [16 x i8]
  br label %pre

pre:
  %ptr.pre = phi ptr [ %a, %entry ], [ %ptr.pre.next, %pre ]
  %ptr.pre.next = getelementptr inbounds i8, ptr %ptr.pre, i64 1
  br i1 %c, label %pre.exit, label %pre

pre.exit:
  %ptr.pre.lcssa = phi ptr [ %ptr.pre.next, %pre ]
  br label %outer

outer:
  %ptr.outer = phi ptr [ %ptr.pre.lcssa, %pre.exit ], [ %ptr.next.lcssa, %outer.latch ]
  br label %inner

inner:
  %iv = phi i64 [ 0, %outer ], [ %iv.next, %inner ]
  %ptr.next = getelementptr inbounds i8, ptr %ptr.outer, i64 %iv
  %l = load i8, ptr %buf
  store i8 %l, ptr %ptr.outer
  %iv.next = add i64 %iv, 1
  br i1 %c, label %outer.latch, label %inner

outer.latch:
  %ptr.next.lcssa = phi ptr [ %ptr.next, %inner ]
  br i1 %c, label %exit, label %outer

exit:
  ret void
}

; %ptr.outer may be based on %buf.
; CHECK-LABEL: recursive_lcssa_phi_and_alloca
; CHECK: MayAlias: i8* %buf, i8* %ptr.outer
define void @recursive_lcssa_phi_and_alloca(ptr %a, i1 %c) {
entry:
  %buf = alloca [16 x i8]
  br i1 %c, label %then, label %outer

then:
  br label %outer

outer:
  %ptr.outer = phi ptr [ %a, %entry ], [ %buf, %then ], [ %ptr.next.lcssa, %outer.latch ]
  br label %inner

inner:
  %iv = phi i64 [ 0, %outer ], [ %iv.next, %inner ]
  %ptr.next = getelementptr inbounds i8, ptr %ptr.outer, i64 %iv
  %l = load i8, ptr %buf
  store i8 %l, ptr %ptr.outer
  %iv.next = add i64 %iv, 1
  br i1 %c, label %outer.latch, label %inner

outer.latch:
  %ptr.next.lcssa = phi ptr [ %ptr.next, %inner ]
  br i1 %c, label %exit, label %outer

exit:
  ret void
}

; %ptr.outer has two recursive LCSSA phis and %small as incoming values.
; CHECK-LABEL: recursive_lcssa_phis_and_small_alloca
; CHECK: NoAlias: i8* %ptr.outer, i32* %q
define void @recursive_lcssa_phis_and_small_alloca(ptr %pp, i1 %c) {
entry:
  %small = alloca i8
  call void @inc(ptr %small)
  %q = load ptr, ptr %pp
  %l = load i32, ptr %q
  br label %outer

outer:
  %ptr.outer = phi ptr [ %small, %entry ], [ %ptr.next.lcssa.1, %outer.latch.1 ], [ %ptr.next.lcssa.2, %outer.latch.2 ]
  store i8 0, ptr %ptr.outer
  br label %inner

inner:
  %iv = phi i64 [ 0, %outer ], [ %iv.next, %inner ]
  %ptr.next = getelementptr inbounds i8, ptr %ptr.outer, i64 %iv
  %iv.next = add i64 %iv, 1
  br i1 %c, label %inner.exit, label %inner

inner.exit:
  br i1 %c, label %outer.latch.1, label %outer.latch.2

outer.latch.1:
  %ptr.next.lcssa.1 = phi ptr [ %ptr.next, %inner.exit ]
  br i1 %c, label %exit, label %outer

outer.latch.2:
  %ptr.next.lcssa.2 = phi ptr [ %ptr.next, %inner.exit ]
  br i1 %c, label %exit, label %outer

exit:
  ret void
}

; The start value of %ptr and %other are selects with the same condition, so
; %ptr is based on %a if and only if %other is %b and vice versa.
; CHECK-LABEL: recursive_phi_select_same_cond
; CHECK: NoAlias: i8* %other, i8* %ptr
define void @recursive_phi_select_same_cond(i1 %c, i1 %c2) {
entry:
  %a = alloca [16 x i8]
  %b = alloca [16 x i8]
  call void @inc(ptr %a)
  call void @inc(ptr %b)
  %start = select i1 %c, ptr %a, ptr %b
  %other = select i1 %c, ptr %b, ptr %a
  br label %loop

loop:
  %ptr = phi ptr [ %start, %entry ], [ %ptr.next, %loop ]
  store i8 0, ptr %ptr
  store i8 1, ptr %other
  %ptr.next = getelementptr inbounds i8, ptr %ptr, i64 1
  br i1 %c2, label %loop, label %exit

exit:
  ret void
}

; %ptr is based on one of many arguments, which cannot alias %buf.
; CHECK-LABEL: recursive_phi_long_select_chain
; CHECK: NoAlias: i8* %buf, i8* %ptr
define void @recursive_phi_long_select_chain(ptr %a0, ptr %a1, ptr %a2, ptr %a3, ptr %a4, ptr %a5, ptr %a6, ptr %a7, ptr %a8, ptr %a9, ptr %a10, ptr %a11, ptr %a12, ptr %a13, ptr %a14, ptr %a15, ptr %a16, ptr %a17, ptr %a18, ptr %a19, ptr %a20, ptr %a21, ptr %a22, ptr %a23, ptr %a24, ptr %a25, ptr %a26, ptr %a27, ptr %a28, ptr %a29, ptr %a30, ptr %a31, ptr %a32, ptr %a33, ptr %a34, ptr %a35, ptr %a36, ptr %a37, ptr %a38, ptr %a39, i1 %c) {
entry:
  %buf = alloca [16 x i8]
  %s1 = select i1 %c, ptr %a0, ptr %a1
  %s2 = select i1 %c, ptr %s1, ptr %a2
  %s3 = select i1 %c, ptr %s2, ptr %a3
  %s4 = select i1 %c, ptr %s3, ptr %a4
  %s5 = select i1 %c, ptr %s4, ptr %a5
  %s6 = select i1 %c, ptr %s5, ptr %a6
  %s7 = select i1 %c, ptr %s6, ptr %a7
  %s8 = select i1 %c, ptr %s7, ptr %a8
  %s9 = select i1 %c, ptr %s8, ptr %a9
  %s10 = select i1 %c, ptr %s9, ptr %a10
  %s11 = select i1 %c, ptr %s10, ptr %a11
  %s12 = select i1 %c, ptr %s11, ptr %a12
  %s13 = select i1 %c, ptr %s12, ptr %a13
  %s14 = select i1 %c, ptr %s13, ptr %a14
  %s15 = select i1 %c, ptr %s14, ptr %a15
  %s16 = select i1 %c, ptr %s15, ptr %a16
  %s17 = select i1 %c, ptr %s16, ptr %a17
  %s18 = select i1 %c, ptr %s17, ptr %a18
  %s19 = select i1 %c, ptr %s18, ptr %a19
  %s20 = select i1 %c, ptr %s19, ptr %a20
  %s21 = select i1 %c, ptr %s20, ptr %a21
  %s22 = select i1 %c, ptr %s21, ptr %a22
  %s23 = select i1 %c, ptr %s22, ptr %a23
  %s24 = select i1 %c, ptr %s23, ptr %a24
  %s25 = select i1 %c, ptr %s24, ptr %a25
  %s26 = select i1 %c, ptr %s25, ptr %a26
  %s27 = select i1 %c, ptr %s26, ptr %a27
  %s28 = select i1 %c, ptr %s27, ptr %a28
  %s29 = select i1 %c, ptr %s28, ptr %a29
  %s30 = select i1 %c, ptr %s29, ptr %a30
  %s31 = select i1 %c, ptr %s30, ptr %a31
  %s32 = select i1 %c, ptr %s31, ptr %a32
  %s33 = select i1 %c, ptr %s32, ptr %a33
  %s34 = select i1 %c, ptr %s33, ptr %a34
  %s35 = select i1 %c, ptr %s34, ptr %a35
  %s36 = select i1 %c, ptr %s35, ptr %a36
  %s37 = select i1 %c, ptr %s36, ptr %a37
  %s38 = select i1 %c, ptr %s37, ptr %a38
  %s39 = select i1 %c, ptr %s38, ptr %a39
  br label %loop

loop:
  %ptr = phi ptr [ %s39, %entry ], [ %ptr.next, %loop ]
  store i8 0, ptr %ptr
  store i8 1, ptr %buf
  %ptr.next = getelementptr inbounds i8, ptr %ptr, i64 1
  br i1 %c, label %loop, label %exit

exit:
  ret void
}

; %p9 is based on arguments through 8 phis with multiple sources including a
; phi.
; CHECK-LABEL: phi_sources_at_limit
; CHECK: MayAlias: i8* %buf, i8* %p9
define void @phi_sources_at_limit(ptr %a0, ptr %a1, ptr %a2, ptr %a3, ptr %a4, ptr %a5, ptr %a6, ptr %a7, ptr %a8, ptr %a9, i1 %c) {
entry:
  %buf = alloca [16 x i8]
  br i1 %c, label %bb1.l, label %bb1.r

bb1.l:
  br label %bb1

bb1.r:
  br label %bb1

bb1:
  %p1 = phi ptr [ %a0, %bb1.l ], [ %a1, %bb1.r ]
  br i1 %c, label %bb2.l, label %bb2.r

bb2.l:
  br label %bb2

bb2.r:
  br label %bb2

bb2:
  %p2 = phi ptr [ %p1, %bb2.l ], [ %a2, %bb2.r ]
  br i1 %c, label %bb3.l, label %bb3.r

bb3.l:
  br label %bb3

bb3.r:
  br label %bb3

bb3:
  %p3 = phi ptr [ %p2, %bb3.l ], [ %a3, %bb3.r ]
  br i1 %c, label %bb4.l, label %bb4.r

bb4.l:
  br label %bb4

bb4.r:
  br label %bb4

bb4:
  %p4 = phi ptr [ %p3, %bb4.l ], [ %a4, %bb4.r ]
  br i1 %c, label %bb5.l, label %bb5.r

bb5.l:
  br label %bb5

bb5.r:
  br label %bb5

bb5:
  %p5 = phi ptr [ %p4, %bb5.l ], [ %a5, %bb5.r ]
  br i1 %c, label %bb6.l, label %bb6.r

bb6.l:
  br label %bb6

bb6.r:
  br label %bb6

bb6:
  %p6 = phi ptr [ %p5, %bb6.l ], [ %a6, %bb6.r ]
  br i1 %c, label %bb7.l, label %bb7.r

bb7.l:
  br label %bb7

bb7.r:
  br label %bb7

bb7:
  %p7 = phi ptr [ %p6, %bb7.l ], [ %a7, %bb7.r ]
  br i1 %c, label %bb8.l, label %bb8.r

bb8.l:
  br label %bb8

bb8.r:
  br label %bb8

bb8:
  %p8 = phi ptr [ %p7, %bb8.l ], [ %a8, %bb8.r ]
  br i1 %c, label %bb9.l, label %bb9.r

bb9.l:
  br label %bb9

bb9.r:
  br label %bb9

bb9:
  %p9 = phi ptr [ %p8, %bb9.l ], [ %a9, %bb9.r ]
  br i1 %c, label %exit, label %exit

exit:
  store i8 0, ptr %p9
  store i8 1, ptr %buf
  ret void
}

; %p10 is based on arguments through 9 phis with multiple sources including a
; phi.
; CHECK-LABEL: phi_sources_over_limit
; CHECK: MayAlias: i8* %buf, i8* %p10
define void @phi_sources_over_limit(ptr %a0, ptr %a1, ptr %a2, ptr %a3, ptr %a4, ptr %a5, ptr %a6, ptr %a7, ptr %a8, ptr %a9, ptr %a10, i1 %c) {
entry:
  %buf = alloca [16 x i8]
  br i1 %c, label %bb1.l, label %bb1.r

bb1.l:
  br label %bb1

bb1.r:
  br label %bb1

bb1:
  %p1 = phi ptr [ %a0, %bb1.l ], [ %a1, %bb1.r ]
  br i1 %c, label %bb2.l, label %bb2.r

bb2.l:
  br label %bb2

bb2.r:
  br label %bb2

bb2:
  %p2 = phi ptr [ %p1, %bb2.l ], [ %a2, %bb2.r ]
  br i1 %c, label %bb3.l, label %bb3.r

bb3.l:
  br label %bb3

bb3.r:
  br label %bb3

bb3:
  %p3 = phi ptr [ %p2, %bb3.l ], [ %a3, %bb3.r ]
  br i1 %c, label %bb4.l, label %bb4.r

bb4.l:
  br label %bb4

bb4.r:
  br label %bb4

bb4:
  %p4 = phi ptr [ %p3, %bb4.l ], [ %a4, %bb4.r ]
  br i1 %c, label %bb5.l, label %bb5.r

bb5.l:
  br label %bb5

bb5.r:
  br label %bb5

bb5:
  %p5 = phi ptr [ %p4, %bb5.l ], [ %a5, %bb5.r ]
  br i1 %c, label %bb6.l, label %bb6.r

bb6.l:
  br label %bb6

bb6.r:
  br label %bb6

bb6:
  %p6 = phi ptr [ %p5, %bb6.l ], [ %a6, %bb6.r ]
  br i1 %c, label %bb7.l, label %bb7.r

bb7.l:
  br label %bb7

bb7.r:
  br label %bb7

bb7:
  %p7 = phi ptr [ %p6, %bb7.l ], [ %a7, %bb7.r ]
  br i1 %c, label %bb8.l, label %bb8.r

bb8.l:
  br label %bb8

bb8.r:
  br label %bb8

bb8:
  %p8 = phi ptr [ %p7, %bb8.l ], [ %a8, %bb8.r ]
  br i1 %c, label %bb9.l, label %bb9.r

bb9.l:
  br label %bb9

bb9.r:
  br label %bb9

bb9:
  %p9 = phi ptr [ %p8, %bb9.l ], [ %a9, %bb9.r ]
  br i1 %c, label %bb10.l, label %bb10.r

bb10.l:
  br label %bb10

bb10.r:
  br label %bb10

bb10:
  %p10 = phi ptr [ %p9, %bb10.l ], [ %a10, %bb10.r ]
  br i1 %c, label %exit, label %exit

exit:
  store i8 0, ptr %p10
  store i8 1, ptr %buf
  ret void
}

; %p is not recursive and takes the value of %q or %a, and %q the value of %a,
; so the accesses through %p are at offset 0 of %a.
; CHECK-LABEL: phi_sources_keep_size
; CHECK: MayAlias: i32* %g, i32* %p
define void @phi_sources_keep_size(ptr %a, i1 %c, i1 %d) {
entry:
  br i1 %c, label %l, label %r

l:
  br i1 %d, label %l1, label %l2

l1:
  br label %lj

l2:
  br label %lj

lj:
  %q = phi ptr [ %a, %l1 ], [ %a, %l2 ]
  br label %j

r:
  br label %j

j:
  %p = phi ptr [ %q, %lj ], [ %a, %r ]
  %g = getelementptr i8, ptr %a, i64 16
  store i32 0, ptr %p
  store i32 1, ptr %g
  ret void
}

; %q is recursive and may be at any offset of %a, so the size of the accesses
; through %p is lost for the sources of %q.
; CHECK-LABEL: phi_sources_size_lost_recursive
; CHECK: MayAlias: i32* %g, i32* %p
define void @phi_sources_size_lost_recursive(ptr %a, i1 %c, i1 %d) {
entry:
  br i1 %c, label %loop, label %r

loop:
  %q = phi ptr [ %a, %entry ], [ %q.next, %loop ]
  %q.next = getelementptr i8, ptr %q, i64 4
  br i1 %d, label %loop, label %lj

lj:
  br label %j

r:
  br label %j

j:
  %p = phi ptr [ %q, %lj ], [ %a, %r ]
  %g = getelementptr i8, ptr %a, i64 16
  store i32 0, ptr %p
  store i32 1, ptr %g
  ret void
}

; %p may be %q + 14, which overlaps %g. %q.off is checked as a whole, using
; its offset.
; CHECK-LABEL: phi_sources_gep_offset
; CHECK: MayAlias: i32* %g, i32* %p
define void @phi_sources_gep_offset(ptr %a, i1 %c, i1 %d) {
entry:
  br i1 %c, label %l, label %r

l:
  br i1 %d, label %l1, label %l2

l1:
  br label %lj

l2:
  br label %lj

lj:
  %q = phi ptr [ %a, %l1 ], [ %a, %l2 ]
  %q.off = getelementptr i8, ptr %q, i64 14
  br label %j

r:
  %s = phi ptr [ %a, %entry ]
  br label %j

j:
  %p = phi ptr [ %q.off, %lj ], [ %s, %r ]
  %g = getelementptr i8, ptr %a, i64 16
  store i32 0, ptr %p
  store i32 1, ptr %g
  ret void
}

; %p may be %q + 14, which overlaps %g. %q is looked through first, then
; %q.off is checked as a whole.
; CHECK-LABEL: phi_sources_gep_offset_second
; CHECK: MayAlias: i32* %g, i32* %p
define void @phi_sources_gep_offset_second(ptr %a, i1 %c, i1 %d) {
entry:
  br i1 %c, label %l, label %r

l:
  br i1 %d, label %l1, label %l2

l1:
  br label %lj

l2:
  br label %lj

lj:
  %q = phi ptr [ %a, %l1 ], [ %a, %l2 ]
  %q.off = getelementptr i8, ptr %q, i64 14
  br i1 %d, label %j1, label %j2

r:
  br label %j3

j1:
  br label %j

j2:
  br label %j

j3:
  br label %j

j:
  %p = phi ptr [ %q, %j1 ], [ %q.off, %j2 ], [ %a, %j3 ]
  %g = getelementptr i8, ptr %a, i64 16
  store i32 0, ptr %p
  store i32 1, ptr %g
  ret void
}

; %p may be %q + 14, which overlaps %g. %q.off is checked as a whole first,
; then %q is looked through.
; CHECK-LABEL: phi_sources_gep_offset_first
; CHECK: MayAlias: i32* %g, i32* %p
define void @phi_sources_gep_offset_first(ptr %a, i1 %c, i1 %d) {
entry:
  br i1 %c, label %l, label %r

l:
  br i1 %d, label %l1, label %l2

l1:
  br label %lj

l2:
  br label %lj

lj:
  %q = phi ptr [ %a, %l1 ], [ %a, %l2 ]
  %q.off = getelementptr i8, ptr %q, i64 14
  br i1 %d, label %j1, label %j2

r:
  br label %j3

j1:
  br label %j

j2:
  br label %j

j3:
  br label %j

j:
  %p = phi ptr [ %q.off, %j1 ], [ %q, %j2 ], [ %a, %j3 ]
  %g = getelementptr i8, ptr %a, i64 16
  store i32 0, ptr %p
  store i32 1, ptr %g
  ret void
}

; Each level merges the two phis of the previous level. Both count against
; the limit, even though the second one only adds phis already looked through.
; CHECK-LABEL: phi_sources_shared_phis
; CHECK: MayAlias: i8* %buf, i8* %q6
define void @phi_sources_shared_phis(ptr %a, ptr %b, i1 %c) {
entry:
  %buf = alloca [16 x i8]
  br i1 %c, label %bb0.l, label %bb0.r

bb0.l:
  br label %bb0

bb0.r:
  br label %bb0

bb0:
  %q0 = phi ptr [ %a, %bb0.l ], [ %b, %bb0.r ]
  %r0 = phi ptr [ %b, %bb0.l ], [ %a, %bb0.r ]
  br i1 %c, label %bb1.l, label %bb1.r

bb1.l:
  br label %bb1

bb1.r:
  br label %bb1

bb1:
  %q1 = phi ptr [ %q0, %bb1.l ], [ %r0, %bb1.r ]
  %r1 = phi ptr [ %r0, %bb1.l ], [ %q0, %bb1.r ]
  br i1 %c, label %bb2.l, label %bb2.r

bb2.l:
  br label %bb2

bb2.r:
  br label %bb2

bb2:
  %q2 = phi ptr [ %q1, %bb2.l ], [ %r1, %bb2.r ]
  %r2 = phi ptr [ %r1, %bb2.l ], [ %q1, %bb2.r ]
  br i1 %c, label %bb3.l, label %bb3.r

bb3.l:
  br label %bb3

bb3.r:
  br label %bb3

bb3:
  %q3 = phi ptr [ %q2, %bb3.l ], [ %r2, %bb3.r ]
  %r3 = phi ptr [ %r2, %bb3.l ], [ %q2, %bb3.r ]
  br i1 %c, label %bb4.l, label %bb4.r

bb4.l:
  br label %bb4

bb4.r:
  br label %bb4

bb4:
  %q4 = phi ptr [ %q3, %bb4.l ], [ %r3, %bb4.r ]
  %r4 = phi ptr [ %r3, %bb4.l ], [ %q3, %bb4.r ]
  br i1 %c, label %bb5.l, label %bb5.r

bb5.l:
  br label %bb5

bb5.r:
  br label %bb5

bb5:
  %q5 = phi ptr [ %q4, %bb5.l ], [ %r4, %bb5.r ]
  %r5 = phi ptr [ %r4, %bb5.l ], [ %q4, %bb5.r ]
  br i1 %c, label %bb6.l, label %bb6.r

bb6.l:
  br label %bb6

bb6.r:
  br label %bb6

bb6:
  %q6 = phi ptr [ %q5, %bb6.l ], [ %r5, %bb6.r ]
  %r6 = phi ptr [ %r5, %bb6.l ], [ %q5, %bb6.r ]
  store i8 0, ptr %q6
  store i8 1, ptr %buf
  ret void
}

; Only phis are looked through. Values based on phis, like the GEPs here, are
; checked as a whole and don't count against the limit.
; CHECK-LABEL: phi_sources_gep_of_phi_chain
; CHECK: NoAlias: i8* %buf, i8* %p10
define void @phi_sources_gep_of_phi_chain(ptr %a0, ptr %a1, ptr %a2, ptr %a3, ptr %a4, ptr %a5, ptr %a6, ptr %a7, ptr %a8, ptr %a9, ptr %a10, i1 %c) {
entry:
  %buf = alloca [16 x i8]
  br label %bb0

bb0:
  %p0 = phi ptr [ %a0, %entry ]
  %p0.next = getelementptr i8, ptr %p0, i64 1
  br i1 %c, label %bb1.l, label %bb1.r

bb1.l:
  br label %bb1

bb1.r:
  br label %bb1

bb1:
  %p1 = phi ptr [ %p0.next, %bb1.l ], [ %a1, %bb1.r ]
  %p1.next = getelementptr i8, ptr %p1, i64 1
  br i1 %c, label %bb2.l, label %bb2.r

bb2.l:
  br label %bb2

bb2.r:
  br label %bb2

bb2:
  %p2 = phi ptr [ %p1.next, %bb2.l ], [ %a2, %bb2.r ]
  %p2.next = getelementptr i8, ptr %p2, i64 1
  br i1 %c, label %bb3.l, label %bb3.r

bb3.l:
  br label %bb3

bb3.r:
  br label %bb3

bb3:
  %p3 = phi ptr [ %p2.next, %bb3.l ], [ %a3, %bb3.r ]
  %p3.next = getelementptr i8, ptr %p3, i64 1
  br i1 %c, label %bb4.l, label %bb4.r

bb4.l:
  br label %bb4

bb4.r:
  br label %bb4

bb4:
  %p4 = phi ptr [ %p3.next, %bb4.l ], [ %a4, %bb4.r ]
  %p4.next = getelementptr i8, ptr %p4, i64 1
  br i1 %c, label %bb5.l, label %bb5.r

bb5.l:
  br label %bb5

bb5.r:
  br label %bb5

bb5:
  %p5 = phi ptr [ %p4.next, %bb5.l ], [ %a5, %bb5.r ]
  %p5.next = getelementptr i8, ptr %p5, i64 1
  br i1 %c, label %bb6.l, label %bb6.r

bb6.l:
  br label %bb6

bb6.r:
  br label %bb6

bb6:
  %p6 = phi ptr [ %p5.next, %bb6.l ], [ %a6, %bb6.r ]
  %p6.next = getelementptr i8, ptr %p6, i64 1
  br i1 %c, label %bb7.l, label %bb7.r

bb7.l:
  br label %bb7

bb7.r:
  br label %bb7

bb7:
  %p7 = phi ptr [ %p6.next, %bb7.l ], [ %a7, %bb7.r ]
  %p7.next = getelementptr i8, ptr %p7, i64 1
  br i1 %c, label %bb8.l, label %bb8.r

bb8.l:
  br label %bb8

bb8.r:
  br label %bb8

bb8:
  %p8 = phi ptr [ %p7.next, %bb8.l ], [ %a8, %bb8.r ]
  %p8.next = getelementptr i8, ptr %p8, i64 1
  br i1 %c, label %bb9.l, label %bb9.r

bb9.l:
  br label %bb9

bb9.r:
  br label %bb9

bb9:
  %p9 = phi ptr [ %p8.next, %bb9.l ], [ %a9, %bb9.r ]
  %p9.next = getelementptr i8, ptr %p9, i64 1
  br i1 %c, label %bb10.l, label %bb10.r

bb10.l:
  br label %bb10

bb10.r:
  br label %bb10

bb10:
  %p10 = phi ptr [ %p9.next, %bb10.l ], [ %a10, %bb10.r ]
  br label %loop

loop:
  %iv = phi ptr [ %p10, %bb10 ], [ %iv.next, %loop ]
  store i8 0, ptr %iv
  store i8 1, ptr %buf
  %iv.next = getelementptr i8, ptr %iv, i64 1
  br i1 %c, label %loop, label %exit

exit:
  store i8 0, ptr %p10
  ret void
}

; %g is based on %q, so %q is checked as a whole, using the offset of %g.
; CHECK-LABEL: phi_source_v2_based_on
; CHECK: NoAlias: i32* %g, i32* %p
define void @phi_source_v2_based_on(ptr %a, ptr %b, i1 %c, i1 %d) {
entry:
  br i1 %d, label %l1, label %l2

l1:
  br label %lj

l2:
  br label %lj

lj:
  %q = phi ptr [ %a, %l1 ], [ %b, %l2 ]
  br i1 %c, label %j1, label %j2

j1:
  br label %j

j2:
  br label %j

j:
  %p = phi ptr [ %q, %j1 ], [ %q, %j2 ]
  %g = getelementptr i8, ptr %q, i64 16
  store i32 0, ptr %p
  store i32 1, ptr %g
  ret void
}

; The sources of %p are inbounds GEPs at least 16 bytes past %q, so they
; cannot access the start of %a, even though %q may be %a. Only phis are
; looked through, so the offsets are used.
; CHECK-LABEL: phi_source_gep_of_phi
; CHECK: NoAlias: i32* %a, i32* %p
define void @phi_source_gep_of_phi(ptr %b, i1 %c, i1 %d) {
entry:
  %a = alloca [64 x i8]
  br i1 %d, label %l1, label %l2

l1:
  br label %lj

l2:
  br label %lj

lj:
  %q = phi ptr [ %a, %l1 ], [ %b, %l2 ]
  %q.16 = getelementptr inbounds i8, ptr %q, i64 16
  %q.32 = getelementptr inbounds i8, ptr %q, i64 32
  br i1 %c, label %j1, label %j2

j1:
  br label %j

j2:
  br label %j

j:
  %p = phi ptr [ %q.16, %j1 ], [ %q.32, %j2 ]
  store i32 0, ptr %p
  store i32 1, ptr %a
  ret void
}

; %p may be %q or %r, and %r is recursive and may be at any offset of %a. %q
; is reached directly first, then through %r.
; CHECK-LABEL: phi_sources_recursive_second
; CHECK: MayAlias: i32* %g, i32* %p
define void @phi_sources_recursive_second(ptr %a, i1 %c, i1 %d) {
entry:
  br i1 %d, label %l1, label %l2

l1:
  br label %lj

l2:
  br label %lj

lj:
  %q = phi ptr [ %a, %l1 ], [ %a, %l2 ]
  br i1 %c, label %loop, label %j2

loop:
  %r = phi ptr [ %q, %lj ], [ %r.next, %loop ]
  %r.next = getelementptr i8, ptr %r, i64 4
  br i1 %d, label %loop, label %j1

j1:
  br label %j

j2:
  br label %j

j:
  %p = phi ptr [ %r, %j1 ], [ %q, %j2 ]
  %g = getelementptr i8, ptr %a, i64 16
  store i32 0, ptr %p
  store i32 1, ptr %g
  ret void
}

; %p may be %q or %r, and %r is recursive and may be at any offset of %a. %q
; is reached through %r first, then directly.
; CHECK-LABEL: phi_sources_recursive_first
; CHECK: MayAlias: i32* %g, i32* %p
define void @phi_sources_recursive_first(ptr %a, i1 %c, i1 %d) {
entry:
  br i1 %d, label %l1, label %l2

l1:
  br label %lj

l2:
  br label %lj

lj:
  %q = phi ptr [ %a, %l1 ], [ %a, %l2 ]
  br i1 %c, label %loop, label %j2

loop:
  %r = phi ptr [ %q, %lj ], [ %r.next, %loop ]
  %r.next = getelementptr i8, ptr %r, i64 4
  br i1 %d, label %loop, label %j1

j1:
  br label %j

j2:
  br label %j

j:
  %p = phi ptr [ %q, %j2 ], [ %r, %j1 ]
  %g = getelementptr i8, ptr %a, i64 16
  store i32 0, ptr %p
  store i32 1, ptr %g
  ret void
}

; %q is %pn or %pn + 4, so %pn may be at any offset of %a, even though neither
; phi is based on itself.
; CHECK-LABEL: phi_cycle_through_phi
; CHECK: MayAlias: i32* %pn, i32* %v2
define void @phi_cycle_through_phi(i1 %c, i1 %d) {
entry:
  %a = alloca [64 x i8]
  %v2 = getelementptr inbounds i8, ptr %a, i64 4
  br label %loop

loop:
  %pn = phi ptr [ %a, %entry ], [ %q, %latch ]
  br i1 %c, label %x, label %y

x:
  br label %latch

y:
  %g = getelementptr inbounds i8, ptr %pn, i64 4
  br label %latch

latch:
  %q = phi ptr [ %pn, %x ], [ %g, %y ]
  load i32, ptr %pn
  load i32, ptr %v2
  br i1 %d, label %loop, label %exit

exit:
  ret void
}
