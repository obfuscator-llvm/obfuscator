//===- llvm/unittest/Support/CommandLineTest.cpp - CommandLine tests ------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "llvm/Support/CommandLine.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/Config/config.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/StringSaver.h"
#include "gtest/gtest.h"
#include <fstream>
#include <stdlib.h>
#include <string>

using namespace llvm;

namespace {

class TempEnvVar {
 public:
  TempEnvVar(const char *name, const char *value)
      : name(name) {
    const char *old_value = getenv(name);
    EXPECT_EQ(nullptr, old_value) << old_value;
#if HAVE_SETENV
    setenv(name, value, true);
#else
#   define SKIP_ENVIRONMENT_TESTS
#endif
  }

  ~TempEnvVar() {
#if HAVE_SETENV
    // Assume setenv and unsetenv come together.
    unsetenv(name);
#else
    (void)name; // Suppress -Wunused-private-field.
#endif
  }

 private:
  const char *const name;
};

template <typename T>
class StackOption : public cl::opt<T> {
  typedef cl::opt<T> Base;
public:
  // One option...
  template<class M0t>
  explicit StackOption(const M0t &M0) : Base(M0) {}

  // Two options...
  template<class M0t, class M1t>
  StackOption(const M0t &M0, const M1t &M1) : Base(M0, M1) {}

  // Three options...
  template<class M0t, class M1t, class M2t>
  StackOption(const M0t &M0, const M1t &M1, const M2t &M2) : Base(M0, M1, M2) {}

  // Four options...
  template<class M0t, class M1t, class M2t, class M3t>
  StackOption(const M0t &M0, const M1t &M1, const M2t &M2, const M3t &M3)
    : Base(M0, M1, M2, M3) {}

  ~StackOption() override { this->removeArgument(); }

  template <class DT> StackOption<T> &operator=(const DT &V) {
    this->setValue(V);
    return *this;
  }
};

class StackSubCommand : public cl::SubCommand {
public:
  StackSubCommand(StringRef Name,
                  StringRef Description = StringRef())
      : SubCommand(Name, Description) {}

  StackSubCommand() : SubCommand() {}

