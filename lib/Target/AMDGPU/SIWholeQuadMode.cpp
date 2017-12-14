//===-- SIWholeQuadMode.cpp - enter and suspend whole quad mode -----------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
/// \file
/// \brief This pass adds instructions to enable whole quad mode for pixel
/// shaders.
///
/// Whole quad mode is required for derivative computations, but it interferes
/// with shader side effects (stores and atomics). This pass is run on the
/// scheduled machine IR but before register coalescing, so that machine SSA is
/// available for analysis. It ensures that WQM is enabled when necessary, but
/// disabled around stores and atomics.
///
/// When necessary, this pass creates a function prolog
///
///   S_MOV_B64 LiveMask, EXEC
///   S_WQM_B64 EXEC, EXEC
///
/// to enter WQM at the top of the function and surrounds blocks of Exact
/// instructions by
///
///   S_AND_SAVEEXEC_B64 Tmp, LiveMask
///   ...
///   S_MOV_B64 EXEC, Tmp
///
/// In order to avoid excessive switching during sequences of Exact
/// instructions, the pass first analyzes which instructions must be run in WQM
/// (aka which instructions produce values that lead to derivative
/// computations).
///
/// Basic blocks are always exited in WQM as long as some successor needs WQM.
///
/// There is room for improvement given better control flow analysis:
///
///  (1) at the top level (outside of control flow statements, and as long as
///      kill hasn't been used), one SGPR can be saved by recovering WQM from
///      the LiveMask (this is implemented for the entry block).
///
///  (2) when entire regions (e.g. if-else blocks or entire loops) only
///      consist of exact and don't-care instructions, the switch only has to
///      be done at the entry and exit points rather than potentially in each
///      block of the region.
///
//===----------------------------------------------------------------------===//

#include "AMDGPU.h"
#include "AMDGPUSubtarget.h"
#include "SIInstrInfo.h"
#include "SIMachineFunctionInfo.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/CodeGen/LiveInterval.h"
#include "llvm/CodeGen/LiveIntervalAnalysis.h"
#include "llvm/CodeGen/MachineBasicBlock.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineInstr.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/MachineOperand.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/CodeGen/SlotIndexes.h"
#include "llvm/IR/CallingConv.h"
#include "llvm/IR/DebugLoc.h"
#include "llvm/MC/MCRegisterInfo.h"
#include "llvm/Pass.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Target/TargetRegisterInfo.h"
#include <cassert>
#include <vector>

using namespace llvm;

#define DEBUG_TYPE "si-wqm"

namespace {

enum {
  StateWQM = 0x1,
  StateExact = 0x2,
};

struct PrintState {
public:
  int State;

  explicit PrintState(int State) : State(State) {}
};

static raw_ostream &operator<<(raw_ostream &OS, const PrintState &PS) {
  if (PS.State & StateWQM)
    OS << "WQM";
  if (PS.State & StateExact) {
    if (PS.State & StateWQM)
      OS << '|';
    OS << "Exact";
  }

  return OS;
}

struct InstrInfo {
  char Needs = 0;
  char OutNeeds = 0;
};

struct BlockInfo {
  char Needs = 0;
  char InNeeds = 0;
  char OutNeeds = 0;
};

struct WorkItem {
  MachineBasicBlock *MBB = nullptr;
  MachineInstr *MI = nullptr;

  WorkItem() = default;
  WorkItem(MachineBasicBlock *MBB) : MBB(MBB) {}
  WorkItem(MachineInstr *MI) : MI(MI) {}
};

class SIWholeQuadMode : public MachineFunctionPass {
private:
  const SIInstrInfo *TII;
  const SIRegisterInfo *TRI;
  MachineRegisterInfo *MRI;
  LiveIntervals *LIS;

  DenseMap<const MachineInstr *, InstrInfo> Instructions;
  DenseMap<MachineBasicBlock *, BlockInfo> Blocks;
  SmallVector<MachineInstr *, 1> LiveMaskQueries;

  void printInfo();

  void markInstruction(MachineInstr &MI, char Flag,
                       std::vector<WorkItem> &Worklist);
  void markUsesWQM(const MachineInstr &MI, std::vector<WorkItem> &Worklist);
  char scanInstructions(MachineFunction &MF, std::vector<WorkItem> &Worklist);
  void propagateInstruction(MachineInstr &MI, std::vector<WorkItem> &Worklist);
  void propagateBlock(MachineBasicBlock &MBB, std::vector<WorkItem> &Worklist);
  char analyzeFunction(MachineFunction &MF);

