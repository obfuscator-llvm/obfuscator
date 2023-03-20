//===-- llvm/CodeGen/GlobalISel/CombinerHelper.h --------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===--------------------------------------------------------------------===//
//
/// This contains common combine transformations that may be used in a combine
/// pass,or by the target elsewhere.
/// Targets can pick individual opcode transformations from the helper or use
/// tryCombine which invokes all transformations. All of the transformations
/// return true if the MachineInstruction changed and false otherwise.
//
//===--------------------------------------------------------------------===//

#ifndef LLVM_CODEGEN_GLOBALISEL_COMBINER_HELPER_H
#define LLVM_CODEGEN_GLOBALISEL_COMBINER_HELPER_H

#include "llvm/CodeGen/LowLevelType.h"
#include "llvm/CodeGen/Register.h"

namespace llvm {

class GISelChangeObserver;
class MachineIRBuilder;
class MachineRegisterInfo;
class MachineInstr;
class MachineOperand;

struct PreferredTuple {
  LLT Ty;                // The result type of the extend.
  unsigned ExtendOpcode; // G_ANYEXT/G_SEXT/G_ZEXT
  MachineInstr *MI;
};

class CombinerHelper {
  MachineIRBuilder &Builder;
  MachineRegisterInfo &MRI;
  GISelChangeObserver &Observer;

public:
  CombinerHelper(GISelChangeObserver &Observer, MachineIRBuilder &B);

  /// MachineRegisterInfo::replaceRegWith() and inform the observer of the changes
  void replaceRegWith(MachineRegisterInfo &MRI, Register FromReg, Register ToReg) const;

  /// Replace a single register operand with a new register and inform the
  /// observer of the changes.
  void replaceRegOpWith(MachineRegisterInfo &MRI, MachineOperand &FromRegOp,
                        Register ToReg) const;

  /// If \p MI is COPY, try to combine it.
  /// Returns true if MI changed.
  bool tryCombineCopy(MachineInstr &MI);
  bool matchCombineCopy(MachineInstr &MI);
  void applyCombineCopy(MachineInstr &MI);

  /// If \p MI is extend that consumes the result of a load, try to combine it.
  /// Returns true if MI changed.
  bool tryCombineExtendingLoads(MachineInstr &MI);
  bool matchCombineExtendingLoads(MachineInstr &MI, PreferredTuple &MatchInfo);
  void applyCombineExtendingLoads(MachineInstr &MI, PreferredTuple &MatchInfo);

  bool matchCombineBr(MachineInstr &MI);
  bool tryCombineBr(MachineInstr &MI);

  /// Try to transform \p MI by using all of the above
  /// combine functions. Returns true if changed.
  bool tryCombine(MachineInstr &MI);
};
} // namespace llvm

#endif
