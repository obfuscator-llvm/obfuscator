//===- llvm/lib/CodeGen/AsmPrinter/CodeViewDebug.cpp ----------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file contains support for writing Microsoft CodeView debug info.
//
//===----------------------------------------------------------------------===//

#include "CodeViewDebug.h"
#include "llvm/ADT/APSInt.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/MapVector.h"
#include "llvm/ADT/None.h"
#include "llvm/ADT/Optional.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/TinyPtrVector.h"
#include "llvm/ADT/Triple.h"
#include "llvm/ADT/Twine.h"
#include "llvm/BinaryFormat/COFF.h"
#include "llvm/BinaryFormat/Dwarf.h"
#include "llvm/CodeGen/AsmPrinter.h"
#include "llvm/CodeGen/LexicalScopes.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineInstr.h"
#include "llvm/CodeGen/MachineModuleInfo.h"
#include "llvm/CodeGen/MachineOperand.h"
#include "llvm/Config/llvm-config.h"
#include "llvm/DebugInfo/CodeView/CVTypeVisitor.h"
#include "llvm/DebugInfo/CodeView/CodeView.h"
#include "llvm/DebugInfo/CodeView/DebugInlineeLinesSubsection.h"
#include "llvm/DebugInfo/CodeView/Line.h"
#include "llvm/DebugInfo/CodeView/SymbolRecord.h"
#include "llvm/DebugInfo/CodeView/TypeDumpVisitor.h"
#include "llvm/DebugInfo/CodeView/TypeIndex.h"
#include "llvm/DebugInfo/CodeView/TypeRecord.h"
#include "llvm/DebugInfo/CodeView/TypeTableCollection.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/IR/DebugLoc.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalValue.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Module.h"
#include "llvm/MC/MCAsmInfo.h"
#include "llvm/MC/MCContext.h"
#include "llvm/MC/MCSectionCOFF.h"
#include "llvm/MC/MCStreamer.h"
#include "llvm/MC/MCSymbol.h"
#include "llvm/Support/BinaryByteStream.h"
#include "llvm/Support/BinaryStreamReader.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/Compiler.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/ScopedPrinter.h"
#include "llvm/Support/SMLoc.h"
#include "llvm/Target/TargetFrameLowering.h"
#include "llvm/Target/TargetLoweringObjectFile.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Target/TargetRegisterInfo.h"
#include "llvm/Target/TargetSubtargetInfo.h"
#include <algorithm>
#include <cassert>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <string>
#include <utility>
#include <vector>

using namespace llvm;
using namespace llvm::codeview;

CodeViewDebug::CodeViewDebug(AsmPrinter *AP)
    : DebugHandlerBase(AP), OS(*Asm->OutStreamer), TypeTable(Allocator) {
  // If module doesn't have named metadata anchors or COFF debug section
  // is not available, skip any debug info related stuff.
  if (!MMI->getModule()->getNamedMetadata("llvm.dbg.cu") ||
      !AP->getObjFileLowering().getCOFFDebugSymbolsSection()) {
    Asm = nullptr;
    return;
  }

  // Tell MMI that we have debug info.
  MMI->setDebugInfoAvailability(true);
}

StringRef CodeViewDebug::getFullFilepath(const DIFile *File) {
  std::string &Filepath = FileToFilepathMap[File];
  if (!Filepath.empty())
    return Filepath;

  StringRef Dir = File->getDirectory(), Filename = File->getFilename();

  // Clang emits directory and relative filename info into the IR, but CodeView
  // operates on full paths.  We could change Clang to emit full paths too, but
  // that would increase the IR size and probably not needed for other users.
  // For now, just concatenate and canonicalize the path here.
  if (Filename.find(':') == 1)
    Filepath = Filename;
  else
    Filepath = (Dir + "\\" + Filename).str();

  // Canonicalize the path.  We have to do it textually because we may no longer
  // have access the file in the filesystem.
  // First, replace all slashes with backslashes.
  std::replace(Filepath.begin(), Filepath.end(), '/', '\\');

  // Remove all "\.\" with "\".
  size_t Cursor = 0;
  while ((Cursor = Filepath.find("\\.\\", Cursor)) != std::string::npos)
    Filepath.erase(Cursor, 2);

  // Replace all "\XXX\..\" with "\".  Don't try too hard though as the original
  // path should be well-formatted, e.g. start with a drive letter, etc.
  Cursor = 0;
  while ((Cursor = Filepath.find("\\..\\", Cursor)) != std::string::npos) {
    // Something's wrong if the path starts with "\..\", abort.
    if (Cursor == 0)
      break;

    size_t PrevSlash = Filepath.rfind('\\', Cursor - 1);
    if (PrevSlash == std::string::npos)
      // Something's wrong, abort.
      break;

    Filepath.erase(PrevSlash, Cursor + 3 - PrevSlash);
    // The next ".." might be following the one we've just erased.
    Cursor = PrevSlash;
  }

  // Remove all duplicate backslashes.
  Cursor = 0;
  while ((Cursor = Filepath.find("\\\\", Cursor)) != std::string::npos)
    Filepath.erase(Cursor, 1);

  return Filepath;
}

unsigned CodeViewDebug::maybeRecordFile(const DIFile *F) {
  unsigned NextId = FileIdMap.size() + 1;
  auto Insertion = FileIdMap.insert(std::make_pair(F, NextId));
  if (Insertion.second) {
    // We have to compute the full filepath and emit a .cv_file directive.
    StringRef FullPath = getFullFilepath(F);
    bool Success = OS.EmitCVFileDirective(NextId, FullPath);
    (void)Success;
    assert(Success && ".cv_file directive failed");
  }
  return Insertion.first->second;
}

CodeViewDebug::InlineSite &
CodeViewDebug::getInlineSite(const DILocation *InlinedAt,
                             const DISubprogram *Inlinee) {
  auto SiteInsertion = CurFn->InlineSites.insert({InlinedAt, InlineSite()});
  InlineSite *Site = &SiteInsertion.first->second;
  if (SiteInsertion.second) {
    unsigned ParentFuncId = CurFn->FuncId;
    if (const DILocation *OuterIA = InlinedAt->getInlinedAt())
      ParentFuncId =
          getInlineSite(OuterIA, InlinedAt->getScope()->getSubprogram())
              .SiteFuncId;

    Site->SiteFuncId = NextFuncId++;
    OS.EmitCVInlineSiteIdDirective(
        Site->SiteFuncId, ParentFuncId, maybeRecordFile(InlinedAt->getFile()),
        InlinedAt->getLine(), InlinedAt->getColumn(), SMLoc());
    Site->Inlinee = Inlinee;
    InlinedSubprograms.insert(Inlinee);
    getFuncIdForSubprogram(Inlinee);
  }
  return *Site;
}

static StringRef getPrettyScopeName(const DIScope *Scope) {
  StringRef ScopeName = Scope->getName();
  if (!ScopeName.empty())
    return ScopeName;

  switch (Scope->getTag()) {
  case dwarf::DW_TAG_enumeration_type:
  case dwarf::DW_TAG_class_type:
  case dwarf::DW_TAG_structure_type:
  case dwarf::DW_TAG_union_type:
    return "<unnamed-tag>";
  case dwarf::DW_TAG_namespace:
    return "`anonymous namespace'";
  }

  return StringRef();
}

static const DISubprogram *getQualifiedNameComponents(
    const DIScope *Scope, SmallVectorImpl<StringRef> &QualifiedNameComponents) {
  const DISubprogram *ClosestSubprogram = nullptr;
  while (Scope != nullptr) {
    if (ClosestSubprogram == nullptr)
      ClosestSubprogram = dyn_cast<DISubprogram>(Scope);
    StringRef ScopeName = getPrettyScopeName(Scope);
    if (!ScopeName.empty())
      QualifiedNameComponents.push_back(ScopeName);
    Scope = Scope->getScope().resolve();
  }
  return ClosestSubprogram;
}

static std::string getQualifiedName(ArrayRef<StringRef> QualifiedNameComponents,
                                    StringRef TypeName) {
  std::string FullyQualifiedName;
  for (StringRef QualifiedNameComponent :
       llvm::reverse(QualifiedNameComponents)) {
    FullyQualifiedName.append(QualifiedNameComponent);
    FullyQualifiedName.append("::");
  }
  FullyQualifiedName.append(TypeName);
  return FullyQualifiedName;
}

static std::string getFullyQualifiedName(const DIScope *Scope, StringRef Name) {
  SmallVector<StringRef, 5> QualifiedNameComponents;
  getQualifiedNameComponents(Scope, QualifiedNameComponents);
  return getQualifiedName(QualifiedNameComponents, Name);
}

struct CodeViewDebug::TypeLoweringScope {
  TypeLoweringScope(CodeViewDebug &CVD) : CVD(CVD) { ++CVD.TypeEmissionLevel; }
  ~TypeLoweringScope() {
    // Don't decrement TypeEmissionLevel until after emitting deferred types, so
    // inner TypeLoweringScopes don't attempt to emit deferred types.
    if (CVD.TypeEmissionLevel == 1)
      CVD.emitDeferredCompleteTypes();
    --CVD.TypeEmissionLevel;
  }
  CodeViewDebug &CVD;
};

static std::string getFullyQualifiedName(const DIScope *Ty) {
  const DIScope *Scope = Ty->getScope().resolve();
  return getFullyQualifiedName(Scope, getPrettyScopeName(Ty));
}

TypeIndex CodeViewDebug::getScopeIndex(const DIScope *Scope) {
  // No scope means global scope and that uses the zero index.
  if (!Scope || isa<DIFile>(Scope))
    return TypeIndex();

  assert(!isa<DIType>(Scope) && "shouldn't make a namespace scope for a type");

  // Check if we've already translated this scope.
  auto I = TypeIndices.find({Scope, nullptr});
  if (I != TypeIndices.end())
    return I->second;

  // Build the fully qualified name of the scope.
  std::string ScopeName = getFullyQualifiedName(Scope);
  StringIdRecord SID(TypeIndex(), ScopeName);
  auto TI = TypeTable.writeKnownType(SID);
  return recordTypeIndexForDINode(Scope, TI);
}

TypeIndex CodeViewDebug::getFuncIdForSubprogram(const DISubprogram *SP) {
  assert(SP);

  // Check if we've already translated this subprogram.
  auto I = TypeIndices.find({SP, nullptr});
  if (I != TypeIndices.end())
    return I->second;

  // The display name includes function template arguments. Drop them to match
  // MSVC.
  StringRef DisplayName = SP->getName().split('<').first;

  const DIScope *Scope = SP->getScope().resolve();
  TypeIndex TI;
  if (const auto *Class = dyn_cast_or_null<DICompositeType>(Scope)) {
    // If the scope is a DICompositeType, then this must be a method. Member
    // function types take some special handling, and require access to the
    // subprogram.
    TypeIndex ClassType = getTypeIndex(Class);
    MemberFuncIdRecord MFuncId(ClassType, getMemberFunctionType(SP, Class),
                               DisplayName);
    TI = TypeTable.writeKnownType(MFuncId);
  } else {
    // Otherwise, this must be a free function.
    TypeIndex ParentScope = getScopeIndex(Scope);
    FuncIdRecord FuncId(ParentScope, getTypeIndex(SP->getType()), DisplayName);
    TI = TypeTable.writeKnownType(FuncId);
  }

  return recordTypeIndexForDINode(SP, TI);
}

TypeIndex CodeViewDebug::getMemberFunctionType(const DISubprogram *SP,
                                               const DICompositeType *Class) {
  // Always use the method declaration as the key for the function type. The
  // method declaration contains the this adjustment.
  if (SP->getDeclaration())
    SP = SP->getDeclaration();
  assert(!SP->getDeclaration() && "should use declaration as key");

  // Key the MemberFunctionRecord into the map as {SP, Class}. It won't collide
  // with the MemberFuncIdRecord, which is keyed in as {SP, nullptr}.
  auto I = TypeIndices.find({SP, Class});
  if (I != TypeIndices.end())
    return I->second;

  // Make sure complete type info for the class is emitted *after* the member
  // function type, as the complete class type is likely to reference this
  // member function type.
  TypeLoweringScope S(*this);
  TypeIndex TI =
      lowerTypeMemberFunction(SP->getType(), Class, SP->getThisAdjustment());
  return recordTypeIndexForDINode(SP, TI, Class);
}

