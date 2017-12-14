//===--- CommonArgs.cpp - Args handling for multiple toolchains -*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "CommonArgs.h"
#include "InputInfo.h"
#include "Hexagon.h"
#include "Arch/AArch64.h"
#include "Arch/ARM.h"
#include "Arch/Mips.h"
#include "Arch/PPC.h"
#include "Arch/SystemZ.h"
#include "Arch/X86.h"
#include "clang/Basic/CharInfo.h"
#include "clang/Basic/LangOptions.h"
#include "clang/Basic/ObjCRuntime.h"
#include "clang/Basic/Version.h"
#include "clang/Basic/VirtualFileSystem.h"
#include "clang/Config/config.h"
#include "clang/Driver/Action.h"
#include "clang/Driver/Compilation.h"
#include "clang/Driver/Driver.h"
#include "clang/Driver/DriverDiagnostic.h"
#include "clang/Driver/Job.h"
#include "clang/Driver/Options.h"
#include "clang/Driver/SanitizerArgs.h"
#include "clang/Driver/ToolChain.h"
#include "clang/Driver/Util.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringSwitch.h"
#include "llvm/ADT/Twine.h"
#include "llvm/Option/Arg.h"
#include "llvm/Option/ArgList.h"
#include "llvm/Option/Option.h"
#include "llvm/Support/CodeGen.h"
#include "llvm/Support/Compression.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Host.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/Process.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/ScopedPrinter.h"
#include "llvm/Support/TargetParser.h"
#include "llvm/Support/YAMLParser.h"

using namespace clang::driver;
using namespace clang::driver::tools;
using namespace clang;
using namespace llvm::opt;

void tools::addPathIfExists(const Driver &D, const Twine &Path,
                            ToolChain::path_list &Paths) {
  if (D.getVFS().exists(Path))
    Paths.push_back(Path.str());
}

void tools::handleTargetFeaturesGroup(const ArgList &Args,
                                      std::vector<StringRef> &Features,
                                      OptSpecifier Group) {
  for (const Arg *A : Args.filtered(Group)) {
    StringRef Name = A->getOption().getName();
    A->claim();

    // Skip over "-m".
    assert(Name.startswith("m") && "Invalid feature name.");
    Name = Name.substr(1);

    bool IsNegative = Name.startswith("no-");
    if (IsNegative)
      Name = Name.substr(3);
    Features.push_back(Args.MakeArgString((IsNegative ? "-" : "+") + Name));
  }
}

void tools::addDirectoryList(const ArgList &Args, ArgStringList &CmdArgs,
                             const char *ArgName, const char *EnvVar) {
  const char *DirList = ::getenv(EnvVar);
  bool CombinedArg = false;

  if (!DirList)
    return; // Nothing to do.

  StringRef Name(ArgName);
  if (Name.equals("-I") || Name.equals("-L"))
    CombinedArg = true;

  StringRef Dirs(DirList);
  if (Dirs.empty()) // Empty string should not add '.'.
    return;

  StringRef::size_type Delim;
  while ((Delim = Dirs.find(llvm::sys::EnvPathSeparator)) != StringRef::npos) {
    if (Delim == 0) { // Leading colon.
      if (CombinedArg) {
        CmdArgs.push_back(Args.MakeArgString(std::string(ArgName) + "."));
      } else {
        CmdArgs.push_back(ArgName);
        CmdArgs.push_back(".");
      }
    } else {
      if (CombinedArg) {
        CmdArgs.push_back(
            Args.MakeArgString(std::string(ArgName) + Dirs.substr(0, Delim)));
      } else {
        CmdArgs.push_back(ArgName);
        CmdArgs.push_back(Args.MakeArgString(Dirs.substr(0, Delim)));
      }
    }
    Dirs = Dirs.substr(Delim + 1);
  }

  if (Dirs.empty()) { // Trailing colon.
    if (CombinedArg) {
      CmdArgs.push_back(Args.MakeArgString(std::string(ArgName) + "."));
    } else {
      CmdArgs.push_back(ArgName);
      CmdArgs.push_back(".");
    }
  } else { // Add the last path.
    if (CombinedArg) {
      CmdArgs.push_back(Args.MakeArgString(std::string(ArgName) + Dirs));
    } else {
      CmdArgs.push_back(ArgName);
      CmdArgs.push_back(Args.MakeArgString(Dirs));
    }
  }
}

