//==--- llvm/CodeGen/ReachingDefAnalysis.h - Reaching Def Analysis -*- C++ -*---==//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
/// \file Reaching Defs Analysis pass.
///
/// This pass tracks for each instruction what is the "closest" reaching def of
/// a given register. It is used by BreakFalseDeps (for clearance calculation)
/// and ExecutionDomainFix (for arbitrating conflicting domains).
///
/// Note that this is different from the usual definition notion of liveness.
/// The CPU doesn't care whether or not we consider a register killed.
///
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CODEGEN_REACHINGDEFANALYSIS_H
#define LLVM_CODEGEN_REACHINGDEFANALYSIS_H

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/CodeGen/LoopTraversal.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachinePassManager.h"
#include "llvm/InitializePasses.h"
#include <tuple>

namespace llvm {

class MachineBasicBlock;
class MachineInstr;

/// This class provides the reaching def analysis.
class ReachingDefInfo {
private:
  MachineFunction *MF = nullptr;
  const TargetRegisterInfo *TRI = nullptr;
  const TargetInstrInfo *TII = nullptr;
  LoopTraversal::TraversalOrder TraversedMBBOrder;
  unsigned NumRegUnits = 0;
  unsigned NumStackObjects = 0;
  int ObjectIndexBegin = 0;
  /// Instruction that defined each register, relative to the beginning of the
  /// current basic block.  When a LiveRegsDefInfo is used to represent a
  /// live-out register, this value is relative to the end of the basic block,
  /// so it will be a negative number.
  using LiveRegsDefInfo = std::vector<int>;
  LiveRegsDefInfo LiveRegs;

  /// Keeps clearance information for all registers. Note that this
  /// is different from the usual definition notion of liveness. The CPU
  /// doesn't care whether or not we consider a register killed.
  /// The entry of a block is its slice of OutRegsStorage, or empty if the
  /// block has not been processed yet.
  using OutRegsInfoMap = SmallVector<MutableArrayRef<int>, 4>;
  OutRegsInfoMap MBBOutRegsInfos;
  LiveRegsDefInfo OutRegsStorage;

  /// Current instruction number.
  /// The first instruction in each basic block is 0.
  int CurInstr = -1;

  /// Maps instructions to their instruction Ids, relative to the beginning of
  /// their basic blocks.
  DenseMap<MachineInstr *, int> InstIds;

  /// A def of a register unit by the instruction with id InstId.
  struct UnitDef {
    unsigned Unit;
    int InstId;

    bool operator<(const UnitDef &RHS) const {
      return std::tie(Unit, InstId) < std::tie(RHS.Unit, RHS.InstId);
    }
    bool operator==(const UnitDef &RHS) const {
      return Unit == RHS.Unit && InstId == RHS.InstId;
    }
  };

  /// Defs in all blocks. Each block owns the index range BlockDefRanges[N] of
  /// LocalDefs, sorted by unit and then by instruction id.
  SmallVector<UnitDef> LocalDefs;
  SmallVector<std::pair<unsigned, unsigned>> BlockDefRanges;

  /// Number of non-debug instructions in each block.
  SmallVector<int> BlockNumInsts;

  /// Dense index of each register unit that is defined in a reachable block or
  /// live into the function, or NoDenseUnit. No other unit can have a reaching
  /// def, so the dataflow only tracks these, and LocalDefs, LiveRegs,
  /// MBBOutRegsInfos and BlockLiveIns are indexed by the dense index.
  static constexpr unsigned NoDenseUnit = ~0u;
  SmallVector<unsigned> UnitToDenseIdx;
  unsigned NumDenseUnits = 0;

  /// Reaching def of each unit on entry to each block (indexed by block
  /// number * NumDenseUnits + unit), or ReachingDefDefaultVal if none.
  SmallVector<int> BlockLiveIns;

  /// MBBFrameObjsReachingDefs[{i, j}] is a list of instruction indices
  /// (relative to begining of MBB i) that define frame index j in MBB i. This
  /// is used in answering reaching definition queries.
  using MBBFrameObjsReachingDefsInfo =
      DenseMap<std::pair<unsigned, int>, SmallVector<int>>;
  MBBFrameObjsReachingDefsInfo MBBFrameObjsReachingDefs;

  /// Default values are 'nothing happened a long time ago'.
  static constexpr int ReachingDefDefaultVal = -(1 << 21);
  /// Special values for function live-ins.
  static constexpr int FunctionLiveInMarker = -1;

