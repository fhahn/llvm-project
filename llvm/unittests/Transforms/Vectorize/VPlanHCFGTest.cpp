//===- llvm/unittest/Transforms/Vectorize/VPlanHCFGTest.cpp ---------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "../lib/Transforms/Vectorize/VPlan.h"
#include "../lib/Transforms/Vectorize/VPlanTransforms.h"
#include "VPlanTestBase.h"
#include "llvm/Analysis/TargetLibraryInfo.h"
#include "llvm/TargetParser/Triple.h"
#include "gtest/gtest.h"
#include <string>

namespace llvm {
namespace {

class VPlanHCFGTest : public VPlanTestIRBase {};

TEST_F(VPlanHCFGTest, KeepHeaderAnyOfBranch) {
  const char *ModuleString = R"(
define void @f(ptr %a, ptr %b, i64 %n) {
entry:
  br label %header
header:
  %iv = phi i64 [ 0, %entry ], [ %iv.next, %latch ]
  %index = getelementptr i32, ptr %a, i64 %iv
  %value = load i32, ptr %index
  %skip = icmp eq i32 %value, 0
  br i1 %skip, label %latch, label %body
body:
  %x = load i32, ptr %b
  store i32 %x, ptr %index
  %cond = icmp sgt i32 %x, 0
  br i1 %cond, label %then, label %merge
then:
  %y = load i32, ptr %index
  store i32 %y, ptr %b
  br label %merge
merge:
  %result = phi i32 [ %x, %body ], [ %y, %then ]
  store i32 %result, ptr %index
  br label %latch
latch:
  store i32 0, ptr %b
  %iv.next = add i64 %iv, 1
  %done = icmp eq i64 %iv.next, %n
  br i1 %done, label %exit, label %header
exit:
  ret void
}
)";
  Module &M = parseModule(ModuleString);
  BasicBlock *LoopHeader =
      M.getFunction("f")->getEntryBlock().getSingleSuccessor();