void tools::AddLinkerInputs(const ToolChain &TC, const InputInfoList &Inputs,
                            const ArgList &Args, ArgStringList &CmdArgs,
                            const JobAction &JA) {
  const Driver &D = TC.getDriver();

  // Add extra linker input arguments which are not treated as inputs
  // (constructed via -Xarch_).
  Args.AddAllArgValues(CmdArgs, options::OPT_Zlinker_input);

  for (const auto &II : Inputs) {
    // If the current tool chain refers to an OpenMP offloading host, we should
    // ignore inputs that refer to OpenMP offloading devices - they will be
    // embedded according to a proper linker script.
    if (auto *IA = II.getAction())
      if (JA.isHostOffloading(Action::OFK_OpenMP) &&
          IA->isDeviceOffloading(Action::OFK_OpenMP))
        continue;

    if (!TC.HasNativeLLVMSupport() && types::isLLVMIR(II.getType()))
      // Don't try to pass LLVM inputs unless we have native support.
      D.Diag(diag::err_drv_no_linker_llvm_support) << TC.getTripleString();

    // Add filenames immediately.
    if (II.isFilename()) {
      CmdArgs.push_back(II.getFilename());
      continue;
    }

    // Otherwise, this is a linker input argument.
    const Arg &A = II.getInputArg();

    // Handle reserved library options.
    if (A.getOption().matches(options::OPT_Z_reserved_lib_stdcxx))
      TC.AddCXXStdlibLibArgs(Args, CmdArgs);
    else if (A.getOption().matches(options::OPT_Z_reserved_lib_cckext))
      TC.AddCCKextLibArgs(Args, CmdArgs);
    else if (A.getOption().matches(options::OPT_z)) {
      // Pass -z prefix for gcc linker compatibility.
      A.claim();
      A.render(Args, CmdArgs);
    } else {
      A.renderAsInput(Args, CmdArgs);
    }
  }

  // LIBRARY_PATH - included following the user specified library paths.
  //                and only supported on native toolchains.
  if (!TC.isCrossCompiling()) {
    addDirectoryList(Args, CmdArgs, "-L", "LIBRARY_PATH");
  }
}

void tools::AddTargetFeature(const ArgList &Args,
                             std::vector<StringRef> &Features,
                             OptSpecifier OnOpt, OptSpecifier OffOpt,
                             StringRef FeatureName) {
  if (Arg *A = Args.getLastArg(OnOpt, OffOpt)) {
    if (A->getOption().matches(OnOpt))
      Features.push_back(Args.MakeArgString("+" + FeatureName));
    else
      Features.push_back(Args.MakeArgString("-" + FeatureName));
  }
}

/// Get the (LLVM) name of the R600 gpu we are targeting.
static std::string getR600TargetGPU(const ArgList &Args) {
  if (Arg *A = Args.getLastArg(options::OPT_mcpu_EQ)) {
    const char *GPUName = A->getValue();
    return llvm::StringSwitch<const char *>(GPUName)
        .Cases("rv630", "rv635", "r600")
        .Cases("rv610", "rv620", "rs780", "rs880")
        .Case("rv740", "rv770")
        .Case("palm", "cedar")
        .Cases("sumo", "sumo2", "sumo")
        .Case("hemlock", "cypress")
        .Case("aruba", "cayman")
        .Default(GPUName);
  }
  return "";
}

static std::string getNios2TargetCPU(const ArgList &Args) {
  Arg *A = Args.getLastArg(options::OPT_mcpu_EQ);
  if (!A)
    A = Args.getLastArg(options::OPT_march_EQ);

  if (!A)
    return "";

  const char *name = A->getValue();
  return llvm::StringSwitch<const char *>(name)
      .Case("r1", "nios2r1")
      .Case("r2", "nios2r2")
      .Default(name);
}

static std::string getLanaiTargetCPU(const ArgList &Args) {
  if (Arg *A = Args.getLastArg(options::OPT_mcpu_EQ)) {
    return A->getValue();
  }
  return "";
}

/// Get the (LLVM) name of the WebAssembly cpu we are targeting.
static StringRef getWebAssemblyTargetCPU(const ArgList &Args) {
  // If we have -mcpu=, use that.
  if (Arg *A = Args.getLastArg(options::OPT_mcpu_EQ)) {
    StringRef CPU = A->getValue();

#ifdef __wasm__
    // Handle "native" by examining the host. "native" isn't meaningful when
    // cross compiling, so only support this when the host is also WebAssembly.
    if (CPU == "native")
      return llvm::sys::getHostCPUName();
#endif

    return CPU;
  }

  return "generic";
}

std::string tools::getCPUName(const ArgList &Args, const llvm::Triple &T,
                              bool FromAs) {
  Arg *A;

  switch (T.getArch()) {
  default:
    return "";

  case llvm::Triple::aarch64:
  case llvm::Triple::aarch64_be:
    return aarch64::getAArch64TargetCPU(Args, A);

  case llvm::Triple::arm:
  case llvm::Triple::armeb:
  case llvm::Triple::thumb:
  case llvm::Triple::thumbeb: {
    StringRef MArch, MCPU;
    arm::getARMArchCPUFromArgs(Args, MArch, MCPU, FromAs);
    return arm::getARMTargetCPU(MCPU, MArch, T);
  }

  case llvm::Triple::avr:
    if (const Arg *A = Args.getLastArg(options::OPT_mmcu_EQ))
      return A->getValue();
    return "";

  case llvm::Triple::nios2: {
    return getNios2TargetCPU(Args);
  }

  case llvm::Triple::mips:
  case llvm::Triple::mipsel:
  case llvm::Triple::mips64:
  case llvm::Triple::mips64el: {
    StringRef CPUName;
    StringRef ABIName;
    mips::getMipsCPUAndABI(Args, T, CPUName, ABIName);
    return CPUName;
  }

  case llvm::Triple::nvptx:
  case llvm::Triple::nvptx64:
    if (const Arg *A = Args.getLastArg(options::OPT_march_EQ))
      return A->getValue();
    return "";

  case llvm::Triple::ppc:
  case llvm::Triple::ppc64:
  case llvm::Triple::ppc64le: {
    std::string TargetCPUName = ppc::getPPCTargetCPU(Args);
    // LLVM may default to generating code for the native CPU,
    // but, like gcc, we default to a more generic option for
    // each architecture. (except on Darwin)
    if (TargetCPUName.empty() && !T.isOSDarwin()) {
      if (T.getArch() == llvm::Triple::ppc64)
        TargetCPUName = "ppc64";
      else if (T.getArch() == llvm::Triple::ppc64le)
        TargetCPUName = "ppc64le";
      else
        TargetCPUName = "ppc";
    }
    return TargetCPUName;
  }

  case llvm::Triple::sparc:
  case llvm::Triple::sparcel:
  case llvm::Triple::sparcv9:
    if (const Arg *A = Args.getLastArg(options::OPT_mcpu_EQ))
      return A->getValue();
    return "";

  case llvm::Triple::x86:
  case llvm::Triple::x86_64:
    return x86::getX86TargetCPU(Args, T);

  case llvm::Triple::hexagon:
    return "hexagon" +
           toolchains::HexagonToolChain::GetTargetCPUVersion(Args).str();

  case llvm::Triple::lanai:
    return getLanaiTargetCPU(Args);

  case llvm::Triple::systemz:
    return systemz::getSystemZTargetCPU(Args);

  case llvm::Triple::r600:
  case llvm::Triple::amdgcn:
    return getR600TargetGPU(Args);

  case llvm::Triple::wasm32:
  case llvm::Triple::wasm64:
    return getWebAssemblyTargetCPU(Args);
  }
}

