//===- DWARFContext.cpp ---------------------------------------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "llvm/DebugInfo/DWARF/DWARFContext.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/StringSwitch.h"
#include "llvm/BinaryFormat/Dwarf.h"
#include "llvm/DebugInfo/DWARF/DWARFAcceleratorTable.h"
#include "llvm/DebugInfo/DWARF/DWARFCompileUnit.h"
#include "llvm/DebugInfo/DWARF/DWARFDebugAbbrev.h"
#include "llvm/DebugInfo/DWARF/DWARFDebugArangeSet.h"
#include "llvm/DebugInfo/DWARF/DWARFDebugAranges.h"
#include "llvm/DebugInfo/DWARF/DWARFDebugFrame.h"
#include "llvm/DebugInfo/DWARF/DWARFDebugLine.h"
#include "llvm/DebugInfo/DWARF/DWARFDebugLoc.h"
#include "llvm/DebugInfo/DWARF/DWARFDebugMacro.h"
#include "llvm/DebugInfo/DWARF/DWARFDebugPubTable.h"
#include "llvm/DebugInfo/DWARF/DWARFDebugRangeList.h"
#include "llvm/DebugInfo/DWARF/DWARFDie.h"
#include "llvm/DebugInfo/DWARF/DWARFFormValue.h"
#include "llvm/DebugInfo/DWARF/DWARFGdbIndex.h"
#include "llvm/DebugInfo/DWARF/DWARFSection.h"
#include "llvm/DebugInfo/DWARF/DWARFUnitIndex.h"
#include "llvm/DebugInfo/DWARF/DWARFVerifier.h"
#include "llvm/Object/Decompressor.h"
#include "llvm/Object/MachO.h"
#include "llvm/Object/ObjectFile.h"
#include "llvm/Object/RelocVisitor.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/DataExtractor.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/Format.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/raw_ostream.h"
#include <algorithm>
#include <cstdint>
#include <map>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

using namespace llvm;
using namespace dwarf;
using namespace object;

#define DEBUG_TYPE "dwarf"

using DWARFLineTable = DWARFDebugLine::LineTable;
using FileLineInfoKind = DILineInfoSpecifier::FileLineInfoKind;
using FunctionNameKind = DILineInfoSpecifier::FunctionNameKind;

static void dumpAccelSection(raw_ostream &OS, StringRef Name,
                             const DWARFSection& Section, StringRef StringSection,
                             bool LittleEndian) {
  DWARFDataExtractor AccelSection(Section, LittleEndian, 0);
  DataExtractor StrData(StringSection, LittleEndian, 0);
  OS << "\n." << Name << " contents:\n";
  DWARFAcceleratorTable Accel(AccelSection, StrData);
  if (!Accel.extract())
    return;
  Accel.dump(OS);
}

static void
dumpDWARFv5StringOffsetsSection(raw_ostream &OS, StringRef SectionName,
                                const DWARFSection &StringOffsetsSection,
                                StringRef StringSection, bool LittleEndian) {
  DWARFDataExtractor StrOffsetExt(StringOffsetsSection, LittleEndian, 0);
  uint32_t Offset = 0;
  uint64_t SectionSize = StringOffsetsSection.Data.size();

  while (Offset < SectionSize) {
    unsigned Version = 0;
    DwarfFormat Format = DWARF32;
    unsigned EntrySize = 4;
    // Perform validation and extract the segment size from the header.
    if (!StrOffsetExt.isValidOffsetForDataOfSize(Offset, 4)) {
      OS << "error: invalid contribution to string offsets table in section ."
         << SectionName << ".\n";
      return;
    }
    uint32_t ContributionStart = Offset;
    uint64_t ContributionSize = StrOffsetExt.getU32(&Offset);
    // A contribution size of 0xffffffff indicates DWARF64, with the actual size
    // in the following 8 bytes. Otherwise, the DWARF standard mandates that
    // the contribution size must be at most 0xfffffff0.
    if (ContributionSize == 0xffffffff) {
      if (!StrOffsetExt.isValidOffsetForDataOfSize(Offset, 8)) {
        OS << "error: invalid contribution to string offsets table in section ."
           << SectionName << ".\n";
        return;
      }
      Format = DWARF64;
      EntrySize = 8;
      ContributionSize = StrOffsetExt.getU64(&Offset);
    } else if (ContributionSize > 0xfffffff0) {
      OS << "error: invalid contribution to string offsets table in section ."
         << SectionName << ".\n";
      return;
    }

    // We must ensure that we don't read a partial record at the end, so we
    // validate for a multiple of EntrySize. Also, we're expecting a version
    // number and padding, which adds an additional 4 bytes.
    uint64_t ValidationSize =
        4 + ((ContributionSize + EntrySize - 1) & (-(uint64_t)EntrySize));
    if (!StrOffsetExt.isValidOffsetForDataOfSize(Offset, ValidationSize)) {
      OS << "error: contribution to string offsets table in section ."
         << SectionName << " has invalid length.\n";
      return;
    }

    Version = StrOffsetExt.getU16(&Offset);
    Offset += 2;
    OS << format("0x%8.8x: ", ContributionStart);
    OS << "Contribution size = " << ContributionSize
       << ", Version = " << Version << "\n";

    uint32_t ContributionBase = Offset;
    DataExtractor StrData(StringSection, LittleEndian, 0);
    while (Offset - ContributionBase < ContributionSize) {
      OS << format("0x%8.8x: ", Offset);
      // FIXME: We can only extract strings in DWARF32 format at the moment.
      uint64_t StringOffset =
          StrOffsetExt.getRelocatedValue(EntrySize, &Offset);
      if (Format == DWARF32) {
        uint32_t StringOffset32 = (uint32_t)StringOffset;
        OS << format("%8.8x ", StringOffset32);
        const char *S = StrData.getCStr(&StringOffset32);
        if (S)
          OS << format("\"%s\"", S);
      } else
        OS << format("%16.16" PRIx64 " ", StringOffset);
      OS << "\n";
    }
  }
}