TypeIndex CodeViewDebug::recordTypeIndexForDINode(const DINode *Node,
                                                  TypeIndex TI,
                                                  const DIType *ClassTy) {
  auto InsertResult = TypeIndices.insert({{Node, ClassTy}, TI});
  (void)InsertResult;
  assert(InsertResult.second && "DINode was already assigned a type index");
  return TI;
}

unsigned CodeViewDebug::getPointerSizeInBytes() {
  return MMI->getModule()->getDataLayout().getPointerSizeInBits() / 8;
}

void CodeViewDebug::recordLocalVariable(LocalVariable &&Var,
                                        const DILocation *InlinedAt) {
  if (InlinedAt) {
    // This variable was inlined. Associate it with the InlineSite.
    const DISubprogram *Inlinee = Var.DIVar->getScope()->getSubprogram();
    InlineSite &Site = getInlineSite(InlinedAt, Inlinee);
    Site.InlinedLocals.emplace_back(Var);
  } else {
    // This variable goes in the main ProcSym.
    CurFn->Locals.emplace_back(Var);
  }
}

static void addLocIfNotPresent(SmallVectorImpl<const DILocation *> &Locs,
                               const DILocation *Loc) {
  auto B = Locs.begin(), E = Locs.end();
  if (std::find(B, E, Loc) == E)
    Locs.push_back(Loc);
}

void CodeViewDebug::maybeRecordLocation(const DebugLoc &DL,
                                        const MachineFunction *MF) {
  // Skip this instruction if it has the same location as the previous one.
  if (!DL || DL == PrevInstLoc)
    return;

  const DIScope *Scope = DL.get()->getScope();
  if (!Scope)
    return;

  // Skip this line if it is longer than the maximum we can record.
  LineInfo LI(DL.getLine(), DL.getLine(), /*IsStatement=*/true);
  if (LI.getStartLine() != DL.getLine() || LI.isAlwaysStepInto() ||
      LI.isNeverStepInto())
    return;

  ColumnInfo CI(DL.getCol(), /*EndColumn=*/0);
  if (CI.getStartColumn() != DL.getCol())
    return;

  if (!CurFn->HaveLineInfo)
    CurFn->HaveLineInfo = true;
  unsigned FileId = 0;
  if (PrevInstLoc.get() && PrevInstLoc->getFile() == DL->getFile())
    FileId = CurFn->LastFileId;
  else
    FileId = CurFn->LastFileId = maybeRecordFile(DL->getFile());
  PrevInstLoc = DL;

  unsigned FuncId = CurFn->FuncId;
  if (const DILocation *SiteLoc = DL->getInlinedAt()) {
    const DILocation *Loc = DL.get();

    // If this location was actually inlined from somewhere else, give it the ID
    // of the inline call site.
    FuncId =
        getInlineSite(SiteLoc, Loc->getScope()->getSubprogram()).SiteFuncId;

    // Ensure we have links in the tree of inline call sites.
    bool FirstLoc = true;
    while ((SiteLoc = Loc->getInlinedAt())) {
      InlineSite &Site =
          getInlineSite(SiteLoc, Loc->getScope()->getSubprogram());
      if (!FirstLoc)
        addLocIfNotPresent(Site.ChildSites, Loc);
      FirstLoc = false;
      Loc = SiteLoc;
    }
    addLocIfNotPresent(CurFn->ChildSites, Loc);
  }

  OS.EmitCVLocDirective(FuncId, FileId, DL.getLine(), DL.getCol(),
                        /*PrologueEnd=*/false, /*IsStmt=*/false,
                        DL->getFilename(), SMLoc());
}

void CodeViewDebug::emitCodeViewMagicVersion() {
  OS.EmitValueToAlignment(4);
  OS.AddComment("Debug section magic");
  OS.EmitIntValue(COFF::DEBUG_SECTION_MAGIC, 4);
}

void CodeViewDebug::endModule() {
  if (!Asm || !MMI->hasDebugInfo())
    return;

  assert(Asm != nullptr);

  // The COFF .debug$S section consists of several subsections, each starting
  // with a 4-byte control code (e.g. 0xF1, 0xF2, etc) and then a 4-byte length
  // of the payload followed by the payload itself.  The subsections are 4-byte
  // aligned.

  // Use the generic .debug$S section, and make a subsection for all the inlined
  // subprograms.
  switchToDebugSectionForSymbol(nullptr);

  MCSymbol *CompilerInfo = beginCVSubsection(DebugSubsectionKind::Symbols);
  emitCompilerInformation();
  endCVSubsection(CompilerInfo);

  emitInlineeLinesSubsection();

  // Emit per-function debug information.
  for (auto &P : FnDebugInfo)
    if (!P.first->isDeclarationForLinker())
      emitDebugInfoForFunction(P.first, P.second);

  // Emit global variable debug information.
  setCurrentSubprogram(nullptr);
  emitDebugInfoForGlobals();

  // Emit retained types.
  emitDebugInfoForRetainedTypes();

  // Switch back to the generic .debug$S section after potentially processing
  // comdat symbol sections.
  switchToDebugSectionForSymbol(nullptr);

  // Emit UDT records for any types used by global variables.
  if (!GlobalUDTs.empty()) {
    MCSymbol *SymbolsEnd = beginCVSubsection(DebugSubsectionKind::Symbols);
    emitDebugInfoForUDTs(GlobalUDTs);
    endCVSubsection(SymbolsEnd);
  }

  // This subsection holds a file index to offset in string table table.
  OS.AddComment("File index to string table offset subsection");
  OS.EmitCVFileChecksumsDirective();

  // This subsection holds the string table.
  OS.AddComment("String table");
  OS.EmitCVStringTableDirective();

  // Emit type information last, so that any types we translate while emitting
  // function info are included.
  emitTypeInformation();

  clear();
}

static void emitNullTerminatedSymbolName(MCStreamer &OS, StringRef S) {
  // The maximum CV record length is 0xFF00. Most of the strings we emit appear
  // after a fixed length portion of the record. The fixed length portion should
  // always be less than 0xF00 (3840) bytes, so truncate the string so that the
  // overall record size is less than the maximum allowed.
  unsigned MaxFixedRecordLength = 0xF00;
  SmallString<32> NullTerminatedString(
      S.take_front(MaxRecordLength - MaxFixedRecordLength - 1));
  NullTerminatedString.push_back('\0');
  OS.EmitBytes(NullTerminatedString);
}

void CodeViewDebug::emitTypeInformation() {
  // Do nothing if we have no debug info or if no non-trivial types were emitted
  // to TypeTable during codegen.
  NamedMDNode *CU_Nodes = MMI->getModule()->getNamedMetadata("llvm.dbg.cu");
  if (!CU_Nodes)
    return;
  if (TypeTable.empty())
    return;

  // Start the .debug$T section with 0x4.
  OS.SwitchSection(Asm->getObjFileLowering().getCOFFDebugTypesSection());
  emitCodeViewMagicVersion();

  SmallString<8> CommentPrefix;
  if (OS.isVerboseAsm()) {
    CommentPrefix += '\t';
    CommentPrefix += Asm->MAI->getCommentString();
    CommentPrefix += ' ';
  }

  TypeTableCollection Table(TypeTable.records());
  Optional<TypeIndex> B = Table.getFirst();
  while (B) {
    // This will fail if the record data is invalid.
    CVType Record = Table.getType(*B);

    if (OS.isVerboseAsm()) {
      // Emit a block comment describing the type record for readability.
      SmallString<512> CommentBlock;
      raw_svector_ostream CommentOS(CommentBlock);
      ScopedPrinter SP(CommentOS);
      SP.setPrefix(CommentPrefix);
      TypeDumpVisitor TDV(Table, &SP, false);

      Error E = codeview::visitTypeRecord(Record, *B, TDV);
      if (E) {
        logAllUnhandledErrors(std::move(E), errs(), "error: ");
        llvm_unreachable("produced malformed type record");
      }
      // emitRawComment will insert its own tab and comment string before
      // the first line, so strip off our first one. It also prints its own
      // newline.
      OS.emitRawComment(
          CommentOS.str().drop_front(CommentPrefix.size() - 1).rtrim());
    }
    OS.EmitBinaryData(Record.str_data());
    B = Table.getNext(*B);
  }
}

namespace {

static SourceLanguage MapDWLangToCVLang(unsigned DWLang) {
  switch (DWLang) {
  case dwarf::DW_LANG_C:
  case dwarf::DW_LANG_C89:
  case dwarf::DW_LANG_C99:
  case dwarf::DW_LANG_C11:
  case dwarf::DW_LANG_ObjC:
    return SourceLanguage::C;
  case dwarf::DW_LANG_C_plus_plus:
  case dwarf::DW_LANG_C_plus_plus_03:
  case dwarf::DW_LANG_C_plus_plus_11:
  case dwarf::DW_LANG_C_plus_plus_14:
    return SourceLanguage::Cpp;
  case dwarf::DW_LANG_Fortran77:
  case dwarf::DW_LANG_Fortran90:
  case dwarf::DW_LANG_Fortran03:
  case dwarf::DW_LANG_Fortran08:
    return SourceLanguage::Fortran;
  case dwarf::DW_LANG_Pascal83:
    return SourceLanguage::Pascal;
  case dwarf::DW_LANG_Cobol74:
  case dwarf::DW_LANG_Cobol85:
    return SourceLanguage::Cobol;
  case dwarf::DW_LANG_Java:
    return SourceLanguage::Java;
  default:
    // There's no CodeView representation for this language, and CV doesn't
    // have an "unknown" option for the language field, so we'll use MASM,
    // as it's very low level.
    return SourceLanguage::Masm;
  }
}

struct Version {
  int Part[4];
};

// Takes a StringRef like "clang 4.0.0.0 (other nonsense 123)" and parses out
// the version number.
static Version parseVersion(StringRef Name) {
  Version V = {{0}};
  int N = 0;
  for (const char C : Name) {
    if (isdigit(C)) {
      V.Part[N] *= 10;
      V.Part[N] += C - '0';
    } else if (C == '.') {
      ++N;
      if (N >= 4)
        return V;
    } else if (N > 0)
      return V;
  }
  return V;
}

static CPUType mapArchToCVCPUType(Triple::ArchType Type) {
  switch (Type) {
    case Triple::ArchType::x86:
      return CPUType::Pentium3;
    case Triple::ArchType::x86_64:
      return CPUType::X64;
    case Triple::ArchType::thumb:
      return CPUType::Thumb;
    default:
      report_fatal_error("target architecture doesn't map to a CodeView "
                         "CPUType");
  }
}

} // end anonymous namespace

void CodeViewDebug::emitCompilerInformation() {
  MCContext &Context = MMI->getContext();
  MCSymbol *CompilerBegin = Context.createTempSymbol(),
           *CompilerEnd = Context.createTempSymbol();
  OS.AddComment("Record length");
  OS.emitAbsoluteSymbolDiff(CompilerEnd, CompilerBegin, 2);
  OS.EmitLabel(CompilerBegin);
  OS.AddComment("Record kind: S_COMPILE3");
  OS.EmitIntValue(SymbolKind::S_COMPILE3, 2);
  uint32_t Flags = 0;

  NamedMDNode *CUs = MMI->getModule()->getNamedMetadata("llvm.dbg.cu");
  const MDNode *Node = *CUs->operands().begin();
  const auto *CU = cast<DICompileUnit>(Node);

  // The low byte of the flags indicates the source language.
  Flags = MapDWLangToCVLang(CU->getSourceLanguage());
  // TODO:  Figure out which other flags need to be set.

  OS.AddComment("Flags and language");
  OS.EmitIntValue(Flags, 4);

  OS.AddComment("CPUType");
  CPUType CPU =
      mapArchToCVCPUType(Triple(MMI->getModule()->getTargetTriple()).getArch());
  OS.EmitIntValue(static_cast<uint64_t>(CPU), 2);

  StringRef CompilerVersion = CU->getProducer();
  Version FrontVer = parseVersion(CompilerVersion);
  OS.AddComment("Frontend version");
  for (int N = 0; N < 4; ++N)
    OS.EmitIntValue(FrontVer.Part[N], 2);

  // Some Microsoft tools, like Binscope, expect a backend version number of at
  // least 8.something, so we'll coerce the LLVM version into a form that
  // guarantees it'll be big enough without really lying about the version.
  int Major = 1000 * LLVM_VERSION_MAJOR +
              10 * LLVM_VERSION_MINOR +
              LLVM_VERSION_PATCH;
  // Clamp it for builds that use unusually large version numbers.
  Major = std::min<int>(Major, std::numeric_limits<uint16_t>::max());
  Version BackVer = {{ Major, 0, 0, 0 }};
  OS.AddComment("Backend version");
  for (int N = 0; N < 4; ++N)
    OS.EmitIntValue(BackVer.Part[N], 2);

  OS.AddComment("Null-terminated compiler version string");
  emitNullTerminatedSymbolName(OS, CompilerVersion);

  OS.EmitLabel(CompilerEnd);
}

