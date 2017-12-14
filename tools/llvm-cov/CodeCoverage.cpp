//===- CodeCoverage.cpp - Coverage tool based on profiling instrumentation-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// The 'CodeCoverageTool' class implements a command line tool to analyze and
// report coverage information using the profiling instrumentation and code
// coverage mapping.
//
//===----------------------------------------------------------------------===//

#include "CoverageFilters.h"
#include "CoverageReport.h"
#include "CoverageSummaryInfo.h"
#include "CoverageViewOptions.h"
#include "RenderingSupport.h"
#include "SourceCoverageView.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/Triple.h"
#include "llvm/ProfileData/Coverage/CoverageMapping.h"
#include "llvm/ProfileData/InstrProfReader.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Format.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/Process.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/ScopedPrinter.h"
#include "llvm/Support/Threading.h"
#include "llvm/Support/ThreadPool.h"
#include "llvm/Support/ToolOutputFile.h"
#include <functional>
#include <system_error>

using namespace llvm;
using namespace coverage;

void exportCoverageDataToJson(const coverage::CoverageMapping &CoverageMapping,
                              raw_ostream &OS);

namespace {
/// \brief The implementation of the coverage tool.
class CodeCoverageTool {
public:
  enum Command {
    /// \brief The show command.
    Show,
    /// \brief The report command.
    Report,
    /// \brief The export command.
    Export
  };

  int run(Command Cmd, int argc, const char **argv);

private:
  /// \brief Print the error message to the error output stream.
  void error(const Twine &Message, StringRef Whence = "");

  /// \brief Print the warning message to the error output stream.
  void warning(const Twine &Message, StringRef Whence = "");

  /// \brief Convert \p Path into an absolute path and append it to the list
  /// of collected paths.
  void addCollectedPath(const std::string &Path);

  /// \brief If \p Path is a regular file, collect the path. If it's a
  /// directory, recursively collect all of the paths within the directory.
  void collectPaths(const std::string &Path);

  /// \brief Return a memory buffer for the given source file.
  ErrorOr<const MemoryBuffer &> getSourceFile(StringRef SourceFile);

  /// \brief Create source views for the expansions of the view.
  void attachExpansionSubViews(SourceCoverageView &View,
                               ArrayRef<ExpansionRecord> Expansions,
                               const CoverageMapping &Coverage);

  /// \brief Create the source view of a particular function.
  std::unique_ptr<SourceCoverageView>
  createFunctionView(const FunctionRecord &Function,
                     const CoverageMapping &Coverage);

  /// \brief Create the main source view of a particular source file.
  std::unique_ptr<SourceCoverageView>
  createSourceFileView(StringRef SourceFile, const CoverageMapping &Coverage);

  /// \brief Load the coverage mapping data. Return nullptr if an error occurred.
  std::unique_ptr<CoverageMapping> load();

  /// \brief Remove input source files which aren't mapped by \p Coverage.
  void removeUnmappedInputs(const CoverageMapping &Coverage);

  /// \brief If a demangler is available, demangle all symbol names.
  void demangleSymbols(const CoverageMapping &Coverage);

  /// \brief Write out a source file view to the filesystem.
  void writeSourceFileView(StringRef SourceFile, CoverageMapping *Coverage,
                           CoveragePrinter *Printer, bool ShowFilenames);

  typedef llvm::function_ref<int(int, const char **)> CommandLineParserType;

  int show(int argc, const char **argv,
           CommandLineParserType commandLineParser);

  int report(int argc, const char **argv,
             CommandLineParserType commandLineParser);

  int export_(int argc, const char **argv,
              CommandLineParserType commandLineParser);

  std::vector<StringRef> ObjectFilenames;
  CoverageViewOptions ViewOpts;
  CoverageFiltersMatchAll Filters;

  /// The path to the indexed profile.
  std::string PGOFilename;

  /// A list of input source files.
  std::vector<std::string> SourceFiles;

  /// Whether or not we're in -filename-equivalence mode.
  bool CompareFilenamesOnly;

  /// In -filename-equivalence mode, this maps absolute paths from the
  /// coverage mapping data to input source files.
  StringMap<std::string> RemappedFilenames;

