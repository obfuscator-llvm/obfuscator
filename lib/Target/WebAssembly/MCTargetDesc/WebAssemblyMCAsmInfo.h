//===-- WebAssemblyMCAsmInfo.h - WebAssembly asm properties -----*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
///
/// \file
/// \brief This file contains the declaration of the WebAssemblyMCAsmInfo class.
///
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_WEBASSEMBLY_MCTARGETDESC_WEBASSEMBLYMCASMINFO_H
#define LLVM_LIB_TARGET_WEBASSEMBLY_MCTARGETDESC_WEBASSEMBLYMCASMINFO_H

#include "llvm/MC/MCAsmInfoELF.h"
#include "llvm/MC/MCAsmInfoWasm.h"

namespace llvm {

class Triple;

class WebAssemblyMCAsmInfoELF final : public MCAsmInfoELF {
public:
  explicit WebAssemblyMCAsmInfoELF(const Triple &T);
  ~WebAssemblyMCAsmInfoELF() override;
};

class WebAssemblyMCAsmInfo final : public MCAsmInfoWasm {
public:
  explicit WebAssemblyMCAsmInfo(const Triple &T);
  ~WebAssemblyMCAsmInfo() override;
};

} // end namespace llvm

#endif
