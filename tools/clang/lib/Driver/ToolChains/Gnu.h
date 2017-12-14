//===--- Gnu.h - Gnu Tool and ToolChain Implementations ---------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_LIB_DRIVER_TOOLCHAINS_GNU_H
#define LLVM_CLANG_LIB_DRIVER_TOOLCHAINS_GNU_H

#include "Cuda.h"
#include "clang/Driver/Tool.h"
#include "clang/Driver/ToolChain.h"
#include <set>

namespace clang {
namespace driver {

struct DetectedMultilibs {
  /// The set of multilibs that the detected installation supports.
  MultilibSet Multilibs;

  /// The primary multilib appropriate for the given flags.
  Multilib SelectedMultilib;

  /// On Biarch systems, this corresponds to the default multilib when
  /// targeting the non-default multilib. Otherwise, it is empty.
  llvm::Optional<Multilib> BiarchSibling;
};

bool findMIPSMultilibs(const Driver &D, const llvm::Triple &TargetTriple,
                       StringRef Path, const llvm::opt::ArgList &Args,
                       DetectedMultilibs &Result);

namespace tools {

/// \brief Base class for all GNU tools that provide the same behavior when
/// it comes to response files support
class LLVM_LIBRARY_VISIBILITY GnuTool : public Tool {
  virtual void anchor();

public:
  GnuTool(const char *Name, const char *ShortName, const ToolChain &TC)
      : Tool(Name, ShortName, TC, RF_Full, llvm::sys::WEM_CurrentCodePage) {}
};

/// Directly call GNU Binutils' assembler and linker.
namespace gnutools {
class LLVM_LIBRARY_VISIBILITY Assembler : public GnuTool {
public:
  Assembler(const ToolChain &TC) : GnuTool("GNU::Assembler", "assembler", TC) {}

  bool hasIntegratedCPP() const override { return false; }

  void ConstructJob(Compilation &C, const JobAction &JA,
                    const InputInfo &Output, const InputInfoList &Inputs,
                    const llvm::opt::ArgList &TCArgs,
                    const char *LinkingOutput) const override;
};

class LLVM_LIBRARY_VISIBILITY Linker : public GnuTool {
public:
  Linker(const ToolChain &TC) : GnuTool("GNU::Linker", "linker", TC) {}

  bool hasIntegratedCPP() const override { return false; }
  bool isLinkJob() const override { return true; }

  void ConstructJob(Compilation &C, const JobAction &JA,
                    const InputInfo &Output, const InputInfoList &Inputs,
                    const llvm::opt::ArgList &TCArgs,
                    const char *LinkingOutput) const override;
};
} // end namespace gnutools

/// gcc - Generic GCC tool implementations.
namespace gcc {
class LLVM_LIBRARY_VISIBILITY Common : public GnuTool {
public:
  Common(const char *Name, const char *ShortName, const ToolChain &TC)
      : GnuTool(Name, ShortName, TC) {}

  // A gcc tool has an "integrated" assembler that it will call to produce an
  // object. Let it use that assembler so that we don't have to deal with
  // assembly syntax incompatibilities.
  bool hasIntegratedAssembler() const override { return true; }
  void ConstructJob(Compilation &C, const JobAction &JA,
                    const InputInfo &Output, const InputInfoList &Inputs,
                    const llvm::opt::ArgList &TCArgs,
                    const char *LinkingOutput) const override;

  /// RenderExtraToolArgs - Render any arguments necessary to force
  /// the particular tool mode.
  virtual void RenderExtraToolArgs(const JobAction &JA,
                                   llvm::opt::ArgStringList &CmdArgs) const = 0;
};

class LLVM_LIBRARY_VISIBILITY Preprocessor : public Common {
public:
  Preprocessor(const ToolChain &TC)
      : Common("gcc::Preprocessor", "gcc preprocessor", TC) {}

  bool hasGoodDiagnostics() const override { return true; }
  bool hasIntegratedCPP() const override { return false; }

  void RenderExtraToolArgs(const JobAction &JA,
                           llvm::opt::ArgStringList &CmdArgs) const override;
};

class LLVM_LIBRARY_VISIBILITY Compiler : public Common {
public:
  Compiler(const ToolChain &TC) : Common("gcc::Compiler", "gcc frontend", TC) {}

  bool hasGoodDiagnostics() const override { return true; }
  bool hasIntegratedCPP() const override { return true; }

  void RenderExtraToolArgs(const JobAction &JA,
                           llvm::opt::ArgStringList &CmdArgs) const override;
};

class LLVM_LIBRARY_VISIBILITY Linker : public Common {
public:
  Linker(const ToolChain &TC) : Common("gcc::Linker", "linker (via gcc)", TC) {}