  /// The architecture the coverage mapping data targets.
  std::string CoverageArch;

  /// A cache for demangled symbols.
  DemangleCache DC;

  /// A lock which guards printing to stderr.
  std::mutex ErrsLock;

  /// A container for input source file buffers.
  std::mutex LoadedSourceFilesLock;
  std::vector<std::pair<std::string, std::unique_ptr<MemoryBuffer>>>
      LoadedSourceFiles;
};
}

static std::string getErrorString(const Twine &Message, StringRef Whence,
                                  bool Warning) {
  std::string Str = (Warning ? "warning" : "error");
  Str += ": ";
  if (!Whence.empty())
    Str += Whence.str() + ": ";
  Str += Message.str() + "\n";
  return Str;
}

void CodeCoverageTool::error(const Twine &Message, StringRef Whence) {
  std::unique_lock<std::mutex> Guard{ErrsLock};
  ViewOpts.colored_ostream(errs(), raw_ostream::RED)
      << getErrorString(Message, Whence, false);
}

void CodeCoverageTool::warning(const Twine &Message, StringRef Whence) {
  std::unique_lock<std::mutex> Guard{ErrsLock};
  ViewOpts.colored_ostream(errs(), raw_ostream::RED)
      << getErrorString(Message, Whence, true);
}

void CodeCoverageTool::addCollectedPath(const std::string &Path) {
  if (CompareFilenamesOnly) {
    SourceFiles.emplace_back(Path);
  } else {
    SmallString<128> EffectivePath(Path);
    if (std::error_code EC = sys::fs::make_absolute(EffectivePath)) {
      error(EC.message(), Path);
      return;
    }
    sys::path::remove_dots(EffectivePath, /*remove_dot_dots=*/true);
    SourceFiles.emplace_back(EffectivePath.str());
  }
}

void CodeCoverageTool::collectPaths(const std::string &Path) {
  llvm::sys::fs::file_status Status;
  llvm::sys::fs::status(Path, Status);
  if (!llvm::sys::fs::exists(Status)) {
    if (CompareFilenamesOnly)
      addCollectedPath(Path);
    else
      error("Missing source file", Path);
    return;
  }

  if (llvm::sys::fs::is_regular_file(Status)) {
    addCollectedPath(Path);
    return;
  }

  if (llvm::sys::fs::is_directory(Status)) {
    std::error_code EC;
    for (llvm::sys::fs::recursive_directory_iterator F(Path, EC), E;
         F != E && !EC; F.increment(EC)) {
      if (llvm::sys::fs::is_regular_file(F->path()))
        addCollectedPath(F->path());
    }
    if (EC)
      warning(EC.message(), Path);
  }
}

ErrorOr<const MemoryBuffer &>
CodeCoverageTool::getSourceFile(StringRef SourceFile) {
  // If we've remapped filenames, look up the real location for this file.
  std::unique_lock<std::mutex> Guard{LoadedSourceFilesLock};
  if (!RemappedFilenames.empty()) {
    auto Loc = RemappedFilenames.find(SourceFile);
    if (Loc != RemappedFilenames.end())
      SourceFile = Loc->second;
  }
  for (const auto &Files : LoadedSourceFiles)
    if (sys::fs::equivalent(SourceFile, Files.first))
      return *Files.second;
  auto Buffer = MemoryBuffer::getFile(SourceFile);
  if (auto EC = Buffer.getError()) {
    error(EC.message(), SourceFile);
    return EC;
  }
  LoadedSourceFiles.emplace_back(SourceFile, std::move(Buffer.get()));
  return *LoadedSourceFiles.back().second;
}

void CodeCoverageTool::attachExpansionSubViews(
    SourceCoverageView &View, ArrayRef<ExpansionRecord> Expansions,
    const CoverageMapping &Coverage) {
  if (!ViewOpts.ShowExpandedRegions)
    return;
  for (const auto &Expansion : Expansions) {
    auto ExpansionCoverage = Coverage.getCoverageForExpansion(Expansion);
    if (ExpansionCoverage.empty())
      continue;
    auto SourceBuffer = getSourceFile(ExpansionCoverage.getFilename());
    if (!SourceBuffer)
      continue;

    auto SubViewExpansions = ExpansionCoverage.getExpansions();
    auto SubView =
        SourceCoverageView::create(Expansion.Function.Name, SourceBuffer.get(),
                                   ViewOpts, std::move(ExpansionCoverage));
    attachExpansionSubViews(*SubView, SubViewExpansions, Coverage);
    View.addExpansion(Expansion.Region, std::move(SubView));
  }
}

