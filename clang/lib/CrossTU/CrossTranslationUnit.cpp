//===--- CrossTranslationUnit.cpp - -----------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
//  This file implements the CrossTranslationUnit interface.
//
//===----------------------------------------------------------------------===//
#include "clang/CrossTU/CrossTranslationUnit.h"
#include "clang/AST/ASTImporter.h"
#include "clang/AST/Decl.h"
#include "clang/Basic/TargetInfo.h"
#include "clang/CrossTU/CrossTUDiagnostic.h"
#include "clang/Frontend/ASTUnit.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/TextDiagnosticPrinter.h"
#include "clang/Index/USRGeneration.h"
#include "llvm/ADT/Triple.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/ManagedStatic.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"
#include <fstream>
#include <sstream>

namespace clang {
namespace cross_tu {

namespace {

#define DEBUG_TYPE "CrossTranslationUnit"
STATISTIC(NumGetCTUCalled, "The # of getCTUDefinition function called");
STATISTIC(
    NumNotInOtherTU,
    "The # of getCTUDefinition called but the function is not in any other TU");
STATISTIC(NumGetCTUSuccess,
          "The # of getCTUDefinition successfully returned the "
          "requested function's body");
STATISTIC(NumUnsupportedNodeFound, "The # of imports when the ASTImporter "
                                   "encountered an unsupported AST Node");
STATISTIC(NumNameConflicts, "The # of imports when the ASTImporter "
                            "encountered an ODR error");
STATISTIC(NumTripleMismatch, "The # of triple mismatches");
STATISTIC(NumLangMismatch, "The # of language mismatches");
STATISTIC(NumLangDialectMismatch, "The # of language dialect mismatches");
STATISTIC(NumASTLoadThresholdReached,
          "The # of ASTs not loaded because of threshold");

// Same as Triple's equality operator, but we check a field only if that is
// known in both instances.
bool hasEqualKnownFields(const llvm::Triple &Lhs, const llvm::Triple &Rhs) {
  using llvm::Triple;
  if (Lhs.getArch() != Triple::UnknownArch &&
      Rhs.getArch() != Triple::UnknownArch && Lhs.getArch() != Rhs.getArch())
    return false;
  if (Lhs.getSubArch() != Triple::NoSubArch &&
      Rhs.getSubArch() != Triple::NoSubArch &&
      Lhs.getSubArch() != Rhs.getSubArch())
    return false;
  if (Lhs.getVendor() != Triple::UnknownVendor &&
      Rhs.getVendor() != Triple::UnknownVendor &&
      Lhs.getVendor() != Rhs.getVendor())
    return false;
  if (!Lhs.isOSUnknown() && !Rhs.isOSUnknown() &&
      Lhs.getOS() != Rhs.getOS())
    return false;
  if (Lhs.getEnvironment() != Triple::UnknownEnvironment &&
      Rhs.getEnvironment() != Triple::UnknownEnvironment &&
      Lhs.getEnvironment() != Rhs.getEnvironment())
    return false;
  if (Lhs.getObjectFormat() != Triple::UnknownObjectFormat &&
      Rhs.getObjectFormat() != Triple::UnknownObjectFormat &&
      Lhs.getObjectFormat() != Rhs.getObjectFormat())
    return false;
  return true;
}

// FIXME: This class is will be removed after the transition to llvm::Error.
class IndexErrorCategory : public std::error_category {
public:
  const char *name() const noexcept override { return "clang.index"; }