unsigned tools::getLTOParallelism(const ArgList &Args, const Driver &D) {
  unsigned Parallelism = 0;
  Arg *LtoJobsArg = Args.getLastArg(options::OPT_flto_jobs_EQ);
  if (LtoJobsArg &&
      StringRef(LtoJobsArg->getValue()).getAsInteger(10, Parallelism))
    D.Diag(diag::err_drv_invalid_int_value) << LtoJobsArg->getAsString(Args)
                                            << LtoJobsArg->getValue();
  return Parallelism;
}

// CloudABI and WebAssembly use -ffunction-sections and -fdata-sections by
// default.
bool tools::isUseSeparateSections(const llvm::Triple &Triple) {
  return Triple.getOS() == llvm::Triple::CloudABI ||
         Triple.getArch() == llvm::Triple::wasm32 ||
         Triple.getArch() == llvm::Triple::wasm64;
}

void tools::AddGoldPlugin(const ToolChain &ToolChain, const ArgList &Args,
                          ArgStringList &CmdArgs, bool IsThinLTO,
                          const Driver &D) {
  // Tell the linker to load the plugin. This has to come before AddLinkerInputs
  // as gold requires -plugin to come before any -plugin-opt that -Wl might
  // forward.
  CmdArgs.push_back("-plugin");
  std::string Plugin =
      ToolChain.getDriver().Dir + "/../lib" CLANG_LIBDIR_SUFFIX "/LLVMgold.so";
  CmdArgs.push_back(Args.MakeArgString(Plugin));

  // Try to pass driver level flags relevant to LTO code generation down to
  // the plugin.

  // Handle flags for selecting CPU variants.
  std::string CPU = getCPUName(Args, ToolChain.getTriple());
  if (!CPU.empty())
    CmdArgs.push_back(Args.MakeArgString(Twine("-plugin-opt=mcpu=") + CPU));

  if (Arg *A = Args.getLastArg(options::OPT_O_Group)) {
    StringRef OOpt;
    if (A->getOption().matches(options::OPT_O4) ||
        A->getOption().matches(options::OPT_Ofast))
      OOpt = "3";
    else if (A->getOption().matches(options::OPT_O))
      OOpt = A->getValue();
    else if (A->getOption().matches(options::OPT_O0))
      OOpt = "0";
    if (!OOpt.empty())
      CmdArgs.push_back(Args.MakeArgString(Twine("-plugin-opt=O") + OOpt));
  }

  if (IsThinLTO)
    CmdArgs.push_back("-plugin-opt=thinlto");

  if (unsigned Parallelism = getLTOParallelism(Args, D))
    CmdArgs.push_back(Args.MakeArgString(Twine("-plugin-opt=jobs=") +
                                         llvm::to_string(Parallelism)));

  // If an explicit debugger tuning argument appeared, pass it along.
  if (Arg *A = Args.getLastArg(options::OPT_gTune_Group,
                               options::OPT_ggdbN_Group)) {
    if (A->getOption().matches(options::OPT_glldb))
      CmdArgs.push_back("-plugin-opt=-debugger-tune=lldb");
    else if (A->getOption().matches(options::OPT_gsce))
      CmdArgs.push_back("-plugin-opt=-debugger-tune=sce");
    else
      CmdArgs.push_back("-plugin-opt=-debugger-tune=gdb");
  }

  bool UseSeparateSections =
      isUseSeparateSections(ToolChain.getEffectiveTriple());

  if (Args.hasFlag(options::OPT_ffunction_sections,
                   options::OPT_fno_function_sections, UseSeparateSections)) {
    CmdArgs.push_back("-plugin-opt=-function-sections");
  }

  if (Args.hasFlag(options::OPT_fdata_sections, options::OPT_fno_data_sections,
                   UseSeparateSections)) {
    CmdArgs.push_back("-plugin-opt=-data-sections");
  }

  if (Arg *A = getLastProfileSampleUseArg(Args)) {
    StringRef FName = A->getValue();
    if (!llvm::sys::fs::exists(FName))
      D.Diag(diag::err_drv_no_such_file) << FName;
    else
      CmdArgs.push_back(
          Args.MakeArgString(Twine("-plugin-opt=sample-profile=") + FName));
  }
}

