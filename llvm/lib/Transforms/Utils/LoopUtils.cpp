//===-- LoopUtils.cpp - Loop Utility functions -------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file defines common loop utility functions.
//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/Utils/LoopUtils.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/PriorityWorklist.h"
#include "llvm/ADT/ScopeExit.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/Analysis/BasicAliasAnalysis.h"
#include "llvm/Analysis/DomTreeUpdater.h"
#include "llvm/Analysis/GlobalsModRef.h"
#include "llvm/Analysis/InstSimplifyFolder.h"
#include "llvm/Analysis/LoopAccessAnalysis.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/LoopPass.h"
#include "llvm/Analysis/MemorySSA.h"
#include "llvm/Analysis/MemorySSAUpdater.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/Analysis/ScalarEvolutionAliasAnalysis.h"
#include "llvm/Analysis/ScalarEvolutionExpressions.h"
#include "llvm/IR/DIBuilder.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/MDBuilder.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/PatternMatch.h"
#include "llvm/IR/ProfDataUtils.h"
#include "llvm/IR/ValueHandle.h"
#include "llvm/InitializePasses.h"
#include "llvm/Pass.h"
#include "llvm/Support/Compiler.h"
#include "llvm/Support/Debug.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include "llvm/Transforms/Utils/Local.h"
#include "llvm/Transforms/Utils/ScalarEvolutionExpander.h"

using namespace llvm;
using namespace llvm::PatternMatch;

#define DEBUG_TYPE "loop-utils"

static const char *LLVMLoopDisableNonforced = "llvm.loop.disable_nonforced";
static const char *LLVMLoopDisableLICM = "llvm.licm.disable";
namespace llvm {
extern cl::opt<bool> ProfcheckDisableMetadataFixes;
} // namespace llvm

bool llvm::formDedicatedExitBlocks(Loop *L, DominatorTree *DT, LoopInfo *LI,
                                   MemorySSAUpdater *MSSAU,
                                   bool PreserveLCSSA) {
  bool Changed = false;

  // We re-use a vector for the in-loop predecesosrs.
  SmallVector<BasicBlock *, 4> InLoopPredecessors;

  auto RewriteExit = [&](BasicBlock *BB) {
    assert(InLoopPredecessors.empty() &&
           "Must start with an empty predecessors list!");
    llvm::scope_exit Cleanup([&] { InLoopPredecessors.clear(); });

    // See if there are any non-loop predecessors of this exit block and
    // keep track of the in-loop predecessors.
    bool IsDedicatedExit = true;
    for (auto *PredBB : predecessors(BB))
      if (L->contains(PredBB)) {
        if (isa<IndirectBrInst>(PredBB->getTerminator()))
          // We cannot rewrite exiting edges from an indirectbr.
          return false;

        InLoopPredecessors.push_back(PredBB);
      } else {
        IsDedicatedExit = false;
      }

    assert(!InLoopPredecessors.empty() && "Must have *some* loop predecessor!");

    // Nothing to do if this is already a dedicated exit.
    if (IsDedicatedExit)
      return false;

    auto *NewExitBB = SplitBlockPredecessors(
        BB, InLoopPredecessors, ".loopexit", DT, LI, MSSAU, PreserveLCSSA);

    if (!NewExitBB)
      LLVM_DEBUG(
          dbgs() << "WARNING: Can't create a dedicated exit block for loop: "
                 << *L << "\n");
    else
      LLVM_DEBUG(dbgs() << "LoopSimplify: Creating dedicated exit block "
                        << NewExitBB->getName() << "\n");
    return true;
  };

  // Walk the exit blocks directly rather than building up a data structure for
  // them, but only visit each one once.
  SmallPtrSet<BasicBlock *, 4> Visited;
  for (auto *BB : L->blocks())
    for (auto *SuccBB : successors(BB)) {
      // We're looking for exit blocks so skip in-loop successors.
      if (L->contains(SuccBB))
        continue;

      // Visit each exit block exactly once.
      if (!Visited.insert(SuccBB).second)
        continue;

      Changed |= RewriteExit(SuccBB);
    }

  return Changed;
}

/// Returns the instructions that use values defined in the loop.
SmallVector<Instruction *, 8> llvm::findDefsUsedOutsideOfLoop(Loop *L) {
  SmallVector<Instruction *, 8> UsedOutside;

  for (auto *Block : L->getBlocks())
    // FIXME: I believe that this could use copy_if if the Inst reference could
    // be adapted into a pointer.
    for (auto &Inst : *Block) {
      auto Users = Inst.users();
      if (any_of(Users, [&](User *U) {
            auto *Use = cast<Instruction>(U);
            return !L->contains(Use->getParent());
          }))
        UsedOutside.push_back(&Inst);
    }

  return UsedOutside;
}

void llvm::getLoopAnalysisUsage(AnalysisUsage &AU) {
  // By definition, all loop passes need the LoopInfo analysis and the
  // Dominator tree it depends on. Because they all participate in the loop
  // pass manager, they must also preserve these.
  AU.addRequired<DominatorTreeWrapperPass>();
  AU.addPreserved<DominatorTreeWrapperPass>();
  AU.addRequired<LoopInfoWrapperPass>();
  AU.addPreserved<LoopInfoWrapperPass>();

  // We must also preserve LoopSimplify and LCSSA. We locally access their IDs
  // here because users shouldn't directly get them from this header.
  extern char &LoopSimplifyID;
  extern char &LCSSAID;
  AU.addRequiredID(LoopSimplifyID);
  AU.addPreservedID(LoopSimplifyID);
  AU.addRequiredID(LCSSAID);
  AU.addPreservedID(LCSSAID);
  // This is used in the LPPassManager to perform LCSSA verification on passes
  // which preserve lcssa form
  AU.addRequired<LCSSAVerificationPass>();
  AU.addPreserved<LCSSAVerificationPass>();

  // Loop passes are designed to run inside of a loop pass manager which means
  // that any function analyses they require must be required by the first loop
  // pass in the manager (so that it is computed before the loop pass manager
  // runs) and preserved by all loop pasess in the manager. To make this
  // reasonably robust, the set needed for most loop passes is maintained here.
  // If your loop pass requires an analysis not listed here, you will need to
  // carefully audit the loop pass manager nesting structure that results.
  AU.addRequired<AAResultsWrapperPass>();
  AU.addPreserved<AAResultsWrapperPass>();
  AU.addPreserved<BasicAAWrapperPass>();
  AU.addPreserved<GlobalsAAWrapperPass>();
  AU.addPreserved<SCEVAAWrapperPass>();
  AU.addRequired<ScalarEvolutionWrapperPass>();
  AU.addPreserved<ScalarEvolutionWrapperPass>();
  // FIXME: When all loop passes preserve MemorySSA, it can be required and
  // preserved here instead of the individual handling in each pass.
}

/// Manually defined generic "LoopPass" dependency initialization. This is used
/// to initialize the exact set of passes from above in \c
/// getLoopAnalysisUsage. It can be used within a loop pass's initialization
/// with:
///
///   INITIALIZE_PASS_DEPENDENCY(LoopPass)
///
/// As-if "LoopPass" were a pass.
void llvm::initializeLoopPassPass(PassRegistry &Registry) {
  INITIALIZE_PASS_DEPENDENCY(DominatorTreeWrapperPass)
  INITIALIZE_PASS_DEPENDENCY(LoopInfoWrapperPass)
  INITIALIZE_PASS_DEPENDENCY(LoopSimplify)
  INITIALIZE_PASS_DEPENDENCY(LCSSAWrapperPass)
  INITIALIZE_PASS_DEPENDENCY(AAResultsWrapperPass)
  INITIALIZE_PASS_DEPENDENCY(BasicAAWrapperPass)
  INITIALIZE_PASS_DEPENDENCY(GlobalsAAWrapperPass)
  INITIALIZE_PASS_DEPENDENCY(SCEVAAWrapperPass)
  INITIALIZE_PASS_DEPENDENCY(ScalarEvolutionWrapperPass)
  INITIALIZE_PASS_DEPENDENCY(MemorySSAWrapperPass)
}

/// Create MDNode for input string.
static MDNode *createStringMetadata(Loop *TheLoop, StringRef Name, unsigned V) {
  LLVMContext &Context = TheLoop->getHeader()->getContext();
  Metadata *MDs[] = {
      MDString::get(Context, Name),
      ConstantAsMetadata::get(ConstantInt::get(Type::getInt32Ty(Context), V))};
  return MDNode::get(Context, MDs);
}

/// Set input string into loop metadata by keeping other values intact.
/// If the string is already in loop metadata update value if it is
/// different.
void llvm::addStringMetadataToLoop(Loop *TheLoop, const char *StringMD,
                                   unsigned V) {
  SmallVector<Metadata *, 4> MDs(1);
  // If the loop already has metadata, retain it.
  MDNode *LoopID = TheLoop->getLoopID();
  if (LoopID) {
    for (unsigned i = 1, ie = LoopID->getNumOperands(); i < ie; ++i) {
      MDNode *Node = cast<MDNode>(LoopID->getOperand(i));
      // If it is of form key = value, try to parse it.
      if (Node->getNumOperands() == 2) {
        MDString *S = dyn_cast<MDString>(Node->getOperand(0));
        if (S && S->getString() == StringMD) {
          ConstantInt *IntMD =
              mdconst::extract_or_null<ConstantInt>(Node->getOperand(1));
          if (IntMD && IntMD->getSExtValue() == V)
            // It is already in place. Do nothing.
            return;
          // We need to update the value, so just skip it here and it will
          // be added after copying other existed nodes.
          continue;
        }
      }
      MDs.push_back(Node);
    }
  }
  // Add new metadata.
  MDs.push_back(createStringMetadata(TheLoop, StringMD, V));
  // Replace current metadata node with new one.
  LLVMContext &Context = TheLoop->getHeader()->getContext();
  MDNode *NewLoopID = MDNode::get(Context, MDs);
  // Set operand 0 to refer to the loop id itself.
  NewLoopID->replaceOperandWith(0, NewLoopID);
  TheLoop->setLoopID(NewLoopID);
}

std::optional<ElementCount>
llvm::getOptionalElementCountLoopAttribute(const Loop *TheLoop) {
  std::optional<int> Width =
      getOptionalIntLoopAttribute(TheLoop, "llvm.loop.vectorize.width");

  if (Width) {
    std::optional<int> IsScalable = getOptionalIntLoopAttribute(
        TheLoop, "llvm.loop.vectorize.scalable.enable");
    return ElementCount::get(*Width, IsScalable.value_or(false));
  }

  return std::nullopt;
}

std::optional<MDNode *> llvm::makeFollowupLoopID(
    MDNode *OrigLoopID, ArrayRef<StringRef> FollowupOptions,
    const char *InheritOptionsExceptPrefix, bool AlwaysNew) {
  if (!OrigLoopID) {
    if (AlwaysNew)
      return nullptr;
    return std::nullopt;
  }

  assert(OrigLoopID->getOperand(0) == OrigLoopID);

  bool InheritAllAttrs = !InheritOptionsExceptPrefix;
  bool InheritSomeAttrs =
      InheritOptionsExceptPrefix && InheritOptionsExceptPrefix[0] != '\0';
  SmallVector<Metadata *, 8> MDs;
  MDs.push_back(nullptr);

  bool Changed = false;
  if (InheritAllAttrs || InheritSomeAttrs) {
    for (const MDOperand &Existing : drop_begin(OrigLoopID->operands())) {
      MDNode *Op = cast<MDNode>(Existing.get());

      auto InheritThisAttribute = [InheritSomeAttrs,
                                   InheritOptionsExceptPrefix](MDNode *Op) {
        if (!InheritSomeAttrs)
          return false;

        // Skip malformatted attribute metadata nodes.
        if (Op->getNumOperands() == 0)
          return true;
        Metadata *NameMD = Op->getOperand(0).get();
        if (!isa<MDString>(NameMD))
          return true;
        StringRef AttrName = cast<MDString>(NameMD)->getString();

        // Do not inherit excluded attributes.
        return !AttrName.starts_with(InheritOptionsExceptPrefix);
      };

      if (InheritThisAttribute(Op))
        MDs.push_back(Op);
      else
        Changed = true;
    }
  } else {
    // Modified if we dropped at least one attribute.
    Changed = OrigLoopID->getNumOperands() > 1;
  }

  bool HasAnyFollowup = false;
  for (StringRef OptionName : FollowupOptions) {
    MDNode *FollowupNode = findOptionMDForLoopID(OrigLoopID, OptionName);
    if (!FollowupNode)
      continue;

    HasAnyFollowup = true;
    for (const MDOperand &Option : drop_begin(FollowupNode->operands())) {
      MDs.push_back(Option.get());
      Changed = true;
    }
  }

  // Attributes of the followup loop not specified explicity, so signal to the
  // transformation pass to add suitable attributes.
  if (!AlwaysNew && !HasAnyFollowup)
    return std::nullopt;

  // If no attributes were added or remove, the previous loop Id can be reused.
  if (!AlwaysNew && !Changed)
    return OrigLoopID;

  // No attributes is equivalent to having no !llvm.loop metadata at all.
  if (MDs.size() == 1)
    return nullptr;

  // Build the new loop ID.
  MDTuple *FollowupLoopID = MDNode::get(OrigLoopID->getContext(), MDs);
  FollowupLoopID->replaceOperandWith(0, FollowupLoopID);
  return FollowupLoopID;
}

bool llvm::hasDisableAllTransformsHint(const Loop *L) {
  return getBooleanLoopAttribute(L, LLVMLoopDisableNonforced);
}

bool llvm::hasDisableLICMTransformsHint(const Loop *L) {
  return getBooleanLoopAttribute(L, LLVMLoopDisableLICM);
}

StringRef llvm::getLoopVectorizeKindPrefix(const Loop *L) {
  bool IsVectorBody = getBooleanLoopAttribute(L, "llvm.loop.vectorize.body");
  bool IsEpilogue = getBooleanLoopAttribute(L, "llvm.loop.vectorize.epilogue");
  if (IsVectorBody && IsEpilogue)
    return "vectorized epilogue ";
  if (IsVectorBody)
    return "vectorized ";
  if (IsEpilogue)
    return "epilogue ";
  return "";
}

TransformationMode llvm::hasUnrollTransformation(const Loop *L) {
  if (getBooleanLoopAttribute(L, "llvm.loop.unroll.disable"))
    return TM_SuppressedByUser;

  std::optional<int> Count =
      getOptionalIntLoopAttribute(L, "llvm.loop.unroll.count");
  if (Count)
    return *Count == 1 ? TM_SuppressedByUser : TM_ForcedByUser;

  if (getBooleanLoopAttribute(L, "llvm.loop.unroll.enable"))
    return TM_ForcedByUser;

  if (getBooleanLoopAttribute(L, "llvm.loop.unroll.full"))
    return TM_ForcedByUser;

  if (hasDisableAllTransformsHint(L))
    return TM_Disable;

  return TM_Unspecified;
}

TransformationMode llvm::hasUnrollAndJamTransformation(const Loop *L) {
  if (getBooleanLoopAttribute(L, "llvm.loop.unroll_and_jam.disable"))
    return TM_SuppressedByUser;

  std::optional<int> Count =
      getOptionalIntLoopAttribute(L, "llvm.loop.unroll_and_jam.count");
  if (Count)
    return *Count == 1 ? TM_SuppressedByUser : TM_ForcedByUser;

  if (getBooleanLoopAttribute(L, "llvm.loop.unroll_and_jam.enable"))
    return TM_ForcedByUser;

  if (hasDisableAllTransformsHint(L))
    return TM_Disable;

  return TM_Unspecified;
}

TransformationMode llvm::hasVectorizeTransformation(const Loop *L) {
  std::optional<bool> Enable =
      getOptionalBoolLoopAttribute(L, "llvm.loop.vectorize.enable");

  if (Enable == false)
    return TM_SuppressedByUser;

  std::optional<ElementCount> VectorizeWidth =
      getOptionalElementCountLoopAttribute(L);
  std::optional<int> InterleaveCount =
      getOptionalIntLoopAttribute(L, "llvm.loop.interleave.count");

  // 'Forcing' vector width and interleave count to one effectively disables
  // this tranformation.
  if (Enable == true && VectorizeWidth && VectorizeWidth->isScalar() &&
      InterleaveCount == 1)
    return TM_SuppressedByUser;

  if (getBooleanLoopAttribute(L, "llvm.loop.isvectorized"))
    return TM_Disable;

  if (Enable == true)
    return TM_ForcedByUser;

  if ((VectorizeWidth && VectorizeWidth->isScalar()) && InterleaveCount == 1)
    return TM_Disable;

  if ((VectorizeWidth && VectorizeWidth->isVector()) || InterleaveCount > 1)
    return TM_Enable;

  if (hasDisableAllTransformsHint(L))
    return TM_Disable;

  return TM_Unspecified;
}