  // Check both successor orders.
  for (bool SwapSuccessors : {false, true}) {
    SCOPED_TRACE(SwapSuccessors);
    auto Plan = buildVPlan(LoopHeader);
    VPBasicBlock *Header = Plan->getVectorLoopRegion()->getEntryBasicBlock();
    auto *Latch = cast<VPBasicBlock>(Header->getSuccessors()[0]);
    auto *Body = cast<VPBasicBlock>(Header->getSuccessors()[1]);
    auto *Then = cast<VPBasicBlock>(Body->getSuccessors()[0]);
    auto *Merge = cast<VPBasicBlock>(Body->getSuccessors()[1]);
    VPValue *InnerCond = Body->getTerminator()->getOperand(0);

    // Test AnyOf predication independently of the bailout transform.
    auto *HeaderBranch = cast<VPInstruction>(Header->getTerminator());
    auto *AnyOf =
        new VPInstruction(VPInstruction::AnyOf, {HeaderBranch->getOperand(0)});
    AnyOf->insertBefore(HeaderBranch);
    HeaderBranch->setOperand(0, AnyOf);
    if (SwapSuccessors)
      Header->swapSuccessors();

    VPlanTransforms::introduceMasksAndLinearize(*Plan);

    // Preserve the branch and successor order, without masking accesses.
    ASSERT_EQ(HeaderBranch, Header->getTerminator());
    EXPECT_EQ(AnyOf, HeaderBranch->getOperand(0));
    ASSERT_EQ(2u, Header->getNumSuccessors());
    EXPECT_EQ(SwapSuccessors ? Body : Latch, Header->getSuccessors()[0]);
    EXPECT_EQ(SwapSuccessors ? Latch : Body, Header->getSuccessors()[1]);
    for (VPBasicBlock *VPBB : {Header, Body, Merge, Latch})
      for (VPInstruction &I : make_isa_range<VPInstruction>(*VPBB))
        EXPECT_FALSE(I.isMasked());

    // If-convert the inner branch using only its condition.
    EXPECT_EQ(Merge, Then->getSingleSuccessor());
    EXPECT_EQ(Then, Body->getSingleSuccessor());
    EXPECT_EQ(Latch, Merge->getSingleSuccessor());
    EXPECT_EQ(nullptr, Body->getTerminator());
    EXPECT_EQ(InnerCond, cast<VPInstruction>(&Then->front())->getMask());
    EXPECT_EQ(InnerCond, cast<VPInstruction>(&Then->back())->getMask());
    EXPECT_TRUE(Merge->phis().empty());
    VPValue *MergedValue = cast<VPInstruction>(&Merge->back())->getOperand(0);
    EXPECT_TRUE(isa<VPBlendRecipe>(MergedValue->getDefiningRecipe()));
  }
}

TEST_F(VPlanHCFGTest, testBuildHCFGInnerLoop) {
  const char *ModuleString =
      "define void @f(ptr %A, i64 %N) {\n"
      "entry:\n"
      "  br label %for.body\n"
      "for.body:\n"
      "  %indvars.iv = phi i64 [ 0, %entry ], [ %indvars.iv.next, %for.body ]\n"
      "  %arr.idx = getelementptr inbounds i32, ptr %A, i64 %indvars.iv\n"
      "  %l1 = load i32, ptr %arr.idx, align 4\n"
      "  %res = add i32 %l1, 10\n"
      "  store i32 %res, ptr %arr.idx, align 4\n"
      "  %indvars.iv.next = add i64 %indvars.iv, 1\n"
      "  %exitcond = icmp ne i64 %indvars.iv.next, %N\n"
      "  br i1 %exitcond, label %for.body, label %for.end\n"
      "for.end:\n"
      "  ret void\n"
      "}\n";

  Module &M = parseModule(ModuleString);

  Function *F = M.getFunction("f");
  BasicBlock *LoopHeader = F->getEntryBlock().getSingleSuccessor();
  auto Plan = buildVPlan(LoopHeader);

  VPBasicBlock *Entry = Plan->getEntry()->getEntryBasicBlock();
  EXPECT_EQ(0u, Entry->getNumPredecessors());
  EXPECT_EQ(2u, Entry->getNumSuccessors());

  // Check that the region following the preheader consists of a block for the
  // original header and a separate latch.
  VPBasicBlock *VecBB = Plan->getVectorLoopRegion()->getEntryBasicBlock();
  EXPECT_EQ(10u, VecBB->size());
  EXPECT_EQ(0u, VecBB->getNumPredecessors());
  EXPECT_EQ(0u, VecBB->getNumSuccessors());
  EXPECT_EQ(VecBB->getParent()->getEntryBasicBlock(), VecBB);
  EXPECT_EQ(&*Plan, VecBB->getPlan());

  auto Iter = VecBB->begin();
  // The first recipe is now a scalar PHI for the canonical IV
  VPRecipeBase *ScalarIVPhi = &*Iter++;
  VPValue *IVValue = ScalarIVPhi->getVPSingleValue();
  EXPECT_NE(nullptr, IVValue);

  VPInstruction *Idx = dyn_cast<VPInstruction>(&*Iter++);
  EXPECT_EQ(Instruction::GetElementPtr, Idx->getOpcode());
  EXPECT_EQ(2u, Idx->getNumOperands());
  EXPECT_EQ(IVValue, Idx->getOperand(1));

  VPInstruction *Load = dyn_cast<VPInstruction>(&*Iter++);
  EXPECT_EQ(Instruction::Load, Load->getOpcode());
  EXPECT_EQ(1u, Load->getNumOperands());
  EXPECT_EQ(Idx, Load->getOperand(0));

  VPInstruction *Add = dyn_cast<VPInstruction>(&*Iter++);
  EXPECT_EQ(Instruction::Add, Add->getOpcode());
  EXPECT_EQ(2u, Add->getNumOperands());
  EXPECT_EQ(Load, Add->getOperand(0));

  VPInstruction *Store = dyn_cast<VPInstruction>(&*Iter++);
  EXPECT_EQ(Instruction::Store, Store->getOpcode());
  EXPECT_EQ(2u, Store->getNumOperands());
  EXPECT_EQ(Add, Store->getOperand(0));
  EXPECT_EQ(Idx, Store->getOperand(1));

  VPInstruction *IndvarAdd = dyn_cast<VPInstruction>(&*Iter++);
  EXPECT_EQ(Instruction::Add, IndvarAdd->getOpcode());
  EXPECT_EQ(2u, IndvarAdd->getNumOperands());
  EXPECT_EQ(IVValue, IndvarAdd->getOperand(0));

  VPInstruction *ICmp = dyn_cast<VPInstruction>(&*Iter++);
  EXPECT_EQ(Instruction::ICmp, ICmp->getOpcode());
  EXPECT_EQ(2u, ICmp->getNumOperands());
  EXPECT_EQ(IndvarAdd, ICmp->getOperand(0));

#if !defined(NDEBUG) || defined(LLVM_ENABLE_DUMP)
  // Add an external value to check we do not print the list of external values,
  // as this is not required with the new printing.
  Plan->getOrAddLiveIn(&*F->arg_begin());
  std::string FullDump;
  raw_string_ostream OS(FullDump);
  Plan->printDOT(OS);
  const char *ExpectedStr = R"(digraph VPlan {
graph [labelloc=t, fontsize=30; label="Vectorization Plan\n for UF\>=1\nLive-in vp\<%0\> = VF * UF\nLive-in vp\<%1\> = vector-trip-count\nLive-in ir\<%N\> = original trip-count\n"]
node [shape=rect, fontname=Courier, fontsize=30]
edge [fontname=Courier, fontsize=30]
compound=true
  N0 [label =
    "ir-bb\<entry\>:\l" +
    "Successor(s): scalar.ph, vector.ph\l"
  ]
  N0 -> N1 [ label="T"]
  N0 -> N2 [ label="F"]
  N1 [label =
    "scalar.ph:\l" +
    "  EMIT-SCALAR vp\<%8\> = phi [ vp\<%6\>, middle.block ], [ ir\<0\>, ir-bb\<entry\> ]\l" +
    "Successor(s): ir-bb\<for.body\>\l"
  ]
  N1 -> N3 [ label=""]
  N3 [label =
    "ir-bb\<for.body\>:\l" +
    "  IR   %indvars.iv = phi i64 [ 0, %entry ], [ %indvars.iv.next, %for.body ] (extra operand: vp\<%8\> from scalar.ph)\l" +
    "  IR   %arr.idx = getelementptr inbounds i32, ptr %A, i64 %indvars.iv\l" +
    "  IR   %l1 = load i32, ptr %arr.idx, align 4\l" +
    "  IR   %res = add i32 %l1, 10\l" +
    "  IR   store i32 %res, ptr %arr.idx, align 4\l" +
    "  IR   %indvars.iv.next = add i64 %indvars.iv, 1\l" +
    "  IR   %exitcond = icmp ne i64 %indvars.iv.next, %N\l" +
    "No successors\l"
  ]
  N2 [label =
    "vector.ph:\l" +
    "Successor(s): vector loop\l"
  ]
  N2 -> N4 [ label="" lhead=cluster_N5]
  subgraph cluster_N5 {
    fontname=Courier
    label="\<x1\> vector loop"
    "vp\<%2\> = CANONICAL-IV"
    N4 [label =
      "vector.body:\l" +
      "  EMIT-SCALAR ir\<%indvars.iv\> = phi [ ir\<0\>, vector.ph ], [ ir\<%indvars.iv.next\>, vector.body ]\l" +
      "  EMIT ir\<%arr.idx\> = getelementptr inbounds ir\<%A\>, ir\<%indvars.iv\>\l" +
      "  EMIT-SCALAR ir\<%l1\> = load ir\<%arr.idx\>\l" +
      "  EMIT ir\<%res\> = add ir\<%l1\>, ir\<10\>\l" +
      "  EMIT store ir\<%res\>, ir\<%arr.idx\>\l" +
      "  EMIT ir\<%indvars.iv.next\> = add ir\<%indvars.iv\>, ir\<1\>\l" +
      "  EMIT ir\<%exitcond\> = icmp ne ir\<%indvars.iv.next\>, ir\<%N\>\l" +
      "  EMIT vp\<%3\> = not ir\<%exitcond\>\l" +
      "  EMIT vp\<%index.next\> = add nuw vp\<%2\>, vp\<%0\>\l" +
      "  EMIT branch-on-count vp\<%index.next\>, vp\<%1\>\l" +
      "No successors\l"
    ]
  }
  N4 -> N6 [ label="" ltail=cluster_N5]
  N6 [label =
    "middle.block:\l" +
    "  EMIT vp\<%5\> = extract-last-part ir\<%indvars.iv.next\>\l" +
    "  EMIT vp\<%6\> = extract-last-lane vp\<%5\>\l" +
    "  EMIT vp\<%cmp.n\> = icmp eq ir\<%N\>, vp\<%1\>\l" +
    "  EMIT branch-on-cond vp\<%cmp.n\>\l" +
    "Successor(s): ir-bb\<for.end\>, scalar.ph\l"
  ]
  N6 -> N7 [ label="T"]
  N6 -> N1 [ label="F"]
  N7 [label =
    "ir-bb\<for.end\>:\l" +
    "No successors\l"
  ]
}
)";
  EXPECT_EQ(ExpectedStr, FullDump);