void CodeViewDebug::emitInlineeLinesSubsection() {
  if (InlinedSubprograms.empty())
    return;

  OS.AddComment("Inlinee lines subsection");
  MCSymbol *InlineEnd = beginCVSubsection(DebugSubsectionKind::InlineeLines);

  // We don't provide any extra file info.
  // FIXME: Find out if debuggers use this info.
  OS.AddComment("Inlinee lines signature");
  OS.EmitIntValue(unsigned(InlineeLinesSignature::Normal), 4);

  for (const DISubprogram *SP : InlinedSubprograms) {
    assert(TypeIndices.count({SP, nullptr}));
    TypeIndex InlineeIdx = TypeIndices[{SP, nullptr}];

    OS.AddBlankLine();
    unsigned FileId = maybeRecordFile(SP->getFile());
    OS.AddComment("Inlined function " + SP->getName() + " starts at " +
                  SP->getFilename() + Twine(':') + Twine(SP->getLine()));
    OS.AddBlankLine();
    // The filechecksum table uses 8 byte entries for now, and file ids start at
    // 1.
    unsigned FileOffset = (FileId - 1) * 8;
    OS.AddComment("Type index of inlined function");
    OS.EmitIntValue(InlineeIdx.getIndex(), 4);
    OS.AddComment("Offset into filechecksum table");
    OS.EmitIntValue(FileOffset, 4);
    OS.AddComment("Starting line number");
    OS.EmitIntValue(SP->getLine(), 4);
  }

  endCVSubsection(InlineEnd);
}

void CodeViewDebug::emitInlinedCallSite(const FunctionInfo &FI,
                                        const DILocation *InlinedAt,
                                        const InlineSite &Site) {
  MCSymbol *InlineBegin = MMI->getContext().createTempSymbol(),
           *InlineEnd = MMI->getContext().createTempSymbol();

  assert(TypeIndices.count({Site.Inlinee, nullptr}));
  TypeIndex InlineeIdx = TypeIndices[{Site.Inlinee, nullptr}];

  // SymbolRecord
  OS.AddComment("Record length");
  OS.emitAbsoluteSymbolDiff(InlineEnd, InlineBegin, 2);   // RecordLength
  OS.EmitLabel(InlineBegin);
  OS.AddComment("Record kind: S_INLINESITE");
  OS.EmitIntValue(SymbolKind::S_INLINESITE, 2); // RecordKind

  OS.AddComment("PtrParent");
  OS.EmitIntValue(0, 4);
  OS.AddComment("PtrEnd");
  OS.EmitIntValue(0, 4);
  OS.AddComment("Inlinee type index");
  OS.EmitIntValue(InlineeIdx.getIndex(), 4);

  unsigned FileId = maybeRecordFile(Site.Inlinee->getFile());
  unsigned StartLineNum = Site.Inlinee->getLine();

  OS.EmitCVInlineLinetableDirective(Site.SiteFuncId, FileId, StartLineNum,
                                    FI.Begin, FI.End);

  OS.EmitLabel(InlineEnd);

  emitLocalVariableList(Site.InlinedLocals);

  // Recurse on child inlined call sites before closing the scope.
  for (const DILocation *ChildSite : Site.ChildSites) {
    auto I = FI.InlineSites.find(ChildSite);
    assert(I != FI.InlineSites.end() &&
           "child site not in function inline site map");
    emitInlinedCallSite(FI, ChildSite, I->second);
  }

  // Close the scope.
  OS.AddComment("Record length");
  OS.EmitIntValue(2, 2);                                  // RecordLength
  OS.AddComment("Record kind: S_INLINESITE_END");
  OS.EmitIntValue(SymbolKind::S_INLINESITE_END, 2); // RecordKind
}

void CodeViewDebug::switchToDebugSectionForSymbol(const MCSymbol *GVSym) {
  // If we have a symbol, it may be in a section that is COMDAT. If so, find the
  // comdat key. A section may be comdat because of -ffunction-sections or
  // because it is comdat in the IR.
  MCSectionCOFF *GVSec =
      GVSym ? dyn_cast<MCSectionCOFF>(&GVSym->getSection()) : nullptr;
  const MCSymbol *KeySym = GVSec ? GVSec->getCOMDATSymbol() : nullptr;

  MCSectionCOFF *DebugSec = cast<MCSectionCOFF>(
      Asm->getObjFileLowering().getCOFFDebugSymbolsSection());
  DebugSec = OS.getContext().getAssociativeCOFFSection(DebugSec, KeySym);

  OS.SwitchSection(DebugSec);

  // Emit the magic version number if this is the first time we've switched to
  // this section.
  if (ComdatDebugSections.insert(DebugSec).second)
    emitCodeViewMagicVersion();
}

void CodeViewDebug::emitDebugInfoForFunction(const Function *GV,
                                             FunctionInfo &FI) {
  // For each function there is a separate subsection
  // which holds the PC to file:line table.
  const MCSymbol *Fn = Asm->getSymbol(GV);
  assert(Fn);

  // Switch to the to a comdat section, if appropriate.
  switchToDebugSectionForSymbol(Fn);

  std::string FuncName;
  auto *SP = GV->getSubprogram();
  assert(SP);
  setCurrentSubprogram(SP);

  // If we have a display name, build the fully qualified name by walking the
  // chain of scopes.
  if (!SP->getName().empty())
    FuncName =
        getFullyQualifiedName(SP->getScope().resolve(), SP->getName());

  // If our DISubprogram name is empty, use the mangled name.
  if (FuncName.empty())
    FuncName = GlobalValue::dropLLVMManglingEscape(GV->getName());

  // Emit a symbol subsection, required by VS2012+ to find function boundaries.
  OS.AddComment("Symbol subsection for " + Twine(FuncName));
  MCSymbol *SymbolsEnd = beginCVSubsection(DebugSubsectionKind::Symbols);
  {
    MCSymbol *ProcRecordBegin = MMI->getContext().createTempSymbol(),
             *ProcRecordEnd = MMI->getContext().createTempSymbol();
    OS.AddComment("Record length");
    OS.emitAbsoluteSymbolDiff(ProcRecordEnd, ProcRecordBegin, 2);
    OS.EmitLabel(ProcRecordBegin);

    if (GV->hasLocalLinkage()) {
      OS.AddComment("Record kind: S_LPROC32_ID");
      OS.EmitIntValue(unsigned(SymbolKind::S_LPROC32_ID), 2);
    } else {
      OS.AddComment("Record kind: S_GPROC32_ID");
      OS.EmitIntValue(unsigned(SymbolKind::S_GPROC32_ID), 2);
    }

    // These fields are filled in by tools like CVPACK which run after the fact.
    OS.AddComment("PtrParent");
    OS.EmitIntValue(0, 4);
    OS.AddComment("PtrEnd");
    OS.EmitIntValue(0, 4);
    OS.AddComment("PtrNext");
    OS.EmitIntValue(0, 4);
    // This is the important bit that tells the debugger where the function
    // code is located and what's its size:
    OS.AddComment("Code size");
    OS.emitAbsoluteSymbolDiff(FI.End, Fn, 4);
    OS.AddComment("Offset after prologue");
    OS.EmitIntValue(0, 4);
    OS.AddComment("Offset before epilogue");
    OS.EmitIntValue(0, 4);
    OS.AddComment("Function type index");
    OS.EmitIntValue(getFuncIdForSubprogram(GV->getSubprogram()).getIndex(), 4);
    OS.AddComment("Function section relative address");
    OS.EmitCOFFSecRel32(Fn, /*Offset=*/0);
    OS.AddComment("Function section index");
    OS.EmitCOFFSectionIndex(Fn);
    OS.AddComment("Flags");
    OS.EmitIntValue(0, 1);
    // Emit the function display name as a null-terminated string.
    OS.AddComment("Function name");
    // Truncate the name so we won't overflow the record length field.
    emitNullTerminatedSymbolName(OS, FuncName);
    OS.EmitLabel(ProcRecordEnd);

    emitLocalVariableList(FI.Locals);

    // Emit inlined call site information. Only emit functions inlined directly
    // into the parent function. We'll emit the other sites recursively as part
    // of their parent inline site.
    for (const DILocation *InlinedAt : FI.ChildSites) {
      auto I = FI.InlineSites.find(InlinedAt);
      assert(I != FI.InlineSites.end() &&
             "child site not in function inline site map");
      emitInlinedCallSite(FI, InlinedAt, I->second);
    }

    if (SP != nullptr)
      emitDebugInfoForUDTs(LocalUDTs);

    // We're done with this function.
    OS.AddComment("Record length");
    OS.EmitIntValue(0x0002, 2);
    OS.AddComment("Record kind: S_PROC_ID_END");
    OS.EmitIntValue(unsigned(SymbolKind::S_PROC_ID_END), 2);
  }
  endCVSubsection(SymbolsEnd);

  // We have an assembler directive that takes care of the whole line table.
  OS.EmitCVLinetableDirective(FI.FuncId, Fn, FI.End);
}

CodeViewDebug::LocalVarDefRange
CodeViewDebug::createDefRangeMem(uint16_t CVRegister, int Offset) {
  LocalVarDefRange DR;
  DR.InMemory = -1;
  DR.DataOffset = Offset;
  assert(DR.DataOffset == Offset && "truncation");
  DR.IsSubfield = 0;
  DR.StructOffset = 0;
  DR.CVRegister = CVRegister;
  return DR;
}

CodeViewDebug::LocalVarDefRange
CodeViewDebug::createDefRangeGeneral(uint16_t CVRegister, bool InMemory,
                                     int Offset, bool IsSubfield,
                                     uint16_t StructOffset) {
  LocalVarDefRange DR;
  DR.InMemory = InMemory;
  DR.DataOffset = Offset;
  DR.IsSubfield = IsSubfield;
  DR.StructOffset = StructOffset;
  DR.CVRegister = CVRegister;
  return DR;
}

void CodeViewDebug::collectVariableInfoFromMFTable(
    DenseSet<InlinedVariable> &Processed) {
  const MachineFunction &MF = *Asm->MF;
  const TargetSubtargetInfo &TSI = MF.getSubtarget();
  const TargetFrameLowering *TFI = TSI.getFrameLowering();
  const TargetRegisterInfo *TRI = TSI.getRegisterInfo();

  for (const MachineFunction::VariableDbgInfo &VI : MF.getVariableDbgInfo()) {
    if (!VI.Var)
      continue;
    assert(VI.Var->isValidLocationForIntrinsic(VI.Loc) &&
           "Expected inlined-at fields to agree");

    Processed.insert(InlinedVariable(VI.Var, VI.Loc->getInlinedAt()));
    LexicalScope *Scope = LScopes.findLexicalScope(VI.Loc);

    // If variable scope is not found then skip this variable.
    if (!Scope)
      continue;

    // If the variable has an attached offset expression, extract it.
    // FIXME: Try to handle DW_OP_deref as well.
    int64_t ExprOffset = 0;
    if (VI.Expr)
      if (!VI.Expr->extractIfOffset(ExprOffset))
        continue;

    // Get the frame register used and the offset.
    unsigned FrameReg = 0;
    int FrameOffset = TFI->getFrameIndexReference(*Asm->MF, VI.Slot, FrameReg);
    uint16_t CVReg = TRI->getCodeViewRegNum(FrameReg);

    // Calculate the label ranges.
    LocalVarDefRange DefRange =
        createDefRangeMem(CVReg, FrameOffset + ExprOffset);
    for (const InsnRange &Range : Scope->getRanges()) {
      const MCSymbol *Begin = getLabelBeforeInsn(Range.first);
      const MCSymbol *End = getLabelAfterInsn(Range.second);
      End = End ? End : Asm->getFunctionEnd();
      DefRange.Ranges.emplace_back(Begin, End);
    }

    LocalVariable Var;
    Var.DIVar = VI.Var;
    Var.DefRanges.emplace_back(std::move(DefRange));
    recordLocalVariable(std::move(Var), VI.Loc->getInlinedAt());
  }
}