  using InstSet = SmallPtrSetImpl<MachineInstr*>;
  using BlockSet = SmallPtrSetImpl<MachineBasicBlock*>;

public:
  LLVM_ABI ReachingDefInfo();
  LLVM_ABI ReachingDefInfo(ReachingDefInfo &&);
  LLVM_ABI ~ReachingDefInfo();
  /// Handle invalidation explicitly.
  LLVM_ABI bool invalidate(MachineFunction &F, const PreservedAnalyses &PA,
                           MachineFunctionAnalysisManager::Invalidator &);

  LLVM_ABI void run(MachineFunction &mf);
  LLVM_ABI void print(raw_ostream &OS);
  LLVM_ABI void releaseMemory();

  /// Re-run the analysis.
  LLVM_ABI void reset();

  /// Initialize data structures.
  LLVM_ABI void init();

  /// Traverse the machine function, mapping definitions.
  LLVM_ABI void traverse();

  /// Provides the instruction id of the closest reaching def instruction of
  /// Reg that reaches MI, relative to the begining of MI's basic block.
  /// Note that Reg may represent a stack slot.
  LLVM_ABI int getReachingDef(MachineInstr *MI, Register Reg) const;

  /// Return whether A and B use the same def of Reg.
  LLVM_ABI bool hasSameReachingDef(MachineInstr *A, MachineInstr *B,
                                   Register Reg) const;

  /// Return whether the reaching def for MI also is live out of its parent
  /// block.
  LLVM_ABI bool isReachingDefLiveOut(MachineInstr *MI, Register Reg) const;

  /// Return the local MI that produces the live out value for Reg, or
  /// nullptr for a non-live out or non-local def.
  LLVM_ABI MachineInstr *getLocalLiveOutMIDef(MachineBasicBlock *MBB,
                                              Register Reg) const;

  /// If a single MachineInstr creates the reaching definition, then return it.
  /// Otherwise return null.
  LLVM_ABI MachineInstr *getUniqueReachingMIDef(MachineInstr *MI,
                                                Register Reg) const;

  /// If a single MachineInstr creates the reaching definition, for MIs operand
  /// at Idx, then return it. Otherwise return null.
  LLVM_ABI MachineInstr *getMIOperand(MachineInstr *MI, unsigned Idx) const;

  /// If a single MachineInstr creates the reaching definition, for MIs MO,
  /// then return it. Otherwise return null.
  LLVM_ABI MachineInstr *getMIOperand(MachineInstr *MI,
                                      MachineOperand &MO) const;

  /// Provide whether the register has been defined in the same basic block as,
  /// and before, MI.
  LLVM_ABI bool hasLocalDefBefore(MachineInstr *MI, Register Reg) const;

  /// Return whether the given register is used after MI, whether it's a local
  /// use or a live out.
  LLVM_ABI bool isRegUsedAfter(MachineInstr *MI, Register Reg) const;

  /// Return whether the given register is defined after MI.
  LLVM_ABI bool isRegDefinedAfter(MachineInstr *MI, Register Reg) const;

  /// Provides the clearance - the number of instructions since the closest
  /// reaching def instruction of Reg that reaches MI.
  LLVM_ABI int getClearance(MachineInstr *MI, Register Reg) const;

  /// Provides the uses, in the same block as MI, of register that MI defines.
  /// This does not consider live-outs.
  LLVM_ABI void getReachingLocalUses(MachineInstr *MI, Register Reg,
                                     InstSet &Uses) const;

  /// Search MBB for a definition of Reg and insert it into Defs. If no
  /// definition is found, recursively search the predecessor blocks for them.
  LLVM_ABI void getLiveOuts(MachineBasicBlock *MBB, Register Reg, InstSet &Defs,
                            BlockSet &VisitedBBs) const;
  LLVM_ABI void getLiveOuts(MachineBasicBlock *MBB, Register Reg,
                            InstSet &Defs) const;

  /// For the given block, collect the instructions that use the live-in
  /// value of the provided register. Return whether the value is still
  /// live on exit.
  LLVM_ABI bool getLiveInUses(MachineBasicBlock *MBB, Register Reg,
                              InstSet &Uses) const;

  /// Collect the users of the value stored in Reg, which is defined
  /// by MI.
  LLVM_ABI void getGlobalUses(MachineInstr *MI, Register Reg,
                              InstSet &Uses) const;

  /// Collect all possible definitions of the value stored in Reg, which is
  /// used by MI.
  LLVM_ABI void getGlobalReachingDefs(MachineInstr *MI, Register Reg,
                                      InstSet &Defs) const;

  /// Return whether From can be moved forwards to just before To.
  LLVM_ABI bool isSafeToMoveForwards(MachineInstr *From,
                                     MachineInstr *To) const;

