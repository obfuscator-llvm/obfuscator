//===--- BackendUtil.cpp - LLVM Backend Utilities -------------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "clang/CodeGen/BackendUtil.h"
#include "clang/Basic/Diagnostic.h"
#include "clang/Basic/LangOptions.h"
#include "clang/Basic/TargetOptions.h"
#include "clang/Frontend/CodeGenOptions.h"
#include "clang/Frontend/FrontendDiagnostic.h"
#include "clang/Frontend/Utils.h"
#include "clang/Lex/HeaderSearchOptions.h"
#include "llvm/ADT/SmallSet.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringSwitch.h"
#include "llvm/ADT/Triple.h"
#include "llvm/Analysis/TargetLibraryInfo.h"
#include "llvm/Analysis/TargetTransformInfo.h"
#include "llvm/Bitcode/BitcodeReader.h"
#include "llvm/Bitcode/BitcodeWriter.h"
#include "llvm/Bitcode/BitcodeWriterPass.h"
#include "llvm/CodeGen/RegAllocRegistry.h"
#include "llvm/CodeGen/SchedulerRegistry.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/IRPrintingPasses.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/ModuleSummaryIndex.h"
#include "llvm/IR/Verifier.h"
#include "llvm/LTO/LTOBackend.h"
#include "llvm/MC/MCAsmInfo.h"
#include "llvm/MC/SubtargetFeature.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/PrettyStackTrace.h"
#include "llvm/Support/TargetRegistry.h"
#include "llvm/Support/Timer.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Target/TargetOptions.h"
#include "llvm/Target/TargetSubtargetInfo.h"
#include "llvm/Transforms/Coroutines.h"
#include "llvm/Transforms/IPO.h"
#include "llvm/Transforms/IPO/AlwaysInliner.h"
#include "llvm/Transforms/IPO/PassManagerBuilder.h"
#include "llvm/Transforms/IPO/ThinLTOBitcodeWriter.h"
#include "llvm/Transforms/Instrumentation.h"
#include "llvm/Transforms/ObjCARC.h"
#include "llvm/Transforms/Scalar.h"
#include "llvm/Transforms/Scalar/GVN.h"
#include "llvm/Transforms/Utils/NameAnonGlobals.h"
#include "llvm/Transforms/Utils/SymbolRewriter.h"
#include <memory>
using namespace clang;
using namespace llvm;

namespace {

// Default filename used for profile generation.
static constexpr StringLiteral DefaultProfileGenName = "default_%m.profraw";

class EmitAssemblyHelper {
  DiagnosticsEngine &Diags;
  const HeaderSearchOptions &HSOpts;
  const CodeGenOptions &CodeGenOpts;
  const clang::TargetOptions &TargetOpts;
  const LangOptions &LangOpts;
  Module *TheModule;

  Timer CodeGenerationTime;

  std::unique_ptr<raw_pwrite_stream> OS;

  TargetIRAnalysis getTargetIRAnalysis() const {
    if (TM)
      return TM->getTargetIRAnalysis();

    return TargetIRAnalysis();
  }

  void CreatePasses(legacy::PassManager &MPM, legacy::FunctionPassManager &FPM);

  /// Generates the TargetMachine.
  /// Leaves TM unchanged if it is unable to create the target machine.
  /// Some of our clang tests specify triples which are not built
  /// into clang. This is okay because these tests check the generated
  /// IR, and they require DataLayout which depends on the triple.
  /// In this case, we allow this method to fail and not report an error.
  /// When MustCreateTM is used, we print an error if we are unable to load
  /// the requested target.
  void CreateTargetMachine(bool MustCreateTM);

  /// Add passes necessary to emit assembly or LLVM IR.
  ///
  /// \return True on success.
  bool AddEmitPasses(legacy::PassManager &CodeGenPasses, BackendAction Action,
                     raw_pwrite_stream &OS);

public:
  EmitAssemblyHelper(DiagnosticsEngine &_Diags,
                     const HeaderSearchOptions &HeaderSearchOpts,
                     const CodeGenOptions &CGOpts,
                     const clang::TargetOptions &TOpts,
                     const LangOptions &LOpts, Module *M)
      : Diags(_Diags), HSOpts(HeaderSearchOpts), CodeGenOpts(CGOpts),
        TargetOpts(TOpts), LangOpts(LOpts), TheModule(M),
        CodeGenerationTime("codegen", "Code Generation Time") {}

  ~EmitAssemblyHelper() {
    if (CodeGenOpts.DisableFree)
      BuryPointer(std::move(TM));
  }

  std::unique_ptr<TargetMachine> TM;

  void EmitAssembly(BackendAction Action,
                    std::unique_ptr<raw_pwrite_stream> OS);

  void EmitAssemblyWithNewPassManager(BackendAction Action,
                                      std::unique_ptr<raw_pwrite_stream> OS);
};

// We need this wrapper to access LangOpts and CGOpts from extension functions
// that we add to the PassManagerBuilder.
class PassManagerBuilderWrapper : public PassManagerBuilder {
public:
  PassManagerBuilderWrapper(const Triple &TargetTriple,
                            const CodeGenOptions &CGOpts,
                            const LangOptions &LangOpts)
      : PassManagerBuilder(), TargetTriple(TargetTriple), CGOpts(CGOpts),
        LangOpts(LangOpts) {}
  const Triple &getTargetTriple() const { return TargetTriple; }
  const CodeGenOptions &getCGOpts() const { return CGOpts; }
  const LangOptions &getLangOpts() const { return LangOpts; }

private:
  const Triple &TargetTriple;
  const CodeGenOptions &CGOpts;
  const LangOptions &LangOpts;
};
}

static void addObjCARCAPElimPass(const PassManagerBuilder &Builder, PassManagerBase &PM) {
  if (Builder.OptLevel > 0)
    PM.add(createObjCARCAPElimPass());
}

static void addObjCARCExpandPass(const PassManagerBuilder &Builder, PassManagerBase &PM) {
  if (Builder.OptLevel > 0)
    PM.add(createObjCARCExpandPass());
}

static void addObjCARCOptPass(const PassManagerBuilder &Builder, PassManagerBase &PM) {
  if (Builder.OptLevel > 0)
    PM.add(createObjCARCOptPass());
}

static void addAddDiscriminatorsPass(const PassManagerBuilder &Builder,
                                     legacy::PassManagerBase &PM) {
  PM.add(createAddDiscriminatorsPass());
}

static void addBoundsCheckingPass(const PassManagerBuilder &Builder,
                                  legacy::PassManagerBase &PM) {
  PM.add(createBoundsCheckingPass());
}