void CodeViewDebug::collectVariableInfo(const DISubprogram *SP) {
  DenseSet<InlinedVariable> Processed;
  // Grab the variable info that was squirreled away in the MMI side-table.
  collectVariableInfoFromMFTable(Processed);

  const TargetRegisterInfo *TRI = Asm->MF->getSubtarget().getRegisterInfo();

  for (const auto &I : DbgValues) {
    InlinedVariable IV = I.first;
    if (Processed.count(IV))
      continue;
    const DILocalVariable *DIVar = IV.first;
    const DILocation *InlinedAt = IV.second;

    // Instruction ranges, specifying where IV is accessible.
    const auto &Ranges = I.second;

    LexicalScope *Scope = nullptr;
    if (InlinedAt)
      Scope = LScopes.findInlinedScope(DIVar->getScope(), InlinedAt);
    else
      Scope = LScopes.findLexicalScope(DIVar->getScope());
    // If variable scope is not found then skip this variable.
    if (!Scope)
      continue;

    LocalVariable Var;
    Var.DIVar = DIVar;

    // Calculate the definition ranges.
    for (auto I = Ranges.begin(), E = Ranges.end(); I != E; ++I) {
      const InsnRange &Range = *I;
      const MachineInstr *DVInst = Range.first;
      assert(DVInst->isDebugValue() && "Invalid History entry");
      const DIExpression *DIExpr = DVInst->getDebugExpression();
      bool IsSubfield = false;
      unsigned StructOffset = 0;

      // Handle fragments.
      auto Fragment = DIExpr->getFragmentInfo();
      if (Fragment) {
        IsSubfield = true;
        StructOffset = Fragment->OffsetInBits / 8;
      } else if (DIExpr->getNumElements() > 0) {
        continue; // Ignore unrecognized exprs.
      }

      // Bail if operand 0 is not a valid register. This means the variable is a
      // simple constant, or is described by a complex expression.
      // FIXME: Find a way to represent constant variables, since they are
      // relatively common.
      unsigned Reg =
          DVInst->getOperand(0).isReg() ? DVInst->getOperand(0).getReg() : 0;
      if (Reg == 0)
        continue;

      // Handle the two cases we can handle: indirect in memory and in register.
      unsigned CVReg = TRI->getCodeViewRegNum(Reg);
      bool InMemory = DVInst->getOperand(1).isImm();
      int Offset = InMemory ? DVInst->getOperand(1).getImm() : 0;
      {
        LocalVarDefRange DR;
        DR.CVRegister = CVReg;
        DR.InMemory = InMemory;
        DR.DataOffset = Offset;
        DR.IsSubfield = IsSubfield;
        DR.StructOffset = StructOffset;

        if (Var.DefRanges.empty() ||
            Var.DefRanges.back().isDifferentLocation(DR)) {
          Var.DefRanges.emplace_back(std::move(DR));
        }
      }

      // Compute the label range.
      const MCSymbol *Begin = getLabelBeforeInsn(Range.first);
      const MCSymbol *End = getLabelAfterInsn(Range.second);
      if (!End) {
        // This range is valid until the next overlapping bitpiece. In the
        // common case, ranges will not be bitpieces, so they will overlap.
        auto J = std::next(I);
        while (J != E &&
               !fragmentsOverlap(DIExpr, J->first->getDebugExpression()))
          ++J;
        if (J != E)
          End = getLabelBeforeInsn(J->first);
        else
          End = Asm->getFunctionEnd();
      }

      // If the last range end is our begin, just extend the last range.
      // Otherwise make a new range.
      SmallVectorImpl<std::pair<const MCSymbol *, const MCSymbol *>> &Ranges =
          Var.DefRanges.back().Ranges;
      if (!Ranges.empty() && Ranges.back().second == Begin)
        Ranges.back().second = End;
      else
        Ranges.emplace_back(Begin, End);

      // FIXME: Do more range combining.
    }

    recordLocalVariable(std::move(Var), InlinedAt);
  }
}

void CodeViewDebug::beginFunctionImpl(const MachineFunction *MF) {
  const Function *GV = MF->getFunction();
  assert(FnDebugInfo.count(GV) == false);
  CurFn = &FnDebugInfo[GV];
  CurFn->FuncId = NextFuncId++;
  CurFn->Begin = Asm->getFunctionBegin();

  OS.EmitCVFuncIdDirective(CurFn->FuncId);

  // Find the end of the function prolog.  First known non-DBG_VALUE and
  // non-frame setup location marks the beginning of the function body.
  // FIXME: is there a simpler a way to do this? Can we just search
  // for the first instruction of the function, not the last of the prolog?
  DebugLoc PrologEndLoc;
  bool EmptyPrologue = true;
  for (const auto &MBB : *MF) {
    for (const auto &MI : MBB) {
      if (!MI.isMetaInstruction() && !MI.getFlag(MachineInstr::FrameSetup) &&
          MI.getDebugLoc()) {
        PrologEndLoc = MI.getDebugLoc();
        break;
      } else if (!MI.isMetaInstruction()) {
        EmptyPrologue = false;
      }
    }
  }

  // Record beginning of function if we have a non-empty prologue.
  if (PrologEndLoc && !EmptyPrologue) {
    DebugLoc FnStartDL = PrologEndLoc.getFnDebugLoc();
    maybeRecordLocation(FnStartDL, MF);
  }
}

void CodeViewDebug::addToUDTs(const DIType *Ty, TypeIndex TI) {
  // Don't record empty UDTs.
  if (Ty->getName().empty())
    return;

  SmallVector<StringRef, 5> QualifiedNameComponents;
  const DISubprogram *ClosestSubprogram = getQualifiedNameComponents(
      Ty->getScope().resolve(), QualifiedNameComponents);

  std::string FullyQualifiedName =
      getQualifiedName(QualifiedNameComponents, getPrettyScopeName(Ty));

  if (ClosestSubprogram == nullptr)
    GlobalUDTs.emplace_back(std::move(FullyQualifiedName), TI);
  else if (ClosestSubprogram == CurrentSubprogram)
    LocalUDTs.emplace_back(std::move(FullyQualifiedName), TI);

  // TODO: What if the ClosestSubprogram is neither null or the current
  // subprogram?  Currently, the UDT just gets dropped on the floor.
  //
  // The current behavior is not desirable.  To get maximal fidelity, we would
  // need to perform all type translation before beginning emission of .debug$S
  // and then make LocalUDTs a member of FunctionInfo
}

TypeIndex CodeViewDebug::lowerType(const DIType *Ty, const DIType *ClassTy) {
  // Generic dispatch for lowering an unknown type.
  switch (Ty->getTag()) {
  case dwarf::DW_TAG_array_type:
    return lowerTypeArray(cast<DICompositeType>(Ty));
  case dwarf::DW_TAG_typedef:
    return lowerTypeAlias(cast<DIDerivedType>(Ty));
  case dwarf::DW_TAG_base_type:
    return lowerTypeBasic(cast<DIBasicType>(Ty));
  case dwarf::DW_TAG_pointer_type:
    if (cast<DIDerivedType>(Ty)->getName() == "__vtbl_ptr_type")
      return lowerTypeVFTableShape(cast<DIDerivedType>(Ty));
    LLVM_FALLTHROUGH;
  case dwarf::DW_TAG_reference_type:
  case dwarf::DW_TAG_rvalue_reference_type:
    return lowerTypePointer(cast<DIDerivedType>(Ty));
  case dwarf::DW_TAG_ptr_to_member_type:
    return lowerTypeMemberPointer(cast<DIDerivedType>(Ty));
  case dwarf::DW_TAG_const_type:
  case dwarf::DW_TAG_volatile_type:
  // TODO: add support for DW_TAG_atomic_type here
    return lowerTypeModifier(cast<DIDerivedType>(Ty));
  case dwarf::DW_TAG_subroutine_type:
    if (ClassTy) {
      // The member function type of a member function pointer has no
      // ThisAdjustment.
      return lowerTypeMemberFunction(cast<DISubroutineType>(Ty), ClassTy,
                                     /*ThisAdjustment=*/0);
    }
    return lowerTypeFunction(cast<DISubroutineType>(Ty));
  case dwarf::DW_TAG_enumeration_type:
    return lowerTypeEnum(cast<DICompositeType>(Ty));
  case dwarf::DW_TAG_class_type:
  case dwarf::DW_TAG_structure_type:
    return lowerTypeClass(cast<DICompositeType>(Ty));
  case dwarf::DW_TAG_union_type:
    return lowerTypeUnion(cast<DICompositeType>(Ty));
  default:
    // Use the null type index.
    return TypeIndex();
  }
}

TypeIndex CodeViewDebug::lowerTypeAlias(const DIDerivedType *Ty) {
  DITypeRef UnderlyingTypeRef = Ty->getBaseType();
  TypeIndex UnderlyingTypeIndex = getTypeIndex(UnderlyingTypeRef);
  StringRef TypeName = Ty->getName();

  addToUDTs(Ty, UnderlyingTypeIndex);

  if (UnderlyingTypeIndex == TypeIndex(SimpleTypeKind::Int32Long) &&
      TypeName == "HRESULT")
    return TypeIndex(SimpleTypeKind::HResult);
  if (UnderlyingTypeIndex == TypeIndex(SimpleTypeKind::UInt16Short) &&
      TypeName == "wchar_t")
    return TypeIndex(SimpleTypeKind::WideCharacter);

  return UnderlyingTypeIndex;
}

TypeIndex CodeViewDebug::lowerTypeArray(const DICompositeType *Ty) {
  DITypeRef ElementTypeRef = Ty->getBaseType();
  TypeIndex ElementTypeIndex = getTypeIndex(ElementTypeRef);
  // IndexType is size_t, which depends on the bitness of the target.
  TypeIndex IndexType = Asm->TM.getPointerSize() == 8
                            ? TypeIndex(SimpleTypeKind::UInt64Quad)
                            : TypeIndex(SimpleTypeKind::UInt32Long);

  uint64_t ElementSize = getBaseTypeSize(ElementTypeRef) / 8;

  // Add subranges to array type.
  DINodeArray Elements = Ty->getElements();
  for (int i = Elements.size() - 1; i >= 0; --i) {
    const DINode *Element = Elements[i];
    assert(Element->getTag() == dwarf::DW_TAG_subrange_type);

    const DISubrange *Subrange = cast<DISubrange>(Element);
    assert(Subrange->getLowerBound() == 0 &&
           "codeview doesn't support subranges with lower bounds");
    int64_t Count = Subrange->getCount();

    // Variable Length Array (VLA) has Count equal to '-1'.
    // Replace with Count '1', assume it is the minimum VLA length.
    // FIXME: Make front-end support VLA subrange and emit LF_DIMVARLU.
    if (Count == -1)
      Count = 1;

    // Update the element size and element type index for subsequent subranges.
    ElementSize *= Count;

    // If this is the outermost array, use the size from the array. It will be
    // more accurate if we had a VLA or an incomplete element type size.
    uint64_t ArraySize =
        (i == 0 && ElementSize == 0) ? Ty->getSizeInBits() / 8 : ElementSize;

    StringRef Name = (i == 0) ? Ty->getName() : "";
    ArrayRecord AR(ElementTypeIndex, IndexType, ArraySize, Name);
    ElementTypeIndex = TypeTable.writeKnownType(AR);
  }

  return ElementTypeIndex;
}