TransformationMode llvm::hasDistributeTransformation(const Loop *L) {
  if (getBooleanLoopAttribute(L, "llvm.loop.distribute.enable"))
    return TM_ForcedByUser;

  if (hasDisableAllTransformsHint(L))
    return TM_Disable;

  return TM_Unspecified;
}

TransformationMode llvm::hasLICMVersioningTransformation(const Loop *L) {
  if (getBooleanLoopAttribute(L, "llvm.loop.licm_versioning.disable"))
    return TM_SuppressedByUser;

  if (hasDisableAllTransformsHint(L))
    return TM_Disable;

  return TM_Unspecified;
}

/// Does a BFS from a given node to all of its children inside a given loop.
/// The returned vector of basic blocks includes the starting point.
SmallVector<BasicBlock *, 16> llvm::collectChildrenInLoop(DominatorTree *DT,
                                                          DomTreeNode *N,
                                                          const Loop *CurLoop) {
  SmallVector<BasicBlock *, 16> Worklist;
  auto AddRegionToWorklist = [&](DomTreeNode *DTN) {
    // Only include subregions in the top level loop.
    BasicBlock *BB = DTN->getBlock();
    if (CurLoop->contains(BB))
      Worklist.push_back(DTN->getBlock());
  };

  AddRegionToWorklist(N);

  for (size_t I = 0; I < Worklist.size(); I++) {
    for (DomTreeNode *Child : DT->getNode(Worklist[I])->children())
      AddRegionToWorklist(Child);
  }

  return Worklist;
}

bool llvm::isAlmostDeadIV(PHINode *PN, BasicBlock *LatchBlock, Value *Cond) {
  int LatchIdx = PN->getBasicBlockIndex(LatchBlock);
  assert(LatchIdx != -1 && "LatchBlock is not a case in this PHINode");
  Value *IncV = PN->getIncomingValue(LatchIdx);

  for (User *U : PN->users())
    if (U != Cond && U != IncV) return false;

  for (User *U : IncV->users())
    if (U != Cond && U != PN) return false;
  return true;
}


void llvm::deleteDeadLoop(Loop *L, DominatorTree *DT, ScalarEvolution *SE,
                          LoopInfo *LI, MemorySSA *MSSA) {
  assert((!DT || L->isLCSSAForm(*DT)) && "Expected LCSSA!");
  auto *Preheader = L->getLoopPreheader();
  assert(Preheader && "Preheader should exist!");

  std::unique_ptr<MemorySSAUpdater> MSSAU;
  if (MSSA)
    MSSAU = std::make_unique<MemorySSAUpdater>(MSSA);

  // Now that we know the removal is safe, remove the loop by changing the
  // branch from the preheader to go to the single exit block.
  //
  // Because we're deleting a large chunk of code at once, the sequence in which
  // we remove things is very important to avoid invalidation issues.

  // Tell ScalarEvolution that the loop is deleted. Do this before
  // deleting the loop so that ScalarEvolution can look at the loop
  // to determine what it needs to clean up.
  if (SE) {
    SE->forgetLoop(L);
    SE->forgetBlockAndLoopDispositions();
  }

  Instruction *OldTerm = Preheader->getTerminator();
  assert(!OldTerm->mayHaveSideEffects() &&
         "Preheader must end with a side-effect-free terminator");
  assert(OldTerm->getNumSuccessors() == 1 &&
         "Preheader must have a single successor");
  // Connect the preheader to the exit block. Keep the old edge to the header
  // around to perform the dominator tree update in two separate steps
  // -- #1 insertion of the edge preheader -> exit and #2 deletion of the edge
  // preheader -> header.
  //
  //
  // 0.  Preheader          1.  Preheader           2.  Preheader
  //        |                    |   |                   |
  //        V                    |   V                   |
  //      Header <--\            | Header <--\           | Header <--\
  //       |  |     |            |  |  |     |           |  |  |     |
  //       |  V     |            |  |  V     |           |  |  V     |
  //       | Body --/            |  | Body --/           |  | Body --/
  //       V                     V  V                    V  V
  //      Exit                   Exit                    Exit
  //
  // By doing this is two separate steps we can perform the dominator tree
  // update without using the batch update API.
  //
  // Even when the loop is never executed, we cannot remove the edge from the
  // source block to the exit block. Consider the case where the unexecuted loop
  // branches back to an outer loop. If we deleted the loop and removed the edge
  // coming to this inner loop, this will break the outer loop structure (by
  // deleting the backedge of the outer loop). If the outer loop is indeed a
  // non-loop, it will be deleted in a future iteration of loop deletion pass.
  IRBuilder<> Builder(OldTerm);

  auto *ExitBlock = L->getUniqueExitBlock();
  DomTreeUpdater DTU(DT, DomTreeUpdater::UpdateStrategy::Eager);
  if (ExitBlock) {
    assert(ExitBlock && "Should have a unique exit block!");
    assert(L->hasDedicatedExits() && "Loop should have dedicated exits!");

    Builder.CreateCondBr(Builder.getFalse(), L->getHeader(), ExitBlock);
    // Remove the old branch. The conditional branch becomes a new terminator.
    OldTerm->eraseFromParent();

    // Rewrite phis in the exit block to get their inputs from the Preheader
    // instead of the exiting block.
    for (PHINode &P : ExitBlock->phis()) {
      // Set the zero'th element of Phi to be from the preheader and remove all
      // other incoming values. Given the loop has dedicated exits, all other
      // incoming values must be from the exiting blocks.
      int PredIndex = 0;
      P.setIncomingBlock(PredIndex, Preheader);
      // Removes all incoming values from all other exiting blocks (including
      // duplicate values from an exiting block).
      // Nuke all entries except the zero'th entry which is the preheader entry.
      P.removeIncomingValueIf([](unsigned Idx) { return Idx != 0; },
                              /* DeletePHIIfEmpty */ false);

      assert((P.getNumIncomingValues() == 1 &&
              P.getIncomingBlock(PredIndex) == Preheader) &&
             "Should have exactly one value and that's from the preheader!");
    }

    if (DT) {
      DTU.applyUpdates({{DominatorTree::Insert, Preheader, ExitBlock}});
      if (MSSA) {
        MSSAU->applyUpdates({{DominatorTree::Insert, Preheader, ExitBlock}},
                            *DT);
        if (VerifyMemorySSA)
          MSSA->verifyMemorySSA();
      }
    }

    // Disconnect the loop body by branching directly to its exit.
    Builder.SetInsertPoint(Preheader->getTerminator());
    Builder.CreateBr(ExitBlock);
    // Remove the old branch.
    Preheader->getTerminator()->eraseFromParent();
  } else {
    assert(L->hasNoExitBlocks() &&
           "Loop should have either zero or one exit blocks.");

    Builder.SetInsertPoint(OldTerm);
    Builder.CreateUnreachable();
    Preheader->getTerminator()->eraseFromParent();
  }

  if (DT) {
    DTU.applyUpdates({{DominatorTree::Delete, Preheader, L->getHeader()}});
    if (MSSA) {
      MSSAU->applyUpdates({{DominatorTree::Delete, Preheader, L->getHeader()}},
                          *DT);
      SmallSetVector<BasicBlock *, 8> DeadBlockSet(L->block_begin(),
                                                   L->block_end());
      MSSAU->removeBlocks(DeadBlockSet);
      if (VerifyMemorySSA)
        MSSA->verifyMemorySSA();
    }
  }

  // Use a map to unique and a vector to guarantee deterministic ordering.
  llvm::SmallDenseSet<DebugVariable, 4> DeadDebugSet;
  llvm::SmallVector<DbgVariableRecord *, 4> DeadDbgVariableRecords;

  // Given LCSSA form is satisfied, we should not have users of instructions
  // within the dead loop outside of the loop. However, LCSSA doesn't take
  // unreachable uses into account. We handle them here.
  // We could do it after drop all references (in this case all users in the
  // loop will be already eliminated and we have less work to do but according
  // to API doc of User::dropAllReferences only valid operation after dropping
  // references, is deletion. So let's substitute all usages of
  // instruction from the loop with poison value of corresponding type first.
  for (auto *Block : L->blocks())
    for (Instruction &I : *Block) {
      auto *Poison = PoisonValue::get(I.getType());
      for (Use &U : llvm::make_early_inc_range(I.uses())) {
        if (auto *Usr = dyn_cast<Instruction>(U.getUser()))
          if (L->contains(Usr->getParent()))
            continue;
        // If we have a DT then we can check that uses outside a loop only in
        // unreachable block.
        if (DT)
          assert(!DT->isReachableFromEntry(U) &&
                 "Unexpected user in reachable block");
        U.set(Poison);
      }

      if (ExitBlock) {
        // For one of each variable encountered, preserve a debug record (set
        // to Poison) and transfer it to the loop exit. This terminates any
        // variable locations that were set during the loop.
        for (DbgVariableRecord &DVR :
             llvm::make_early_inc_range(filterDbgVars(I.getDbgRecordRange()))) {
          DebugVariable Key(DVR.getVariable(), DVR.getExpression(),
                            DVR.getDebugLoc().get());
          if (!DeadDebugSet.insert(Key).second)
            continue;
          // Unlinks the DVR from it's container, for later insertion.
          DVR.removeFromParent();
          DeadDbgVariableRecords.push_back(&DVR);
        }
      }
    }

  if (ExitBlock) {
    // After the loop has been deleted all the values defined and modified
    // inside the loop are going to be unavailable. Values computed in the
    // loop will have been deleted, automatically causing their debug uses
    // be be replaced with undef. Loop invariant values will still be available.
    // Move dbg.values out the loop so that earlier location ranges are still
    // terminated and loop invariant assignments are preserved.
    DIBuilder DIB(*ExitBlock->getModule());
    BasicBlock::iterator InsertDbgValueBefore =
        ExitBlock->getFirstInsertionPt();
    assert(InsertDbgValueBefore != ExitBlock->end() &&
           "There should be a non-PHI instruction in exit block, else these "
           "instructions will have no parent.");

    // Due to the "head" bit in BasicBlock::iterator, we're going to insert
    // each DbgVariableRecord right at the start of the block, wheras dbg.values
    // would be repeatedly inserted before the first instruction. To replicate
    // this behaviour, do it backwards.
    for (DbgVariableRecord *DVR : llvm::reverse(DeadDbgVariableRecords))
      ExitBlock->insertDbgRecordBefore(DVR, InsertDbgValueBefore);
  }

  // Remove the block from the reference counting scheme, so that we can
  // delete it freely later.
  for (auto *Block : L->blocks())
    Block->dropAllReferences();

  if (MSSA && VerifyMemorySSA)
    MSSA->verifyMemorySSA();

  if (LI) {
    SmallPtrSet<BasicBlock *, 8> Blocks(llvm::from_range, L->blocks());

    // Erase the instructions and the blocks without having to worry
    // about ordering because we already dropped the references.
    // Remove blocks from loopinfo before erasing them, otherwise the loopinfo
    // cannot find the loop using block numbers.
    for (BasicBlock *BB : Blocks) {
      LI->removeBlock(BB);
      BB->eraseFromParent();
    }

    // The last step is to update LoopInfo now that we've eliminated this loop.
    // Note: LoopInfo::erase remove the given loop and relink its subloops with
    // its parent. While removeLoop/removeChildLoop remove the given loop but
    // not relink its subloops, which is what we want.
    if (Loop *ParentLoop = L->getParentLoop()) {
      Loop::iterator I = find(*ParentLoop, L);
      assert(I != ParentLoop->end() && "Couldn't find loop");
      ParentLoop->removeChildLoop(I);
    } else {
      Loop::iterator I = find(*LI, L);
      assert(I != LI->end() && "Couldn't find loop");
      LI->removeLoop(I);
    }
    LI->destroy(L);
  }
}

void llvm::breakLoopBackedge(Loop *L, DominatorTree &DT, ScalarEvolution &SE,
                             LoopInfo &LI, MemorySSA *MSSA) {
  auto *Latch = L->getLoopLatch();
  assert(Latch && "multiple latches not yet supported");
  auto *Header = L->getHeader();
  Loop *OutermostLoop = L->getOutermostLoop();

  SE.forgetLoop(L);
  SE.forgetBlockAndLoopDispositions();

  std::unique_ptr<MemorySSAUpdater> MSSAU;
  if (MSSA)
    MSSAU = std::make_unique<MemorySSAUpdater>(MSSA);

  // Update the CFG and domtree.  We chose to special case a couple of
  // of common cases for code quality and test readability reasons.
  [&]() -> void {
    if (auto *BI = dyn_cast<UncondBrInst>(Latch->getTerminator())) {
      DomTreeUpdater DTU(&DT, DomTreeUpdater::UpdateStrategy::Eager);
      (void)changeToUnreachable(BI, /*PreserveLCSSA*/ true, &DTU, MSSAU.get());
      return;
    }
    if (auto *BI = dyn_cast<CondBrInst>(Latch->getTerminator())) {
      // Conditional latch/exit - note that latch can be shared by inner
      // and outer loop so the other target doesn't need to an exit
      if (L->isLoopExiting(Latch)) {
        // TODO: Generalize ConstantFoldTerminator so that it can be used
        // here without invalidating LCSSA or MemorySSA.  (Tricky case for
        // LCSSA: header is an exit block of a preceeding sibling loop w/o
        // dedicated exits.)
        const unsigned ExitIdx = L->contains(BI->getSuccessor(0)) ? 1 : 0;
        BasicBlock *ExitBB = BI->getSuccessor(ExitIdx);

        DomTreeUpdater DTU(&DT, DomTreeUpdater::UpdateStrategy::Eager);
        Header->removePredecessor(Latch, true);

        IRBuilder<> Builder(BI);
        auto *NewBI = Builder.CreateBr(ExitBB);
        // Transfer the metadata to the new branch instruction (minus the
        // loop info since this is no longer a loop)
        NewBI->copyMetadata(*BI, {LLVMContext::MD_dbg,
                                  LLVMContext::MD_annotation});

        BI->eraseFromParent();
        DTU.applyUpdates({{DominatorTree::Delete, Latch, Header}});
        if (MSSA)
          MSSAU->applyUpdates({{DominatorTree::Delete, Latch, Header}}, DT);
        return;
      }
    }

    // General case.  By splitting the backedge, and then explicitly making it
    // unreachable we gracefully handle corner cases such as switch and invoke
    // termiantors.
    auto *BackedgeBB = SplitEdge(Latch, Header, &DT, &LI, MSSAU.get());

    DomTreeUpdater DTU(&DT, DomTreeUpdater::UpdateStrategy::Eager);
    (void)changeToUnreachable(BackedgeBB->getTerminator(),
                              /*PreserveLCSSA*/ true, &DTU, MSSAU.get());
  }();

  // Erase (and destroy) this loop instance.  Handles relinking sub-loops
  // and blocks within the loop as needed.
  LI.erase(L);

  // If the loop we broke had a parent, then changeToUnreachable might have
  // caused a block to be removed from the parent loop (see loop_nest_lcssa
  // test case in zero-btc.ll for an example), thus changing the parent's
  // exit blocks.  If that happened, we need to rebuild LCSSA on the outermost
  // loop which might have a had a block removed.
  if (OutermostLoop != L)
    formLCSSARecursively(*OutermostLoop, DT, &LI, &SE);
}


/// Checks if \p L has an exiting latch branch.  There may also be other
/// exiting blocks.  Returns branch instruction terminating the loop
/// latch if above check is successful, nullptr otherwise.
static CondBrInst *getExpectedExitLoopLatchBranch(Loop *L) {
  BasicBlock *Latch = L->getLoopLatch();
  if (!Latch)
    return nullptr;

  CondBrInst *LatchBR = dyn_cast<CondBrInst>(Latch->getTerminator());
  if (!LatchBR || !L->isLoopExiting(Latch))
    return nullptr;

  assert((LatchBR->getSuccessor(0) == L->getHeader() ||
          LatchBR->getSuccessor(1) == L->getHeader()) &&
         "At least one edge out of the latch must go to the header");

  return LatchBR;
}

struct DbgLoop {
  const Loop *L;
  explicit DbgLoop(const Loop *L) : L(L) {}
};