// Dump a DWARF string offsets section. This may be a DWARF v5 formatted
// string offsets section, where each compile or type unit contributes a
// number of entries (string offsets), with each contribution preceded by
// a header containing size and version number. Alternatively, it may be a
// monolithic series of string offsets, as generated by the pre-DWARF v5
// implementation of split DWARF.
static void dumpStringOffsetsSection(raw_ostream &OS, StringRef SectionName,
                                     const DWARFSection &StringOffsetsSection,
                                     StringRef StringSection, bool LittleEndian,
                                     unsigned MaxVersion) {
  if (StringOffsetsSection.Data.empty())
    return;
  OS << "\n." << SectionName << " contents:\n";
  // If we have at least one (compile or type) unit with DWARF v5 or greater,
  // we assume that the section is formatted like a DWARF v5 string offsets
  // section.
  if (MaxVersion >= 5)
    dumpDWARFv5StringOffsetsSection(OS, SectionName, StringOffsetsSection,
                                    StringSection, LittleEndian);
  else {
    DataExtractor strOffsetExt(StringOffsetsSection.Data, LittleEndian, 0);
    uint32_t offset = 0;
    uint64_t size = StringOffsetsSection.Data.size();
    // Ensure that size is a multiple of the size of an entry.
    if (size & ((uint64_t)(sizeof(uint32_t) - 1))) {
      OS << "error: size of ." << SectionName << " is not a multiple of "
         << sizeof(uint32_t) << ".\n";
      size &= -(uint64_t)sizeof(uint32_t);
    }
    DataExtractor StrData(StringSection, LittleEndian, 0);
    while (offset < size) {
      OS << format("0x%8.8x: ", offset);
      uint32_t StringOffset = strOffsetExt.getU32(&offset);
      OS << format("%8.8x  ", StringOffset);
      const char *S = StrData.getCStr(&StringOffset);
      if (S)
        OS << format("\"%s\"", S);
      OS << "\n";
    }
  }
}