std::unique_ptr<SourceCoverageView>
CodeCoverageTool::createFunctionView(const FunctionRecord &Function,
                                     const CoverageMapping &Coverage) {
  auto FunctionCoverage = Coverage.getCoverageForFunction(Function);
  if (FunctionCoverage.empty())
    return nullptr;
  auto SourceBuffer = getSourceFile(FunctionCoverage.getFilename());
  if (!SourceBuffer)
    return nullptr;

  auto Expansions = FunctionCoverage.getExpansions();
  auto View = SourceCoverageView::create(DC.demangle(Function.Name),
                                         SourceBuffer.get(), ViewOpts,
                                         std::move(FunctionCoverage));
  attachExpansionSubViews(*View, Expansions, Coverage);

  return View;
}

std::unique_ptr<SourceCoverageView>
CodeCoverageTool::createSourceFileView(StringRef SourceFile,
                                       const CoverageMapping &Coverage) {
  auto SourceBuffer = getSourceFile(SourceFile);
  if (!SourceBuffer)
    return nullptr;
  auto FileCoverage = Coverage.getCoverageForFile(SourceFile);
  if (FileCoverage.empty())
    return nullptr;

  auto Expansions = FileCoverage.getExpansions();
  auto View = SourceCoverageView::create(SourceFile, SourceBuffer.get(),
                                         ViewOpts, std::move(FileCoverage));
  attachExpansionSubViews(*View, Expansions, Coverage);

  for (const auto *Function : Coverage.getInstantiations(SourceFile)) {
    std::unique_ptr<SourceCoverageView> SubView{nullptr};

    StringRef Funcname = DC.demangle(Function->Name);

    if (Function->ExecutionCount > 0) {
      auto SubViewCoverage = Coverage.getCoverageForFunction(*Function);
      auto SubViewExpansions = SubViewCoverage.getExpansions();
      SubView = SourceCoverageView::create(
          Funcname, SourceBuffer.get(), ViewOpts, std::move(SubViewCoverage));
      attachExpansionSubViews(*SubView, SubViewExpansions, Coverage);
    }

    unsigned FileID = Function->CountedRegions.front().FileID;
    unsigned Line = 0;
    for (const auto &CR : Function->CountedRegions)
      if (CR.FileID == FileID)
        Line = std::max(CR.LineEnd, Line);
    View->addInstantiation(Funcname, Line, std::move(SubView));
  }
  return View;
}

static bool modifiedTimeGT(StringRef LHS, StringRef RHS) {
  sys::fs::file_status Status;
  if (sys::fs::status(LHS, Status))
    return false;
  auto LHSTime = Status.getLastModificationTime();
  if (sys::fs::status(RHS, Status))
    return false;
  auto RHSTime = Status.getLastModificationTime();
  return LHSTime > RHSTime;
}

std::unique_ptr<CoverageMapping> CodeCoverageTool::load() {
  for (StringRef ObjectFilename : ObjectFilenames)
    if (modifiedTimeGT(ObjectFilename, PGOFilename))
      warning("profile data may be out of date - object is newer",
              ObjectFilename);
  auto CoverageOrErr =
      CoverageMapping::load(ObjectFilenames, PGOFilename, CoverageArch);
  if (Error E = CoverageOrErr.takeError()) {
    error("Failed to load coverage: " + toString(std::move(E)),
          join(ObjectFilenames.begin(), ObjectFilenames.end(), ", "));
    return nullptr;
  }
  auto Coverage = std::move(CoverageOrErr.get());
  unsigned Mismatched = Coverage->getMismatchedCount();
  if (Mismatched)
    warning(utostr(Mismatched) + " functions have mismatched data");

  if (!SourceFiles.empty())
    removeUnmappedInputs(*Coverage);

  demangleSymbols(*Coverage);

  return Coverage;
}

