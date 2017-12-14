//===- llvm-mt.cpp - Merge .manifest files ---------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===---------------------------------------------------------------------===//
//
// Merge .manifest files.  This is intended to be a platform-independent port
// of Microsoft's mt.exe.
//
//===---------------------------------------------------------------------===//

#include "llvm/Option/Arg.h"
#include "llvm/Option/ArgList.h"
#include "llvm/Option/Option.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/ManagedStatic.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/PrettyStackTrace.h"
#include "llvm/Support/Process.h"
#include "llvm/Support/Signals.h"
#include "llvm/Support/raw_ostream.h"

#include <system_error>

using namespace llvm;

namespace {

enum ID {
  OPT_INVALID = 0, // This is not an option ID.
#define OPTION(PREFIX, NAME, ID, KIND, GROUP, ALIAS, ALIASARGS, FLAGS, PARAM,  \
               HELPTEXT, METAVAR, VALUES)                                      \
  OPT_##ID,
#include "Opts.inc"
#undef OPTION
};

#define PREFIX(NAME, VALUE) const char *const NAME[] = VALUE;
#include "Opts.inc"
#undef PREFIX

static const opt::OptTable::Info InfoTable[] = {
#define OPTION(PREFIX, NAME, ID, KIND, GROUP, ALIAS, ALIASARGS, FLAGS, PARAM,  \
               HELPTEXT, METAVAR, VALUES)                                      \
{                                                                              \
      PREFIX,      NAME,      HELPTEXT,                                        \
      METAVAR,     OPT_##ID,  opt::Option::KIND##Class,                        \
      PARAM,       FLAGS,     OPT_##GROUP,                                     \
      OPT_##ALIAS, ALIASARGS, VALUES},
#include "Opts.inc"
#undef OPTION
};

class CvtResOptTable : public opt::OptTable {
public:
  CvtResOptTable() : OptTable(InfoTable, true) {}
};

static ExitOnError ExitOnErr;
} // namespace

LLVM_ATTRIBUTE_NORETURN void reportError(Twine Msg) {
  errs() << "llvm-mt error: " << Msg << "\n";
  exit(1);
}

int main(int argc, const char **argv) {
  sys::PrintStackTraceOnErrorSignal(argv[0]);
  PrettyStackTraceProgram X(argc, argv);

  ExitOnErr.setBanner("llvm-mt: ");

  SmallVector<const char *, 256> argv_buf;
  SpecificBumpPtrAllocator<char> ArgAllocator;
  ExitOnErr(errorCodeToError(sys::Process::GetArgumentVector(
      argv_buf, makeArrayRef(argv, argc), ArgAllocator)));

  llvm_shutdown_obj Y; // Call llvm_shutdown() on exit.

  CvtResOptTable T;
  unsigned MAI, MAC;
  ArrayRef<const char *> ArgsArr = makeArrayRef(argv + 1, argc);
  opt::InputArgList InputArgs = T.ParseArgs(ArgsArr, MAI, MAC);

  for (auto &Arg : InputArgs) {
    if (Arg->getOption().matches(OPT_unsupported)) {
      outs() << "llvm-mt: ignoring unsupported '" << Arg->getOption().getName()
             << "' option\n";
    }
  }

  if (InputArgs.hasArg(OPT_help)) {
    T.PrintHelp(outs(), "mt", "Manifest Tool", false);
    return 0;
  }

  std::vector<std::string> InputFiles = InputArgs.getAllArgValues(OPT_manifest);

  if (InputFiles.size() == 0) {
    reportError("no input file specified");
  }

  StringRef OutputFile;

  if (InputArgs.hasArg(OPT_out)) {
    OutputFile = InputArgs.getLastArgValue(OPT_out);
  } else if (InputFiles.size() == 1) {
    OutputFile = InputFiles[0];
  } else {
    reportError("no output file specified");
  }

  return 0;
}