void DWARFContext::dump(raw_ostream &OS, DIDumpOptions DumpOpts) {
  DIDumpType DumpType = DumpOpts.DumpType;
  bool DumpEH = DumpOpts.DumpEH;
  bool SummarizeTypes = DumpOpts.SummarizeTypes;

  if (DumpType == DIDT_All || DumpType == DIDT_Abbrev) {
    OS << ".debug_abbrev contents:\n";
    getDebugAbbrev()->dump(OS);
  }

  if (DumpType == DIDT_All || DumpType == DIDT_AbbrevDwo)
    if (const DWARFDebugAbbrev *D = getDebugAbbrevDWO()) {
      OS << "\n.debug_abbrev.dwo contents:\n";
      D->dump(OS);
    }

  if (DumpType == DIDT_All || DumpType == DIDT_Info) {
    OS << "\n.debug_info contents:\n";
    for (const auto &CU : compile_units())
      CU->dump(OS, DumpOpts);
  }

  if ((DumpType == DIDT_All || DumpType == DIDT_InfoDwo) &&
      getNumDWOCompileUnits()) {
    OS << "\n.debug_info.dwo contents:\n";
    for (const auto &DWOCU : dwo_compile_units())
      DWOCU->dump(OS, DumpOpts);
  }

  if ((DumpType == DIDT_All || DumpType == DIDT_Types) && getNumTypeUnits()) {
    OS << "\n.debug_types contents:\n";
    for (const auto &TUS : type_unit_sections())
      for (const auto &TU : TUS)
        TU->dump(OS, SummarizeTypes);
  }

  if ((DumpType == DIDT_All || DumpType == DIDT_TypesDwo) &&
      getNumDWOTypeUnits()) {
    OS << "\n.debug_types.dwo contents:\n";
    for (const auto &DWOTUS : dwo_type_unit_sections())
      for (const auto &DWOTU : DWOTUS)
        DWOTU->dump(OS, SummarizeTypes);
  }

  if (DumpType == DIDT_All || DumpType == DIDT_Loc) {
    OS << "\n.debug_loc contents:\n";
    getDebugLoc()->dump(OS);
  }

  if (DumpType == DIDT_All || DumpType == DIDT_LocDwo) {
    OS << "\n.debug_loc.dwo contents:\n";
    getDebugLocDWO()->dump(OS);
  }

  if (DumpType == DIDT_All || DumpType == DIDT_Frames) {
    OS << "\n.debug_frame contents:\n";
    getDebugFrame()->dump(OS);
    if (DumpEH) {
      OS << "\n.eh_frame contents:\n";
      getEHFrame()->dump(OS);
    }
  }

  if (DumpType == DIDT_All || DumpType == DIDT_Macro) {
    OS << "\n.debug_macinfo contents:\n";
    getDebugMacro()->dump(OS);
  }

  uint32_t offset = 0;
  if (DumpType == DIDT_All || DumpType == DIDT_Aranges) {
    OS << "\n.debug_aranges contents:\n";
    DataExtractor arangesData(getARangeSection(), isLittleEndian(), 0);
    DWARFDebugArangeSet set;
    while (set.extract(arangesData, &offset))
      set.dump(OS);
  }

  uint8_t savedAddressByteSize = 0;
  if (DumpType == DIDT_All || DumpType == DIDT_Line) {
    OS << "\n.debug_line contents:\n";
    for (const auto &CU : compile_units()) {
      savedAddressByteSize = CU->getAddressByteSize();
      auto CUDIE = CU->getUnitDIE();
      if (!CUDIE)
        continue;
      if (auto StmtOffset = toSectionOffset(CUDIE.find(DW_AT_stmt_list))) {
        DWARFDataExtractor lineData(getLineSection(), isLittleEndian(),
                                    savedAddressByteSize);
        DWARFDebugLine::LineTable LineTable;
        uint32_t Offset = *StmtOffset;
        LineTable.parse(lineData, &Offset);
        LineTable.dump(OS);
      }
    }
  }

  if (DumpType == DIDT_All || DumpType == DIDT_CUIndex) {
    OS << "\n.debug_cu_index contents:\n";
    getCUIndex().dump(OS);
  }

  if (DumpType == DIDT_All || DumpType == DIDT_TUIndex) {
    OS << "\n.debug_tu_index contents:\n";
    getTUIndex().dump(OS);
  }

  if (DumpType == DIDT_All || DumpType == DIDT_LineDwo) {
    OS << "\n.debug_line.dwo contents:\n";
    unsigned stmtOffset = 0;
    DWARFDataExtractor lineData(getLineDWOSection(), isLittleEndian(),
                                savedAddressByteSize);
    DWARFDebugLine::LineTable LineTable;
    while (LineTable.Prologue.parse(lineData, &stmtOffset)) {
      LineTable.dump(OS);
      LineTable.clear();
    }
  }

  if (DumpType == DIDT_All || DumpType == DIDT_Str) {
    OS << "\n.debug_str contents:\n";
    DataExtractor strData(getStringSection(), isLittleEndian(), 0);
    offset = 0;
    uint32_t strOffset = 0;
    while (const char *s = strData.getCStr(&offset)) {
      OS << format("0x%8.8x: \"%s\"\n", strOffset, s);
      strOffset = offset;
    }
  }

  if ((DumpType == DIDT_All || DumpType == DIDT_StrDwo) &&
      !getStringDWOSection().empty()) {
    OS << "\n.debug_str.dwo contents:\n";
    DataExtractor strDWOData(getStringDWOSection(), isLittleEndian(), 0);
    offset = 0;
    uint32_t strDWOOffset = 0;
    while (const char *s = strDWOData.getCStr(&offset)) {
      OS << format("0x%8.8x: \"%s\"\n", strDWOOffset, s);
      strDWOOffset = offset;
    }
  }

  if (DumpType == DIDT_All || DumpType == DIDT_Ranges) {
    OS << "\n.debug_ranges contents:\n";
    // In fact, different compile units may have different address byte
    // sizes, but for simplicity we just use the address byte size of the last
    // compile unit (there is no easy and fast way to associate address range
    // list and the compile unit it describes).
    DWARFDataExtractor rangesData(getRangeSection(), isLittleEndian(),
                                  savedAddressByteSize);
    offset = 0;
    DWARFDebugRangeList rangeList;
    while (rangeList.extract(rangesData, &offset))
      rangeList.dump(OS);
  }

  if (DumpType == DIDT_All || DumpType == DIDT_Pubnames)
    DWARFDebugPubTable(getPubNamesSection(), isLittleEndian(), false)
        .dump("debug_pubnames", OS);

  if (DumpType == DIDT_All || DumpType == DIDT_Pubtypes)
    DWARFDebugPubTable(getPubTypesSection(), isLittleEndian(), false)
        .dump("debug_pubtypes", OS);

  if (DumpType == DIDT_All || DumpType == DIDT_GnuPubnames)
    DWARFDebugPubTable(getGnuPubNamesSection(), isLittleEndian(),
                       true /* GnuStyle */)
        .dump("debug_gnu_pubnames", OS);

  if (DumpType == DIDT_All || DumpType == DIDT_GnuPubtypes)
    DWARFDebugPubTable(getGnuPubTypesSection(), isLittleEndian(),
                       true /* GnuStyle */)
        .dump("debug_gnu_pubtypes", OS);

  if (DumpType == DIDT_All || DumpType == DIDT_StrOffsets)
    dumpStringOffsetsSection(OS, "debug_str_offsets", getStringOffsetSection(),
                             getStringSection(), isLittleEndian(),
                             getMaxVersion());

  if (DumpType == DIDT_All || DumpType == DIDT_StrOffsetsDwo) {
    dumpStringOffsetsSection(OS, "debug_str_offsets.dwo",
                             getStringOffsetDWOSection(), getStringDWOSection(),
                             isLittleEndian(), getMaxVersion());
  }

  if ((DumpType == DIDT_All || DumpType == DIDT_GdbIndex) &&
      !getGdbIndexSection().empty()) {
    OS << "\n.gnu_index contents:\n";
    getGdbIndex().dump(OS);
  }

  if (DumpType == DIDT_All || DumpType == DIDT_AppleNames)
    dumpAccelSection(OS, "apple_names", getAppleNamesSection(),
                     getStringSection(), isLittleEndian());

  if (DumpType == DIDT_All || DumpType == DIDT_AppleTypes)
    dumpAccelSection(OS, "apple_types", getAppleTypesSection(),
                     getStringSection(), isLittleEndian());

  if (DumpType == DIDT_All || DumpType == DIDT_AppleNamespaces)
    dumpAccelSection(OS, "apple_namespaces", getAppleNamespacesSection(),
                     getStringSection(), isLittleEndian());

  if (DumpType == DIDT_All || DumpType == DIDT_AppleObjC)
    dumpAccelSection(OS, "apple_objc", getAppleObjCSection(),
                     getStringSection(), isLittleEndian());
}