  bool requiresCorrectState(const MachineInstr &MI) const;

  MachineBasicBlock::iterator saveSCC(MachineBasicBlock &MBB,
                                      MachineBasicBlock::iterator Before);
  MachineBasicBlock::iterator
  prepareInsertion(MachineBasicBlock &MBB, MachineBasicBlock::iterator First,
                   MachineBasicBlock::iterator Last, bool PreferLast,
                   bool SaveSCC);
  void toExact(MachineBasicBlock &MBB, MachineBasicBlock::iterator Before,
               unsigned SaveWQM, unsigned LiveMaskReg);
  void toWQM(MachineBasicBlock &MBB, MachineBasicBlock::iterator Before,
             unsigned SavedWQM);
  void processBlock(MachineBasicBlock &MBB, unsigned LiveMaskReg, bool isEntry);

  void lowerLiveMaskQueries(unsigned LiveMaskReg);

public:
  static char ID;

  SIWholeQuadMode() :
    MachineFunctionPass(ID) { }

  bool runOnMachineFunction(MachineFunction &MF) override;

  StringRef getPassName() const override { return "SI Whole Quad Mode"; }

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.addRequired<LiveIntervals>();
    AU.setPreservesCFG();
    MachineFunctionPass::getAnalysisUsage(AU);
  }
};

} // end anonymous namespace

char SIWholeQuadMode::ID = 0;

INITIALIZE_PASS_BEGIN(SIWholeQuadMode, DEBUG_TYPE, "SI Whole Quad Mode", false,
                      false)
INITIALIZE_PASS_DEPENDENCY(LiveIntervals)
INITIALIZE_PASS_END(SIWholeQuadMode, DEBUG_TYPE, "SI Whole Quad Mode", false,
                    false)

char &llvm::SIWholeQuadModeID = SIWholeQuadMode::ID;

FunctionPass *llvm::createSIWholeQuadModePass() {
  return new SIWholeQuadMode;
}

void SIWholeQuadMode::printInfo() {
  for (const auto &BII : Blocks) {
    dbgs() << "\nBB#" << BII.first->getNumber() << ":\n"
           << "  InNeeds = " << PrintState(BII.second.InNeeds)
           << ", Needs = " << PrintState(BII.second.Needs)
           << ", OutNeeds = " << PrintState(BII.second.OutNeeds) << "\n\n";

    for (const MachineInstr &MI : *BII.first) {
      auto III = Instructions.find(&MI);
      if (III == Instructions.end())
        continue;

      dbgs() << "  " << MI << "    Needs = " << PrintState(III->second.Needs)
             << ", OutNeeds = " << PrintState(III->second.OutNeeds) << '\n';
    }
  }
}

void SIWholeQuadMode::markInstruction(MachineInstr &MI, char Flag,
                                      std::vector<WorkItem> &Worklist) {
  InstrInfo &II = Instructions[&MI];

  assert(Flag == StateWQM || Flag == StateExact);

  // Ignore if the instruction is already marked. The typical case is that we
  // mark an instruction WQM multiple times, but for atomics it can happen that
  // Flag is StateWQM, but Needs is already set to StateExact. In this case,
  // letting the atomic run in StateExact is correct as per the relevant specs.
  if (II.Needs)
    return;

  II.Needs = Flag;
  Worklist.push_back(&MI);
}

/// Mark all instructions defining the uses in \p MI as WQM.
void SIWholeQuadMode::markUsesWQM(const MachineInstr &MI,
                                  std::vector<WorkItem> &Worklist) {
  for (const MachineOperand &Use : MI.uses()) {
    if (!Use.isReg() || !Use.isUse())
      continue;

    unsigned Reg = Use.getReg();

    // Handle physical registers that we need to track; this is mostly relevant
    // for VCC, which can appear as the (implicit) input of a uniform branch,
    // e.g. when a loop counter is stored in a VGPR.
    if (!TargetRegisterInfo::isVirtualRegister(Reg)) {
      if (Reg == AMDGPU::EXEC)
        continue;

      for (MCRegUnitIterator RegUnit(Reg, TRI); RegUnit.isValid(); ++RegUnit) {
        LiveRange &LR = LIS->getRegUnit(*RegUnit);
        const VNInfo *Value = LR.Query(LIS->getInstructionIndex(MI)).valueIn();
        if (!Value)
          continue;

        // Since we're in machine SSA, we do not need to track physical
        // registers across basic blocks.
        if (Value->isPHIDef())
          continue;

        markInstruction(*LIS->getInstructionFromIndex(Value->def), StateWQM,
                        Worklist);
      }

      continue;
    }

    for (MachineInstr &DefMI : MRI->def_instructions(Use.getReg()))
      markInstruction(DefMI, StateWQM, Worklist);
  }
}

