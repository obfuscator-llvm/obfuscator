//===--- LivePhysRegs.cpp - Live Physical Register Set --------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implements the LivePhysRegs utility for tracking liveness of
// physical registers across machine instructions in forward or backward order.
// A more detailed description can be found in the corresponding header file.
//
//===----------------------------------------------------------------------===//

#include "llvm/CodeGen/LivePhysRegs.h"
#include "llvm/CodeGen/MachineFrameInfo.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineInstrBundle.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"
using namespace llvm;


/// \brief Remove all registers from the set that get clobbered by the register
/// mask.
/// The clobbers set will be the list of live registers clobbered
/// by the regmask.
void LivePhysRegs::removeRegsInMask(const MachineOperand &MO,
        SmallVectorImpl<std::pair<unsigned, const MachineOperand*>> *Clobbers) {
  SparseSet<unsigned>::iterator LRI = LiveRegs.begin();
  while (LRI != LiveRegs.end()) {
    if (MO.clobbersPhysReg(*LRI)) {
      if (Clobbers)
        Clobbers->push_back(std::make_pair(*LRI, &MO));
      LRI = LiveRegs.erase(LRI);
    } else
      ++LRI;
  }
}

/// Simulates liveness when stepping backwards over an instruction(bundle):
/// Remove Defs, add uses. This is the recommended way of calculating liveness.
void LivePhysRegs::stepBackward(const MachineInstr &MI) {
  // Remove defined registers and regmask kills from the set.
  for (ConstMIBundleOperands O(MI); O.isValid(); ++O) {
    if (O->isReg()) {
      if (!O->isDef())
        continue;
      unsigned Reg = O->getReg();
      if (!TargetRegisterInfo::isPhysicalRegister(Reg))
        continue;
      removeReg(Reg);
    } else if (O->isRegMask())
      removeRegsInMask(*O);
  }

  // Add uses to the set.
  for (ConstMIBundleOperands O(MI); O.isValid(); ++O) {
    if (!O->isReg() || !O->readsReg())
      continue;
    unsigned Reg = O->getReg();
    if (!TargetRegisterInfo::isPhysicalRegister(Reg))
      continue;
    addReg(Reg);
  }
}

/// Simulates liveness when stepping forward over an instruction(bundle): Remove
/// killed-uses, add defs. This is the not recommended way, because it depends
/// on accurate kill flags. If possible use stepBackward() instead of this
/// function.
void LivePhysRegs::stepForward(const MachineInstr &MI,
        SmallVectorImpl<std::pair<unsigned, const MachineOperand*>> &Clobbers) {
  // Remove killed registers from the set.
  for (ConstMIBundleOperands O(MI); O.isValid(); ++O) {
    if (O->isReg()) {
      unsigned Reg = O->getReg();
      if (!TargetRegisterInfo::isPhysicalRegister(Reg))
        continue;
      if (O->isDef()) {
        // Note, dead defs are still recorded.  The caller should decide how to
        // handle them.
        Clobbers.push_back(std::make_pair(Reg, &*O));
      } else {
        if (!O->isKill())
          continue;
        assert(O->isUse());
        removeReg(Reg);
      }
    } else if (O->isRegMask())
      removeRegsInMask(*O, &Clobbers);
  }

  // Add defs to the set.
  for (auto Reg : Clobbers) {
    // Skip dead defs.  They shouldn't be added to the set.
    if (Reg.second->isReg() && Reg.second->isDead())
      continue;
    addReg(Reg.first);
  }
}

/// Prin the currently live registers to OS.
void LivePhysRegs::print(raw_ostream &OS) const {
  OS << "Live Registers:";
  if (!TRI) {
    OS << " (uninitialized)\n";
    return;
  }

  if (empty()) {
    OS << " (empty)\n";
    return;
  }

  for (const_iterator I = begin(), E = end(); I != E; ++I)
    OS << " " << PrintReg(*I, TRI);
  OS << "\n";
}

#if !defined(NDEBUG) || defined(LLVM_ENABLE_DUMP)
LLVM_DUMP_METHOD void LivePhysRegs::dump() const {
  dbgs() << "  " << *this;
}
#endif

bool LivePhysRegs::available(const MachineRegisterInfo &MRI,
                             unsigned Reg) const {
  if (LiveRegs.count(Reg))
    return false;
  if (MRI.isReserved(Reg))
    return false;
  for (MCRegAliasIterator R(Reg, TRI, false); R.isValid(); ++R) {
    if (LiveRegs.count(*R))
      return false;
  }
  return true;
}

