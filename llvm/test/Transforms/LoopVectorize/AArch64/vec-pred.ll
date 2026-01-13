target datalayout = "e-m:o-p270:32:32-p271:32:32-p272:64:64-i64:64-i128:128-n32:64-S128-Fn32"
target triple = "arm64-apple-macosx13.0.0"

define [2 x i64] @_ZN4llvm16BasicTTIImplBaseINS_12BasicTTIImplEE22getArithmeticInstrCostEjPNS_4TypeENS_19TargetTransformInfo14TargetCostKindENS5_16OperandValueInfoES7_NS_8ArrayRefIPKNS_5ValueEEEPKNS_11InstructionE([2 x i64] %Args.coerce) {
entry:
  %call2 = load volatile i32, ptr null, align 4
  %Args.coerce.fca.1.extract = extractvalue [2 x i64] %Args.coerce, 1
  %cmp.i.i.i270 = icmp ugt i64 %Args.coerce.fca.1.extract, 1
  br i1 %cmp.i.i.i270, label %common.ret, label %for.body.i.i11.i.i.i

common.ret:                                       ; preds = %for.body.i.i11.i.i.i, %entry
  ret [2 x i64] zeroinitializer

for.body.i.i11.i.i.i:                             ; preds = %for.body.i.i11.i.i.i, %entry
  %__idx.06.i.i.i.i.i = phi ptr [ %incdec.ptr.i.i13.i.i.i, %for.body.i.i11.i.i.i ], [ null, %entry ]
  %__n.addr.05.i.i12.i.i.i = phi i64 [ %dec.i.i14.i.i.i, %for.body.i.i11.i.i.i ], [ %Args.coerce.fca.1.extract, %entry ]
  store ptr null, ptr %__idx.06.i.i.i.i.i, align 8
  %incdec.ptr.i.i13.i.i.i = getelementptr i8, ptr %__idx.06.i.i.i.i.i, i64 8
  %dec.i.i14.i.i.i = add i64 %__n.addr.05.i.i12.i.i.i, -1
  %cmp.not.i.i15.i.i.i = icmp eq i64 %__n.addr.05.i.i12.i.i.i, 0
  br i1 %cmp.not.i.i15.i.i.i, label %common.ret, label %for.body.i.i11.i.i.i
}
