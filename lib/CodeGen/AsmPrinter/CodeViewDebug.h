//===- llvm/lib/CodeGen/AsmPrinter/CodeViewDebug.h --------------*- C++ -*-===//
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

#ifndef LLVM_LIB_CODEGEN_ASMPRINTER_CODEVIEWDEBUG_H
#define LLVM_LIB_CODEGEN_ASMPRINTER_CODEVIEWDEBUG_H

#include "DbgValueHistoryCalculator.h"
#include "DebugHandlerBase.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/MapVector.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/DebugInfo/CodeView/CodeView.h"
#include "llvm/DebugInfo/CodeView/TypeIndex.h"
#include "llvm/DebugInfo/CodeView/TypeTableBuilder.h"
#include "llvm/IR/DebugLoc.h"
#include "llvm/Support/Allocator.h"
#include "llvm/Support/Compiler.h"
#include <cstdint>
#include <map>
#include <string>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

namespace llvm {

struct ClassInfo;
class StringRef;
class AsmPrinter;
class Function;
class GlobalVariable;
class MCSectionCOFF;
class MCStreamer;
class MCSymbol;
class MachineFunction;

/// \brief Collects and handles line tables information in a CodeView format.
class LLVM_LIBRARY_VISIBILITY CodeViewDebug : public DebugHandlerBase {
  MCStreamer &OS;
  BumpPtrAllocator Allocator;
  codeview::TypeTableBuilder TypeTable;

  /// Represents the most general definition range.
  struct LocalVarDefRange {
    /// Indicates that variable data is stored in memory relative to the
    /// specified register.
    int InMemory : 1;

    /// Offset of variable data in memory.
    int DataOffset : 31;

    /// Non-zero if this is a piece of an aggregate.
    uint16_t IsSubfield : 1;

    /// Offset into aggregate.
    uint16_t StructOffset : 15;

    /// Register containing the data or the register base of the memory
    /// location containing the data.
    uint16_t CVRegister;

    /// Compares all location fields. This includes all fields except the label
    /// ranges.
    bool isDifferentLocation(LocalVarDefRange &O) {
      return InMemory != O.InMemory || DataOffset != O.DataOffset ||
             IsSubfield != O.IsSubfield || StructOffset != O.StructOffset ||
             CVRegister != O.CVRegister;
    }

    SmallVector<std::pair<const MCSymbol *, const MCSymbol *>, 1> Ranges;
  };

  static LocalVarDefRange createDefRangeMem(uint16_t CVRegister, int Offset);
  static LocalVarDefRange createDefRangeGeneral(uint16_t CVRegister,
                                                bool InMemory, int Offset,
                                                bool IsSubfield,
                                                uint16_t StructOffset);

  /// Similar to DbgVariable in DwarfDebug, but not dwarf-specific.
  struct LocalVariable {
    const DILocalVariable *DIVar = nullptr;
    SmallVector<LocalVarDefRange, 1> DefRanges;
  };

  struct InlineSite {
    SmallVector<LocalVariable, 1> InlinedLocals;
    SmallVector<const DILocation *, 1> ChildSites;
    const DISubprogram *Inlinee = nullptr;

    /// The ID of the inline site or function used with .cv_loc. Not a type
    /// index.
    unsigned SiteFuncId = 0;
  };

  // For each function, store a vector of labels to its instructions, as well as
  // to the end of the function.
  struct FunctionInfo {
    /// Map from inlined call site to inlined instructions and child inlined
    /// call sites. Listed in program order.
    std::unordered_map<const DILocation *, InlineSite> InlineSites;

    /// Ordered list of top-level inlined call sites.
    SmallVector<const DILocation *, 1> ChildSites;

    SmallVector<LocalVariable, 1> Locals;

    const MCSymbol *Begin = nullptr;
    const MCSymbol *End = nullptr;
    unsigned FuncId = 0;
    unsigned LastFileId = 0;
    bool HaveLineInfo = false;
  };
  FunctionInfo *CurFn = nullptr;

  /// The set of comdat .debug$S sections that we've seen so far. Each section
  /// must start with a magic version number that must only be emitted once.
  /// This set tracks which sections we've already opened.
  DenseSet<MCSectionCOFF *> ComdatDebugSections;