static void addSanitizerCoveragePass(const PassManagerBuilder &Builder,
                                     legacy::PassManagerBase &PM) {
  const PassManagerBuilderWrapper &BuilderWrapper =
      static_cast<const PassManagerBuilderWrapper&>(Builder);
  const CodeGenOptions &CGOpts = BuilderWrapper.getCGOpts();
  SanitizerCoverageOptions Opts;
  Opts.CoverageType =
      static_cast<SanitizerCoverageOptions::Type>(CGOpts.SanitizeCoverageType);
  Opts.IndirectCalls = CGOpts.SanitizeCoverageIndirectCalls;
  Opts.TraceBB = CGOpts.SanitizeCoverageTraceBB;
  Opts.TraceCmp = CGOpts.SanitizeCoverageTraceCmp;
  Opts.TraceDiv = CGOpts.SanitizeCoverageTraceDiv;
  Opts.TraceGep = CGOpts.SanitizeCoverageTraceGep;
  Opts.Use8bitCounters = CGOpts.SanitizeCoverage8bitCounters;
  Opts.TracePC = CGOpts.SanitizeCoverageTracePC;
  Opts.TracePCGuard = CGOpts.SanitizeCoverageTracePCGuard;
  Opts.NoPrune = CGOpts.SanitizeCoverageNoPrune;
  Opts.Inline8bitCounters = CGOpts.SanitizeCoverageInline8bitCounters;
  PM.add(createSanitizerCoverageModulePass(Opts));
}

// Check if ASan should use GC-friendly instrumentation for globals.
// First of all, there is no point if -fdata-sections is off (expect for MachO,
// where this is not a factor). Also, on ELF this feature requires an assembler
// extension that only works with -integrated-as at the moment.
static bool asanUseGlobalsGC(const Triple &T, const CodeGenOptions &CGOpts) {
  if (!CGOpts.SanitizeAddressGlobalsDeadStripping)
    return false;
  switch (T.getObjectFormat()) {
  case Triple::MachO:
  case Triple::COFF:
    return true;
  case Triple::ELF:
    return CGOpts.DataSections && !CGOpts.DisableIntegratedAS;
  default:
    return false;
  }
}

static void addAddressSanitizerPasses(const PassManagerBuilder &Builder,
                                      legacy::PassManagerBase &PM) {
  const PassManagerBuilderWrapper &BuilderWrapper =
      static_cast<const PassManagerBuilderWrapper&>(Builder);
  const Triple &T = BuilderWrapper.getTargetTriple();
  const CodeGenOptions &CGOpts = BuilderWrapper.getCGOpts();
  bool Recover = CGOpts.SanitizeRecover.has(SanitizerKind::Address);
  bool UseAfterScope = CGOpts.SanitizeAddressUseAfterScope;
  bool UseGlobalsGC = asanUseGlobalsGC(T, CGOpts);
  PM.add(createAddressSanitizerFunctionPass(/*CompileKernel*/ false, Recover,
                                            UseAfterScope));
  PM.add(createAddressSanitizerModulePass(/*CompileKernel*/ false, Recover,
                                          UseGlobalsGC));
}

static void addKernelAddressSanitizerPasses(const PassManagerBuilder &Builder,
                                            legacy::PassManagerBase &PM) {
  PM.add(createAddressSanitizerFunctionPass(
      /*CompileKernel*/ true,
      /*Recover*/ true, /*UseAfterScope*/ false));
  PM.add(createAddressSanitizerModulePass(/*CompileKernel*/true,
                                          /*Recover*/true));
}

static void addMemorySanitizerPass(const PassManagerBuilder &Builder,
                                   legacy::PassManagerBase &PM) {
  const PassManagerBuilderWrapper &BuilderWrapper =
      static_cast<const PassManagerBuilderWrapper&>(Builder);
  const CodeGenOptions &CGOpts = BuilderWrapper.getCGOpts();
  int TrackOrigins = CGOpts.SanitizeMemoryTrackOrigins;
  bool Recover = CGOpts.SanitizeRecover.has(SanitizerKind::Memory);
  PM.add(createMemorySanitizerPass(TrackOrigins, Recover));

  // MemorySanitizer inserts complex instrumentation that mostly follows
  // the logic of the original code, but operates on "shadow" values.
  // It can benefit from re-running some general purpose optimization passes.
  if (Builder.OptLevel > 0) {
    PM.add(createEarlyCSEPass());
    PM.add(createReassociatePass());
    PM.add(createLICMPass());
    PM.add(createGVNPass());
    PM.add(createInstructionCombiningPass());
    PM.add(createDeadStoreEliminationPass());
  }
}

static void addThreadSanitizerPass(const PassManagerBuilder &Builder,
                                   legacy::PassManagerBase &PM) {
  PM.add(createThreadSanitizerPass());
}

static void addDataFlowSanitizerPass(const PassManagerBuilder &Builder,
                                     legacy::PassManagerBase &PM) {
  const PassManagerBuilderWrapper &BuilderWrapper =
      static_cast<const PassManagerBuilderWrapper&>(Builder);
  const LangOptions &LangOpts = BuilderWrapper.getLangOpts();
  PM.add(createDataFlowSanitizerPass(LangOpts.SanitizerBlacklistFiles));
}

static void addEfficiencySanitizerPass(const PassManagerBuilder &Builder,
                                       legacy::PassManagerBase &PM) {
  const PassManagerBuilderWrapper &BuilderWrapper =
      static_cast<const PassManagerBuilderWrapper&>(Builder);
  const LangOptions &LangOpts = BuilderWrapper.getLangOpts();
  EfficiencySanitizerOptions Opts;
  if (LangOpts.Sanitize.has(SanitizerKind::EfficiencyCacheFrag))
    Opts.ToolType = EfficiencySanitizerOptions::ESAN_CacheFrag;
  else if (LangOpts.Sanitize.has(SanitizerKind::EfficiencyWorkingSet))
    Opts.ToolType = EfficiencySanitizerOptions::ESAN_WorkingSet;
  PM.add(createEfficiencySanitizerPass(Opts));
}

static TargetLibraryInfoImpl *createTLII(llvm::Triple &TargetTriple,
                                         const CodeGenOptions &CodeGenOpts) {
  TargetLibraryInfoImpl *TLII = new TargetLibraryInfoImpl(TargetTriple);
  if (!CodeGenOpts.SimplifyLibCalls)
    TLII->disableAllFunctions();
  else {
    // Disable individual libc/libm calls in TargetLibraryInfo.
    LibFunc F;
    for (auto &FuncName : CodeGenOpts.getNoBuiltinFuncs())
      if (TLII->getLibFunc(FuncName, F))
        TLII->setUnavailable(F);
  }

  switch (CodeGenOpts.getVecLib()) {
  case CodeGenOptions::Accelerate:
    TLII->addVectorizableFunctionsFromVecLib(TargetLibraryInfoImpl::Accelerate);
    break;
  case CodeGenOptions::SVML:
    TLII->addVectorizableFunctionsFromVecLib(TargetLibraryInfoImpl::SVML);
    break;
  default:
    break;
  }
  return TLII;
}

static void addSymbolRewriterPass(const CodeGenOptions &Opts,
                                  legacy::PassManager *MPM) {
  llvm::SymbolRewriter::RewriteDescriptorList DL;

  llvm::SymbolRewriter::RewriteMapParser MapParser;
  for (const auto &MapFile : Opts.RewriteMapFiles)
    MapParser.parse(MapFile, &DL);

  MPM->add(createRewriteSymbolsPass(DL));
}

