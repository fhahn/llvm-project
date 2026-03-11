//===- DeadMachineLoopElim.cpp - Remove dead machine loops ----------------===//
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
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineLoopInfo.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/CodeGen/TargetSubtargetInfo.h"
#include "llvm/InitializePasses.h"
#include "llvm/Pass.h"
#include "llvm/Support/Debug.h"

using namespace llvm;

#define DEBUG_TYPE "dead-machine-loop-elim"

STATISTIC(NumDeleted, "Number of dead machine loops deleted");

namespace {
class DeadMachineLoopElim : public MachineFunctionPass {
public:
  static char ID;
  DeadMachineLoopElim() : MachineFunctionPass(ID) {
    initializeDeadMachineLoopElimPass(*PassRegistry::getPassRegistry());
  }

  bool runOnMachineFunction(MachineFunction &MF) override;

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.addRequired<MachineLoopInfoWrapperPass>();
    AU.addPreserved<MachineLoopInfoWrapperPass>();
    MachineFunctionPass::getAnalysisUsage(AU);
  }
};
} // namespace

char DeadMachineLoopElim::ID = 0;
char &llvm::DeadMachineLoopElimID = DeadMachineLoopElim::ID;

INITIALIZE_PASS_BEGIN(DeadMachineLoopElim, DEBUG_TYPE,
                      "Remove dead machine loops", false, false)
INITIALIZE_PASS_DEPENDENCY(MachineLoopInfoWrapperPass)
INITIALIZE_PASS_END(DeadMachineLoopElim, DEBUG_TYPE,
                    "Remove dead machine loops", false, false)

static bool isLoopDead(MachineLoop *L, const MachineRegisterInfo &MRI,
                       const TargetRegisterInfo &TRI) {
  LiveRegUnits LiveAtExit(TRI);
  SmallVector<MachineBasicBlock *> ExitBlocks;
  L->getExitBlocks(ExitBlocks);
  for (MachineBasicBlock *Exit : ExitBlocks)
    LiveAtExit.addLiveIns(*Exit);

  for (MachineBasicBlock *MBB : L->blocks()) {
    for (MachineInstr &MI : *MBB) {
      if (MI.mayStore() || MI.isCall() || MI.hasUnmodeledSideEffects())
        return false;

      for (const MachineOperand &MO : MI.all_defs()) {
        Register Reg = MO.getReg();
        if (Reg.isPhysical()) {
          if (!LiveAtExit.available(Reg))
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

bool DeadMachineLoopElim::runOnMachineFunction(MachineFunction &MF) {
  if (skipFunction(MF.getFunction()))
    return false;

  MachineLoopInfo &MLI =
      getAnalysis<MachineLoopInfoWrapperPass>().getLI();
  const MachineRegisterInfo &MRI = MF.getRegInfo();
  const TargetRegisterInfo &TRI = *MF.getSubtarget().getRegisterInfo();

  bool Changed = false;
  SmallVector<MachineLoop *, 4> Worklist;
  for (MachineLoop *L : MLI.getLoopsInPreorder())
    if (L->isInnermost())
      Worklist.push_back(L);

  for (MachineLoop *L : Worklist) {
    MachineBasicBlock *Preheader = MLI.findLoopPreheader(L);
    MachineBasicBlock *ExitBlock = L->getUniqueExitBlock();
    if (!Preheader || !ExitBlock)
      continue;

    if (!isLoopDead(L, MRI, TRI))
      continue;

    LLVM_DEBUG(dbgs() << "Deleting dead machine loop: " << *L);

    Preheader->ReplaceUsesOfBlockWith(L->getHeader(), ExitBlock);

    SmallVector<MachineBasicBlock *, 4> Blocks(L->blocks());
    if (MachineLoop *Parent = L->getParentLoop())
      Parent->removeChildLoop(L);
    else
      MLI.removeLoop(llvm::find(MLI, L));

    for (MachineBasicBlock *MBB : Blocks) {
      while (!MBB->succ_empty())
        MBB->removeSuccessor(MBB->succ_begin());
      MBB->eraseFromParent();
    }
    Changed = true;
    ++NumDeleted;
  }

  return Changed;
}
