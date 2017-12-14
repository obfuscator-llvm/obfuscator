//===-- MipsTargetMachine.cpp - Define TargetMachine for Mips -------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// Implements the info about Mips target spec.
//
//===----------------------------------------------------------------------===//

#include "MipsTargetMachine.h"
#include "MCTargetDesc/MipsABIInfo.h"
#include "MCTargetDesc/MipsMCTargetDesc.h"
#include "Mips.h"
#include "Mips16ISelDAGToDAG.h"
#include "MipsSEISelDAGToDAG.h"
#include "MipsSubtarget.h"
#include "MipsTargetObjectFile.h"
#include "llvm/ADT/Optional.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Analysis/TargetTransformInfo.h"
#include "llvm/CodeGen/BasicTTIImpl.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/Passes.h"
#include "llvm/CodeGen/TargetPassConfig.h"
#include "llvm/IR/Attributes.h"
#include "llvm/IR/Function.h"
#include "llvm/Support/CodeGen.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/TargetRegistry.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Target/TargetOptions.h"
#include <string>

using namespace llvm;

#define DEBUG_TYPE "mips"

extern "C" void LLVMInitializeMipsTarget() {
  // Register the target.
  RegisterTargetMachine<MipsebTargetMachine> X(getTheMipsTarget());
  RegisterTargetMachine<MipselTargetMachine> Y(getTheMipselTarget());
  RegisterTargetMachine<MipsebTargetMachine> A(getTheMips64Target());
  RegisterTargetMachine<MipselTargetMachine> B(getTheMips64elTarget());
}

static std::string computeDataLayout(const Triple &TT, StringRef CPU,
                                     const TargetOptions &Options,
                                     bool isLittle) {
  std::string Ret;
  MipsABIInfo ABI = MipsABIInfo::computeTargetABI(TT, CPU, Options.MCOptions);

  // There are both little and big endian mips.
  if (isLittle)
    Ret += "e";
  else
    Ret += "E";

  if (ABI.IsO32())
    Ret += "-m:m";
  else
    Ret += "-m:e";

  // Pointers are 32 bit on some ABIs.
  if (!ABI.IsN64())
    Ret += "-p:32:32";

  // 8 and 16 bit integers only need to have natural alignment, but try to
  // align them to 32 bits. 64 bit integers have natural alignment.
  Ret += "-i8:8:32-i16:16:32-i64:64";

  // 32 bit registers are always available and the stack is at least 64 bit
  // aligned. On N64 64 bit registers are also available and the stack is
  // 128 bit aligned.
  if (ABI.IsN64() || ABI.IsN32())
    Ret += "-n32:64-S128";
  else
    Ret += "-n32-S64";

  return Ret;
}

static Reloc::Model getEffectiveRelocModel(CodeModel::Model CM,
                                           Optional<Reloc::Model> RM) {
  if (!RM.hasValue() || CM == CodeModel::JITDefault)
    return Reloc::Static;
  return *RM;
}

// On function prologue, the stack is created by decrementing
// its pointer. Once decremented, all references are done with positive
// offset from the stack/frame pointer, using StackGrowsUp enables
// an easier handling.
// Using CodeModel::Large enables different CALL behavior.
MipsTargetMachine::MipsTargetMachine(const Target &T, const Triple &TT,
                                     StringRef CPU, StringRef FS,
                                     const TargetOptions &Options,
                                     Optional<Reloc::Model> RM,
                                     CodeModel::Model CM, CodeGenOpt::Level OL,
                                     bool isLittle)
    : LLVMTargetMachine(T, computeDataLayout(TT, CPU, Options, isLittle), TT,
                        CPU, FS, Options, getEffectiveRelocModel(CM, RM), CM,
                        OL),
      isLittle(isLittle), TLOF(llvm::make_unique<MipsTargetObjectFile>()),
      ABI(MipsABIInfo::computeTargetABI(TT, CPU, Options.MCOptions)),
      Subtarget(nullptr), DefaultSubtarget(TT, CPU, FS, isLittle, *this),
      NoMips16Subtarget(TT, CPU, FS.empty() ? "-mips16" : FS.str() + ",-mips16",
                        isLittle, *this),
      Mips16Subtarget(TT, CPU, FS.empty() ? "+mips16" : FS.str() + ",+mips16",
                      isLittle, *this) {
  Subtarget = &DefaultSubtarget;
  initAsmInfo();
}

MipsTargetMachine::~MipsTargetMachine() = default;

void MipsebTargetMachine::anchor() {}

MipsebTargetMachine::MipsebTargetMachine(const Target &T, const Triple &TT,
                                         StringRef CPU, StringRef FS,
                                         const TargetOptions &Options,
                                         Optional<Reloc::Model> RM,
                                         CodeModel::Model CM,
                                         CodeGenOpt::Level OL)
    : MipsTargetMachine(T, TT, CPU, FS, Options, RM, CM, OL, false) {}

void MipselTargetMachine::anchor() {}

MipselTargetMachine::MipselTargetMachine(const Target &T, const Triple &TT,
                                         StringRef CPU, StringRef FS,
                                         const TargetOptions &Options,
                                         Optional<Reloc::Model> RM,
                                         CodeModel::Model CM,
                                         CodeGenOpt::Level OL)
    : MipsTargetMachine(T, TT, CPU, FS, Options, RM, CM, OL, true) {}

