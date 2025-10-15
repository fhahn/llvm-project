" Vim color scheme for VPlan (Dark)
" Maintainer: LLVM Project
" Based on the VSCode VPlan Dark theme

if &background == "dark"
  " VPlan-specific highlighting for dark backgrounds

  " Block names (bold teal/cyan)
  hi vplanBlockLabel ctermfg=80 guifg=#4EC9B0 cterm=bold gui=bold
  hi vplanBlockName ctermfg=80 guifg=#4EC9B0 cterm=bold gui=bold
  hi vplanVectorBlock ctermfg=80 guifg=#4EC9B0 cterm=bold gui=bold

  " IR block names (bold yellow)
  hi vplanIRBlockName ctermfg=186 guifg=#DCDCAA cterm=bold gui=bold

  " Predicated blocks (bold blue)
  hi vplanPredBlock ctermfg=81 guifg=#4FC1FF cterm=bold gui=bold

  " VPlan instructions (bold purple/magenta)
  hi vplanInstruction ctermfg=176 guifg=#C586C0 cterm=bold gui=bold

  " IR instructions (yellow)
  hi vplanIRInstruction ctermfg=186 guifg=#DCDCAA cterm=none gui=none

  " VPlan values (light blue)
  hi vplanValue ctermfg=117 guifg=#9CDCFE cterm=none gui=none

  " IR values (orange)
  hi vplanIRValue ctermfg=174 guifg=#CE9178 cterm=none gui=none

  " VPlan header (blue, bold)
  hi vplanHeader ctermfg=75 guifg=#569CD6 cterm=bold gui=bold

  " Control flow keywords (purple)
  hi vplanSuccessor ctermfg=176 guifg=#C586C0 cterm=none gui=none

  " Modifiers and storage class (blue)
  hi vplanModifier ctermfg=75 guifg=#569CD6 cterm=none gui=none
  hi vplanIRBlockKeyword ctermfg=75 guifg=#569CD6 cterm=none gui=none

  " Comments (green, italic)
  hi vplanComment ctermfg=65 guifg=#6A9955 cterm=italic gui=italic

  " Numbers (light green)
  hi vplanNumber ctermfg=150 guifg=#B5CEA8 cterm=none gui=none
  hi vplanFloat ctermfg=150 guifg=#B5CEA8 cterm=none gui=none
  hi vplanMultiplicity ctermfg=150 guifg=#B5CEA8 cterm=none gui=none

  " Predicates (teal)
  hi vplanPredicate ctermfg=80 guifg=#4EC9B0 cterm=none gui=none

  " Operators
  hi vplanOperator ctermfg=white guifg=#D4D4D4 cterm=none gui=none

  " Global variables
  hi vplanGlobal ctermfg=117 guifg=#9CDCFE cterm=none gui=none

  " Debug info
  hi vplanDebug ctermfg=176 guifg=#C586C0 cterm=none gui=none

  " Keywords
  hi vplanLiveIn ctermfg=176 guifg=#C586C0 cterm=none gui=none
  hi vplanInterleavePart ctermfg=176 guifg=#C586C0 cterm=none gui=none

else
  " VPlan-specific highlighting for light backgrounds

  " Block names (bold dark teal)
  hi vplanBlockLabel ctermfg=31 guifg=#267F99 cterm=bold gui=bold
  hi vplanBlockName ctermfg=31 guifg=#267F99 cterm=bold gui=bold
  hi vplanVectorBlock ctermfg=31 guifg=#267F99 cterm=bold gui=bold

  " IR block names (bold brown)
  hi vplanIRBlockName ctermfg=94 guifg=#795E26 cterm=bold gui=bold

  " Predicated blocks (bold blue)
  hi vplanPredBlock ctermfg=32 guifg=#0070C1 cterm=bold gui=bold

  " VPlan instructions (bold magenta)
  hi vplanInstruction ctermfg=128 guifg=#AF00DB cterm=bold gui=bold

  " IR instructions (brown)
  hi vplanIRInstruction ctermfg=94 guifg=#795E26 cterm=none gui=none

  " VPlan values (dark blue)
  hi vplanValue ctermfg=18 guifg=#001080 cterm=none gui=none

  " IR values (dark red)
  hi vplanIRValue ctermfg=124 guifg=#A31515 cterm=none gui=none

  " VPlan header (blue, bold)
  hi vplanHeader ctermfg=21 guifg=#0000FF cterm=bold gui=bold

  " Control flow keywords (magenta)
  hi vplanSuccessor ctermfg=128 guifg=#AF00DB cterm=none gui=none

  " Modifiers and storage class (blue)
  hi vplanModifier ctermfg=21 guifg=#0000FF cterm=none gui=none
  hi vplanIRBlockKeyword ctermfg=21 guifg=#0000FF cterm=none gui=none

  " Comments (green, italic)
  hi vplanComment ctermfg=28 guifg=#008000 cterm=italic gui=italic

  " Numbers (dark green)
  hi vplanNumber ctermfg=29 guifg=#098658 cterm=none gui=none
  hi vplanFloat ctermfg=29 guifg=#098658 cterm=none gui=none
  hi vplanMultiplicity ctermfg=29 guifg=#098658 cterm=none gui=none

  " Predicates (dark teal)
  hi vplanPredicate ctermfg=31 guifg=#267F99 cterm=none gui=none

  " Operators
  hi vplanOperator ctermfg=black guifg=#000000 cterm=none gui=none

  " Global variables
  hi vplanGlobal ctermfg=18 guifg=#001080 cterm=none gui=none

  " Debug info
  hi vplanDebug ctermfg=128 guifg=#AF00DB cterm=none gui=none

  " Keywords
  hi vplanLiveIn ctermfg=128 guifg=#AF00DB cterm=none gui=none
  hi vplanInterleavePart ctermfg=128 guifg=#AF00DB cterm=none gui=none
endif