#endif
}

TEST_F(VPlanHCFGTest, testVPInstructionToVPRecipesInner) {
  const char *ModuleString =
      "define void @f(ptr %A, i64 %N) {\n"
      "entry:\n"
      "  br label %for.body\n"
      "for.body:\n"
      "  %indvars.iv = phi i64 [ 0, %entry ], [ %indvars.iv.next, %for.body ]\n"
      "  %arr.idx = getelementptr inbounds i32, ptr %A, i64 %indvars.iv\n"
      "  %l1 = load i32, ptr %arr.idx, align 4\n"
      "  %res = add i32 %l1, 10\n"
      "  store i32 %res, ptr %arr.idx, align 4\n"
      "  %indvars.iv.next = add i64 %indvars.iv, 1\n"
      "  %exitcond = icmp ne i64 %indvars.iv.next, %N\n"
      "  br i1 %exitcond, label %for.body, label %for.end\n"
      "for.end:\n"
      "  ret void\n"
      "}\n";

  Module &M = parseModule(ModuleString);

  Function *F = M.getFunction("f");
  BasicBlock *LoopHeader = F->getEntryBlock().getSingleSuccessor();
  auto Plan = buildVPlan(LoopHeader);

  Loop *L = LI->getLoopFor(LoopHeader);
  PredicatedScalarEvolution PSE(*SE, *L);

  TargetLibraryInfoImpl TLII(M.getTargetTriple());
  TargetLibraryInfo TLI(TLII);
  // Current VPlan construction doesn't add a terminator for top-level loop
  // latches. Add it before running transform.
  cast<VPBasicBlock>(Plan->getVectorLoopRegion()->getExiting())
      ->appendRecipe(new VPInstruction(
          VPInstruction::BranchOnCond,
          {Plan->getOrAddLiveIn(ConstantInt::getTrue(F->getContext()))}));
  VPlanTransforms::tryToConvertVPInstructionsToVPRecipes(*Plan, TLI, PSE, L);

  VPBlockBase *Entry = Plan->getEntry()->getEntryBasicBlock();
  EXPECT_EQ(0u, Entry->getNumPredecessors());
  EXPECT_EQ(2u, Entry->getNumSuccessors());

  // Check that the region following the preheader consists of a block for the
  // original header and a separate latch.
  VPBasicBlock *VecBB = Plan->getVectorLoopRegion()->getEntryBasicBlock();
  EXPECT_EQ(11u, VecBB->size());
  EXPECT_EQ(0u, VecBB->getNumPredecessors());
  EXPECT_EQ(0u, VecBB->getNumSuccessors());
  EXPECT_EQ(VecBB->getParent()->getEntryBasicBlock(), VecBB);

  auto Iter = VecBB->begin();
  EXPECT_NE(nullptr, dyn_cast<VPWidenPHIRecipe>(&*Iter++));
  EXPECT_NE(nullptr, dyn_cast<VPWidenGEPRecipe>(&*Iter++));
  EXPECT_NE(nullptr, dyn_cast<VPWidenMemoryRecipe>(&*Iter++));
  EXPECT_NE(nullptr, dyn_cast<VPWidenRecipe>(&*Iter++));
  EXPECT_NE(nullptr, dyn_cast<VPWidenMemoryRecipe>(&*Iter++));
  EXPECT_NE(nullptr, dyn_cast<VPWidenRecipe>(&*Iter++));
  EXPECT_NE(nullptr, dyn_cast<VPWidenRecipe>(&*Iter++));
  EXPECT_NE(nullptr, dyn_cast<VPInstruction>(&*Iter++));
  EXPECT_NE(nullptr, dyn_cast<VPInstruction>(&*Iter++));
  EXPECT_NE(nullptr, dyn_cast<VPInstruction>(&*Iter++));
  EXPECT_NE(nullptr, dyn_cast<VPInstruction>(&*Iter++));
  EXPECT_EQ(VecBB->end(), Iter);
}