#ifndef NDEBUG
static inline raw_ostream &operator<<(raw_ostream &OS, DbgLoop D) {
  OS << "function ";
  D.L->getHeader()->getParent()->printAsOperand(OS, /*PrintType=*/false);
  return OS << " " << *D.L;
}
#endif // NDEBUG

static std::optional<unsigned> estimateLoopTripCount(Loop *L) {
  // Currently we take the estimate exit count only from the loop latch,
  // ignoring other exiting blocks.  This can overestimate the trip count
  // if we exit through another exit, but can never underestimate it.
  // TODO: incorporate information from other exits
  CondBrInst *ExitingBranch = getExpectedExitLoopLatchBranch(L);
  if (!ExitingBranch) {
    LLVM_DEBUG(dbgs() << "estimateLoopTripCount: Failed to find exiting "
                      << "latch branch of required form in " << DbgLoop(L)
                      << "\n");
    return std::nullopt;
  }

  // To estimate the number of times the loop body was executed, we want to
  // know the number of times the backedge was taken, vs. the number of times
  // we exited the loop.
  uint64_t LoopWeight, ExitWeight;
  if (!extractBranchWeights(*ExitingBranch, LoopWeight, ExitWeight)) {
    LLVM_DEBUG(dbgs() << "estimateLoopTripCount: Failed to extract branch "
                      << "weights for " << DbgLoop(L) << "\n");
    return std::nullopt;
  }

  if (L->contains(ExitingBranch->getSuccessor(1)))
    std::swap(LoopWeight, ExitWeight);

  if (!ExitWeight) {
    // Don't have a way to return predicated infinite
    LLVM_DEBUG(dbgs() << "estimateLoopTripCount: Failed because of zero exit "
                      << "probability for " << DbgLoop(L) << "\n");
    return std::nullopt;
  }

  // Estimated exit count is a ratio of the loop weight by the weight of the
  // edge exiting the loop, rounded to nearest.
  uint64_t ExitCount = llvm::divideNearest(LoopWeight, ExitWeight);

  // When ExitCount + 1 would wrap in unsigned, saturate at UINT_MAX.
  if (ExitCount >= std::numeric_limits<unsigned>::max())
    return std::numeric_limits<unsigned>::max();

  // Estimated trip count is one plus estimated exit count.
  uint64_t TC = ExitCount + 1;
  LLVM_DEBUG(dbgs() << "estimateLoopTripCount: Estimated trip count of " << TC
                    << " for " << DbgLoop(L) << "\n");
  return TC;
}

std::optional<unsigned>
llvm::getLoopEstimatedTripCount(Loop *L,
                                unsigned *EstimatedLoopInvocationWeight) {
  // If EstimatedLoopInvocationWeight, we do not support this loop if
  // getExpectedExitLoopLatchBranch returns nullptr.
  //
  // FIXME: Also, this is a stop-gap solution for nested loops.  It avoids
  // mistaking LLVMLoopEstimatedTripCount metadata to be for an outer loop when
  // it was created for an inner loop.  The problem is that loop metadata is
  // attached to the branch instruction in the loop latch block, but that can be
  // shared by the loops.  A solution is to attach loop metadata to loop headers
  // instead, but that would be a large change to LLVM.
  //
  // Until that happens, we work around the problem as follows.
  // getExpectedExitLoopLatchBranch (which also guards
  // setLoopEstimatedTripCount) returns nullptr for a loop unless the loop has
  // one latch and that latch has exactly two successors one of which is an exit
  // from the loop.  If the latch is shared by nested loops, then that condition
  // might hold for the inner loop but cannot hold for the outer loop:
  // - Because the latch is shared, it must have at least two successors: the
  //   inner loop header and the outer loop header, which is also an exit for
  //   the inner loop.  That satisifies the condition for the inner loop.
  // - To satsify the condition for the outer loop, the latch must have a third
  //   successor that is an exit for the outer loop.  But that violates the
  //   condition for both loops.
  CondBrInst *ExitingBranch = getExpectedExitLoopLatchBranch(L);
  if (!ExitingBranch)
    return std::nullopt;

  // If requested, either compute *EstimatedLoopInvocationWeight or return
  // nullopt if cannot.
  //
  // TODO: Eventually, once all passes have migrated away from setting branch
  // weights to indicate estimated trip counts, this function will drop the
  // EstimatedLoopInvocationWeight parameter.
  if (EstimatedLoopInvocationWeight) {
    uint64_t LoopWeight = 0, ExitWeight = 0; // Inits expected to be unused.
    if (!extractBranchWeights(*ExitingBranch, LoopWeight, ExitWeight))
      return std::nullopt;
    if (L->contains(ExitingBranch->getSuccessor(1)))
      std::swap(LoopWeight, ExitWeight);
    if (!ExitWeight)
      return std::nullopt;
    *EstimatedLoopInvocationWeight = ExitWeight;
  }

  // Return the estimated trip count from metadata unless the metadata is
  // missing or has no value.
  //
  // Some passes set llvm.loop.estimated_trip_count to 0.  For example, after
  // peeling 10 or more iterations from a loop with an estimated trip count of
  // 10, llvm.loop.estimated_trip_count becomes 0 on the remaining loop.  It
  // indicates that, each time execution reaches the peeled iterations,
  // execution is estimated to exit them without reaching the remaining loop's
  // header.
  //
  // Even if the probability of reaching a loop's header is low, if it is
  // reached, it is the start of an iteration.  Consequently, some passes
  // historically assume that llvm::getLoopEstimatedTripCount always returns a
  // positive count or std::nullopt.  Thus, return std::nullopt when
  // llvm.loop.estimated_trip_count is 0.
  if (std::optional<unsigned> TC =
          getOptionalIntLoopAttribute(L, LLVMLoopEstimatedTripCount)) {
    LLVM_DEBUG(dbgs() << "getLoopEstimatedTripCount: "
                      << LLVMLoopEstimatedTripCount << " metadata has trip "
                      << "count of " << *TC
                      << (*TC == 0 ? " (returning std::nullopt)" : "")
                      << " for " << DbgLoop(L) << "\n");
    return *TC == 0 ? std::nullopt : TC;
  }

  // Estimate the trip count from latch branch weights.
  return estimateLoopTripCount(L);
}

bool llvm::setLoopEstimatedTripCount(
    Loop *L, unsigned EstimatedTripCount,
    std::optional<unsigned> EstimatedloopInvocationWeight) {
  // If EstimatedLoopInvocationWeight, we do not support this loop if
  // getExpectedExitLoopLatchBranch returns nullptr.
  //
  // FIXME: See comments in getLoopEstimatedTripCount for why this is required
  // here regardless of EstimatedLoopInvocationWeight.
  CondBrInst *LatchBranch = getExpectedExitLoopLatchBranch(L);
  if (!LatchBranch)
    return false;

  // Set the metadata.
  addStringMetadataToLoop(L, LLVMLoopEstimatedTripCount, EstimatedTripCount);

  // At the moment, we currently support changing the estimated trip count in
  // the latch branch's branch weights only.  We could extend this API to
  // manipulate estimated trip counts for any exit.
  //
  // TODO: Eventually, once all passes have migrated away from setting branch
  // weights to indicate estimated trip counts, we will not set branch weights
  // here at all.
  if (!EstimatedloopInvocationWeight)
    return true;

  // Calculate taken and exit weights.
  unsigned LatchExitWeight = ProfcheckDisableMetadataFixes ? 0 : 1;
  unsigned BackedgeTakenWeight = 0;

  if (EstimatedTripCount != 0) {
    LatchExitWeight = *EstimatedloopInvocationWeight;
    BackedgeTakenWeight = (EstimatedTripCount - 1) * LatchExitWeight;
  }

  // Make a swap if back edge is taken when condition is "false".
  if (LatchBranch->getSuccessor(0) != L->getHeader())
    std::swap(BackedgeTakenWeight, LatchExitWeight);

  // Set/Update profile metadata.
  setBranchWeights(*LatchBranch, {BackedgeTakenWeight, LatchExitWeight},
                   /*IsExpected=*/false);

  return true;
}

BranchProbability llvm::getLoopProbability(Loop *L) {
  CondBrInst *LatchBranch = getExpectedExitLoopLatchBranch(L);
  if (!LatchBranch)
    return BranchProbability::getUnknown();
  bool FirstTargetIsLoop = LatchBranch->getSuccessor(0) == L->getHeader();
  return getBranchProbability(LatchBranch, FirstTargetIsLoop);
}

bool llvm::setLoopProbability(Loop *L, BranchProbability P) {
  CondBrInst *LatchBranch = getExpectedExitLoopLatchBranch(L);
  if (!LatchBranch)
    return false;
  bool FirstTargetIsLoop = LatchBranch->getSuccessor(0) == L->getHeader();
  setBranchProbability(LatchBranch, P, FirstTargetIsLoop);
  return true;
}

BranchProbability llvm::getBranchProbability(CondBrInst *B,
                                             bool ForFirstTarget) {
  uint64_t Weight0, Weight1;
  if (!extractBranchWeights(*B, Weight0, Weight1))
    return BranchProbability::getUnknown();
  uint64_t Denominator = Weight0 + Weight1;
  if (Denominator == 0)
    return BranchProbability::getUnknown();
  if (!ForFirstTarget)
    std::swap(Weight0, Weight1);
  return BranchProbability::getBranchProbability(Weight0, Denominator);
}

BranchProbability llvm::getBranchProbability(BasicBlock *Src, BasicBlock *Dst) {
  assert(Src != Dst && "Passed in same source as destination");

  Instruction *TI = Src->getTerminator();
  if (!TI || TI->getNumSuccessors() == 0)
    return BranchProbability::getZero();

  SmallVector<uint32_t, 4> Weights;

  if (!extractBranchWeights(*TI, Weights)) {
    // No metadata
    return BranchProbability::getUnknown();
  }
  assert(TI->getNumSuccessors() == Weights.size() &&
         "Missing weights in branch_weights");

  uint64_t Total = 0;
  uint32_t Numerator = 0;
  for (auto [i, Weight] : llvm::enumerate(Weights)) {
    if (TI->getSuccessor(i) == Dst)
      Numerator += Weight;
    Total += Weight;
  }

  // Total of edges might be 0 if the metadata is incorrect/set by hand
  // or missing. In such case return here to avoid division by 0 later on.
  // There might also be a case where the value of Total cannot fit into
  // uint32_t, in such case, just bail out.
  if (Total == 0 || Total > std::numeric_limits<uint32_t>::max())
    return BranchProbability::getUnknown();

  return BranchProbability(Numerator, Total);
}

void llvm::setBranchProbability(CondBrInst *B, BranchProbability P,
                                bool ForFirstTarget) {
  BranchProbability Prob0 = P;
  BranchProbability Prob1 = P.getCompl();
  if (!ForFirstTarget)
    std::swap(Prob0, Prob1);
  setBranchWeights(*B, {Prob0.getNumerator(), Prob1.getNumerator()},
                   /*IsExpected=*/false);
}

bool llvm::hasIterationCountInvariantInParent(Loop *InnerLoop,
                                              ScalarEvolution &SE) {
  Loop *OuterL = InnerLoop->getParentLoop();
  if (!OuterL)
    return true;

  // Get the backedge taken count for the inner loop
  BasicBlock *InnerLoopLatch = InnerLoop->getLoopLatch();
  const SCEV *InnerLoopBECountSC = SE.getExitCount(InnerLoop, InnerLoopLatch);
  if (isa<SCEVCouldNotCompute>(InnerLoopBECountSC) ||
      !InnerLoopBECountSC->getType()->isIntegerTy())
    return false;

  // Get whether count is invariant to the outer loop
  ScalarEvolution::LoopDisposition LD =
      SE.getLoopDisposition(InnerLoopBECountSC, OuterL);
  if (LD != ScalarEvolution::LoopInvariant)
    return false;

  return true;
}

constexpr Intrinsic::ID llvm::getReductionIntrinsicID(RecurKind RK) {
  switch (RK) {
  default:
    llvm_unreachable("Unexpected recurrence kind");
  case RecurKind::AddChainWithSubs:
  case RecurKind::Sub:
  case RecurKind::Add:
    return Intrinsic::vector_reduce_add;
  case RecurKind::Mul:
    return Intrinsic::vector_reduce_mul;
  case RecurKind::And:
    return Intrinsic::vector_reduce_and;
  case RecurKind::Or:
    return Intrinsic::vector_reduce_or;
  case RecurKind::Xor:
    return Intrinsic::vector_reduce_xor;
  case RecurKind::FMulAdd:
  case RecurKind::FAddChainWithSubs:
  case RecurKind::FSub:
  case RecurKind::FAdd:
    return Intrinsic::vector_reduce_fadd;
  case RecurKind::FMul:
    return Intrinsic::vector_reduce_fmul;
  case RecurKind::SMax:
    return Intrinsic::vector_reduce_smax;
  case RecurKind::SMin:
    return Intrinsic::vector_reduce_smin;
  case RecurKind::UMax:
    return Intrinsic::vector_reduce_umax;
  case RecurKind::UMin:
    return Intrinsic::vector_reduce_umin;
  case RecurKind::FMax:
  case RecurKind::FMaxNum:
    return Intrinsic::vector_reduce_fmax;
  case RecurKind::FMin:
  case RecurKind::FMinNum:
    return Intrinsic::vector_reduce_fmin;
  case RecurKind::FMaximum:
    return Intrinsic::vector_reduce_fmaximum;
  case RecurKind::FMinimum:
    return Intrinsic::vector_reduce_fminimum;
  case RecurKind::FMaximumNum:
    return Intrinsic::vector_reduce_fmax;
  case RecurKind::FMinimumNum:
    return Intrinsic::vector_reduce_fmin;
  }
}

Intrinsic::ID llvm::getMinMaxReductionIntrinsicID(Intrinsic::ID IID) {
  switch (IID) {
  default:
    llvm_unreachable("Unexpected intrinsic id");
  case Intrinsic::umin:
    return Intrinsic::vector_reduce_umin;
  case Intrinsic::umax:
    return Intrinsic::vector_reduce_umax;
  case Intrinsic::smin:
    return Intrinsic::vector_reduce_smin;
  case Intrinsic::smax:
    return Intrinsic::vector_reduce_smax;
  }
}

// This is the inverse to getReductionForBinop
unsigned llvm::getArithmeticReductionInstruction(Intrinsic::ID RdxID) {
  switch (RdxID) {
  case Intrinsic::vector_reduce_fadd:
    return Instruction::FAdd;
  case Intrinsic::vector_reduce_fmul:
    return Instruction::FMul;
  case Intrinsic::vector_reduce_add:
    return Instruction::Add;
  case Intrinsic::vector_reduce_mul:
    return Instruction::Mul;
  case Intrinsic::vector_reduce_and:
    return Instruction::And;
  case Intrinsic::vector_reduce_or:
    return Instruction::Or;
  case Intrinsic::vector_reduce_xor:
    return Instruction::Xor;
  case Intrinsic::vector_reduce_smax:
  case Intrinsic::vector_reduce_smin:
  case Intrinsic::vector_reduce_umax:
  case Intrinsic::vector_reduce_umin:
    return Instruction::ICmp;
  case Intrinsic::vector_reduce_fmax:
  case Intrinsic::vector_reduce_fmin:
    return Instruction::FCmp;
  default:
    llvm_unreachable("Unexpected ID");
  }
}

// This is the inverse to getArithmeticReductionInstruction
Intrinsic::ID llvm::getReductionForBinop(Instruction::BinaryOps Opc) {
  switch (Opc) {
  default:
    break;
  case Instruction::Add:
    return Intrinsic::vector_reduce_add;
  case Instruction::Mul:
    return Intrinsic::vector_reduce_mul;
  case Instruction::And:
    return Intrinsic::vector_reduce_and;
  case Instruction::Or:
    return Intrinsic::vector_reduce_or;
  case Instruction::Xor:
    return Intrinsic::vector_reduce_xor;
  }
  return Intrinsic::not_intrinsic;
}

Intrinsic::ID llvm::getMinMaxReductionIntrinsicOp(Intrinsic::ID RdxID) {
  switch (RdxID) {
  default:
    llvm_unreachable("Unknown min/max recurrence kind");
  case Intrinsic::vector_reduce_umin:
    return Intrinsic::umin;
  case Intrinsic::vector_reduce_umax:
    return Intrinsic::umax;
  case Intrinsic::vector_reduce_smin:
    return Intrinsic::smin;
  case Intrinsic::vector_reduce_smax:
    return Intrinsic::smax;
  case Intrinsic::vector_reduce_fmin:
    return Intrinsic::minnum;
  case Intrinsic::vector_reduce_fmax:
    return Intrinsic::maxnum;
  case Intrinsic::vector_reduce_fminimum:
    return Intrinsic::minimum;
  case Intrinsic::vector_reduce_fmaximum:
    return Intrinsic::maximum;
  }
}