DWARFCompileUnit *DWARFContext::getDWOCompileUnitForHash(uint64_t Hash) {
  // FIXME: Improve this for the case where this DWO file is really a DWP file
  // with an index - use the index for lookup instead of a linear search.
  for (const auto &DWOCU : dwo_compile_units())
    if (DWOCU->getDWOId() == Hash)
      return DWOCU.get();
  return nullptr;
}

DWARFDie DWARFContext::getDIEForOffset(uint32_t Offset) {
  parseCompileUnits();
  if (auto *CU = CUs.getUnitForOffset(Offset))
    return CU->getDIEForOffset(Offset);
  return DWARFDie();
}

bool DWARFContext::verify(raw_ostream &OS, DIDumpType DumpType) {
  bool Success = true;
  DWARFVerifier verifier(OS, *this);
  if (DumpType == DIDT_All || DumpType == DIDT_Info) {
    if (!verifier.handleDebugInfo())
      Success = false;
  }
  if (DumpType == DIDT_All || DumpType == DIDT_Line) {
    if (!verifier.handleDebugLine())
      Success = false;
  }
  if (DumpType == DIDT_All || DumpType == DIDT_AppleNames) {
    if (!verifier.handleAppleNames())
      Success = false;
  }
  return Success;
}

const DWARFUnitIndex &DWARFContext::getCUIndex() {
  if (CUIndex)
    return *CUIndex;

  DataExtractor CUIndexData(getCUIndexSection(), isLittleEndian(), 0);

  CUIndex = llvm::make_unique<DWARFUnitIndex>(DW_SECT_INFO);
  CUIndex->parse(CUIndexData);
  return *CUIndex;
}

const DWARFUnitIndex &DWARFContext::getTUIndex() {
  if (TUIndex)
    return *TUIndex;

  DataExtractor TUIndexData(getTUIndexSection(), isLittleEndian(), 0);

  TUIndex = llvm::make_unique<DWARFUnitIndex>(DW_SECT_TYPES);
  TUIndex->parse(TUIndexData);
  return *TUIndex;
}

DWARFGdbIndex &DWARFContext::getGdbIndex() {
  if (GdbIndex)
    return *GdbIndex;

  DataExtractor GdbIndexData(getGdbIndexSection(), true /*LE*/, 0);
  GdbIndex = llvm::make_unique<DWARFGdbIndex>();
  GdbIndex->parse(GdbIndexData);
  return *GdbIndex;
}

const DWARFDebugAbbrev *DWARFContext::getDebugAbbrev() {
  if (Abbrev)
    return Abbrev.get();

  DataExtractor abbrData(getAbbrevSection(), isLittleEndian(), 0);

  Abbrev.reset(new DWARFDebugAbbrev());
  Abbrev->extract(abbrData);
  return Abbrev.get();
}

const DWARFDebugAbbrev *DWARFContext::getDebugAbbrevDWO() {
  if (AbbrevDWO)
    return AbbrevDWO.get();

  DataExtractor abbrData(getAbbrevDWOSection(), isLittleEndian(), 0);
  AbbrevDWO.reset(new DWARFDebugAbbrev());
  AbbrevDWO->extract(abbrData);
  return AbbrevDWO.get();
}

const DWARFDebugLoc *DWARFContext::getDebugLoc() {
  if (Loc)
    return Loc.get();

  Loc.reset(new DWARFDebugLoc);
  // assume all compile units have the same address byte size
  if (getNumCompileUnits()) {
    DWARFDataExtractor LocData(getLocSection(), isLittleEndian(),
                               getCompileUnitAtIndex(0)->getAddressByteSize());
    Loc->parse(LocData);
  }
  return Loc.get();
}

const DWARFDebugLocDWO *DWARFContext::getDebugLocDWO() {
  if (LocDWO)
    return LocDWO.get();

  DataExtractor LocData(getLocDWOSection().Data, isLittleEndian(), 0);
  LocDWO.reset(new DWARFDebugLocDWO());
  LocDWO->parse(LocData);
  return LocDWO.get();
}

const DWARFDebugAranges *DWARFContext::getDebugAranges() {
  if (Aranges)
    return Aranges.get();

  Aranges.reset(new DWARFDebugAranges());
  Aranges->generate(this);
  return Aranges.get();
}

const DWARFDebugFrame *DWARFContext::getDebugFrame() {
  if (DebugFrame)
    return DebugFrame.get();

  // There's a "bug" in the DWARFv3 standard with respect to the target address
  // size within debug frame sections. While DWARF is supposed to be independent
  // of its container, FDEs have fields with size being "target address size",
  // which isn't specified in DWARF in general. It's only specified for CUs, but
  // .eh_frame can appear without a .debug_info section. Follow the example of
  // other tools (libdwarf) and extract this from the container (ObjectFile
  // provides this information). This problem is fixed in DWARFv4
  // See this dwarf-discuss discussion for more details:
  // http://lists.dwarfstd.org/htdig.cgi/dwarf-discuss-dwarfstd.org/2011-December/001173.html
  DataExtractor debugFrameData(getDebugFrameSection(), isLittleEndian(),
                               getAddressSize());
  DebugFrame.reset(new DWARFDebugFrame(false /* IsEH */));
  DebugFrame->parse(debugFrameData);
  return DebugFrame.get();
}

