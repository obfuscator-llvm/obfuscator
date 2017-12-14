//===-- AMDGPURegisterInfo.h - AMDGPURegisterInfo Interface -*- C++ -*-----===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
/// \file
/// \brief TargetRegisterInfo interface that is implemented by all hw codegen
/// targets.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_AMDGPU_AMDGPUREGISTERINFO_H
#define LLVM_LIB_TARGET_AMDGPU_AMDGPUREGISTERINFO_H

#define GET_REGINFO_HEADER
#include "AMDGPUGenRegisterInfo.inc"

namespace llvm {

class AMDGPUSubtarget;
class TargetInstrInfo;

struct AMDGPURegisterInfo : public AMDGPUGenRegisterInfo {
  AMDGPURegisterInfo();

  /// \returns the sub reg enum value for the given \p Channel
  /// (e.g. getSubRegFromChannel(0) -> AMDGPU::sub0)
  unsigned getSubRegFromChannel(unsigned Channel) const;
};

} // End namespace llvm

#endif