void tools::addArchSpecificRPath(const ToolChain &TC, const ArgList &Args,
                                 ArgStringList &CmdArgs) {
  std::string CandidateRPath = TC.getArchSpecificLibPath();
  if (TC.getVFS().exists(CandidateRPath)) {
    CmdArgs.push_back("-rpath");
    CmdArgs.push_back(Args.MakeArgString(CandidateRPath.c_str()));
  }
}

bool tools::addOpenMPRuntime(ArgStringList &CmdArgs, const ToolChain &TC,
                             const ArgList &Args, bool IsOffloadingHost,
                             bool GompNeedsRT) {
  if (!Args.hasFlag(options::OPT_fopenmp, options::OPT_fopenmp_EQ,
                    options::OPT_fno_openmp, false))
    return false;

  switch (TC.getDriver().getOpenMPRuntime(Args)) {
  case Driver::OMPRT_OMP:
    CmdArgs.push_back("-lomp");
    break;
  case Driver::OMPRT_GOMP:
    CmdArgs.push_back("-lgomp");

    if (GompNeedsRT)
      CmdArgs.push_back("-lrt");
    break;
  case Driver::OMPRT_IOMP5:
    CmdArgs.push_back("-liomp5");
    break;
  case Driver::OMPRT_Unknown:
    // Already diagnosed.
    return false;
  }

  if (IsOffloadingHost)
    CmdArgs.push_back("-lomptarget");

  addArchSpecificRPath(TC, Args, CmdArgs);

  return true;
}

static void addSanitizerRuntime(const ToolChain &TC, const ArgList &Args,
                                ArgStringList &CmdArgs, StringRef Sanitizer,
                                bool IsShared, bool IsWhole) {
  // Wrap any static runtimes that must be forced into executable in
  // whole-archive.
  if (IsWhole) CmdArgs.push_back("-whole-archive");
  CmdArgs.push_back(TC.getCompilerRTArgString(Args, Sanitizer, IsShared));
  if (IsWhole) CmdArgs.push_back("-no-whole-archive");

  if (IsShared) {
    addArchSpecificRPath(TC, Args, CmdArgs);
  }
}

// Tries to use a file with the list of dynamic symbols that need to be exported
// from the runtime library. Returns true if the file was found.
static bool addSanitizerDynamicList(const ToolChain &TC, const ArgList &Args,
                                    ArgStringList &CmdArgs,
                                    StringRef Sanitizer) {
  SmallString<128> SanRT(TC.getCompilerRT(Args, Sanitizer));
  if (llvm::sys::fs::exists(SanRT + ".syms")) {
    CmdArgs.push_back(Args.MakeArgString("--dynamic-list=" + SanRT + ".syms"));
    return true;
  }
  return false;
}

void tools::linkSanitizerRuntimeDeps(const ToolChain &TC,
                                     ArgStringList &CmdArgs) {
  // Force linking against the system libraries sanitizers depends on
  // (see PR15823 why this is necessary).
  CmdArgs.push_back("--no-as-needed");
  // There's no libpthread or librt on RTEMS.
  if (TC.getTriple().getOS() != llvm::Triple::RTEMS) {
    CmdArgs.push_back("-lpthread");
    CmdArgs.push_back("-lrt");
  }
  CmdArgs.push_back("-lm");
  // There's no libdl on FreeBSD or RTEMS.
  if (TC.getTriple().getOS() != llvm::Triple::FreeBSD &&
      TC.getTriple().getOS() != llvm::Triple::NetBSD &&
      TC.getTriple().getOS() != llvm::Triple::RTEMS)
    CmdArgs.push_back("-ldl");
}