// Scan instructions to determine which ones require an Exact execmask and
// which ones seed WQM requirements.
char SIWholeQuadMode::scanInstructions(MachineFunction &MF,
                                       std::vector<WorkItem> &Worklist) {
  char GlobalFlags = 0;
  bool WQMOutputs = MF.getFunction()->hasFnAttribute("amdgpu-ps-wqm-outputs");

  for (auto BI = MF.begin(), BE = MF.end(); BI != BE; ++BI) {
    MachineBasicBlock &MBB = *BI;

    for (auto II = MBB.begin(), IE = MBB.end(); II != IE; ++II) {
      MachineInstr &MI = *II;
      unsigned Opcode = MI.getOpcode();
      char Flags = 0;

      if (TII->isDS(Opcode)) {
        Flags = StateWQM;
      } else if (TII->isWQM(Opcode)) {
        // Sampling instructions don't need to produce results for all pixels
        // in a quad, they just require all inputs of a quad to have been
        // computed for derivatives.
        markUsesWQM(MI, Worklist);
        GlobalFlags |= StateWQM;
        continue;
      } else if (TII->isDisableWQM(MI)) {
        Flags = StateExact;
      } else {
        if (Opcode == AMDGPU::SI_PS_LIVE) {
          LiveMaskQueries.push_back(&MI);
        } else if (WQMOutputs) {
          // The function is in machine SSA form, which means that physical
          // VGPRs correspond to shader inputs and outputs. Inputs are
          // only used, outputs are only defined.
          for (const MachineOperand &MO : MI.defs()) {
            if (!MO.isReg())
              continue;

            unsigned Reg = MO.getReg();

            if (!TRI->isVirtualRegister(Reg) &&
                TRI->hasVGPRs(TRI->getPhysRegClass(Reg))) {
              Flags = StateWQM;
              break;
            }
          }
        }

        if (!Flags)
          continue;
      }

      markInstruction(MI, Flags, Worklist);
      GlobalFlags |= Flags;
    }
  }

  return GlobalFlags;
}

void SIWholeQuadMode::propagateInstruction(MachineInstr &MI,
                                           std::vector<WorkItem>& Worklist) {
  MachineBasicBlock *MBB = MI.getParent();
  InstrInfo II = Instructions[&MI]; // take a copy to prevent dangling references
  BlockInfo &BI = Blocks[MBB];

  // Control flow-type instructions and stores to temporary memory that are
  // followed by WQM computations must themselves be in WQM.
  if ((II.OutNeeds & StateWQM) && !II.Needs &&
      (MI.isTerminator() || (TII->usesVM_CNT(MI) && MI.mayStore()))) {
    Instructions[&MI].Needs = StateWQM;
    II.Needs = StateWQM;
  }

  // Propagate to block level
  BI.Needs |= II.Needs;
  if ((BI.InNeeds | II.Needs) != BI.InNeeds) {
    BI.InNeeds |= II.Needs;
    Worklist.push_back(MBB);
  }

  // Propagate backwards within block
  if (MachineInstr *PrevMI = MI.getPrevNode()) {
    char InNeeds = II.Needs | II.OutNeeds;
    if (!PrevMI->isPHI()) {
      InstrInfo &PrevII = Instructions[PrevMI];
      if ((PrevII.OutNeeds | InNeeds) != PrevII.OutNeeds) {
        PrevII.OutNeeds |= InNeeds;
        Worklist.push_back(PrevMI);
      }
    }
  }

  // Propagate WQM flag to instruction inputs
  assert(II.Needs != (StateWQM | StateExact));

  if (II.Needs == StateWQM)
    markUsesWQM(MI, Worklist);
}