static CodeGenOpt::Level getCGOptLevel(const CodeGenOptions &CodeGenOpts) {
  switch (CodeGenOpts.OptimizationLevel) {
  default:
    llvm_unreachable("Invalid optimization level!");
  case 0:
    return CodeGenOpt::None;
  case 1:
    return CodeGenOpt::Less;
  case 2:
    return CodeGenOpt::Default; // O2/Os/Oz
  case 3:
    return CodeGenOpt::Aggressive;
  }
}

static llvm::CodeModel::Model getCodeModel(const CodeGenOptions &CodeGenOpts) {
  unsigned CodeModel =
      llvm::StringSwitch<unsigned>(CodeGenOpts.CodeModel)
      .Case("small", llvm::CodeModel::Small)
      .Case("kernel", llvm::CodeModel::Kernel)
      .Case("medium", llvm::CodeModel::Medium)
      .Case("large", llvm::CodeModel::Large)
      .Case("default", llvm::CodeModel::Default)
      .Default(~0u);
  assert(CodeModel != ~0u && "invalid code model!");
  return static_cast<llvm::CodeModel::Model>(CodeModel);
}

static llvm::Reloc::Model getRelocModel(const CodeGenOptions &CodeGenOpts) {
  // Keep this synced with the equivalent code in
  // lib/Frontend/CompilerInvocation.cpp
  llvm::Optional<llvm::Reloc::Model> RM;
  RM = llvm::StringSwitch<llvm::Reloc::Model>(CodeGenOpts.RelocationModel)
      .Case("static", llvm::Reloc::Static)
      .Case("pic", llvm::Reloc::PIC_)
      .Case("ropi", llvm::Reloc::ROPI)
      .Case("rwpi", llvm::Reloc::RWPI)
      .Case("ropi-rwpi", llvm::Reloc::ROPI_RWPI)
      .Case("dynamic-no-pic", llvm::Reloc::DynamicNoPIC);
  assert(RM.hasValue() && "invalid PIC model!");
  return *RM;
}

static TargetMachine::CodeGenFileType getCodeGenFileType(BackendAction Action) {
  if (Action == Backend_EmitObj)
    return TargetMachine::CGFT_ObjectFile;
  else if (Action == Backend_EmitMCNull)
    return TargetMachine::CGFT_Null;
  else {
    assert(Action == Backend_EmitAssembly && "Invalid action!");
    return TargetMachine::CGFT_AssemblyFile;
  }
}

static void initTargetOptions(llvm::TargetOptions &Options,
                              const CodeGenOptions &CodeGenOpts,
                              const clang::TargetOptions &TargetOpts,
                              const LangOptions &LangOpts,
                              const HeaderSearchOptions &HSOpts) {
  Options.ThreadModel =
      llvm::StringSwitch<llvm::ThreadModel::Model>(CodeGenOpts.ThreadModel)
          .Case("posix", llvm::ThreadModel::POSIX)
          .Case("single", llvm::ThreadModel::Single);

  // Set float ABI type.
  assert((CodeGenOpts.FloatABI == "soft" || CodeGenOpts.FloatABI == "softfp" ||
          CodeGenOpts.FloatABI == "hard" || CodeGenOpts.FloatABI.empty()) &&
         "Invalid Floating Point ABI!");
  Options.FloatABIType =
      llvm::StringSwitch<llvm::FloatABI::ABIType>(CodeGenOpts.FloatABI)
          .Case("soft", llvm::FloatABI::Soft)
          .Case("softfp", llvm::FloatABI::Soft)
          .Case("hard", llvm::FloatABI::Hard)
          .Default(llvm::FloatABI::Default);

  // Set FP fusion mode.
  switch (LangOpts.getDefaultFPContractMode()) {
  case LangOptions::FPC_Off:
    // Preserve any contraction performed by the front-end.  (Strict performs
    // splitting of the muladd instrinsic in the backend.)
    Options.AllowFPOpFusion = llvm::FPOpFusion::Standard;
    break;
  case LangOptions::FPC_On:
    Options.AllowFPOpFusion = llvm::FPOpFusion::Standard;
    break;
  case LangOptions::FPC_Fast:
    Options.AllowFPOpFusion = llvm::FPOpFusion::Fast;
    break;
  }

  Options.UseInitArray = CodeGenOpts.UseInitArray;
  Options.DisableIntegratedAS = CodeGenOpts.DisableIntegratedAS;
  Options.CompressDebugSections = CodeGenOpts.getCompressDebugSections();
  Options.RelaxELFRelocations = CodeGenOpts.RelaxELFRelocations;

  // Set EABI version.
  Options.EABIVersion = TargetOpts.EABIVersion;

  if (LangOpts.SjLjExceptions)
    Options.ExceptionModel = llvm::ExceptionHandling::SjLj;

  Options.NoInfsFPMath = CodeGenOpts.NoInfsFPMath;
  Options.NoNaNsFPMath = CodeGenOpts.NoNaNsFPMath;
  Options.NoZerosInBSS = CodeGenOpts.NoZeroInitializedInBSS;
  Options.UnsafeFPMath = CodeGenOpts.UnsafeFPMath;
  Options.StackAlignmentOverride = CodeGenOpts.StackAlignment;
  Options.FunctionSections = CodeGenOpts.FunctionSections;
  Options.DataSections = CodeGenOpts.DataSections;
  Options.UniqueSectionNames = CodeGenOpts.UniqueSectionNames;
  Options.EmulatedTLS = CodeGenOpts.EmulatedTLS;
  Options.DebuggerTuning = CodeGenOpts.getDebuggerTuning();

  if (CodeGenOpts.EnableSplitDwarf)
    Options.MCOptions.SplitDwarfFile = CodeGenOpts.SplitDwarfFile;
  Options.MCOptions.MCRelaxAll = CodeGenOpts.RelaxAll;
  Options.MCOptions.MCSaveTempLabels = CodeGenOpts.SaveTempLabels;
  Options.MCOptions.MCUseDwarfDirectory = !CodeGenOpts.NoDwarfDirectoryAsm;
  Options.MCOptions.MCNoExecStack = CodeGenOpts.NoExecStack;
  Options.MCOptions.MCIncrementalLinkerCompatible =
      CodeGenOpts.IncrementalLinkerCompatible;
  Options.MCOptions.MCPIECopyRelocations = CodeGenOpts.PIECopyRelocations;
  Options.MCOptions.MCFatalWarnings = CodeGenOpts.FatalWarnings;
  Options.MCOptions.AsmVerbose = CodeGenOpts.AsmVerbose;
  Options.MCOptions.PreserveAsmComments = CodeGenOpts.PreserveAsmComments;
  Options.MCOptions.ABIName = TargetOpts.ABI;
  for (const auto &Entry : HSOpts.UserEntries)
    if (!Entry.IsFramework &&
        (Entry.Group == frontend::IncludeDirGroup::Quoted ||
         Entry.Group == frontend::IncludeDirGroup::Angled ||
         Entry.Group == frontend::IncludeDirGroup::System))
      Options.MCOptions.IASSearchPaths.push_back(
          Entry.IgnoreSysRoot ? Entry.Path : HSOpts.Sysroot + Entry.Path);
}