static void
collectSanitizerRuntimes(const ToolChain &TC, const ArgList &Args,
                         SmallVectorImpl<StringRef> &SharedRuntimes,
                         SmallVectorImpl<StringRef> &StaticRuntimes,
                         SmallVectorImpl<StringRef> &NonWholeStaticRuntimes,
                         SmallVectorImpl<StringRef> &HelperStaticRuntimes,
                         SmallVectorImpl<StringRef> &RequiredSymbols) {
  const SanitizerArgs &SanArgs = TC.getSanitizerArgs();
  // Collect shared runtimes.
  if (SanArgs.needsAsanRt() && SanArgs.needsSharedAsanRt()) {
    SharedRuntimes.push_back("asan");
  }
  // The stats_client library is also statically linked into DSOs.
  if (SanArgs.needsStatsRt())
    StaticRuntimes.push_back("stats_client");

  // Collect static runtimes.
  if (Args.hasArg(options::OPT_shared) || TC.getTriple().isAndroid()) {
    // Don't link static runtimes into DSOs or if compiling for Android.
    return;
  }
  if (SanArgs.needsAsanRt()) {
    if (SanArgs.needsSharedAsanRt()) {
      HelperStaticRuntimes.push_back("asan-preinit");
    } else {
      StaticRuntimes.push_back("asan");
      if (SanArgs.linkCXXRuntimes())
        StaticRuntimes.push_back("asan_cxx");
    }
  }
  if (SanArgs.needsDfsanRt())
    StaticRuntimes.push_back("dfsan");
  if (SanArgs.needsLsanRt())
    StaticRuntimes.push_back("lsan");
  if (SanArgs.needsMsanRt()) {
    StaticRuntimes.push_back("msan");
    if (SanArgs.linkCXXRuntimes())
      StaticRuntimes.push_back("msan_cxx");
  }
  if (SanArgs.needsTsanRt()) {
    StaticRuntimes.push_back("tsan");
    if (SanArgs.linkCXXRuntimes())
      StaticRuntimes.push_back("tsan_cxx");
  }
  if (SanArgs.needsUbsanRt()) {
    StaticRuntimes.push_back("ubsan_standalone");
    if (SanArgs.linkCXXRuntimes())
      StaticRuntimes.push_back("ubsan_standalone_cxx");
  }
  if (SanArgs.needsSafeStackRt()) {
    NonWholeStaticRuntimes.push_back("safestack");
    RequiredSymbols.push_back("__safestack_init");
  }
  if (SanArgs.needsCfiRt())
    StaticRuntimes.push_back("cfi");
  if (SanArgs.needsCfiDiagRt()) {
    StaticRuntimes.push_back("cfi_diag");
    if (SanArgs.linkCXXRuntimes())
      StaticRuntimes.push_back("ubsan_standalone_cxx");
  }
  if (SanArgs.needsStatsRt()) {
    NonWholeStaticRuntimes.push_back("stats");
    RequiredSymbols.push_back("__sanitizer_stats_register");
  }
  if (SanArgs.needsEsanRt())
    StaticRuntimes.push_back("esan");
}

static void addLibFuzzerRuntime(const ToolChain &TC,
                                const ArgList &Args,
                                ArgStringList &CmdArgs) {
    StringRef ParentDir = llvm::sys::path::parent_path(TC.getDriver().InstalledDir);
    SmallString<128> P(ParentDir);
    llvm::sys::path::append(P, "lib", "libLLVMFuzzer.a");
    CmdArgs.push_back(Args.MakeArgString(P));
    TC.AddCXXStdlibLibArgs(Args, CmdArgs);
}


// Should be called before we add system libraries (C++ ABI, libstdc++/libc++,
// C runtime, etc). Returns true if sanitizer system deps need to be linked in.
bool tools::addSanitizerRuntimes(const ToolChain &TC, const ArgList &Args,
                                 ArgStringList &CmdArgs) {
  SmallVector<StringRef, 4> SharedRuntimes, StaticRuntimes,
      NonWholeStaticRuntimes, HelperStaticRuntimes, RequiredSymbols;
  collectSanitizerRuntimes(TC, Args, SharedRuntimes, StaticRuntimes,
                           NonWholeStaticRuntimes, HelperStaticRuntimes,
                           RequiredSymbols);
  // Inject libfuzzer dependencies.
  if (TC.getSanitizerArgs().needsFuzzer()
      && !Args.hasArg(options::OPT_shared)) {
    addLibFuzzerRuntime(TC, Args, CmdArgs);
  }

  for (auto RT : SharedRuntimes)
    addSanitizerRuntime(TC, Args, CmdArgs, RT, true, false);
  for (auto RT : HelperStaticRuntimes)
    addSanitizerRuntime(TC, Args, CmdArgs, RT, false, true);
  bool AddExportDynamic = false;
  for (auto RT : StaticRuntimes) {
    addSanitizerRuntime(TC, Args, CmdArgs, RT, false, true);
    AddExportDynamic |= !addSanitizerDynamicList(TC, Args, CmdArgs, RT);
  }
  for (auto RT : NonWholeStaticRuntimes) {
    addSanitizerRuntime(TC, Args, CmdArgs, RT, false, false);
    AddExportDynamic |= !addSanitizerDynamicList(TC, Args, CmdArgs, RT);
  }
  for (auto S : RequiredSymbols) {
    CmdArgs.push_back("-u");
    CmdArgs.push_back(Args.MakeArgString(S));
  }
  // If there is a static runtime with no dynamic list, force all the symbols
  // to be dynamic to be sure we export sanitizer interface functions.
  if (AddExportDynamic)
    CmdArgs.push_back("-export-dynamic");

  const SanitizerArgs &SanArgs = TC.getSanitizerArgs();
  if (SanArgs.hasCrossDsoCfi() && !AddExportDynamic)
    CmdArgs.push_back("-export-dynamic-symbol=__cfi_check");

  return !StaticRuntimes.empty() || !NonWholeStaticRuntimes.empty();
}

bool tools::areOptimizationsEnabled(const ArgList &Args) {
  // Find the last -O arg and see if it is non-zero.
  if (Arg *A = Args.getLastArg(options::OPT_O_Group))
    return !A->getOption().matches(options::OPT_O0);
  // Defaults to -O0.
  return false;
}