void SIWholeQuadMode::propagateBlock(MachineBasicBlock &MBB,
                                     std::vector<WorkItem>& Worklist) {
  BlockInfo BI = Blocks[&MBB]; // Make a copy to prevent dangling references.

  // Propagate through instructions
  if (!MBB.empty()) {
    MachineInstr *LastMI = &*MBB.rbegin();
    InstrInfo &LastII = Instructions[LastMI];
    if ((LastII.OutNeeds | BI.OutNeeds) != LastII.OutNeeds) {
      LastII.OutNeeds |= BI.OutNeeds;
      Worklist.push_back(LastMI);
    }
  }

  // Predecessor blocks must provide for our WQM/Exact needs.
  for (MachineBasicBlock *Pred : MBB.predecessors()) {
    BlockInfo &PredBI = Blocks[Pred];
    if ((PredBI.OutNeeds | BI.InNeeds) == PredBI.OutNeeds)
      continue;

    PredBI.OutNeeds |= BI.InNeeds;
    PredBI.InNeeds |= BI.InNeeds;
    Worklist.push_back(Pred);
  }

  // All successors must be prepared to accept the same set of WQM/Exact data.
  for (MachineBasicBlock *Succ : MBB.successors()) {
    BlockInfo &SuccBI = Blocks[Succ];
    if ((SuccBI.InNeeds | BI.OutNeeds) == SuccBI.InNeeds)
      continue;

    SuccBI.InNeeds |= BI.OutNeeds;
    Worklist.push_back(Succ);
  }
}

char SIWholeQuadMode::analyzeFunction(MachineFunction &MF) {
  std::vector<WorkItem> Worklist;
  char GlobalFlags = scanInstructions(MF, Worklist);

  while (!Worklist.empty()) {
    WorkItem WI = Worklist.back();
    Worklist.pop_back();

    if (WI.MI)
      propagateInstruction(*WI.MI, Worklist);
    else
      propagateBlock(*WI.MBB, Worklist);
  }

  return GlobalFlags;
}

/// Whether \p MI really requires the exec state computed during analysis.
///
/// Scalar instructions must occasionally be marked WQM for correct propagation
/// (e.g. thread masks leading up to branches), but when it comes to actual
/// execution, they don't care about EXEC.
bool SIWholeQuadMode::requiresCorrectState(const MachineInstr &MI) const {
  if (MI.isTerminator())
    return true;

  // Skip instructions that are not affected by EXEC
  if (TII->isScalarUnit(MI))
    return false;

  // Generic instructions such as COPY will either disappear by register
  // coalescing or be lowered to SALU or VALU instructions.
  if (MI.isTransient()) {
    if (MI.getNumExplicitOperands() >= 1) {
      const MachineOperand &Op = MI.getOperand(0);
      if (Op.isReg()) {
        if (TRI->isSGPRReg(*MRI, Op.getReg())) {
          // SGPR instructions are not affected by EXEC
          return false;
        }
      }
    }
  }

  return true;
}

MachineBasicBlock::iterator
SIWholeQuadMode::saveSCC(MachineBasicBlock &MBB,
                         MachineBasicBlock::iterator Before) {
  unsigned SaveReg = MRI->createVirtualRegister(&AMDGPU::SReg_32_XM0RegClass);

  MachineInstr *Save =
      BuildMI(MBB, Before, DebugLoc(), TII->get(AMDGPU::COPY), SaveReg)
          .addReg(AMDGPU::SCC);
  MachineInstr *Restore =
      BuildMI(MBB, Before, DebugLoc(), TII->get(AMDGPU::COPY), AMDGPU::SCC)
          .addReg(SaveReg);

  LIS->InsertMachineInstrInMaps(*Save);
  LIS->InsertMachineInstrInMaps(*Restore);
  LIS->createAndComputeVirtRegInterval(SaveReg);

  return Restore;
}