const DWARFDebugFrame *DWARFContext::getEHFrame() {
  if (EHFrame)
    return EHFrame.get();

  DataExtractor debugFrameData(getEHFrameSection(), isLittleEndian(),
                               getAddressSize());
  DebugFrame.reset(new DWARFDebugFrame(true /* IsEH */));
  DebugFrame->parse(debugFrameData);
  return DebugFrame.get();
}

const DWARFDebugMacro *DWARFContext::getDebugMacro() {
  if (Macro)
    return Macro.get();

  DataExtractor MacinfoData(getMacinfoSection(), isLittleEndian(), 0);
  Macro.reset(new DWARFDebugMacro());
  Macro->parse(MacinfoData);
  return Macro.get();
}

const DWARFLineTable *
DWARFContext::getLineTableForUnit(DWARFUnit *U) {
  if (!Line)
    Line.reset(new DWARFDebugLine);

  auto UnitDIE = U->getUnitDIE();
  if (!UnitDIE)
    return nullptr;

  auto Offset = toSectionOffset(UnitDIE.find(DW_AT_stmt_list));
  if (!Offset)
    return nullptr; // No line table for this compile unit.

  uint32_t stmtOffset = *Offset + U->getLineTableOffset();
  // See if the line table is cached.
  if (const DWARFLineTable *lt = Line->getLineTable(stmtOffset))
    return lt;

  // Make sure the offset is good before we try to parse.
  if (stmtOffset >= U->getLineSection().Data.size())
    return nullptr;  

  // We have to parse it first.
  DWARFDataExtractor lineData(U->getLineSection(), isLittleEndian(),
                              U->getAddressByteSize());
  return Line->getOrParseLineTable(lineData, stmtOffset);
}

void DWARFContext::parseCompileUnits() {
  CUs.parse(*this, getInfoSection());
}

void DWARFContext::parseTypeUnits() {
  if (!TUs.empty())
    return;
  forEachTypesSections([&](const DWARFSection &S) {
    TUs.emplace_back();
    TUs.back().parse(*this, S);
  });
}

void DWARFContext::parseDWOCompileUnits() {
  DWOCUs.parseDWO(*this, getInfoDWOSection());
}

void DWARFContext::parseDWOTypeUnits() {
  if (!DWOTUs.empty())
    return;
  forEachTypesDWOSections([&](const DWARFSection &S) {
    DWOTUs.emplace_back();
    DWOTUs.back().parseDWO(*this, S);
  });
}

DWARFCompileUnit *DWARFContext::getCompileUnitForOffset(uint32_t Offset) {
  parseCompileUnits();
  return CUs.getUnitForOffset(Offset);
}

DWARFCompileUnit *DWARFContext::getCompileUnitForAddress(uint64_t Address) {
  // First, get the offset of the compile unit.
  uint32_t CUOffset = getDebugAranges()->findAddress(Address);
  // Retrieve the compile unit.
  return getCompileUnitForOffset(CUOffset);
}

static bool getFunctionNameAndStartLineForAddress(DWARFCompileUnit *CU,
                                                  uint64_t Address,
                                                  FunctionNameKind Kind,
                                                  std::string &FunctionName,
                                                  uint32_t &StartLine) {
  // The address may correspond to instruction in some inlined function,
  // so we have to build the chain of inlined functions and take the
  // name of the topmost function in it.
  SmallVector<DWARFDie, 4> InlinedChain;
  CU->getInlinedChainForAddress(Address, InlinedChain);
  if (InlinedChain.empty())
    return false;

  const DWARFDie &DIE = InlinedChain[0];
  bool FoundResult = false;
  const char *Name = nullptr;
  if (Kind != FunctionNameKind::None && (Name = DIE.getSubroutineName(Kind))) {
    FunctionName = Name;
    FoundResult = true;
  }
  if (auto DeclLineResult = DIE.getDeclLine()) {
    StartLine = DeclLineResult;
    FoundResult = true;
  }

  return FoundResult;
}

DILineInfo DWARFContext::getLineInfoForAddress(uint64_t Address,
                                               DILineInfoSpecifier Spec) {
  DILineInfo Result;

  DWARFCompileUnit *CU = getCompileUnitForAddress(Address);
  if (!CU)
    return Result;
  getFunctionNameAndStartLineForAddress(CU, Address, Spec.FNKind,
                                        Result.FunctionName,
                                        Result.StartLine);
  if (Spec.FLIKind != FileLineInfoKind::None) {
    if (const DWARFLineTable *LineTable = getLineTableForUnit(CU))
      LineTable->getFileLineInfoForAddress(Address, CU->getCompilationDir(),
                                           Spec.FLIKind, Result);
  }
  return Result;
}

DILineInfoTable
DWARFContext::getLineInfoForAddressRange(uint64_t Address, uint64_t Size,
                                         DILineInfoSpecifier Spec) {
  DILineInfoTable  Lines;
  DWARFCompileUnit *CU = getCompileUnitForAddress(Address);
  if (!CU)
    return Lines;

  std::string FunctionName = "<invalid>";
  uint32_t StartLine = 0;
  getFunctionNameAndStartLineForAddress(CU, Address, Spec.FNKind, FunctionName,
                                        StartLine);

  // If the Specifier says we don't need FileLineInfo, just
  // return the top-most function at the starting address.
  if (Spec.FLIKind == FileLineInfoKind::None) {
    DILineInfo Result;
    Result.FunctionName = FunctionName;
    Result.StartLine = StartLine;
    Lines.push_back(std::make_pair(Address, Result));
    return Lines;
  }

  const DWARFLineTable *LineTable = getLineTableForUnit(CU);

  // Get the index of row we're looking for in the line table.
  std::vector<uint32_t> RowVector;
  if (!LineTable->lookupAddressRange(Address, Size, RowVector))
    return Lines;

  for (uint32_t RowIndex : RowVector) {
    // Take file number and line/column from the row.
    const DWARFDebugLine::Row &Row = LineTable->Rows[RowIndex];
    DILineInfo Result;
    LineTable->getFileNameByIndex(Row.File, CU->getCompilationDir(),
                                  Spec.FLIKind, Result.FileName);
    Result.FunctionName = FunctionName;
    Result.Line = Row.Line;
    Result.Column = Row.Column;
    Result.StartLine = StartLine;
    Lines.push_back(std::make_pair(Row.Address, Result));
  }

  return Lines;
}

