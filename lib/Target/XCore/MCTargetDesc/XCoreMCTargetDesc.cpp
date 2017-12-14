//===-- XCoreMCTargetDesc.cpp - XCore Target Descriptions -----------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file provides XCore specific target descriptions.
//
//===----------------------------------------------------------------------===//

#include "MCTargetDesc/XCoreMCTargetDesc.h"
#include "InstPrinter/XCoreInstPrinter.h"
#include "MCTargetDesc/XCoreMCAsmInfo.h"
#include "XCoreTargetStreamer.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/MC/MCDwarf.h"
#include "llvm/MC/MCInstrInfo.h"
#include "llvm/MC/MCRegisterInfo.h"
#include "llvm/MC/MCSubtargetInfo.h"
#include "llvm/Support/CodeGen.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/FormattedStream.h"
#include "llvm/Support/TargetRegistry.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;

#define GET_INSTRINFO_MC_DESC
#include "XCoreGenInstrInfo.inc"

#define GET_SUBTARGETINFO_MC_DESC
#include "XCoreGenSubtargetInfo.inc"

#define GET_REGINFO_MC_DESC
#include "XCoreGenRegisterInfo.inc"

static MCInstrInfo *createXCoreMCInstrInfo() {
  MCInstrInfo *X = new MCInstrInfo();
  InitXCoreMCInstrInfo(X);
  return X;
}

static MCRegisterInfo *createXCoreMCRegisterInfo(const Triple &TT) {
  MCRegisterInfo *X = new MCRegisterInfo();
  InitXCoreMCRegisterInfo(X, XCore::LR);
  return X;
}

static MCSubtargetInfo *
createXCoreMCSubtargetInfo(const Triple &TT, StringRef CPU, StringRef FS) {
  return createXCoreMCSubtargetInfoImpl(TT, CPU, FS);
}

static MCAsmInfo *createXCoreMCAsmInfo(const MCRegisterInfo &MRI,
                                       const Triple &TT) {
  MCAsmInfo *MAI = new XCoreMCAsmInfo(TT);

  // Initial state of the frame pointer is SP.
  MCCFIInstruction Inst = MCCFIInstruction::createDefCfa(nullptr, XCore::SP, 0);
  MAI->addInitialFrameState(Inst);

  return MAI;
}

static void adjustCodeGenOpts(const Triple &TT, Reloc::Model RM,
                              CodeModel::Model &CM) {
  if (CM == CodeModel::Default) {
    CM = CodeModel::Small;
  }
  if (CM != CodeModel::Small && CM != CodeModel::Large)
    report_fatal_error("Target only supports CodeModel Small or Large");
}

static MCInstPrinter *createXCoreMCInstPrinter(const Triple &T,
                                               unsigned SyntaxVariant,
                                               const MCAsmInfo &MAI,
                                               const MCInstrInfo &MII,
                                               const MCRegisterInfo &MRI) {
  return new XCoreInstPrinter(MAI, MII, MRI);
}

XCoreTargetStreamer::XCoreTargetStreamer(MCStreamer &S) : MCTargetStreamer(S) {}

XCoreTargetStreamer::~XCoreTargetStreamer() = default;

namespace {

class XCoreTargetAsmStreamer : public XCoreTargetStreamer {
  formatted_raw_ostream &OS;

public:
  XCoreTargetAsmStreamer(MCStreamer &S, formatted_raw_ostream &OS);

  void emitCCTopData(StringRef Name) override;
  void emitCCTopFunction(StringRef Name) override;
  void emitCCBottomData(StringRef Name) override;
  void emitCCBottomFunction(StringRef Name) override;
};

} // end anonymous namespace

XCoreTargetAsmStreamer::XCoreTargetAsmStreamer(MCStreamer &S,
                                               formatted_raw_ostream &OS)
    : XCoreTargetStreamer(S), OS(OS) {}

void XCoreTargetAsmStreamer::emitCCTopData(StringRef Name) {
  OS << "\t.cc_top " << Name << ".data," << Name << '\n';
}

void XCoreTargetAsmStreamer::emitCCTopFunction(StringRef Name) {
  OS << "\t.cc_top " << Name << ".function," << Name << '\n';
}

void XCoreTargetAsmStreamer::emitCCBottomData(StringRef Name) {
  OS << "\t.cc_bottom " << Name << ".data\n";
}

void XCoreTargetAsmStreamer::emitCCBottomFunction(StringRef Name) {
  OS << "\t.cc_bottom " << Name << ".function\n";
}

static MCTargetStreamer *createTargetAsmStreamer(MCStreamer &S,
                                                 formatted_raw_ostream &OS,
                                                 MCInstPrinter *InstPrint,
                                                 bool isVerboseAsm) {
  return new XCoreTargetAsmStreamer(S, OS);
}

// Force static initialization.
extern "C" void LLVMInitializeXCoreTargetMC() {
  // Register the MC asm info.
  RegisterMCAsmInfoFn X(getTheXCoreTarget(), createXCoreMCAsmInfo);

  // Register the MC codegen info.
  TargetRegistry::registerMCAdjustCodeGenOpts(getTheXCoreTarget(),
                                              adjustCodeGenOpts);

  // Register the MC instruction info.
  TargetRegistry::RegisterMCInstrInfo(getTheXCoreTarget(),
                                      createXCoreMCInstrInfo);

  // Register the MC register info.
  TargetRegistry::RegisterMCRegInfo(getTheXCoreTarget(),
                                    createXCoreMCRegisterInfo);

  // Register the MC subtarget info.
  TargetRegistry::RegisterMCSubtargetInfo(getTheXCoreTarget(),
                                          createXCoreMCSubtargetInfo);

  // Register the MCInstPrinter
  TargetRegistry::RegisterMCInstPrinter(getTheXCoreTarget(),
                                        createXCoreMCInstPrinter);

  TargetRegistry::RegisterAsmTargetStreamer(getTheXCoreTarget(),
                                            createTargetAsmStreamer);
}
