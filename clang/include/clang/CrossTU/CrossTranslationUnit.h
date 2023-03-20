//===--- CrossTranslationUnit.h - -------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
//  This file provides an interface to load binary AST dumps on demand. This
//  feature can be utilized for tools that require cross translation unit
//  support.
//
//===----------------------------------------------------------------------===//
#ifndef LLVM_CLANG_CROSSTU_CROSSTRANSLATIONUNIT_H
#define LLVM_CLANG_CROSSTU_CROSSTRANSLATIONUNIT_H

#include "clang/AST/ASTImporterSharedState.h"
#include "clang/Basic/LLVM.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/Support/Error.h"

namespace clang {
class CompilerInstance;
class ASTContext;
class ASTImporter;
class ASTUnit;
class DeclContext;
class FunctionDecl;
class VarDecl;
class NamedDecl;
class TranslationUnitDecl;

namespace cross_tu {

enum class index_error_code {
  unspecified = 1,
  missing_index_file,
  invalid_index_format,
  multiple_definitions,
  missing_definition,
  failed_import,
  failed_to_get_external_ast,
  failed_to_generate_usr,
  triple_mismatch,
  lang_mismatch,
  lang_dialect_mismatch,
  load_threshold_reached
};

class IndexError : public llvm::ErrorInfo<IndexError> {
public:
  static char ID;
  IndexError(index_error_code C) : Code(C), LineNo(0) {}
  IndexError(index_error_code C, std::string FileName, int LineNo = 0)
      : Code(C), FileName(std::move(FileName)), LineNo(LineNo) {}
  IndexError(index_error_code C, std::string FileName, std::string TripleToName,
             std::string TripleFromName)
      : Code(C), FileName(std::move(FileName)),
        TripleToName(std::move(TripleToName)),
        TripleFromName(std::move(TripleFromName)) {}
  void log(raw_ostream &OS) const override;
  std::error_code convertToErrorCode() const override;
  index_error_code getCode() const { return Code; }
  int getLineNum() const { return LineNo; }
  std::string getFileName() const { return FileName; }
  std::string getTripleToName() const { return TripleToName; }
  std::string getTripleFromName() const { return TripleFromName; }

private:
  index_error_code Code;
  std::string FileName;
  int LineNo;
  std::string TripleToName;
  std::string TripleFromName;
};

/// This function parses an index file that determines which
///        translation unit contains which definition.
///
/// The index file format is the following:
/// each line consists of an USR and a filepath separated by a space.
///
/// \return Returns a map where the USR is the key and the filepath is the value
///         or an error.
llvm::Expected<llvm::StringMap<std::string>>
parseCrossTUIndex(StringRef IndexPath, StringRef CrossTUDir);

std::string createCrossTUIndexString(const llvm::StringMap<std::string> &Index);

// Returns true if the variable or any field of a record variable is const.
bool containsConst(const VarDecl *VD, const ASTContext &ACtx);

/// This class is used for tools that requires cross translation
///        unit capability.
///
/// This class can load definitions from external AST files.
/// The loaded definition will be merged back to the original AST using the
/// AST Importer.
/// In order to use this class, an index file is required that describes
/// the locations of the AST files for each definition.
///
/// Note that this class also implements caching.
class CrossTranslationUnitContext {
public:
  CrossTranslationUnitContext(CompilerInstance &CI);
  ~CrossTranslationUnitContext();

  /// This function loads a function or variable definition from an
  ///        external AST file and merges it into the original AST.
  ///
  /// This method should only be used on functions that have no definitions or
  /// variables that have no initializer in
  /// the current translation unit. A function definition with the same
  /// declaration will be looked up in the index file which should be in the
  /// \p CrossTUDir directory, called \p IndexName. In case the declaration is
  /// found in the index the corresponding AST file will be loaded and the
  /// definition will be merged into the original AST using the AST Importer.
  ///
  /// \return The declaration with the definition will be returned.
  /// If no suitable definition is found in the index file or multiple
  /// definitions found error will be returned.
  ///
  /// Note that the AST files should also be in the \p CrossTUDir.
  llvm::Expected<const FunctionDecl *>
  getCrossTUDefinition(const FunctionDecl *FD, StringRef CrossTUDir,
                       StringRef IndexName, bool DisplayCTUProgress = false);
  llvm::Expected<const VarDecl *>
  getCrossTUDefinition(const VarDecl *VD, StringRef CrossTUDir,
                       StringRef IndexName, bool DisplayCTUProgress = false);

  /// This function loads a definition from an external AST file.
  ///
  /// A definition with the same declaration will be looked up in the
  /// index file which should be in the \p CrossTUDir directory, called
  /// \p IndexName. In case the declaration is found in the index the
  /// corresponding AST file will be loaded. If the number of TUs imported
  /// reaches \p CTULoadTreshold, no loading is performed.
  ///
  /// \return Returns a pointer to the ASTUnit that contains the definition of
  /// the looked up name or an Error.
  /// The returned pointer is never a nullptr.
  ///
  /// Note that the AST files should also be in the \p CrossTUDir.
  llvm::Expected<ASTUnit *> loadExternalAST(StringRef LookupName,
                                            StringRef CrossTUDir,
                                            StringRef IndexName,
                                            bool DisplayCTUProgress = false);

  /// This function merges a definition from a separate AST Unit into
  ///        the current one which was created by the compiler instance that
  ///        was passed to the constructor.
  ///
  /// \return Returns the resulting definition or an error.
  llvm::Expected<const FunctionDecl *> importDefinition(const FunctionDecl *FD);
  llvm::Expected<const VarDecl *> importDefinition(const VarDecl *VD);

  /// Get a name to identify a named decl.
  static std::string getLookupName(const NamedDecl *ND);

  /// Emit diagnostics for the user for potential configuration errors.
  void emitCrossTUDiagnostics(const IndexError &IE);

private:
  void lazyInitImporterSharedSt(TranslationUnitDecl *ToTU);
  ASTImporter &getOrCreateASTImporter(ASTContext &From);
  template <typename T>
  llvm::Expected<const T *> getCrossTUDefinitionImpl(const T *D,
                                                     StringRef CrossTUDir,
                                                     StringRef IndexName,
                                                     bool DisplayCTUProgress);
  template <typename T>
  const T *findDefInDeclContext(const DeclContext *DC,
                                StringRef LookupName);
  template <typename T>
  llvm::Expected<const T *> importDefinitionImpl(const T *D);

  llvm::StringMap<std::unique_ptr<clang::ASTUnit>> FileASTUnitMap;
  llvm::StringMap<clang::ASTUnit *> NameASTUnitMap;
  llvm::StringMap<std::string> NameFileMap;
  llvm::DenseMap<TranslationUnitDecl *, std::unique_ptr<ASTImporter>>
      ASTUnitImporterMap;
  CompilerInstance &CI;
  ASTContext &Context;
  std::shared_ptr<ASTImporterSharedState> ImporterSharedSt;
  /// \p CTULoadTreshold should serve as an upper limit to the number of TUs
  /// imported in order to reduce the memory footprint of CTU analysis.
  const unsigned CTULoadThreshold;
  unsigned NumASTLoaded{0u};
};

} // namespace cross_tu
} // namespace clang

#endif // LLVM_CLANG_CROSSTU_CROSSTRANSLATIONUNIT_H