const MipsSubtarget *
MipsTargetMachine::getSubtargetImpl(const Function &F) const {
  Attribute CPUAttr = F.getFnAttribute("target-cpu");
  Attribute FSAttr = F.getFnAttribute("target-features");

  std::string CPU = !CPUAttr.hasAttribute(Attribute::None)
                        ? CPUAttr.getValueAsString().str()
                        : TargetCPU;
  std::string FS = !FSAttr.hasAttribute(Attribute::None)
                       ? FSAttr.getValueAsString().str()
                       : TargetFS;
  bool hasMips16Attr =
      !F.getFnAttribute("mips16").hasAttribute(Attribute::None);
  bool hasNoMips16Attr =
      !F.getFnAttribute("nomips16").hasAttribute(Attribute::None);

  bool HasMicroMipsAttr =
      !F.getFnAttribute("micromips").hasAttribute(Attribute::None);
  bool HasNoMicroMipsAttr =
      !F.getFnAttribute("nomicromips").hasAttribute(Attribute::None);

  // FIXME: This is related to the code below to reset the target options,
  // we need to know whether or not the soft float flag is set on the
  // function, so we can enable it as a subtarget feature.
  bool softFloat =
      F.hasFnAttribute("use-soft-float") &&
      F.getFnAttribute("use-soft-float").getValueAsString() == "true";

  if (hasMips16Attr)
    FS += FS.empty() ? "+mips16" : ",+mips16";
  else if (hasNoMips16Attr)
    FS += FS.empty() ? "-mips16" : ",-mips16";
  if (HasMicroMipsAttr)
    FS += FS.empty() ? "+micromips" : ",+micromips";
  else if (HasNoMicroMipsAttr)
    FS += FS.empty() ? "-micromips" : ",-micromips";
  if (softFloat)
    FS += FS.empty() ? "+soft-float" : ",+soft-float";

  auto &I = SubtargetMap[CPU + FS];
  if (!I) {
    // This needs to be done before we create a new subtarget since any
    // creation will depend on the TM and the code generation flags on the
    // function that reside in TargetOptions.
    resetTargetOptions(F);
    I = llvm::make_unique<MipsSubtarget>(TargetTriple, CPU, FS, isLittle,
                                         *this);
  }
  return I.get();
}

void MipsTargetMachine::resetSubtarget(MachineFunction *MF) {
  DEBUG(dbgs() << "resetSubtarget\n");

  Subtarget = const_cast<MipsSubtarget *>(getSubtargetImpl(*MF->getFunction()));
  MF->setSubtarget(Subtarget);
}

namespace {

/// Mips Code Generator Pass Configuration Options.
class MipsPassConfig : public TargetPassConfig {
public:
  MipsPassConfig(MipsTargetMachine &TM, PassManagerBase &PM)
    : TargetPassConfig(TM, PM) {
    // The current implementation of long branch pass requires a scratch
    // register ($at) to be available before branch instructions. Tail merging
    // can break this requirement, so disable it when long branch pass is
    // enabled.
    EnableTailMerge = !getMipsSubtarget().enableLongBranchPass();
  }

  MipsTargetMachine &getMipsTargetMachine() const {
    return getTM<MipsTargetMachine>();
  }

  const MipsSubtarget &getMipsSubtarget() const {
    return *getMipsTargetMachine().getSubtargetImpl();
  }

  void addIRPasses() override;
  bool addInstSelector() override;
  void addPreEmitPass() override;
  void addPreRegAlloc() override;
};

} // end anonymous namespace

TargetPassConfig *MipsTargetMachine::createPassConfig(PassManagerBase &PM) {
  return new MipsPassConfig(*this, PM);
}

void MipsPassConfig::addIRPasses() {
  TargetPassConfig::addIRPasses();
  addPass(createAtomicExpandPass());
  if (getMipsSubtarget().os16())
    addPass(createMipsOs16Pass());
  if (getMipsSubtarget().inMips16HardFloat())
    addPass(createMips16HardFloatPass());
}
// Install an instruction selector pass using
// the ISelDag to gen Mips code.
bool MipsPassConfig::addInstSelector() {
  addPass(createMipsModuleISelDagPass());
  addPass(createMips16ISelDag(getMipsTargetMachine(), getOptLevel()));
  addPass(createMipsSEISelDag(getMipsTargetMachine(), getOptLevel()));
  return false;
}

void MipsPassConfig::addPreRegAlloc() {
  addPass(createMipsOptimizePICCallPass());
}

TargetIRAnalysis MipsTargetMachine::getTargetIRAnalysis() {
  return TargetIRAnalysis([this](const Function &F) {
    if (Subtarget->allowMixed16_32()) {
      DEBUG(errs() << "No Target Transform Info Pass Added\n");
      // FIXME: This is no longer necessary as the TTI returned is per-function.
      return TargetTransformInfo(F.getParent()->getDataLayout());
    }

    DEBUG(errs() << "Target Transform Info Pass Added\n");
    return TargetTransformInfo(BasicTTIImpl(this, F));
  });
}

// Implemented by targets that want to run passes immediately before
// machine code is emitted. return true if -print-machineinstrs should
// print out the code after the passes.
void MipsPassConfig::addPreEmitPass() {
  addPass(createMicroMipsSizeReductionPass());

  // The delay slot filler pass can potientially create forbidden slot (FS)
  // hazards for MIPSR6 which the hazard schedule pass (HSP) will fix. Any
  // (new) pass that creates compact branches after the HSP must handle FS
  // hazards itself or be pipelined before the HSP.
  addPass(createMipsDelaySlotFillerPass());
  addPass(createMipsHazardSchedule());
  addPass(createMipsLongBranchPass());
  addPass(createMipsConstantIslandPass());
}