DIInliningInfo
DWARFContext::getInliningInfoForAddress(uint64_t Address,
                                        DILineInfoSpecifier Spec) {
  DIInliningInfo InliningInfo;

  DWARFCompileUnit *CU = getCompileUnitForAddress(Address);
  if (!CU)
    return InliningInfo;

  const DWARFLineTable *LineTable = nullptr;
  SmallVector<DWARFDie, 4> InlinedChain;
  CU->getInlinedChainForAddress(Address, InlinedChain);
  if (InlinedChain.size() == 0) {
    // If there is no DIE for address (e.g. it is in unavailable .dwo file),
    // try to at least get file/line info from symbol table.
    if (Spec.FLIKind != FileLineInfoKind::None) {
      DILineInfo Frame;
      LineTable = getLineTableForUnit(CU);
      if (LineTable &&
          LineTable->getFileLineInfoForAddress(Address, CU->getCompilationDir(),
                                               Spec.FLIKind, Frame))
        InliningInfo.addFrame(Frame);
    }
    return InliningInfo;
  }

  uint32_t CallFile = 0, CallLine = 0, CallColumn = 0, CallDiscriminator = 0;
  for (uint32_t i = 0, n = InlinedChain.size(); i != n; i++) {
    DWARFDie &FunctionDIE = InlinedChain[i];
    DILineInfo Frame;
    // Get function name if necessary.
    if (const char *Name = FunctionDIE.getSubroutineName(Spec.FNKind))
      Frame.FunctionName = Name;
    if (auto DeclLineResult = FunctionDIE.getDeclLine())
      Frame.StartLine = DeclLineResult;
    if (Spec.FLIKind != FileLineInfoKind::None) {
      if (i == 0) {
        // For the topmost frame, initialize the line table of this
        // compile unit and fetch file/line info from it.
        LineTable = getLineTableForUnit(CU);
        // For the topmost routine, get file/line info from line table.
        if (LineTable)
          LineTable->getFileLineInfoForAddress(Address, CU->getCompilationDir(),
                                               Spec.FLIKind, Frame);
      } else {
        // Otherwise, use call file, call line and call column from
        // previous DIE in inlined chain.
        if (LineTable)
          LineTable->getFileNameByIndex(CallFile, CU->getCompilationDir(),
                                        Spec.FLIKind, Frame.FileName);
        Frame.Line = CallLine;
        Frame.Column = CallColumn;
        Frame.Discriminator = CallDiscriminator;
      }
      // Get call file/line/column of a current DIE.
      if (i + 1 < n) {
        FunctionDIE.getCallerFrame(CallFile, CallLine, CallColumn,
                                   CallDiscriminator);
      }
    }
    InliningInfo.addFrame(Frame);
  }
  return InliningInfo;
}

std::shared_ptr<DWARFContext>
DWARFContext::getDWOContext(StringRef AbsolutePath) {
  if (auto S = DWP.lock()) {
    DWARFContext *Ctxt = S->Context.get();
    return std::shared_ptr<DWARFContext>(std::move(S), Ctxt);
  }

  std::weak_ptr<DWOFile> *Entry = &DWOFiles[AbsolutePath];

  if (auto S = Entry->lock()) {
    DWARFContext *Ctxt = S->Context.get();
    return std::shared_ptr<DWARFContext>(std::move(S), Ctxt);
  }

  SmallString<128> DWPName;
  Expected<OwningBinary<ObjectFile>> Obj = [&] {
    if (!CheckedForDWP) {
      (getFileName() + ".dwp").toVector(DWPName);
      auto Obj = object::ObjectFile::createObjectFile(DWPName);
      if (Obj) {
        Entry = &DWP;
        return Obj;
      } else {
        CheckedForDWP = true;
        // TODO: Should this error be handled (maybe in a high verbosity mode)
        // before falling back to .dwo files?
        consumeError(Obj.takeError());
      }
    }

    return object::ObjectFile::createObjectFile(AbsolutePath);
  }();

  if (!Obj) {
    // TODO: Actually report errors helpfully.
    consumeError(Obj.takeError());
    return nullptr;
  }

  auto S = std::make_shared<DWOFile>();
  S->File = std::move(Obj.get());
  S->Context = llvm::make_unique<DWARFContextInMemory>(*S->File.getBinary());
  *Entry = S;
  auto *Ctxt = S->Context.get();
  return std::shared_ptr<DWARFContext>(std::move(S), Ctxt);
}

static Error createError(const Twine &Reason, llvm::Error E) {
  return make_error<StringError>(Reason + toString(std::move(E)),
                                 inconvertibleErrorCode());
}

/// SymInfo contains information about symbol: it's address
/// and section index which is -1LL for absolute symbols.
struct SymInfo {
  uint64_t Address;
  uint64_t SectionIndex;
};