const char *tools::SplitDebugName(const ArgList &Args, const InputInfo &Input) {
  Arg *FinalOutput = Args.getLastArg(options::OPT_o);
  if (FinalOutput && Args.hasArg(options::OPT_c)) {
    SmallString<128> T(FinalOutput->getValue());
    llvm::sys::path::replace_extension(T, "dwo");
    return Args.MakeArgString(T);
  } else {
    // Use the compilation dir.
    SmallString<128> T(
        Args.getLastArgValue(options::OPT_fdebug_compilation_dir));
    SmallString<128> F(llvm::sys::path::stem(Input.getBaseInput()));
    llvm::sys::path::replace_extension(F, "dwo");
    T += F;
    return Args.MakeArgString(F);
  }
}

void tools::SplitDebugInfo(const ToolChain &TC, Compilation &C, const Tool &T,
                           const JobAction &JA, const ArgList &Args,
                           const InputInfo &Output, const char *OutFile) {
  ArgStringList ExtractArgs;
  ExtractArgs.push_back("--extract-dwo");

  ArgStringList StripArgs;
  StripArgs.push_back("--strip-dwo");

  // Grabbing the output of the earlier compile step.
  StripArgs.push_back(Output.getFilename());
  ExtractArgs.push_back(Output.getFilename());
  ExtractArgs.push_back(OutFile);

  const char *Exec = Args.MakeArgString(TC.GetProgramPath("objcopy"));
  InputInfo II(types::TY_Object, Output.getFilename(), Output.getFilename());

  // First extract the dwo sections.
  C.addCommand(llvm::make_unique<Command>(JA, T, Exec, ExtractArgs, II));

  // Then remove them from the original .o file.
  C.addCommand(llvm::make_unique<Command>(JA, T, Exec, StripArgs, II));
}

// Claim options we don't want to warn if they are unused. We do this for
// options that build systems might add but are unused when assembling or only
// running the preprocessor for example.
void tools::claimNoWarnArgs(const ArgList &Args) {
  // Don't warn about unused -f(no-)?lto.  This can happen when we're
  // preprocessing, precompiling or assembling.
  Args.ClaimAllArgs(options::OPT_flto_EQ);
  Args.ClaimAllArgs(options::OPT_flto);
  Args.ClaimAllArgs(options::OPT_fno_lto);
}

Arg *tools::getLastProfileUseArg(const ArgList &Args) {
  auto *ProfileUseArg = Args.getLastArg(
      options::OPT_fprofile_instr_use, options::OPT_fprofile_instr_use_EQ,
      options::OPT_fprofile_use, options::OPT_fprofile_use_EQ,
      options::OPT_fno_profile_instr_use);

  if (ProfileUseArg &&
      ProfileUseArg->getOption().matches(options::OPT_fno_profile_instr_use))
    ProfileUseArg = nullptr;

  return ProfileUseArg;
}

Arg *tools::getLastProfileSampleUseArg(const ArgList &Args) {
  auto *ProfileSampleUseArg = Args.getLastArg(
      options::OPT_fprofile_sample_use, options::OPT_fprofile_sample_use_EQ,
      options::OPT_fauto_profile, options::OPT_fauto_profile_EQ,
      options::OPT_fno_profile_sample_use, options::OPT_fno_auto_profile);

  if (ProfileSampleUseArg &&
      (ProfileSampleUseArg->getOption().matches(
           options::OPT_fno_profile_sample_use) ||
       ProfileSampleUseArg->getOption().matches(options::OPT_fno_auto_profile)))
    return nullptr;

  return Args.getLastArg(options::OPT_fprofile_sample_use_EQ,
                         options::OPT_fauto_profile_EQ);
}