void CodeCoverageTool::removeUnmappedInputs(const CoverageMapping &Coverage) {
  std::vector<StringRef> CoveredFiles = Coverage.getUniqueSourceFiles();

  auto UncoveredFilesIt = SourceFiles.end();
  if (!CompareFilenamesOnly) {
    // The user may have specified source files which aren't in the coverage
    // mapping. Filter these files away.
    UncoveredFilesIt = std::remove_if(
        SourceFiles.begin(), SourceFiles.end(), [&](const std::string &SF) {
          return !std::binary_search(CoveredFiles.begin(), CoveredFiles.end(),
                                     SF);
        });
  } else {
    for (auto &SF : SourceFiles) {
      StringRef SFBase = sys::path::filename(SF);
      for (const auto &CF : CoveredFiles) {
        if (SFBase == sys::path::filename(CF)) {
          RemappedFilenames[CF] = SF;
          SF = CF;
          break;
        }
      }
    }
    UncoveredFilesIt = std::remove_if(
        SourceFiles.begin(), SourceFiles.end(),
        [&](const std::string &SF) { return !RemappedFilenames.count(SF); });
  }

  SourceFiles.erase(UncoveredFilesIt, SourceFiles.end());
}

void CodeCoverageTool::demangleSymbols(const CoverageMapping &Coverage) {
  if (!ViewOpts.hasDemangler())
    return;

  // Pass function names to the demangler in a temporary file.
  int InputFD;
  SmallString<256> InputPath;
  std::error_code EC =
      sys::fs::createTemporaryFile("demangle-in", "list", InputFD, InputPath);
  if (EC) {
    error(InputPath, EC.message());
    return;
  }
  tool_output_file InputTOF{InputPath, InputFD};

  unsigned NumSymbols = 0;
  for (const auto &Function : Coverage.getCoveredFunctions()) {
    InputTOF.os() << Function.Name << '\n';
    ++NumSymbols;
  }
  InputTOF.os().close();

  // Use another temporary file to store the demangler's output.
  int OutputFD;
  SmallString<256> OutputPath;
  EC = sys::fs::createTemporaryFile("demangle-out", "list", OutputFD,
                                    OutputPath);
  if (EC) {
    error(OutputPath, EC.message());
    return;
  }
  tool_output_file OutputTOF{OutputPath, OutputFD};
  OutputTOF.os().close();

  // Invoke the demangler.
  std::vector<const char *> ArgsV;
  for (const std::string &Arg : ViewOpts.DemanglerOpts)
    ArgsV.push_back(Arg.c_str());
  ArgsV.push_back(nullptr);
  StringRef InputPathRef = InputPath.str();
  StringRef OutputPathRef = OutputPath.str();
  StringRef StderrRef;
  const StringRef *Redirects[] = {&InputPathRef, &OutputPathRef, &StderrRef};
  std::string ErrMsg;
  int RC = sys::ExecuteAndWait(ViewOpts.DemanglerOpts[0], ArgsV.data(),
                               /*env=*/nullptr, Redirects, /*secondsToWait=*/0,
                               /*memoryLimit=*/0, &ErrMsg);
  if (RC) {
    error(ErrMsg, ViewOpts.DemanglerOpts[0]);
    return;
  }

  // Parse the demangler's output.
  auto BufOrError = MemoryBuffer::getFile(OutputPath);
  if (!BufOrError) {
    error(OutputPath, BufOrError.getError().message());
    return;
  }

  std::unique_ptr<MemoryBuffer> DemanglerBuf = std::move(*BufOrError);

  SmallVector<StringRef, 8> Symbols;
  StringRef DemanglerData = DemanglerBuf->getBuffer();
  DemanglerData.split(Symbols, '\n', /*MaxSplit=*/NumSymbols,
                      /*KeepEmpty=*/false);
  if (Symbols.size() != NumSymbols) {
    error("Demangler did not provide expected number of symbols");
    return;
  }

  // Cache the demangled names.
  unsigned I = 0;
  for (const auto &Function : Coverage.getCoveredFunctions())
    // On Windows, lines in the demangler's output file end with "\r\n".
    // Splitting by '\n' keeps '\r's, so cut them now.
    DC.DemangledNames[Function.Name] = Symbols[I++].rtrim();
}