  /// Switch to the appropriate .debug$S section for GVSym. If GVSym, the symbol
  /// of an emitted global value, is in a comdat COFF section, this will switch
  /// to a new .debug$S section in that comdat. This method ensures that the
  /// section starts with the magic version number on first use. If GVSym is
  /// null, uses the main .debug$S section.
  void switchToDebugSectionForSymbol(const MCSymbol *GVSym);

  /// The next available function index for use with our .cv_* directives. Not
  /// to be confused with type indices for LF_FUNC_ID records.
  unsigned NextFuncId = 0;

  InlineSite &getInlineSite(const DILocation *InlinedAt,
                            const DISubprogram *Inlinee);

  codeview::TypeIndex getFuncIdForSubprogram(const DISubprogram *SP);

  static void collectInlineSiteChildren(SmallVectorImpl<unsigned> &Children,
                                        const FunctionInfo &FI,
                                        const InlineSite &Site);

  /// Remember some debug info about each function. Keep it in a stable order to
  /// emit at the end of the TU.
  MapVector<const Function *, FunctionInfo> FnDebugInfo;

  /// Map from DIFile to .cv_file id.
  DenseMap<const DIFile *, unsigned> FileIdMap;

  /// All inlined subprograms in the order they should be emitted.
  SmallSetVector<const DISubprogram *, 4> InlinedSubprograms;

  /// Map from a pair of DI metadata nodes and its DI type (or scope) that can
  /// be nullptr, to CodeView type indices. Primarily indexed by
  /// {DIType*, DIType*} and {DISubprogram*, DIType*}.
  ///
  /// The second entry in the key is needed for methods as DISubroutineType
  /// representing static method type are shared with non-method function type.
  DenseMap<std::pair<const DINode *, const DIType *>, codeview::TypeIndex>
      TypeIndices;

  /// Map from DICompositeType* to complete type index. Non-record types are
  /// always looked up in the normal TypeIndices map.
  DenseMap<const DICompositeType *, codeview::TypeIndex> CompleteTypeIndices;

  /// Complete record types to emit after all active type lowerings are
  /// finished.
  SmallVector<const DICompositeType *, 4> DeferredCompleteTypes;

  /// Number of type lowering frames active on the stack.
  unsigned TypeEmissionLevel = 0;

  codeview::TypeIndex VBPType;

  const DISubprogram *CurrentSubprogram = nullptr;

  // The UDTs we have seen while processing types; each entry is a pair of type
  // index and type name.
  std::vector<std::pair<std::string, codeview::TypeIndex>> LocalUDTs,
      GlobalUDTs;

  using FileToFilepathMapTy = std::map<const DIFile *, std::string>;
  FileToFilepathMapTy FileToFilepathMap;

  StringRef getFullFilepath(const DIFile *S);

  unsigned maybeRecordFile(const DIFile *F);

  void maybeRecordLocation(const DebugLoc &DL, const MachineFunction *MF);

  void clear();

  void setCurrentSubprogram(const DISubprogram *SP) {
    CurrentSubprogram = SP;
    LocalUDTs.clear();
  }

  /// Emit the magic version number at the start of a CodeView type or symbol
  /// section. Appears at the front of every .debug$S or .debug$T section.
  void emitCodeViewMagicVersion();

  void emitTypeInformation();

  void emitCompilerInformation();

  void emitInlineeLinesSubsection();

  void emitDebugInfoForFunction(const Function *GV, FunctionInfo &FI);

  void emitDebugInfoForGlobals();

  void emitDebugInfoForRetainedTypes();

  void emitDebugInfoForUDTs(
      ArrayRef<std::pair<std::string, codeview::TypeIndex>> UDTs);

  void emitDebugInfoForGlobal(const DIGlobalVariable *DIGV,
                              const GlobalVariable *GV, MCSymbol *GVSym);

  /// Opens a subsection of the given kind in a .debug$S codeview section.
  /// Returns an end label for use with endCVSubsection when the subsection is
  /// finished.
  MCSymbol *beginCVSubsection(codeview::DebugSubsectionKind Kind);

  void endCVSubsection(MCSymbol *EndLabel);

  void emitInlinedCallSite(const FunctionInfo &FI, const DILocation *InlinedAt,
                           const InlineSite &Site);

  using InlinedVariable = DbgValueHistoryMap::InlinedVariable;

  void collectVariableInfo(const DISubprogram *SP);

