" Vim syntax file
" Language: VPlan
" Maintainer: LLVM Project
" Latest Revision: 2025

if exists("b:current_syntax")
  finish
endif

" VPlan Header
syn match vplanHeader /^VPlan\s\+'[^']\+'/
syn match vplanLiveIn /^Live-in\s\+vp<[^>]\+>\s*=.*$/
syn match vplanLiveIn /^vp<[^>]\+>\s*=.*$/

" Basic Blocks
" IR basic blocks: ir-bb<name>:
syn match vplanIRBlock /^\s*ir-bb<\([^>]\+\)>:/ contains=vplanIRBlockKeyword,vplanIRBlockName
syn match vplanIRBlockKeyword /ir-bb/ contained
syn match vplanIRBlockName /<[^>]\+>/ contained

" VPlan blocks with multiplicity: <x1> vector loop:
syn match vplanVectorBlock /<x[0-9]\+>\s\+\([a-z][a-z.0-9_ -]\+\):/ contains=vplanMultiplicity,vplanBlockName
syn match vplanMultiplicity /<x[0-9]\+>/ contained

" Standard block names: vector.body:, middle.block:, scalar.ph:
syn match vplanBlockLabel /^\s*\([a-z][a-z.0-9_-]\+\):/
syn match vplanBlockName /\([a-z][a-z.0-9_ -]\+\):/ contained

" Predicated blocks: pred.udiv.entry:, pred.sdiv.if:
syn match vplanPredBlock /^\s*\(pred\.[a-z]\+\.[a-z]\+\):/

" VPlan Instructions (uppercase keywords)
syn keyword vplanInstruction EMIT WIDEN CLONE REPLICATE BLEND
syn keyword vplanInstruction WIDEN-INDUCTION CANONICAL-INDUCTION SCALAR-STEPS
syn keyword vplanInstruction WIDEN-INTRINSIC WIDEN-GEP WIDEN-SELECT WIDEN-CAST
syn keyword vplanInstruction BRANCH-ON-MASK BRANCH-ON-COND
syn keyword vplanInstruction PHI-PREDICATED-INSTRUCTION FIRST-ORDER-RECURRENCE-PHI
syn keyword vplanInstruction INTERLEAVE-GROUP DERIVED-IV EMIT-SCALAR
syn match vplanInstruction /\<EXPAND SCEV\>/
syn match vplanInstruction /\<extract-last-element\>/
syn match vplanInstruction /\<extract-penultimate-element\>/
syn match vplanInstruction /\<first-order splice\>/
syn match vplanInstruction /\<vector-pointer\>/
syn match vplanInstruction /\<logical-and\>/
syn match vplanInstruction /\<branch-on-count\>/

" IR Instructions (lowercase operations)
syn keyword vplanIRInstruction load store add sub mul div udiv sdiv
syn keyword vplanIRInstruction fadd fsub fmul fdiv frem
syn keyword vplanIRInstruction getelementptr phi icmp fcmp select call
syn keyword vplanIRInstruction zext sext trunc or and xor
syn keyword vplanIRInstruction shl lshr ashr not

" Comparison predicates
syn keyword vplanPredicate eq ne slt sle sgt sge ult ule ugt uge
syn keyword vplanPredicate oeq one olt ole ogt oge ord uno ueq une

" Instruction flags and modifiers
syn keyword vplanModifier inbounds nuw nsw exact
syn keyword vplanModifier nnan ninf nsz arcp contract afn reassoc fast
syn keyword vplanModifier disjoint nneg

" VPlan values: vp<[[VALUE:%.+]]>
syn match vplanValue /vp<\[\{0,2}[^\]>]\+\]\{0,2}>/

" IR values: ir<%value>, ir<123>
syn match vplanIRValue /ir<[^>]\+>/
syn match vplanIRValue /ir%[a-zA-Z0-9_.]\+/

" Control Flow
syn match vplanSuccessor /^\s*Successor(s):/
syn match vplanSuccessor /^\s*No successors/

" INTERLEAVE-GROUP details
syn match vplanInterleavePart /\<from index\>/
syn match vplanInterleavePart /\<to index\>/
syn match vplanInterleavePart /\<with factor\>/
syn match vplanInterleavePart /\<at\>/

" Numbers
syn match vplanNumber /\<\d\+\>/
syn match vplanFloat /\<\d\+\.\d\+\(e[+-]\?\d\+\)\?>/

" Global variables: @name
syn match vplanGlobal /@[a-zA-Z0-9_.]\+/

" Debug locations
syn match vplanDebug /!dbg\s\+[^,\s]\+/

" Comments
syn match vplanComment /;.*$/
syn match vplanComment /(S->V)/
syn match vplanComment /(truncated to[^)]\+)/
syn match vplanComment /(extra operand:[^)]\+)/

" Operators
syn match vplanOperator /=/
syn match vplanOperator /+/
syn match vplanOperator /\*/
syn match vplanOperator /\//

" Define highlighting
hi def link vplanHeader Title
hi def link vplanLiveIn Keyword

hi def link vplanIRBlockKeyword StorageClass
hi def link vplanIRBlockName Function
hi def link vplanBlockLabel Function
hi def link vplanBlockName Function
hi def link vplanPredBlock Type
hi def link vplanMultiplicity Number
hi def link vplanVectorBlock Function

hi def link vplanInstruction Statement
hi def link vplanIRInstruction Identifier
hi def link vplanPredicate Constant
hi def link vplanModifier StorageClass

hi def link vplanValue Special
hi def link vplanIRValue String

hi def link vplanSuccessor Keyword
hi def link vplanInterleavePart Keyword

hi def link vplanNumber Number
hi def link vplanFloat Float
hi def link vplanGlobal Identifier

hi def link vplanDebug PreProc
hi def link vplanComment Comment
hi def link vplanOperator Operator

let b:current_syntax = "vplan"
