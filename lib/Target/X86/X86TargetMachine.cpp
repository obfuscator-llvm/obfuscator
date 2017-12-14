//===-- X86TargetMachine.cpp - Define TargetMachine for the X86 -----------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file defines the X86 specific subclass of TargetMachine.
//
//===----------------------------------------------------------------------===//

#include "MCTargetDesc/X86MCTargetDesc.h"
#include "X86.h"
#include "X86CallLowering.h"
#include "X86LegalizerInfo.h"
#include "X86MacroFusion.h"
#include "X86Subtarget.h"
#include "X86TargetMachine.h"
#include "X86TargetObjectFile.h"
#include "X86TargetTransformInfo.h"
#include "llvm/ADT/Optional.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/Triple.h"
#include "llvm/Analysis/TargetTransformInfo.h"
#include "llvm/CodeGen/ExecutionDepsFix.h"
#include "llvm/CodeGen/GlobalISel/CallLowering.h"
#include "llvm/CodeGen/GlobalISel/IRTranslator.h"
#include "llvm/CodeGen/GlobalISel/InstructionSelect.h"
#include "llvm/CodeGen/GlobalISel/Legalizer.h"
#include "llvm/CodeGen/GlobalISel/RegBankSelect.h"
#include "llvm/CodeGen/MachineScheduler.h"
#include "llvm/CodeGen/Passes.h"
#include "llvm/CodeGen/TargetPassConfig.h"
#include "llvm/IR/Attributes.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/Function.h"
#include "llvm/Pass.h"
#include "llvm/Support/CodeGen.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/TargetRegistry.h"
#include "llvm/Target/TargetLoweringObjectFile.h"
#include "llvm/Target/TargetOptions.h"
#include <memory>
#include <string>

using namespace llvm;

static cl::opt<bool> EnableMachineCombinerPass("x86-machine-combiner",
                               cl::desc("Enable the machine combiner pass"),
                               cl::init(true), cl::Hidden);

namespace llvm {

void initializeWinEHStatePassPass(PassRegistry &);
void initializeFixupLEAPassPass(PassRegistry &);
void initializeX86ExecutionDepsFixPass(PassRegistry &);

} // end namespace llvm

extern "C" void LLVMInitializeX86Target() {
  // Register the target.
  RegisterTargetMachine<X86TargetMachine> X(getTheX86_32Target());
  RegisterTargetMachine<X86TargetMachine> Y(getTheX86_64Target());

  PassRegistry &PR = *PassRegistry::getPassRegistry();
  initializeGlobalISel(PR);
  initializeWinEHStatePassPass(PR);
  initializeFixupBWInstPassPass(PR);
  initializeEvexToVexInstPassPass(PR);
  initializeFixupLEAPassPass(PR);
  initializeX86ExecutionDepsFixPass(PR);
}

static std::unique_ptr<TargetLoweringObjectFile> createTLOF(const Triple &TT) {
  if (TT.isOSBinFormatMachO()) {
    if (TT.getArch() == Triple::x86_64)
      return llvm::make_unique<X86_64MachoTargetObjectFile>();
    return llvm::make_unique<TargetLoweringObjectFileMachO>();
  }

  if (TT.isOSFreeBSD())
    return llvm::make_unique<X86FreeBSDTargetObjectFile>();
  if (TT.isOSLinux() || TT.isOSNaCl() || TT.isOSIAMCU())
    return llvm::make_unique<X86LinuxNaClTargetObjectFile>();
  if (TT.isOSSolaris())
    return llvm::make_unique<X86SolarisTargetObjectFile>();
  if (TT.isOSFuchsia())
    return llvm::make_unique<X86FuchsiaTargetObjectFile>();
  if (TT.isOSBinFormatELF())
    return llvm::make_unique<X86ELFTargetObjectFile>();
  if (TT.isKnownWindowsMSVCEnvironment() || TT.isWindowsCoreCLREnvironment())
    return llvm::make_unique<X86WindowsTargetObjectFile>();
  if (TT.isOSBinFormatCOFF())
    return llvm::make_unique<TargetLoweringObjectFileCOFF>();
  llvm_unreachable("unknown subtarget type");
}

