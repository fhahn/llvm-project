//===- MachineLoopDeletion.cpp - Dead Loop Deletion Pass ------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This pass removes innermost machine loops that are read-only and whose
// register definitions are all dead outside the loop.
//
//===----------------------------------------------------------------------===//

#include "llvm/ADT/Statistic.h"
#include "llvm/CodeGen/LiveRegUnits.h"
#include "llvm/CodeGen/MachineDominators.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineLoopInfo.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/CodeGen/TargetInstrInfo.h"
#include "llvm/CodeGen/TargetSubtargetInfo.h"
#include "llvm/InitializePasses.h"
#include "llvm/Pass.h"
#include "llvm/Support/Debug.h"

using namespace llvm;

#define DEBUG_TYPE "machine-loop-delete"

STATISTIC(NumAnalyzed, "Number of innermost machine loops analyzed");
STATISTIC(NumDeleted, "Number of dead machine loops deleted");

namespace {
class MachineLoopDeletion : public MachineFunctionPass {
public:
  static char ID;
  MachineLoopDeletion() : MachineFunctionPass(ID) {
    initializeMachineLoopDeletionPass(*PassRegistry::getPassRegistry());
  }

  bool runOnMachineFunction(MachineFunction &MF) override;

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.addRequired<MachineLoopInfoWrapperPass>();
    AU.addPreserved<MachineDominatorTreeWrapperPass>();
    AU.addPreserved<MachineLoopInfoWrapperPass>();
    MachineFunctionPass::getAnalysisUsage(AU);
  }
};
} // namespace

char MachineLoopDeletion::ID = 0;
char &llvm::MachineLoopDeletionID = MachineLoopDeletion::ID;

INITIALIZE_PASS_BEGIN(MachineLoopDeletion, DEBUG_TYPE,
                      "Delete dead machine loops", false, false)
INITIALIZE_PASS_DEPENDENCY(MachineLoopInfoWrapperPass)
INITIALIZE_PASS_END(MachineLoopDeletion, DEBUG_TYPE,
                    "Delete dead machine loops", false, false)

static bool isLoopDead(MachineLoop *L, const MachineRegisterInfo &MRI,
                       const TargetRegisterInfo &TRI) {
  LiveRegUnits LiveAtExit(TRI);
  SmallVector<MachineBasicBlock *> ExitBlocks;
  L->getExitBlocks(ExitBlocks);
  for (MachineBasicBlock *Exit : ExitBlocks)
    LiveAtExit.addLiveIns(*Exit);

  for (MachineBasicBlock *MBB : L->blocks()) {
    for (MachineInstr &MI : *MBB) {
      // Inline asm represents explicit user intent and should not be removed,
      // even if it lacks the sideeffect flag.
      if (MI.mayStore() || MI.isCall() || MI.isInlineAsm() ||
          MI.hasUnmodeledSideEffects())
        return false;
      if (MI.mayLoad() &&
          llvm::any_of(MI.memoperands(),
                       [](const MachineMemOperand *MMO) {
                         return !MMO->isUnordered();
                       }))
        return false;

      for (const MachineOperand &MO : MI.all_defs()) {
        if (MO.isDead())
          continue;
        Register Reg = MO.getReg();
        if (Reg.isPhysical()) {
          // Constant registers (e.g. the zero register) are unaffected by
          // writes, so they can be ignored.
          if (MRI.isConstantPhysReg(Reg))
            continue;
          // Reserved registers (e.g. the stack pointer) have implicit global
          // liveness that may not be reflected in live-in lists.
          if (MRI.isReserved(Reg) || !LiveAtExit.available(Reg))
            return false;
        } else {
          for (const MachineInstr &User : MRI.use_nodbg_instructions(Reg))
            if (!L->contains(User.getParent()))
              return false;
        }
      }
    }
  }
  return true;
}