Intrinsic::ID llvm::getMinMaxReductionIntrinsicOp(RecurKind RK) {
  switch (RK) {
  default:
    llvm_unreachable("Unknown min/max recurrence kind");
  case RecurKind::UMin:
    return Intrinsic::umin;
  case RecurKind::UMax:
    return Intrinsic::umax;
  case RecurKind::SMin:
    return Intrinsic::smin;
  case RecurKind::SMax:
    return Intrinsic::smax;
  case RecurKind::FMin:
  case RecurKind::FMinNum:
    return Intrinsic::minnum;
  case RecurKind::FMax:
  case RecurKind::FMaxNum:
    return Intrinsic::maxnum;
  case RecurKind::FMinimum:
    return Intrinsic::minimum;
  case RecurKind::FMaximum:
    return Intrinsic::maximum;
  case RecurKind::FMinimumNum:
    return Intrinsic::minimumnum;
  case RecurKind::FMaximumNum:
    return Intrinsic::maximumnum;
  }
}

RecurKind llvm::getMinMaxReductionRecurKind(Intrinsic::ID RdxID) {
  switch (RdxID) {
  case Intrinsic::vector_reduce_smax:
    return RecurKind::SMax;
  case Intrinsic::vector_reduce_smin:
    return RecurKind::SMin;
  case Intrinsic::vector_reduce_umax:
    return RecurKind::UMax;
  case Intrinsic::vector_reduce_umin:
    return RecurKind::UMin;
  case Intrinsic::vector_reduce_fmax:
    return RecurKind::FMax;
  case Intrinsic::vector_reduce_fmin:
    return RecurKind::FMin;
  default:
    return RecurKind::None;
  }
}

CmpInst::Predicate llvm::getMinMaxReductionPredicate(RecurKind RK) {
  switch (RK) {
  default:
    llvm_unreachable("Unknown min/max recurrence kind");
  case RecurKind::UMin:
    return CmpInst::ICMP_ULT;
  case RecurKind::UMax:
    return CmpInst::ICMP_UGT;
  case RecurKind::SMin:
    return CmpInst::ICMP_SLT;
  case RecurKind::SMax:
    return CmpInst::ICMP_SGT;
  case RecurKind::FMin:
    return CmpInst::FCMP_OLT;
  case RecurKind::FMax:
    return CmpInst::FCMP_OGT;
  // We do not add FMinimum/FMaximum recurrence kind here since there is no
  // equivalent predicate which compares signed zeroes according to the
  // semantics of the intrinsics (llvm.minimum/maximum).
  }
}

Value *llvm::createMinMaxOp(IRBuilderBase &Builder, RecurKind RK, Value *Left,
                            Value *Right) {
  Type *Ty = Left->getType();
  if (Ty->isIntOrIntVectorTy() ||
      (RK == RecurKind::FMinNum || RK == RecurKind::FMaxNum ||
       RK == RecurKind::FMinimum || RK == RecurKind::FMaximum ||
       RK == RecurKind::FMinimumNum || RK == RecurKind::FMaximumNum)) {
    Intrinsic::ID Id = getMinMaxReductionIntrinsicOp(RK);
    return Builder.CreateIntrinsic(Ty, Id, {Left, Right}, nullptr,
                                   "rdx.minmax");
  }
  CmpInst::Predicate Pred = getMinMaxReductionPredicate(RK);
  Value *Cmp = Builder.CreateCmp(Pred, Left, Right, "rdx.minmax.cmp");
  Value *Select = Builder.CreateSelect(Cmp, Left, Right, "rdx.minmax.select");
  return Select;
}

// Helper to generate an ordered reduction.
Value *llvm::getOrderedReduction(IRBuilderBase &Builder, Value *Acc, Value *Src,
                                 unsigned Op, RecurKind RdxKind) {
  unsigned VF = cast<FixedVectorType>(Src->getType())->getNumElements();

  // Extract and apply reduction ops in ascending order:
  // e.g. ((((Acc + Scl[0]) + Scl[1]) + Scl[2]) + ) ... + Scl[VF-1]
  Value *Result = Acc;
  for (unsigned ExtractIdx = 0; ExtractIdx != VF; ++ExtractIdx) {
    Value *Ext =
        Builder.CreateExtractElement(Src, Builder.getInt32(ExtractIdx));

    if (Op != Instruction::ICmp && Op != Instruction::FCmp) {
      Result = Builder.CreateBinOp((Instruction::BinaryOps)Op, Result, Ext,
                                   "bin.rdx");
    } else {
      assert(RecurrenceDescriptor::isMinMaxRecurrenceKind(RdxKind) &&
             "Invalid min/max");
      Result = createMinMaxOp(Builder, RdxKind, Result, Ext);
    }
  }

  return Result;
}

Value *llvm::expandReductionViaLoop(IRBuilderBase &Builder, Value *Vec,
                                    unsigned RdxOpcode, Value *Acc,
                                    DominatorTree *DT, LoopInfo *LI) {
  auto *VTy = cast<VectorType>(Vec->getType());
  Type *EltTy = VTy->getElementType();
  Function *F = Builder.GetInsertBlock()->getParent();

  const DataLayout &DL = F->getDataLayout();
  Type *IdxTy = DL.getIndexType(EltTy->getContext(), 0);
  unsigned MinElts = VTy->getElementCount().getKnownMinValue();
  Value *NumElts = Builder.CreateVScale(IdxTy);
  NumElts = Builder.CreateMul(NumElts, ConstantInt::get(IdxTy, MinElts));

  BasicBlock *EntryBB = Builder.GetInsertBlock();
  BasicBlock *LoopBB = BasicBlock::Create(F->getContext(), "rdx.loop", F);
  BasicBlock *ExitBB = SplitBlock(EntryBB, Builder.GetInsertPoint(), DT, LI,
                                  nullptr, "rdx.exit");

  EntryBB->getTerminator()->eraseFromParent();
  Builder.SetInsertPoint(EntryBB);
  Builder.CreateBr(LoopBB);

  Builder.SetInsertPoint(LoopBB);
  PHINode *IV = Builder.CreatePHI(IdxTy, 2, "rdx.iv");
  PHINode *AccPhi = Builder.CreatePHI(EltTy, 2, "rdx.acc");
  IV->addIncoming(ConstantInt::get(IdxTy, 0), EntryBB);
  AccPhi->addIncoming(Acc, EntryBB);

  Value *Elt = Builder.CreateExtractElement(Vec, IV);
  Value *Res = Builder.CreateBinOp((Instruction::BinaryOps)RdxOpcode, AccPhi,
                                   Elt, "rdx.op");

  Value *NextIV =
      Builder.CreateNUWAdd(IV, ConstantInt::get(IdxTy, 1), "rdx.next");
  IV->addIncoming(NextIV, LoopBB);
  AccPhi->addIncoming(Res, LoopBB);

  Value *Done = Builder.CreateICmpEQ(NextIV, NumElts, "rdx.done");
  Builder.CreateCondBr(Done, ExitBB, LoopBB);

  // SplitBlock above updated DT/LI for EntryBB -> ExitBB. Now update
  // for replacing that edge with EntryBB -> LoopBB -> {ExitBB, LoopBB}.
  if (DT)
    DT->applyUpdates({{DominatorTree::Insert, EntryBB, LoopBB},
                      {DominatorTree::Insert, LoopBB, LoopBB},
                      {DominatorTree::Insert, LoopBB, ExitBB},
                      {DominatorTree::Delete, EntryBB, ExitBB}});

  if (LI) {
    Loop *NewLoop = LI->AllocateLoop();
    if (Loop *ParentLoop = LI->getLoopFor(EntryBB))
      ParentLoop->addChildLoop(NewLoop);
    else
      LI->addTopLevelLoop(NewLoop);
    NewLoop->addBasicBlockToLoop(LoopBB, *LI);
  }

  Builder.SetInsertPoint(ExitBB, ExitBB->begin());
  return Res;
}

// Helper to generate a log2 shuffle reduction.
Value *llvm::getShuffleReduction(IRBuilderBase &Builder, Value *Src,
                                 unsigned Op,
                                 TargetTransformInfo::ReductionShuffle RS,
                                 RecurKind RdxKind) {
  unsigned VF = cast<FixedVectorType>(Src->getType())->getNumElements();
  // VF is a power of 2 so we can emit the reduction using log2(VF) shuffles
  // and vector ops, reducing the set of values being computed by half each
  // round.
  assert(isPowerOf2_32(VF) &&
         "Reduction emission only supported for pow2 vectors!");
  // Note: fast-math-flags flags are controlled by the builder configuration
  // and are assumed to apply to all generated arithmetic instructions.  Other
  // poison generating flags (nsw/nuw/inbounds/inrange/exact) are not part
  // of the builder configuration, and since they're not passed explicitly,
  // will never be relevant here.  Note that it would be generally unsound to
  // propagate these from an intrinsic call to the expansion anyways as we/
  // change the order of operations.
  auto BuildShuffledOp = [&Builder, &Op,
                          &RdxKind](SmallVectorImpl<int> &ShuffleMask,
                                    Value *&TmpVec) -> void {
    Value *Shuf = Builder.CreateShuffleVector(TmpVec, ShuffleMask, "rdx.shuf");
    if (Op != Instruction::ICmp && Op != Instruction::FCmp) {
      TmpVec = Builder.CreateBinOp((Instruction::BinaryOps)Op, TmpVec, Shuf,
                                   "bin.rdx");
    } else {
      assert(RecurrenceDescriptor::isMinMaxRecurrenceKind(RdxKind) &&
             "Invalid min/max");
      TmpVec = createMinMaxOp(Builder, RdxKind, TmpVec, Shuf);
    }
  };

  Value *TmpVec = Src;
  if (TargetTransformInfo::ReductionShuffle::Pairwise == RS) {
    SmallVector<int, 32> ShuffleMask(VF);
    for (unsigned stride = 1; stride < VF; stride <<= 1) {
      // Initialise the mask with undef.
      llvm::fill(ShuffleMask, -1);
      for (unsigned j = 0; j < VF; j += stride << 1) {
        ShuffleMask[j] = j + stride;
      }
      BuildShuffledOp(ShuffleMask, TmpVec);
    }
  } else {
    SmallVector<int, 32> ShuffleMask(VF);
    for (unsigned i = VF; i != 1; i >>= 1) {
      // Move the upper half of the vector to the lower half.
      for (unsigned j = 0; j != i / 2; ++j)
        ShuffleMask[j] = i / 2 + j;

      // Fill the rest of the mask with undef.
      std::fill(&ShuffleMask[i / 2], ShuffleMask.end(), -1);
      BuildShuffledOp(ShuffleMask, TmpVec);
    }
  }
  // The result is in the first element of the vector.
  return Builder.CreateExtractElement(TmpVec, Builder.getInt32(0));
}

Value *llvm::createAnyOfReduction(IRBuilderBase &Builder, Value *Src,
                                  Value *InitVal, PHINode *OrigPhi) {
  Value *NewVal = nullptr;

  // First use the original phi to determine the new value we're trying to
  // select from in the loop.
  SelectInst *SI = nullptr;
  for (auto *U : OrigPhi->users()) {
    if ((SI = dyn_cast<SelectInst>(U)))
      break;
  }
  assert(SI && "One user of the original phi should be a select");

  if (SI->getTrueValue() == OrigPhi)
    NewVal = SI->getFalseValue();
  else {
    assert(SI->getFalseValue() == OrigPhi &&
           "At least one input to the select should be the original Phi");
    NewVal = SI->getTrueValue();
  }

  // If any predicate is true it means that we want to select the new value.
  Value *AnyOf =
      Src->getType()->isVectorTy() ? Builder.CreateOrReduce(Src) : Src;
  // The compares in the loop may yield poison, which propagates through the
  // bitwise ORs. Freeze it here before the condition is used.
  AnyOf = Builder.CreateFreeze(AnyOf);
  return Builder.CreateSelect(AnyOf, NewVal, InitVal, "rdx.select");
}

Value *llvm::getReductionIdentity(Intrinsic::ID RdxID, Type *Ty,
                                  FastMathFlags Flags) {
  bool Negative = false;
  switch (RdxID) {
  default:
    llvm_unreachable("Expecting a reduction intrinsic");
  case Intrinsic::vector_reduce_add:
  case Intrinsic::vector_reduce_mul:
  case Intrinsic::vector_reduce_or:
  case Intrinsic::vector_reduce_xor:
  case Intrinsic::vector_reduce_and:
  case Intrinsic::vector_reduce_fadd:
  case Intrinsic::vector_reduce_fmul: {
    unsigned Opc = getArithmeticReductionInstruction(RdxID);
    return ConstantExpr::getBinOpIdentity(Opc, Ty, false,
                                          Flags.noSignedZeros());
  }
  case Intrinsic::vector_reduce_umax:
  case Intrinsic::vector_reduce_umin:
  case Intrinsic::vector_reduce_smin:
  case Intrinsic::vector_reduce_smax: {
    Intrinsic::ID ScalarID = getMinMaxReductionIntrinsicOp(RdxID);
    return ConstantExpr::getIntrinsicIdentity(ScalarID, Ty);
  }
  case Intrinsic::vector_reduce_fmax:
  case Intrinsic::vector_reduce_fmaximum:
    Negative = true;
    [[fallthrough]];
  case Intrinsic::vector_reduce_fmin:
  case Intrinsic::vector_reduce_fminimum: {
    bool PropagatesNaN = RdxID == Intrinsic::vector_reduce_fminimum ||
                         RdxID == Intrinsic::vector_reduce_fmaximum;
    const fltSemantics &Semantics = Ty->getFltSemantics();
    return (!Flags.noNaNs() && !PropagatesNaN)
               ? ConstantFP::getQNaN(Ty, Negative)
           : !Flags.noInfs()
               ? ConstantFP::getInfinity(Ty, Negative)
               : ConstantFP::get(Ty, APFloat::getLargest(Semantics, Negative));
  }
  }
}

Value *llvm::getRecurrenceIdentity(RecurKind K, Type *Tp, FastMathFlags FMF) {
  assert((!(K == RecurKind::FMin || K == RecurKind::FMax) ||
          (FMF.noNaNs() && FMF.noSignedZeros())) &&
         "nnan, nsz is expected to be set for FP min/max reduction.");
  Intrinsic::ID RdxID = getReductionIntrinsicID(K);
  return getReductionIdentity(RdxID, Tp, FMF);
}

Value *llvm::createSimpleReduction(IRBuilderBase &Builder, Value *Src,
                                   RecurKind RdxKind) {
  auto *SrcVecEltTy = cast<VectorType>(Src->getType())->getElementType();
  auto getIdentity = [&]() {
    return getRecurrenceIdentity(RdxKind, SrcVecEltTy,
                                 Builder.getFastMathFlags());
  };
  switch (RdxKind) {
  case RecurKind::AddChainWithSubs:
  case RecurKind::Sub:
  case RecurKind::Add:
  case RecurKind::Mul:
  case RecurKind::And:
  case RecurKind::Or:
  case RecurKind::Xor:
  case RecurKind::SMax:
  case RecurKind::SMin:
  case RecurKind::UMax:
  case RecurKind::UMin:
  case RecurKind::FMax:
  case RecurKind::FMin:
  case RecurKind::FMinNum:
  case RecurKind::FMaxNum:
  case RecurKind::FMinimum:
  case RecurKind::FMaximum:
  case RecurKind::FMinimumNum:
  case RecurKind::FMaximumNum:
    return Builder.CreateUnaryIntrinsic(getReductionIntrinsicID(RdxKind), Src);
  case RecurKind::FMulAdd:
  case RecurKind::FAddChainWithSubs:
  case RecurKind::FSub:
  case RecurKind::FAdd:
    return Builder.CreateFAddReduce(getIdentity(), Src);
  case RecurKind::FMul:
    return Builder.CreateFMulReduce(getIdentity(), Src);
  default:
    llvm_unreachable("Unhandled opcode");
  }
}