static std::string computeDataLayout(const Triple &TT) {
  // X86 is little endian
  std::string Ret = "e";

  Ret += DataLayout::getManglingComponent(TT);
  // X86 and x32 have 32 bit pointers.
  if ((TT.isArch64Bit() &&
       (TT.getEnvironment() == Triple::GNUX32 || TT.isOSNaCl())) ||
      !TT.isArch64Bit())
    Ret += "-p:32:32";

  // Some ABIs align 64 bit integers and doubles to 64 bits, others to 32.
  if (TT.isArch64Bit() || TT.isOSWindows() || TT.isOSNaCl())
    Ret += "-i64:64";
  else if (TT.isOSIAMCU())
    Ret += "-i64:32-f64:32";
  else
    Ret += "-f64:32:64";

  // Some ABIs align long double to 128 bits, others to 32.
  if (TT.isOSNaCl() || TT.isOSIAMCU())
    ; // No f80
  else if (TT.isArch64Bit() || TT.isOSDarwin())
    Ret += "-f80:128";
  else
    Ret += "-f80:32";

  if (TT.isOSIAMCU())
    Ret += "-f128:32";

  // The registers can hold 8, 16, 32 or, in x86-64, 64 bits.
  if (TT.isArch64Bit())
    Ret += "-n8:16:32:64";
  else
    Ret += "-n8:16:32";

  // The stack is aligned to 32 bits on some ABIs and 128 bits on others.
  if ((!TT.isArch64Bit() && TT.isOSWindows()) || TT.isOSIAMCU())
    Ret += "-a:0:32-S32";
  else
    Ret += "-S128";

  return Ret;
}

static Reloc::Model getEffectiveRelocModel(const Triple &TT,
                                           Optional<Reloc::Model> RM) {
  bool is64Bit = TT.getArch() == Triple::x86_64;
  if (!RM.hasValue()) {
    // Darwin defaults to PIC in 64 bit mode and dynamic-no-pic in 32 bit mode.
    // Win64 requires rip-rel addressing, thus we force it to PIC. Otherwise we
    // use static relocation model by default.
    if (TT.isOSDarwin()) {
      if (is64Bit)
        return Reloc::PIC_;
      return Reloc::DynamicNoPIC;
    }
    if (TT.isOSWindows() && is64Bit)
      return Reloc::PIC_;
    return Reloc::Static;
  }

  // ELF and X86-64 don't have a distinct DynamicNoPIC model.  DynamicNoPIC
  // is defined as a model for code which may be used in static or dynamic
  // executables but not necessarily a shared library. On X86-32 we just
  // compile in -static mode, in x86-64 we use PIC.
  if (*RM == Reloc::DynamicNoPIC) {
    if (is64Bit)
      return Reloc::PIC_;
    if (!TT.isOSDarwin())
      return Reloc::Static;
  }

  // If we are on Darwin, disallow static relocation model in X86-64 mode, since
  // the Mach-O file format doesn't support it.
  if (*RM == Reloc::Static && TT.isOSDarwin() && is64Bit)
    return Reloc::PIC_;

  return *RM;
}

