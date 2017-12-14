//===-- BPFMCTargetDesc.h - BPF Target Descriptions -------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file provides BPF specific target descriptions.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_BPF_MCTARGETDESC_BPFMCTARGETDESC_H
#define LLVM_LIB_TARGET_BPF_MCTARGETDESC_BPFMCTARGETDESC_H

#include "llvm/Config/config.h"
#include "llvm/Support/DataTypes.h"

namespace llvm {
class MCAsmBackend;
class MCCodeEmitter;
class MCContext;
class MCInstrInfo;
class MCObjectWriter;
class MCRegisterInfo;
class MCSubtargetInfo;
class MCTargetOptions;
class StringRef;
class Target;
class Triple;
class raw_ostream;
class raw_pwrite_stream;

Target &getTheBPFleTarget();
Target &getTheBPFbeTarget();
Target &getTheBPFTarget();

MCCodeEmitter *createBPFMCCodeEmitter(const MCInstrInfo &MCII,
                                      const MCRegisterInfo &MRI,
                                      MCContext &Ctx);
MCCodeEmitter *createBPFbeMCCodeEmitter(const MCInstrInfo &MCII,
                                        const MCRegisterInfo &MRI,
                                        MCContext &Ctx);

MCAsmBackend *createBPFAsmBackend(const Target &T, const MCRegisterInfo &MRI,
                                  const Triple &TT, StringRef CPU,
                                  const MCTargetOptions &Options);
MCAsmBackend *createBPFbeAsmBackend(const Target &T, const MCRegisterInfo &MRI,
                                    const Triple &TT, StringRef CPU,
                                    const MCTargetOptions &Options);

MCObjectWriter *createBPFELFObjectWriter(raw_pwrite_stream &OS,
                                         uint8_t OSABI, bool IsLittleEndian);
}

// Defines symbolic names for BPF registers.  This defines a mapping from
// register name to register number.
//
#define GET_REGINFO_ENUM
#include "BPFGenRegisterInfo.inc"

// Defines symbolic names for the BPF instructions.
//
#define GET_INSTRINFO_ENUM
#include "BPFGenInstrInfo.inc"

#define GET_SUBTARGETINFO_ENUM
#include "BPFGenSubtargetInfo.inc"

#endif