void EmitAssemblyHelper::CreatePasses(legacy::PassManager &MPM,
                                      legacy::FunctionPassManager &FPM) {
  // Handle disabling of all LLVM passes, where we want to preserve the
  // internal module before any optimization.
  if (CodeGenOpts.DisableLLVMPasses)
    return;

  // Figure out TargetLibraryInfo.  This needs to be added to MPM and FPM
  // manually (and not via PMBuilder), since some passes (eg. InstrProfiling)
  // are inserted before PMBuilder ones - they'd get the default-constructed
  // TLI with an unknown target otherwise.
  Triple TargetTriple(TheModule->getTargetTriple());
  std::unique_ptr<TargetLibraryInfoImpl> TLII(
      createTLII(TargetTriple, CodeGenOpts));

  PassManagerBuilderWrapper PMBuilder(TargetTriple, CodeGenOpts, LangOpts);

  // At O0 and O1 we only run the always inliner which is more efficient. At
  // higher optimization levels we run the normal inliner.
  if (CodeGenOpts.OptimizationLevel <= 1) {
    bool InsertLifetimeIntrinsics = (CodeGenOpts.OptimizationLevel != 0 &&
                                     !CodeGenOpts.DisableLifetimeMarkers);
    PMBuilder.Inliner = createAlwaysInlinerLegacyPass(InsertLifetimeIntrinsics);
  } else {
    // We do not want to inline hot callsites for SamplePGO module-summary build
    // because profile annotation will happen again in ThinLTO backend, and we
    // want the IR of the hot path to match the profile.
    PMBuilder.Inliner = createFunctionInliningPass(
        CodeGenOpts.OptimizationLevel, CodeGenOpts.OptimizeSize,
        (!CodeGenOpts.SampleProfileFile.empty() &&
         CodeGenOpts.EmitSummaryIndex));
  }

  PMBuilder.OptLevel = CodeGenOpts.OptimizationLevel;
  PMBuilder.SizeLevel = CodeGenOpts.OptimizeSize;
  PMBuilder.SLPVectorize = CodeGenOpts.VectorizeSLP;
  PMBuilder.LoopVectorize = CodeGenOpts.VectorizeLoop;

  PMBuilder.DisableUnrollLoops = !CodeGenOpts.UnrollLoops;
  PMBuilder.MergeFunctions = CodeGenOpts.MergeFunctions;
  PMBuilder.PrepareForThinLTO = CodeGenOpts.EmitSummaryIndex;
  PMBuilder.PrepareForLTO = CodeGenOpts.PrepareForLTO;
  PMBuilder.RerollLoops = CodeGenOpts.RerollLoops;

  MPM.add(new TargetLibraryInfoWrapperPass(*TLII));

  if (TM)
    TM->adjustPassManager(PMBuilder);

  if (CodeGenOpts.DebugInfoForProfiling ||
      !CodeGenOpts.SampleProfileFile.empty())
    PMBuilder.addExtension(PassManagerBuilder::EP_EarlyAsPossible,
                           addAddDiscriminatorsPass);

  // In ObjC ARC mode, add the main ARC optimization passes.
  if (LangOpts.ObjCAutoRefCount) {
    PMBuilder.addExtension(PassManagerBuilder::EP_EarlyAsPossible,
                           addObjCARCExpandPass);
    PMBuilder.addExtension(PassManagerBuilder::EP_ModuleOptimizerEarly,
                           addObjCARCAPElimPass);
    PMBuilder.addExtension(PassManagerBuilder::EP_ScalarOptimizerLate,
                           addObjCARCOptPass);
  }

  if (LangOpts.Sanitize.has(SanitizerKind::LocalBounds)) {
    PMBuilder.addExtension(PassManagerBuilder::EP_ScalarOptimizerLate,
                           addBoundsCheckingPass);
    PMBuilder.addExtension(PassManagerBuilder::EP_EnabledOnOptLevel0,
                           addBoundsCheckingPass);
  }

  if (CodeGenOpts.SanitizeCoverageType ||
      CodeGenOpts.SanitizeCoverageIndirectCalls ||
      CodeGenOpts.SanitizeCoverageTraceCmp) {
    PMBuilder.addExtension(PassManagerBuilder::EP_OptimizerLast,
                           addSanitizerCoveragePass);
    PMBuilder.addExtension(PassManagerBuilder::EP_EnabledOnOptLevel0,
                           addSanitizerCoveragePass);
  }

  if (LangOpts.Sanitize.has(SanitizerKind::Address)) {
    PMBuilder.addExtension(PassManagerBuilder::EP_OptimizerLast,
                           addAddressSanitizerPasses);
    PMBuilder.addExtension(PassManagerBuilder::EP_EnabledOnOptLevel0,
                           addAddressSanitizerPasses);
  }

  if (LangOpts.Sanitize.has(SanitizerKind::KernelAddress)) {
    PMBuilder.addExtension(PassManagerBuilder::EP_OptimizerLast,
                           addKernelAddressSanitizerPasses);
    PMBuilder.addExtension(PassManagerBuilder::EP_EnabledOnOptLevel0,
                           addKernelAddressSanitizerPasses);
  }

  if (LangOpts.Sanitize.has(SanitizerKind::Memory)) {
    PMBuilder.addExtension(PassManagerBuilder::EP_OptimizerLast,
                           addMemorySanitizerPass);
    PMBuilder.addExtension(PassManagerBuilder::EP_EnabledOnOptLevel0,
                           addMemorySanitizerPass);
  }

  if (LangOpts.Sanitize.has(SanitizerKind::Thread)) {
    PMBuilder.addExtension(PassManagerBuilder::EP_OptimizerLast,
                           addThreadSanitizerPass);
    PMBuilder.addExtension(PassManagerBuilder::EP_EnabledOnOptLevel0,
                           addThreadSanitizerPass);
  }

  if (LangOpts.Sanitize.has(SanitizerKind::DataFlow)) {
    PMBuilder.addExtension(PassManagerBuilder::EP_OptimizerLast,
                           addDataFlowSanitizerPass);
    PMBuilder.addExtension(PassManagerBuilder::EP_EnabledOnOptLevel0,
                           addDataFlowSanitizerPass);
  }

  if (LangOpts.CoroutinesTS)
    addCoroutinePassesToExtensionPoints(PMBuilder);

  if (LangOpts.Sanitize.hasOneOf(SanitizerKind::Efficiency)) {
    PMBuilder.addExtension(PassManagerBuilder::EP_OptimizerLast,
                           addEfficiencySanitizerPass);
    PMBuilder.addExtension(PassManagerBuilder::EP_EnabledOnOptLevel0,
                           addEfficiencySanitizerPass);
  }

  // Set up the per-function pass manager.
  FPM.add(new TargetLibraryInfoWrapperPass(*TLII));
  if (CodeGenOpts.VerifyModule)
    FPM.add(createVerifierPass());

  // Set up the per-module pass manager.
  if (!CodeGenOpts.RewriteMapFiles.empty())
    addSymbolRewriterPass(CodeGenOpts, &MPM);

  if (!CodeGenOpts.DisableGCov &&
      (CodeGenOpts.EmitGcovArcs || CodeGenOpts.EmitGcovNotes)) {
    // Not using 'GCOVOptions::getDefault' allows us to avoid exiting if
    // LLVM's -default-gcov-version flag is set to something invalid.
    GCOVOptions Options;
    Options.EmitNotes = CodeGenOpts.EmitGcovNotes;
    Options.EmitData = CodeGenOpts.EmitGcovArcs;
    memcpy(Options.Version, CodeGenOpts.CoverageVersion, 4);
    Options.UseCfgChecksum = CodeGenOpts.CoverageExtraChecksum;
    Options.NoRedZone = CodeGenOpts.DisableRedZone;
    Options.FunctionNamesInData =
        !CodeGenOpts.CoverageNoFunctionNamesInData;
    Options.ExitBlockBeforeBody = CodeGenOpts.CoverageExitBlockBeforeBody;
    MPM.add(createGCOVProfilerPass(Options));
    if (CodeGenOpts.getDebugInfo() == codegenoptions::NoDebugInfo)
      MPM.add(createStripSymbolsPass(true));
  }

  if (CodeGenOpts.hasProfileClangInstr()) {
    InstrProfOptions Options;
    Options.NoRedZone = CodeGenOpts.DisableRedZone;
    Options.InstrProfileOutput = CodeGenOpts.InstrProfileOutput;
    MPM.add(createInstrProfilingLegacyPass(Options));
  }
  if (CodeGenOpts.hasProfileIRInstr()) {
    PMBuilder.EnablePGOInstrGen = true;
    if (!CodeGenOpts.InstrProfileOutput.empty())
      PMBuilder.PGOInstrGen = CodeGenOpts.InstrProfileOutput;
    else
      PMBuilder.PGOInstrGen = DefaultProfileGenName;
  }
  if (CodeGenOpts.hasProfileIRUse())
    PMBuilder.PGOInstrUse = CodeGenOpts.ProfileInstrumentUsePath;

  if (!CodeGenOpts.SampleProfileFile.empty())
    PMBuilder.PGOSampleUse = CodeGenOpts.SampleProfileFile;

  PMBuilder.populateFunctionPassManager(FPM);
  PMBuilder.populateModulePassManager(MPM);
}