  /// Return whether From can be moved backwards to just after To.
  LLVM_ABI bool isSafeToMoveBackwards(MachineInstr *From,
                                      MachineInstr *To) const;

  /// Assuming MI is dead, recursively search the incoming operands which are
  /// killed by MI and collect those that would become dead.
  LLVM_ABI void collectKilledOperands(MachineInstr *MI, InstSet &Dead) const;

  /// Return whether removing this instruction will have no effect on the
  /// program, returning the redundant use-def chain.
  LLVM_ABI bool isSafeToRemove(MachineInstr *MI, InstSet &ToRemove) const;

  /// Return whether removing this instruction will have no effect on the
  /// program, ignoring the possible effects on some instructions, returning
  /// the redundant use-def chain.
  LLVM_ABI bool isSafeToRemove(MachineInstr *MI, InstSet &ToRemove,
                               InstSet &Ignore) const;

  /// Return whether a MachineInstr could be inserted at MI and safely define
  /// the given register without affecting the program.
  LLVM_ABI bool isSafeToDefRegAt(MachineInstr *MI, Register Reg) const;

  /// Return whether a MachineInstr could be inserted at MI and safely define
  /// the given register without affecting the program, ignoring any effects
  /// on the provided instructions.
  LLVM_ABI bool isSafeToDefRegAt(MachineInstr *MI, Register Reg,
                                 InstSet &Ignore) const;

private:
  /// Set up LiveRegs by merging predecessor live-out values.
  void enterBasicBlock(MachineBasicBlock *MBB);

  /// Update live-out values.
  void leaveBasicBlock(MachineBasicBlock *MBB);

  /// Process he given basic block.
  void processBasicBlock(const LoopTraversal::TraversedMBBInfo &TraversedMBB);

  /// Process block that is part of a loop again.
  void reprocessBasicBlock(MachineBasicBlock *MBB);

  /// Record the defs of all instructions in MBB.
  void scanBasicBlock(MachineBasicBlock *MBB);

  /// Update def-ages for registers defined by MI.
  /// Also break dependencies on partial defs and undef uses.
  void processDefs(MachineInstr *);

  /// Utility function for isSafeToMoveForwards/Backwards.
  template<typename Iterator>
  bool isSafeToMove(MachineInstr *From, MachineInstr *To) const;

  /// Return whether removing this instruction will have no effect on the
  /// program, ignoring the possible effects on some instructions, returning
  /// the redundant use-def chain.
  bool isSafeToRemove(MachineInstr *MI, InstSet &Visited,
                      InstSet &ToRemove, InstSet &Ignore) const;

  /// Provides the MI, from the given block, corresponding to the Id or a
  /// nullptr if the id does not refer to the block.
  MachineInstr *getInstFromId(MachineBasicBlock *MBB, int InstId) const;

  /// Provides the instruction of the closest reaching def instruction of
  /// Reg that reaches MI, relative to the begining of MI's basic block.
  /// Note that Reg may represent a stack slot.
  MachineInstr *getReachingLocalMIDef(MachineInstr *MI, Register Reg) const;
};

class ReachingDefAnalysis : public AnalysisInfoMixin<ReachingDefAnalysis> {
  friend AnalysisInfoMixin<ReachingDefAnalysis>;
  static AnalysisKey Key;

public:
  using Result = ReachingDefInfo;

  LLVM_ABI Result run(MachineFunction &MF,
                      MachineFunctionAnalysisManager &MFAM);
};

/// Printer pass for the \c ReachingDefInfo results.
class ReachingDefPrinterPass
    : public RequiredPassInfoMixin<ReachingDefPrinterPass> {
  raw_ostream &OS;

public:
  explicit ReachingDefPrinterPass(raw_ostream &OS) : OS(OS) {}

  LLVM_ABI PreservedAnalyses run(MachineFunction &MF,
                                 MachineFunctionAnalysisManager &MFAM);
};

class LLVM_ABI ReachingDefInfoWrapperPass : public MachineFunctionPass {
  ReachingDefInfo RDI;

public:
  static char ID;

  ReachingDefInfoWrapperPass();

  void getAnalysisUsage(AnalysisUsage &AU) const override;
  MachineFunctionProperties getRequiredProperties() const override;
  bool runOnMachineFunction(MachineFunction &F) override;
  void releaseMemory() override { RDI.releaseMemory(); }

  ReachingDefInfo &getRDI() { return RDI; }
  const ReachingDefInfo &getRDI() const { return RDI; }
};

} // namespace llvm

#endif // LLVM_CODEGEN_REACHINGDEFANALYSIS_H