/// Returns the address of symbol relocation used against and a section index.
/// Used for futher relocations computation. Symbol's section load address is
static Expected<SymInfo> getSymbolInfo(const object::ObjectFile &Obj,
                                       const RelocationRef &Reloc,
                                       const LoadedObjectInfo *L,
                                       std::map<SymbolRef, SymInfo> &Cache) {
  SymInfo Ret = {0, (uint64_t)-1LL};
  object::section_iterator RSec = Obj.section_end();
  object::symbol_iterator Sym = Reloc.getSymbol();

  std::map<SymbolRef, SymInfo>::iterator CacheIt = Cache.end();
  // First calculate the address of the symbol or section as it appears
  // in the object file
  if (Sym != Obj.symbol_end()) {
    bool New;
    std::tie(CacheIt, New) = Cache.insert({*Sym, {0, 0}});
    if (!New)
      return CacheIt->second;

    Expected<uint64_t> SymAddrOrErr = Sym->getAddress();
    if (!SymAddrOrErr)
      return createError("failed to compute symbol address: ",
                         SymAddrOrErr.takeError());

    // Also remember what section this symbol is in for later
    auto SectOrErr = Sym->getSection();
    if (!SectOrErr)
      return createError("failed to get symbol section: ",
                         SectOrErr.takeError());

    RSec = *SectOrErr;
    Ret.Address = *SymAddrOrErr;
  } else if (auto *MObj = dyn_cast<MachOObjectFile>(&Obj)) {
    RSec = MObj->getRelocationSection(Reloc.getRawDataRefImpl());
    Ret.Address = RSec->getAddress();
  }

  if (RSec != Obj.section_end())
    Ret.SectionIndex = RSec->getIndex();

  // If we are given load addresses for the sections, we need to adjust:
  // SymAddr = (Address of Symbol Or Section in File) -
  //           (Address of Section in File) +
  //           (Load Address of Section)
  // RSec is now either the section being targeted or the section
  // containing the symbol being targeted. In either case,
  // we need to perform the same computation.
  if (L && RSec != Obj.section_end())
    if (uint64_t SectionLoadAddress = L->getSectionLoadAddress(*RSec))
      Ret.Address += SectionLoadAddress - RSec->getAddress();

  if (CacheIt != Cache.end())
    CacheIt->second = Ret;

  return Ret;
}

static bool isRelocScattered(const object::ObjectFile &Obj,
                             const RelocationRef &Reloc) {
  const MachOObjectFile *MachObj = dyn_cast<MachOObjectFile>(&Obj);
  if (!MachObj)
    return false;
  // MachO also has relocations that point to sections and
  // scattered relocations.
  auto RelocInfo = MachObj->getRelocation(Reloc.getRawDataRefImpl());
  return MachObj->isRelocationScattered(RelocInfo);
}

Error DWARFContextInMemory::maybeDecompress(const SectionRef &Sec,
                                            StringRef Name, StringRef &Data) {
  if (!Decompressor::isCompressed(Sec))
    return Error::success();

  Expected<Decompressor> Decompressor =
      Decompressor::create(Name, Data, IsLittleEndian, AddressSize == 8);
  if (!Decompressor)
    return Decompressor.takeError();

  SmallString<32> Out;
  if (auto Err = Decompressor->resizeAndDecompress(Out))
    return Err;

  UncompressedSections.emplace_back(std::move(Out));
  Data = UncompressedSections.back();

  return Error::success();
}

ErrorPolicy DWARFContextInMemory::defaultErrorHandler(Error E) {
  errs() << "error: " + toString(std::move(E)) << '\n';
  return ErrorPolicy::Continue;
}