static void setCommandLineOpts(const CodeGenOptions &CodeGenOpts) {
  SmallVector<const char *, 16> BackendArgs;
  BackendArgs.push_back("clang"); // Fake program name.
  if (!CodeGenOpts.DebugPass.empty()) {
    BackendArgs.push_back("-debug-pass");
    BackendArgs.push_back(CodeGenOpts.DebugPass.c_str());
  }
  if (!CodeGenOpts.LimitFloatPrecision.empty()) {
    BackendArgs.push_back("-limit-float-precision");
    BackendArgs.push_back(CodeGenOpts.LimitFloatPrecision.c_str());
  }
  for (const std::string &BackendOption : CodeGenOpts.BackendOptions)
    BackendArgs.push_back(BackendOption.c_str());
  BackendArgs.push_back(nullptr);
  llvm::cl::ParseCommandLineOptions(BackendArgs.size() - 1,
                                    BackendArgs.data());
}

void EmitAssemblyHelper::CreateTargetMachine(bool MustCreateTM) {
  // Create the TargetMachine for generating code.
  std::string Error;
  std::string Triple = TheModule->getTargetTriple();
  const llvm::Target *TheTarget = TargetRegistry::lookupTarget(Triple, Error);
  if (!TheTarget) {
    if (MustCreateTM)
      Diags.Report(diag::err_fe_unable_to_create_target) << Error;
    return;
  }

  llvm::CodeModel::Model CM  = getCodeModel(CodeGenOpts);
  std::string FeaturesStr =
      llvm::join(TargetOpts.Features.begin(), TargetOpts.Features.end(), ",");
  llvm::Reloc::Model RM = getRelocModel(CodeGenOpts);
  CodeGenOpt::Level OptLevel = getCGOptLevel(CodeGenOpts);

  llvm::TargetOptions Options;
  initTargetOptions(Options, CodeGenOpts, TargetOpts, LangOpts, HSOpts);
  TM.reset(TheTarget->createTargetMachine(Triple, TargetOpts.CPU, FeaturesStr,
                                          Options, RM, CM, OptLevel));
}

bool EmitAssemblyHelper::AddEmitPasses(legacy::PassManager &CodeGenPasses,
                                       BackendAction Action,
                                       raw_pwrite_stream &OS) {
  // Add LibraryInfo.
  llvm::Triple TargetTriple(TheModule->getTargetTriple());
  std::unique_ptr<TargetLibraryInfoImpl> TLII(
      createTLII(TargetTriple, CodeGenOpts));
  CodeGenPasses.add(new TargetLibraryInfoWrapperPass(*TLII));

  // Normal mode, emit a .s or .o file by running the code generator. Note,
  // this also adds codegenerator level optimization passes.
  TargetMachine::CodeGenFileType CGFT = getCodeGenFileType(Action);

  // Add ObjC ARC final-cleanup optimizations. This is done as part of the
  // "codegen" passes so that it isn't run multiple times when there is
  // inlining happening.
  if (CodeGenOpts.OptimizationLevel > 0)
    CodeGenPasses.add(createObjCARCContractPass());

  if (TM->addPassesToEmitFile(CodeGenPasses, OS, CGFT,
                              /*DisableVerify=*/!CodeGenOpts.VerifyModule)) {
    Diags.Report(diag::err_fe_unable_to_interface_with_target);
    return false;
  }

  return true;
}

void EmitAssemblyHelper::EmitAssembly(BackendAction Action,
                                      std::unique_ptr<raw_pwrite_stream> OS) {
  TimeRegion Region(llvm::TimePassesIsEnabled ? &CodeGenerationTime : nullptr);

  setCommandLineOpts(CodeGenOpts);

  bool UsesCodeGen = (Action != Backend_EmitNothing &&
                      Action != Backend_EmitBC &&
                      Action != Backend_EmitLL);
  CreateTargetMachine(UsesCodeGen);

  if (UsesCodeGen && !TM)
    return;
  if (TM)
    TheModule->setDataLayout(TM->createDataLayout());

  legacy::PassManager PerModulePasses;
  PerModulePasses.add(
      createTargetTransformInfoWrapperPass(getTargetIRAnalysis()));

  legacy::FunctionPassManager PerFunctionPasses(TheModule);
  PerFunctionPasses.add(
      createTargetTransformInfoWrapperPass(getTargetIRAnalysis()));

  CreatePasses(PerModulePasses, PerFunctionPasses);

  legacy::PassManager CodeGenPasses;
  CodeGenPasses.add(
      createTargetTransformInfoWrapperPass(getTargetIRAnalysis()));

  std::unique_ptr<raw_fd_ostream> ThinLinkOS;

  switch (Action) {
  case Backend_EmitNothing:
    break;

  case Backend_EmitBC:
    if (CodeGenOpts.EmitSummaryIndex) {
      if (!CodeGenOpts.ThinLinkBitcodeFile.empty()) {
        std::error_code EC;
        ThinLinkOS.reset(new llvm::raw_fd_ostream(
            CodeGenOpts.ThinLinkBitcodeFile, EC,
            llvm::sys::fs::F_None));
        if (EC) {
          Diags.Report(diag::err_fe_unable_to_open_output) << CodeGenOpts.ThinLinkBitcodeFile
                                                           << EC.message();
          return;
        }
      }
      PerModulePasses.add(
          createWriteThinLTOBitcodePass(*OS, ThinLinkOS.get()));
    }
    else
      PerModulePasses.add(
          createBitcodeWriterPass(*OS, CodeGenOpts.EmitLLVMUseLists));
    break;

  case Backend_EmitLL:
    PerModulePasses.add(
        createPrintModulePass(*OS, "", CodeGenOpts.EmitLLVMUseLists));
    break;

  default:
    if (!AddEmitPasses(CodeGenPasses, Action, *OS))
      return;
  }

  // Before executing passes, print the final values of the LLVM options.
  cl::PrintOptionValues();

  // Run passes. For now we do all passes at once, but eventually we
  // would like to have the option of streaming code generation.

  {
    PrettyStackTraceString CrashInfo("Per-function optimization");

    PerFunctionPasses.doInitialization();
    for (Function &F : *TheModule)
      if (!F.isDeclaration())
        PerFunctionPasses.run(F);
    PerFunctionPasses.doFinalization();
  }

  {
    PrettyStackTraceString CrashInfo("Per-module optimization passes");
    PerModulePasses.run(*TheModule);
  }

  {
    PrettyStackTraceString CrashInfo("Code generation");
    CodeGenPasses.run(*TheModule);
  }
}