  bool hasIntegratedCPP() const override { return false; }
  bool isLinkJob() const override { return true; }

  void RenderExtraToolArgs(const JobAction &JA,
                           llvm::opt::ArgStringList &CmdArgs) const override;
};
} // end namespace gcc
} // end namespace tools

namespace toolchains {

/// Generic_GCC - A tool chain using the 'gcc' command to perform
/// all subcommands; this relies on gcc translating the majority of
/// command line options.
class LLVM_LIBRARY_VISIBILITY Generic_GCC : public ToolChain {
public:
  /// \brief Struct to store and manipulate GCC versions.
  ///
  /// We rely on assumptions about the form and structure of GCC version
  /// numbers: they consist of at most three '.'-separated components, and each
  /// component is a non-negative integer except for the last component. For
  /// the last component we are very flexible in order to tolerate release
  /// candidates or 'x' wildcards.
  ///
  /// Note that the ordering established among GCCVersions is based on the
  /// preferred version string to use. For example we prefer versions without
  /// a hard-coded patch number to those with a hard coded patch number.
  ///
  /// Currently this doesn't provide any logic for textual suffixes to patches
  /// in the way that (for example) Debian's version format does. If that ever
  /// becomes necessary, it can be added.
  struct GCCVersion {
    /// \brief The unparsed text of the version.
    std::string Text;

    /// \brief The parsed major, minor, and patch numbers.
    int Major, Minor, Patch;

    /// \brief The text of the parsed major, and major+minor versions.
    std::string MajorStr, MinorStr;

    /// \brief Any textual suffix on the patch number.
    std::string PatchSuffix;

    static GCCVersion Parse(StringRef VersionText);
    bool isOlderThan(int RHSMajor, int RHSMinor, int RHSPatch,
                     StringRef RHSPatchSuffix = StringRef()) const;
    bool operator<(const GCCVersion &RHS) const {
      return isOlderThan(RHS.Major, RHS.Minor, RHS.Patch, RHS.PatchSuffix);
    }
    bool operator>(const GCCVersion &RHS) const { return RHS < *this; }
    bool operator<=(const GCCVersion &RHS) const { return !(*this > RHS); }
    bool operator>=(const GCCVersion &RHS) const { return !(*this < RHS); }
  };

  /// \brief This is a class to find a viable GCC installation for Clang to
  /// use.
  ///
  /// This class tries to find a GCC installation on the system, and report
  /// information about it. It starts from the host information provided to the
  /// Driver, and has logic for fuzzing that where appropriate.
  class GCCInstallationDetector {
    bool IsValid;
    llvm::Triple GCCTriple;
    const Driver &D;

    // FIXME: These might be better as path objects.
    std::string GCCInstallPath;
    std::string GCCParentLibPath;

    /// The primary multilib appropriate for the given flags.
    Multilib SelectedMultilib;
    /// On Biarch systems, this corresponds to the default multilib when
    /// targeting the non-default multilib. Otherwise, it is empty.
    llvm::Optional<Multilib> BiarchSibling;

    GCCVersion Version;

    // We retain the list of install paths that were considered and rejected in
    // order to print out detailed information in verbose mode.
    std::set<std::string> CandidateGCCInstallPaths;

    /// The set of multilibs that the detected installation supports.
    MultilibSet Multilibs;

  public:
    explicit GCCInstallationDetector(const Driver &D) : IsValid(false), D(D) {}
    void init(const llvm::Triple &TargetTriple, const llvm::opt::ArgList &Args,
              ArrayRef<std::string> ExtraTripleAliases = None);

    /// \brief Check whether we detected a valid GCC install.
    bool isValid() const { return IsValid; }

    /// \brief Get the GCC triple for the detected install.
    const llvm::Triple &getTriple() const { return GCCTriple; }

    /// \brief Get the detected GCC installation path.
    StringRef getInstallPath() const { return GCCInstallPath; }

    /// \brief Get the detected GCC parent lib path.
    StringRef getParentLibPath() const { return GCCParentLibPath; }

    /// \brief Get the detected Multilib
    const Multilib &getMultilib() const { return SelectedMultilib; }

    /// \brief Get the whole MultilibSet
    const MultilibSet &getMultilibs() const { return Multilibs; }

    /// Get the biarch sibling multilib (if it exists).
    /// \return true iff such a sibling exists
    bool getBiarchSibling(Multilib &M) const;