DWARFContextInMemory::DWARFContextInMemory(
    const object::ObjectFile &Obj, const LoadedObjectInfo *L,
    function_ref<ErrorPolicy(Error)> HandleError)
    : FileName(Obj.getFileName()), IsLittleEndian(Obj.isLittleEndian()),
      AddressSize(Obj.getBytesInAddress()) {
  for (const SectionRef &Section : Obj.sections()) {
    StringRef Name;
    Section.getName(Name);
    // Skip BSS and Virtual sections, they aren't interesting.
    if (Section.isBSS() || Section.isVirtual())
      continue;

    StringRef Data;
    section_iterator RelocatedSection = Section.getRelocatedSection();
    // Try to obtain an already relocated version of this section.
    // Else use the unrelocated section from the object file. We'll have to
    // apply relocations ourselves later.
    if (!L || !L->getLoadedSectionContents(*RelocatedSection, Data))
      Section.getContents(Data);

    if (auto Err = maybeDecompress(Section, Name, Data)) {
      ErrorPolicy EP = HandleError(
          createError("failed to decompress '" + Name + "', ", std::move(Err)));
      if (EP == ErrorPolicy::Halt)
        return;
      continue;
    }

    // Compressed sections names in GNU style starts from ".z",
    // at this point section is decompressed and we drop compression prefix.
    Name = Name.substr(
        Name.find_first_not_of("._z")); // Skip ".", "z" and "_" prefixes.

    // Map platform specific debug section names to DWARF standard section
    // names.
    Name = Obj.mapDebugSectionName(Name);

    if (StringRef *SectionData = mapSectionToMember(Name)) {
      *SectionData = Data;
      if (Name == "debug_ranges") {
        // FIXME: Use the other dwo range section when we emit it.
        RangeDWOSection.Data = Data;
      }
    } else if (Name == "debug_types") {
      // Find debug_types data by section rather than name as there are
      // multiple, comdat grouped, debug_types sections.
      TypesSections[Section].Data = Data;
    } else if (Name == "debug_types.dwo") {
      TypesDWOSections[Section].Data = Data;
    }

    if (RelocatedSection == Obj.section_end())
      continue;

    StringRef RelSecName;
    StringRef RelSecData;
    RelocatedSection->getName(RelSecName);

    // If the section we're relocating was relocated already by the JIT,
    // then we used the relocated version above, so we do not need to process
    // relocations for it now.
    if (L && L->getLoadedSectionContents(*RelocatedSection, RelSecData))
      continue;

    // In Mach-o files, the relocations do not need to be applied if
    // there is no load offset to apply. The value read at the
    // relocation point already factors in the section address
    // (actually applying the relocations will produce wrong results
    // as the section address will be added twice).
    if (!L && isa<MachOObjectFile>(&Obj))
      continue;

    RelSecName = RelSecName.substr(
        RelSecName.find_first_not_of("._z")); // Skip . and _ prefixes.

    // TODO: Add support for relocations in other sections as needed.
    // Record relocations for the debug_info and debug_line sections.
    DWARFSection *Sec = mapNameToDWARFSection(RelSecName);
    RelocAddrMap *Map = Sec ? &Sec->Relocs : nullptr;
    if (!Map) {
      // Find debug_types relocs by section rather than name as there are
      // multiple, comdat grouped, debug_types sections.
      if (RelSecName == "debug_types")
        Map = &TypesSections[*RelocatedSection].Relocs;
      else if (RelSecName == "debug_types.dwo")
        Map = &TypesDWOSections[*RelocatedSection].Relocs;
      else
        continue;
    }

    if (Section.relocation_begin() == Section.relocation_end())
      continue;

    // Symbol to [address, section index] cache mapping.
    std::map<SymbolRef, SymInfo> AddrCache;
    for (const RelocationRef &Reloc : Section.relocations()) {
      // FIXME: it's not clear how to correctly handle scattered
      // relocations.
      if (isRelocScattered(Obj, Reloc))
        continue;

      Expected<SymInfo> SymInfoOrErr = getSymbolInfo(Obj, Reloc, L, AddrCache);
      if (!SymInfoOrErr) {
        if (HandleError(SymInfoOrErr.takeError()) == ErrorPolicy::Halt)
          return;
        continue;
      }

      object::RelocVisitor V(Obj);
      uint64_t Val = V.visit(Reloc.getType(), Reloc, SymInfoOrErr->Address);
      if (V.error()) {
        SmallString<32> Type;
        Reloc.getTypeName(Type);
        ErrorPolicy EP = HandleError(
            createError("failed to compute relocation: " + Type + ", ",
                        errorCodeToError(object_error::parse_failed)));
        if (EP == ErrorPolicy::Halt)
          return;
        continue;
      }
      RelocAddrEntry Rel = {SymInfoOrErr->SectionIndex, Val};
      Map->insert({Reloc.getOffset(), Rel});
    }
  }
}

DWARFContextInMemory::DWARFContextInMemory(
    const StringMap<std::unique_ptr<MemoryBuffer>> &Sections, uint8_t AddrSize,
    bool isLittleEndian)
    : IsLittleEndian(isLittleEndian), AddressSize(AddrSize) {
  for (const auto &SecIt : Sections) {
    if (StringRef *SectionData = mapSectionToMember(SecIt.first()))
      *SectionData = SecIt.second->getBuffer();
  }
}

DWARFSection *DWARFContextInMemory::mapNameToDWARFSection(StringRef Name) {
  return StringSwitch<DWARFSection *>(Name)
      .Case("debug_info", &InfoSection)
      .Case("debug_loc", &LocSection)
      .Case("debug_line", &LineSection)
      .Case("debug_str_offsets", &StringOffsetSection)
      .Case("debug_ranges", &RangeSection)
      .Case("debug_info.dwo", &InfoDWOSection)
      .Case("debug_loc.dwo", &LocDWOSection)
      .Case("debug_line.dwo", &LineDWOSection)
      .Case("debug_str_offsets.dwo", &StringOffsetDWOSection)
      .Case("debug_addr", &AddrSection)
      .Case("apple_names", &AppleNamesSection)
      .Case("apple_types", &AppleTypesSection)
      .Case("apple_namespaces", &AppleNamespacesSection)
      .Case("apple_namespac", &AppleNamespacesSection)
      .Case("apple_objc", &AppleObjCSection)
      .Default(nullptr);
}

StringRef *DWARFContextInMemory::mapSectionToMember(StringRef Name) {
  if (DWARFSection *Sec = mapNameToDWARFSection(Name))
    return &Sec->Data;
  return StringSwitch<StringRef *>(Name)
      .Case("debug_abbrev", &AbbrevSection)
      .Case("debug_aranges", &ARangeSection)
      .Case("debug_frame", &DebugFrameSection)
      .Case("eh_frame", &EHFrameSection)
      .Case("debug_str", &StringSection)
      .Case("debug_macinfo", &MacinfoSection)
      .Case("debug_pubnames", &PubNamesSection)
      .Case("debug_pubtypes", &PubTypesSection)
      .Case("debug_gnu_pubnames", &GnuPubNamesSection)
      .Case("debug_gnu_pubtypes", &GnuPubTypesSection)
      .Case("debug_abbrev.dwo", &AbbrevDWOSection)
      .Case("debug_str.dwo", &StringDWOSection)
      .Case("debug_cu_index", &CUIndexSection)
      .Case("debug_tu_index", &TUIndexSection)
      .Case("gdb_index", &GdbIndexSection)
      // Any more debug info sections go here.
      .Default(nullptr);
}

void DWARFContextInMemory::anchor() {}