// Return an iterator in the (inclusive) range [First, Last] at which
// instructions can be safely inserted, keeping in mind that some of the
// instructions we want to add necessarily clobber SCC.
MachineBasicBlock::iterator SIWholeQuadMode::prepareInsertion(
    MachineBasicBlock &MBB, MachineBasicBlock::iterator First,
    MachineBasicBlock::iterator Last, bool PreferLast, bool SaveSCC) {
  if (!SaveSCC)
    return PreferLast ? Last : First;

  LiveRange &LR = LIS->getRegUnit(*MCRegUnitIterator(AMDGPU::SCC, TRI));
  auto MBBE = MBB.end();
  SlotIndex FirstIdx = First != MBBE ? LIS->getInstructionIndex(*First)
                                     : LIS->getMBBEndIdx(&MBB);
  SlotIndex LastIdx =
      Last != MBBE ? LIS->getInstructionIndex(*Last) : LIS->getMBBEndIdx(&MBB);
  SlotIndex Idx = PreferLast ? LastIdx : FirstIdx;
  const LiveRange::Segment *S;

  for (;;) {
    S = LR.getSegmentContaining(Idx);
    if (!S)
      break;

    if (PreferLast) {
      SlotIndex Next = S->start.getBaseIndex();
      if (Next < FirstIdx)
        break;
      Idx = Next;
    } else {
      SlotIndex Next = S->end.getNextIndex().getBaseIndex();
      if (Next > LastIdx)
        break;
      Idx = Next;
    }
  }

  MachineBasicBlock::iterator MBBI;

  if (MachineInstr *MI = LIS->getInstructionFromIndex(Idx))
    MBBI = MI;
  else {
    assert(Idx == LIS->getMBBEndIdx(&MBB));
    MBBI = MBB.end();
  }

  if (S)
    MBBI = saveSCC(MBB, MBBI);

  return MBBI;
}

void SIWholeQuadMode::toExact(MachineBasicBlock &MBB,
                              MachineBasicBlock::iterator Before,
                              unsigned SaveWQM, unsigned LiveMaskReg) {
  MachineInstr *MI;

  if (SaveWQM) {
    MI = BuildMI(MBB, Before, DebugLoc(), TII->get(AMDGPU::S_AND_SAVEEXEC_B64),
                 SaveWQM)
             .addReg(LiveMaskReg);
  } else {
    MI = BuildMI(MBB, Before, DebugLoc(), TII->get(AMDGPU::S_AND_B64),
                 AMDGPU::EXEC)
             .addReg(AMDGPU::EXEC)
             .addReg(LiveMaskReg);
  }

  LIS->InsertMachineInstrInMaps(*MI);
}

void SIWholeQuadMode::toWQM(MachineBasicBlock &MBB,
                            MachineBasicBlock::iterator Before,
                            unsigned SavedWQM) {
  MachineInstr *MI;

  if (SavedWQM) {
    MI = BuildMI(MBB, Before, DebugLoc(), TII->get(AMDGPU::COPY), AMDGPU::EXEC)
             .addReg(SavedWQM);
  } else {
    MI = BuildMI(MBB, Before, DebugLoc(), TII->get(AMDGPU::S_WQM_B64),
                 AMDGPU::EXEC)
             .addReg(AMDGPU::EXEC);
  }

  LIS->InsertMachineInstrInMaps(*MI);
}

void SIWholeQuadMode::processBlock(MachineBasicBlock &MBB, unsigned LiveMaskReg,
                                   bool isEntry) {
  auto BII = Blocks.find(&MBB);
  if (BII == Blocks.end())
    return;

  const BlockInfo &BI = BII->second;

  if (!(BI.InNeeds & StateWQM))
    return;

  // This is a non-entry block that is WQM throughout, so no need to do
  // anything.
  if (!isEntry && !(BI.Needs & StateExact) && BI.OutNeeds != StateExact)
    return;

  DEBUG(dbgs() << "\nProcessing block BB#" << MBB.getNumber() << ":\n");

  unsigned SavedWQMReg = 0;
  bool WQMFromExec = isEntry;
  char State = isEntry ? StateExact : StateWQM;

  auto II = MBB.getFirstNonPHI(), IE = MBB.end();
  if (isEntry)
    ++II; // Skip the instruction that saves LiveMask

  MachineBasicBlock::iterator First = IE;
  for (;;) {
    MachineBasicBlock::iterator Next = II;
    char Needs = 0;
    char OutNeeds = 0;

    if (First == IE)
      First = II;

    if (II != IE) {
      MachineInstr &MI = *II;

      if (requiresCorrectState(MI)) {
        auto III = Instructions.find(&MI);
        if (III != Instructions.end()) {
          Needs = III->second.Needs;
          OutNeeds = III->second.OutNeeds;
        }
      }

      if (MI.isTerminator() && !Needs && OutNeeds == StateExact)
        Needs = StateExact;

      if (MI.getOpcode() == AMDGPU::SI_ELSE && BI.OutNeeds == StateExact)
        MI.getOperand(3).setImm(1);

      ++Next;
    } else {
      // End of basic block
      if (BI.OutNeeds & StateWQM)
        Needs = StateWQM;
      else if (BI.OutNeeds == StateExact)
        Needs = StateExact;
    }

    if (Needs) {
      if (Needs != State) {
        MachineBasicBlock::iterator Before =
            prepareInsertion(MBB, First, II, Needs == StateWQM,
                             Needs == StateExact || WQMFromExec);

        if (Needs == StateExact) {
          if (!WQMFromExec && (OutNeeds & StateWQM))
            SavedWQMReg = MRI->createVirtualRegister(&AMDGPU::SReg_64RegClass);

          toExact(MBB, Before, SavedWQMReg, LiveMaskReg);
        } else {
          assert(WQMFromExec == (SavedWQMReg == 0));

          toWQM(MBB, Before, SavedWQMReg);

          if (SavedWQMReg) {
            LIS->createAndComputeVirtRegInterval(SavedWQMReg);
            SavedWQMReg = 0;
          }
        }

        State = Needs;
      }

      First = IE;
    }

    if (II == IE)
      break;
    II = Next;
  }
}