void CodeCoverageTool::writeSourceFileView(StringRef SourceFile,
                                           CoverageMapping *Coverage,
                                           CoveragePrinter *Printer,
                                           bool ShowFilenames) {
  auto View = createSourceFileView(SourceFile, *Coverage);
  if (!View) {
    warning("The file '" + SourceFile + "' isn't covered.");
    return;
  }

  auto OSOrErr = Printer->createViewFile(SourceFile, /*InToplevel=*/false);
  if (Error E = OSOrErr.takeError()) {
    error("Could not create view file!", toString(std::move(E)));
    return;
  }
  auto OS = std::move(OSOrErr.get());

  View->print(*OS.get(), /*Wholefile=*/true,
              /*ShowSourceName=*/ShowFilenames);
  Printer->closeViewFile(std::move(OS));
}

int CodeCoverageTool::run(Command Cmd, int argc, const char **argv) {
  cl::opt<std::string> CovFilename(
      cl::Positional, cl::desc("Covered executable or object file."));

  cl::list<std::string> CovFilenames(
      "object", cl::desc("Coverage executable or object file"), cl::ZeroOrMore,
      cl::CommaSeparated);

  cl::list<std::string> InputSourceFiles(
      cl::Positional, cl::desc("<Source files>"), cl::ZeroOrMore);

  cl::opt<bool> DebugDumpCollectedPaths(
      "dump-collected-paths", cl::Optional, cl::Hidden,
      cl::desc("Show the collected paths to source files"));

  cl::opt<std::string, true> PGOFilename(
      "instr-profile", cl::Required, cl::location(this->PGOFilename),
      cl::desc(
          "File with the profile data obtained after an instrumented run"));

  cl::opt<std::string> Arch(
      "arch", cl::desc("architecture of the coverage mapping binary"));

  cl::opt<bool> DebugDump("dump", cl::Optional,
                          cl::desc("Show internal debug dump"));

  cl::opt<CoverageViewOptions::OutputFormat> Format(
      "format", cl::desc("Output format for line-based coverage reports"),
      cl::values(clEnumValN(CoverageViewOptions::OutputFormat::Text, "text",
                            "Text output"),
                 clEnumValN(CoverageViewOptions::OutputFormat::HTML, "html",
                            "HTML output")),
      cl::init(CoverageViewOptions::OutputFormat::Text));

  cl::opt<bool> FilenameEquivalence(
      "filename-equivalence", cl::Optional,
      cl::desc("Treat source files as equivalent to paths in the coverage data "
               "when the file names match, even if the full paths do not"));

  cl::OptionCategory FilteringCategory("Function filtering options");

  cl::list<std::string> NameFilters(
      "name", cl::Optional,
      cl::desc("Show code coverage only for functions with the given name"),
      cl::ZeroOrMore, cl::cat(FilteringCategory));

  cl::list<std::string> NameRegexFilters(
      "name-regex", cl::Optional,
      cl::desc("Show code coverage only for functions that match the given "
               "regular expression"),
      cl::ZeroOrMore, cl::cat(FilteringCategory));

  cl::opt<double> RegionCoverageLtFilter(
      "region-coverage-lt", cl::Optional,
      cl::desc("Show code coverage only for functions with region coverage "
               "less than the given threshold"),
      cl::cat(FilteringCategory));

  cl::opt<double> RegionCoverageGtFilter(
      "region-coverage-gt", cl::Optional,
      cl::desc("Show code coverage only for functions with region coverage "
               "greater than the given threshold"),
      cl::cat(FilteringCategory));

  cl::opt<double> LineCoverageLtFilter(
      "line-coverage-lt", cl::Optional,
      cl::desc("Show code coverage only for functions with line coverage less "
               "than the given threshold"),
      cl::cat(FilteringCategory));

  cl::opt<double> LineCoverageGtFilter(
      "line-coverage-gt", cl::Optional,
      cl::desc("Show code coverage only for functions with line coverage "
               "greater than the given threshold"),
      cl::cat(FilteringCategory));

  cl::opt<cl::boolOrDefault> UseColor(
      "use-color", cl::desc("Emit colored output (default=autodetect)"),
      cl::init(cl::BOU_UNSET));

  cl::list<std::string> DemanglerOpts(
      "Xdemangler", cl::desc("<demangler-path>|<demangler-option>"));

  auto commandLineParser = [&, this](int argc, const char **argv) -> int {
    cl::ParseCommandLineOptions(argc, argv, "LLVM code coverage tool\n");
    ViewOpts.Debug = DebugDump;
    CompareFilenamesOnly = FilenameEquivalence;

    if (!CovFilename.empty())
      ObjectFilenames.emplace_back(CovFilename);
    for (const std::string &Filename : CovFilenames)
      ObjectFilenames.emplace_back(Filename);
    if (ObjectFilenames.empty()) {
      errs() << "No filenames specified!\n";
      ::exit(1);
    }

    ViewOpts.Format = Format;
    switch (ViewOpts.Format) {
    case CoverageViewOptions::OutputFormat::Text:
      ViewOpts.Colors = UseColor == cl::BOU_UNSET
                            ? sys::Process::StandardOutHasColors()
                            : UseColor == cl::BOU_TRUE;
      break;
    case CoverageViewOptions::OutputFormat::HTML:
      if (UseColor == cl::BOU_FALSE)
        errs() << "Color output cannot be disabled when generating html.\n";
      ViewOpts.Colors = true;
      break;
    }

    // If a demangler is supplied, check if it exists and register it.
    if (DemanglerOpts.size()) {
      auto DemanglerPathOrErr = sys::findProgramByName(DemanglerOpts[0]);
      if (!DemanglerPathOrErr) {
        error("Could not find the demangler!",
              DemanglerPathOrErr.getError().message());
        return 1;
      }
      DemanglerOpts[0] = *DemanglerPathOrErr;
      ViewOpts.DemanglerOpts.swap(DemanglerOpts);
    }

    // Create the function filters
    if (!NameFilters.empty() || !NameRegexFilters.empty()) {
      auto NameFilterer = new CoverageFilters;
      for (const auto &Name : NameFilters)
        NameFilterer->push_back(llvm::make_unique<NameCoverageFilter>(Name));
      for (const auto &Regex : NameRegexFilters)
        NameFilterer->push_back(
            llvm::make_unique<NameRegexCoverageFilter>(Regex));
      Filters.push_back(std::unique_ptr<CoverageFilter>(NameFilterer));
    }
    if (RegionCoverageLtFilter.getNumOccurrences() ||
        RegionCoverageGtFilter.getNumOccurrences() ||
        LineCoverageLtFilter.getNumOccurrences() ||
        LineCoverageGtFilter.getNumOccurrences()) {
      auto StatFilterer = new CoverageFilters;
      if (RegionCoverageLtFilter.getNumOccurrences())
        StatFilterer->push_back(llvm::make_unique<RegionCoverageFilter>(
            RegionCoverageFilter::LessThan, RegionCoverageLtFilter));
      if (RegionCoverageGtFilter.getNumOccurrences())
        StatFilterer->push_back(llvm::make_unique<RegionCoverageFilter>(
            RegionCoverageFilter::GreaterThan, RegionCoverageGtFilter));
      if (LineCoverageLtFilter.getNumOccurrences())
        StatFilterer->push_back(llvm::make_unique<LineCoverageFilter>(
            LineCoverageFilter::LessThan, LineCoverageLtFilter));
      if (LineCoverageGtFilter.getNumOccurrences())
        StatFilterer->push_back(llvm::make_unique<LineCoverageFilter>(
            RegionCoverageFilter::GreaterThan, LineCoverageGtFilter));
      Filters.push_back(std::unique_ptr<CoverageFilter>(StatFilterer));
    }

    if (!Arch.empty() &&
        Triple(Arch).getArch() == llvm::Triple::ArchType::UnknownArch) {
      error("Unknown architecture: " + Arch);
      return 1;
    }
    CoverageArch = Arch;

    for (const std::string &File : InputSourceFiles)
      collectPaths(File);

    if (DebugDumpCollectedPaths) {
      for (const std::string &SF : SourceFiles)
        outs() << SF << '\n';
      ::exit(0);
    }

    return 0;
  };

  switch (Cmd) {
  case Show:
    return show(argc, argv, commandLineParser);
  case Report:
    return report(argc, argv, commandLineParser);
  case Export:
    return export_(argc, argv, commandLineParser);
  }
  return 0;
}