TypeIndex CodeViewDebug::lowerTypeBasic(const DIBasicType *Ty) {
  TypeIndex Index;
  dwarf::TypeKind Kind;
  uint32_t ByteSize;

  Kind = static_cast<dwarf::TypeKind>(Ty->getEncoding());
  ByteSize = Ty->getSizeInBits() / 8;

  SimpleTypeKind STK = SimpleTypeKind::None;
  switch (Kind) {
  case dwarf::DW_ATE_address:
    // FIXME: Translate
    break;
  case dwarf::DW_ATE_boolean:
    switch (ByteSize) {
    case 1:  STK = SimpleTypeKind::Boolean8;   break;
    case 2:  STK = SimpleTypeKind::Boolean16;  break;
    case 4:  STK = SimpleTypeKind::Boolean32;  break;
    case 8:  STK = SimpleTypeKind::Boolean64;  break;
    case 16: STK = SimpleTypeKind::Boolean128; break;
    }
    break;
  case dwarf::DW_ATE_complex_float:
    switch (ByteSize) {
    case 2:  STK = SimpleTypeKind::Complex16;  break;
    case 4:  STK = SimpleTypeKind::Complex32;  break;
    case 8:  STK = SimpleTypeKind::Complex64;  break;
    case 10: STK = SimpleTypeKind::Complex80;  break;
    case 16: STK = SimpleTypeKind::Complex128; break;
    }
    break;
  case dwarf::DW_ATE_float:
    switch (ByteSize) {
    case 2:  STK = SimpleTypeKind::Float16;  break;
    case 4:  STK = SimpleTypeKind::Float32;  break;
    case 6:  STK = SimpleTypeKind::Float48;  break;
    case 8:  STK = SimpleTypeKind::Float64;  break;
    case 10: STK = SimpleTypeKind::Float80;  break;
    case 16: STK = SimpleTypeKind::Float128; break;
    }
    break;
  case dwarf::DW_ATE_signed:
    switch (ByteSize) {
    case 1:  STK = SimpleTypeKind::SignedCharacter; break;
    case 2:  STK = SimpleTypeKind::Int16Short;      break;
    case 4:  STK = SimpleTypeKind::Int32;           break;
    case 8:  STK = SimpleTypeKind::Int64Quad;       break;
    case 16: STK = SimpleTypeKind::Int128Oct;       break;
    }
    break;
  case dwarf::DW_ATE_unsigned:
    switch (ByteSize) {
    case 1:  STK = SimpleTypeKind::UnsignedCharacter; break;
    case 2:  STK = SimpleTypeKind::UInt16Short;       break;
    case 4:  STK = SimpleTypeKind::UInt32;            break;
    case 8:  STK = SimpleTypeKind::UInt64Quad;        break;
    case 16: STK = SimpleTypeKind::UInt128Oct;        break;
    }
    break;
  case dwarf::DW_ATE_UTF:
    switch (ByteSize) {
    case 2: STK = SimpleTypeKind::Character16; break;
    case 4: STK = SimpleTypeKind::Character32; break;
    }
    break;
  case dwarf::DW_ATE_signed_char:
    if (ByteSize == 1)
      STK = SimpleTypeKind::SignedCharacter;
    break;
  case dwarf::DW_ATE_unsigned_char:
    if (ByteSize == 1)
      STK = SimpleTypeKind::UnsignedCharacter;
    break;
  default:
    break;
  }

  // Apply some fixups based on the source-level type name.
  if (STK == SimpleTypeKind::Int32 && Ty->getName() == "long int")
    STK = SimpleTypeKind::Int32Long;
  if (STK == SimpleTypeKind::UInt32 && Ty->getName() == "long unsigned int")
    STK = SimpleTypeKind::UInt32Long;
  if (STK == SimpleTypeKind::UInt16Short &&
      (Ty->getName() == "wchar_t" || Ty->getName() == "__wchar_t"))
    STK = SimpleTypeKind::WideCharacter;
  if ((STK == SimpleTypeKind::SignedCharacter ||
       STK == SimpleTypeKind::UnsignedCharacter) &&
      Ty->getName() == "char")
    STK = SimpleTypeKind::NarrowCharacter;

  return TypeIndex(STK);
}

TypeIndex CodeViewDebug::lowerTypePointer(const DIDerivedType *Ty) {
  TypeIndex PointeeTI = getTypeIndex(Ty->getBaseType());

  // Pointers to simple types can use SimpleTypeMode, rather than having a
  // dedicated pointer type record.
  if (PointeeTI.isSimple() &&
      PointeeTI.getSimpleMode() == SimpleTypeMode::Direct &&
      Ty->getTag() == dwarf::DW_TAG_pointer_type) {
    SimpleTypeMode Mode = Ty->getSizeInBits() == 64
                              ? SimpleTypeMode::NearPointer64
                              : SimpleTypeMode::NearPointer32;
    return TypeIndex(PointeeTI.getSimpleKind(), Mode);
  }

  PointerKind PK =
      Ty->getSizeInBits() == 64 ? PointerKind::Near64 : PointerKind::Near32;
  PointerMode PM = PointerMode::Pointer;
  switch (Ty->getTag()) {
  default: llvm_unreachable("not a pointer tag type");
  case dwarf::DW_TAG_pointer_type:
    PM = PointerMode::Pointer;
    break;
  case dwarf::DW_TAG_reference_type:
    PM = PointerMode::LValueReference;
    break;
  case dwarf::DW_TAG_rvalue_reference_type:
    PM = PointerMode::RValueReference;
    break;
  }
  // FIXME: MSVC folds qualifiers into PointerOptions in the context of a method
  // 'this' pointer, but not normal contexts. Figure out what we're supposed to
  // do.
  PointerOptions PO = PointerOptions::None;
  PointerRecord PR(PointeeTI, PK, PM, PO, Ty->getSizeInBits() / 8);
  return TypeTable.writeKnownType(PR);
}

static PointerToMemberRepresentation
translatePtrToMemberRep(unsigned SizeInBytes, bool IsPMF, unsigned Flags) {
  // SizeInBytes being zero generally implies that the member pointer type was
  // incomplete, which can happen if it is part of a function prototype. In this
  // case, use the unknown model instead of the general model.
  if (IsPMF) {
    switch (Flags & DINode::FlagPtrToMemberRep) {
    case 0:
      return SizeInBytes == 0 ? PointerToMemberRepresentation::Unknown
                              : PointerToMemberRepresentation::GeneralFunction;
    case DINode::FlagSingleInheritance:
      return PointerToMemberRepresentation::SingleInheritanceFunction;
    case DINode::FlagMultipleInheritance:
      return PointerToMemberRepresentation::MultipleInheritanceFunction;
    case DINode::FlagVirtualInheritance:
      return PointerToMemberRepresentation::VirtualInheritanceFunction;
    }
  } else {
    switch (Flags & DINode::FlagPtrToMemberRep) {
    case 0:
      return SizeInBytes == 0 ? PointerToMemberRepresentation::Unknown
                              : PointerToMemberRepresentation::GeneralData;
    case DINode::FlagSingleInheritance:
      return PointerToMemberRepresentation::SingleInheritanceData;
    case DINode::FlagMultipleInheritance:
      return PointerToMemberRepresentation::MultipleInheritanceData;
    case DINode::FlagVirtualInheritance:
      return PointerToMemberRepresentation::VirtualInheritanceData;
    }
  }
  llvm_unreachable("invalid ptr to member representation");
}

TypeIndex CodeViewDebug::lowerTypeMemberPointer(const DIDerivedType *Ty) {
  assert(Ty->getTag() == dwarf::DW_TAG_ptr_to_member_type);
  TypeIndex ClassTI = getTypeIndex(Ty->getClassType());
  TypeIndex PointeeTI = getTypeIndex(Ty->getBaseType(), Ty->getClassType());
  PointerKind PK = Asm->TM.getPointerSize() == 8 ? PointerKind::Near64
                                                 : PointerKind::Near32;
  bool IsPMF = isa<DISubroutineType>(Ty->getBaseType());
  PointerMode PM = IsPMF ? PointerMode::PointerToMemberFunction
                         : PointerMode::PointerToDataMember;
  PointerOptions PO = PointerOptions::None; // FIXME
  assert(Ty->getSizeInBits() / 8 <= 0xff && "pointer size too big");
  uint8_t SizeInBytes = Ty->getSizeInBits() / 8;
  MemberPointerInfo MPI(
      ClassTI, translatePtrToMemberRep(SizeInBytes, IsPMF, Ty->getFlags()));
  PointerRecord PR(PointeeTI, PK, PM, PO, SizeInBytes, MPI);
  return TypeTable.writeKnownType(PR);
}

/// Given a DWARF calling convention, get the CodeView equivalent. If we don't
/// have a translation, use the NearC convention.
static CallingConvention dwarfCCToCodeView(unsigned DwarfCC) {
  switch (DwarfCC) {
  case dwarf::DW_CC_normal:             return CallingConvention::NearC;
  case dwarf::DW_CC_BORLAND_msfastcall: return CallingConvention::NearFast;
  case dwarf::DW_CC_BORLAND_thiscall:   return CallingConvention::ThisCall;
  case dwarf::DW_CC_BORLAND_stdcall:    return CallingConvention::NearStdCall;
  case dwarf::DW_CC_BORLAND_pascal:     return CallingConvention::NearPascal;
  case dwarf::DW_CC_LLVM_vectorcall:    return CallingConvention::NearVector;
  }
  return CallingConvention::NearC;
}

TypeIndex CodeViewDebug::lowerTypeModifier(const DIDerivedType *Ty) {
  ModifierOptions Mods = ModifierOptions::None;
  bool IsModifier = true;
  const DIType *BaseTy = Ty;
  while (IsModifier && BaseTy) {
    // FIXME: Need to add DWARF tags for __unaligned and _Atomic
    switch (BaseTy->getTag()) {
    case dwarf::DW_TAG_const_type:
      Mods |= ModifierOptions::Const;
      break;
    case dwarf::DW_TAG_volatile_type:
      Mods |= ModifierOptions::Volatile;
      break;
    default:
      IsModifier = false;
      break;
    }
    if (IsModifier)
      BaseTy = cast<DIDerivedType>(BaseTy)->getBaseType().resolve();
  }
  TypeIndex ModifiedTI = getTypeIndex(BaseTy);
  ModifierRecord MR(ModifiedTI, Mods);
  return TypeTable.writeKnownType(MR);
}

TypeIndex CodeViewDebug::lowerTypeFunction(const DISubroutineType *Ty) {
  SmallVector<TypeIndex, 8> ReturnAndArgTypeIndices;
  for (DITypeRef ArgTypeRef : Ty->getTypeArray())
    ReturnAndArgTypeIndices.push_back(getTypeIndex(ArgTypeRef));

  TypeIndex ReturnTypeIndex = TypeIndex::Void();
  ArrayRef<TypeIndex> ArgTypeIndices = None;
  if (!ReturnAndArgTypeIndices.empty()) {
    auto ReturnAndArgTypesRef = makeArrayRef(ReturnAndArgTypeIndices);
    ReturnTypeIndex = ReturnAndArgTypesRef.front();
    ArgTypeIndices = ReturnAndArgTypesRef.drop_front();
  }

  ArgListRecord ArgListRec(TypeRecordKind::ArgList, ArgTypeIndices);
  TypeIndex ArgListIndex = TypeTable.writeKnownType(ArgListRec);

  CallingConvention CC = dwarfCCToCodeView(Ty->getCC());

  ProcedureRecord Procedure(ReturnTypeIndex, CC, FunctionOptions::None,
                            ArgTypeIndices.size(), ArgListIndex);
  return TypeTable.writeKnownType(Procedure);
}

