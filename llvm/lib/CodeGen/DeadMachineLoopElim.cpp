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
#include "llvm/CodeGen/TargetInstrInfo.h"
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
  // Physical registers that are live on entry to any of the loop's exit blocks
  // cannot be considered dead. Note that this only catches escapes that are
  // visible to register liveness; opaque physical-register writes are handled
  // separately below.
  LiveRegUnits LiveAtExit(TRI);
  SmallVector<MachineBasicBlock *> ExitBlocks;
  L->getExitBlocks(ExitBlocks);
  for (MachineBasicBlock *Exit : ExitBlocks)
    LiveAtExit.addLiveIns(*Exit);

  for (MachineBasicBlock *MBB : L->blocks()) {
    for (MachineInstr &MI : *MBB) {
      if (MI.mayStore() || MI.isCall() || MI.hasUnmodeledSideEffects())
        return false;
      if (MI.mayLoad() &&
          llvm::any_of(MI.memoperands(),
                       [](const MachineMemOperand *MMO) {
                         return !MMO->isUnordered();
                       }))
        return false;

      // Inline assembly and FAKE_USE can keep a value live in a physical
      // register through channels that are invisible to register liveness:
      // inline asm may reference registers by name in its template string or
      // clobber list, and FAKE_USE (e.g. from llvm.write_register) is emitted
      // precisely to keep an otherwise-dead physical register live. Treating
      // such loops as dead would drop those writes, so bail out conservatively.
      if (MI.isInlineAsm() || MI.isFakeUse())
        return false;

      for (const MachineOperand &MO : MI.all_defs()) {
        Register Reg = MO.getReg();
        if (Reg.isPhysical()) {
          // A COPY into a physical register (e.g. the lowering of
          // llvm.write_register) can make that register's value escape the loop
          // without any register-operand use that liveness could track. Bail
          // out rather than risk deleting such a write.
          if (MI.isCopy())
            return false;
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

    // Update PHI nodes in the exit block: remove entries from loop blocks
    // and add an entry for the preheader. Since the loop is dead, any values
    // from loop blocks that feed into exit block PHIs must be defined outside
    // the loop (isLoopDead ensures no loop-defined registers escape).
    // All loop-block entries in a given PHI must carry the same value; if they
    // differ we cannot determine which value the preheader should use.
    bool BadPHI = false;
    for (MachineInstr &PHI : ExitBlock->phis()) {
      Register PreheaderVal;
      bool FoundLoopEntry = false;
      for (unsigned I = PHI.getNumOperands() - 1; I >= 2; I -= 2) {
        if (L->contains(PHI.getOperand(I).getMBB())) {
          Register Val = PHI.getOperand(I - 1).getReg();
          if (!FoundLoopEntry) {
            PreheaderVal = Val;
            FoundLoopEntry = true;
          } else if (Val != PreheaderVal) {
            BadPHI = true;
            break;
          }
        }
      }
      if (BadPHI)
        break;
    }

    if (BadPHI) {
      LLVM_DEBUG(dbgs() << "  Skipping: exit block PHI has conflicting "
                           "values from different loop exits\n");
      continue;
    }

    for (MachineInstr &PHI : ExitBlock->phis()) {
      Register PreheaderVal;
      bool FoundLoopEntry = false;
      for (unsigned I = PHI.getNumOperands() - 1; I >= 2; I -= 2) {
        if (L->contains(PHI.getOperand(I).getMBB())) {
          if (!FoundLoopEntry) {
            PreheaderVal = PHI.getOperand(I - 1).getReg();
            FoundLoopEntry = true;
          }
          PHI.removeOperand(I);
          PHI.removeOperand(I - 1);
        }
      }
      PHI.addOperand(MachineOperand::CreateReg(PreheaderVal, false));
      PHI.addOperand(MachineOperand::CreateMBB(Preheader));
    }

    Preheader->ReplaceUsesOfBlockWith(L->getHeader(), ExitBlock);

    SmallVector<MachineBasicBlock *, 4> Blocks(L->blocks());
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