bool MachineLoopDeletion::runOnMachineFunction(MachineFunction &MF) {
  if (skipFunction(MF.getFunction()))
    return false;

  // Without mustprogress, an infinite loop with no side effects is legal and
  // its non-termination is observable behavior. We cannot remove such loops.
  if (!MF.getFunction().mustProgress())
    return false;

  MachineLoopInfo &MLI =
      getAnalysis<MachineLoopInfoWrapperPass>().getLI();
  const MachineRegisterInfo &MRI = MF.getRegInfo();
  const TargetRegisterInfo &TRI = *MF.getSubtarget().getRegisterInfo();

  MachineDominatorTree *MDT = nullptr;
  if (auto *MDTWP = getAnalysisIfAvailable<MachineDominatorTreeWrapperPass>())
    MDT = &MDTWP->getDomTree();

  bool Changed = false;
  SmallVector<MachineLoop *, 4> Worklist;
  for (MachineLoop *L : MLI.getLoopsInPreorder())
    if (L->isInnermost())
      Worklist.push_back(L);

  for (MachineLoop *L : Worklist) {
    ++NumAnalyzed;
    MachineBasicBlock *Preheader = MLI.findLoopPreheader(L);
    MachineBasicBlock *ExitBlock = L->getUniqueExitBlock();
    if (!Preheader || !ExitBlock)
      continue;

    if (!isLoopDead(L, MRI, TRI))
      continue;

    LLVM_DEBUG(dbgs() << "Deleting dead machine loop: " << *L);

    // Update exit block PHIs: replace loop block entries with a single
    // preheader entry. All loop-block entries in a given PHI must carry the
    // same value; bail if they differ.
    SmallVector<Register, 4> PreheaderVals;
    bool UnsupportedPHI = false;
    for (MachineInstr &PHI : ExitBlock->phis()) {
      Register PreheaderVal;
      for (int I = PHI.getNumOperands() - 1; I >= 2; I -= 2) {
        if (!L->contains(PHI.getOperand(I).getMBB()))
          continue;
        Register Val = PHI.getOperand(I - 1).getReg();
        if (!PreheaderVal.isValid()) {
          PreheaderVal = Val;
        } else if (Val != PreheaderVal) {
          UnsupportedPHI = true;
          break;
        }
      }
      if (UnsupportedPHI)
        break;
      assert(PreheaderVal.isValid() &&
             "expected loop predecessor in exit block PHI");
      PreheaderVals.push_back(PreheaderVal);
    }

    if (UnsupportedPHI) {
      LLVM_DEBUG(dbgs() << "  Skipping: exit block PHI has conflicting "
                           "values from different loop exits\n");
      continue;
    }

    unsigned PhiIdx = 0;
    for (MachineInstr &PHI : ExitBlock->phis()) {
      for (int I = PHI.getNumOperands() - 1; I >= 2; I -= 2) {
        if (L->contains(PHI.getOperand(I).getMBB())) {
          PHI.removeOperand(I);
          PHI.removeOperand(I - 1);
        }
      }
      PHI.addOperand(MachineOperand::CreateReg(PreheaderVals[PhiIdx++], false));
      PHI.addOperand(MachineOperand::CreateMBB(Preheader));
    }

    Preheader->ReplaceUsesOfBlockWith(L->getHeader(), ExitBlock);

    SmallVector<MachineBasicBlock *, 4> Blocks(L->blocks());

    // Update dominator tree if available. The preheader->header edge has
    // already been replaced with preheader->exit by ReplaceUsesOfBlockWith
    // above, matching the post-update CFG that applyUpdates expects.
    if (MDT)
      MDT->applyUpdates(
          {{MachineDominatorTree::Insert, Preheader, ExitBlock},
           {MachineDominatorTree::Delete, Preheader, L->getHeader()}});

    // Mark debug uses of virtual registers defined in the loop as undef,
    // since those defs are about to be erased.
    for (MachineBasicBlock *MBB : Blocks)
      for (MachineInstr &MI : *MBB)
        for (const MachineOperand &MO : MI.all_defs())
          if (MO.getReg().isVirtual())
            MRI.markUsesInDebugValueAsUndef(MO.getReg());

    if (MachineLoop *Parent = L->getParentLoop())
      Parent->removeChildLoop(L);
    else
      MLI.removeLoop(llvm::find(MLI, L));

    for (MachineBasicBlock *MBB : Blocks) {
      MLI.removeBlock(MBB);
      while (!MBB->succ_empty())
        MBB->removeSuccessor(MBB->succ_begin());
      MBB->eraseFromParent();
    }

    MLI.destroy(L);

    // If the preheader originally fell through to the loop header (no explicit
    // branch), the erasure of loop blocks may leave the preheader falling
    // through to the wrong block. Add an explicit branch if needed.
    if (!Preheader->isLayoutSuccessor(ExitBlock)) {
      MachineBasicBlock::iterator Term = Preheader->getFirstTerminator();
      if (Term == Preheader->end()) {
        const TargetInstrInfo *TII = MF.getSubtarget().getInstrInfo();
        TII->insertBranch(*Preheader, ExitBlock, nullptr, {}, DebugLoc());
      }
    }
    Changed = true;
    ++NumDeleted;
  }

  return Changed;
}