TypeIndex CodeViewDebug::lowerTypeMemberFunction(const DISubroutineType *Ty,
                                                 const DIType *ClassTy,
                                                 int ThisAdjustment) {
  // Lower the containing class type.
  TypeIndex ClassType = getTypeIndex(ClassTy);

  SmallVector<TypeIndex, 8> ReturnAndArgTypeIndices;
  for (DITypeRef ArgTypeRef : Ty->getTypeArray())
    ReturnAndArgTypeIndices.push_back(getTypeIndex(ArgTypeRef));

  TypeIndex ReturnTypeIndex = TypeIndex::Void();
  ArrayRef<TypeIndex> ArgTypeIndices = None;
  if (!ReturnAndArgTypeIndices.empty()) {
    auto ReturnAndArgTypesRef = makeArrayRef(ReturnAndArgTypeIndices);
    ReturnTypeIndex = ReturnAndArgTypesRef.front();
    ArgTypeIndices = ReturnAndArgTypesRef.drop_front();
  }
  TypeIndex ThisTypeIndex = TypeIndex::Void();
  if (!ArgTypeIndices.empty()) {
    ThisTypeIndex = ArgTypeIndices.front();
    ArgTypeIndices = ArgTypeIndices.drop_front();
  }

  ArgListRecord ArgListRec(TypeRecordKind::ArgList, ArgTypeIndices);
  TypeIndex ArgListIndex = TypeTable.writeKnownType(ArgListRec);

  CallingConvention CC = dwarfCCToCodeView(Ty->getCC());

  // TODO: Need to use the correct values for:
  //       FunctionOptions
  //       ThisPointerAdjustment.
  MemberFunctionRecord MFR(ReturnTypeIndex, ClassType, ThisTypeIndex, CC,
                           FunctionOptions::None, ArgTypeIndices.size(),
                           ArgListIndex, ThisAdjustment);
  TypeIndex TI = TypeTable.writeKnownType(MFR);

  return TI;
}

TypeIndex CodeViewDebug::lowerTypeVFTableShape(const DIDerivedType *Ty) {
  unsigned VSlotCount =
      Ty->getSizeInBits() / (8 * Asm->MAI->getCodePointerSize());
  SmallVector<VFTableSlotKind, 4> Slots(VSlotCount, VFTableSlotKind::Near);

  VFTableShapeRecord VFTSR(Slots);
  return TypeTable.writeKnownType(VFTSR);
}

static MemberAccess translateAccessFlags(unsigned RecordTag, unsigned Flags) {
  switch (Flags & DINode::FlagAccessibility) {
  case DINode::FlagPrivate:   return MemberAccess::Private;
  case DINode::FlagPublic:    return MemberAccess::Public;
  case DINode::FlagProtected: return MemberAccess::Protected;
  case 0:
    // If there was no explicit access control, provide the default for the tag.
    return RecordTag == dwarf::DW_TAG_class_type ? MemberAccess::Private
                                                 : MemberAccess::Public;
  }
  llvm_unreachable("access flags are exclusive");
}

static MethodOptions translateMethodOptionFlags(const DISubprogram *SP) {
  if (SP->isArtificial())
    return MethodOptions::CompilerGenerated;

  // FIXME: Handle other MethodOptions.

  return MethodOptions::None;
}

static MethodKind translateMethodKindFlags(const DISubprogram *SP,
                                           bool Introduced) {
  switch (SP->getVirtuality()) {
  case dwarf::DW_VIRTUALITY_none:
    break;
  case dwarf::DW_VIRTUALITY_virtual:
    return Introduced ? MethodKind::IntroducingVirtual : MethodKind::Virtual;
  case dwarf::DW_VIRTUALITY_pure_virtual:
    return Introduced ? MethodKind::PureIntroducingVirtual
                      : MethodKind::PureVirtual;
  default:
    llvm_unreachable("unhandled virtuality case");
  }

  // FIXME: Get Clang to mark DISubprogram as static and do something with it.

  return MethodKind::Vanilla;
}

static TypeRecordKind getRecordKind(const DICompositeType *Ty) {
  switch (Ty->getTag()) {
  case dwarf::DW_TAG_class_type:     return TypeRecordKind::Class;
  case dwarf::DW_TAG_structure_type: return TypeRecordKind::Struct;
  }
  llvm_unreachable("unexpected tag");
}

/// Return ClassOptions that should be present on both the forward declaration
/// and the defintion of a tag type.
static ClassOptions getCommonClassOptions(const DICompositeType *Ty) {
  ClassOptions CO = ClassOptions::None;

  // MSVC always sets this flag, even for local types. Clang doesn't always
  // appear to give every type a linkage name, which may be problematic for us.
  // FIXME: Investigate the consequences of not following them here.
  if (!Ty->getIdentifier().empty())
    CO |= ClassOptions::HasUniqueName;

  // Put the Nested flag on a type if it appears immediately inside a tag type.
  // Do not walk the scope chain. Do not attempt to compute ContainsNestedClass
  // here. That flag is only set on definitions, and not forward declarations.
  const DIScope *ImmediateScope = Ty->getScope().resolve();
  if (ImmediateScope && isa<DICompositeType>(ImmediateScope))
    CO |= ClassOptions::Nested;

  // Put the Scoped flag on function-local types.
  for (const DIScope *Scope = ImmediateScope; Scope != nullptr;
       Scope = Scope->getScope().resolve()) {
    if (isa<DISubprogram>(Scope)) {
      CO |= ClassOptions::Scoped;
      break;
    }
  }

  return CO;
}

TypeIndex CodeViewDebug::lowerTypeEnum(const DICompositeType *Ty) {
  ClassOptions CO = getCommonClassOptions(Ty);
  TypeIndex FTI;
  unsigned EnumeratorCount = 0;

  if (Ty->isForwardDecl()) {
    CO |= ClassOptions::ForwardReference;
  } else {
    FieldListRecordBuilder FLRB(TypeTable);

    FLRB.begin();
    for (const DINode *Element : Ty->getElements()) {
      // We assume that the frontend provides all members in source declaration
      // order, which is what MSVC does.
      if (auto *Enumerator = dyn_cast_or_null<DIEnumerator>(Element)) {
        EnumeratorRecord ER(MemberAccess::Public,
                            APSInt::getUnsigned(Enumerator->getValue()),
                            Enumerator->getName());
        FLRB.writeMemberType(ER);
        EnumeratorCount++;
      }
    }
    FTI = FLRB.end(true);
  }

  std::string FullName = getFullyQualifiedName(Ty);

  EnumRecord ER(EnumeratorCount, CO, FTI, FullName, Ty->getIdentifier(),
                getTypeIndex(Ty->getBaseType()));
  return TypeTable.writeKnownType(ER);
}

//===----------------------------------------------------------------------===//
// ClassInfo
//===----------------------------------------------------------------------===//

struct llvm::ClassInfo {
  struct MemberInfo {
    const DIDerivedType *MemberTypeNode;
    uint64_t BaseOffset;
  };
  // [MemberInfo]
  using MemberList = std::vector<MemberInfo>;

  using MethodsList = TinyPtrVector<const DISubprogram *>;
  // MethodName -> MethodsList
  using MethodsMap = MapVector<MDString *, MethodsList>;

  /// Base classes.
  std::vector<const DIDerivedType *> Inheritance;

  /// Direct members.
  MemberList Members;
  // Direct overloaded methods gathered by name.
  MethodsMap Methods;

  TypeIndex VShapeTI;

  std::vector<const DICompositeType *> NestedClasses;
};

void CodeViewDebug::clear() {
  assert(CurFn == nullptr);
  FileIdMap.clear();
  FnDebugInfo.clear();
  FileToFilepathMap.clear();
  LocalUDTs.clear();
  GlobalUDTs.clear();
  TypeIndices.clear();
  CompleteTypeIndices.clear();
}

void CodeViewDebug::collectMemberInfo(ClassInfo &Info,
                                      const DIDerivedType *DDTy) {
  if (!DDTy->getName().empty()) {
    Info.Members.push_back({DDTy, 0});
    return;
  }
  // An unnamed member must represent a nested struct or union. Add all the
  // indirect fields to the current record.
  assert((DDTy->getOffsetInBits() % 8) == 0 && "Unnamed bitfield member!");
  uint64_t Offset = DDTy->getOffsetInBits();
  const DIType *Ty = DDTy->getBaseType().resolve();
  const DICompositeType *DCTy = cast<DICompositeType>(Ty);
  ClassInfo NestedInfo = collectClassInfo(DCTy);
  for (const ClassInfo::MemberInfo &IndirectField : NestedInfo.Members)
    Info.Members.push_back(
        {IndirectField.MemberTypeNode, IndirectField.BaseOffset + Offset});
}

ClassInfo CodeViewDebug::collectClassInfo(const DICompositeType *Ty) {
  ClassInfo Info;
  // Add elements to structure type.
  DINodeArray Elements = Ty->getElements();
  for (auto *Element : Elements) {
    // We assume that the frontend provides all members in source declaration
    // order, which is what MSVC does.
    if (!Element)
      continue;
    if (auto *SP = dyn_cast<DISubprogram>(Element)) {
      Info.Methods[SP->getRawName()].push_back(SP);
    } else if (auto *DDTy = dyn_cast<DIDerivedType>(Element)) {
      if (DDTy->getTag() == dwarf::DW_TAG_member) {
        collectMemberInfo(Info, DDTy);
      } else if (DDTy->getTag() == dwarf::DW_TAG_inheritance) {
        Info.Inheritance.push_back(DDTy);
      } else if (DDTy->getTag() == dwarf::DW_TAG_pointer_type &&
                 DDTy->getName() == "__vtbl_ptr_type") {
        Info.VShapeTI = getTypeIndex(DDTy);
      } else if (DDTy->getTag() == dwarf::DW_TAG_friend) {
        // Ignore friend members. It appears that MSVC emitted info about
        // friends in the past, but modern versions do not.
      }
    } else if (auto *Composite = dyn_cast<DICompositeType>(Element)) {
      Info.NestedClasses.push_back(Composite);
    }
    // Skip other unrecognized kinds of elements.
  }
  return Info;
}

TypeIndex CodeViewDebug::lowerTypeClass(const DICompositeType *Ty) {
  // First, construct the forward decl.  Don't look into Ty to compute the
  // forward decl options, since it might not be available in all TUs.
  TypeRecordKind Kind = getRecordKind(Ty);
  ClassOptions CO =
      ClassOptions::ForwardReference | getCommonClassOptions(Ty);
  std::string FullName = getFullyQualifiedName(Ty);
  ClassRecord CR(Kind, 0, CO, TypeIndex(), TypeIndex(), TypeIndex(), 0,
                 FullName, Ty->getIdentifier());
  TypeIndex FwdDeclTI = TypeTable.writeKnownType(CR);
  if (!Ty->isForwardDecl())
    DeferredCompleteTypes.push_back(Ty);
  return FwdDeclTI;
}

TypeIndex CodeViewDebug::lowerCompleteTypeClass(const DICompositeType *Ty) {
  // Construct the field list and complete type record.
  TypeRecordKind Kind = getRecordKind(Ty);
  ClassOptions CO = getCommonClassOptions(Ty);
  TypeIndex FieldTI;
  TypeIndex VShapeTI;
  unsigned FieldCount;
  bool ContainsNestedClass;
  std::tie(FieldTI, VShapeTI, FieldCount, ContainsNestedClass) =
      lowerRecordFieldList(Ty);

  if (ContainsNestedClass)
    CO |= ClassOptions::ContainsNestedClass;

  std::string FullName = getFullyQualifiedName(Ty);

  uint64_t SizeInBytes = Ty->getSizeInBits() / 8;

  ClassRecord CR(Kind, FieldCount, CO, FieldTI, TypeIndex(), VShapeTI,
                 SizeInBytes, FullName, Ty->getIdentifier());
  TypeIndex ClassTI = TypeTable.writeKnownType(CR);

  if (const auto *File = Ty->getFile()) {
    StringIdRecord SIDR(TypeIndex(0x0), getFullFilepath(File));
    TypeIndex SIDI = TypeTable.writeKnownType(SIDR);
    UdtSourceLineRecord USLR(ClassTI, SIDI, Ty->getLine());
    TypeTable.writeKnownType(USLR);
  }

  addToUDTs(Ty, ClassTI);

  return ClassTI;
}

