//===- llvm/CodeGen/LivePhysRegs.h - Live Physical Register Set -*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
/// \file
/// This file implements the LivePhysRegs utility for tracking liveness of
/// physical registers. This can be used for ad-hoc liveness tracking after
/// register allocation. You can start with the live-ins/live-outs at the
/// beginning/end of a block and update the information while walking the
/// instructions inside the block. This implementation tracks the liveness on a
/// sub-register granularity.
///
/// We assume that the high bits of a physical super-register are not preserved
/// unless the instruction has an implicit-use operand reading the super-
/// register.
///
/// X86 Example:
/// %YMM0<def> = ...
/// %XMM0<def> = ... (Kills %XMM0, all %XMM0s sub-registers, and %YMM0)
///
/// %YMM0<def> = ...
/// %XMM0<def> = ..., %YMM0<imp-use> (%YMM0 and all its sub-registers are alive)
//===----------------------------------------------------------------------===//

#ifndef LLVM_CODEGEN_LIVEPHYSREGS_H
#define LLVM_CODEGEN_LIVEPHYSREGS_H

#include "llvm/ADT/SparseSet.h"
#include "llvm/CodeGen/MachineBasicBlock.h"
#include "llvm/MC/MCRegisterInfo.h"
#include "llvm/Target/TargetRegisterInfo.h"
#include <cassert>
#include <utility>

namespace llvm {

class MachineInstr;
class MachineOperand;
class MachineRegisterInfo;
class raw_ostream;

/// \brief A set of physical registers with utility functions to track liveness
/// when walking backward/forward through a basic block.
class LivePhysRegs {
  const TargetRegisterInfo *TRI = nullptr;
  SparseSet<unsigned> LiveRegs;

public:
  /// Constructs an unitialized set. init() needs to be called to initialize it.
  LivePhysRegs() = default;

  /// Constructs and initializes an empty set.
  LivePhysRegs(const TargetRegisterInfo &TRI) : TRI(&TRI) {
    LiveRegs.setUniverse(TRI.getNumRegs());
  }

  LivePhysRegs(const LivePhysRegs&) = delete;
  LivePhysRegs &operator=(const LivePhysRegs&) = delete;

  /// (re-)initializes and clears the set.
  void init(const TargetRegisterInfo &TRI) {
    this->TRI = &TRI;
    LiveRegs.clear();
    LiveRegs.setUniverse(TRI.getNumRegs());
  }

  /// Clears the set.
  void clear() { LiveRegs.clear(); }

  /// Returns true if the set is empty.
  bool empty() const { return LiveRegs.empty(); }

  /// Adds a physical register and all its sub-registers to the set.
  void addReg(unsigned Reg) {
    assert(TRI && "LivePhysRegs is not initialized.");
    assert(Reg <= TRI->getNumRegs() && "Expected a physical register.");
    for (MCSubRegIterator SubRegs(Reg, TRI, /*IncludeSelf=*/true);
         SubRegs.isValid(); ++SubRegs)
      LiveRegs.insert(*SubRegs);
  }

  /// \brief Removes a physical register, all its sub-registers, and all its
  /// super-registers from the set.
  void removeReg(unsigned Reg) {
    assert(TRI && "LivePhysRegs is not initialized.");
    assert(Reg <= TRI->getNumRegs() && "Expected a physical register.");
    for (MCRegAliasIterator R(Reg, TRI, true); R.isValid(); ++R)
      LiveRegs.erase(*R);
  }

  /// Removes physical registers clobbered by the regmask operand \p MO.
  void removeRegsInMask(const MachineOperand &MO,
        SmallVectorImpl<std::pair<unsigned, const MachineOperand*>> *Clobbers =
        nullptr);

  /// \brief Returns true if register \p Reg is contained in the set. This also
  /// works if only the super register of \p Reg has been defined, because
  /// addReg() always adds all sub-registers to the set as well.
  /// Note: Returns false if just some sub registers are live, use available()
  /// when searching a free register.
  bool contains(unsigned Reg) const { return LiveRegs.count(Reg); }

  /// Returns true if register \p Reg and no aliasing register is in the set.
  bool available(const MachineRegisterInfo &MRI, unsigned Reg) const;

  /// Simulates liveness when stepping backwards over an instruction(bundle).
  /// Remove Defs, add uses. This is the recommended way of calculating
  /// liveness.
  void stepBackward(const MachineInstr &MI);

  /// Simulates liveness when stepping forward over an instruction(bundle).
  /// Remove killed-uses, add defs. This is the not recommended way, because it
  /// depends on accurate kill flags. If possible use stepBackward() instead of
  /// this function. The clobbers set will be the list of registers either
  /// defined or clobbered by a regmask.  The operand will identify whether this
  /// is a regmask or register operand.
  void stepForward(const MachineInstr &MI,
        SmallVectorImpl<std::pair<unsigned, const MachineOperand*>> &Clobbers);

  /// Adds all live-in registers of basic block \p MBB.
  /// Live in registers are the registers in the blocks live-in list and the
  /// pristine registers.
  void addLiveIns(const MachineBasicBlock &MBB);

  /// Adds all live-out registers of basic block \p MBB.
  /// Live out registers are the union of the live-in registers of the successor
  /// blocks and pristine registers. Live out registers of the end block are the
  /// callee saved registers.
  void addLiveOuts(const MachineBasicBlock &MBB);

  /// Adds all live-out registers of basic block \p MBB but skips pristine
  /// registers.
  void addLiveOutsNoPristines(const MachineBasicBlock &MBB);

  using const_iterator = SparseSet<unsigned>::const_iterator;

  const_iterator begin() const { return LiveRegs.begin(); }
  const_iterator end() const { return LiveRegs.end(); }

  /// Prints the currently live registers to \p OS.
  void print(raw_ostream &OS) const;

  /// Dumps the currently live registers to the debug output.
  void dump() const;

private:
  /// \brief Adds live-in registers from basic block \p MBB, taking associated
  /// lane masks into consideration.
  void addBlockLiveIns(const MachineBasicBlock &MBB);
};

inline raw_ostream &operator<<(raw_ostream &OS, const LivePhysRegs& LR) {
  LR.print(OS);
  return OS;
}

/// \brief Computes the live-in list for \p MBB assuming all of its successors
/// live-in lists are up-to-date. Uses the given LivePhysReg instance \p
/// LiveRegs; This is just here to avoid repeated heap allocations when calling
/// this multiple times in a pass.
void computeLiveIns(LivePhysRegs &LiveRegs, const MachineRegisterInfo &MRI,
                    MachineBasicBlock &MBB);

} // end namespace llvm

#endif // LLVM_CODEGEN_LIVEPHYSREGS_H