static PassBuilder::OptimizationLevel mapToLevel(const CodeGenOptions &Opts) {
  switch (Opts.OptimizationLevel) {
  default:
    llvm_unreachable("Invalid optimization level!");

  case 1:
    return PassBuilder::O1;

  case 2:
    switch (Opts.OptimizeSize) {
    default:
      llvm_unreachable("Invalide optimization level for size!");

    case 0:
      return PassBuilder::O2;

    case 1:
      return PassBuilder::Os;

    case 2:
      return PassBuilder::Oz;
    }

  case 3:
    return PassBuilder::O3;
  }
}

/// A clean version of `EmitAssembly` that uses the new pass manager.
///
/// Not all features are currently supported in this system, but where
/// necessary it falls back to the legacy pass manager to at least provide
/// basic functionality.
///
/// This API is planned to have its functionality finished and then to replace
/// `EmitAssembly` at some point in the future when the default switches.
void EmitAssemblyHelper::EmitAssemblyWithNewPassManager(
    BackendAction Action, std::unique_ptr<raw_pwrite_stream> OS) {
  TimeRegion Region(llvm::TimePassesIsEnabled ? &CodeGenerationTime : nullptr);
  setCommandLineOpts(CodeGenOpts);

  // The new pass manager always makes a target machine available to passes
  // during construction.
  CreateTargetMachine(/*MustCreateTM*/ true);
  if (!TM)
    // This will already be diagnosed, just bail.
    return;
  TheModule->setDataLayout(TM->createDataLayout());

  PGOOptions PGOOpt;

  // -fprofile-generate.
  PGOOpt.RunProfileGen = CodeGenOpts.hasProfileIRInstr();
  if (PGOOpt.RunProfileGen)
    PGOOpt.ProfileGenFile = CodeGenOpts.InstrProfileOutput.empty() ?
      DefaultProfileGenName : CodeGenOpts.InstrProfileOutput;

  // -fprofile-use.
  if (CodeGenOpts.hasProfileIRUse())
    PGOOpt.ProfileUseFile = CodeGenOpts.ProfileInstrumentUsePath;

  if (!CodeGenOpts.SampleProfileFile.empty())
    PGOOpt.SampleProfileFile = CodeGenOpts.SampleProfileFile;

  // Only pass a PGO options struct if -fprofile-generate or
  // -fprofile-use were passed on the cmdline.
  PassBuilder PB(TM.get(),
    (PGOOpt.RunProfileGen ||
      !PGOOpt.ProfileUseFile.empty() ||
      !PGOOpt.SampleProfileFile.empty()) ?
        Optional<PGOOptions>(PGOOpt) : None);

  LoopAnalysisManager LAM;
  FunctionAnalysisManager FAM;
  CGSCCAnalysisManager CGAM;
  ModuleAnalysisManager MAM;

  // Register the AA manager first so that our version is the one used.
  FAM.registerPass([&] { return PB.buildDefaultAAPipeline(); });

  // Register all the basic analyses with the managers.
  PB.registerModuleAnalyses(MAM);
  PB.registerCGSCCAnalyses(CGAM);
  PB.registerFunctionAnalyses(FAM);
  PB.registerLoopAnalyses(LAM);
  PB.crossRegisterProxies(LAM, FAM, CGAM, MAM);

  ModulePassManager MPM(CodeGenOpts.DebugPassManager);

  if (!CodeGenOpts.DisableLLVMPasses) {
    bool IsThinLTO = CodeGenOpts.EmitSummaryIndex;
    bool IsLTO = CodeGenOpts.PrepareForLTO;

    if (CodeGenOpts.OptimizationLevel == 0) {
      // Build a minimal pipeline based on the semantics required by Clang,
      // which is just that always inlining occurs.
      MPM.addPass(AlwaysInlinerPass());
      if (IsThinLTO)
        MPM.addPass(NameAnonGlobalPass());
    } else {
      // Map our optimization levels into one of the distinct levels used to
      // configure the pipeline.
      PassBuilder::OptimizationLevel Level = mapToLevel(CodeGenOpts);

      if (IsThinLTO) {
        MPM = PB.buildThinLTOPreLinkDefaultPipeline(
            Level, CodeGenOpts.DebugPassManager);
        MPM.addPass(NameAnonGlobalPass());
      } else if (IsLTO) {
        MPM = PB.buildLTOPreLinkDefaultPipeline(Level,
                                                CodeGenOpts.DebugPassManager);
      } else {
        MPM = PB.buildPerModuleDefaultPipeline(Level,
                                               CodeGenOpts.DebugPassManager);
      }
    }
  }

  // FIXME: We still use the legacy pass manager to do code generation. We
  // create that pass manager here and use it as needed below.
  legacy::PassManager CodeGenPasses;
  bool NeedCodeGen = false;
  Optional<raw_fd_ostream> ThinLinkOS;

  // Append any output we need to the pass manager.
  switch (Action) {
  case Backend_EmitNothing:
    break;

  case Backend_EmitBC:
    if (CodeGenOpts.EmitSummaryIndex) {
      if (!CodeGenOpts.ThinLinkBitcodeFile.empty()) {
        std::error_code EC;
        ThinLinkOS.emplace(CodeGenOpts.ThinLinkBitcodeFile, EC,
                           llvm::sys::fs::F_None);
        if (EC) {
          Diags.Report(diag::err_fe_unable_to_open_output)
              << CodeGenOpts.ThinLinkBitcodeFile << EC.message();
          return;
        }
      }
      MPM.addPass(
          ThinLTOBitcodeWriterPass(*OS, ThinLinkOS ? &*ThinLinkOS : nullptr));
    } else {
      MPM.addPass(BitcodeWriterPass(*OS, CodeGenOpts.EmitLLVMUseLists,
                                    CodeGenOpts.EmitSummaryIndex,
                                    CodeGenOpts.EmitSummaryIndex));
    }
    break;

  case Backend_EmitLL:
    MPM.addPass(PrintModulePass(*OS, "", CodeGenOpts.EmitLLVMUseLists));
    break;

  case Backend_EmitAssembly:
  case Backend_EmitMCNull:
  case Backend_EmitObj:
    NeedCodeGen = true;
    CodeGenPasses.add(
        createTargetTransformInfoWrapperPass(getTargetIRAnalysis()));
    if (!AddEmitPasses(CodeGenPasses, Action, *OS))
      // FIXME: Should we handle this error differently?
      return;
    break;
  }

  // Before executing passes, print the final values of the LLVM options.
  cl::PrintOptionValues();

  // Now that we have all of the passes ready, run them.
  {
    PrettyStackTraceString CrashInfo("Optimizer");
    MPM.run(*TheModule, MAM);
  }

  // Now if needed, run the legacy PM for codegen.
  if (NeedCodeGen) {
    PrettyStackTraceString CrashInfo("Code generation");
    CodeGenPasses.run(*TheModule);
  }
}

