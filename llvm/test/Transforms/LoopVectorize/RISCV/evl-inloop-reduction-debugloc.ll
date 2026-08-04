; RUN: opt -p loop-vectorize -mtriple=riscv64 -mattr=+v \
; RUN:   -prefer-inloop-reductions -force-tail-folding-style=data-with-evl -S %s \
; RUN:   | FileCheck %s

; The EVL in-loop reduction must keep the debug location of the reduction it
; replaces.

define dso_local signext i32 @sum(ptr nofree noundef readonly captures(none) %a, i32 noundef signext %n) !dbg !18 {
; CHECK-LABEL: define dso_local signext i32 @sum(
; CHECK:       vector.body:
; CHECK:         call i32 @llvm.vp.reduce.add.nxv4i32({{.*}}), !dbg [[DBG:![0-9]+]]
;

entry:
    #dbg_value(ptr %a, !24, !DIExpression(), !29)
    #dbg_value(i32 %n, !25, !DIExpression(), !29)
    #dbg_value(i32 0, !26, !DIExpression(), !29)
    #dbg_value(i32 0, !27, !DIExpression(), !30)
  %cmp4 = icmp sgt i32 %n, 0, !dbg !31
  br i1 %cmp4, label %for.body.preheader, label %for.cond.cleanup, !dbg !33

for.body.preheader:                               ; preds = %entry
  %wide.trip.count = zext nneg i32 %n to i64, !dbg !34
  br label %for.body, !dbg !35

for.cond.cleanup:                                 ; preds = %for.body, %entry
  %s.0.lcssa = phi i32 [ 0, %entry ], [ %add, %for.body ], !dbg !29
  ret i32 %s.0.lcssa, !dbg !36

for.body:                                         ; preds = %for.body.preheader, %for.body
  %indvars.iv = phi i64 [ 0, %for.body.preheader ], [ %indvars.iv.next, %for.body ]
  %s.05 = phi i32 [ 0, %for.body.preheader ], [ %add, %for.body ]
    #dbg_value(i64 %indvars.iv, !27, !DIExpression(), !30)
    #dbg_value(i32 %s.05, !26, !DIExpression(), !29)
  %arrayidx = getelementptr inbounds nuw [4 x i8], ptr %a, i64 %indvars.iv, !dbg !37
  %0 = load i32, ptr %arrayidx, align 4, !dbg !37, !tbaa !38
  %add = add nsw i32 %0, %s.05, !dbg !39
    #dbg_value(i32 %add, !26, !DIExpression(), !29)
  %indvars.iv.next = add nuw nsw i64 %indvars.iv, 1, !dbg !40
    #dbg_value(i64 %indvars.iv.next, !27, !DIExpression(), !30)
  %exitcond.not = icmp eq i64 %indvars.iv.next, %wide.trip.count, !dbg !34
  br i1 %exitcond.not, label %for.cond.cleanup, label %for.body, !dbg !41, !llvm.loop !42
}

!llvm.dbg.cu = !{!0}
!llvm.module.flags = !{!2, !3, !4, !5, !7, !8, !9, !10, !11}
!llvm.ident = !{!12}
!llvm.errno.tbaa = !{!13}
!0 = distinct !DICompileUnit(language: DW_LANG_C11, file: !1, producer: "clang version 24.0.0git (git@github.com:apple/llvm-project.git 682e8b30467b2a731532ead04fe9495cf038bb9f)", isOptimized: true, runtimeVersion: 0, emissionKind: FullDebug, splitDebugInlining: false, nameTableKind: None)
!1 = !DIFile(filename: "rdx.c", directory: "/tmp/review", checksumkind: CSK_MD5, checksum: "474280b0e6605c57fc6804dc9a875096")
!2 = !{i32 7, !"Dwarf Version", i32 5}
!3 = !{i32 2, !"Debug Info Version", i32 3}
!4 = !{i32 1, !"target-abi", !"lp64d"}
!5 = !{i32 6, !"riscv-isa", !6}
!6 = !{!"rv64i2p1_m2p0_a2p1_f2p2_d2p2_c2p0_v1p0_zicsr2p0_zifencei2p0_zmmul1p0_zaamo1p0_zalrsc1p0_zca1p0_zcd1p0_zve32f1p0_zve32x1p0_zve64d1p0_zve64f1p0_zve64x1p0_zvl128b1p0_zvl32b1p0_zvl64b1p0"}
!7 = !{i32 8, !"PIC Level", i32 2}
!8 = !{i32 7, !"PIE Level", i32 2}
!9 = !{i32 7, !"uwtable", i32 2}
!10 = !{i32 8, !"SmallDataLimit", i32 0}
!11 = !{i32 7, !"debug-info-assignment-tracking", i1 true}
!12 = !{!"clang version 24.0.0git (git@github.com:apple/llvm-project.git 682e8b30467b2a731532ead04fe9495cf038bb9f)"}
!13 = !{!14, !15, i64 0}
!14 = !{!"__libc_errno", !15, i64 0}
!15 = !{!"int", !16, i64 0}
!16 = !{!"omnipotent char", !17, i64 0}
!17 = !{!"Simple C/C++ TBAA"}
!18 = distinct !DISubprogram(name: "sum", scope: !1, file: !1, line: 1, type: !19, scopeLine: 1, flags: DIFlagPrototyped | DIFlagAllCallsDescribed, spFlags: DISPFlagDefinition | DISPFlagOptimized, unit: !0, retainedNodes: !23, keyInstructions: true)
!19 = !DISubroutineType(types: !20)
!20 = !{!21, !22, !21}
!21 = !DIBasicType(name: "int", size: 32, encoding: DW_ATE_signed)
!22 = !DIDerivedType(tag: DW_TAG_pointer_type, baseType: !21, size: 64)
!23 = !{!24, !25, !26, !27}
!24 = !DILocalVariable(name: "a", arg: 1, scope: !18, file: !1, line: 1, type: !22)
!25 = !DILocalVariable(name: "n", arg: 2, scope: !18, file: !1, line: 1, type: !21)
!26 = !DILocalVariable(name: "s", scope: !18, file: !1, line: 2, type: !21)
!27 = !DILocalVariable(name: "i", scope: !28, file: !1, line: 3, type: !21)
!28 = distinct !DILexicalBlock(scope: !18, file: !1, line: 3, column: 3)
!29 = !DILocation(line: 0, scope: !18)
!30 = !DILocation(line: 0, scope: !28)
!31 = !DILocation(line: 3, column: 21, scope: !32, atomGroup: 10, atomRank: 1)
!32 = distinct !DILexicalBlock(scope: !28, file: !1, line: 3, column: 3)
!33 = !DILocation(line: 3, column: 3, scope: !28, atomGroup: 11, atomRank: 1)
!34 = !DILocation(line: 3, column: 21, scope: !32, atomGroup: 3, atomRank: 1)
!35 = !DILocation(line: 3, column: 3, scope: !28)
!36 = !DILocation(line: 5, column: 3, scope: !18, atomGroup: 9, atomRank: 1)
!37 = !DILocation(line: 4, column: 10, scope: !32)
!38 = !{!15, !15, i64 0}
!39 = !DILocation(line: 4, column: 7, scope: !32, atomGroup: 5, atomRank: 2)
!40 = !DILocation(line: 3, column: 27, scope: !32, atomGroup: 6, atomRank: 2)
!41 = !DILocation(line: 3, column: 3, scope: !28, atomGroup: 4, atomRank: 1)
!42 = distinct !{!42, !35, !43, !44, !45}
!43 = !DILocation(line: 4, column: 13, scope: !28)
!44 = !{!"llvm.loop.mustprogress"}
!45 = !{!"llvm.loop.unroll.disable"}
