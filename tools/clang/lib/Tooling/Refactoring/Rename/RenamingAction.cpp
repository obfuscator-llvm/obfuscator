//===--- RenamingAction.cpp - Clang refactoring library -------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
///
/// \file
/// \brief Provides an action to rename every symbol at a point.
///
//===----------------------------------------------------------------------===//

#include "clang/Tooling/Refactoring/Rename/RenamingAction.h"
#include "clang/AST/ASTConsumer.h"
#include "clang/AST/ASTContext.h"
#include "clang/Basic/FileManager.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/FrontendAction.h"
#include "clang/Lex/Lexer.h"
#include "clang/Lex/Preprocessor.h"
#include "clang/Tooling/CommonOptionsParser.h"
#include "clang/Tooling/Refactoring.h"
#include "clang/Tooling/Refactoring/Rename/USRLocFinder.h"
#include "clang/Tooling/Tooling.h"
#include <string>
#include <vector>

using namespace llvm;

namespace clang {
namespace tooling {

class RenamingASTConsumer : public ASTConsumer {
public:
  RenamingASTConsumer(
      const std::vector<std::string> &NewNames,
      const std::vector<std::string> &PrevNames,
      const std::vector<std::vector<std::string>> &USRList,
      std::map<std::string, tooling::Replacements> &FileToReplaces,
      bool PrintLocations)
      : NewNames(NewNames), PrevNames(PrevNames), USRList(USRList),
        FileToReplaces(FileToReplaces), PrintLocations(PrintLocations) {}

  void HandleTranslationUnit(ASTContext &Context) override {
    for (unsigned I = 0; I < NewNames.size(); ++I)
      HandleOneRename(Context, NewNames[I], PrevNames[I], USRList[I]);
  }

  void HandleOneRename(ASTContext &Context, const std::string &NewName,
                       const std::string &PrevName,
                       const std::vector<std::string> &USRs) {
    const SourceManager &SourceMgr = Context.getSourceManager();
    std::vector<SourceLocation> RenamingCandidates;
    std::vector<SourceLocation> NewCandidates;

    NewCandidates = tooling::getLocationsOfUSRs(
        USRs, PrevName, Context.getTranslationUnitDecl());
    RenamingCandidates.insert(RenamingCandidates.end(), NewCandidates.begin(),
                              NewCandidates.end());

    unsigned PrevNameLen = PrevName.length();
    for (const auto &Loc : RenamingCandidates) {
      if (PrintLocations) {
        FullSourceLoc FullLoc(Loc, SourceMgr);
        errs() << "clang-rename: renamed at: " << SourceMgr.getFilename(Loc)
               << ":" << FullLoc.getSpellingLineNumber() << ":"
               << FullLoc.getSpellingColumnNumber() << "\n";
      }
      // FIXME: better error handling.
      tooling::Replacement Replace(SourceMgr, Loc, PrevNameLen, NewName);
      llvm::Error Err = FileToReplaces[Replace.getFilePath()].add(Replace);
      if (Err)
        llvm::errs() << "Renaming failed in " << Replace.getFilePath() << "! "
                     << llvm::toString(std::move(Err)) << "\n";
    }
  }

private:
  const std::vector<std::string> &NewNames, &PrevNames;
  const std::vector<std::vector<std::string>> &USRList;
  std::map<std::string, tooling::Replacements> &FileToReplaces;
  bool PrintLocations;
};

// A renamer to rename symbols which are identified by a give USRList to
// new name.
//
// FIXME: Merge with the above RenamingASTConsumer.
class USRSymbolRenamer : public ASTConsumer {
public:
  USRSymbolRenamer(const std::vector<std::string> &NewNames,
                   const std::vector<std::vector<std::string>> &USRList,
                   std::map<std::string, tooling::Replacements> &FileToReplaces)
      : NewNames(NewNames), USRList(USRList), FileToReplaces(FileToReplaces) {
    assert(USRList.size() == NewNames.size());
  }

  void HandleTranslationUnit(ASTContext &Context) override {
    for (unsigned I = 0; I < NewNames.size(); ++I) {
      // FIXME: Apply AtomicChanges directly once the refactoring APIs are
      // ready.
      auto AtomicChanges = tooling::createRenameAtomicChanges(
          USRList[I], NewNames[I], Context.getTranslationUnitDecl());
      for (const auto AtomicChange : AtomicChanges) {
        for (const auto &Replace : AtomicChange.getReplacements()) {
          llvm::Error Err = FileToReplaces[Replace.getFilePath()].add(Replace);
          if (Err) {
            llvm::errs() << "Renaming failed in " << Replace.getFilePath()
                         << "! " << llvm::toString(std::move(Err)) << "\n";
          }
        }
      }
    }
  }

private:
  const std::vector<std::string> &NewNames;
  const std::vector<std::vector<std::string>> &USRList;
  std::map<std::string, tooling::Replacements> &FileToReplaces;
};

std::unique_ptr<ASTConsumer> RenamingAction::newASTConsumer() {
  return llvm::make_unique<RenamingASTConsumer>(NewNames, PrevNames, USRList,
                                                FileToReplaces, PrintLocations);
}

std::unique_ptr<ASTConsumer> QualifiedRenamingAction::newASTConsumer() {
  return llvm::make_unique<USRSymbolRenamer>(NewNames, USRList, FileToReplaces);
}

} // end namespace tooling
} // end namespace clang