Value *llvm::createSimpleReduction(IRBuilderBase &Builder, Value *Src,
                                   RecurKind Kind, Value *Mask, Value *EVL) {
  assert(!RecurrenceDescriptor::isAnyOfRecurrenceKind(Kind) &&
         !RecurrenceDescriptor::isFindRecurrenceKind(Kind) &&
         "AnyOf and FindIV reductions are not supported.");
  Intrinsic::ID Id = getReductionIntrinsicID(Kind);
  auto VPID = VPIntrinsic::getForIntrinsic(Id);
  assert(VPReductionIntrinsic::isVPReduction(VPID) &&
         "No VPIntrinsic for this reduction");
  auto *EltTy = cast<VectorType>(Src->getType())->getElementType();
  Value *Iden = getRecurrenceIdentity(Kind, EltTy, Builder.getFastMathFlags());
  Value *Ops[] = {Iden, Src, Mask, EVL};
  return Builder.CreateIntrinsic(EltTy, VPID, Ops);
}

Value *llvm::createOrderedReduction(IRBuilderBase &B, RecurKind Kind,
                                    Value *Src, Value *Start) {
  assert((Kind == RecurKind::FAdd || Kind == RecurKind::FMulAdd) &&
         "Unexpected reduction kind");
  assert(Src->getType()->isVectorTy() && "Expected a vector type");
  assert(!Start->getType()->isVectorTy() && "Expected a scalar type");

  return B.CreateFAddReduce(Start, Src);
}

Value *llvm::createOrderedReduction(IRBuilderBase &Builder, RecurKind Kind,
                                    Value *Src, Value *Start, Value *Mask,
                                    Value *EVL) {
  assert((Kind == RecurKind::FAdd || Kind == RecurKind::FMulAdd) &&
         "Unexpected reduction kind");
  assert(Src->getType()->isVectorTy() && "Expected a vector type");
  assert(!Start->getType()->isVectorTy() && "Expected a scalar type");

  Intrinsic::ID Id = getReductionIntrinsicID(RecurKind::FAdd);
  auto VPID = VPIntrinsic::getForIntrinsic(Id);
  assert(VPReductionIntrinsic::isVPReduction(VPID) &&
         "No VPIntrinsic for this reduction");
  auto *EltTy = cast<VectorType>(Src->getType())->getElementType();
  Value *Ops[] = {Start, Src, Mask, EVL};
  return Builder.CreateIntrinsic(EltTy, VPID, Ops);
}

void llvm::propagateIRFlags(Value *I, ArrayRef<Value *> VL, Value *OpValue,
                            bool IncludeWrapFlags) {
  auto *VecOp = dyn_cast<Instruction>(I);
  if (!VecOp)
    return;
  auto *Intersection = (OpValue == nullptr) ? dyn_cast<Instruction>(VL[0])
                                            : dyn_cast<Instruction>(OpValue);
  if (!Intersection)
    return;
  const unsigned Opcode = Intersection->getOpcode();
  VecOp->copyIRFlags(Intersection, IncludeWrapFlags);
  for (auto *V : VL) {
    auto *Instr = dyn_cast<Instruction>(V);
    if (!Instr)
      continue;
    if (OpValue == nullptr || Opcode == Instr->getOpcode())
      VecOp->andIRFlags(V);
  }
}

bool llvm::isKnownNegativeInLoop(const SCEV *S, const Loop *L,
                                 ScalarEvolution &SE) {
  const SCEV *Zero = SE.getZero(S->getType());
  return SE.isAvailableAtLoopEntry(S, L) &&
         SE.isLoopEntryGuardedByCond(L, ICmpInst::ICMP_SLT, S, Zero);
}

bool llvm::isKnownNonNegativeInLoop(const SCEV *S, const Loop *L,
                                    ScalarEvolution &SE) {
  const SCEV *Zero = SE.getZero(S->getType());
  return SE.isAvailableAtLoopEntry(S, L) &&
         SE.isLoopEntryGuardedByCond(L, ICmpInst::ICMP_SGE, S, Zero);
}

bool llvm::isKnownPositiveInLoop(const SCEV *S, const Loop *L,
                                 ScalarEvolution &SE) {
  const SCEV *Zero = SE.getZero(S->getType());
  return SE.isAvailableAtLoopEntry(S, L) &&
         SE.isLoopEntryGuardedByCond(L, ICmpInst::ICMP_SGT, S, Zero);
}

bool llvm::isKnownNonPositiveInLoop(const SCEV *S, const Loop *L,
                                    ScalarEvolution &SE) {
  const SCEV *Zero = SE.getZero(S->getType());
  return SE.isAvailableAtLoopEntry(S, L) &&
         SE.isLoopEntryGuardedByCond(L, ICmpInst::ICMP_SLE, S, Zero);
}

bool llvm::cannotBeMinInLoop(const SCEV *S, const Loop *L, ScalarEvolution &SE,
                             bool Signed) {
  unsigned BitWidth = cast<IntegerType>(S->getType())->getBitWidth();
  APInt Min = Signed ? APInt::getSignedMinValue(BitWidth) :
    APInt::getMinValue(BitWidth);
  auto Predicate = Signed ? ICmpInst::ICMP_SGT : ICmpInst::ICMP_UGT;
  return SE.isAvailableAtLoopEntry(S, L) &&
         SE.isLoopEntryGuardedByCond(L, Predicate, S,
                                     SE.getConstant(Min));
}

bool llvm::cannotBeMaxInLoop(const SCEV *S, const Loop *L, ScalarEvolution &SE,
                             bool Signed) {
  unsigned BitWidth = cast<IntegerType>(S->getType())->getBitWidth();
  APInt Max = Signed ? APInt::getSignedMaxValue(BitWidth) :
    APInt::getMaxValue(BitWidth);
  auto Predicate = Signed ? ICmpInst::ICMP_SLT : ICmpInst::ICMP_ULT;
  return SE.isAvailableAtLoopEntry(S, L) &&
         SE.isLoopEntryGuardedByCond(L, Predicate, S,
                                     SE.getConstant(Max));
}

//===----------------------------------------------------------------------===//
// rewriteLoopExitValues - Optimize IV users outside the loop.
// As a side effect, reduces the amount of IV processing within the loop.
//===----------------------------------------------------------------------===//

static bool hasHardUserWithinLoop(const Loop *L, const Instruction *I) {
  SmallPtrSet<const Instruction *, 8> Visited;
  SmallVector<const Instruction *, 8> WorkList;
  Visited.insert(I);
  WorkList.push_back(I);
  while (!WorkList.empty()) {
    const Instruction *Curr = WorkList.pop_back_val();
    // This use is outside the loop, nothing to do.
    if (!L->contains(Curr))
      continue;
    // Do we assume it is a "hard" use which will not be eliminated easily?
    if (Curr->mayHaveSideEffects())
      return true;
    // Otherwise, add all its users to worklist.
    for (const auto *U : Curr->users()) {
      auto *UI = cast<Instruction>(U);
      if (Visited.insert(UI).second)
        WorkList.push_back(UI);
    }
  }
  return false;
}

// Collect information about PHI nodes which can be transformed in
// rewriteLoopExitValues.
struct RewritePhi {
  PHINode *PN;               // For which PHI node is this replacement?
  unsigned Ith;              // For which incoming value?
  const SCEV *ExpansionSCEV; // The SCEV of the incoming value we are rewriting.
  Instruction *ExpansionPoint; // Where we'd like to expand that SCEV?
  bool HighCost;               // Is this expansion a high-cost?

  RewritePhi(PHINode *P, unsigned I, const SCEV *Val, Instruction *ExpansionPt,
             bool H)
      : PN(P), Ith(I), ExpansionSCEV(Val), ExpansionPoint(ExpansionPt),
        HighCost(H) {}
};

// Check whether it is possible to delete the loop after rewriting exit
// value. If it is possible, ignore ReplaceExitValue and do rewriting
// aggressively.
static bool canLoopBeDeleted(Loop *L, SmallVector<RewritePhi, 8> &RewritePhiSet) {
  BasicBlock *Preheader = L->getLoopPreheader();
  // If there is no preheader, the loop will not be deleted.
  if (!Preheader)
    return false;

  // In LoopDeletion pass Loop can be deleted when ExitingBlocks.size() > 1.
  // We obviate multiple ExitingBlocks case for simplicity.
  // TODO: If we see testcase with multiple ExitingBlocks can be deleted
  // after exit value rewriting, we can enhance the logic here.
  SmallVector<BasicBlock *, 4> ExitingBlocks;
  L->getExitingBlocks(ExitingBlocks);
  SmallVector<BasicBlock *, 8> ExitBlocks;
  L->getUniqueExitBlocks(ExitBlocks);
  if (ExitBlocks.size() != 1 || ExitingBlocks.size() != 1)
    return false;

  BasicBlock *ExitBlock = ExitBlocks[0];
  BasicBlock::iterator BI = ExitBlock->begin();
  while (PHINode *P = dyn_cast<PHINode>(BI)) {
    Value *Incoming = P->getIncomingValueForBlock(ExitingBlocks[0]);

    // If the Incoming value of P is found in RewritePhiSet, we know it
    // could be rewritten to use a loop invariant value in transformation
    // phase later. Skip it in the loop invariant check below.
    bool found = false;
    for (const RewritePhi &Phi : RewritePhiSet) {
      unsigned i = Phi.Ith;
      if (Phi.PN == P && (Phi.PN)->getIncomingValue(i) == Incoming) {
        found = true;
        break;
      }
    }

    Instruction *I;
    if (!found && (I = dyn_cast<Instruction>(Incoming)))
      if (!L->hasLoopInvariantOperands(I))
        return false;

    ++BI;
  }

  for (auto *BB : L->blocks())
    if (llvm::any_of(*BB, [](Instruction &I) {
          return I.mayHaveSideEffects();
        }))
      return false;

  return true;
}

/// Checks if it is safe to call InductionDescriptor::isInductionPHI for \p Phi,
/// and returns true if this Phi is an induction phi in the loop. When
/// isInductionPHI returns true, \p ID will be also be set by isInductionPHI.
static bool checkIsIndPhi(PHINode *Phi, Loop *L, ScalarEvolution *SE,
                          InductionDescriptor &ID) {
  if (!Phi)
    return false;
  if (!L->getLoopPreheader())
    return false;
  if (Phi->getParent() != L->getHeader())
    return false;
  return InductionDescriptor::isInductionPHI(Phi, L, SE, ID);
}

int llvm::rewriteLoopExitValues(Loop *L, LoopInfo *LI, TargetLibraryInfo *TLI,
                                ScalarEvolution *SE,
                                const TargetTransformInfo *TTI,
                                SCEVExpander &Rewriter, DominatorTree *DT,
                                ReplaceExitVal ReplaceExitValue,
                                SmallVector<WeakTrackingVH, 16> &DeadInsts) {
  // Check a pre-condition.
  assert(L->isRecursivelyLCSSAForm(*DT, *LI) &&
         "Caller did not preserve LCSSA!");

  SmallVector<BasicBlock*, 8> ExitBlocks;
  L->getUniqueExitBlocks(ExitBlocks);

  SmallVector<RewritePhi, 8> RewritePhiSet;
  // Find all values that are computed inside the loop, but used outside of it.
  // Because of LCSSA, these values will only occur in LCSSA PHI Nodes.  Scan
  // the exit blocks of the loop to find them.
  for (BasicBlock *ExitBB : ExitBlocks) {
    // If there are no PHI nodes in this exit block, then no values defined
    // inside the loop are used on this path, skip it.
    PHINode *PN = dyn_cast<PHINode>(ExitBB->begin());
    if (!PN) continue;

    unsigned NumPreds = PN->getNumIncomingValues();

    // Iterate over all of the PHI nodes.
    BasicBlock::iterator BBI = ExitBB->begin();
    while ((PN = dyn_cast<PHINode>(BBI++))) {
      if (PN->use_empty())
        continue; // dead use, don't replace it

      if (!SE->isSCEVable(PN->getType()))
        continue;

      // Iterate over all of the values in all the PHI nodes.
      for (unsigned i = 0; i != NumPreds; ++i) {
        // If the value being merged in is not integer or is not defined
        // in the loop, skip it.
        Value *InVal = PN->getIncomingValue(i);
        if (!isa<Instruction>(InVal))
          continue;

        // If this pred is for a subloop, not L itself, skip it.
        if (LI->getLoopFor(PN->getIncomingBlock(i)) != L)
          continue; // The Block is in a subloop, skip it.

        // Check that InVal is defined in the loop.
        Instruction *Inst = cast<Instruction>(InVal);
        if (!L->contains(Inst))
          continue;

        // Find exit values which are induction variables in the loop, and are
        // unused in the loop, with the only use being the exit block PhiNode,
        // and the induction variable update binary operator.
        // The exit value can be replaced with the final value when it is cheap
        // to do so.
        if (ReplaceExitValue == UnusedIndVarInLoop) {
          InductionDescriptor ID;
          PHINode *IndPhi = dyn_cast<PHINode>(Inst);
          if (IndPhi) {
            if (!checkIsIndPhi(IndPhi, L, SE, ID))
              continue;
            // This is an induction PHI. Check that the only users are PHI
            // nodes, and induction variable update binary operators.
            if (llvm::any_of(Inst->users(), [&](User *U) {
                  if (!isa<PHINode>(U) && !isa<BinaryOperator>(U))
                    return true;
                  BinaryOperator *B = dyn_cast<BinaryOperator>(U);
                  if (B && B != ID.getInductionBinOp())
                    return true;
                  return false;
                }))
              continue;
          } else {
            // If it is not an induction phi, it must be an induction update
            // binary operator with an induction phi user.
            BinaryOperator *B = dyn_cast<BinaryOperator>(Inst);
            if (!B)
              continue;
            if (llvm::any_of(Inst->users(), [&](User *U) {
                  PHINode *Phi = dyn_cast<PHINode>(U);
                  if (Phi != PN && !checkIsIndPhi(Phi, L, SE, ID))
                    return true;
                  return false;
                }))
              continue;
            if (B != ID.getInductionBinOp())
              continue;
          }
        }

        // Okay, this instruction has a user outside of the current loop
        // and varies predictably *inside* the loop.  Evaluate the value it
        // contains when the loop exits, if possible.  We prefer to start with
        // expressions which are true for all exits (so as to maximize
        // expression reuse by the SCEVExpander), but resort to per-exit
        // evaluation if that fails.
        const SCEV *ExitValue = SE->getSCEVAtScope(Inst, L->getParentLoop());
        if (isa<SCEVCouldNotCompute>(ExitValue) ||
            !SE->isLoopInvariant(ExitValue, L) ||
            !Rewriter.isSafeToExpand(ExitValue)) {
          // TODO: This should probably be sunk into SCEV in some way; maybe a
          // getSCEVForExit(SCEV*, L, ExitingBB)?  It can be generalized for
          // most SCEV expressions and other recurrence types (e.g. shift
          // recurrences).  Is there existing code we can reuse?
          const SCEV *ExitCount = SE->getExitCount(L, PN->getIncomingBlock(i));
          if (isa<SCEVCouldNotCompute>(ExitCount))
            continue;
          if (auto *AddRec = dyn_cast<SCEVAddRecExpr>(SE->getSCEV(Inst)))
            if (AddRec->getLoop() == L)
              ExitValue = AddRec->evaluateAtIteration(ExitCount, *SE);
          if (isa<SCEVCouldNotCompute>(ExitValue) ||
              !SE->isLoopInvariant(ExitValue, L) ||
              !Rewriter.isSafeToExpand(ExitValue))
            continue;
        }

        // Computing the value outside of the loop brings no benefit if it is
        // definitely used inside the loop in a way which can not be optimized
        // away. Avoid doing so unless we know we have a value which computes
        // the ExitValue already. TODO: This should be merged into SCEV
        // expander to leverage its knowledge of existing expressions.
        if (ReplaceExitValue != AlwaysRepl && !isa<SCEVConstant>(ExitValue) &&
            !isa<SCEVUnknown>(ExitValue) && hasHardUserWithinLoop(L, Inst))
          continue;

        // Check if expansions of this SCEV would count as being high cost.
        bool HighCost = Rewriter.isHighCostExpansion(
            ExitValue, L, SCEVCheapExpansionBudget, TTI, Inst);

        // Note that we must not perform expansions until after
        // we query *all* the costs, because if we perform temporary expansion
        // inbetween, one that we might not intend to keep, said expansion
        // *may* affect cost calculation of the next SCEV's we'll query,
        // and next SCEV may errneously get smaller cost.

        // Collect all the candidate PHINodes to be rewritten.
        Instruction *InsertPt =
          (isa<PHINode>(Inst) || isa<LandingPadInst>(Inst)) ?
          &*Inst->getParent()->getFirstInsertionPt() : Inst;
        RewritePhiSet.emplace_back(PN, i, ExitValue, InsertPt, HighCost);
      }
    }
  }

  // TODO: evaluate whether it is beneficial to change how we calculate
  // high-cost: if we have SCEV 'A' which we know we will expand, should we
  // calculate the cost of other SCEV's after expanding SCEV 'A', thus
  // potentially giving cost bonus to those other SCEV's?

  bool LoopCanBeDel = canLoopBeDeleted(L, RewritePhiSet);
  int NumReplaced = 0;

  // Transformation.
  for (const RewritePhi &Phi : RewritePhiSet) {
    PHINode *PN = Phi.PN;

    // Only do the rewrite when the ExitValue can be expanded cheaply.
    // If LoopCanBeDel is true, rewrite exit value aggressively.
    if ((ReplaceExitValue == OnlyCheapRepl ||
         ReplaceExitValue == UnusedIndVarInLoop) &&
        !LoopCanBeDel && Phi.HighCost)
      continue;

    Value *ExitVal = Rewriter.expandCodeFor(
        Phi.ExpansionSCEV, Phi.PN->getType(), Phi.ExpansionPoint);

    LLVM_DEBUG(dbgs() << "rewriteLoopExitValues: AfterLoopVal = " << *ExitVal
                      << '\n'
                      << "  LoopVal = " << *(Phi.ExpansionPoint) << "\n");

#ifndef NDEBUG
    // If we reuse an instruction from a loop which is neither L nor one of
    // its containing loops, we end up breaking LCSSA form for this loop by
    // creating a new use of its instruction.
    if (auto *ExitInsn = dyn_cast<Instruction>(ExitVal))
      if (auto *EVL = LI->getLoopFor(ExitInsn->getParent()))
        if (EVL != L)
          assert(EVL->contains(L) && "LCSSA breach detected!");
#endif

    NumReplaced++;
    Instruction *Inst = cast<Instruction>(PN->getIncomingValue(Phi.Ith));
    PN->setIncomingValue(Phi.Ith, ExitVal);
    // It's necessary to tell ScalarEvolution about this explicitly so that
    // it can walk the def-use list and forget all SCEVs, as it may not be
    // watching the PHI itself. Once the new exit value is in place, there
    // may not be a def-use connection between the loop and every instruction
    // which got a SCEVAddRecExpr for that loop.
    SE->forgetValue(PN);

    // If this instruction is dead now, delete it. Don't do it now to avoid
    // invalidating iterators.
    if (isInstructionTriviallyDead(Inst, TLI))
      DeadInsts.push_back(Inst);

    // Replace PN with ExitVal if that is legal and does not break LCSSA.
    if (PN->getNumIncomingValues() == 1 &&
        LI->replacementPreservesLCSSAForm(PN, ExitVal)) {
      PN->replaceAllUsesWith(ExitVal);
      PN->eraseFromParent();
    }
  }

  // The insertion point instruction may have been deleted; clear it out
  // so that the rewriter doesn't trip over it later.
  Rewriter.clearInsertPoint();
  return NumReplaced;
}