TypeIndex CodeViewDebug::lowerTypeUnion(const DICompositeType *Ty) {
  ClassOptions CO =
      ClassOptions::ForwardReference | getCommonClassOptions(Ty);
  std::string FullName = getFullyQualifiedName(Ty);
  UnionRecord UR(0, CO, TypeIndex(), 0, FullName, Ty->getIdentifier());
  TypeIndex FwdDeclTI = TypeTable.writeKnownType(UR);
  if (!Ty->isForwardDecl())
    DeferredCompleteTypes.push_back(Ty);
  return FwdDeclTI;
}

TypeIndex CodeViewDebug::lowerCompleteTypeUnion(const DICompositeType *Ty) {
  ClassOptions CO = ClassOptions::Sealed | getCommonClassOptions(Ty);
  TypeIndex FieldTI;
  unsigned FieldCount;
  bool ContainsNestedClass;
  std::tie(FieldTI, std::ignore, FieldCount, ContainsNestedClass) =
      lowerRecordFieldList(Ty);

  if (ContainsNestedClass)
    CO |= ClassOptions::ContainsNestedClass;

  uint64_t SizeInBytes = Ty->getSizeInBits() / 8;
  std::string FullName = getFullyQualifiedName(Ty);

  UnionRecord UR(FieldCount, CO, FieldTI, SizeInBytes, FullName,
                 Ty->getIdentifier());
  TypeIndex UnionTI = TypeTable.writeKnownType(UR);

  StringIdRecord SIR(TypeIndex(0x0), getFullFilepath(Ty->getFile()));
  TypeIndex SIRI = TypeTable.writeKnownType(SIR);
  UdtSourceLineRecord USLR(UnionTI, SIRI, Ty->getLine());
  TypeTable.writeKnownType(USLR);

  addToUDTs(Ty, UnionTI);

  return UnionTI;
}

std::tuple<TypeIndex, TypeIndex, unsigned, bool>
CodeViewDebug::lowerRecordFieldList(const DICompositeType *Ty) {
  // Manually count members. MSVC appears to count everything that generates a
  // field list record. Each individual overload in a method overload group
  // contributes to this count, even though the overload group is a single field
  // list record.
  unsigned MemberCount = 0;
  ClassInfo Info = collectClassInfo(Ty);
  FieldListRecordBuilder FLBR(TypeTable);
  FLBR.begin();

  // Create base classes.
  for (const DIDerivedType *I : Info.Inheritance) {
    if (I->getFlags() & DINode::FlagVirtual) {
      // Virtual base.
      // FIXME: Emit VBPtrOffset when the frontend provides it.
      unsigned VBPtrOffset = 0;
      // FIXME: Despite the accessor name, the offset is really in bytes.
      unsigned VBTableIndex = I->getOffsetInBits() / 4;
      auto RecordKind = (I->getFlags() & DINode::FlagIndirectVirtualBase) == DINode::FlagIndirectVirtualBase
                            ? TypeRecordKind::IndirectVirtualBaseClass
                            : TypeRecordKind::VirtualBaseClass;
      VirtualBaseClassRecord VBCR(
          RecordKind, translateAccessFlags(Ty->getTag(), I->getFlags()),
          getTypeIndex(I->getBaseType()), getVBPTypeIndex(), VBPtrOffset,
          VBTableIndex);

      FLBR.writeMemberType(VBCR);
    } else {
      assert(I->getOffsetInBits() % 8 == 0 &&
             "bases must be on byte boundaries");
      BaseClassRecord BCR(translateAccessFlags(Ty->getTag(), I->getFlags()),
                          getTypeIndex(I->getBaseType()),
                          I->getOffsetInBits() / 8);
      FLBR.writeMemberType(BCR);
    }
  }

  // Create members.
  for (ClassInfo::MemberInfo &MemberInfo : Info.Members) {
    const DIDerivedType *Member = MemberInfo.MemberTypeNode;
    TypeIndex MemberBaseType = getTypeIndex(Member->getBaseType());
    StringRef MemberName = Member->getName();
    MemberAccess Access =
        translateAccessFlags(Ty->getTag(), Member->getFlags());

    if (Member->isStaticMember()) {
      StaticDataMemberRecord SDMR(Access, MemberBaseType, MemberName);
      FLBR.writeMemberType(SDMR);
      MemberCount++;
      continue;
    }

    // Virtual function pointer member.
    if ((Member->getFlags() & DINode::FlagArtificial) &&
        Member->getName().startswith("_vptr$")) {
      VFPtrRecord VFPR(getTypeIndex(Member->getBaseType()));
      FLBR.writeMemberType(VFPR);
      MemberCount++;
      continue;
    }

    // Data member.
    uint64_t MemberOffsetInBits =
        Member->getOffsetInBits() + MemberInfo.BaseOffset;
    if (Member->isBitField()) {
      uint64_t StartBitOffset = MemberOffsetInBits;
      if (const auto *CI =
              dyn_cast_or_null<ConstantInt>(Member->getStorageOffsetInBits())) {
        MemberOffsetInBits = CI->getZExtValue() + MemberInfo.BaseOffset;
      }
      StartBitOffset -= MemberOffsetInBits;
      BitFieldRecord BFR(MemberBaseType, Member->getSizeInBits(),
                         StartBitOffset);
      MemberBaseType = TypeTable.writeKnownType(BFR);
    }
    uint64_t MemberOffsetInBytes = MemberOffsetInBits / 8;
    DataMemberRecord DMR(Access, MemberBaseType, MemberOffsetInBytes,
                         MemberName);
    FLBR.writeMemberType(DMR);
    MemberCount++;
  }

  // Create methods
  for (auto &MethodItr : Info.Methods) {
    StringRef Name = MethodItr.first->getString();

    std::vector<OneMethodRecord> Methods;
    for (const DISubprogram *SP : MethodItr.second) {
      TypeIndex MethodType = getMemberFunctionType(SP, Ty);
      bool Introduced = SP->getFlags() & DINode::FlagIntroducedVirtual;

      unsigned VFTableOffset = -1;
      if (Introduced)
        VFTableOffset = SP->getVirtualIndex() * getPointerSizeInBytes();

      Methods.push_back(OneMethodRecord(
          MethodType, translateAccessFlags(Ty->getTag(), SP->getFlags()),
          translateMethodKindFlags(SP, Introduced),
          translateMethodOptionFlags(SP), VFTableOffset, Name));
      MemberCount++;
    }
    assert(!Methods.empty() && "Empty methods map entry");
    if (Methods.size() == 1)
      FLBR.writeMemberType(Methods[0]);
    else {
      MethodOverloadListRecord MOLR(Methods);
      TypeIndex MethodList = TypeTable.writeKnownType(MOLR);
      OverloadedMethodRecord OMR(Methods.size(), MethodList, Name);
      FLBR.writeMemberType(OMR);
    }
  }

  // Create nested classes.
  for (const DICompositeType *Nested : Info.NestedClasses) {
    NestedTypeRecord R(getTypeIndex(DITypeRef(Nested)), Nested->getName());
    FLBR.writeMemberType(R);
    MemberCount++;
  }

  TypeIndex FieldTI = FLBR.end(true);
  return std::make_tuple(FieldTI, Info.VShapeTI, MemberCount,
                         !Info.NestedClasses.empty());
}

TypeIndex CodeViewDebug::getVBPTypeIndex() {
  if (!VBPType.getIndex()) {
    // Make a 'const int *' type.
    ModifierRecord MR(TypeIndex::Int32(), ModifierOptions::Const);
    TypeIndex ModifiedTI = TypeTable.writeKnownType(MR);

    PointerKind PK = getPointerSizeInBytes() == 8 ? PointerKind::Near64
                                                  : PointerKind::Near32;
    PointerMode PM = PointerMode::Pointer;
    PointerOptions PO = PointerOptions::None;
    PointerRecord PR(ModifiedTI, PK, PM, PO, getPointerSizeInBytes());

    VBPType = TypeTable.writeKnownType(PR);
  }

  return VBPType;
}

TypeIndex CodeViewDebug::getTypeIndex(DITypeRef TypeRef, DITypeRef ClassTyRef) {
  const DIType *Ty = TypeRef.resolve();
  const DIType *ClassTy = ClassTyRef.resolve();

  // The null DIType is the void type. Don't try to hash it.
  if (!Ty)
    return TypeIndex::Void();

  // Check if we've already translated this type. Don't try to do a
  // get-or-create style insertion that caches the hash lookup across the
  // lowerType call. It will update the TypeIndices map.
  auto I = TypeIndices.find({Ty, ClassTy});
  if (I != TypeIndices.end())
    return I->second;

  TypeLoweringScope S(*this);
  TypeIndex TI = lowerType(Ty, ClassTy);
  return recordTypeIndexForDINode(Ty, TI, ClassTy);
}

TypeIndex CodeViewDebug::getCompleteTypeIndex(DITypeRef TypeRef) {
  const DIType *Ty = TypeRef.resolve();

  // The null DIType is the void type. Don't try to hash it.
  if (!Ty)
    return TypeIndex::Void();

  // If this is a non-record type, the complete type index is the same as the
  // normal type index. Just call getTypeIndex.
  switch (Ty->getTag()) {
  case dwarf::DW_TAG_class_type:
  case dwarf::DW_TAG_structure_type:
  case dwarf::DW_TAG_union_type:
    break;
  default:
    return getTypeIndex(Ty);
  }

  // Check if we've already translated the complete record type.  Lowering a
  // complete type should never trigger lowering another complete type, so we
  // can reuse the hash table lookup result.
  const auto *CTy = cast<DICompositeType>(Ty);
  auto InsertResult = CompleteTypeIndices.insert({CTy, TypeIndex()});
  if (!InsertResult.second)
    return InsertResult.first->second;

  TypeLoweringScope S(*this);

  // Make sure the forward declaration is emitted first. It's unclear if this
  // is necessary, but MSVC does it, and we should follow suit until we can show
  // otherwise.
  TypeIndex FwdDeclTI = getTypeIndex(CTy);

  // Just use the forward decl if we don't have complete type info. This might
  // happen if the frontend is using modules and expects the complete definition
  // to be emitted elsewhere.
  if (CTy->isForwardDecl())
    return FwdDeclTI;

  TypeIndex TI;
  switch (CTy->getTag()) {
  case dwarf::DW_TAG_class_type:
  case dwarf::DW_TAG_structure_type:
    TI = lowerCompleteTypeClass(CTy);
    break;
  case dwarf::DW_TAG_union_type:
    TI = lowerCompleteTypeUnion(CTy);
    break;
  default:
    llvm_unreachable("not a record");
  }

  InsertResult.first->second = TI;
  return TI;
}

/// Emit all the deferred complete record types. Try to do this in FIFO order,
/// and do this until fixpoint, as each complete record type typically
/// references
/// many other record types.
void CodeViewDebug::emitDeferredCompleteTypes() {
  SmallVector<const DICompositeType *, 4> TypesToEmit;
  while (!DeferredCompleteTypes.empty()) {
    std::swap(DeferredCompleteTypes, TypesToEmit);
    for (const DICompositeType *RecordTy : TypesToEmit)
      getCompleteTypeIndex(RecordTy);
    TypesToEmit.clear();
  }
}

void CodeViewDebug::emitLocalVariableList(ArrayRef<LocalVariable> Locals) {
  // Get the sorted list of parameters and emit them first.
  SmallVector<const LocalVariable *, 6> Params;
  for (const LocalVariable &L : Locals)
    if (L.DIVar->isParameter())
      Params.push_back(&L);
  std::sort(Params.begin(), Params.end(),
            [](const LocalVariable *L, const LocalVariable *R) {
              return L->DIVar->getArg() < R->DIVar->getArg();
            });
  for (const LocalVariable *L : Params)
    emitLocalVariable(*L);

  // Next emit all non-parameters in the order that we found them.
  for (const LocalVariable &L : Locals)
    if (!L.DIVar->isParameter())
      emitLocalVariable(L);
}

