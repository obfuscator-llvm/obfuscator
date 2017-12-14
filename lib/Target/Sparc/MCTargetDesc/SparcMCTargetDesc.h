//===-- SparcMCTargetDesc.h - Sparc Target Descriptions ---------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file provides Sparc specific target descriptions.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_SPARC_MCTARGETDESC_SPARCMCTARGETDESC_H
#define LLVM_LIB_TARGET_SPARC_MCTARGETDESC_SPARCMCTARGETDESC_H

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
class Target;
class Triple;
class StringRef;
class raw_pwrite_stream;
class raw_ostream;

Target &getTheSparcTarget();
Target &getTheSparcV9Target();
Target &getTheSparcelTarget();

MCCodeEmitter *createSparcMCCodeEmitter(const MCInstrInfo &MCII,
                                        const MCRegisterInfo &MRI,
                                        MCContext &Ctx);
MCAsmBackend *createSparcAsmBackend(const Target &T, const MCRegisterInfo &MRI,
                                    const Triple &TT, StringRef CPU,
                                    const MCTargetOptions &Options);
MCObjectWriter *createSparcELFObjectWriter(raw_pwrite_stream &OS, bool Is64Bit,
                                           bool IsLIttleEndian, uint8_t OSABI);
} // End llvm namespace

// Defines symbolic names for Sparc registers.  This defines a mapping from
// register name to register number.
//
#define GET_REGINFO_ENUM
#include "SparcGenRegisterInfo.inc"

// Defines symbolic names for the Sparc instructions.
//
#define GET_INSTRINFO_ENUM
#include "SparcGenInstrInfo.inc"

#define GET_SUBTARGETINFO_ENUM
#include "SparcGenSubtargetInfo.inc"

#endif