/// Utility that implements appending of loops onto a worklist.
/// Loops are added in preorder (analogous for reverse postorder for trees),
/// and the worklist is processed LIFO.
template <typename RangeT>
void llvm::appendReversedLoopsToWorklist(
    RangeT &&Loops, SmallPriorityWorklist<Loop *, 4> &Worklist) {
  // We use an internal worklist to build up the preorder traversal without
  // recursion.
  SmallVector<Loop *, 4> PreOrderLoops, PreOrderWorklist;

  // We walk the initial sequence of loops in reverse because we generally want
  // to visit defs before uses and the worklist is LIFO.
  for (Loop *RootL : Loops) {
    assert(PreOrderLoops.empty() && "Must start with an empty preorder walk.");
    assert(PreOrderWorklist.empty() &&
           "Must start with an empty preorder walk worklist.");
    PreOrderWorklist.push_back(RootL);
    do {
      Loop *L = PreOrderWorklist.pop_back_val();
      PreOrderWorklist.append(L->begin(), L->end());
      PreOrderLoops.push_back(L);
    } while (!PreOrderWorklist.empty());

    Worklist.insert(std::move(PreOrderLoops));
    PreOrderLoops.clear();
  }
}

template <typename RangeT>
void llvm::appendLoopsToWorklist(RangeT &&Loops,
                                 SmallPriorityWorklist<Loop *, 4> &Worklist) {
  appendReversedLoopsToWorklist(reverse(Loops), Worklist);
}

template LLVM_EXPORT_TEMPLATE void
llvm::appendLoopsToWorklist<ArrayRef<Loop *> &>(
    ArrayRef<Loop *> &Loops, SmallPriorityWorklist<Loop *, 4> &Worklist);

template LLVM_EXPORT_TEMPLATE void
llvm::appendLoopsToWorklist<Loop &>(Loop &L,
                                    SmallPriorityWorklist<Loop *, 4> &Worklist);

void llvm::appendLoopsToWorklist(LoopInfo &LI,
                                 SmallPriorityWorklist<Loop *, 4> &Worklist) {
  appendReversedLoopsToWorklist(LI, Worklist);
}

Loop *llvm::cloneLoop(Loop *L, Loop *PL, ValueToValueMapTy &VM,
                      LoopInfo *LI, LPPassManager *LPM) {
  Loop &New = *LI->AllocateLoop();
  if (PL)
    PL->addChildLoop(&New);
  else
    LI->addTopLevelLoop(&New);

  if (LPM)
    LPM->addLoop(New);

  // Add all of the blocks in L to the new loop.
  for (BasicBlock *BB : L->blocks())
    if (LI->getLoopFor(BB) == L)
      New.addBasicBlockToLoop(cast<BasicBlock>(VM[BB]), *LI);

  // Add all of the subloops to the new loop.
  for (Loop *I : *L)
    cloneLoop(I, &New, VM, LI, LPM);

  return &New;
}

/// IR Values for the lower and upper bounds of a pointer evolution.  We
/// need to use value-handles because SCEV expansion can invalidate previously
/// expanded values.  Thus expansion of a pointer can invalidate the bounds for
/// a previous one.
struct PointerBounds {
  TrackingVH<Value> Start;
  TrackingVH<Value> End;
  Value *StrideToCheck;
};

/// Expand code for the lower and upper bound of the pointer group \p CG
/// in \p TheLoop.  \return the values for the bounds.
static PointerBounds expandBounds(const RuntimeCheckingPtrGroup *CG,
                                  Loop *TheLoop, Instruction *Loc,
                                  SCEVExpander &Exp, bool HoistRuntimeChecks) {
  LLVMContext &Ctx = Loc->getContext();
  Type *PtrArithTy = PointerType::get(Ctx, CG->AddressSpace);

  Value *Start = nullptr, *End = nullptr;
  LLVM_DEBUG(dbgs() << "LAA: Adding RT check for range:\n");
  const SCEV *Low = CG->Low, *High = CG->High, *Stride = nullptr;

  // If the Low and High values are themselves loop-variant, then we may want
  // to expand the range to include those covered by the outer loop as well.
  // There is a trade-off here with the advantage being that creating checks
  // using the expanded range permits the runtime memory checks to be hoisted
  // out of the outer loop. This reduces the cost of entering the inner loop,
  // which can be significant for low trip counts. The disadvantage is that
  // there is a chance we may now never enter the vectorized inner loop,
  // whereas using a restricted range check could have allowed us to enter at
  // least once. This is why the behaviour is not currently the default and is
  // controlled by the parameter 'HoistRuntimeChecks'.
  if (HoistRuntimeChecks && TheLoop->getParentLoop() &&
      isa<SCEVAddRecExpr>(High) && isa<SCEVAddRecExpr>(Low)) {
    auto *HighAR = cast<SCEVAddRecExpr>(High);
    auto *LowAR = cast<SCEVAddRecExpr>(Low);
    const Loop *OuterLoop = TheLoop->getParentLoop();
    ScalarEvolution &SE = *Exp.getSE();
    const SCEV *Recur = LowAR->getStepRecurrence(SE);
    if (Recur == HighAR->getStepRecurrence(SE) &&
        HighAR->getLoop() == OuterLoop && LowAR->getLoop() == OuterLoop) {
      BasicBlock *OuterLoopLatch = OuterLoop->getLoopLatch();
      const SCEV *OuterExitCount = SE.getExitCount(OuterLoop, OuterLoopLatch);
      if (!isa<SCEVCouldNotCompute>(OuterExitCount) &&
          OuterExitCount->getType()->isIntegerTy()) {
        const SCEV *NewHigh =
            cast<SCEVAddRecExpr>(High)->evaluateAtIteration(OuterExitCount, SE);
        if (!isa<SCEVCouldNotCompute>(NewHigh)) {
          LLVM_DEBUG(dbgs() << "LAA: Expanded RT check for range to include "
                               "outer loop in order to permit hoisting\n");
          High = NewHigh;
          Low = cast<SCEVAddRecExpr>(Low)->getStart();
          // If there is a possibility that the stride is negative then we have
          // to generate extra checks to ensure the stride is positive.
          if (!SE.isKnownNonNegative(
                  SE.applyLoopGuards(Recur, HighAR->getLoop()))) {
            Stride = Recur;
            LLVM_DEBUG(dbgs() << "LAA: ... but need to check stride is "
                                 "positive: "
                              << *Stride << '\n');
          }
        }
      }
    }
  }

  Start = Exp.expandCodeFor(Low, PtrArithTy, Loc);
  End = Exp.expandCodeFor(High, PtrArithTy, Loc);
  if (CG->NeedsFreeze) {
    IRBuilder<> Builder(Loc);
    Start = Builder.CreateFreeze(Start, Start->getName() + ".fr");
    End = Builder.CreateFreeze(End, End->getName() + ".fr");
  }
  Value *StrideVal =
      Stride ? Exp.expandCodeFor(Stride, Stride->getType(), Loc) : nullptr;
  LLVM_DEBUG(dbgs() << "Start: " << *Low << " End: " << *High << "\n");
  return {Start, End, StrideVal};
}

/// Turns a collection of checks into a collection of expanded upper and
/// lower bounds for both pointers in the check.
static SmallVector<std::pair<PointerBounds, PointerBounds>, 4>
expandBounds(const SmallVectorImpl<RuntimePointerCheck> &PointerChecks, Loop *L,
             Instruction *Loc, SCEVExpander &Exp, bool HoistRuntimeChecks) {
  SmallVector<std::pair<PointerBounds, PointerBounds>, 4> ChecksWithBounds;

  // Here we're relying on the SCEV Expander's cache to only emit code for the
  // same bounds once.
  transform(PointerChecks, std::back_inserter(ChecksWithBounds),
            [&](const RuntimePointerCheck &Check) {
              PointerBounds First = expandBounds(Check.first, L, Loc, Exp,
                                                 HoistRuntimeChecks),
                            Second = expandBounds(Check.second, L, Loc, Exp,
                                                  HoistRuntimeChecks);
              return std::make_pair(First, Second);
            });

  return ChecksWithBounds;
}

Value *llvm::addRuntimeChecks(
    Instruction *Loc, Loop *TheLoop,
    const SmallVectorImpl<RuntimePointerCheck> &PointerChecks,
    SCEVExpander &Exp, bool HoistRuntimeChecks) {
  // TODO: Move noalias annotation code from LoopVersioning here and share with LV if possible.
  // TODO: Pass  RtPtrChecking instead of PointerChecks and SE separately, if possible
  auto ExpandedChecks =
      expandBounds(PointerChecks, TheLoop, Loc, Exp, HoistRuntimeChecks);

  LLVMContext &Ctx = Loc->getContext();
  IRBuilder ChkBuilder(Ctx, InstSimplifyFolder(Loc->getDataLayout()));
  ChkBuilder.SetInsertPoint(Loc);
  // Our instructions might fold to a constant.
  Value *MemoryRuntimeCheck = nullptr;

  for (const auto &[A, B] : ExpandedChecks) {
    // Check if two pointers (A and B) conflict where conflict is computed as:
    // start(A) <= end(B) && start(B) <= end(A)

    assert((A.Start->getType()->getPointerAddressSpace() ==
            B.End->getType()->getPointerAddressSpace()) &&
           (B.Start->getType()->getPointerAddressSpace() ==
            A.End->getType()->getPointerAddressSpace()) &&
           "Trying to bounds check pointers with different address spaces");

    // [A|B].Start points to the first accessed byte under base [A|B].
    // [A|B].End points to the last accessed byte, plus one.
    // There is no conflict when the intervals are disjoint:
    // NoConflict = (B.Start >= A.End) || (A.Start >= B.End)
    //
    // bound0 = (B.Start < A.End)
    // bound1 = (A.Start < B.End)
    //  IsConflict = bound0 & bound1
    Value *Cmp0 = ChkBuilder.CreateICmpULT(A.Start, B.End, "bound0");
    Value *Cmp1 = ChkBuilder.CreateICmpULT(B.Start, A.End, "bound1");
    Value *IsConflict = ChkBuilder.CreateAnd(Cmp0, Cmp1, "found.conflict");
    if (A.StrideToCheck) {
      Value *IsNegativeStride = ChkBuilder.CreateICmpSLT(
          A.StrideToCheck, ConstantInt::get(A.StrideToCheck->getType(), 0),
          "stride.check");
      IsConflict = ChkBuilder.CreateOr(IsConflict, IsNegativeStride);
    }
    if (B.StrideToCheck) {
      Value *IsNegativeStride = ChkBuilder.CreateICmpSLT(
          B.StrideToCheck, ConstantInt::get(B.StrideToCheck->getType(), 0),
          "stride.check");
      IsConflict = ChkBuilder.CreateOr(IsConflict, IsNegativeStride);
    }
    if (MemoryRuntimeCheck) {
      IsConflict =
          ChkBuilder.CreateOr(MemoryRuntimeCheck, IsConflict, "conflict.rdx");
    }
    MemoryRuntimeCheck = IsConflict;
  }

  Exp.eraseDeadInstructions(MemoryRuntimeCheck);
  return MemoryRuntimeCheck;
}

namespace {
/// Rewriter to replace SCEVPtrToIntExpr with SCEVPtrToAddrExpr when the result
/// type matches the pointer address type. This allows expressions mixing
/// ptrtoint and ptrtoaddr to simplify properly.
struct SCEVPtrToAddrRewriter : SCEVRewriteVisitor<SCEVPtrToAddrRewriter> {
  const DataLayout &DL;
  SCEVPtrToAddrRewriter(ScalarEvolution &SE, const DataLayout &DL)
      : SCEVRewriteVisitor(SE), DL(DL) {}

  const SCEV *visitPtrToIntExpr(const SCEVPtrToIntExpr *E) {
    const SCEV *Op = visit(E->getOperand());
    if (E->getType() == DL.getAddressType(E->getOperand()->getType()))
      return SE.getPtrToAddrExpr(Op);
    return Op == E->getOperand() ? E : SE.getPtrToIntExpr(Op, E->getType());
  }
};
} // namespace

Value *llvm::addDiffRuntimeChecks(
    Instruction *Loc, ArrayRef<PointerDiffInfo> Checks, SCEVExpander &Expander,
    function_ref<Value *(IRBuilderBase &, unsigned)> GetVF, unsigned IC) {

  LLVMContext &Ctx = Loc->getContext();
  IRBuilder ChkBuilder(Ctx, InstSimplifyFolder(Loc->getDataLayout()));
  ChkBuilder.SetInsertPoint(Loc);
  // Our instructions might fold to a constant.
  Value *MemoryRuntimeCheck = nullptr;

  auto &SE = *Expander.getSE();
  const DataLayout &DL = Loc->getDataLayout();
  SCEVPtrToAddrRewriter Rewriter(SE, DL);
  // Map to keep track of created compares, The key is the pair of operands for
  // the compare, to allow detecting and re-using redundant compares.
  DenseMap<std::pair<Value *, Value *>, Value *> SeenCompares;
  for (const auto &[SrcStart, SinkStart, AccessSize, NeedsFreeze] : Checks) {
    Type *Ty = SinkStart->getType();
    // Compute VF * IC * AccessSize.
    auto *VFTimesICTimesSize =
        ChkBuilder.CreateMul(GetVF(ChkBuilder, Ty->getScalarSizeInBits()),
                             ConstantInt::get(Ty, IC * AccessSize));
    const SCEV *SinkStartRewritten = Rewriter.visit(SinkStart);
    const SCEV *SrcStartRewritten = Rewriter.visit(SrcStart);
    Value *Diff = Expander.expandCodeFor(
        SE.getMinusSCEV(SinkStartRewritten, SrcStartRewritten), Ty, Loc);

    // Check if the same compare has already been created earlier. In that case,
    // there is no need to check it again.
    Value *IsConflict = SeenCompares.lookup({Diff, VFTimesICTimesSize});
    if (IsConflict)
      continue;

    IsConflict =
        ChkBuilder.CreateICmpULT(Diff, VFTimesICTimesSize, "diff.check");
    SeenCompares.insert({{Diff, VFTimesICTimesSize}, IsConflict});
    if (NeedsFreeze)
      IsConflict =
          ChkBuilder.CreateFreeze(IsConflict, IsConflict->getName() + ".fr");
    if (MemoryRuntimeCheck) {
      IsConflict =
          ChkBuilder.CreateOr(MemoryRuntimeCheck, IsConflict, "conflict.rdx");
    }
    MemoryRuntimeCheck = IsConflict;
  }

  Expander.eraseDeadInstructions(MemoryRuntimeCheck);
  return MemoryRuntimeCheck;
}