/// Parses the various -fpic/-fPIC/-fpie/-fPIE arguments.  Then,
/// smooshes them together with platform defaults, to decide whether
/// this compile should be using PIC mode or not. Returns a tuple of
/// (RelocationModel, PICLevel, IsPIE).
std::tuple<llvm::Reloc::Model, unsigned, bool>
tools::ParsePICArgs(const ToolChain &ToolChain, const ArgList &Args) {
  const llvm::Triple &EffectiveTriple = ToolChain.getEffectiveTriple();
  const llvm::Triple &Triple = ToolChain.getTriple();

  bool PIE = ToolChain.isPIEDefault();
  bool PIC = PIE || ToolChain.isPICDefault();
  // The Darwin/MachO default to use PIC does not apply when using -static.
  if (Triple.isOSBinFormatMachO() && Args.hasArg(options::OPT_static))
    PIE = PIC = false;
  bool IsPICLevelTwo = PIC;

  bool KernelOrKext =
      Args.hasArg(options::OPT_mkernel, options::OPT_fapple_kext);

  // Android-specific defaults for PIC/PIE
  if (Triple.isAndroid()) {
    switch (Triple.getArch()) {
    case llvm::Triple::arm:
    case llvm::Triple::armeb:
    case llvm::Triple::thumb:
    case llvm::Triple::thumbeb:
    case llvm::Triple::aarch64:
    case llvm::Triple::mips:
    case llvm::Triple::mipsel:
    case llvm::Triple::mips64:
    case llvm::Triple::mips64el:
      PIC = true; // "-fpic"
      break;

    case llvm::Triple::x86:
    case llvm::Triple::x86_64:
      PIC = true; // "-fPIC"
      IsPICLevelTwo = true;
      break;

    default:
      break;
    }
  }

  // OpenBSD-specific defaults for PIE
  if (Triple.getOS() == llvm::Triple::OpenBSD) {
    switch (ToolChain.getArch()) {
    case llvm::Triple::arm:
    case llvm::Triple::aarch64:
    case llvm::Triple::mips64:
    case llvm::Triple::mips64el:
    case llvm::Triple::x86:
    case llvm::Triple::x86_64:
      IsPICLevelTwo = false; // "-fpie"
      break;

    case llvm::Triple::ppc:
    case llvm::Triple::sparc:
    case llvm::Triple::sparcel:
    case llvm::Triple::sparcv9:
      IsPICLevelTwo = true; // "-fPIE"
      break;

    default:
      break;
    }
  }

  // The last argument relating to either PIC or PIE wins, and no
  // other argument is used. If the last argument is any flavor of the
  // '-fno-...' arguments, both PIC and PIE are disabled. Any PIE
  // option implicitly enables PIC at the same level.
  Arg *LastPICArg = Args.getLastArg(options::OPT_fPIC, options::OPT_fno_PIC,
                                    options::OPT_fpic, options::OPT_fno_pic,
                                    options::OPT_fPIE, options::OPT_fno_PIE,
                                    options::OPT_fpie, options::OPT_fno_pie);
  if (Triple.isOSWindows() && LastPICArg &&
      LastPICArg ==
          Args.getLastArg(options::OPT_fPIC, options::OPT_fpic,
                          options::OPT_fPIE, options::OPT_fpie)) {
    ToolChain.getDriver().Diag(diag::err_drv_unsupported_opt_for_target)
        << LastPICArg->getSpelling() << Triple.str();
    if (Triple.getArch() == llvm::Triple::x86_64)
      return std::make_tuple(llvm::Reloc::PIC_, 2U, false);
    return std::make_tuple(llvm::Reloc::Static, 0U, false);
  }

  // Check whether the tool chain trumps the PIC-ness decision. If the PIC-ness
  // is forced, then neither PIC nor PIE flags will have no effect.
  if (!ToolChain.isPICDefaultForced()) {
    if (LastPICArg) {
      Option O = LastPICArg->getOption();
      if (O.matches(options::OPT_fPIC) || O.matches(options::OPT_fpic) ||
          O.matches(options::OPT_fPIE) || O.matches(options::OPT_fpie)) {
        PIE = O.matches(options::OPT_fPIE) || O.matches(options::OPT_fpie);
        PIC =
            PIE || O.matches(options::OPT_fPIC) || O.matches(options::OPT_fpic);
        IsPICLevelTwo =
            O.matches(options::OPT_fPIE) || O.matches(options::OPT_fPIC);
      } else {
        PIE = PIC = false;
        if (EffectiveTriple.isPS4CPU()) {
          Arg *ModelArg = Args.getLastArg(options::OPT_mcmodel_EQ);
          StringRef Model = ModelArg ? ModelArg->getValue() : "";
          if (Model != "kernel") {
            PIC = true;
            ToolChain.getDriver().Diag(diag::warn_drv_ps4_force_pic)
                << LastPICArg->getSpelling();
          }
        }
      }
    }
  }

  // Introduce a Darwin and PS4-specific hack. If the default is PIC, but the
  // PIC level would've been set to level 1, force it back to level 2 PIC
  // instead.
  if (PIC && (Triple.isOSDarwin() || EffectiveTriple.isPS4CPU()))
    IsPICLevelTwo |= ToolChain.isPICDefault();

  // This kernel flags are a trump-card: they will disable PIC/PIE
  // generation, independent of the argument order.
  if (KernelOrKext &&
      ((!EffectiveTriple.isiOS() || EffectiveTriple.isOSVersionLT(6)) &&
       !EffectiveTriple.isWatchOS()))
    PIC = PIE = false;

  if (Arg *A = Args.getLastArg(options::OPT_mdynamic_no_pic)) {
    // This is a very special mode. It trumps the other modes, almost no one
    // uses it, and it isn't even valid on any OS but Darwin.
    if (!Triple.isOSDarwin())
      ToolChain.getDriver().Diag(diag::err_drv_unsupported_opt_for_target)
          << A->getSpelling() << Triple.str();

    // FIXME: Warn when this flag trumps some other PIC or PIE flag.

    // Only a forced PIC mode can cause the actual compile to have PIC defines
    // etc., no flags are sufficient. This behavior was selected to closely
    // match that of llvm-gcc and Apple GCC before that.
    PIC = ToolChain.isPICDefault() && ToolChain.isPICDefaultForced();

    return std::make_tuple(llvm::Reloc::DynamicNoPIC, PIC ? 2U : 0U, false);
  }

  bool EmbeddedPISupported;
  switch (Triple.getArch()) {
    case llvm::Triple::arm:
    case llvm::Triple::armeb:
    case llvm::Triple::thumb:
    case llvm::Triple::thumbeb:
      EmbeddedPISupported = true;
      break;
    default:
      EmbeddedPISupported = false;
      break;
  }

  bool ROPI = false, RWPI = false;
  Arg* LastROPIArg = Args.getLastArg(options::OPT_fropi, options::OPT_fno_ropi);
  if (LastROPIArg && LastROPIArg->getOption().matches(options::OPT_fropi)) {
    if (!EmbeddedPISupported)
      ToolChain.getDriver().Diag(diag::err_drv_unsupported_opt_for_target)
          << LastROPIArg->getSpelling() << Triple.str();
    ROPI = true;
  }
  Arg *LastRWPIArg = Args.getLastArg(options::OPT_frwpi, options::OPT_fno_rwpi);
  if (LastRWPIArg && LastRWPIArg->getOption().matches(options::OPT_frwpi)) {
    if (!EmbeddedPISupported)
      ToolChain.getDriver().Diag(diag::err_drv_unsupported_opt_for_target)
          << LastRWPIArg->getSpelling() << Triple.str();
    RWPI = true;
  }

  // ROPI and RWPI are not comaptible with PIC or PIE.
  if ((ROPI || RWPI) && (PIC || PIE))
    ToolChain.getDriver().Diag(diag::err_drv_ropi_rwpi_incompatible_with_pic);

  // When targettng MIPS64 with N64, the default is PIC, unless -mno-abicalls is
  // used.
  if ((Triple.getArch() == llvm::Triple::mips64 ||
       Triple.getArch() == llvm::Triple::mips64el) &&
      Args.hasArg(options::OPT_mno_abicalls))
    return std::make_tuple(llvm::Reloc::Static, 0U, false);

  if (PIC)
    return std::make_tuple(llvm::Reloc::PIC_, IsPICLevelTwo ? 2U : 1U, PIE);

  llvm::Reloc::Model RelocM = llvm::Reloc::Static;
  if (ROPI && RWPI)
    RelocM = llvm::Reloc::ROPI_RWPI;
  else if (ROPI)
    RelocM = llvm::Reloc::ROPI;
  else if (RWPI)
    RelocM = llvm::Reloc::RWPI;

  return std::make_tuple(RelocM, 0U, false);
}

