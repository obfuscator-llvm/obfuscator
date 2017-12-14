//===-- AMDGPUInstrInfo.h - AMDGPU Instruction Information ------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
/// \file
/// \brief Contains the definition of a TargetInstrInfo class that is common
/// to all AMD GPUs.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_AMDGPU_AMDGPUINSTRINFO_H
#define LLVM_LIB_TARGET_AMDGPU_AMDGPUINSTRINFO_H

#include "AMDGPU.h"
#include "Utils/AMDGPUBaseInfo.h"
#include "llvm/Target/TargetInstrInfo.h"

#define GET_INSTRINFO_HEADER
#include "AMDGPUGenInstrInfo.inc"

namespace llvm {

class AMDGPUSubtarget;
class MachineFunction;
class MachineInstr;
class MachineInstrBuilder;

class AMDGPUInstrInfo : public AMDGPUGenInstrInfo {
private:
  const AMDGPUSubtarget &ST;

  virtual void anchor();
protected:
  AMDGPUAS AMDGPUASI;

public:
  explicit AMDGPUInstrInfo(const AMDGPUSubtarget &st);

  bool shouldScheduleLoadsNear(SDNode *Load1, SDNode *Load2,
                               int64_t Offset1, int64_t Offset2,
                               unsigned NumLoads) const override;

  /// \brief Return a target-specific opcode if Opcode is a pseudo instruction.
  /// Return -1 if the target-specific opcode for the pseudo instruction does
  /// not exist. If Opcode is not a pseudo instruction, this is identity.
  int pseudoToMCOpcode(int Opcode) const;

  /// \brief Given a MIMG \p Opcode that writes all 4 channels, return the
  /// equivalent opcode that writes \p Channels Channels.
  int getMaskedMIMGOp(uint16_t Opcode, unsigned Channels) const;
};
} // End llvm namespace

#endif