  ~StackSubCommand() { unregisterSubCommand(); }
};


cl::OptionCategory TestCategory("Test Options", "Description");
TEST(CommandLineTest, ModifyExisitingOption) {
  StackOption<int> TestOption("test-option", cl::desc("old description"));

  const char Description[] = "New description";
  const char ArgString[] = "new-test-option";
  const char ValueString[] = "Integer";

  StringMap<cl::Option *> &Map =
      cl::getRegisteredOptions(*cl::TopLevelSubCommand);

  ASSERT_TRUE(Map.count("test-option") == 1) <<
    "Could not find option in map.";

  cl::Option *Retrieved = Map["test-option"];
  ASSERT_EQ(&TestOption, Retrieved) << "Retrieved wrong option.";

  ASSERT_EQ(&cl::GeneralCategory,Retrieved->Category) <<
    "Incorrect default option category.";

  Retrieved->setCategory(TestCategory);
  ASSERT_EQ(&TestCategory,Retrieved->Category) <<
    "Failed to modify option's option category.";

  Retrieved->setDescription(Description);
  ASSERT_STREQ(Retrieved->HelpStr.data(), Description)
      << "Changing option description failed.";

  Retrieved->setArgStr(ArgString);
  ASSERT_STREQ(ArgString, Retrieved->ArgStr.data())
      << "Failed to modify option's Argument string.";

  Retrieved->setValueStr(ValueString);
  ASSERT_STREQ(Retrieved->ValueStr.data(), ValueString)
      << "Failed to modify option's Value string.";

  Retrieved->setHiddenFlag(cl::Hidden);
  ASSERT_EQ(cl::Hidden, TestOption.getOptionHiddenFlag()) <<
    "Failed to modify option's hidden flag.";
}
#ifndef SKIP_ENVIRONMENT_TESTS

const char test_env_var[] = "LLVM_TEST_COMMAND_LINE_FLAGS";

cl::opt<std::string> EnvironmentTestOption("env-test-opt");
TEST(CommandLineTest, ParseEnvironment) {
  TempEnvVar TEV(test_env_var, "-env-test-opt=hello");
  EXPECT_EQ("", EnvironmentTestOption);
  cl::ParseEnvironmentOptions("CommandLineTest", test_env_var);
  EXPECT_EQ("hello", EnvironmentTestOption);
}

// This test used to make valgrind complain
// ("Conditional jump or move depends on uninitialised value(s)")
//
// Warning: Do not run any tests after this one that try to gain access to
// registered command line options because this will likely result in a
// SEGFAULT. This can occur because the cl::opt in the test below is declared
// on the stack which will be destroyed after the test completes but the
// command line system will still hold a pointer to a deallocated cl::Option.
TEST(CommandLineTest, ParseEnvironmentToLocalVar) {
  // Put cl::opt on stack to check for proper initialization of fields.
  StackOption<std::string> EnvironmentTestOptionLocal("env-test-opt-local");
  TempEnvVar TEV(test_env_var, "-env-test-opt-local=hello-local");
  EXPECT_EQ("", EnvironmentTestOptionLocal);
  cl::ParseEnvironmentOptions("CommandLineTest", test_env_var);
  EXPECT_EQ("hello-local", EnvironmentTestOptionLocal);
}

#endif  // SKIP_ENVIRONMENT_TESTS

TEST(CommandLineTest, UseOptionCategory) {
  StackOption<int> TestOption2("test-option", cl::cat(TestCategory));

  ASSERT_EQ(&TestCategory,TestOption2.Category) << "Failed to assign Option "
                                                  "Category.";
}

typedef void ParserFunction(StringRef Source, StringSaver &Saver,
                            SmallVectorImpl<const char *> &NewArgv,
                            bool MarkEOLs);

void testCommandLineTokenizer(ParserFunction *parse, StringRef Input,
                              const char *const Output[], size_t OutputSize) {
  SmallVector<const char *, 0> Actual;
  BumpPtrAllocator A;
  StringSaver Saver(A);
  parse(Input, Saver, Actual, /*MarkEOLs=*/false);
  EXPECT_EQ(OutputSize, Actual.size());
  for (unsigned I = 0, E = Actual.size(); I != E; ++I) {
    if (I < OutputSize) {
      EXPECT_STREQ(Output[I], Actual[I]);
    }
  }
}

TEST(CommandLineTest, TokenizeGNUCommandLine) {
  const char Input[] =
      "foo\\ bar \"foo bar\" \'foo bar\' 'foo\\\\bar' -DFOO=bar\\(\\) "
      "foo\"bar\"baz C:\\\\src\\\\foo.cpp \"C:\\src\\foo.cpp\"";
  const char *const Output[] = {
      "foo bar",     "foo bar",   "foo bar",          "foo\\bar",
      "-DFOO=bar()", "foobarbaz", "C:\\src\\foo.cpp", "C:srcfoo.cpp"};
  testCommandLineTokenizer(cl::TokenizeGNUCommandLine, Input, Output,
                           array_lengthof(Output));
}

TEST(CommandLineTest, TokenizeWindowsCommandLine) {
  const char Input[] = "a\\b c\\\\d e\\\\\"f g\" h\\\"i j\\\\\\\"k \"lmn\" o pqr "
                      "\"st \\\"u\" \\v";
  const char *const Output[] = { "a\\b", "c\\\\d", "e\\f g", "h\"i", "j\\\"k",
                                 "lmn", "o", "pqr", "st \"u", "\\v" };
  testCommandLineTokenizer(cl::TokenizeWindowsCommandLine, Input, Output,
                           array_lengthof(Output));
}

TEST(CommandLineTest, AliasesWithArguments) {
  static const size_t ARGC = 3;
  const char *const Inputs[][ARGC] = {
    { "-tool", "-actual=x", "-extra" },
    { "-tool", "-actual", "x" },
    { "-tool", "-alias=x", "-extra" },
    { "-tool", "-alias", "x" }
  };

  for (size_t i = 0, e = array_lengthof(Inputs); i < e; ++i) {
    StackOption<std::string> Actual("actual");
    StackOption<bool> Extra("extra");
    StackOption<std::string> Input(cl::Positional);

    cl::alias Alias("alias", llvm::cl::aliasopt(Actual));

    cl::ParseCommandLineOptions(ARGC, Inputs[i]);
    EXPECT_EQ("x", Actual);
    EXPECT_EQ(0, Input.getNumOccurrences());

    Alias.removeArgument();
  }
}

void testAliasRequired(int argc, const char *const *argv) {
  StackOption<std::string> Option("option", cl::Required);
  cl::alias Alias("o", llvm::cl::aliasopt(Option));

  cl::ParseCommandLineOptions(argc, argv);
  EXPECT_EQ("x", Option);
  EXPECT_EQ(1, Option.getNumOccurrences());

  Alias.removeArgument();
}

TEST(CommandLineTest, AliasRequired) {
  const char *opts1[] = { "-tool", "-option=x" };
  const char *opts2[] = { "-tool", "-o", "x" };
  testAliasRequired(array_lengthof(opts1), opts1);
  testAliasRequired(array_lengthof(opts2), opts2);
}

TEST(CommandLineTest, HideUnrelatedOptions) {
  StackOption<int> TestOption1("hide-option-1");
  StackOption<int> TestOption2("hide-option-2", cl::cat(TestCategory));

  cl::HideUnrelatedOptions(TestCategory);

  ASSERT_EQ(cl::ReallyHidden, TestOption1.getOptionHiddenFlag())
      << "Failed to hide extra option.";
  ASSERT_EQ(cl::NotHidden, TestOption2.getOptionHiddenFlag())
      << "Hid extra option that should be visable.";

  StringMap<cl::Option *> &Map =
      cl::getRegisteredOptions(*cl::TopLevelSubCommand);
  ASSERT_EQ(cl::NotHidden, Map["help"]->getOptionHiddenFlag())
      << "Hid default option that should be visable.";
}

cl::OptionCategory TestCategory2("Test Options set 2", "Description");

TEST(CommandLineTest, HideUnrelatedOptionsMulti) {
  StackOption<int> TestOption1("multi-hide-option-1");
  StackOption<int> TestOption2("multi-hide-option-2", cl::cat(TestCategory));
  StackOption<int> TestOption3("multi-hide-option-3", cl::cat(TestCategory2));

  const cl::OptionCategory *VisibleCategories[] = {&TestCategory,
                                                   &TestCategory2};

  cl::HideUnrelatedOptions(makeArrayRef(VisibleCategories));

  ASSERT_EQ(cl::ReallyHidden, TestOption1.getOptionHiddenFlag())
      << "Failed to hide extra option.";
  ASSERT_EQ(cl::NotHidden, TestOption2.getOptionHiddenFlag())
      << "Hid extra option that should be visable.";
  ASSERT_EQ(cl::NotHidden, TestOption3.getOptionHiddenFlag())
      << "Hid extra option that should be visable.";

  StringMap<cl::Option *> &Map =
      cl::getRegisteredOptions(*cl::TopLevelSubCommand);
  ASSERT_EQ(cl::NotHidden, Map["help"]->getOptionHiddenFlag())
      << "Hid default option that should be visable.";
}

TEST(CommandLineTest, SetValueInSubcategories) {
  cl::ResetCommandLineParser();

  StackSubCommand SC1("sc1", "First subcommand");
  StackSubCommand SC2("sc2", "Second subcommand");

  StackOption<bool> TopLevelOpt("top-level", cl::init(false));
  StackOption<bool> SC1Opt("sc1", cl::sub(SC1), cl::init(false));
  StackOption<bool> SC2Opt("sc2", cl::sub(SC2), cl::init(false));

  EXPECT_FALSE(TopLevelOpt);
  EXPECT_FALSE(SC1Opt);
  EXPECT_FALSE(SC2Opt);
  const char *args[] = {"prog", "-top-level"};
  EXPECT_TRUE(
      cl::ParseCommandLineOptions(2, args, StringRef(), &llvm::nulls()));
  EXPECT_TRUE(TopLevelOpt);
  EXPECT_FALSE(SC1Opt);
  EXPECT_FALSE(SC2Opt);

  TopLevelOpt = false;

  cl::ResetAllOptionOccurrences();
  EXPECT_FALSE(TopLevelOpt);
  EXPECT_FALSE(SC1Opt);
  EXPECT_FALSE(SC2Opt);
  const char *args2[] = {"prog", "sc1", "-sc1"};
  EXPECT_TRUE(
      cl::ParseCommandLineOptions(3, args2, StringRef(), &llvm::nulls()));
  EXPECT_FALSE(TopLevelOpt);
  EXPECT_TRUE(SC1Opt);
  EXPECT_FALSE(SC2Opt);

  SC1Opt = false;

  cl::ResetAllOptionOccurrences();
  EXPECT_FALSE(TopLevelOpt);
  EXPECT_FALSE(SC1Opt);
  EXPECT_FALSE(SC2Opt);
  const char *args3[] = {"prog", "sc2", "-sc2"};
  EXPECT_TRUE(
      cl::ParseCommandLineOptions(3, args3, StringRef(), &llvm::nulls()));
  EXPECT_FALSE(TopLevelOpt);
  EXPECT_FALSE(SC1Opt);
  EXPECT_TRUE(SC2Opt);
}

TEST(CommandLineTest, LookupFailsInWrongSubCommand) {
  cl::ResetCommandLineParser();

  StackSubCommand SC1("sc1", "First subcommand");
  StackSubCommand SC2("sc2", "Second subcommand");

  StackOption<bool> SC1Opt("sc1", cl::sub(SC1), cl::init(false));
  StackOption<bool> SC2Opt("sc2", cl::sub(SC2), cl::init(false));

  std::string Errs;
  raw_string_ostream OS(Errs);

  const char *args[] = {"prog", "sc1", "-sc2"};
  EXPECT_FALSE(cl::ParseCommandLineOptions(3, args, StringRef(), &OS));
  OS.flush();
  EXPECT_FALSE(Errs.empty());
}

TEST(CommandLineTest, AddToAllSubCommands) {
  cl::ResetCommandLineParser();

  StackSubCommand SC1("sc1", "First subcommand");
  StackOption<bool> AllOpt("everywhere", cl::sub(*cl::AllSubCommands),
                           cl::init(false));
  StackSubCommand SC2("sc2", "Second subcommand");

  const char *args[] = {"prog", "-everywhere"};
  const char *args2[] = {"prog", "sc1", "-everywhere"};
  const char *args3[] = {"prog", "sc2", "-everywhere"};

  std::string Errs;
  raw_string_ostream OS(Errs);

  EXPECT_FALSE(AllOpt);
  EXPECT_TRUE(cl::ParseCommandLineOptions(2, args, StringRef(), &OS));
  EXPECT_TRUE(AllOpt);

  AllOpt = false;

  cl::ResetAllOptionOccurrences();
  EXPECT_FALSE(AllOpt);
  EXPECT_TRUE(cl::ParseCommandLineOptions(3, args2, StringRef(), &OS));
  EXPECT_TRUE(AllOpt);

  AllOpt = false;

  cl::ResetAllOptionOccurrences();
  EXPECT_FALSE(AllOpt);
  EXPECT_TRUE(cl::ParseCommandLineOptions(3, args3, StringRef(), &OS));
  EXPECT_TRUE(AllOpt);

  // Since all parsing succeeded, the error message should be empty.
  OS.flush();
  EXPECT_TRUE(Errs.empty());
}

TEST(CommandLineTest, ReparseCommandLineOptions) {
  cl::ResetCommandLineParser();

  StackOption<bool> TopLevelOpt("top-level", cl::sub(*cl::TopLevelSubCommand),
                                cl::init(false));

  const char *args[] = {"prog", "-top-level"};

  EXPECT_FALSE(TopLevelOpt);
  EXPECT_TRUE(
      cl::ParseCommandLineOptions(2, args, StringRef(), &llvm::nulls()));
  EXPECT_TRUE(TopLevelOpt);

  TopLevelOpt = false;

  cl::ResetAllOptionOccurrences();
  EXPECT_FALSE(TopLevelOpt);
  EXPECT_TRUE(
      cl::ParseCommandLineOptions(2, args, StringRef(), &llvm::nulls()));
  EXPECT_TRUE(TopLevelOpt);
}

TEST(CommandLineTest, RemoveFromRegularSubCommand) {
  cl::ResetCommandLineParser();

  StackSubCommand SC("sc", "Subcommand");
  StackOption<bool> RemoveOption("remove-option", cl::sub(SC), cl::init(false));
  StackOption<bool> KeepOption("keep-option", cl::sub(SC), cl::init(false));

  const char *args[] = {"prog", "sc", "-remove-option"};

  std::string Errs;
  raw_string_ostream OS(Errs);

  EXPECT_FALSE(RemoveOption);
  EXPECT_TRUE(cl::ParseCommandLineOptions(3, args, StringRef(), &OS));
  EXPECT_TRUE(RemoveOption);
  OS.flush();
  EXPECT_TRUE(Errs.empty());

  RemoveOption.removeArgument();

  cl::ResetAllOptionOccurrences();
  EXPECT_FALSE(cl::ParseCommandLineOptions(3, args, StringRef(), &OS));
  OS.flush();
  EXPECT_FALSE(Errs.empty());
}

TEST(CommandLineTest, RemoveFromTopLevelSubCommand) {
  cl::ResetCommandLineParser();

  StackOption<bool> TopLevelRemove(
      "top-level-remove", cl::sub(*cl::TopLevelSubCommand), cl::init(false));
  StackOption<bool> TopLevelKeep(
      "top-level-keep", cl::sub(*cl::TopLevelSubCommand), cl::init(false));

  const char *args[] = {"prog", "-top-level-remove"};

  EXPECT_FALSE(TopLevelRemove);
  EXPECT_TRUE(
      cl::ParseCommandLineOptions(2, args, StringRef(), &llvm::nulls()));
  EXPECT_TRUE(TopLevelRemove);

  TopLevelRemove.removeArgument();

  cl::ResetAllOptionOccurrences();
  EXPECT_FALSE(
      cl::ParseCommandLineOptions(2, args, StringRef(), &llvm::nulls()));
}

TEST(CommandLineTest, RemoveFromAllSubCommands) {
  cl::ResetCommandLineParser();

  StackSubCommand SC1("sc1", "First Subcommand");
  StackSubCommand SC2("sc2", "Second Subcommand");
  StackOption<bool> RemoveOption("remove-option", cl::sub(*cl::AllSubCommands),
                                 cl::init(false));
  StackOption<bool> KeepOption("keep-option", cl::sub(*cl::AllSubCommands),
                               cl::init(false));

  const char *args0[] = {"prog", "-remove-option"};
  const char *args1[] = {"prog", "sc1", "-remove-option"};
  const char *args2[] = {"prog", "sc2", "-remove-option"};

  // It should work for all subcommands including the top-level.
  EXPECT_FALSE(RemoveOption);
  EXPECT_TRUE(
      cl::ParseCommandLineOptions(2, args0, StringRef(), &llvm::nulls()));
  EXPECT_TRUE(RemoveOption);

  RemoveOption = false;

  cl::ResetAllOptionOccurrences();
  EXPECT_FALSE(RemoveOption);
  EXPECT_TRUE(
      cl::ParseCommandLineOptions(3, args1, StringRef(), &llvm::nulls()));
  EXPECT_TRUE(RemoveOption);

  RemoveOption = false;

  cl::ResetAllOptionOccurrences();
  EXPECT_FALSE(RemoveOption);
  EXPECT_TRUE(
      cl::ParseCommandLineOptions(3, args2, StringRef(), &llvm::nulls()));
  EXPECT_TRUE(RemoveOption);

  RemoveOption.removeArgument();

  // It should not work for any subcommands including the top-level.
  cl::ResetAllOptionOccurrences();
  EXPECT_FALSE(
      cl::ParseCommandLineOptions(2, args0, StringRef(), &llvm::nulls()));
  cl::ResetAllOptionOccurrences();
  EXPECT_FALSE(
      cl::ParseCommandLineOptions(3, args1, StringRef(), &llvm::nulls()));
  cl::ResetAllOptionOccurrences();
  EXPECT_FALSE(
      cl::ParseCommandLineOptions(3, args2, StringRef(), &llvm::nulls()));
}

TEST(CommandLineTest, GetRegisteredSubcommands) {
  cl::ResetCommandLineParser();

  StackSubCommand SC1("sc1", "First Subcommand");
  StackOption<bool> Opt1("opt1", cl::sub(SC1), cl::init(false));
  StackSubCommand SC2("sc2", "Second subcommand");
  StackOption<bool> Opt2("opt2", cl::sub(SC2), cl::init(false));

  const char *args0[] = {"prog", "sc1"};
  const char *args1[] = {"prog", "sc2"};

  EXPECT_TRUE(
      cl::ParseCommandLineOptions(2, args0, StringRef(), &llvm::nulls()));
  EXPECT_FALSE(Opt1);
  EXPECT_FALSE(Opt2);
  for (auto *S : cl::getRegisteredSubcommands()) {
    if (*S) {
      EXPECT_EQ("sc1", S->getName());
    }
  }

  cl::ResetAllOptionOccurrences();
  EXPECT_TRUE(
      cl::ParseCommandLineOptions(2, args1, StringRef(), &llvm::nulls()));
  EXPECT_FALSE(Opt1);
  EXPECT_FALSE(Opt2);
  for (auto *S : cl::getRegisteredSubcommands()) {
    if (*S) {
      EXPECT_EQ("sc2", S->getName());
    }
  }
}

TEST(CommandLineTest, ArgumentLimit) {
  std::string args(32 * 4096, 'a');
  EXPECT_FALSE(llvm::sys::commandLineFitsWithinSystemLimits("cl", args.data()));
}

TEST(CommandLineTest, ResponseFiles) {
  llvm::SmallString<128> TestDir;
  std::error_code EC =
    llvm::sys::fs::createUniqueDirectory("unittest", TestDir);
  EXPECT_TRUE(!EC);

  // Create included response file of first level.
  llvm::SmallString<128> IncludedFileName;
  llvm::sys::path::append(IncludedFileName, TestDir, "resp1");
  std::ofstream IncludedFile(IncludedFileName.c_str());
  EXPECT_TRUE(IncludedFile.is_open());
  IncludedFile << "-option_1 -option_2\n"
                  "@incdir/resp2\n"
                  "-option_3=abcd\n";
  IncludedFile.close();

  // Directory for included file.
  llvm::SmallString<128> IncDir;
  llvm::sys::path::append(IncDir, TestDir, "incdir");
  EC = llvm::sys::fs::create_directory(IncDir);
  EXPECT_TRUE(!EC);

  // Create included response file of second level.
  llvm::SmallString<128> IncludedFileName2;
  llvm::sys::path::append(IncludedFileName2, IncDir, "resp2");
  std::ofstream IncludedFile2(IncludedFileName2.c_str());
  EXPECT_TRUE(IncludedFile2.is_open());
  IncludedFile2 << "-option_21 -option_22\n";
  IncludedFile2 << "-option_23=abcd\n";
  IncludedFile2.close();

  // Prepare 'file' with reference to response file.
  SmallString<128> IncRef;
  IncRef.append(1, '@');
  IncRef.append(IncludedFileName.c_str());
  llvm::SmallVector<const char *, 4> Argv =
                          { "test/test", "-flag_1", IncRef.c_str(), "-flag_2" };

  // Expand response files.
  llvm::BumpPtrAllocator A;
  llvm::StringSaver Saver(A);
  bool Res = llvm::cl::ExpandResponseFiles(
                    Saver, llvm::cl::TokenizeGNUCommandLine, Argv, false, true);
  EXPECT_TRUE(Res);
  EXPECT_EQ(Argv.size(), 9U);
  EXPECT_STREQ(Argv[0], "test/test");
  EXPECT_STREQ(Argv[1], "-flag_1");
  EXPECT_STREQ(Argv[2], "-option_1");
  EXPECT_STREQ(Argv[3], "-option_2");
  EXPECT_STREQ(Argv[4], "-option_21");
  EXPECT_STREQ(Argv[5], "-option_22");
  EXPECT_STREQ(Argv[6], "-option_23=abcd");
  EXPECT_STREQ(Argv[7], "-option_3=abcd");
  EXPECT_STREQ(Argv[8], "-flag_2");

  llvm::sys::fs::remove(IncludedFileName2);
  llvm::sys::fs::remove(IncDir);
  llvm::sys::fs::remove(IncludedFileName);
  llvm::sys::fs::remove(TestDir);
}

}  // anonymous namespace