std::optional<IVConditionInfo>
llvm::hasPartialIVCondition(const Loop &L, unsigned MSSAThreshold,
                            const MemorySSA &MSSA, AAResults &AA) {
  auto *TI = dyn_cast<CondBrInst>(L.getHeader()->getTerminator());
  if (!TI)
    return {};

  auto *CondI = dyn_cast<Instruction>(TI->getCondition());
  // The case with the condition outside the loop should already be handled
  // earlier.
  // Allow CmpInst and TruncInsts as they may be users of load instructions
  // and have potential for partial unswitching
  if (!CondI || !isa<CmpInst, TruncInst>(CondI) || !L.contains(CondI))
    return {};

  SmallVector<Instruction *> InstToDuplicate;
  InstToDuplicate.push_back(CondI);

  SmallVector<Value *, 4> WorkList;
  WorkList.append(CondI->op_begin(), CondI->op_end());

  SmallVector<MemoryAccess *, 4> AccessesToCheck;
  SmallVector<MemoryLocation, 4> AccessedLocs;
  while (!WorkList.empty()) {
    Instruction *I = dyn_cast<Instruction>(WorkList.pop_back_val());
    if (!I || !L.contains(I))
      continue;

    // TODO: support additional instructions.
    if (!isa<LoadInst>(I) && !isa<GetElementPtrInst>(I))
      return {};

    // Do not duplicate volatile and atomic loads.
    if (auto *LI = dyn_cast<LoadInst>(I))
      if (LI->isVolatile() || LI->isAtomic())
        return {};

    InstToDuplicate.push_back(I);
    if (MemoryAccess *MA = MSSA.getMemoryAccess(I)) {
      if (auto *MemUse = dyn_cast_or_null<MemoryUse>(MA)) {
        // Queue the defining access to check for alias checks.
        AccessesToCheck.push_back(MemUse->getDefiningAccess());
        AccessedLocs.push_back(MemoryLocation::get(I));
      } else {
        // MemoryDefs may clobber the location or may be atomic memory
        // operations. Bail out.
        return {};
      }
    }
    WorkList.append(I->op_begin(), I->op_end());
  }

  if (InstToDuplicate.empty())
    return {};

  SmallVector<BasicBlock *, 4> ExitingBlocks;
  L.getExitingBlocks(ExitingBlocks);
  auto HasNoClobbersOnPath =
      [&L, &AA, &AccessedLocs, &ExitingBlocks, &InstToDuplicate,
       MSSAThreshold](BasicBlock *Succ, BasicBlock *Header,
                      SmallVector<MemoryAccess *, 4> AccessesToCheck)
      -> std::optional<IVConditionInfo> {
    IVConditionInfo Info;
    // First, collect all blocks in the loop that are on a patch from Succ
    // to the header.
    SmallVector<BasicBlock *, 4> WorkList;
    WorkList.push_back(Succ);
    WorkList.push_back(Header);
    SmallPtrSet<BasicBlock *, 4> Seen;
    Seen.insert(Header);
    Info.PathIsNoop &=
        all_of(*Header, [](Instruction &I) { return !I.mayHaveSideEffects(); });

    while (!WorkList.empty()) {
      BasicBlock *Current = WorkList.pop_back_val();
      if (!L.contains(Current))
        continue;
      const auto &SeenIns = Seen.insert(Current);
      if (!SeenIns.second)
        continue;

      Info.PathIsNoop &= all_of(
          *Current, [](Instruction &I) { return !I.mayHaveSideEffects(); });
      WorkList.append(succ_begin(Current), succ_end(Current));
    }

    // Require at least 2 blocks on a path through the loop. This skips
    // paths that directly exit the loop.
    if (Seen.size() < 2)
      return {};

    // Next, check if there are any MemoryDefs that are on the path through
    // the loop (in the Seen set) and they may-alias any of the locations in
    // AccessedLocs. If that is the case, they may modify the condition and
    // partial unswitching is not possible.
    SmallPtrSet<MemoryAccess *, 4> SeenAccesses;
    while (!AccessesToCheck.empty()) {
      MemoryAccess *Current = AccessesToCheck.pop_back_val();
      auto SeenI = SeenAccesses.insert(Current);
      if (!SeenI.second || !Seen.contains(Current->getBlock()))
        continue;

      // Bail out if exceeded the threshold.
      if (SeenAccesses.size() >= MSSAThreshold)
        return {};

      // MemoryUse are read-only accesses.
      if (isa<MemoryUse>(Current))
        continue;

      // For a MemoryDef, check if is aliases any of the location feeding
      // the original condition.
      if (auto *CurrentDef = dyn_cast<MemoryDef>(Current)) {
        if (any_of(AccessedLocs, [&AA, CurrentDef](MemoryLocation &Loc) {
              return isModSet(
                  AA.getModRefInfo(CurrentDef->getMemoryInst(), Loc));
            }))
          return {};
      }

      for (Use &U : Current->uses())
        AccessesToCheck.push_back(cast<MemoryAccess>(U.getUser()));
    }

    // We could also allow loops with known trip counts without mustprogress,
    // but ScalarEvolution may not be available.
    Info.PathIsNoop &= isMustProgress(&L);

    // If the path is considered a no-op so far, check if it reaches a
    // single exit block without any phis. This ensures no values from the
    // loop are used outside of the loop.
    if (Info.PathIsNoop) {
      for (auto *Exiting : ExitingBlocks) {
        if (!Seen.contains(Exiting))
          continue;
        for (auto *Succ : successors(Exiting)) {
          if (L.contains(Succ))
            continue;

          Info.PathIsNoop &= Succ->phis().empty() &&
                             (!Info.ExitForPath || Info.ExitForPath == Succ);
          if (!Info.PathIsNoop)
            break;
          assert((!Info.ExitForPath || Info.ExitForPath == Succ) &&
                 "cannot have multiple exit blocks");
          Info.ExitForPath = Succ;
        }
      }
    }
    if (!Info.ExitForPath)
      Info.PathIsNoop = false;

    Info.InstToDuplicate = std::move(InstToDuplicate);
    return Info;
  };

  // If we branch to the same successor, partial unswitching will not be
  // beneficial.
  if (TI->getSuccessor(0) == TI->getSuccessor(1))
    return {};

  if (auto Info = HasNoClobbersOnPath(TI->getSuccessor(0), L.getHeader(),
                                      AccessesToCheck)) {
    Info->KnownValue = ConstantInt::getTrue(TI->getContext());
    return Info;
  }
  if (auto Info = HasNoClobbersOnPath(TI->getSuccessor(1), L.getHeader(),
                                      AccessesToCheck)) {
    Info->KnownValue = ConstantInt::getFalse(TI->getContext());
    return Info;
  }

  return {};
}

//===----------------------------------------------------------------------===//
// getConstantLoopAccessByteRange
//===----------------------------------------------------------------------===//