  void collectVariableInfoFromMFTable(DenseSet<InlinedVariable> &Processed);

  /// Records information about a local variable in the appropriate scope. In
  /// particular, locals from inlined code live inside the inlining site.
  void recordLocalVariable(LocalVariable &&Var, const DILocation *Loc);

  /// Emits local variables in the appropriate order.
  void emitLocalVariableList(ArrayRef<LocalVariable> Locals);

  /// Emits an S_LOCAL record and its associated defined ranges.
  void emitLocalVariable(const LocalVariable &Var);

  /// Translates the DIType to codeview if necessary and returns a type index
  /// for it.
  codeview::TypeIndex getTypeIndex(DITypeRef TypeRef,
                                   DITypeRef ClassTyRef = DITypeRef());

  codeview::TypeIndex getMemberFunctionType(const DISubprogram *SP,
                                            const DICompositeType *Class);

  codeview::TypeIndex getScopeIndex(const DIScope *Scope);

  codeview::TypeIndex getVBPTypeIndex();

  void addToUDTs(const DIType *Ty, codeview::TypeIndex TI);

  codeview::TypeIndex lowerType(const DIType *Ty, const DIType *ClassTy);
  codeview::TypeIndex lowerTypeAlias(const DIDerivedType *Ty);
  codeview::TypeIndex lowerTypeArray(const DICompositeType *Ty);
  codeview::TypeIndex lowerTypeBasic(const DIBasicType *Ty);
  codeview::TypeIndex lowerTypePointer(const DIDerivedType *Ty);
  codeview::TypeIndex lowerTypeMemberPointer(const DIDerivedType *Ty);
  codeview::TypeIndex lowerTypeModifier(const DIDerivedType *Ty);
  codeview::TypeIndex lowerTypeFunction(const DISubroutineType *Ty);
  codeview::TypeIndex lowerTypeVFTableShape(const DIDerivedType *Ty);
  codeview::TypeIndex lowerTypeMemberFunction(const DISubroutineType *Ty,
                                              const DIType *ClassTy,
                                              int ThisAdjustment);
  codeview::TypeIndex lowerTypeEnum(const DICompositeType *Ty);
  codeview::TypeIndex lowerTypeClass(const DICompositeType *Ty);
  codeview::TypeIndex lowerTypeUnion(const DICompositeType *Ty);

  /// Symbol records should point to complete types, but type records should
  /// always point to incomplete types to avoid cycles in the type graph. Only
  /// use this entry point when generating symbol records. The complete and
  /// incomplete type indices only differ for record types. All other types use
  /// the same index.
  codeview::TypeIndex getCompleteTypeIndex(DITypeRef TypeRef);

  codeview::TypeIndex lowerCompleteTypeClass(const DICompositeType *Ty);
  codeview::TypeIndex lowerCompleteTypeUnion(const DICompositeType *Ty);

  struct TypeLoweringScope;

  void emitDeferredCompleteTypes();

  void collectMemberInfo(ClassInfo &Info, const DIDerivedType *DDTy);
  ClassInfo collectClassInfo(const DICompositeType *Ty);

  /// Common record member lowering functionality for record types, which are
  /// structs, classes, and unions. Returns the field list index and the member
  /// count.
  std::tuple<codeview::TypeIndex, codeview::TypeIndex, unsigned, bool>
  lowerRecordFieldList(const DICompositeType *Ty);

  /// Inserts {{Node, ClassTy}, TI} into TypeIndices and checks for duplicates.
  codeview::TypeIndex recordTypeIndexForDINode(const DINode *Node,
                                               codeview::TypeIndex TI,
                                               const DIType *ClassTy = nullptr);

  unsigned getPointerSizeInBytes();

protected:
  /// \brief Gather pre-function debug information.
  void beginFunctionImpl(const MachineFunction *MF) override;

  /// \brief Gather post-function debug information.
  void endFunctionImpl(const MachineFunction *) override;

public:
  CodeViewDebug(AsmPrinter *Asm);

  void setSymbolSize(const MCSymbol *, uint64_t) override {}

  /// \brief Emit the COFF section that holds the line table information.
  void endModule() override;

  /// \brief Process beginning of an instruction.
  void beginInstruction(const MachineInstr *MI) override;
};

} // end namespace llvm

#endif // LLVM_LIB_CODEGEN_ASMPRINTER_CODEVIEWDEBUG_H