TEST_F(VPlanHCFGTest, testBuildHCFGInnerLoopMultiExit) {
  const char *ModuleString =
      "define void @f(ptr %A, i64 %N) {\n"
      "entry:\n"
      "  br label %loop.header\n"
      "loop.header:\n"
      "  %iv = phi i64 [ 0, %entry ], [ %iv.next, %loop.latch ]\n"
      "  %arr.idx = getelementptr inbounds i32, ptr %A, i64 %iv\n"
      "  %l1 = load i32, ptr %arr.idx, align 4\n"
      "  %c = icmp eq i32 %l1, 0\n"
      "  br i1 %c, label %exit.1, label %loop.latch\n"
      "loop.latch:\n"
      "  %res = add i32 %l1, 10\n"
      "  store i32 %res, ptr %arr.idx, align 4\n"
      "  %iv.next = add i64 %iv, 1\n"
      "  %exitcond = icmp ne i64 %iv.next, %N\n"
      "  br i1 %exitcond, label %loop.header, label %exit.2\n"
      "exit.1:\n"
      "  ret void\n"
      "exit.2:\n"
      "  ret void\n"
      "}\n";

  Module &M = parseModule(ModuleString);

  Function *F = M.getFunction("f");
  BasicBlock *LoopHeader = F->getEntryBlock().getSingleSuccessor();
  auto Plan = buildVPlan(LoopHeader);

#if !defined(NDEBUG) || defined(LLVM_ENABLE_DUMP)
  // Add an external value to check we do not print the list of external values,
  // as this is not required with the new printing.
  Plan->getOrAddLiveIn(&*F->arg_begin());
  std::string FullDump;
  raw_string_ostream OS(FullDump);
  Plan->printDOT(OS);
  const char *ExpectedStr = R"(digraph VPlan {
graph [labelloc=t, fontsize=30; label="Vectorization Plan\n for UF\>=1\nLive-in vp\<%0\> = VF * UF\nLive-in vp\<%1\> = vector-trip-count\nLive-in ir\<%N\> = original trip-count\n"]
node [shape=rect, fontname=Courier, fontsize=30]
edge [fontname=Courier, fontsize=30]
compound=true
  N0 [label =
    "ir-bb\<entry\>:\l" +
    "Successor(s): scalar.ph, vector.ph\l"
  ]
  N0 -> N1 [ label="T"]
  N0 -> N2 [ label="F"]
  N1 [label =
    "scalar.ph:\l" +
    "  EMIT-SCALAR vp\<%8\> = phi [ vp\<%6\>, middle.block ], [ ir\<0\>, ir-bb\<entry\> ]\l" +
    "Successor(s): ir-bb\<loop.header\>\l"
  ]
  N1 -> N3 [ label=""]
  N3 [label =
    "ir-bb\<loop.header\>:\l" +
    "  IR   %iv = phi i64 [ 0, %entry ], [ %iv.next, %loop.latch ] (extra operand: vp\<%8\> from scalar.ph)\l" +
    "  IR   %arr.idx = getelementptr inbounds i32, ptr %A, i64 %iv\l" +
    "  IR   %l1 = load i32, ptr %arr.idx, align 4\l" +
    "  IR   %c = icmp eq i32 %l1, 0\l" +
    "No successors\l"
  ]
  N2 [label =
    "vector.ph:\l" +
    "Successor(s): vector loop\l"
  ]
  N2 -> N4 [ label="" lhead=cluster_N5]
  subgraph cluster_N5 {
    fontname=Courier
    label="\<x1\> vector loop"
    "vp\<%2\> = CANONICAL-IV"
    N4 [label =
      "vector.body:\l" +
      "  EMIT-SCALAR ir\<%iv\> = phi [ ir\<0\>, vector.ph ], [ ir\<%iv.next\>, loop.latch ]\l" +
      "  EMIT ir\<%arr.idx\> = getelementptr inbounds ir\<%A\>, ir\<%iv\>\l" +
      "  EMIT-SCALAR ir\<%l1\> = load ir\<%arr.idx\>\l" +
      "  EMIT ir\<%c\> = icmp eq ir\<%l1\>, ir\<0\>\l" +
      "Successor(s): loop.latch\l"
    ]
    N4 -> N6 [ label=""]
    N6 [label =
      "loop.latch:\l" +
      "  EMIT ir\<%res\> = add ir\<%l1\>, ir\<10\>\l" +
      "  EMIT store ir\<%res\>, ir\<%arr.idx\>\l" +
      "  EMIT ir\<%iv.next\> = add ir\<%iv\>, ir\<1\>\l" +
      "  EMIT ir\<%exitcond\> = icmp ne ir\<%iv.next\>, ir\<%N\>\l" +
      "  EMIT vp\<%3\> = not ir\<%exitcond\>\l" +
      "  EMIT vp\<%index.next\> = add nuw vp\<%2\>, vp\<%0\>\l" +
      "  EMIT branch-on-count vp\<%index.next\>, vp\<%1\>\l" +
      "No successors\l"
    ]
  }
  N6 -> N7 [ label="" ltail=cluster_N5]
  N7 [label =
    "middle.block:\l" +
    "  EMIT vp\<%5\> = extract-last-part ir\<%iv.next\>\l" +
    "  EMIT vp\<%6\> = extract-last-lane vp\<%5\>\l" +
    "  EMIT vp\<%cmp.n\> = icmp eq ir\<%N\>, vp\<%1\>\l" +
    "  EMIT branch-on-cond vp\<%cmp.n\>\l" +
    "Successor(s): ir-bb\<exit.2\>, scalar.ph\l"
  ]
  N7 -> N8 [ label="T"]
  N7 -> N1 [ label="F"]
  N8 [label =
    "ir-bb\<exit.2\>:\l" +
    "No successors\l"
  ]
}
)";
  EXPECT_EQ(ExpectedStr, FullDump);
#endif
}

} // namespace
} // namespace llvm