namespace {

/// Signedness of how the IV is interpreted by its consumer (the GEP index
/// path and the loop exit comparison). Once chosen, all IV-related
/// arithmetic (Init, Bound, trip count, range endpoints) is performed in
/// the IV's own bitwidth using APInt with the matching sign convention.
enum class IVSign { Signed, Unsigned };

/// Information about a counted-from-constant induction PHI.
struct SimpleIV {
  const PHINode *PN = nullptr;
  /// Initial value of the IV from the preheader, in the IV's own bitwidth.
  APInt Init;
  /// Per-iteration step (must be strictly positive), in the IV's bitwidth.
  APInt Step;
  /// True if the IV's `iv.next = add iv, Step` is marked nsw.
  bool HasNSW = false;
  /// True if the IV's `iv.next = add iv, Step` is marked nuw.
  bool HasNUW = false;
};

/// Match a counted-from-constant induction PHI in \p Header. The IV must
/// have exactly two incoming values: a `ConstantInt` from \p Preheader and
/// `add iv, ConstantStep` from \p Latch. The step must be non-zero and,
/// when interpreted using either signed or unsigned semantics, must be
/// strictly positive in some sign domain (we reject signed-negative steps
/// for simplicity). Reports the wrap flags on the add so that the caller
/// can decide whether the chosen IV interpretation is sound.
std::optional<SimpleIV> matchSimpleConstantIV(const BasicBlock *Header,
                                              const BasicBlock *Preheader,
                                              const BasicBlock *Latch) {
  for (const PHINode &PN : Header->phis()) {
    if (PN.getNumIncomingValues() != 2)
      continue;
    auto *Init = dyn_cast<ConstantInt>(PN.getIncomingValueForBlock(Preheader));
    if (!Init)
      continue;
    auto *Add = dyn_cast<BinaryOperator>(PN.getIncomingValueForBlock(Latch));
    if (!Add || Add->getOpcode() != Instruction::Add)
      continue;
    ConstantInt *Step = nullptr;
    if (Add->getOperand(0) == &PN)
      Step = dyn_cast<ConstantInt>(Add->getOperand(1));
    else if (Add->getOperand(1) == &PN)
      Step = dyn_cast<ConstantInt>(Add->getOperand(0));
    if (!Step)
      continue;
    const APInt &StepVal = Step->getValue();
    // Require a positive step under both signed and unsigned reads (i.e.
    // step is in [1, 2^(bw-1) - 1]).  This means the same numeric value
    // works in both sign domains and we don't have to second-guess later.
    if (StepVal.isZero() || StepVal.isNegative())
      continue;
    SimpleIV Out;
    Out.PN = &PN;
    Out.Init = Init->getValue();
    Out.Step = StepVal;
    Out.HasNSW = Add->hasNoSignedWrap();
    Out.HasNUW = Add->hasNoUnsignedWrap();
    return Out;
  }
  return std::nullopt;
}

/// Extract a constant trip count from a `icmp` against a constant bound
/// where one operand is the IV phi or the IV's increment. We accept both
/// head-tested forms and latch-tested forms. All arithmetic is performed
/// in the IV's bitwidth using APInt; \p Sign selects whether the bound
/// and predicate are interpreted as signed or unsigned.
std::optional<APInt> matchConstantTripCount(const BasicBlock *Header,
                                            const BasicBlock *Latch,
                                            const SimpleIV &IV, IVSign Sign) {
  auto FindCondBr = [](const BasicBlock *BB) -> const BranchInst * {
    auto *BR = dyn_cast<BranchInst>(BB->getTerminator());
    if (!BR || !BR->isConditional())
      return nullptr;
    return BR;
  };
  // Most loops at this point in the pipeline have the conditional branch in
  // the header (head-test) or the latch (latch-test). Try the latch first;
  // the header conditional branch is the head-test form.
  const BranchInst *BR = nullptr;
  if (Latch != Header)
    BR = FindCondBr(Latch);
  if (!BR)
    BR = FindCondBr(Header);
  if (!BR)
    return std::nullopt;
  auto *Cmp = dyn_cast<ICmpInst>(BR->getCondition());
  if (!Cmp)
    return std::nullopt;

  // Identify the IV-side operand: the phi itself, or the post-increment
  // `add iv, Step`.
  const Value *Op0 = Cmp->getOperand(0);
  const Value *Op1 = Cmp->getOperand(1);
  enum class IVKind { Phi, Inc };
  auto ClassifyIVOperand =
      [&](const Value *V) -> std::optional<IVKind> {
    if (V == IV.PN)
      return IVKind::Phi;
    if (auto *Add = dyn_cast<BinaryOperator>(V))
      if (Add->getOpcode() == Instruction::Add &&
          (Add->getOperand(0) == IV.PN || Add->getOperand(1) == IV.PN))
        return IVKind::Inc;
    return std::nullopt;
  };

  const ConstantInt *BoundC = nullptr;
  bool IVOnLeft = false;
  std::optional<IVKind> Kind;
  if (auto K = ClassifyIVOperand(Op0)) {
    Kind = K;
    BoundC = dyn_cast<ConstantInt>(Op1);
    IVOnLeft = true;
  } else if (auto K = ClassifyIVOperand(Op1)) {
    Kind = K;
    BoundC = dyn_cast<ConstantInt>(Op0);
  }
  if (!BoundC || !Kind)
    return std::nullopt;

  // The bound's bitwidth must match the IV (the cmp is between same-typed
  // values in valid IR, but be defensive).
  if (BoundC->getBitWidth() != IV.Init.getBitWidth())
    return std::nullopt;
  const APInt &Bound = BoundC->getValue();

  CmpInst::Predicate P =
      IVOnLeft ? Cmp->getPredicate() : Cmp->getSwappedPredicate();

  // Verify the predicate's signedness matches the requested IV
  // interpretation. Equality predicates are sign-agnostic and accepted
  // for either.
  bool IsSignedPred;
  switch (P) {
  case CmpInst::ICMP_SLT:
  case CmpInst::ICMP_SLE:
    IsSignedPred = true;
    break;
  case CmpInst::ICMP_ULT:
  case CmpInst::ICMP_ULE:
    IsSignedPred = false;
    break;
  case CmpInst::ICMP_NE:
  case CmpInst::ICMP_EQ:
    IsSignedPred = (Sign == IVSign::Signed);
    break;
  default:
    return std::nullopt;
  }
  if (Sign == IVSign::Signed && !IsSignedPred)
    return std::nullopt;
  if (Sign == IVSign::Unsigned && IsSignedPred)
    return std::nullopt;

  // Determine which successor of the conditional branch continues the
  // loop. If the true edge goes to the header we have "continue if true";
  // if the false edge goes to the header, invert the predicate to
  // normalise.
  bool TrueGoesToHeader = BR->getSuccessor(0) == Header;
  bool FalseGoesToHeader = BR->getSuccessor(1) == Header;
  if (TrueGoesToHeader == FalseGoesToHeader)
    return std::nullopt;
  if (FalseGoesToHeader)
    P = CmpInst::getInversePredicate(P);

  // Init < Bound under the chosen sign domain. (For NE / EQ we still
  // require this so the iteration count is well-defined.)
  bool InitLtBound = Sign == IVSign::Signed ? IV.Init.slt(Bound)
                                            : IV.Init.ult(Bound);
  if (!InitLtBound)
    return std::nullopt;

  // Compute Span = Bound - Init in the IV's bitwidth, checking that no
  // wrap occurs. With the InitLtBound check above this should not
  // overflow, but be defensive.
  bool Ov = false;
  APInt Span = Sign == IVSign::Signed ? Bound.ssub_ov(IV.Init, Ov)
                                      : Bound.usub_ov(IV.Init, Ov);
  if (Ov)
    return std::nullopt;

  // Compute the iteration count in IV bitwidth. We deliberately compute
  // here using unsigned APInt arithmetic on Span and Step, since both
  // are non-negative and we only care about non-negative iteration
  // counts. udiv/urem handle the magnitudes correctly.
  APInt Iters;
  switch (P) {
  case CmpInst::ICMP_ULT:
  case CmpInst::ICMP_SLT:
    // ceil(Span / Step) iterations satisfy "iv < Bound" with iv stepped.
    Iters = (Span + IV.Step - 1).udiv(IV.Step);
    break;
  case CmpInst::ICMP_ULE:
  case CmpInst::ICMP_SLE:
    Iters = Span.udiv(IV.Step) + 1;
    break;
  case CmpInst::ICMP_NE:
    if (!Span.urem(IV.Step).isZero())
      return std::nullopt;
    Iters = Span.udiv(IV.Step);
    break;
  default:
    return std::nullopt;
  }

  // Latch-test on the phi means the body has already executed for the
  // value that fails the predicate -- one extra iteration. Head-tested
  // form is when the conditional branch is in the header itself.
  bool IsLatchTest = BR->getParent() != Header;
  if (IsLatchTest && *Kind == IVKind::Phi)
    Iters += 1;

  return Iters;
}

/// Classification of an index value relative to \p IV.  \c std::nullopt
/// means "this isn't the IV (possibly cast)".  An empty optional sign
/// (i.e. \c IVUseClass with no Sign) means "the IV is used directly with
/// the GEP index width — either sign convention is acceptable, the caller
/// should pick based on the loop exit cmp".
struct IVUseClass {
  /// If set, the IV was reached through an explicit zext (Unsigned) or
  /// sext (Signed). If unset, the IV is used directly and either sign
  /// works.
  std::optional<IVSign> Sign;
};

/// If \p V is the IV (or the IV through a single zext/sext that widens
/// to \p IndexWidth), classify the use. Otherwise return std::nullopt.
std::optional<IVUseClass> classifyIVUseInIndex(const Value *V,
                                               const PHINode *IV,
                                               unsigned IndexWidth) {
  if (V == IV) {
    if (IV->getType()->getIntegerBitWidth() != IndexWidth)
      return std::nullopt;
    return IVUseClass{};
  }
  if (auto *Z = dyn_cast<ZExtInst>(V)) {
    if (Z->getOperand(0) == IV &&
        Z->getType()->getIntegerBitWidth() == IndexWidth)
      return IVUseClass{IVSign::Unsigned};
    return std::nullopt;
  }
  if (auto *S = dyn_cast<SExtInst>(V)) {
    if (S->getOperand(0) == IV &&
        S->getType()->getIntegerBitWidth() == IndexWidth)
      return IVUseClass{IVSign::Signed};
    return std::nullopt;
  }
  // Trunc (and any other cast) is rejected: it discards bits and would
  // require reasoning the helper isn't equipped for.
  return std::nullopt;
}

/// Walk the GEP chain backward from \p Ptr until reaching \p Base. Each
/// link must accumulate either a fully constant offset, or a single
/// non-constant index that is the loop's IV (possibly via zext or sext).
/// On success returns the constant base byte offset, the IV-scaled byte
/// stride (as an APInt with the IV's bitwidth, treated according to the
/// inferred sign), and the inferred sign of the IV use.
struct WalkedGEP {
  /// Constant byte offset accumulated along the chain.
  int64_t ConstOff;
  /// Scale (in bytes) applied to the IV value at the GEP index.
  uint64_t IVScale;
  /// Signedness with which the IV value is consumed by the GEP. If
  /// std::nullopt, the IV is used directly (no zext/sext) at the index
  /// position and either sign works -- the caller picks.
  std::optional<IVSign> Sign;
};

std::optional<WalkedGEP> walkGEPChain(const Value *Ptr, const Value *Base,
                                      const PHINode *IV,
                                      const DataLayout &DL) {
  // We require all GEPs in the chain to live in address space 0 (whose
  // index width is at least 64 bits on every supported target). This
  // sidesteps M4 entirely: address-space-modular offsets in narrower
  // index spaces require sign convention we can't infer here.
  int64_t ConstOff = 0;
  uint64_t IVScale = 0;
  bool SawIV = false;
  // Sign of the IV use: nullopt means "either sign acceptable" (direct
  // IV use with matching width), or "no IV use seen yet". A concrete
  // value is set once we see an explicit zext/sext on the IV.
  std::optional<IVSign> Sign;
  bool SignFixed = false;

  while (Ptr != Base) {
    auto *GEP = dyn_cast<GEPOperator>(Ptr);
    if (!GEP)
      return std::nullopt;
    if (GEP->getPointerAddressSpace() != 0)
      return std::nullopt;
    unsigned IdxWidth = DL.getIndexSizeInBits(0);
    APInt Off(IdxWidth, 0);
    if (GEP->accumulateConstantOffset(DL, Off)) {
      // Sign-extend (or truncate) the AS-modular offset into i64. For
      // AS=0 the index width is >= 64 on all targets we care about, but
      // be defensive and bail if the sign-extended value doesn't fit
      // in i64.
      if (!Off.isSignedIntN(64))
        return std::nullopt;
      bool Ov = false;
      int64_t OffI64 = Off.sextOrTrunc(64).getSExtValue();
      int64_t NewConst =
          APInt(64, ConstOff, true).sadd_ov(APInt(64, OffI64, true), Ov)
              .getSExtValue();
      if (Ov)
        return std::nullopt;
      ConstOff = NewConst;
      Ptr = GEP->getPointerOperand();
      continue;
    }
    // GEP has at least one non-constant index. Walk the indices manually:
    // accumulate constant indices, and require exactly one non-constant
    // index that is the IV (possibly through a single zext/sext cast).
    if (SawIV)
      return std::nullopt; // Already saw the IV in another GEP -- bail.
    Type *SrcTy = GEP->getSourceElementType();

    // Helper: multiply Stride * ConstIndex (signed, in i64) with overflow
    // checking, then add into LocalConst. The constant index APInt may be
    // wider than 64 bits; bail if so.
    auto AddStridedConst = [&](uint64_t Stride, const APInt &Idx,
                               int64_t &LocalConst) -> bool {
      if (!Idx.isSignedIntN(64))
        return false;
      if (Stride > (uint64_t)std::numeric_limits<int64_t>::max())
        return false;
      APInt StrideAP(64, Stride, true);
      APInt IdxAP(64, Idx.getSExtValue(), true);
      bool Ov = false;
      APInt Prod = StrideAP.smul_ov(IdxAP, Ov);
      if (Ov)
        return false;
      APInt NewLC =
          APInt(64, LocalConst, true).sadd_ov(Prod, Ov);
      if (Ov)
        return false;
      LocalConst = NewLC.getSExtValue();
      return true;
    };

    int64_t LocalConst = 0;
    uint64_t LocalScale = 0;
    bool HasVarIdx = false;
    Type *CurTy = SrcTy;
    auto Indices = drop_begin(GEP->indices(), 1);
    // First index is over the source element type itself.
    {
      const Value *I0 = GEP->getOperand(1);
      TypeSize Ts = DL.getTypeAllocSize(SrcTy);
      if (Ts.isScalable())
        return std::nullopt;
      uint64_t Stride = Ts.getFixedValue();
      if (auto *C = dyn_cast<ConstantInt>(I0)) {
        if (!AddStridedConst(Stride, C->getValue(), LocalConst))
          return std::nullopt;
      } else if (auto Use = classifyIVUseInIndex(I0, IV, IdxWidth)) {
        if (Stride > (uint64_t)std::numeric_limits<int64_t>::max())
          return std::nullopt;
        LocalScale = Stride;
        HasVarIdx = true;
        if (Use->Sign) {
          Sign = Use->Sign;
          SignFixed = true;
        }
      } else {
        return std::nullopt;
      }
    }
    for (const Value *Idx : Indices) {
      if (auto *ST = dyn_cast<StructType>(CurTy)) {
        auto *C = dyn_cast<ConstantInt>(Idx);
        if (!C)
          return std::nullopt;
        // Field index is unsigned; bail if it does not fit in 32 bits.
        if (!C->getValue().isIntN(32))
          return std::nullopt;
        unsigned FieldNo = C->getZExtValue();
        const StructLayout *SL = DL.getStructLayout(ST);
        uint64_t FieldOff = SL->getElementOffset(FieldNo);
        if (FieldOff > (uint64_t)std::numeric_limits<int64_t>::max())
          return std::nullopt;
        bool Ov = false;
        APInt NewLC =
            APInt(64, LocalConst, true)
                .sadd_ov(APInt(64, (int64_t)FieldOff, true), Ov);
        if (Ov)
          return std::nullopt;
        LocalConst = NewLC.getSExtValue();
        CurTy = ST->getElementType(FieldNo);
        continue;
      }
      // Array / vector / sequential type.
      Type *ElemTy = GetElementPtrInst::getTypeAtIndex(CurTy, (uint64_t)0);
      if (!ElemTy)
        return std::nullopt;
      TypeSize Ts = DL.getTypeAllocSize(ElemTy);
      if (Ts.isScalable())
        return std::nullopt;
      uint64_t Stride = Ts.getFixedValue();
      if (auto *C = dyn_cast<ConstantInt>(Idx)) {
        if (!AddStridedConst(Stride, C->getValue(), LocalConst))
          return std::nullopt;
      } else if (!HasVarIdx) {
        if (auto Use = classifyIVUseInIndex(Idx, IV, IdxWidth)) {
          if (Stride > (uint64_t)std::numeric_limits<int64_t>::max())
            return std::nullopt;
          LocalScale = Stride;
          HasVarIdx = true;
          if (Use->Sign) {
            Sign = Use->Sign;
            SignFixed = true;
          }
        } else {
          return std::nullopt;
        }
      } else {
        return std::nullopt;
      }
      CurTy = ElemTy;
    }
    bool Ov = false;
    APInt NewConst = APInt(64, ConstOff, true)
                         .sadd_ov(APInt(64, LocalConst, true), Ov);
    if (Ov)
      return std::nullopt;
    ConstOff = NewConst.getSExtValue();
    if (HasVarIdx) {
      IVScale = LocalScale;
      SawIV = true;
    }
    Ptr = GEP->getPointerOperand();
  }
  if (!SawIV)
    return std::nullopt;
  WalkedGEP Out;
  Out.ConstOff = ConstOff;
  Out.IVScale = IVScale;
  Out.Sign = SignFixed ? Sign : std::nullopt;
  return Out;
}

} // namespace

std::optional<std::pair<int64_t, int64_t>>
llvm::getConstantLoopAccessByteRange(const Instruction *I, const Value *Base,
                                     const Loop *L, const DataLayout &DL) {
  if (!L->isInnermost())
    return std::nullopt;
  // Only handle a simple two-block-or-fewer canonical loop with a preheader
  // and a latch that branches back to the header. The header carries the
  // IV phi; the exit comparison may live in the header or in the latch.
  const BasicBlock *Header = L->getHeader();
  const BasicBlock *Preheader = L->getLoopPreheader();
  const BasicBlock *Latch = L->getLoopLatch();
  if (!Preheader || !Latch)
    return std::nullopt;
  if (L->getNumBlocks() > 2)
    return std::nullopt;

  // Identify the IV.
  std::optional<SimpleIV> IV = matchSimpleConstantIV(Header, Preheader, Latch);
  if (!IV)
    return std::nullopt;

  // Determine pointer and access size for I.
  const Value *Ptr = nullptr;
  uint64_t AccessSize = 0;
  if (auto *S = dyn_cast<StoreInst>(I)) {
    if (!S->isSimple())
      return std::nullopt;
    Ptr = S->getPointerOperand();
    TypeSize Ts = DL.getTypeStoreSize(S->getValueOperand()->getType());
    if (Ts.isScalable())
      return std::nullopt;
    AccessSize = Ts.getFixedValue();
  } else if (auto *Ld = dyn_cast<LoadInst>(I)) {
    if (!Ld->isSimple())
      return std::nullopt;
    Ptr = Ld->getPointerOperand();
    TypeSize Ts = DL.getTypeStoreSize(Ld->getType());
    if (Ts.isScalable())
      return std::nullopt;
    AccessSize = Ts.getFixedValue();
  } else {
    return std::nullopt;
  }
  if (AccessSize == 0 ||
      AccessSize > (uint64_t)std::numeric_limits<int64_t>::max())
    return std::nullopt;

  // Walk back to Base, collecting constant offset, IV-scaled byte stride
  // and the signedness with which the GEP uses the IV.
  auto Walked = walkGEPChain(Ptr, Base, IV->PN, DL);
  if (!Walked)
    return std::nullopt;
  int64_t ConstOff = Walked->ConstOff;
  uint64_t IVScale = Walked->IVScale;
  if (IVScale == 0)
    return std::nullopt;
  IVSign Sign;
  APInt Iters(64, 0);

  auto TryWithSign = [&](IVSign S) -> bool {
    // Wrap-flag requirement.
    if (S == IVSign::Signed && !IV->HasNSW)
      return false;
    if (S == IVSign::Unsigned && !IV->HasNUW)
      return false;
    auto MaybeIters = matchConstantTripCount(Header, Latch, *IV, S);
    if (!MaybeIters || MaybeIters->isZero())
      return false;
    Iters = *MaybeIters;
    Sign = S;
    return true;
  };

  if (Walked->Sign) {
    if (!TryWithSign(*Walked->Sign))
      return std::nullopt;
  } else {
    // Direct IV use with matching index width: either sign is OK as long
    // as the wrap flag and exit cmp agree. Try unsigned first so that a
    // canonical loop with `add nuw nsw` and `icmp ult` ends up in the
    // unsigned domain (matches a typical zext-to-pointer-index lowering).
    if (!TryWithSign(IVSign::Unsigned) && !TryWithSign(IVSign::Signed))
      return std::nullopt;
  }

  // We need to be able to express Iters and Iters - 1 in i64. Bail
  // otherwise.
  if (!Iters.isIntN(64))
    return std::nullopt;
  uint64_t TC = Iters.getZExtValue();
  if (TC == 0)
    return std::nullopt;

  // Convert Init and Step to int64 according to the chosen sign. Bail if
  // they don't fit in i64 with the chosen sign convention. (Step is
  // already known positive.)
  if (Sign == IVSign::Signed && !IV->Init.isSignedIntN(64))
    return std::nullopt;
  if (Sign == IVSign::Unsigned && !IV->Init.isIntN(64))
    return std::nullopt;
  // Step is positive in both sign domains, so isIntN(64) suffices for both.
  if (!IV->Step.isIntN(64))
    return std::nullopt;
  int64_t InitI64 = Sign == IVSign::Signed ? IV->Init.getSExtValue()
                                           : (int64_t)IV->Init.getZExtValue();
  // For unsigned-domain Init, isIntN(64) means the value fits in u64 but
  // could be > INT64_MAX; in that case (int64_t)getZExtValue is negative
  // when reinterpreted, which would corrupt the signed range arithmetic.
  // Reject that case: the byte range model below uses int64_t.
  if (Sign == IVSign::Unsigned && InitI64 < 0)
    return std::nullopt;
  int64_t StepI64 = (int64_t)IV->Step.getZExtValue();
  if (StepI64 < 0)
    return std::nullopt;
  if (IVScale > (uint64_t)std::numeric_limits<int64_t>::max())
    return std::nullopt;
  int64_t IVScaleI64 = (int64_t)IVScale;

  // Per-iteration stride in bytes = IVScale * Step, signed. (IVScale is
  // non-negative as a byte size and Step is positive, so this is in
  // [0, INT64_MAX] when no overflow is signalled.)
  bool Ov = false;
  APInt IterStride = APInt(64, IVScaleI64, true)
                         .smul_ov(APInt(64, StepI64, true), Ov);
  if (Ov)
    return std::nullopt;
  // No-holes check: stride must cover at least the access size. Use APInt
  // abs() to avoid the std::abs(INT64_MIN) UB.
  uint64_t AbsStride = IterStride.abs().getZExtValue();
  if (AbsStride < AccessSize)
    return std::nullopt;

  // Compute the [low, high) byte range across all iterations.
  // low  = ConstOff + Init      * IVScale
  // high = ConstOff + LastIVVal * IVScale + AccessSize
  // where LastIVVal = Init + (TC - 1) * Step.
  APInt InitAP(64, InitI64, true);
  APInt StepAP(64, StepI64, true);
  APInt ScaleAP(64, IVScaleI64, true);
  APInt ConstAP(64, ConstOff, true);
  APInt AccessAP(64, (int64_t)AccessSize, true);

  Ov = false;
  APInt IVBase = InitAP.smul_ov(ScaleAP, Ov);
  if (Ov)
    return std::nullopt;
  APInt TCMinus1(64, TC - 1, true);
  APInt LastDelta = TCMinus1.smul_ov(StepAP, Ov);
  if (Ov)
    return std::nullopt;
  APInt LastIVVal = InitAP.sadd_ov(LastDelta, Ov);
  if (Ov)
    return std::nullopt;
  APInt LastTimesScale = LastIVVal.smul_ov(ScaleAP, Ov);
  if (Ov)
    return std::nullopt;
  APInt Low = ConstAP.sadd_ov(IVBase, Ov);
  if (Ov)
    return std::nullopt;
  APInt High = ConstAP.sadd_ov(LastTimesScale, Ov);
  if (Ov)
    return std::nullopt;
  High = High.sadd_ov(AccessAP, Ov);
  if (Ov)
    return std::nullopt;
  int64_t LowI = Low.getSExtValue();
  int64_t HighI = High.getSExtValue();
  if (LowI > HighI)
    std::swap(LowI, HighI);
  return std::make_pair(LowI, HighI);
}