/// Create an X86 target.
///
X86TargetMachine::X86TargetMachine(const Target &T, const Triple &TT,
                                   StringRef CPU, StringRef FS,
                                   const TargetOptions &Options,
                                   Optional<Reloc::Model> RM,
                                   CodeModel::Model CM, CodeGenOpt::Level OL)
    : LLVMTargetMachine(T, computeDataLayout(TT), TT, CPU, FS, Options,
                        getEffectiveRelocModel(TT, RM), CM, OL),
      TLOF(createTLOF(getTargetTriple())) {
  // Windows stack unwinder gets confused when execution flow "falls through"
  // after a call to 'noreturn' function.
  // To prevent that, we emit a trap for 'unreachable' IR instructions.
  // (which on X86, happens to be the 'ud2' instruction)
  // On PS4, the "return address" of a 'noreturn' call must still be within
  // the calling function, and TrapUnreachable is an easy way to get that.
  // The check here for 64-bit windows is a bit icky, but as we're unlikely
  // to ever want to mix 32 and 64-bit windows code in a single module
  // this should be fine.
  if ((TT.isOSWindows() && TT.getArch() == Triple::x86_64) || TT.isPS4())
    this->Options.TrapUnreachable = true;

  initAsmInfo();
}

X86TargetMachine::~X86TargetMachine() = default;

const X86Subtarget *
X86TargetMachine::getSubtargetImpl(const Function &F) const {
  Attribute CPUAttr = F.getFnAttribute("target-cpu");
  Attribute FSAttr = F.getFnAttribute("target-features");

  StringRef CPU = !CPUAttr.hasAttribute(Attribute::None)
                      ? CPUAttr.getValueAsString()
                      : (StringRef)TargetCPU;
  StringRef FS = !FSAttr.hasAttribute(Attribute::None)
                     ? FSAttr.getValueAsString()
                     : (StringRef)TargetFS;

  SmallString<512> Key;
  Key.reserve(CPU.size() + FS.size());
  Key += CPU;
  Key += FS;

  // FIXME: This is related to the code below to reset the target options,
  // we need to know whether or not the soft float flag is set on the
  // function before we can generate a subtarget. We also need to use
  // it as a key for the subtarget since that can be the only difference
  // between two functions.
  bool SoftFloat =
      F.getFnAttribute("use-soft-float").getValueAsString() == "true";
  // If the soft float attribute is set on the function turn on the soft float
  // subtarget feature.
  if (SoftFloat)
    Key += FS.empty() ? "+soft-float" : ",+soft-float";

  FS = Key.substr(CPU.size());

  auto &I = SubtargetMap[Key];
  if (!I) {
    // This needs to be done before we create a new subtarget since any
    // creation will depend on the TM and the code generation flags on the
    // function that reside in TargetOptions.
    resetTargetOptions(F);
    I = llvm::make_unique<X86Subtarget>(TargetTriple, CPU, FS, *this,
                                        Options.StackAlignmentOverride);
  }
  return I.get();
}

//===----------------------------------------------------------------------===//
// Command line options for x86
//===----------------------------------------------------------------------===//
static cl::opt<bool>
UseVZeroUpper("x86-use-vzeroupper", cl::Hidden,
  cl::desc("Minimize AVX to SSE transition penalty"),
  cl::init(true));

//===----------------------------------------------------------------------===//
// X86 TTI query.
//===----------------------------------------------------------------------===//

TargetIRAnalysis X86TargetMachine::getTargetIRAnalysis() {
  return TargetIRAnalysis([this](const Function &F) {
    return TargetTransformInfo(X86TTIImpl(this, F));
  });
}

//===----------------------------------------------------------------------===//
// Pass Pipeline Configuration
//===----------------------------------------------------------------------===//

namespace {

/// X86 Code Generator Pass Configuration Options.
class X86PassConfig : public TargetPassConfig {
public:
  X86PassConfig(X86TargetMachine &TM, PassManagerBase &PM)
    : TargetPassConfig(TM, PM) {}

  X86TargetMachine &getX86TargetMachine() const {
    return getTM<X86TargetMachine>();
  }

  ScheduleDAGInstrs *
  createMachineScheduler(MachineSchedContext *C) const override {
    ScheduleDAGMILive *DAG = createGenericSchedLive(C);
    DAG->addMutation(createX86MacroFusionDAGMutation());
    return DAG;
  }