void CodeViewDebug::emitLocalVariable(const LocalVariable &Var) {
  // LocalSym record, see SymbolRecord.h for more info.
  MCSymbol *LocalBegin = MMI->getContext().createTempSymbol(),
           *LocalEnd = MMI->getContext().createTempSymbol();
  OS.AddComment("Record length");
  OS.emitAbsoluteSymbolDiff(LocalEnd, LocalBegin, 2);
  OS.EmitLabel(LocalBegin);

  OS.AddComment("Record kind: S_LOCAL");
  OS.EmitIntValue(unsigned(SymbolKind::S_LOCAL), 2);

  LocalSymFlags Flags = LocalSymFlags::None;
  if (Var.DIVar->isParameter())
    Flags |= LocalSymFlags::IsParameter;
  if (Var.DefRanges.empty())
    Flags |= LocalSymFlags::IsOptimizedOut;

  OS.AddComment("TypeIndex");
  TypeIndex TI = getCompleteTypeIndex(Var.DIVar->getType());
  OS.EmitIntValue(TI.getIndex(), 4);
  OS.AddComment("Flags");
  OS.EmitIntValue(static_cast<uint16_t>(Flags), 2);
  // Truncate the name so we won't overflow the record length field.
  emitNullTerminatedSymbolName(OS, Var.DIVar->getName());
  OS.EmitLabel(LocalEnd);

  // Calculate the on disk prefix of the appropriate def range record. The
  // records and on disk formats are described in SymbolRecords.h. BytePrefix
  // should be big enough to hold all forms without memory allocation.
  SmallString<20> BytePrefix;
  for (const LocalVarDefRange &DefRange : Var.DefRanges) {
    BytePrefix.clear();
    if (DefRange.InMemory) {
      uint16_t RegRelFlags = 0;
      if (DefRange.IsSubfield) {
        RegRelFlags = DefRangeRegisterRelSym::IsSubfieldFlag |
                      (DefRange.StructOffset
                       << DefRangeRegisterRelSym::OffsetInParentShift);
      }
      DefRangeRegisterRelSym Sym(S_DEFRANGE_REGISTER_REL);
      Sym.Hdr.Register = DefRange.CVRegister;
      Sym.Hdr.Flags = RegRelFlags;
      Sym.Hdr.BasePointerOffset = DefRange.DataOffset;
      ulittle16_t SymKind = ulittle16_t(S_DEFRANGE_REGISTER_REL);
      BytePrefix +=
          StringRef(reinterpret_cast<const char *>(&SymKind), sizeof(SymKind));
      BytePrefix +=
          StringRef(reinterpret_cast<const char *>(&Sym.Hdr), sizeof(Sym.Hdr));
    } else {
      assert(DefRange.DataOffset == 0 && "unexpected offset into register");
      if (DefRange.IsSubfield) {
        // Unclear what matters here.
        DefRangeSubfieldRegisterSym Sym(S_DEFRANGE_SUBFIELD_REGISTER);
        Sym.Hdr.Register = DefRange.CVRegister;
        Sym.Hdr.MayHaveNoName = 0;
        Sym.Hdr.OffsetInParent = DefRange.StructOffset;

        ulittle16_t SymKind = ulittle16_t(S_DEFRANGE_SUBFIELD_REGISTER);
        BytePrefix += StringRef(reinterpret_cast<const char *>(&SymKind),
                                sizeof(SymKind));
        BytePrefix += StringRef(reinterpret_cast<const char *>(&Sym.Hdr),
                                sizeof(Sym.Hdr));
      } else {
        // Unclear what matters here.
        DefRangeRegisterSym Sym(S_DEFRANGE_REGISTER);
        Sym.Hdr.Register = DefRange.CVRegister;
        Sym.Hdr.MayHaveNoName = 0;
        ulittle16_t SymKind = ulittle16_t(S_DEFRANGE_REGISTER);
        BytePrefix += StringRef(reinterpret_cast<const char *>(&SymKind),
                                sizeof(SymKind));
        BytePrefix += StringRef(reinterpret_cast<const char *>(&Sym.Hdr),
                                sizeof(Sym.Hdr));
      }
    }
    OS.EmitCVDefRangeDirective(DefRange.Ranges, BytePrefix);
  }
}

void CodeViewDebug::endFunctionImpl(const MachineFunction *MF) {
  const Function *GV = MF->getFunction();
  assert(FnDebugInfo.count(GV));
  assert(CurFn == &FnDebugInfo[GV]);

  collectVariableInfo(GV->getSubprogram());

  // Don't emit anything if we don't have any line tables.
  if (!CurFn->HaveLineInfo) {
    FnDebugInfo.erase(GV);
    CurFn = nullptr;
    return;
  }

  CurFn->End = Asm->getFunctionEnd();

  CurFn = nullptr;
}

void CodeViewDebug::beginInstruction(const MachineInstr *MI) {
  DebugHandlerBase::beginInstruction(MI);

  // Ignore DBG_VALUE locations and function prologue.
  if (!Asm || !CurFn || MI->isDebugValue() ||
      MI->getFlag(MachineInstr::FrameSetup))
    return;

  // If the first instruction of a new MBB has no location, find the first
  // instruction with a location and use that.
  DebugLoc DL = MI->getDebugLoc();
  if (!DL && MI->getParent() != PrevInstBB) {
    for (const auto &NextMI : *MI->getParent()) {
      DL = NextMI.getDebugLoc();
      if (DL)
        break;
    }
  }
  PrevInstBB = MI->getParent();

  // If we still don't have a debug location, don't record a location.
  if (!DL)
    return;

  maybeRecordLocation(DL, Asm->MF);
}

MCSymbol *CodeViewDebug::beginCVSubsection(DebugSubsectionKind Kind) {
  MCSymbol *BeginLabel = MMI->getContext().createTempSymbol(),
           *EndLabel = MMI->getContext().createTempSymbol();
  OS.EmitIntValue(unsigned(Kind), 4);
  OS.AddComment("Subsection size");
  OS.emitAbsoluteSymbolDiff(EndLabel, BeginLabel, 4);
  OS.EmitLabel(BeginLabel);
  return EndLabel;
}

void CodeViewDebug::endCVSubsection(MCSymbol *EndLabel) {
  OS.EmitLabel(EndLabel);
  // Every subsection must be aligned to a 4-byte boundary.
  OS.EmitValueToAlignment(4);
}

void CodeViewDebug::emitDebugInfoForUDTs(
    ArrayRef<std::pair<std::string, TypeIndex>> UDTs) {
  for (const std::pair<std::string, codeview::TypeIndex> &UDT : UDTs) {
    MCSymbol *UDTRecordBegin = MMI->getContext().createTempSymbol(),
             *UDTRecordEnd = MMI->getContext().createTempSymbol();
    OS.AddComment("Record length");
    OS.emitAbsoluteSymbolDiff(UDTRecordEnd, UDTRecordBegin, 2);
    OS.EmitLabel(UDTRecordBegin);

    OS.AddComment("Record kind: S_UDT");
    OS.EmitIntValue(unsigned(SymbolKind::S_UDT), 2);

    OS.AddComment("Type");
    OS.EmitIntValue(UDT.second.getIndex(), 4);

    emitNullTerminatedSymbolName(OS, UDT.first);
    OS.EmitLabel(UDTRecordEnd);
  }
}

void CodeViewDebug::emitDebugInfoForGlobals() {
  DenseMap<const DIGlobalVariableExpression *, const GlobalVariable *>
      GlobalMap;
  for (const GlobalVariable &GV : MMI->getModule()->globals()) {
    SmallVector<DIGlobalVariableExpression *, 1> GVEs;
    GV.getDebugInfo(GVEs);
    for (const auto *GVE : GVEs)
      GlobalMap[GVE] = &GV;
  }

  NamedMDNode *CUs = MMI->getModule()->getNamedMetadata("llvm.dbg.cu");
  for (const MDNode *Node : CUs->operands()) {
    const auto *CU = cast<DICompileUnit>(Node);

    // First, emit all globals that are not in a comdat in a single symbol
    // substream. MSVC doesn't like it if the substream is empty, so only open
    // it if we have at least one global to emit.
    switchToDebugSectionForSymbol(nullptr);
    MCSymbol *EndLabel = nullptr;
    for (const auto *GVE : CU->getGlobalVariables()) {
      if (const auto *GV = GlobalMap.lookup(GVE))
        if (!GV->hasComdat() && !GV->isDeclarationForLinker()) {
          if (!EndLabel) {
            OS.AddComment("Symbol subsection for globals");
            EndLabel = beginCVSubsection(DebugSubsectionKind::Symbols);
          }
          // FIXME: emitDebugInfoForGlobal() doesn't handle DIExpressions.
          emitDebugInfoForGlobal(GVE->getVariable(), GV, Asm->getSymbol(GV));
        }
    }
    if (EndLabel)
      endCVSubsection(EndLabel);

    // Second, emit each global that is in a comdat into its own .debug$S
    // section along with its own symbol substream.
    for (const auto *GVE : CU->getGlobalVariables()) {
      if (const auto *GV = GlobalMap.lookup(GVE)) {
        if (GV->hasComdat()) {
          MCSymbol *GVSym = Asm->getSymbol(GV);
          OS.AddComment("Symbol subsection for " +
                        Twine(GlobalValue::dropLLVMManglingEscape(GV->getName())));
          switchToDebugSectionForSymbol(GVSym);
          EndLabel = beginCVSubsection(DebugSubsectionKind::Symbols);
          // FIXME: emitDebugInfoForGlobal() doesn't handle DIExpressions.
          emitDebugInfoForGlobal(GVE->getVariable(), GV, GVSym);
          endCVSubsection(EndLabel);
        }
      }
    }
  }
}

void CodeViewDebug::emitDebugInfoForRetainedTypes() {
  NamedMDNode *CUs = MMI->getModule()->getNamedMetadata("llvm.dbg.cu");
  for (const MDNode *Node : CUs->operands()) {
    for (auto *Ty : cast<DICompileUnit>(Node)->getRetainedTypes()) {
      if (DIType *RT = dyn_cast<DIType>(Ty)) {
        getTypeIndex(RT);
        // FIXME: Add to global/local DTU list.
      }
    }
  }
}

void CodeViewDebug::emitDebugInfoForGlobal(const DIGlobalVariable *DIGV,
                                           const GlobalVariable *GV,
                                           MCSymbol *GVSym) {
  // DataSym record, see SymbolRecord.h for more info.
  // FIXME: Thread local data, etc
  MCSymbol *DataBegin = MMI->getContext().createTempSymbol(),
           *DataEnd = MMI->getContext().createTempSymbol();
  OS.AddComment("Record length");
  OS.emitAbsoluteSymbolDiff(DataEnd, DataBegin, 2);
  OS.EmitLabel(DataBegin);
  if (DIGV->isLocalToUnit()) {
    if (GV->isThreadLocal()) {
      OS.AddComment("Record kind: S_LTHREAD32");
      OS.EmitIntValue(unsigned(SymbolKind::S_LTHREAD32), 2);
    } else {
      OS.AddComment("Record kind: S_LDATA32");
      OS.EmitIntValue(unsigned(SymbolKind::S_LDATA32), 2);
    }
  } else {
    if (GV->isThreadLocal()) {
      OS.AddComment("Record kind: S_GTHREAD32");
      OS.EmitIntValue(unsigned(SymbolKind::S_GTHREAD32), 2);
    } else {
      OS.AddComment("Record kind: S_GDATA32");
      OS.EmitIntValue(unsigned(SymbolKind::S_GDATA32), 2);
    }
  }
  OS.AddComment("Type");
  OS.EmitIntValue(getCompleteTypeIndex(DIGV->getType()).getIndex(), 4);
  OS.AddComment("DataOffset");
  OS.EmitCOFFSecRel32(GVSym, /*Offset=*/0);
  OS.AddComment("Segment");
  OS.EmitCOFFSectionIndex(GVSym);
  OS.AddComment("Name");
  emitNullTerminatedSymbolName(OS, DIGV->getName());
  OS.EmitLabel(DataEnd);
}