  std::string message(int Condition) const override {
    switch (static_cast<index_error_code>(Condition)) {
    case index_error_code::unspecified:
      return "An unknown error has occurred.";
    case index_error_code::missing_index_file:
      return "The index file is missing.";
    case index_error_code::invalid_index_format:
      return "Invalid index file format.";
    case index_error_code::multiple_definitions:
      return "Multiple definitions in the index file.";
    case index_error_code::missing_definition:
      return "Missing definition from the index file.";
    case index_error_code::failed_import:
      return "Failed to import the definition.";
    case index_error_code::failed_to_get_external_ast:
      return "Failed to load external AST source.";
    case index_error_code::failed_to_generate_usr:
      return "Failed to generate USR.";
    case index_error_code::triple_mismatch:
      return "Triple mismatch";
    case index_error_code::lang_mismatch:
      return "Language mismatch";
    case index_error_code::lang_dialect_mismatch:
      return "Language dialect mismatch";
    case index_error_code::load_threshold_reached:
      return "Load threshold reached";
    }
    llvm_unreachable("Unrecognized index_error_code.");
  }
};

static llvm::ManagedStatic<IndexErrorCategory> Category;
} // end anonymous namespace

char IndexError::ID;

void IndexError::log(raw_ostream &OS) const {
  OS << Category->message(static_cast<int>(Code)) << '\n';
}

std::error_code IndexError::convertToErrorCode() const {
  return std::error_code(static_cast<int>(Code), *Category);
}

llvm::Expected<llvm::StringMap<std::string>>
parseCrossTUIndex(StringRef IndexPath, StringRef CrossTUDir) {
  std::ifstream ExternalMapFile(IndexPath);
  if (!ExternalMapFile)
    return llvm::make_error<IndexError>(index_error_code::missing_index_file,
                                        IndexPath.str());

  llvm::StringMap<std::string> Result;
  std::string Line;
  unsigned LineNo = 1;
  while (std::getline(ExternalMapFile, Line)) {
    const size_t Pos = Line.find(" ");
    if (Pos > 0 && Pos != std::string::npos) {
      StringRef LineRef{Line};
      StringRef LookupName = LineRef.substr(0, Pos);
      if (Result.count(LookupName))
        return llvm::make_error<IndexError>(
            index_error_code::multiple_definitions, IndexPath.str(), LineNo);
      StringRef FileName = LineRef.substr(Pos + 1);
      SmallString<256> FilePath = CrossTUDir;
      llvm::sys::path::append(FilePath, FileName);
      Result[LookupName] = FilePath.str().str();
    } else
      return llvm::make_error<IndexError>(
          index_error_code::invalid_index_format, IndexPath.str(), LineNo);
    LineNo++;
  }
  return Result;
}

std::string
createCrossTUIndexString(const llvm::StringMap<std::string> &Index) {
  std::ostringstream Result;
  for (const auto &E : Index)
    Result << E.getKey().str() << " " << E.getValue() << '\n';
  return Result.str();
}

bool containsConst(const VarDecl *VD, const ASTContext &ACtx) {
  CanQualType CT = ACtx.getCanonicalType(VD->getType());
  if (!CT.isConstQualified()) {
    const RecordType *RTy = CT->getAs<RecordType>();
    if (!RTy || !RTy->hasConstFields())
      return false;
  }
  return true;
}

static bool hasBodyOrInit(const FunctionDecl *D, const FunctionDecl *&DefD) {
  return D->hasBody(DefD);
}
static bool hasBodyOrInit(const VarDecl *D, const VarDecl *&DefD) {
  return D->getAnyInitializer(DefD);
}
template <typename T> static bool hasBodyOrInit(const T *D) {
  const T *Unused;
  return hasBodyOrInit(D, Unused);
}

CrossTranslationUnitContext::CrossTranslationUnitContext(CompilerInstance &CI)
    : CI(CI), Context(CI.getASTContext()),
      CTULoadThreshold(CI.getAnalyzerOpts()->CTUImportThreshold) {}

CrossTranslationUnitContext::~CrossTranslationUnitContext() {}

std::string CrossTranslationUnitContext::getLookupName(const NamedDecl *ND) {
  SmallString<128> DeclUSR;
  bool Ret = index::generateUSRForDecl(ND, DeclUSR);
  (void)Ret;
  assert(!Ret && "Unable to generate USR");
  return DeclUSR.str();
}

/// Recursively visits the decls of a DeclContext, and returns one with the
/// given USR.
template <typename T>
const T *
CrossTranslationUnitContext::findDefInDeclContext(const DeclContext *DC,
                                                  StringRef LookupName) {
  assert(DC && "Declaration Context must not be null");
  for (const Decl *D : DC->decls()) {
    const auto *SubDC = dyn_cast<DeclContext>(D);
    if (SubDC)
      if (const auto *ND = findDefInDeclContext<T>(SubDC, LookupName))
        return ND;

    const auto *ND = dyn_cast<T>(D);
    const T *ResultDecl;
    if (!ND || !hasBodyOrInit(ND, ResultDecl))
      continue;
    if (getLookupName(ResultDecl) != LookupName)
      continue;
    return ResultDecl;
  }
  return nullptr;
}

template <typename T>
llvm::Expected<const T *> CrossTranslationUnitContext::getCrossTUDefinitionImpl(
    const T *D, StringRef CrossTUDir, StringRef IndexName,
    bool DisplayCTUProgress) {
  assert(D && "D is missing, bad call to this function!");
  assert(!hasBodyOrInit(D) &&
         "D has a body or init in current translation unit!");
  ++NumGetCTUCalled;
  const std::string LookupName = getLookupName(D);
  if (LookupName.empty())
    return llvm::make_error<IndexError>(
        index_error_code::failed_to_generate_usr);
  llvm::Expected<ASTUnit *> ASTUnitOrError = loadExternalAST(
      LookupName, CrossTUDir, IndexName, DisplayCTUProgress);
  if (!ASTUnitOrError)
    return ASTUnitOrError.takeError();
  ASTUnit *Unit = *ASTUnitOrError;
  assert(&Unit->getFileManager() ==
         &Unit->getASTContext().getSourceManager().getFileManager());

  const llvm::Triple &TripleTo = Context.getTargetInfo().getTriple();
  const llvm::Triple &TripleFrom =
      Unit->getASTContext().getTargetInfo().getTriple();
  // The imported AST had been generated for a different target.
  // Some parts of the triple in the loaded ASTContext can be unknown while the
  // very same parts in the target ASTContext are known. Thus we check for the
  // known parts only.
  if (!hasEqualKnownFields(TripleTo, TripleFrom)) {
    // TODO: Pass the SourceLocation of the CallExpression for more precise
    // diagnostics.
    ++NumTripleMismatch;
    return llvm::make_error<IndexError>(index_error_code::triple_mismatch,
                                        Unit->getMainFileName(), TripleTo.str(),
                                        TripleFrom.str());
  }

  const auto &LangTo = Context.getLangOpts();
  const auto &LangFrom = Unit->getASTContext().getLangOpts();

  // FIXME: Currenty we do not support CTU across C++ and C and across
  // different dialects of C++.
  if (LangTo.CPlusPlus != LangFrom.CPlusPlus) {
    ++NumLangMismatch;
    return llvm::make_error<IndexError>(index_error_code::lang_mismatch);
  }

  // If CPP dialects are different then return with error.
  //
  // Consider this STL code:
  //   template<typename _Alloc>
  //     struct __alloc_traits
  //   #if __cplusplus >= 201103L
  //     : std::allocator_traits<_Alloc>
  //   #endif
  //     { // ...
  //     };
  // This class template would create ODR errors during merging the two units,
  // since in one translation unit the class template has a base class, however
  // in the other unit it has none.
  if (LangTo.CPlusPlus11 != LangFrom.CPlusPlus11 ||
      LangTo.CPlusPlus14 != LangFrom.CPlusPlus14 ||
      LangTo.CPlusPlus17 != LangFrom.CPlusPlus17 ||
      LangTo.CPlusPlus2a != LangFrom.CPlusPlus2a) {
    ++NumLangDialectMismatch;
    return llvm::make_error<IndexError>(
        index_error_code::lang_dialect_mismatch);
  }

  TranslationUnitDecl *TU = Unit->getASTContext().getTranslationUnitDecl();
  if (const T *ResultDecl = findDefInDeclContext<T>(TU, LookupName))
    return importDefinition(ResultDecl);
  return llvm::make_error<IndexError>(index_error_code::failed_import);
}

llvm::Expected<const FunctionDecl *>
CrossTranslationUnitContext::getCrossTUDefinition(const FunctionDecl *FD,
                                                  StringRef CrossTUDir,
                                                  StringRef IndexName,
                                                  bool DisplayCTUProgress) {
  return getCrossTUDefinitionImpl(FD, CrossTUDir, IndexName,
                                  DisplayCTUProgress);
}

llvm::Expected<const VarDecl *>
CrossTranslationUnitContext::getCrossTUDefinition(const VarDecl *VD,
                                                  StringRef CrossTUDir,
                                                  StringRef IndexName,
                                                  bool DisplayCTUProgress) {
  return getCrossTUDefinitionImpl(VD, CrossTUDir, IndexName,
                                  DisplayCTUProgress);
}

void CrossTranslationUnitContext::emitCrossTUDiagnostics(const IndexError &IE) {
  switch (IE.getCode()) {
  case index_error_code::missing_index_file:
    Context.getDiagnostics().Report(diag::err_ctu_error_opening)
        << IE.getFileName();
    break;
  case index_error_code::invalid_index_format:
    Context.getDiagnostics().Report(diag::err_extdefmap_parsing)
        << IE.getFileName() << IE.getLineNum();
    break;
  case index_error_code::multiple_definitions:
    Context.getDiagnostics().Report(diag::err_multiple_def_index)
        << IE.getLineNum();
    break;
  case index_error_code::triple_mismatch:
    Context.getDiagnostics().Report(diag::warn_ctu_incompat_triple)
        << IE.getFileName() << IE.getTripleToName() << IE.getTripleFromName();
    break;
  default:
    break;
  }
}

llvm::Expected<ASTUnit *> CrossTranslationUnitContext::loadExternalAST(
    StringRef LookupName, StringRef CrossTUDir, StringRef IndexName,
    bool DisplayCTUProgress) {
  // FIXME: The current implementation only supports loading decls with
  //        a lookup name from a single translation unit. If multiple
  //        translation units contains decls with the same lookup name an
  //        error will be returned.

  if (NumASTLoaded >= CTULoadThreshold) {
    ++NumASTLoadThresholdReached;
    return llvm::make_error<IndexError>(
        index_error_code::load_threshold_reached);
  }

  ASTUnit *Unit = nullptr;
  auto NameUnitCacheEntry = NameASTUnitMap.find(LookupName);
  if (NameUnitCacheEntry == NameASTUnitMap.end()) {
    if (NameFileMap.empty()) {
      SmallString<256> IndexFile = CrossTUDir;
      if (llvm::sys::path::is_absolute(IndexName))
        IndexFile = IndexName;
      else
        llvm::sys::path::append(IndexFile, IndexName);
      llvm::Expected<llvm::StringMap<std::string>> IndexOrErr =
          parseCrossTUIndex(IndexFile, CrossTUDir);
      if (IndexOrErr)
        NameFileMap = *IndexOrErr;
      else
        return IndexOrErr.takeError();
    }

    auto It = NameFileMap.find(LookupName);
    if (It == NameFileMap.end()) {
      ++NumNotInOtherTU;
      return llvm::make_error<IndexError>(index_error_code::missing_definition);
    }
    StringRef ASTFileName = It->second;
    auto ASTCacheEntry = FileASTUnitMap.find(ASTFileName);
    if (ASTCacheEntry == FileASTUnitMap.end()) {
      IntrusiveRefCntPtr<DiagnosticOptions> DiagOpts = new DiagnosticOptions();
      TextDiagnosticPrinter *DiagClient =
          new TextDiagnosticPrinter(llvm::errs(), &*DiagOpts);
      IntrusiveRefCntPtr<DiagnosticIDs> DiagID(new DiagnosticIDs());
      IntrusiveRefCntPtr<DiagnosticsEngine> Diags(
          new DiagnosticsEngine(DiagID, &*DiagOpts, DiagClient));

      std::unique_ptr<ASTUnit> LoadedUnit(ASTUnit::LoadFromASTFile(
          ASTFileName, CI.getPCHContainerOperations()->getRawReader(),
          ASTUnit::LoadEverything, Diags, CI.getFileSystemOpts()));
      Unit = LoadedUnit.get();
      FileASTUnitMap[ASTFileName] = std::move(LoadedUnit);
      ++NumASTLoaded;
      if (DisplayCTUProgress) {
        llvm::errs() << "CTU loaded AST file: "
                     << ASTFileName << "\n";
      }
    } else {
      Unit = ASTCacheEntry->second.get();
    }
    NameASTUnitMap[LookupName] = Unit;
  } else {
    Unit = NameUnitCacheEntry->second;
  }
  if (!Unit)
    return llvm::make_error<IndexError>(
        index_error_code::failed_to_get_external_ast);
  return Unit;
}

template <typename T>
llvm::Expected<const T *>
CrossTranslationUnitContext::importDefinitionImpl(const T *D) {
  assert(hasBodyOrInit(D) && "Decls to be imported should have body or init.");

  ASTImporter &Importer = getOrCreateASTImporter(D->getASTContext());
  auto ToDeclOrError = Importer.Import(D);
  if (!ToDeclOrError) {
    handleAllErrors(ToDeclOrError.takeError(),
                    [&](const ImportError &IE) {
                      switch (IE.Error) {
                      case ImportError::NameConflict:
                        ++NumNameConflicts;
                         break;
                      case ImportError::UnsupportedConstruct:
                        ++NumUnsupportedNodeFound;
                        break;
                      case ImportError::Unknown:
                        llvm_unreachable("Unknown import error happened.");
                        break;
                      }
                    });
    return llvm::make_error<IndexError>(index_error_code::failed_import);
  }
  auto *ToDecl = cast<T>(*ToDeclOrError);
  assert(hasBodyOrInit(ToDecl) && "Imported Decl should have body or init.");
  ++NumGetCTUSuccess;

  return ToDecl;
}

llvm::Expected<const FunctionDecl *>
CrossTranslationUnitContext::importDefinition(const FunctionDecl *FD) {
  return importDefinitionImpl(FD);
}

llvm::Expected<const VarDecl *>
CrossTranslationUnitContext::importDefinition(const VarDecl *VD) {
  return importDefinitionImpl(VD);
}

void CrossTranslationUnitContext::lazyInitImporterSharedSt(
    TranslationUnitDecl *ToTU) {
  if (!ImporterSharedSt)
    ImporterSharedSt = std::make_shared<ASTImporterSharedState>(*ToTU);
}

ASTImporter &
CrossTranslationUnitContext::getOrCreateASTImporter(ASTContext &From) {
  auto I = ASTUnitImporterMap.find(From.getTranslationUnitDecl());
  if (I != ASTUnitImporterMap.end())
    return *I->second;
  lazyInitImporterSharedSt(Context.getTranslationUnitDecl());
  ASTImporter *NewImporter = new ASTImporter(
      Context, Context.getSourceManager().getFileManager(), From,
      From.getSourceManager().getFileManager(), false, ImporterSharedSt);
  ASTUnitImporterMap[From.getTranslationUnitDecl()].reset(NewImporter);
  return *NewImporter;
}

} // namespace cross_tu
} // namespace clang