  void addIRPasses() override;
  bool addInstSelector() override;
#ifdef LLVM_BUILD_GLOBAL_ISEL
  bool addIRTranslator() override;
  bool addLegalizeMachineIR() override;
  bool addRegBankSelect() override;
  bool addGlobalInstructionSelect() override;
#endif
  bool addILPOpts() override;
  bool addPreISel() override;
  void addPreRegAlloc() override;
  void addPostRegAlloc() override;
  void addPreEmitPass() override;
  void addPreSched2() override;
};

class X86ExecutionDepsFix : public ExecutionDepsFix {
public:
  static char ID;
  X86ExecutionDepsFix() : ExecutionDepsFix(ID, X86::VR128XRegClass) {}
  StringRef getPassName() const override {
    return "X86 Execution Dependency Fix";
  }
};
char X86ExecutionDepsFix::ID;

} // end anonymous namespace

INITIALIZE_PASS(X86ExecutionDepsFix, "x86-execution-deps-fix",
                "X86 Execution Dependency Fix", false, false)

TargetPassConfig *X86TargetMachine::createPassConfig(PassManagerBase &PM) {
  return new X86PassConfig(*this, PM);
}

void X86PassConfig::addIRPasses() {
  addPass(createAtomicExpandPass());

  TargetPassConfig::addIRPasses();

  if (TM->getOptLevel() != CodeGenOpt::None)
    addPass(createInterleavedAccessPass());
}

bool X86PassConfig::addInstSelector() {
  // Install an instruction selector.
  addPass(createX86ISelDag(getX86TargetMachine(), getOptLevel()));

  // For ELF, cleanup any local-dynamic TLS accesses.
  if (TM->getTargetTriple().isOSBinFormatELF() &&
      getOptLevel() != CodeGenOpt::None)
    addPass(createCleanupLocalDynamicTLSPass());

  addPass(createX86GlobalBaseRegPass());
  return false;
}

#ifdef LLVM_BUILD_GLOBAL_ISEL
bool X86PassConfig::addIRTranslator() {
  addPass(new IRTranslator());
  return false;
}

bool X86PassConfig::addLegalizeMachineIR() {
  addPass(new Legalizer());
  return false;
}

bool X86PassConfig::addRegBankSelect() {
  addPass(new RegBankSelect());
  return false;
}

bool X86PassConfig::addGlobalInstructionSelect() {
  addPass(new InstructionSelect());
  return false;
}
#endif

bool X86PassConfig::addILPOpts() {
  addPass(&EarlyIfConverterID);
  if (EnableMachineCombinerPass)
    addPass(&MachineCombinerID);
  addPass(createX86CmovConverterPass());
  return true;
}

bool X86PassConfig::addPreISel() {
  // Only add this pass for 32-bit x86 Windows.
  const Triple &TT = TM->getTargetTriple();
  if (TT.isOSWindows() && TT.getArch() == Triple::x86)
    addPass(createX86WinEHStatePass());
  return true;
}

void X86PassConfig::addPreRegAlloc() {
  if (getOptLevel() != CodeGenOpt::None) {
    addPass(&LiveRangeShrinkID);
    addPass(createX86FixupSetCC());
    addPass(createX86OptimizeLEAs());
    addPass(createX86CallFrameOptimization());
  }

  addPass(createX86WinAllocaExpander());
}

void X86PassConfig::addPostRegAlloc() {
  addPass(createX86FloatingPointStackifierPass());
}

void X86PassConfig::addPreSched2() { addPass(createX86ExpandPseudoPass()); }

void X86PassConfig::addPreEmitPass() {
  if (getOptLevel() != CodeGenOpt::None)
    addPass(new X86ExecutionDepsFix());

  if (UseVZeroUpper)
    addPass(createX86IssueVZeroUpperPass());

  if (getOptLevel() != CodeGenOpt::None) {
    addPass(createX86FixupBWInsts());
    addPass(createX86PadShortFunctions());
    addPass(createX86FixupLEAs());
    addPass(createX86EvexToVexInsts());
  }
}
