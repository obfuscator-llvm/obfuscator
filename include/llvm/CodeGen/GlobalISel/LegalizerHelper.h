//== llvm/CodeGen/GlobalISel/LegalizerHelper.h ---------------- -*- C++ -*-==//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
/// \file A pass to convert the target-illegal operations created by IR -> MIR
/// translation into ones the target expects to be able to select. This may
/// occur in multiple phases, for example G_ADD <2 x i8> -> G_ADD <2 x i16> ->
/// G_ADD <4 x i16>.
///
/// The LegalizerHelper class is where most of the work happens, and is
/// designed to be callable from other passes that find themselves with an
/// illegal instruction.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CODEGEN_GLOBALISEL_MACHINELEGALIZEHELPER_H
#define LLVM_CODEGEN_GLOBALISEL_MACHINELEGALIZEHELPER_H

#include "llvm/CodeGen/GlobalISel/CallLowering.h"
#include "llvm/CodeGen/GlobalISel/MachineIRBuilder.h"
#include "llvm/CodeGen/LowLevelType.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/RuntimeLibcalls.h"

namespace llvm {
// Forward declarations.
class LegalizerInfo;
class Legalizer;
class MachineRegisterInfo;

class LegalizerHelper {
public:
  enum LegalizeResult {
    /// Instruction was already legal and no change was made to the
    /// MachineFunction.
    AlreadyLegal,

    /// Instruction has been legalized and the MachineFunction changed.
    Legalized,

    /// Some kind of error has occurred and we could not legalize this
    /// instruction.
    UnableToLegalize,
  };

  LegalizerHelper(MachineFunction &MF);

  /// Replace \p MI by a sequence of legal instructions that can implement the
  /// same operation. Note that this means \p MI may be deleted, so any iterator
  /// steps should be performed before calling this function. \p Helper should
  /// be initialized to the MachineFunction containing \p MI.
  ///
  /// Considered as an opaque blob, the legal code will use and define the same
  /// registers as \p MI.
  LegalizeResult legalizeInstrStep(MachineInstr &MI);

  /// Legalize an instruction by emiting a runtime library call instead.
  LegalizeResult libcall(MachineInstr &MI);

  /// Legalize an instruction by reducing the width of the underlying scalar
  /// type.
  LegalizeResult narrowScalar(MachineInstr &MI, unsigned TypeIdx, LLT NarrowTy);

  /// Legalize an instruction by performing the operation on a wider scalar type
  /// (for example a 16-bit addition can be safely performed at 32-bits
  /// precision, ignoring the unused bits).
  LegalizeResult widenScalar(MachineInstr &MI, unsigned TypeIdx, LLT WideTy);

  /// Legalize an instruction by splitting it into simpler parts, hopefully
  /// understood by the target.
  LegalizeResult lower(MachineInstr &MI, unsigned TypeIdx, LLT Ty);

  /// Legalize a vector instruction by splitting into multiple components, each
  /// acting on the same scalar type as the original but with fewer elements.
  LegalizeResult fewerElementsVector(MachineInstr &MI, unsigned TypeIdx,
                                     LLT NarrowTy);

  /// Legalize a vector instruction by increasing the number of vector elements
  /// involved and ignoring the added elements later.
  LegalizeResult moreElementsVector(MachineInstr &MI, unsigned TypeIdx,
                                    LLT WideTy);

  /// Expose MIRBuilder so clients can set their own RecordInsertInstruction
  /// functions
  MachineIRBuilder MIRBuilder;

private:

  /// Helper function to split a wide generic register into bitwise blocks with
  /// the given Type (which implies the number of blocks needed). The generic
  /// registers created are appended to Ops, starting at bit 0 of Reg.
  void extractParts(unsigned Reg, LLT Ty, int NumParts,
                    SmallVectorImpl<unsigned> &Ops);

  MachineRegisterInfo &MRI;
  const LegalizerInfo &LI;
};

/// Helper function that creates the given libcall.
LegalizerHelper::LegalizeResult
createLibcall(MachineIRBuilder &MIRBuilder, RTLIB::Libcall Libcall,
              const CallLowering::ArgInfo &Result,
              ArrayRef<CallLowering::ArgInfo> Args);

} // End namespace llvm.

#endif