Expected<BitcodeModule> clang::FindThinLTOModule(MemoryBufferRef MBRef) {
  Expected<std::vector<BitcodeModule>> BMsOrErr = getBitcodeModuleList(MBRef);
  if (!BMsOrErr)
    return BMsOrErr.takeError();

  // The bitcode file may contain multiple modules, we want the one that is
  // marked as being the ThinLTO module.
  for (BitcodeModule &BM : *BMsOrErr) {
    Expected<BitcodeLTOInfo> LTOInfo = BM.getLTOInfo();
    if (LTOInfo && LTOInfo->IsThinLTO)
      return BM;
  }

  return make_error<StringError>("Could not find module summary",
                                 inconvertibleErrorCode());
}

static void runThinLTOBackend(ModuleSummaryIndex *CombinedIndex, Module *M,
                              const HeaderSearchOptions &HeaderOpts,
                              const CodeGenOptions &CGOpts,
                              const clang::TargetOptions &TOpts,
                              const LangOptions &LOpts,
                              std::unique_ptr<raw_pwrite_stream> OS,
                              std::string SampleProfile,
                              BackendAction Action) {
  StringMap<DenseMap<GlobalValue::GUID, GlobalValueSummary *>>
      ModuleToDefinedGVSummaries;
  CombinedIndex->collectDefinedGVSummariesPerModule(ModuleToDefinedGVSummaries);

  setCommandLineOpts(CGOpts);

  // We can simply import the values mentioned in the combined index, since
  // we should only invoke this using the individual indexes written out
  // via a WriteIndexesThinBackend.
  FunctionImporter::ImportMapTy ImportList;
  for (auto &GlobalList : *CombinedIndex) {
    // Ignore entries for undefined references.
    if (GlobalList.second.SummaryList.empty())
      continue;

    auto GUID = GlobalList.first;
    assert(GlobalList.second.SummaryList.size() == 1 &&
           "Expected individual combined index to have one summary per GUID");
    auto &Summary = GlobalList.second.SummaryList[0];
    // Skip the summaries for the importing module. These are included to
    // e.g. record required linkage changes.
    if (Summary->modulePath() == M->getModuleIdentifier())
      continue;
    // Doesn't matter what value we plug in to the map, just needs an entry
    // to provoke importing by thinBackend.
    ImportList[Summary->modulePath()][GUID] = 1;
  }

  std::vector<std::unique_ptr<llvm::MemoryBuffer>> OwnedImports;
  MapVector<llvm::StringRef, llvm::BitcodeModule> ModuleMap;

  for (auto &I : ImportList) {
    ErrorOr<std::unique_ptr<llvm::MemoryBuffer>> MBOrErr =
        llvm::MemoryBuffer::getFile(I.first());
    if (!MBOrErr) {
      errs() << "Error loading imported file '" << I.first()
             << "': " << MBOrErr.getError().message() << "\n";
      return;
    }

    Expected<BitcodeModule> BMOrErr = FindThinLTOModule(**MBOrErr);
    if (!BMOrErr) {
      handleAllErrors(BMOrErr.takeError(), [&](ErrorInfoBase &EIB) {
        errs() << "Error loading imported file '" << I.first()
               << "': " << EIB.message() << '\n';
      });
      return;
    }
    ModuleMap.insert({I.first(), *BMOrErr});

    OwnedImports.push_back(std::move(*MBOrErr));
  }
  auto AddStream = [&](size_t Task) {
    return llvm::make_unique<lto::NativeObjectStream>(std::move(OS));
  };
  lto::Config Conf;
  Conf.CPU = TOpts.CPU;
  Conf.CodeModel = getCodeModel(CGOpts);
  Conf.MAttrs = TOpts.Features;
  Conf.RelocModel = getRelocModel(CGOpts);
  Conf.CGOptLevel = getCGOptLevel(CGOpts);
  initTargetOptions(Conf.Options, CGOpts, TOpts, LOpts, HeaderOpts);
  Conf.SampleProfile = std::move(SampleProfile);
  Conf.UseNewPM = CGOpts.ExperimentalNewPassManager;
  switch (Action) {
  case Backend_EmitNothing:
    Conf.PreCodeGenModuleHook = [](size_t Task, const Module &Mod) {
      return false;
    };
    break;
  case Backend_EmitLL:
    Conf.PreCodeGenModuleHook = [&](size_t Task, const Module &Mod) {
      M->print(*OS, nullptr, CGOpts.EmitLLVMUseLists);
      return false;
    };
    break;
  case Backend_EmitBC:
    Conf.PreCodeGenModuleHook = [&](size_t Task, const Module &Mod) {
      WriteBitcodeToFile(M, *OS, CGOpts.EmitLLVMUseLists);
      return false;
    };
    break;
  default:
    Conf.CGFileType = getCodeGenFileType(Action);
    break;
  }
  if (Error E = thinBackend(
          Conf, 0, AddStream, *M, *CombinedIndex, ImportList,
          ModuleToDefinedGVSummaries[M->getModuleIdentifier()], ModuleMap)) {
    handleAllErrors(std::move(E), [&](ErrorInfoBase &EIB) {
      errs() << "Error running ThinLTO backend: " << EIB.message() << '\n';
    });
  }
}

