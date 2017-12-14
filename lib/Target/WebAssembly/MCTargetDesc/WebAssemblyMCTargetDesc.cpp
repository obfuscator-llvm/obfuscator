//===-- WebAssemblyMCTargetDesc.cpp - WebAssembly Target Descriptions -----===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
///
/// \file
/// \brief This file provides WebAssembly-specific target descriptions.
///
//===----------------------------------------------------------------------===//

#include "WebAssemblyMCTargetDesc.h"
#include "InstPrinter/WebAssemblyInstPrinter.h"
#include "WebAssemblyMCAsmInfo.h"
#include "WebAssemblyTargetStreamer.h"
#include "llvm/MC/MCInstrInfo.h"
#include "llvm/MC/MCRegisterInfo.h"
#include "llvm/MC/MCSubtargetInfo.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/TargetRegistry.h"
using namespace llvm;

#define DEBUG_TYPE "wasm-mc-target-desc"

#define GET_INSTRINFO_MC_DESC
#include "WebAssemblyGenInstrInfo.inc"

#define GET_SUBTARGETINFO_MC_DESC
#include "WebAssemblyGenSubtargetInfo.inc"

#define GET_REGINFO_MC_DESC
#include "WebAssemblyGenRegisterInfo.inc"

static MCAsmInfo *createMCAsmInfo(const MCRegisterInfo & /*MRI*/,
                                  const Triple &TT) {
  if (TT.isOSBinFormatELF())
    return new WebAssemblyMCAsmInfoELF(TT);
  return new WebAssemblyMCAsmInfo(TT);
}

static void adjustCodeGenOpts(const Triple & /*TT*/, Reloc::Model /*RM*/,
                              CodeModel::Model &CM) {
  CodeModel::Model M = (CM == CodeModel::Default || CM == CodeModel::JITDefault)
                           ? CodeModel::Large
                           : CM;
  if (M != CodeModel::Large)
    report_fatal_error("Non-large code models are not supported yet");
}

static MCInstrInfo *createMCInstrInfo() {
  MCInstrInfo *X = new MCInstrInfo();
  InitWebAssemblyMCInstrInfo(X);
  return X;
}

static MCRegisterInfo *createMCRegisterInfo(const Triple & /*T*/) {
  MCRegisterInfo *X = new MCRegisterInfo();
  InitWebAssemblyMCRegisterInfo(X, 0);
  return X;
}

static MCInstPrinter *createMCInstPrinter(const Triple & /*T*/,
                                          unsigned SyntaxVariant,
                                          const MCAsmInfo &MAI,
                                          const MCInstrInfo &MII,
                                          const MCRegisterInfo &MRI) {
  assert(SyntaxVariant == 0 && "WebAssembly only has one syntax variant");
  return new WebAssemblyInstPrinter(MAI, MII, MRI);
}

static MCCodeEmitter *createCodeEmitter(const MCInstrInfo &MCII,
                                        const MCRegisterInfo & /*MRI*/,
                                        MCContext &Ctx) {
  return createWebAssemblyMCCodeEmitter(MCII);
}

static MCAsmBackend *createAsmBackend(const Target & /*T*/,
                                      const MCRegisterInfo & /*MRI*/,
                                      const Triple &TT, StringRef /*CPU*/,
                                      const MCTargetOptions & /*Options*/) {
  return createWebAssemblyAsmBackend(TT);
}

static MCSubtargetInfo *createMCSubtargetInfo(const Triple &TT, StringRef CPU,
                                              StringRef FS) {
  return createWebAssemblyMCSubtargetInfoImpl(TT, CPU, FS);
}

static MCTargetStreamer *
createObjectTargetStreamer(MCStreamer &S, const MCSubtargetInfo &STI) {
  const Triple &TT = STI.getTargetTriple();
  if (TT.isOSBinFormatELF())
    return new WebAssemblyTargetELFStreamer(S);

  return new WebAssemblyTargetWasmStreamer(S);
}

static MCTargetStreamer *createAsmTargetStreamer(MCStreamer &S,
                                                 formatted_raw_ostream &OS,
                                                 MCInstPrinter * /*InstPrint*/,
                                                 bool /*isVerboseAsm*/) {
  return new WebAssemblyTargetAsmStreamer(S, OS);
}

// Force static initialization.
extern "C" void LLVMInitializeWebAssemblyTargetMC() {
  for (Target *T :
       {&getTheWebAssemblyTarget32(), &getTheWebAssemblyTarget64()}) {
    // Register the MC asm info.
    RegisterMCAsmInfoFn X(*T, createMCAsmInfo);

    // Register the MC instruction info.
    TargetRegistry::RegisterMCInstrInfo(*T, createMCInstrInfo);

    // Register the MC codegen info.
    TargetRegistry::registerMCAdjustCodeGenOpts(*T, adjustCodeGenOpts);

    // Register the MC register info.
    TargetRegistry::RegisterMCRegInfo(*T, createMCRegisterInfo);

    // Register the MCInstPrinter.
    TargetRegistry::RegisterMCInstPrinter(*T, createMCInstPrinter);

    // Register the MC code emitter.
    TargetRegistry::RegisterMCCodeEmitter(*T, createCodeEmitter);

    // Register the ASM Backend.
    TargetRegistry::RegisterMCAsmBackend(*T, createAsmBackend);

    // Register the MC subtarget info.
    TargetRegistry::RegisterMCSubtargetInfo(*T, createMCSubtargetInfo);

    // Register the object target streamer.
    TargetRegistry::RegisterObjectTargetStreamer(*T,
                                                 createObjectTargetStreamer);
    // Register the asm target streamer.
    TargetRegistry::RegisterAsmTargetStreamer(*T, createAsmTargetStreamer);
  }
}

wasm::ValType WebAssembly::toValType(const MVT &Ty) {
  switch (Ty.SimpleTy) {
  case MVT::i32: return wasm::ValType::I32;
  case MVT::i64: return wasm::ValType::I64;
  case MVT::f32: return wasm::ValType::F32;
  case MVT::f64: return wasm::ValType::F64;
  default: llvm_unreachable("unexpected type");
  }
}