void SIWholeQuadMode::lowerLiveMaskQueries(unsigned LiveMaskReg) {
  for (MachineInstr *MI : LiveMaskQueries) {
    const DebugLoc &DL = MI->getDebugLoc();
    unsigned Dest = MI->getOperand(0).getReg();
    MachineInstr *Copy =
        BuildMI(*MI->getParent(), MI, DL, TII->get(AMDGPU::COPY), Dest)
            .addReg(LiveMaskReg);

    LIS->ReplaceMachineInstrInMaps(*MI, *Copy);
    MI->eraseFromParent();
  }
}

bool SIWholeQuadMode::runOnMachineFunction(MachineFunction &MF) {
  if (MF.getFunction()->getCallingConv() != CallingConv::AMDGPU_PS)
    return false;

  Instructions.clear();
  Blocks.clear();
  LiveMaskQueries.clear();

  const SISubtarget &ST = MF.getSubtarget<SISubtarget>();

  TII = ST.getInstrInfo();
  TRI = &TII->getRegisterInfo();
  MRI = &MF.getRegInfo();
  LIS = &getAnalysis<LiveIntervals>();

  char GlobalFlags = analyzeFunction(MF);
  if (!(GlobalFlags & StateWQM)) {
    lowerLiveMaskQueries(AMDGPU::EXEC);
    return !LiveMaskQueries.empty();
  }

  // Store a copy of the original live mask when required
  unsigned LiveMaskReg = 0;
  {
    MachineBasicBlock &Entry = MF.front();
    MachineBasicBlock::iterator EntryMI = Entry.getFirstNonPHI();

    if (GlobalFlags & StateExact || !LiveMaskQueries.empty()) {
      LiveMaskReg = MRI->createVirtualRegister(&AMDGPU::SReg_64RegClass);
      MachineInstr *MI = BuildMI(Entry, EntryMI, DebugLoc(),
                                 TII->get(AMDGPU::COPY), LiveMaskReg)
                             .addReg(AMDGPU::EXEC);
      LIS->InsertMachineInstrInMaps(*MI);
    }

    if (GlobalFlags == StateWQM) {
      // For a shader that needs only WQM, we can just set it once.
      BuildMI(Entry, EntryMI, DebugLoc(), TII->get(AMDGPU::S_WQM_B64),
              AMDGPU::EXEC)
          .addReg(AMDGPU::EXEC);

      lowerLiveMaskQueries(LiveMaskReg);
      // EntryMI may become invalid here
      return true;
    }
  }

  DEBUG(printInfo());

  lowerLiveMaskQueries(LiveMaskReg);

  // Handle the general case
  for (auto BII : Blocks)
    processBlock(*BII.first, LiveMaskReg, BII.first == &*MF.begin());

  // Physical registers like SCC aren't tracked by default anyway, so just
  // removing the ranges we computed is the simplest option for maintaining
  // the analysis results.
  LIS->removeRegUnit(*MCRegUnitIterator(AMDGPU::SCC, TRI));

  return true;
}