/// Add live-in registers of basic block \p MBB to \p LiveRegs.
void LivePhysRegs::addBlockLiveIns(const MachineBasicBlock &MBB) {
  for (const auto &LI : MBB.liveins()) {
    unsigned Reg = LI.PhysReg;
    LaneBitmask Mask = LI.LaneMask;
    MCSubRegIndexIterator S(Reg, TRI);
    assert(Mask.any() && "Invalid livein mask");
    if (Mask.all() || !S.isValid()) {
      addReg(Reg);
      continue;
    }
    for (; S.isValid(); ++S) {
      unsigned SI = S.getSubRegIndex();
      if ((Mask & TRI->getSubRegIndexLaneMask(SI)).any())
        addReg(S.getSubReg());
    }
  }
}

/// Adds all callee saved registers to \p LiveRegs.
static void addCalleeSavedRegs(LivePhysRegs &LiveRegs,
                               const MachineFunction &MF) {
  const MachineRegisterInfo &MRI = MF.getRegInfo();
  for (const MCPhysReg *CSR = MRI.getCalleeSavedRegs(); CSR && *CSR; ++CSR)
    LiveRegs.addReg(*CSR);
}

/// Adds pristine registers to the given \p LiveRegs. Pristine registers are
/// callee saved registers that are unused in the function.
static void addPristines(LivePhysRegs &LiveRegs, const MachineFunction &MF) {
  const MachineFrameInfo &MFI = MF.getFrameInfo();
  if (!MFI.isCalleeSavedInfoValid())
    return;
  /// Add all callee saved regs, then remove the ones that are saved+restored.
  addCalleeSavedRegs(LiveRegs, MF);
  /// Remove the ones that are not saved/restored; they are pristine.
  for (const CalleeSavedInfo &Info : MFI.getCalleeSavedInfo())
    LiveRegs.removeReg(Info.getReg());
}

void LivePhysRegs::addLiveOutsNoPristines(const MachineBasicBlock &MBB) {
  if (!MBB.succ_empty()) {
    // To get the live-outs we simply merge the live-ins of all successors.
    for (const MachineBasicBlock *Succ : MBB.successors())
      addBlockLiveIns(*Succ);
  } else if (MBB.isReturnBlock()) {
    // For the return block: Add all callee saved registers that are saved and
    // restored (somewhere); This does not include callee saved registers that
    // are unused and hence not saved and restored; they are called pristine.
    const MachineFunction &MF = *MBB.getParent();
    const MachineFrameInfo &MFI = MF.getFrameInfo();
    if (MFI.isCalleeSavedInfoValid()) {
      for (const CalleeSavedInfo &Info : MFI.getCalleeSavedInfo())
        addReg(Info.getReg());
    }
  }
}

void LivePhysRegs::addLiveOuts(const MachineBasicBlock &MBB) {
  const MachineFunction &MF = *MBB.getParent();
  if (!MBB.succ_empty()) {
    addPristines(*this, MF);
    addLiveOutsNoPristines(MBB);
  } else if (MBB.isReturnBlock()) {
    // For the return block: Add all callee saved registers.
    const MachineFrameInfo &MFI = MF.getFrameInfo();
    if (MFI.isCalleeSavedInfoValid())
      addCalleeSavedRegs(*this, MF);
  }
}

void LivePhysRegs::addLiveIns(const MachineBasicBlock &MBB) {
  const MachineFunction &MF = *MBB.getParent();
  addPristines(*this, MF);
  addBlockLiveIns(MBB);
}

void llvm::computeLiveIns(LivePhysRegs &LiveRegs,
                          const MachineRegisterInfo &MRI,
                          MachineBasicBlock &MBB) {
  const TargetRegisterInfo &TRI = *MRI.getTargetRegisterInfo();
  assert(MBB.livein_empty());
  LiveRegs.init(TRI);
  LiveRegs.addLiveOutsNoPristines(MBB);
  for (MachineInstr &MI : make_range(MBB.rbegin(), MBB.rend()))
    LiveRegs.stepBackward(MI);

  for (unsigned Reg : LiveRegs) {
    if (MRI.isReserved(Reg))
      continue;
    // Skip the register if we are about to add one of its super registers.
    bool ContainsSuperReg = false;
    for (MCSuperRegIterator SReg(Reg, &TRI); SReg.isValid(); ++SReg) {
      if (LiveRegs.contains(*SReg) && !MRI.isReserved(*SReg)) {
        ContainsSuperReg = true;
        break;
      }
    }
    if (ContainsSuperReg)
      continue;
    MBB.addLiveIn(Reg);
  }
}
