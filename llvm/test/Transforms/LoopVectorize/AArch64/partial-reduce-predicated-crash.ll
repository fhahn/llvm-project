; XFAIL: *
; RUN: opt -passes=loop-vectorize -mattr=+dotprod -force-vector-width=16 -S %s | FileCheck %s

; Regression test for a loop-vectorizer crash in partial-reduction chain
; construction. For a *predicated* (conditional) partial-reduction chain
; `acc.next = select(%cond, add(acc, ext*ext), acc)`, the exit value of the
; chain is found via
;   cast_or_null<VPInstruction>(findUserOf(WidenRecipe,
;       m_Select(m_VPValue(Cond), m_Specific(WidenRecipe), m_Specific(RdxPhi))))
; in VPlanTransforms::transformToPartialReduction. The matched select recipe is
; a VPWidenRecipe (a widened select), not a VPInstruction, so the
; cast_or_null<VPInstruction> asserts:
;   "cast_if_present<Ty>() argument of incompatible type!"
;
; FIXME: cast_or_null<VPInstruction> is too narrow here - the select user is a
; VPWidenRecipe. The cast should use dyn_cast_or_null<VPSingleDefRecipe>, like
; the sibling findUserOf calls.

target triple = "aarch64-unknown-linux-gnu"

define i32 @predicated_dotprod(ptr %a, ptr %b, i32 %n) {
; CHECK-LABEL: define i32 @predicated_dotprod(
entry:
  br label %loop
loop:
  %iv = phi i32 [ 0, %entry ], [ %iv.next, %loop ]
  %acc = phi i32 [ 0, %entry ], [ %acc.next, %loop ]
  %ga = getelementptr inbounds i8, ptr %a, i32 %iv
  %gb = getelementptr inbounds i8, ptr %b, i32 %iv
  %av = load i8, ptr %ga
  %bv = load i8, ptr %gb
  %ax = sext i8 %av to i32
  %bx = sext i8 %bv to i32
  %mul = mul i32 %ax, %bx
  %add = add i32 %acc, %mul
  %pos = icmp sgt i8 %av, 0
  %acc.next = select i1 %pos, i32 %add, i32 %acc
  %iv.next = add i32 %iv, 1
  %ec = icmp eq i32 %iv.next, %n
  br i1 %ec, label %exit, label %loop
exit:
  ret i32 %acc.next
}