void clang::EmitBackendOutput(DiagnosticsEngine &Diags,
                              const HeaderSearchOptions &HeaderOpts,
                              const CodeGenOptions &CGOpts,
                              const clang::TargetOptions &TOpts,
                              const LangOptions &LOpts,
                              const llvm::DataLayout &TDesc, Module *M,
                              BackendAction Action,
                              std::unique_ptr<raw_pwrite_stream> OS) {
  if (!CGOpts.ThinLTOIndexFile.empty()) {
    // If we are performing a ThinLTO importing compile, load the function index
    // into memory and pass it into runThinLTOBackend, which will run the
    // function importer and invoke LTO passes.
    Expected<std::unique_ptr<ModuleSummaryIndex>> IndexOrErr =
        llvm::getModuleSummaryIndexForFile(CGOpts.ThinLTOIndexFile,
                                           /*IgnoreEmptyThinLTOIndexFile*/true);
    if (!IndexOrErr) {
      logAllUnhandledErrors(IndexOrErr.takeError(), errs(),
                            "Error loading index file '" +
                            CGOpts.ThinLTOIndexFile + "': ");
      return;
    }
    std::unique_ptr<ModuleSummaryIndex> CombinedIndex = std::move(*IndexOrErr);
    // A null CombinedIndex means we should skip ThinLTO compilation
    // (LLVM will optionally ignore empty index files, returning null instead
    // of an error).
    bool DoThinLTOBackend = CombinedIndex != nullptr;
    if (DoThinLTOBackend) {
      runThinLTOBackend(CombinedIndex.get(), M, HeaderOpts, CGOpts, TOpts,
                        LOpts, std::move(OS), CGOpts.SampleProfileFile, Action);
      return;
    }
  }

  EmitAssemblyHelper AsmHelper(Diags, HeaderOpts, CGOpts, TOpts, LOpts, M);

  if (CGOpts.ExperimentalNewPassManager)
    AsmHelper.EmitAssemblyWithNewPassManager(Action, std::move(OS));
  else
    AsmHelper.EmitAssembly(Action, std::move(OS));

  // Verify clang's TargetInfo DataLayout against the LLVM TargetMachine's
  // DataLayout.
  if (AsmHelper.TM) {
    std::string DLDesc = M->getDataLayout().getStringRepresentation();
    if (DLDesc != TDesc.getStringRepresentation()) {
      unsigned DiagID = Diags.getCustomDiagID(
          DiagnosticsEngine::Error, "backend data layout '%0' does not match "
                                    "expected target description '%1'");
      Diags.Report(DiagID) << DLDesc << TDesc.getStringRepresentation();
    }
  }
}

static const char* getSectionNameForBitcode(const Triple &T) {
  switch (T.getObjectFormat()) {
  case Triple::MachO:
    return "__LLVM,__bitcode";
  case Triple::COFF:
  case Triple::ELF:
  case Triple::Wasm:
  case Triple::UnknownObjectFormat:
    return ".llvmbc";
  }
  llvm_unreachable("Unimplemented ObjectFormatType");
}

static const char* getSectionNameForCommandline(const Triple &T) {
  switch (T.getObjectFormat()) {
  case Triple::MachO:
    return "__LLVM,__cmdline";
  case Triple::COFF:
  case Triple::ELF:
  case Triple::Wasm:
  case Triple::UnknownObjectFormat:
    return ".llvmcmd";
  }
  llvm_unreachable("Unimplemented ObjectFormatType");
}

// With -fembed-bitcode, save a copy of the llvm IR as data in the
// __LLVM,__bitcode section.
void clang::EmbedBitcode(llvm::Module *M, const CodeGenOptions &CGOpts,
                         llvm::MemoryBufferRef Buf) {
  if (CGOpts.getEmbedBitcode() == CodeGenOptions::Embed_Off)
    return;

  // Save llvm.compiler.used and remote it.
  SmallVector<Constant*, 2> UsedArray;
  SmallSet<GlobalValue*, 4> UsedGlobals;
  Type *UsedElementType = Type::getInt8Ty(M->getContext())->getPointerTo(0);
  GlobalVariable *Used = collectUsedGlobalVariables(*M, UsedGlobals, true);
  for (auto *GV : UsedGlobals) {
    if (GV->getName() != "llvm.embedded.module" &&
        GV->getName() != "llvm.cmdline")
      UsedArray.push_back(
          ConstantExpr::getPointerBitCastOrAddrSpaceCast(GV, UsedElementType));
  }
  if (Used)
    Used->eraseFromParent();

  // Embed the bitcode for the llvm module.
  std::string Data;
  ArrayRef<uint8_t> ModuleData;
  Triple T(M->getTargetTriple());
  // Create a constant that contains the bitcode.
  // In case of embedding a marker, ignore the input Buf and use the empty
  // ArrayRef. It is also legal to create a bitcode marker even Buf is empty.
  if (CGOpts.getEmbedBitcode() != CodeGenOptions::Embed_Marker) {
    if (!isBitcode((const unsigned char *)Buf.getBufferStart(),
                   (const unsigned char *)Buf.getBufferEnd())) {
      // If the input is LLVM Assembly, bitcode is produced by serializing
      // the module. Use-lists order need to be perserved in this case.
      llvm::raw_string_ostream OS(Data);
      llvm::WriteBitcodeToFile(M, OS, /* ShouldPreserveUseListOrder */ true);
      ModuleData =
          ArrayRef<uint8_t>((const uint8_t *)OS.str().data(), OS.str().size());
    } else
      // If the input is LLVM bitcode, write the input byte stream directly.
      ModuleData = ArrayRef<uint8_t>((const uint8_t *)Buf.getBufferStart(),
                                     Buf.getBufferSize());
  }
  llvm::Constant *ModuleConstant =
      llvm::ConstantDataArray::get(M->getContext(), ModuleData);
  llvm::GlobalVariable *GV = new llvm::GlobalVariable(
      *M, ModuleConstant->getType(), true, llvm::GlobalValue::PrivateLinkage,
      ModuleConstant);
  GV->setSection(getSectionNameForBitcode(T));
  UsedArray.push_back(
      ConstantExpr::getPointerBitCastOrAddrSpaceCast(GV, UsedElementType));
  if (llvm::GlobalVariable *Old =
          M->getGlobalVariable("llvm.embedded.module", true)) {
    assert(Old->hasOneUse() &&
           "llvm.embedded.module can only be used once in llvm.compiler.used");
    GV->takeName(Old);
    Old->eraseFromParent();
  } else {
    GV->setName("llvm.embedded.module");
  }

  // Skip if only bitcode needs to be embedded.
  if (CGOpts.getEmbedBitcode() != CodeGenOptions::Embed_Bitcode) {
    // Embed command-line options.
    ArrayRef<uint8_t> CmdData(const_cast<uint8_t *>(CGOpts.CmdArgs.data()),
                              CGOpts.CmdArgs.size());
    llvm::Constant *CmdConstant =
      llvm::ConstantDataArray::get(M->getContext(), CmdData);
    GV = new llvm::GlobalVariable(*M, CmdConstant->getType(), true,
                                  llvm::GlobalValue::PrivateLinkage,
                                  CmdConstant);
    GV->setSection(getSectionNameForCommandline(T));
    UsedArray.push_back(
        ConstantExpr::getPointerBitCastOrAddrSpaceCast(GV, UsedElementType));
    if (llvm::GlobalVariable *Old =
            M->getGlobalVariable("llvm.cmdline", true)) {
      assert(Old->hasOneUse() &&
             "llvm.cmdline can only be used once in llvm.compiler.used");
      GV->takeName(Old);
      Old->eraseFromParent();
    } else {
      GV->setName("llvm.cmdline");
    }
  }

  if (UsedArray.empty())
    return;

  // Recreate llvm.compiler.used.
  ArrayType *ATy = ArrayType::get(UsedElementType, UsedArray.size());
  auto *NewUsed = new GlobalVariable(
      *M, ATy, false, llvm::GlobalValue::AppendingLinkage,
      llvm::ConstantArray::get(ATy, UsedArray), "llvm.compiler.used");
  NewUsed->setSection("llvm.metadata");
}