    /// \brief Get the detected GCC version string.
    const GCCVersion &getVersion() const { return Version; }

    /// \brief Print information about the detected GCC installation.
    void print(raw_ostream &OS) const;

  private:
    static void
    CollectLibDirsAndTriples(const llvm::Triple &TargetTriple,
                             const llvm::Triple &BiarchTriple,
                             SmallVectorImpl<StringRef> &LibDirs,
                             SmallVectorImpl<StringRef> &TripleAliases,
                             SmallVectorImpl<StringRef> &BiarchLibDirs,
                             SmallVectorImpl<StringRef> &BiarchTripleAliases);

    bool ScanGCCForMultilibs(const llvm::Triple &TargetTriple,
                             const llvm::opt::ArgList &Args,
                             StringRef Path,
                             bool NeedsBiarchSuffix = false);

    void ScanLibDirForGCCTriple(const llvm::Triple &TargetArch,
                                const llvm::opt::ArgList &Args,
                                const std::string &LibDir,
                                StringRef CandidateTriple,
                                bool NeedsBiarchSuffix = false);

    void scanLibDirForGCCTripleSolaris(const llvm::Triple &TargetArch,
                                       const llvm::opt::ArgList &Args,
                                       const std::string &LibDir,
                                       StringRef CandidateTriple,
                                       bool NeedsBiarchSuffix = false);

    bool ScanGentooGccConfig(const llvm::Triple &TargetTriple,
                             const llvm::opt::ArgList &Args,
                             StringRef CandidateTriple,
                             bool NeedsBiarchSuffix = false);
  };

protected:
  GCCInstallationDetector GCCInstallation;
  CudaInstallationDetector CudaInstallation;

public:
  Generic_GCC(const Driver &D, const llvm::Triple &Triple,
              const llvm::opt::ArgList &Args);
  ~Generic_GCC() override;

  void printVerboseInfo(raw_ostream &OS) const override;

  bool IsUnwindTablesDefault(const llvm::opt::ArgList &Args) const override;
  bool isPICDefault() const override;
  bool isPIEDefault() const override;
  bool isPICDefaultForced() const override;
  bool IsIntegratedAssemblerDefault() const override;
  llvm::opt::DerivedArgList *
  TranslateArgs(const llvm::opt::DerivedArgList &Args, StringRef BoundArch,
                Action::OffloadKind DeviceOffloadKind) const override;

protected:
  Tool *getTool(Action::ActionClass AC) const override;
  Tool *buildAssembler() const override;
  Tool *buildLinker() const override;

  /// \name ToolChain Implementation Helper Functions
  /// @{

  /// \brief Check whether the target triple's architecture is 64-bits.
  bool isTarget64Bit() const { return getTriple().isArch64Bit(); }

  /// \brief Check whether the target triple's architecture is 32-bits.
  bool isTarget32Bit() const { return getTriple().isArch32Bit(); }

  // FIXME: This should be final, but the Solaris tool chain does weird
  // things we can't easily represent.
  void AddClangCXXStdlibIncludeArgs(
      const llvm::opt::ArgList &DriverArgs,
      llvm::opt::ArgStringList &CC1Args) const override;

  virtual std::string findLibCxxIncludePath() const;
  virtual void
  addLibStdCxxIncludePaths(const llvm::opt::ArgList &DriverArgs,
                           llvm::opt::ArgStringList &CC1Args) const;

  bool addLibStdCXXIncludePaths(Twine Base, Twine Suffix, StringRef GCCTriple,
                                StringRef GCCMultiarchTriple,
                                StringRef TargetMultiarchTriple,
                                Twine IncludeSuffix,
                                const llvm::opt::ArgList &DriverArgs,
                                llvm::opt::ArgStringList &CC1Args) const;

  /// @}

private:
  mutable std::unique_ptr<tools::gcc::Preprocessor> Preprocess;
  mutable std::unique_ptr<tools::gcc::Compiler> Compile;
};

class LLVM_LIBRARY_VISIBILITY Generic_ELF : public Generic_GCC {
  virtual void anchor();

public:
  Generic_ELF(const Driver &D, const llvm::Triple &Triple,
              const llvm::opt::ArgList &Args)
      : Generic_GCC(D, Triple, Args) {}

  void addClangTargetOptions(const llvm::opt::ArgList &DriverArgs,
                             llvm::opt::ArgStringList &CC1Args,
                             Action::OffloadKind DeviceOffloadKind) const override;
};

} // end namespace toolchains
} // end namespace driver
} // end namespace clang

#endif // LLVM_CLANG_LIB_DRIVER_TOOLCHAINS_GNU_H
