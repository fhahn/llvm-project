; RUN: opt -passes=loop-vectorize -prefer-inloop-reductions -mtriple=riscv64 \
; RUN:   -mattr=+v -S %s | FileCheck %s

; Test that an EVL in-loop reduction keeps the debug location of the scalar
; reduction.

define i32 @add_reduction(ptr %src, i64 %n) !dbg !3 {
; CHECK-LABEL: define i32 @add_reduction(
; CHECK:       vector.body:
; CHECK:         call i32 @llvm.vp.reduce.add.nxv4i32({{.*}}), !dbg [[DBG:![0-9]+]]
; CHECK:       [[DBG]] = !DILocation(line: 2,
;
entry:
  br label %loop

loop:
  %iv = phi i64 [ 0, %entry ], [ %iv.next, %loop ]
  %red = phi i32 [ 0, %entry ], [ %red.next, %loop ]
  %gep.src = getelementptr inbounds i32, ptr %src, i64 %iv
  %l = load i32, ptr %gep.src, align 4, !dbg !4
  %red.next = add i32 %l, %red, !dbg !5
  %iv.next = add nuw nsw i64 %iv, 1
  %ec = icmp eq i64 %iv.next, %n
  br i1 %ec, label %exit, label %loop

exit:
  ret i32 %red.next
}

!llvm.dbg.cu = !{!0}
!llvm.module.flags = !{!2}

!0 = distinct !DICompileUnit(language: DW_LANG_C11, file: !1, emissionKind: FullDebug)
!1 = !DIFile(filename: "t.c", directory: "/")
!2 = !{i32 2, !"Debug Info Version", i32 3}
!3 = distinct !DISubprogram(name: "add_reduction", scope: !1, file: !1, line: 1, type: !6, spFlags: DISPFlagDefinition, unit: !0)
!4 = !DILocation(line: 1, scope: !3)
!5 = !DILocation(line: 2, scope: !3)
!6 = !DISubroutineType(types: !{})