int CodeCoverageTool::show(int argc, const char **argv,
                           CommandLineParserType commandLineParser) {

  cl::OptionCategory ViewCategory("Viewing options");

  cl::opt<bool> ShowLineExecutionCounts(
      "show-line-counts", cl::Optional,
      cl::desc("Show the execution counts for each line"), cl::init(true),
      cl::cat(ViewCategory));

  cl::opt<bool> ShowRegions(
      "show-regions", cl::Optional,
      cl::desc("Show the execution counts for each region"),
      cl::cat(ViewCategory));

  cl::opt<bool> ShowBestLineRegionsCounts(
      "show-line-counts-or-regions", cl::Optional,
      cl::desc("Show the execution counts for each line, or the execution "
               "counts for each region on lines that have multiple regions"),
      cl::cat(ViewCategory));

  cl::opt<bool> ShowExpansions("show-expansions", cl::Optional,
                               cl::desc("Show expanded source regions"),
                               cl::cat(ViewCategory));

  cl::opt<bool> ShowInstantiations("show-instantiations", cl::Optional,
                                   cl::desc("Show function instantiations"),
                                   cl::cat(ViewCategory));

  cl::opt<std::string> ShowOutputDirectory(
      "output-dir", cl::init(""),
      cl::desc("Directory in which coverage information is written out"));
  cl::alias ShowOutputDirectoryA("o", cl::desc("Alias for --output-dir"),
                                 cl::aliasopt(ShowOutputDirectory));

  cl::opt<uint32_t> TabSize(
      "tab-size", cl::init(2),
      cl::desc(
          "Set tab expansion size for html coverage reports (default = 2)"));

  cl::opt<std::string> ProjectTitle(
      "project-title", cl::Optional,
      cl::desc("Set project title for the coverage report"));

  cl::opt<unsigned> NumThreads(
      "num-threads", cl::init(0),
      cl::desc("Number of merge threads to use (default: autodetect)"));
  cl::alias NumThreadsA("j", cl::desc("Alias for --num-threads"),
                        cl::aliasopt(NumThreads));

  auto Err = commandLineParser(argc, argv);
  if (Err)
    return Err;

  ViewOpts.ShowLineNumbers = true;
  ViewOpts.ShowLineStats = ShowLineExecutionCounts.getNumOccurrences() != 0 ||
                           !ShowRegions || ShowBestLineRegionsCounts;
  ViewOpts.ShowRegionMarkers = ShowRegions || ShowBestLineRegionsCounts;
  ViewOpts.ShowLineStatsOrRegionMarkers = ShowBestLineRegionsCounts;
  ViewOpts.ShowExpandedRegions = ShowExpansions;
  ViewOpts.ShowFunctionInstantiations = ShowInstantiations;
  ViewOpts.ShowOutputDirectory = ShowOutputDirectory;
  ViewOpts.TabSize = TabSize;
  ViewOpts.ProjectTitle = ProjectTitle;

  if (ViewOpts.hasOutputDirectory()) {
    if (auto E = sys::fs::create_directories(ViewOpts.ShowOutputDirectory)) {
      error("Could not create output directory!", E.message());
      return 1;
    }
  }

  sys::fs::file_status Status;
  if (sys::fs::status(PGOFilename, Status)) {
    error("profdata file error: can not get the file status. \n");
    return 1;
  }

  auto ModifiedTime = Status.getLastModificationTime();
  std::string ModifiedTimeStr = to_string(ModifiedTime);
  size_t found = ModifiedTimeStr.rfind(':');
  ViewOpts.CreatedTimeStr = (found != std::string::npos)
                                ? "Created: " + ModifiedTimeStr.substr(0, found)
                                : "Created: " + ModifiedTimeStr;

  auto Coverage = load();
  if (!Coverage)
    return 1;

  auto Printer = CoveragePrinter::create(ViewOpts);

  if (!Filters.empty()) {
    auto OSOrErr = Printer->createViewFile("functions", /*InToplevel=*/true);
    if (Error E = OSOrErr.takeError()) {
      error("Could not create view file!", toString(std::move(E)));
      return 1;
    }
    auto OS = std::move(OSOrErr.get());

    // Show functions.
    for (const auto &Function : Coverage->getCoveredFunctions()) {
      if (!Filters.matches(Function))
        continue;

      auto mainView = createFunctionView(Function, *Coverage);
      if (!mainView) {
        warning("Could not read coverage for '" + Function.Name + "'.");
        continue;
      }

      mainView->print(*OS.get(), /*WholeFile=*/false, /*ShowSourceName=*/true);
    }

    Printer->closeViewFile(std::move(OS));
    return 0;
  }

  // Show files
  bool ShowFilenames =
      (SourceFiles.size() != 1) || ViewOpts.hasOutputDirectory() ||
      (ViewOpts.Format == CoverageViewOptions::OutputFormat::HTML);

  if (SourceFiles.empty())
    // Get the source files from the function coverage mapping.
    for (StringRef Filename : Coverage->getUniqueSourceFiles())
      SourceFiles.push_back(Filename);

  // Create an index out of the source files.
  if (ViewOpts.hasOutputDirectory()) {
    if (Error E = Printer->createIndexFile(SourceFiles, *Coverage)) {
      error("Could not create index file!", toString(std::move(E)));
      return 1;
    }
  }

  // If NumThreads is not specified, auto-detect a good default.
  if (NumThreads == 0)
    NumThreads =
        std::max(1U, std::min(llvm::heavyweight_hardware_concurrency(),
                              unsigned(SourceFiles.size())));

  if (!ViewOpts.hasOutputDirectory() || NumThreads == 1) {
    for (const std::string &SourceFile : SourceFiles)
      writeSourceFileView(SourceFile, Coverage.get(), Printer.get(),
                          ShowFilenames);
  } else {
    // In -output-dir mode, it's safe to use multiple threads to print files.
    ThreadPool Pool(NumThreads);
    for (const std::string &SourceFile : SourceFiles)
      Pool.async(&CodeCoverageTool::writeSourceFileView, this, SourceFile,
                 Coverage.get(), Printer.get(), ShowFilenames);
    Pool.wait();
  }

  return 0;
}