void tools::AddAssemblerKPIC(const ToolChain &ToolChain, const ArgList &Args,
                             ArgStringList &CmdArgs) {
  llvm::Reloc::Model RelocationModel;
  unsigned PICLevel;
  bool IsPIE;
  std::tie(RelocationModel, PICLevel, IsPIE) = ParsePICArgs(ToolChain, Args);

  if (RelocationModel != llvm::Reloc::Static)
    CmdArgs.push_back("-KPIC");
}

/// \brief Determine whether Objective-C automated reference counting is
/// enabled.
bool tools::isObjCAutoRefCount(const ArgList &Args) {
  return Args.hasFlag(options::OPT_fobjc_arc, options::OPT_fno_objc_arc, false);
}

static void AddLibgcc(const llvm::Triple &Triple, const Driver &D,
                      ArgStringList &CmdArgs, const ArgList &Args) {
  bool isAndroid = Triple.isAndroid();
  bool isCygMing = Triple.isOSCygMing();
  bool IsIAMCU = Triple.isOSIAMCU();
  bool StaticLibgcc = Args.hasArg(options::OPT_static_libgcc) ||
                      Args.hasArg(options::OPT_static);
  if (!D.CCCIsCXX())
    CmdArgs.push_back("-lgcc");

  if (StaticLibgcc || isAndroid) {
    if (D.CCCIsCXX())
      CmdArgs.push_back("-lgcc");
  } else {
    if (!D.CCCIsCXX() && !isCygMing)
      CmdArgs.push_back("--as-needed");
    CmdArgs.push_back("-lgcc_s");
    if (!D.CCCIsCXX() && !isCygMing)
      CmdArgs.push_back("--no-as-needed");
  }

  if (StaticLibgcc && !isAndroid && !IsIAMCU)
    CmdArgs.push_back("-lgcc_eh");
  else if (!Args.hasArg(options::OPT_shared) && D.CCCIsCXX())
    CmdArgs.push_back("-lgcc");

  // According to Android ABI, we have to link with libdl if we are
  // linking with non-static libgcc.
  //
  // NOTE: This fixes a link error on Android MIPS as well.  The non-static
  // libgcc for MIPS relies on _Unwind_Find_FDE and dl_iterate_phdr from libdl.
  if (isAndroid && !StaticLibgcc)
    CmdArgs.push_back("-ldl");
}

void tools::AddRunTimeLibs(const ToolChain &TC, const Driver &D,
                           ArgStringList &CmdArgs, const ArgList &Args) {
  // Make use of compiler-rt if --rtlib option is used
  ToolChain::RuntimeLibType RLT = TC.GetRuntimeLibType(Args);

  switch (RLT) {
  case ToolChain::RLT_CompilerRT:
    switch (TC.getTriple().getOS()) {
    default:
      llvm_unreachable("unsupported OS");
    case llvm::Triple::Win32:
    case llvm::Triple::Linux:
    case llvm::Triple::Fuchsia:
      CmdArgs.push_back(TC.getCompilerRTArgString(Args, "builtins"));
      break;
    }
    break;
  case ToolChain::RLT_Libgcc:
    // Make sure libgcc is not used under MSVC environment by default
    if (TC.getTriple().isKnownWindowsMSVCEnvironment()) {
      // Issue error diagnostic if libgcc is explicitly specified
      // through command line as --rtlib option argument.
      if (Args.hasArg(options::OPT_rtlib_EQ)) {
        TC.getDriver().Diag(diag::err_drv_unsupported_rtlib_for_platform)
            << Args.getLastArg(options::OPT_rtlib_EQ)->getValue() << "MSVC";
      }
    } else
      AddLibgcc(TC.getTriple(), D, CmdArgs, Args);
    break;
  }
}
