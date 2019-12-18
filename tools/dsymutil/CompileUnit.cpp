//===- tools/dsymutil/CompileUnit.h - Dwarf compile unit ------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "CompileUnit.h"
#include "DeclContext.h"

namespace llvm {
namespace dsymutil {

/// Check if the DIE at \p Idx is in the scope of a function.
static bool inFunctionScope(CompileUnit &U, unsigned Idx) {
  while (Idx) {
    if (U.getOrigUnit().getDIEAtIndex(Idx).getTag() == dwarf::DW_TAG_subprogram)
      return true;
    Idx = U.getInfo(Idx).ParentIdx;
  }
  return false;
}

void CompileUnit::markEverythingAsKept() {
  unsigned Idx = 0;

  setHasInterestingContent();

  for (auto &I : Info) {
    // Mark everything that wasn't explicit marked for pruning.
    I.Keep = !I.Prune;
    auto DIE = OrigUnit.getDIEAtIndex(Idx++);

    // Try to guess which DIEs must go to the accelerator tables. We do that
    // just for variables, because functions will be handled depending on
    // whether they carry a DW_AT_low_pc attribute or not.
    if (DIE.getTag() != dwarf::DW_TAG_variable &&
        DIE.getTag() != dwarf::DW_TAG_constant)
      continue;

    Optional<DWARFFormValue> Value;
    if (!(Value = DIE.find(dwarf::DW_AT_location))) {
      if ((Value = DIE.find(dwarf::DW_AT_const_value)) &&
          !inFunctionScope(*this, I.ParentIdx))
        I.InDebugMap = true;
      continue;
    }
    if (auto Block = Value->getAsBlock()) {
      if (Block->size() > OrigUnit.getAddressByteSize() &&
          (*Block)[0] == dwarf::DW_OP_addr)
        I.InDebugMap = true;
    }
  }
}

uint64_t CompileUnit::computeNextUnitOffset() {
  NextUnitOffset = StartOffset + 11 /* Header size */;
  // The root DIE might be null, meaning that the Unit had nothing to
  // contribute to the linked output. In that case, we will emit the
  // unit header without any actual DIE.
  if (NewUnit)
    NextUnitOffset += NewUnit->getUnitDie().getSize();
  return NextUnitOffset;
}

/// Keep track of a forward cross-cu reference from this unit
/// to \p Die that lives in \p RefUnit.
void CompileUnit::noteForwardReference(DIE *Die, const CompileUnit *RefUnit,
                                       DeclContext *Ctxt, PatchLocation Attr) {
  ForwardDIEReferences.emplace_back(Die, RefUnit, Ctxt, Attr);
}

void CompileUnit::fixupForwardReferences() {
  for (const auto &Ref : ForwardDIEReferences) {
    DIE *RefDie;
    const CompileUnit *RefUnit;
    PatchLocation Attr;
    DeclContext *Ctxt;
    std::tie(RefDie, RefUnit, Ctxt, Attr) = Ref;
    if (Ctxt && Ctxt->getCanonicalDIEOffset())
      Attr.set(Ctxt->getCanonicalDIEOffset());
    else
      Attr.set(RefDie->getOffset() + RefUnit->getStartOffset());
  }
}

void CompileUnit::addLabelLowPc(uint64_t LabelLowPc, int64_t PcOffset) {
  Labels.insert({LabelLowPc, PcOffset});
}

void CompileUnit::addFunctionRange(uint64_t FuncLowPc, uint64_t FuncHighPc,
                                   int64_t PcOffset) {
  //  Don't add empty ranges to the interval map.  They are a problem because
  //  the interval map expects half open intervals. This is safe because they
  //  are empty anyway.
  if (FuncHighPc != FuncLowPc)
    Ranges.insert(FuncLowPc, FuncHighPc, PcOffset);
  this->LowPc = std::min(LowPc, FuncLowPc + PcOffset);
  this->HighPc = std::max(HighPc, FuncHighPc + PcOffset);
}

void CompileUnit::noteRangeAttribute(const DIE &Die, PatchLocation Attr) {
  if (Die.getTag() != dwarf::DW_TAG_compile_unit)
    RangeAttributes.push_back(Attr);
  else
    UnitRangeAttribute = Attr;
}

void CompileUnit::noteLocationAttribute(PatchLocation Attr, int64_t PcOffset) {
  LocationAttributes.emplace_back(Attr, PcOffset);
}

void CompileUnit::addNamespaceAccelerator(const DIE *Die,
                                          DwarfStringPoolEntryRef Name) {
  Namespaces.emplace_back(Name, Die);
}

void CompileUnit::addObjCAccelerator(const DIE *Die,
                                     DwarfStringPoolEntryRef Name,
                                     bool SkipPubSection) {
  ObjC.emplace_back(Name, Die, SkipPubSection);
}

void CompileUnit::addNameAccelerator(const DIE *Die,
                                     DwarfStringPoolEntryRef Name,
                                     bool SkipPubSection) {
  Pubnames.emplace_back(Name, Die, SkipPubSection);
}

void CompileUnit::addTypeAccelerator(const DIE *Die,
                                     DwarfStringPoolEntryRef Name,
                                     bool ObjcClassImplementation,
                                     uint32_t QualifiedNameHash) {
  Pubtypes.emplace_back(Name, Die, QualifiedNameHash, ObjcClassImplementation);
}

} // namespace dsymutil
} // namespace llvm