int CodeCoverageTool::report(int argc, const char **argv,
                             CommandLineParserType commandLineParser) {
  cl::opt<bool> ShowFunctionSummaries(
      "show-functions", cl::Optional, cl::init(false),
      cl::desc("Show coverage summaries for each function"));

  auto Err = commandLineParser(argc, argv);
  if (Err)
    return Err;

  if (ViewOpts.Format == CoverageViewOptions::OutputFormat::HTML) {
    error("HTML output for summary reports is not yet supported.");
    return 1;
  }

  auto Coverage = load();
  if (!Coverage)
    return 1;

  CoverageReport Report(ViewOpts, *Coverage.get());
  if (!ShowFunctionSummaries)
    Report.renderFileReports(llvm::outs());
  else
    Report.renderFunctionReports(SourceFiles, DC, llvm::outs());
  return 0;
}

int CodeCoverageTool::export_(int argc, const char **argv,
                              CommandLineParserType commandLineParser) {

  auto Err = commandLineParser(argc, argv);
  if (Err)
    return Err;

  if (ViewOpts.Format != CoverageViewOptions::OutputFormat::Text) {
    error("Coverage data can only be exported as textual JSON.");
    return 1;
  }

  auto Coverage = load();
  if (!Coverage) {
    error("Could not load coverage information");
    return 1;
  }

  exportCoverageDataToJson(*Coverage.get(), outs());

  return 0;
}

int showMain(int argc, const char *argv[]) {
  CodeCoverageTool Tool;
  return Tool.run(CodeCoverageTool::Show, argc, argv);
}

int reportMain(int argc, const char *argv[]) {
  CodeCoverageTool Tool;
  return Tool.run(CodeCoverageTool::Report, argc, argv);
}

int exportMain(int argc, const char *argv[]) {
  CodeCoverageTool Tool;
  return Tool.run(CodeCoverageTool::Export, argc, argv);
}
