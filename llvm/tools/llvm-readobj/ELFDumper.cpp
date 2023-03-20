//===- ELFDumper.cpp - ELF-specific dumper --------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
///
/// \file
/// This file implements the ELF-specific dumper for llvm-readobj.
///
//===----------------------------------------------------------------------===//

#include "ARMEHABIPrinter.h"
#include "DwarfCFIEHPrinter.h"
#include "Error.h"
#include "ObjDumper.h"
#include "StackMapPrinter.h"
#include "llvm-readobj.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/Optional.h"
#include "llvm/ADT/PointerIntPair.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/Twine.h"
#include "llvm/BinaryFormat/AMDGPUMetadataVerifier.h"
#include "llvm/BinaryFormat/ELF.h"
#include "llvm/Demangle/Demangle.h"
#include "llvm/Object/ELF.h"
#include "llvm/Object/ELFObjectFile.h"
#include "llvm/Object/ELFTypes.h"
#include "llvm/Object/Error.h"
#include "llvm/Object/ObjectFile.h"
#include "llvm/Object/StackMapParser.h"
#include "llvm/Support/AMDGPUMetadata.h"
#include "llvm/Support/ARMAttributeParser.h"
#include "llvm/Support/ARMBuildAttributes.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/Compiler.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/Format.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/FormattedStream.h"
#include "llvm/Support/LEB128.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/Support/MipsABIFlags.h"
#include "llvm/Support/ScopedPrinter.h"
#include "llvm/Support/raw_ostream.h"
#include <algorithm>
#include <cinttypes>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iterator>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

using namespace llvm;
using namespace llvm::object;
using namespace ELF;

#define LLVM_READOBJ_ENUM_CASE(ns, enum)                                       \
  case ns::enum:                                                               \
    return #enum;

#define ENUM_ENT(enum, altName)                                                \
  { #enum, altName, ELF::enum }

#define ENUM_ENT_1(enum)                                                       \
  { #enum, #enum, ELF::enum }

#define LLVM_READOBJ_PHDR_ENUM(ns, enum)                                       \
  case ns::enum:                                                               \
    return std::string(#enum).substr(3);

#define TYPEDEF_ELF_TYPES(ELFT)                                                \
  using ELFO = ELFFile<ELFT>;                                                  \
  using Elf_Addr = typename ELFT::Addr;                                        \
  using Elf_Shdr = typename ELFT::Shdr;                                        \
  using Elf_Sym = typename ELFT::Sym;                                          \
  using Elf_Dyn = typename ELFT::Dyn;                                          \
  using Elf_Dyn_Range = typename ELFT::DynRange;                               \
  using Elf_Rel = typename ELFT::Rel;                                          \
  using Elf_Rela = typename ELFT::Rela;                                        \
  using Elf_Relr = typename ELFT::Relr;                                        \
  using Elf_Rel_Range = typename ELFT::RelRange;                               \
  using Elf_Rela_Range = typename ELFT::RelaRange;                             \
  using Elf_Relr_Range = typename ELFT::RelrRange;                             \
  using Elf_Phdr = typename ELFT::Phdr;                                        \
  using Elf_Half = typename ELFT::Half;                                        \
  using Elf_Ehdr = typename ELFT::Ehdr;                                        \
  using Elf_Word = typename ELFT::Word;                                        \
  using Elf_Hash = typename ELFT::Hash;                                        \
  using Elf_GnuHash = typename ELFT::GnuHash;                                  \
  using Elf_Note  = typename ELFT::Note;                                       \
  using Elf_Sym_Range = typename ELFT::SymRange;                               \
  using Elf_Versym = typename ELFT::Versym;                                    \
  using Elf_Verneed = typename ELFT::Verneed;                                  \
  using Elf_Vernaux = typename ELFT::Vernaux;                                  \
  using Elf_Verdef = typename ELFT::Verdef;                                    \
  using Elf_Verdaux = typename ELFT::Verdaux;                                  \
  using Elf_CGProfile = typename ELFT::CGProfile;                              \
  using uintX_t = typename ELFT::uint;

namespace {

template <class ELFT> class DumpStyle;

/// Represents a contiguous uniform range in the file. We cannot just create a
/// range directly because when creating one of these from the .dynamic table
/// the size, entity size and virtual address are different entries in arbitrary
/// order (DT_REL, DT_RELSZ, DT_RELENT for example).
struct DynRegionInfo {
  DynRegionInfo() = default;
  DynRegionInfo(const void *A, uint64_t S, uint64_t ES)
      : Addr(A), Size(S), EntSize(ES) {}

  /// Address in current address space.
  const void *Addr = nullptr;
  /// Size in bytes of the region.
  uint64_t Size = 0;
  /// Size of each entity in the region.
  uint64_t EntSize = 0;

  template <typename Type> ArrayRef<Type> getAsArrayRef() const {
    const Type *Start = reinterpret_cast<const Type *>(Addr);
    if (!Start)
      return {Start, Start};
    if (EntSize != sizeof(Type) || Size % EntSize) {
      // TODO: Add a section index to this warning.
      reportWarning("invalid section size (" + Twine(Size) +
                    ") or entity size (" + Twine(EntSize) + ")");
      return {Start, Start};
    }
    return {Start, Start + (Size / EntSize)};
  }
};

template <typename ELFT> class ELFDumper : public ObjDumper {
public:
  ELFDumper(const object::ELFObjectFile<ELFT> *ObjF, ScopedPrinter &Writer);

  void printFileHeaders() override;
  void printSectionHeaders() override;
  void printRelocations() override;
  void printDynamicRelocations() override;
  void printSymbols(bool PrintSymbols, bool PrintDynamicSymbols) override;
  void printHashSymbols() override;
  void printUnwindInfo() override;

  void printDynamicTable() override;
  void printNeededLibraries() override;
  void printProgramHeaders(bool PrintProgramHeaders,
                           cl::boolOrDefault PrintSectionMapping) override;
  void printHashTable() override;
  void printGnuHashTable() override;
  void printLoadName() override;
  void printVersionInfo() override;
  void printGroupSections() override;

  void printAttributes() override;
  void printMipsPLTGOT() override;
  void printMipsABIFlags() override;
  void printMipsReginfo() override;
  void printMipsOptions() override;

  void printStackMap() const override;

  void printHashHistogram() override;

  void printCGProfile() override;
  void printAddrsig() override;

  void printNotes() override;

  void printELFLinkerOptions() override;

  const object::ELFObjectFile<ELFT> *getElfObject() const { return ObjF; };

private:
  std::unique_ptr<DumpStyle<ELFT>> ELFDumperStyle;

  TYPEDEF_ELF_TYPES(ELFT)

  DynRegionInfo checkDRI(DynRegionInfo DRI) {
    const ELFFile<ELFT> *Obj = ObjF->getELFFile();
    if (DRI.Addr < Obj->base() ||
        reinterpret_cast<const uint8_t *>(DRI.Addr) + DRI.Size >
            Obj->base() + Obj->getBufSize())
      error(llvm::object::object_error::parse_failed);
    return DRI;
  }

  DynRegionInfo createDRIFrom(const Elf_Phdr *P, uintX_t EntSize) {
    return checkDRI(
        {ObjF->getELFFile()->base() + P->p_offset, P->p_filesz, EntSize});
  }

  DynRegionInfo createDRIFrom(const Elf_Shdr *S) {
    return checkDRI(
        {ObjF->getELFFile()->base() + S->sh_offset, S->sh_size, S->sh_entsize});
  }

  void loadDynamicTable(const ELFFile<ELFT> *Obj);
  void parseDynamicTable();

  StringRef getSymbolVersion(StringRef StrTab, const Elf_Sym *symb,
                             bool &IsDefault) const;
  void LoadVersionMap() const;
  void LoadVersionNeeds(const Elf_Shdr *ec) const;
  void LoadVersionDefs(const Elf_Shdr *sec) const;

  const object::ELFObjectFile<ELFT> *ObjF;
  DynRegionInfo DynRelRegion;
  DynRegionInfo DynRelaRegion;
  DynRegionInfo DynRelrRegion;
  DynRegionInfo DynPLTRelRegion;
  DynRegionInfo DynSymRegion;
  DynRegionInfo DynamicTable;
  StringRef DynamicStringTable;
  StringRef SOName = "<Not found>";
  const Elf_Hash *HashTable = nullptr;
  const Elf_GnuHash *GnuHashTable = nullptr;
  const Elf_Shdr *DotSymtabSec = nullptr;
  const Elf_Shdr *DotCGProfileSec = nullptr;
  const Elf_Shdr *DotAddrsigSec = nullptr;
  StringRef DynSymtabName;
  ArrayRef<Elf_Word> ShndxTable;

  const Elf_Shdr *SymbolVersionSection = nullptr;   // .gnu.version
  const Elf_Shdr *SymbolVersionNeedSection = nullptr; // .gnu.version_r
  const Elf_Shdr *SymbolVersionDefSection = nullptr; // .gnu.version_d

  // Records for each version index the corresponding Verdef or Vernaux entry.
  // This is filled the first time LoadVersionMap() is called.
  class VersionMapEntry : public PointerIntPair<const void *, 1> {
  public:
    // If the integer is 0, this is an Elf_Verdef*.
    // If the integer is 1, this is an Elf_Vernaux*.
    VersionMapEntry() : PointerIntPair<const void *, 1>(nullptr, 0) {}
    VersionMapEntry(const Elf_Verdef *verdef)
        : PointerIntPair<const void *, 1>(verdef, 0) {}
    VersionMapEntry(const Elf_Vernaux *vernaux)
        : PointerIntPair<const void *, 1>(vernaux, 1) {}

    bool isNull() const { return getPointer() == nullptr; }
    bool isVerdef() const { return !isNull() && getInt() == 0; }
    bool isVernaux() const { return !isNull() && getInt() == 1; }
    const Elf_Verdef *getVerdef() const {
      return isVerdef() ? (const Elf_Verdef *)getPointer() : nullptr;
    }
    const Elf_Vernaux *getVernaux() const {
      return isVernaux() ? (const Elf_Vernaux *)getPointer() : nullptr;
    }
  };
  mutable SmallVector<VersionMapEntry, 16> VersionMap;

public:
  Elf_Dyn_Range dynamic_table() const {
    // A valid .dynamic section contains an array of entries terminated
    // with a DT_NULL entry. However, sometimes the section content may
    // continue past the DT_NULL entry, so to dump the section correctly,
    // we first find the end of the entries by iterating over them.
    Elf_Dyn_Range Table = DynamicTable.getAsArrayRef<Elf_Dyn>();

    size_t Size = 0;
    while (Size < Table.size())
      if (Table[Size++].getTag() == DT_NULL)
        break;

    return Table.slice(0, Size);
  }

  Elf_Sym_Range dynamic_symbols() const {
    return DynSymRegion.getAsArrayRef<Elf_Sym>();
  }

  Elf_Rel_Range dyn_rels() const;
  Elf_Rela_Range dyn_relas() const;
  Elf_Relr_Range dyn_relrs() const;
  std::string getFullSymbolName(const Elf_Sym *Symbol, StringRef StrTable,
                                bool IsDynamic) const;
  void getSectionNameIndex(const Elf_Sym *Symbol, const Elf_Sym *FirstSym,
                           StringRef &SectionName,
                           unsigned &SectionIndex) const;
  std::string getStaticSymbolName(uint32_t Index) const;
  StringRef getSymbolVersionByIndex(StringRef StrTab,
                                    uint32_t VersionSymbolIndex,
                                    bool &IsDefault) const;

  void printSymbolsHelper(bool IsDynamic) const;
  void printDynamicEntry(raw_ostream &OS, uint64_t Type, uint64_t Value) const;

  const Elf_Shdr *getDotSymtabSec() const { return DotSymtabSec; }
  const Elf_Shdr *getDotCGProfileSec() const { return DotCGProfileSec; }
  const Elf_Shdr *getDotAddrsigSec() const { return DotAddrsigSec; }
  ArrayRef<Elf_Word> getShndxTable() const { return ShndxTable; }
  StringRef getDynamicStringTable() const { return DynamicStringTable; }
  const DynRegionInfo &getDynRelRegion() const { return DynRelRegion; }
  const DynRegionInfo &getDynRelaRegion() const { return DynRelaRegion; }
  const DynRegionInfo &getDynRelrRegion() const { return DynRelrRegion; }
  const DynRegionInfo &getDynPLTRelRegion() const { return DynPLTRelRegion; }
  const DynRegionInfo &getDynamicTableRegion() const { return DynamicTable; }
  const Elf_Hash *getHashTable() const { return HashTable; }
  const Elf_GnuHash *getGnuHashTable() const { return GnuHashTable; }
};

template <class ELFT>
void ELFDumper<ELFT>::printSymbolsHelper(bool IsDynamic) const {
  StringRef StrTable, SymtabName;
  size_t Entries = 0;
  Elf_Sym_Range Syms(nullptr, nullptr);
  const ELFFile<ELFT> *Obj = ObjF->getELFFile();
  if (IsDynamic) {
    StrTable = DynamicStringTable;
    Syms = dynamic_symbols();
    SymtabName = DynSymtabName;
    if (DynSymRegion.Addr)
      Entries = DynSymRegion.Size / DynSymRegion.EntSize;
  } else {
    if (!DotSymtabSec)
      return;
    StrTable = unwrapOrError(Obj->getStringTableForSymtab(*DotSymtabSec));
    Syms = unwrapOrError(Obj->symbols(DotSymtabSec));
    SymtabName = unwrapOrError(Obj->getSectionName(DotSymtabSec));
    Entries = DotSymtabSec->getEntityCount();
  }
  if (Syms.begin() == Syms.end())
    return;
  ELFDumperStyle->printSymtabMessage(Obj, SymtabName, Entries);
  for (const auto &Sym : Syms)
    ELFDumperStyle->printSymbol(Obj, &Sym, Syms.begin(), StrTable, IsDynamic);
}

template <class ELFT> class MipsGOTParser;

template <typename ELFT> class DumpStyle {
public:
  using Elf_Shdr = typename ELFT::Shdr;
  using Elf_Sym = typename ELFT::Sym;

  DumpStyle(ELFDumper<ELFT> *Dumper) : Dumper(Dumper) {}
  virtual ~DumpStyle() = default;

  virtual void printFileHeaders(const ELFFile<ELFT> *Obj) = 0;
  virtual void printGroupSections(const ELFFile<ELFT> *Obj) = 0;
  virtual void printRelocations(const ELFFile<ELFT> *Obj) = 0;
  virtual void printSectionHeaders(const ELFFile<ELFT> *Obj) = 0;
  virtual void printSymbols(const ELFFile<ELFT> *Obj, bool PrintSymbols,
                            bool PrintDynamicSymbols) = 0;
  virtual void printHashSymbols(const ELFFile<ELFT> *Obj) {}
  virtual void printDynamic(const ELFFile<ELFT> *Obj) {}
  virtual void printDynamicRelocations(const ELFFile<ELFT> *Obj) = 0;
  virtual void printSymtabMessage(const ELFFile<ELFT> *Obj, StringRef Name,
                                  size_t Offset) {}
  virtual void printSymbol(const ELFFile<ELFT> *Obj, const Elf_Sym *Symbol,
                           const Elf_Sym *FirstSym, StringRef StrTable,
                           bool IsDynamic) = 0;
  virtual void printProgramHeaders(const ELFFile<ELFT> *Obj,
                                   bool PrintProgramHeaders,
                                   cl::boolOrDefault PrintSectionMapping) = 0;
  virtual void printVersionSymbolSection(const ELFFile<ELFT> *Obj,
                                         const Elf_Shdr *Sec) = 0;
  virtual void printVersionDefinitionSection(const ELFFile<ELFT> *Obj,
                                             const Elf_Shdr *Sec) = 0;
  virtual void printVersionDependencySection(const ELFFile<ELFT> *Obj,
                                             const Elf_Shdr *Sec) = 0;
  virtual void printHashHistogram(const ELFFile<ELFT> *Obj) = 0;
  virtual void printCGProfile(const ELFFile<ELFT> *Obj) = 0;
  virtual void printAddrsig(const ELFFile<ELFT> *Obj) = 0;
  virtual void printNotes(const ELFFile<ELFT> *Obj) = 0;
  virtual void printELFLinkerOptions(const ELFFile<ELFT> *Obj) = 0;
  virtual void printMipsGOT(const MipsGOTParser<ELFT> &Parser) = 0;
  virtual void printMipsPLT(const MipsGOTParser<ELFT> &Parser) = 0;
  const ELFDumper<ELFT> *dumper() const { return Dumper; }

private:
  const ELFDumper<ELFT> *Dumper;
};

template <typename ELFT> class GNUStyle : public DumpStyle<ELFT> {
  formatted_raw_ostream &OS;

public:
  TYPEDEF_ELF_TYPES(ELFT)

  GNUStyle(ScopedPrinter &W, ELFDumper<ELFT> *Dumper)
      : DumpStyle<ELFT>(Dumper),
        OS(static_cast<formatted_raw_ostream&>(W.getOStream())) {
    assert (&W.getOStream() == &llvm::fouts());
  }

  void printFileHeaders(const ELFO *Obj) override;
  void printGroupSections(const ELFFile<ELFT> *Obj) override;
  void printRelocations(const ELFO *Obj) override;
  void printSectionHeaders(const ELFO *Obj) override;
  void printSymbols(const ELFO *Obj, bool PrintSymbols,
                    bool PrintDynamicSymbols) override;
  void printHashSymbols(const ELFO *Obj) override;
  void printDynamic(const ELFFile<ELFT> *Obj) override;
  void printDynamicRelocations(const ELFO *Obj) override;
  void printSymtabMessage(const ELFO *Obj, StringRef Name,
                          size_t Offset) override;
  void printProgramHeaders(const ELFO *Obj, bool PrintProgramHeaders,
                           cl::boolOrDefault PrintSectionMapping) override;
  void printVersionSymbolSection(const ELFFile<ELFT> *Obj,
                                 const Elf_Shdr *Sec) override;
  void printVersionDefinitionSection(const ELFFile<ELFT> *Obj,
                                     const Elf_Shdr *Sec) override;
  void printVersionDependencySection(const ELFFile<ELFT> *Obj,
                                     const Elf_Shdr *Sec) override;
  void printHashHistogram(const ELFFile<ELFT> *Obj) override;
  void printCGProfile(const ELFFile<ELFT> *Obj) override;
  void printAddrsig(const ELFFile<ELFT> *Obj) override;
  void printNotes(const ELFFile<ELFT> *Obj) override;
  void printELFLinkerOptions(const ELFFile<ELFT> *Obj) override;
  void printMipsGOT(const MipsGOTParser<ELFT> &Parser) override;
  void printMipsPLT(const MipsGOTParser<ELFT> &Parser) override;

private:
  struct Field {
    std::string Str;
    unsigned Column;

    Field(StringRef S, unsigned Col) : Str(S), Column(Col) {}
    Field(unsigned Col) : Column(Col) {}
  };

  template <typename T, typename TEnum>
  std::string printEnum(T Value, ArrayRef<EnumEntry<TEnum>> EnumValues) {
    for (const auto &EnumItem : EnumValues)
      if (EnumItem.Value == Value)
        return EnumItem.AltName;
    return to_hexString(Value, false);
  }

  template <typename T, typename TEnum>
  std::string printFlags(T Value, ArrayRef<EnumEntry<TEnum>> EnumValues,
                         TEnum EnumMask1 = {}, TEnum EnumMask2 = {},
                         TEnum EnumMask3 = {}) {
    std::string Str;
    for (const auto &Flag : EnumValues) {
      if (Flag.Value == 0)
        continue;

      TEnum EnumMask{};
      if (Flag.Value & EnumMask1)
        EnumMask = EnumMask1;
      else if (Flag.Value & EnumMask2)
        EnumMask = EnumMask2;
      else if (Flag.Value & EnumMask3)
        EnumMask = EnumMask3;
      bool IsEnum = (Flag.Value & EnumMask) != 0;
      if ((!IsEnum && (Value & Flag.Value) == Flag.Value) ||
          (IsEnum && (Value & EnumMask) == Flag.Value)) {
        if (!Str.empty())
          Str += ", ";
        Str += Flag.AltName;
      }
    }
    return Str;
  }

  formatted_raw_ostream &printField(struct Field F) {
    if (F.Column != 0)
      OS.PadToColumn(F.Column);
    OS << F.Str;
    OS.flush();
    return OS;
  }
  void printHashedSymbol(const ELFO *Obj, const Elf_Sym *FirstSym, uint32_t Sym,
                         StringRef StrTable, uint32_t Bucket);
  void printRelocHeader(unsigned SType);
  void printRelocation(const ELFO *Obj, const Elf_Shdr *SymTab,
                       const Elf_Rela &R, bool IsRela);
  void printRelocation(const ELFO *Obj, const Elf_Sym *Sym,
                       StringRef SymbolName, const Elf_Rela &R, bool IsRela);
  void printSymbol(const ELFO *Obj, const Elf_Sym *Symbol, const Elf_Sym *First,
                   StringRef StrTable, bool IsDynamic) override;
  std::string getSymbolSectionNdx(const ELFO *Obj, const Elf_Sym *Symbol,
                                  const Elf_Sym *FirstSym);
  void printDynamicRelocation(const ELFO *Obj, Elf_Rela R, bool IsRela);
  bool checkTLSSections(const Elf_Phdr &Phdr, const Elf_Shdr &Sec);
  bool checkoffsets(const Elf_Phdr &Phdr, const Elf_Shdr &Sec);
  bool checkVMA(const Elf_Phdr &Phdr, const Elf_Shdr &Sec);
  bool checkPTDynamic(const Elf_Phdr &Phdr, const Elf_Shdr &Sec);
  void printProgramHeaders(const ELFO *Obj);
  void printSectionMapping(const ELFO *Obj);
};

template <typename ELFT> class LLVMStyle : public DumpStyle<ELFT> {
public:
  TYPEDEF_ELF_TYPES(ELFT)

  LLVMStyle(ScopedPrinter &W, ELFDumper<ELFT> *Dumper)
      : DumpStyle<ELFT>(Dumper), W(W) {}

  void printFileHeaders(const ELFO *Obj) override;
  void printGroupSections(const ELFFile<ELFT> *Obj) override;
  void printRelocations(const ELFO *Obj) override;
  void printRelocations(const Elf_Shdr *Sec, const ELFO *Obj);
  void printSectionHeaders(const ELFO *Obj) override;
  void printSymbols(const ELFO *Obj, bool PrintSymbols,
                    bool PrintDynamicSymbols) override;
  void printDynamic(const ELFFile<ELFT> *Obj) override;
  void printDynamicRelocations(const ELFO *Obj) override;
  void printProgramHeaders(const ELFO *Obj, bool PrintProgramHeaders,
                           cl::boolOrDefault PrintSectionMapping) override;
  void printVersionSymbolSection(const ELFFile<ELFT> *Obj,
                                 const Elf_Shdr *Sec) override;
  void printVersionDefinitionSection(const ELFFile<ELFT> *Obj,
                                     const Elf_Shdr *Sec) override;
  void printVersionDependencySection(const ELFFile<ELFT> *Obj,
                                     const Elf_Shdr *Sec) override;
  void printHashHistogram(const ELFFile<ELFT> *Obj) override;
  void printCGProfile(const ELFFile<ELFT> *Obj) override;
  void printAddrsig(const ELFFile<ELFT> *Obj) override;
  void printNotes(const ELFFile<ELFT> *Obj) override;
  void printELFLinkerOptions(const ELFFile<ELFT> *Obj) override;
  void printMipsGOT(const MipsGOTParser<ELFT> &Parser) override;
  void printMipsPLT(const MipsGOTParser<ELFT> &Parser) override;

private:
  void printRelocation(const ELFO *Obj, Elf_Rela Rel, const Elf_Shdr *SymTab);
  void printDynamicRelocation(const ELFO *Obj, Elf_Rela Rel);
  void printSymbols(const ELFO *Obj);
  void printDynamicSymbols(const ELFO *Obj);
  void printSymbol(const ELFO *Obj, const Elf_Sym *Symbol, const Elf_Sym *First,
                   StringRef StrTable, bool IsDynamic) override;
  void printProgramHeaders(const ELFO *Obj);
  void printSectionMapping(const ELFO *Obj) {}

  ScopedPrinter &W;
};

} // end anonymous namespace

namespace llvm {

template <class ELFT>
static std::error_code createELFDumper(const ELFObjectFile<ELFT> *Obj,
                                       ScopedPrinter &Writer,
                                       std::unique_ptr<ObjDumper> &Result) {
  Result.reset(new ELFDumper<ELFT>(Obj, Writer));
  return readobj_error::success;
}

std::error_code createELFDumper(const object::ObjectFile *Obj,
                                ScopedPrinter &Writer,
                                std::unique_ptr<ObjDumper> &Result) {
  // Little-endian 32-bit
  if (const ELF32LEObjectFile *ELFObj = dyn_cast<ELF32LEObjectFile>(Obj))
    return createELFDumper(ELFObj, Writer, Result);

  // Big-endian 32-bit
  if (const ELF32BEObjectFile *ELFObj = dyn_cast<ELF32BEObjectFile>(Obj))
    return createELFDumper(ELFObj, Writer, Result);

  // Little-endian 64-bit
  if (const ELF64LEObjectFile *ELFObj = dyn_cast<ELF64LEObjectFile>(Obj))
    return createELFDumper(ELFObj, Writer, Result);

  // Big-endian 64-bit
  if (const ELF64BEObjectFile *ELFObj = dyn_cast<ELF64BEObjectFile>(Obj))
    return createELFDumper(ELFObj, Writer, Result);

  return readobj_error::unsupported_obj_file_format;
}

} // end namespace llvm

// Iterate through the versions needed section, and place each Elf_Vernaux
// in the VersionMap according to its index.
template <class ELFT>
void ELFDumper<ELFT>::LoadVersionNeeds(const Elf_Shdr *Sec) const {
  unsigned VerneedSize = Sec->sh_size;    // Size of section in bytes
  unsigned VerneedEntries = Sec->sh_info; // Number of Verneed entries
  const uint8_t *VerneedStart = reinterpret_cast<const uint8_t *>(
      ObjF->getELFFile()->base() + Sec->sh_offset);
  const uint8_t *VerneedEnd = VerneedStart + VerneedSize;
  // The first Verneed entry is at the start of the section.
  const uint8_t *VerneedBuf = VerneedStart;
  for (unsigned VerneedIndex = 0; VerneedIndex < VerneedEntries;
       ++VerneedIndex) {
    if (VerneedBuf + sizeof(Elf_Verneed) > VerneedEnd)
      report_fatal_error("Section ended unexpectedly while scanning "
                         "version needed records.");
    const Elf_Verneed *Verneed =
        reinterpret_cast<const Elf_Verneed *>(VerneedBuf);
    if (Verneed->vn_version != ELF::VER_NEED_CURRENT)
      report_fatal_error("Unexpected verneed version");
    // Iterate through the Vernaux entries
    const uint8_t *VernauxBuf = VerneedBuf + Verneed->vn_aux;
    for (unsigned VernauxIndex = 0; VernauxIndex < Verneed->vn_cnt;
         ++VernauxIndex) {
      if (VernauxBuf + sizeof(Elf_Vernaux) > VerneedEnd)
        report_fatal_error("Section ended unexpected while scanning auxiliary "
                           "version needed records.");
      const Elf_Vernaux *Vernaux =
          reinterpret_cast<const Elf_Vernaux *>(VernauxBuf);
      size_t Index = Vernaux->vna_other & ELF::VERSYM_VERSION;
      if (Index >= VersionMap.size())
        VersionMap.resize(Index + 1);
      VersionMap[Index] = VersionMapEntry(Vernaux);
      VernauxBuf += Vernaux->vna_next;
    }
    VerneedBuf += Verneed->vn_next;
  }
}

// Iterate through the version definitions, and place each Elf_Verdef
// in the VersionMap according to its index.
template <class ELFT>
void ELFDumper<ELFT>::LoadVersionDefs(const Elf_Shdr *Sec) const {
  unsigned VerdefSize = Sec->sh_size;    // Size of section in bytes
  unsigned VerdefEntries = Sec->sh_info; // Number of Verdef entries
  const uint8_t *VerdefStart = reinterpret_cast<const uint8_t *>(
      ObjF->getELFFile()->base() + Sec->sh_offset);
  const uint8_t *VerdefEnd = VerdefStart + VerdefSize;
  // The first Verdef entry is at the start of the section.
  const uint8_t *VerdefBuf = VerdefStart;
  for (unsigned VerdefIndex = 0; VerdefIndex < VerdefEntries; ++VerdefIndex) {
    if (VerdefBuf + sizeof(Elf_Verdef) > VerdefEnd)
      report_fatal_error("Section ended unexpectedly while scanning "
                         "version definitions.");
    const Elf_Verdef *Verdef = reinterpret_cast<const Elf_Verdef *>(VerdefBuf);
    if (Verdef->vd_version != ELF::VER_DEF_CURRENT)
      report_fatal_error("Unexpected verdef version");
    size_t Index = Verdef->vd_ndx & ELF::VERSYM_VERSION;
    if (Index >= VersionMap.size())
      VersionMap.resize(Index + 1);
    VersionMap[Index] = VersionMapEntry(Verdef);
    VerdefBuf += Verdef->vd_next;
  }
}

template <class ELFT> void ELFDumper<ELFT>::LoadVersionMap() const {
  // If there is no dynamic symtab or version table, there is nothing to do.
  if (!DynSymRegion.Addr || !SymbolVersionSection)
    return;

  // Has the VersionMap already been loaded?
  if (!VersionMap.empty())
    return;

  // The first two version indexes are reserved.
  // Index 0 is LOCAL, index 1 is GLOBAL.
  VersionMap.push_back(VersionMapEntry());
  VersionMap.push_back(VersionMapEntry());

  if (SymbolVersionDefSection)
    LoadVersionDefs(SymbolVersionDefSection);

  if (SymbolVersionNeedSection)
    LoadVersionNeeds(SymbolVersionNeedSection);
}

template <typename ELFT>
StringRef ELFDumper<ELFT>::getSymbolVersion(StringRef StrTab,
                                            const Elf_Sym *Sym,
                                            bool &IsDefault) const {
  // This is a dynamic symbol. Look in the GNU symbol version table.
  if (!SymbolVersionSection) {
    // No version table.
    IsDefault = false;
    return "";
  }

  // Determine the position in the symbol table of this entry.
  size_t EntryIndex = (reinterpret_cast<uintptr_t>(Sym) -
                        reinterpret_cast<uintptr_t>(DynSymRegion.Addr)) /
                       sizeof(Elf_Sym);

  // Get the corresponding version index entry.
  const Elf_Versym *Versym =
      unwrapOrError(ObjF->getELFFile()->template getEntry<Elf_Versym>(
          SymbolVersionSection, EntryIndex));
  return this->getSymbolVersionByIndex(StrTab, Versym->vs_index, IsDefault);
}

static std::string maybeDemangle(StringRef Name) {
  return opts::Demangle ? demangle(Name) : Name.str();
}

template <typename ELFT>
std::string ELFDumper<ELFT>::getStaticSymbolName(uint32_t Index) const {
  const ELFFile<ELFT> *Obj = ObjF->getELFFile();
  StringRef StrTable =
      unwrapOrError(Obj->getStringTableForSymtab(*DotSymtabSec));
  Elf_Sym_Range Syms = unwrapOrError(Obj->symbols(DotSymtabSec));
  if (Index >= Syms.size())
    reportError("Invalid symbol index");
  const Elf_Sym *Sym = &Syms[Index];
  return maybeDemangle(unwrapOrError(Sym->getName(StrTable)));
}

template <typename ELFT>
StringRef ELFDumper<ELFT>::getSymbolVersionByIndex(StringRef StrTab,
                                                   uint32_t SymbolVersionIndex,
                                                   bool &IsDefault) const {
  size_t VersionIndex = SymbolVersionIndex & VERSYM_VERSION;

  // Special markers for unversioned symbols.
  if (VersionIndex == VER_NDX_LOCAL || VersionIndex == VER_NDX_GLOBAL) {
    IsDefault = false;
    return "";
  }

  // Lookup this symbol in the version table.
  LoadVersionMap();
  if (VersionIndex >= VersionMap.size() || VersionMap[VersionIndex].isNull())
    reportError("Invalid version entry");
  const VersionMapEntry &Entry = VersionMap[VersionIndex];

  // Get the version name string.
  size_t NameOffset;
  if (Entry.isVerdef()) {
    // The first Verdaux entry holds the name.
    NameOffset = Entry.getVerdef()->getAux()->vda_name;
    IsDefault = !(SymbolVersionIndex & VERSYM_HIDDEN);
  } else {
    NameOffset = Entry.getVernaux()->vna_name;
    IsDefault = false;
  }
  if (NameOffset >= StrTab.size())
    reportError("Invalid string offset");
  return StrTab.data() + NameOffset;
}

template <typename ELFT>
std::string ELFDumper<ELFT>::getFullSymbolName(const Elf_Sym *Symbol,
                                               StringRef StrTable,
                                               bool IsDynamic) const {
  std::string SymbolName =
      maybeDemangle(unwrapOrError(Symbol->getName(StrTable)));

  if (SymbolName.empty() && Symbol->getType() == ELF::STT_SECTION) {
    unsigned SectionIndex;
    StringRef SectionName;
    Elf_Sym_Range Syms =
        unwrapOrError(ObjF->getELFFile()->symbols(DotSymtabSec));
    getSectionNameIndex(Symbol, Syms.begin(), SectionName, SectionIndex);
    return SectionName;
  }

  if (!IsDynamic)
    return SymbolName;

  bool IsDefault;
  StringRef Version = getSymbolVersion(StrTable, &*Symbol, IsDefault);
  if (!Version.empty()) {
    SymbolName += (IsDefault ? "@@" : "@");
    SymbolName += Version;
  }
  return SymbolName;
}

template <typename ELFT>
void ELFDumper<ELFT>::getSectionNameIndex(const Elf_Sym *Symbol,
                                          const Elf_Sym *FirstSym,
                                          StringRef &SectionName,
                                          unsigned &SectionIndex) const {
  SectionIndex = Symbol->st_shndx;
  if (Symbol->isUndefined())
    SectionName = "Undefined";
  else if (Symbol->isProcessorSpecific())
    SectionName = "Processor Specific";
  else if (Symbol->isOSSpecific())
    SectionName = "Operating System Specific";
  else if (Symbol->isAbsolute())
    SectionName = "Absolute";
  else if (Symbol->isCommon())
    SectionName = "Common";
  else if (Symbol->isReserved() && SectionIndex != SHN_XINDEX)
    SectionName = "Reserved";
  else {
    if (SectionIndex == SHN_XINDEX)
      SectionIndex = unwrapOrError(object::getExtendedSymbolTableIndex<ELFT>(
          Symbol, FirstSym, ShndxTable));
    const ELFFile<ELFT> *Obj = ObjF->getELFFile();
    const typename ELFT::Shdr *Sec =
        unwrapOrError(Obj->getSection(SectionIndex));
    SectionName = unwrapOrError(Obj->getSectionName(Sec));
  }
}

template <class ELFO>
static const typename ELFO::Elf_Shdr *
findNotEmptySectionByAddress(const ELFO *Obj, uint64_t Addr) {
  for (const auto &Shdr : unwrapOrError(Obj->sections()))
    if (Shdr.sh_addr == Addr && Shdr.sh_size > 0)
      return &Shdr;
  return nullptr;
}

template <class ELFO>
static const typename ELFO::Elf_Shdr *findSectionByName(const ELFO &Obj,
                                                        StringRef Name) {
  for (const auto &Shdr : unwrapOrError(Obj.sections())) {
    if (Name == unwrapOrError(Obj.getSectionName(&Shdr)))
      return &Shdr;
  }
  return nullptr;
}

static const EnumEntry<unsigned> ElfClass[] = {
  {"None",   "none",   ELF::ELFCLASSNONE},
  {"32-bit", "ELF32",  ELF::ELFCLASS32},
  {"64-bit", "ELF64",  ELF::ELFCLASS64},
};

static const EnumEntry<unsigned> ElfDataEncoding[] = {
  {"None",         "none",                          ELF::ELFDATANONE},
  {"LittleEndian", "2's complement, little endian", ELF::ELFDATA2LSB},
  {"BigEndian",    "2's complement, big endian",    ELF::ELFDATA2MSB},
};

static const EnumEntry<unsigned> ElfObjectFileType[] = {
  {"None",         "NONE (none)",              ELF::ET_NONE},
  {"Relocatable",  "REL (Relocatable file)",   ELF::ET_REL},
  {"Executable",   "EXEC (Executable file)",   ELF::ET_EXEC},
  {"SharedObject", "DYN (Shared object file)", ELF::ET_DYN},
  {"Core",         "CORE (Core file)",         ELF::ET_CORE},
};

static const EnumEntry<unsigned> ElfOSABI[] = {
  {"SystemV",      "UNIX - System V",      ELF::ELFOSABI_NONE},
  {"HPUX",         "UNIX - HP-UX",         ELF::ELFOSABI_HPUX},
  {"NetBSD",       "UNIX - NetBSD",        ELF::ELFOSABI_NETBSD},
  {"GNU/Linux",    "UNIX - GNU",           ELF::ELFOSABI_LINUX},
  {"GNU/Hurd",     "GNU/Hurd",             ELF::ELFOSABI_HURD},
  {"Solaris",      "UNIX - Solaris",       ELF::ELFOSABI_SOLARIS},
  {"AIX",          "UNIX - AIX",           ELF::ELFOSABI_AIX},
  {"IRIX",         "UNIX - IRIX",          ELF::ELFOSABI_IRIX},
  {"FreeBSD",      "UNIX - FreeBSD",       ELF::ELFOSABI_FREEBSD},
  {"TRU64",        "UNIX - TRU64",         ELF::ELFOSABI_TRU64},
  {"Modesto",      "Novell - Modesto",     ELF::ELFOSABI_MODESTO},
  {"OpenBSD",      "UNIX - OpenBSD",       ELF::ELFOSABI_OPENBSD},
  {"OpenVMS",      "VMS - OpenVMS",        ELF::ELFOSABI_OPENVMS},
  {"NSK",          "HP - Non-Stop Kernel", ELF::ELFOSABI_NSK},
  {"AROS",         "AROS",                 ELF::ELFOSABI_AROS},
  {"FenixOS",      "FenixOS",              ELF::ELFOSABI_FENIXOS},
  {"CloudABI",     "CloudABI",             ELF::ELFOSABI_CLOUDABI},
  {"Standalone",   "Standalone App",       ELF::ELFOSABI_STANDALONE}
};

static const EnumEntry<unsigned> SymVersionFlags[] = {
    {"Base", "BASE", VER_FLG_BASE},
    {"Weak", "WEAK", VER_FLG_WEAK},
    {"Info", "INFO", VER_FLG_INFO}};

static const EnumEntry<unsigned> AMDGPUElfOSABI[] = {
  {"AMDGPU_HSA",    "AMDGPU - HSA",    ELF::ELFOSABI_AMDGPU_HSA},
  {"AMDGPU_PAL",    "AMDGPU - PAL",    ELF::ELFOSABI_AMDGPU_PAL},
  {"AMDGPU_MESA3D", "AMDGPU - MESA3D", ELF::ELFOSABI_AMDGPU_MESA3D}
};

static const EnumEntry<unsigned> ARMElfOSABI[] = {
  {"ARM", "ARM", ELF::ELFOSABI_ARM}
};

static const EnumEntry<unsigned> C6000ElfOSABI[] = {
  {"C6000_ELFABI", "Bare-metal C6000", ELF::ELFOSABI_C6000_ELFABI},
  {"C6000_LINUX",  "Linux C6000",      ELF::ELFOSABI_C6000_LINUX}
};

static const EnumEntry<unsigned> ElfMachineType[] = {
  ENUM_ENT(EM_NONE,          "None"),
  ENUM_ENT(EM_M32,           "WE32100"),
  ENUM_ENT(EM_SPARC,         "Sparc"),
  ENUM_ENT(EM_386,           "Intel 80386"),
  ENUM_ENT(EM_68K,           "MC68000"),
  ENUM_ENT(EM_88K,           "MC88000"),
  ENUM_ENT(EM_IAMCU,         "EM_IAMCU"),
  ENUM_ENT(EM_860,           "Intel 80860"),
  ENUM_ENT(EM_MIPS,          "MIPS R3000"),
  ENUM_ENT(EM_S370,          "IBM System/370"),
  ENUM_ENT(EM_MIPS_RS3_LE,   "MIPS R3000 little-endian"),
  ENUM_ENT(EM_PARISC,        "HPPA"),
  ENUM_ENT(EM_VPP500,        "Fujitsu VPP500"),
  ENUM_ENT(EM_SPARC32PLUS,   "Sparc v8+"),
  ENUM_ENT(EM_960,           "Intel 80960"),
  ENUM_ENT(EM_PPC,           "PowerPC"),
  ENUM_ENT(EM_PPC64,         "PowerPC64"),
  ENUM_ENT(EM_S390,          "IBM S/390"),
  ENUM_ENT(EM_SPU,           "SPU"),
  ENUM_ENT(EM_V800,          "NEC V800 series"),
  ENUM_ENT(EM_FR20,          "Fujistsu FR20"),
  ENUM_ENT(EM_RH32,          "TRW RH-32"),
  ENUM_ENT(EM_RCE,           "Motorola RCE"),
  ENUM_ENT(EM_ARM,           "ARM"),
  ENUM_ENT(EM_ALPHA,         "EM_ALPHA"),
  ENUM_ENT(EM_SH,            "Hitachi SH"),
  ENUM_ENT(EM_SPARCV9,       "Sparc v9"),
  ENUM_ENT(EM_TRICORE,       "Siemens Tricore"),
  ENUM_ENT(EM_ARC,           "ARC"),
  ENUM_ENT(EM_H8_300,        "Hitachi H8/300"),
  ENUM_ENT(EM_H8_300H,       "Hitachi H8/300H"),
  ENUM_ENT(EM_H8S,           "Hitachi H8S"),
  ENUM_ENT(EM_H8_500,        "Hitachi H8/500"),
  ENUM_ENT(EM_IA_64,         "Intel IA-64"),
  ENUM_ENT(EM_MIPS_X,        "Stanford MIPS-X"),
  ENUM_ENT(EM_COLDFIRE,      "Motorola Coldfire"),
  ENUM_ENT(EM_68HC12,        "Motorola MC68HC12 Microcontroller"),
  ENUM_ENT(EM_MMA,           "Fujitsu Multimedia Accelerator"),
  ENUM_ENT(EM_PCP,           "Siemens PCP"),
  ENUM_ENT(EM_NCPU,          "Sony nCPU embedded RISC processor"),
  ENUM_ENT(EM_NDR1,          "Denso NDR1 microprocesspr"),
  ENUM_ENT(EM_STARCORE,      "Motorola Star*Core processor"),
  ENUM_ENT(EM_ME16,          "Toyota ME16 processor"),
  ENUM_ENT(EM_ST100,         "STMicroelectronics ST100 processor"),
  ENUM_ENT(EM_TINYJ,         "Advanced Logic Corp. TinyJ embedded processor"),
  ENUM_ENT(EM_X86_64,        "Advanced Micro Devices X86-64"),
  ENUM_ENT(EM_PDSP,          "Sony DSP processor"),
  ENUM_ENT(EM_PDP10,         "Digital Equipment Corp. PDP-10"),
  ENUM_ENT(EM_PDP11,         "Digital Equipment Corp. PDP-11"),
  ENUM_ENT(EM_FX66,          "Siemens FX66 microcontroller"),
  ENUM_ENT(EM_ST9PLUS,       "STMicroelectronics ST9+ 8/16 bit microcontroller"),
  ENUM_ENT(EM_ST7,           "STMicroelectronics ST7 8-bit microcontroller"),
  ENUM_ENT(EM_68HC16,        "Motorola MC68HC16 Microcontroller"),
  ENUM_ENT(EM_68HC11,        "Motorola MC68HC11 Microcontroller"),
  ENUM_ENT(EM_68HC08,        "Motorola MC68HC08 Microcontroller"),
  ENUM_ENT(EM_68HC05,        "Motorola MC68HC05 Microcontroller"),
  ENUM_ENT(EM_SVX,           "Silicon Graphics SVx"),
  ENUM_ENT(EM_ST19,          "STMicroelectronics ST19 8-bit microcontroller"),
  ENUM_ENT(EM_VAX,           "Digital VAX"),
  ENUM_ENT(EM_CRIS,          "Axis Communications 32-bit embedded processor"),
  ENUM_ENT(EM_JAVELIN,       "Infineon Technologies 32-bit embedded cpu"),
  ENUM_ENT(EM_FIREPATH,      "Element 14 64-bit DSP processor"),
  ENUM_ENT(EM_ZSP,           "LSI Logic's 16-bit DSP processor"),
  ENUM_ENT(EM_MMIX,          "Donald Knuth's educational 64-bit processor"),
  ENUM_ENT(EM_HUANY,         "Harvard Universitys's machine-independent object format"),
  ENUM_ENT(EM_PRISM,         "Vitesse Prism"),
  ENUM_ENT(EM_AVR,           "Atmel AVR 8-bit microcontroller"),
  ENUM_ENT(EM_FR30,          "Fujitsu FR30"),
  ENUM_ENT(EM_D10V,          "Mitsubishi D10V"),
  ENUM_ENT(EM_D30V,          "Mitsubishi D30V"),
  ENUM_ENT(EM_V850,          "NEC v850"),
  ENUM_ENT(EM_M32R,          "Renesas M32R (formerly Mitsubishi M32r)"),
  ENUM_ENT(EM_MN10300,       "Matsushita MN10300"),
  ENUM_ENT(EM_MN10200,       "Matsushita MN10200"),
  ENUM_ENT(EM_PJ,            "picoJava"),
  ENUM_ENT(EM_OPENRISC,      "OpenRISC 32-bit embedded processor"),
  ENUM_ENT(EM_ARC_COMPACT,   "EM_ARC_COMPACT"),
  ENUM_ENT(EM_XTENSA,        "Tensilica Xtensa Processor"),
  ENUM_ENT(EM_VIDEOCORE,     "Alphamosaic VideoCore processor"),
  ENUM_ENT(EM_TMM_GPP,       "Thompson Multimedia General Purpose Processor"),
  ENUM_ENT(EM_NS32K,         "National Semiconductor 32000 series"),
  ENUM_ENT(EM_TPC,           "Tenor Network TPC processor"),
  ENUM_ENT(EM_SNP1K,         "EM_SNP1K"),
  ENUM_ENT(EM_ST200,         "STMicroelectronics ST200 microcontroller"),
  ENUM_ENT(EM_IP2K,          "Ubicom IP2xxx 8-bit microcontrollers"),
  ENUM_ENT(EM_MAX,           "MAX Processor"),
  ENUM_ENT(EM_CR,            "National Semiconductor CompactRISC"),
  ENUM_ENT(EM_F2MC16,        "Fujitsu F2MC16"),
  ENUM_ENT(EM_MSP430,        "Texas Instruments msp430 microcontroller"),
  ENUM_ENT(EM_BLACKFIN,      "Analog Devices Blackfin"),
  ENUM_ENT(EM_SE_C33,        "S1C33 Family of Seiko Epson processors"),
  ENUM_ENT(EM_SEP,           "Sharp embedded microprocessor"),
  ENUM_ENT(EM_ARCA,          "Arca RISC microprocessor"),
  ENUM_ENT(EM_UNICORE,       "Unicore"),
  ENUM_ENT(EM_EXCESS,        "eXcess 16/32/64-bit configurable embedded CPU"),
  ENUM_ENT(EM_DXP,           "Icera Semiconductor Inc. Deep Execution Processor"),
  ENUM_ENT(EM_ALTERA_NIOS2,  "Altera Nios"),
  ENUM_ENT(EM_CRX,           "National Semiconductor CRX microprocessor"),
  ENUM_ENT(EM_XGATE,         "Motorola XGATE embedded processor"),
  ENUM_ENT(EM_C166,          "Infineon Technologies xc16x"),
  ENUM_ENT(EM_M16C,          "Renesas M16C"),
  ENUM_ENT(EM_DSPIC30F,      "Microchip Technology dsPIC30F Digital Signal Controller"),
  ENUM_ENT(EM_CE,            "Freescale Communication Engine RISC core"),
  ENUM_ENT(EM_M32C,          "Renesas M32C"),
  ENUM_ENT(EM_TSK3000,       "Altium TSK3000 core"),
  ENUM_ENT(EM_RS08,          "Freescale RS08 embedded processor"),
  ENUM_ENT(EM_SHARC,         "EM_SHARC"),
  ENUM_ENT(EM_ECOG2,         "Cyan Technology eCOG2 microprocessor"),
  ENUM_ENT(EM_SCORE7,        "SUNPLUS S+Core"),
  ENUM_ENT(EM_DSP24,         "New Japan Radio (NJR) 24-bit DSP Processor"),
  ENUM_ENT(EM_VIDEOCORE3,    "Broadcom VideoCore III processor"),
  ENUM_ENT(EM_LATTICEMICO32, "Lattice Mico32"),
  ENUM_ENT(EM_SE_C17,        "Seiko Epson C17 family"),
  ENUM_ENT(EM_TI_C6000,      "Texas Instruments TMS320C6000 DSP family"),
  ENUM_ENT(EM_TI_C2000,      "Texas Instruments TMS320C2000 DSP family"),
  ENUM_ENT(EM_TI_C5500,      "Texas Instruments TMS320C55x DSP family"),
  ENUM_ENT(EM_MMDSP_PLUS,    "STMicroelectronics 64bit VLIW Data Signal Processor"),
  ENUM_ENT(EM_CYPRESS_M8C,   "Cypress M8C microprocessor"),
  ENUM_ENT(EM_R32C,          "Renesas R32C series microprocessors"),
  ENUM_ENT(EM_TRIMEDIA,      "NXP Semiconductors TriMedia architecture family"),
  ENUM_ENT(EM_HEXAGON,       "Qualcomm Hexagon"),
  ENUM_ENT(EM_8051,          "Intel 8051 and variants"),
  ENUM_ENT(EM_STXP7X,        "STMicroelectronics STxP7x family"),
  ENUM_ENT(EM_NDS32,         "Andes Technology compact code size embedded RISC processor family"),
  ENUM_ENT(EM_ECOG1,         "Cyan Technology eCOG1 microprocessor"),
  ENUM_ENT(EM_ECOG1X,        "Cyan Technology eCOG1X family"),
  ENUM_ENT(EM_MAXQ30,        "Dallas Semiconductor MAXQ30 Core microcontrollers"),
  ENUM_ENT(EM_XIMO16,        "New Japan Radio (NJR) 16-bit DSP Processor"),
  ENUM_ENT(EM_MANIK,         "M2000 Reconfigurable RISC Microprocessor"),
  ENUM_ENT(EM_CRAYNV2,       "Cray Inc. NV2 vector architecture"),
  ENUM_ENT(EM_RX,            "Renesas RX"),
  ENUM_ENT(EM_METAG,         "Imagination Technologies Meta processor architecture"),
  ENUM_ENT(EM_MCST_ELBRUS,   "MCST Elbrus general purpose hardware architecture"),
  ENUM_ENT(EM_ECOG16,        "Cyan Technology eCOG16 family"),
  ENUM_ENT(EM_CR16,          "Xilinx MicroBlaze"),
  ENUM_ENT(EM_ETPU,          "Freescale Extended Time Processing Unit"),
  ENUM_ENT(EM_SLE9X,         "Infineon Technologies SLE9X core"),
  ENUM_ENT(EM_L10M,          "EM_L10M"),
  ENUM_ENT(EM_K10M,          "EM_K10M"),
  ENUM_ENT(EM_AARCH64,       "AArch64"),
  ENUM_ENT(EM_AVR32,         "Atmel Corporation 32-bit microprocessor family"),
  ENUM_ENT(EM_STM8,          "STMicroeletronics STM8 8-bit microcontroller"),
  ENUM_ENT(EM_TILE64,        "Tilera TILE64 multicore architecture family"),
  ENUM_ENT(EM_TILEPRO,       "Tilera TILEPro multicore architecture family"),
  ENUM_ENT(EM_CUDA,          "NVIDIA CUDA architecture"),
  ENUM_ENT(EM_TILEGX,        "Tilera TILE-Gx multicore architecture family"),
  ENUM_ENT(EM_CLOUDSHIELD,   "EM_CLOUDSHIELD"),
  ENUM_ENT(EM_COREA_1ST,     "EM_COREA_1ST"),
  ENUM_ENT(EM_COREA_2ND,     "EM_COREA_2ND"),
  ENUM_ENT(EM_ARC_COMPACT2,  "EM_ARC_COMPACT2"),
  ENUM_ENT(EM_OPEN8,         "EM_OPEN8"),
  ENUM_ENT(EM_RL78,          "Renesas RL78"),
  ENUM_ENT(EM_VIDEOCORE5,    "Broadcom VideoCore V processor"),
  ENUM_ENT(EM_78KOR,         "EM_78KOR"),
  ENUM_ENT(EM_56800EX,       "EM_56800EX"),
  ENUM_ENT(EM_AMDGPU,        "EM_AMDGPU"),
  ENUM_ENT(EM_RISCV,         "RISC-V"),
  ENUM_ENT(EM_LANAI,         "EM_LANAI"),
  ENUM_ENT(EM_BPF,           "EM_BPF"),
};

static const EnumEntry<unsigned> ElfSymbolBindings[] = {
    {"Local",  "LOCAL",  ELF::STB_LOCAL},
    {"Global", "GLOBAL", ELF::STB_GLOBAL},
    {"Weak",   "WEAK",   ELF::STB_WEAK},
    {"Unique", "UNIQUE", ELF::STB_GNU_UNIQUE}};

static const EnumEntry<unsigned> ElfSymbolVisibilities[] = {
    {"DEFAULT",   "DEFAULT",   ELF::STV_DEFAULT},
    {"INTERNAL",  "INTERNAL",  ELF::STV_INTERNAL},
    {"HIDDEN",    "HIDDEN",    ELF::STV_HIDDEN},
    {"PROTECTED", "PROTECTED", ELF::STV_PROTECTED}};

static const EnumEntry<unsigned> AMDGPUSymbolTypes[] = {
  { "AMDGPU_HSA_KERNEL",            ELF::STT_AMDGPU_HSA_KERNEL }
};

static const char *getGroupType(uint32_t Flag) {
  if (Flag & ELF::GRP_COMDAT)
    return "COMDAT";
  else
    return "(unknown)";
}

static const EnumEntry<unsigned> ElfSectionFlags[] = {
  ENUM_ENT(SHF_WRITE,            "W"),
  ENUM_ENT(SHF_ALLOC,            "A"),
  ENUM_ENT(SHF_EXCLUDE,          "E"),
  ENUM_ENT(SHF_EXECINSTR,        "X"),
  ENUM_ENT(SHF_MERGE,            "M"),
  ENUM_ENT(SHF_STRINGS,          "S"),
  ENUM_ENT(SHF_INFO_LINK,        "I"),
  ENUM_ENT(SHF_LINK_ORDER,       "L"),
  ENUM_ENT(SHF_OS_NONCONFORMING, "o"),
  ENUM_ENT(SHF_GROUP,            "G"),
  ENUM_ENT(SHF_TLS,              "T"),
  ENUM_ENT(SHF_MASKOS,           "o"),
  ENUM_ENT(SHF_MASKPROC,         "p"),
  ENUM_ENT_1(SHF_COMPRESSED),
};

static const EnumEntry<unsigned> ElfXCoreSectionFlags[] = {
  LLVM_READOBJ_ENUM_ENT(ELF, XCORE_SHF_CP_SECTION),
  LLVM_READOBJ_ENUM_ENT(ELF, XCORE_SHF_DP_SECTION)
};

static const EnumEntry<unsigned> ElfARMSectionFlags[] = {
  LLVM_READOBJ_ENUM_ENT(ELF, SHF_ARM_PURECODE)
};

static const EnumEntry<unsigned> ElfHexagonSectionFlags[] = {
  LLVM_READOBJ_ENUM_ENT(ELF, SHF_HEX_GPREL)
};

static const EnumEntry<unsigned> ElfMipsSectionFlags[] = {
  LLVM_READOBJ_ENUM_ENT(ELF, SHF_MIPS_NODUPES),
  LLVM_READOBJ_ENUM_ENT(ELF, SHF_MIPS_NAMES  ),
  LLVM_READOBJ_ENUM_ENT(ELF, SHF_MIPS_LOCAL  ),
  LLVM_READOBJ_ENUM_ENT(ELF, SHF_MIPS_NOSTRIP),
  LLVM_READOBJ_ENUM_ENT(ELF, SHF_MIPS_GPREL  ),
  LLVM_READOBJ_ENUM_ENT(ELF, SHF_MIPS_MERGE  ),
  LLVM_READOBJ_ENUM_ENT(ELF, SHF_MIPS_ADDR   ),
  LLVM_READOBJ_ENUM_ENT(ELF, SHF_MIPS_STRING )
};

static const EnumEntry<unsigned> ElfX86_64SectionFlags[] = {
  LLVM_READOBJ_ENUM_ENT(ELF, SHF_X86_64_LARGE)
};

static std::string getGNUFlags(uint64_t Flags) {
  std::string Str;
  for (auto Entry : ElfSectionFlags) {
    uint64_t Flag = Entry.Value & Flags;
    Flags &= ~Entry.Value;
    switch (Flag) {
    case ELF::SHF_WRITE:
    case ELF::SHF_ALLOC:
    case ELF::SHF_EXECINSTR:
    case ELF::SHF_MERGE:
    case ELF::SHF_STRINGS:
    case ELF::SHF_INFO_LINK:
    case ELF::SHF_LINK_ORDER:
    case ELF::SHF_OS_NONCONFORMING:
    case ELF::SHF_GROUP:
    case ELF::SHF_TLS:
    case ELF::SHF_EXCLUDE:
      Str += Entry.AltName;
      break;
    default:
      if (Flag & ELF::SHF_MASKOS)
        Str += "o";
      else if (Flag & ELF::SHF_MASKPROC)
        Str += "p";
      else if (Flag)
        Str += "x";
    }
  }
  return Str;
}

static const char *getElfSegmentType(unsigned Arch, unsigned Type) {
  // Check potentially overlapped processor-specific
  // program header type.
  switch (Arch) {
  case ELF::EM_ARM:
    switch (Type) { LLVM_READOBJ_ENUM_CASE(ELF, PT_ARM_EXIDX); }
    break;
  case ELF::EM_MIPS:
  case ELF::EM_MIPS_RS3_LE:
    switch (Type) {
      LLVM_READOBJ_ENUM_CASE(ELF, PT_MIPS_REGINFO);
    LLVM_READOBJ_ENUM_CASE(ELF, PT_MIPS_RTPROC);
    LLVM_READOBJ_ENUM_CASE(ELF, PT_MIPS_OPTIONS);
    LLVM_READOBJ_ENUM_CASE(ELF, PT_MIPS_ABIFLAGS);
    }
    break;
  }

  switch (Type) {
  LLVM_READOBJ_ENUM_CASE(ELF, PT_NULL   );
  LLVM_READOBJ_ENUM_CASE(ELF, PT_LOAD   );
  LLVM_READOBJ_ENUM_CASE(ELF, PT_DYNAMIC);
  LLVM_READOBJ_ENUM_CASE(ELF, PT_INTERP );
  LLVM_READOBJ_ENUM_CASE(ELF, PT_NOTE   );
  LLVM_READOBJ_ENUM_CASE(ELF, PT_SHLIB  );
  LLVM_READOBJ_ENUM_CASE(ELF, PT_PHDR   );
  LLVM_READOBJ_ENUM_CASE(ELF, PT_TLS    );

  LLVM_READOBJ_ENUM_CASE(ELF, PT_GNU_EH_FRAME);
  LLVM_READOBJ_ENUM_CASE(ELF, PT_SUNW_UNWIND);

    LLVM_READOBJ_ENUM_CASE(ELF, PT_GNU_STACK);
    LLVM_READOBJ_ENUM_CASE(ELF, PT_GNU_RELRO);

    LLVM_READOBJ_ENUM_CASE(ELF, PT_OPENBSD_RANDOMIZE);
    LLVM_READOBJ_ENUM_CASE(ELF, PT_OPENBSD_WXNEEDED);
    LLVM_READOBJ_ENUM_CASE(ELF, PT_OPENBSD_BOOTDATA);

  default:
    return "";
  }
}

static std::string getElfPtType(unsigned Arch, unsigned Type) {
  switch (Type) {
    LLVM_READOBJ_PHDR_ENUM(ELF, PT_NULL)
    LLVM_READOBJ_PHDR_ENUM(ELF, PT_LOAD)
    LLVM_READOBJ_PHDR_ENUM(ELF, PT_DYNAMIC)
    LLVM_READOBJ_PHDR_ENUM(ELF, PT_INTERP)
    LLVM_READOBJ_PHDR_ENUM(ELF, PT_NOTE)
    LLVM_READOBJ_PHDR_ENUM(ELF, PT_SHLIB)
    LLVM_READOBJ_PHDR_ENUM(ELF, PT_PHDR)
    LLVM_READOBJ_PHDR_ENUM(ELF, PT_TLS)
    LLVM_READOBJ_PHDR_ENUM(ELF, PT_GNU_EH_FRAME)
    LLVM_READOBJ_PHDR_ENUM(ELF, PT_SUNW_UNWIND)
    LLVM_READOBJ_PHDR_ENUM(ELF, PT_GNU_STACK)
    LLVM_READOBJ_PHDR_ENUM(ELF, PT_GNU_RELRO)
  default:
    // All machine specific PT_* types
    switch (Arch) {
    case ELF::EM_ARM:
      if (Type == ELF::PT_ARM_EXIDX)
        return "EXIDX";
      break;
    case ELF::EM_MIPS:
    case ELF::EM_MIPS_RS3_LE:
      switch (Type) {
      case PT_MIPS_REGINFO:
        return "REGINFO";
      case PT_MIPS_RTPROC:
        return "RTPROC";
      case PT_MIPS_OPTIONS:
        return "OPTIONS";
      case PT_MIPS_ABIFLAGS:
        return "ABIFLAGS";
      }
      break;
    }
  }
  return std::string("<unknown>: ") + to_string(format_hex(Type, 1));
}

static const EnumEntry<unsigned> ElfSegmentFlags[] = {
  LLVM_READOBJ_ENUM_ENT(ELF, PF_X),
  LLVM_READOBJ_ENUM_ENT(ELF, PF_W),
  LLVM_READOBJ_ENUM_ENT(ELF, PF_R)
};

static const EnumEntry<unsigned> ElfHeaderMipsFlags[] = {
  ENUM_ENT(EF_MIPS_NOREORDER, "noreorder"),
  ENUM_ENT(EF_MIPS_PIC, "pic"),
  ENUM_ENT(EF_MIPS_CPIC, "cpic"),
  ENUM_ENT(EF_MIPS_ABI2, "abi2"),
  ENUM_ENT(EF_MIPS_32BITMODE, "32bitmode"),
  ENUM_ENT(EF_MIPS_FP64, "fp64"),
  ENUM_ENT(EF_MIPS_NAN2008, "nan2008"),
  ENUM_ENT(EF_MIPS_ABI_O32, "o32"),
  ENUM_ENT(EF_MIPS_ABI_O64, "o64"),
  ENUM_ENT(EF_MIPS_ABI_EABI32, "eabi32"),
  ENUM_ENT(EF_MIPS_ABI_EABI64, "eabi64"),
  ENUM_ENT(EF_MIPS_MACH_3900, "3900"),
  ENUM_ENT(EF_MIPS_MACH_4010, "4010"),
  ENUM_ENT(EF_MIPS_MACH_4100, "4100"),
  ENUM_ENT(EF_MIPS_MACH_4650, "4650"),
  ENUM_ENT(EF_MIPS_MACH_4120, "4120"),
  ENUM_ENT(EF_MIPS_MACH_4111, "4111"),
  ENUM_ENT(EF_MIPS_MACH_SB1, "sb1"),
  ENUM_ENT(EF_MIPS_MACH_OCTEON, "octeon"),
  ENUM_ENT(EF_MIPS_MACH_XLR, "xlr"),
  ENUM_ENT(EF_MIPS_MACH_OCTEON2, "octeon2"),
  ENUM_ENT(EF_MIPS_MACH_OCTEON3, "octeon3"),
  ENUM_ENT(EF_MIPS_MACH_5400, "5400"),
  ENUM_ENT(EF_MIPS_MACH_5900, "5900"),
  ENUM_ENT(EF_MIPS_MACH_5500, "5500"),
  ENUM_ENT(EF_MIPS_MACH_9000, "9000"),
  ENUM_ENT(EF_MIPS_MACH_LS2E, "loongson-2e"),
  ENUM_ENT(EF_MIPS_MACH_LS2F, "loongson-2f"),
  ENUM_ENT(EF_MIPS_MACH_LS3A, "loongson-3a"),
  ENUM_ENT(EF_MIPS_MICROMIPS, "micromips"),
  ENUM_ENT(EF_MIPS_ARCH_ASE_M16, "mips16"),
  ENUM_ENT(EF_MIPS_ARCH_ASE_MDMX, "mdmx"),
  ENUM_ENT(EF_MIPS_ARCH_1, "mips1"),
  ENUM_ENT(EF_MIPS_ARCH_2, "mips2"),
  ENUM_ENT(EF_MIPS_ARCH_3, "mips3"),
  ENUM_ENT(EF_MIPS_ARCH_4, "mips4"),
  ENUM_ENT(EF_MIPS_ARCH_5, "mips5"),
  ENUM_ENT(EF_MIPS_ARCH_32, "mips32"),
  ENUM_ENT(EF_MIPS_ARCH_64, "mips64"),
  ENUM_ENT(EF_MIPS_ARCH_32R2, "mips32r2"),
  ENUM_ENT(EF_MIPS_ARCH_64R2, "mips64r2"),
  ENUM_ENT(EF_MIPS_ARCH_32R6, "mips32r6"),
  ENUM_ENT(EF_MIPS_ARCH_64R6, "mips64r6")
};

static const EnumEntry<unsigned> ElfHeaderAMDGPUFlags[] = {
  LLVM_READOBJ_ENUM_ENT(ELF, EF_AMDGPU_MACH_NONE),
  LLVM_READOBJ_ENUM_ENT(ELF, EF_AMDGPU_MACH_R600_R600),
  LLVM_READOBJ_ENUM_ENT(ELF, EF_AMDGPU_MACH_R600_R630),
  LLVM_READOBJ_ENUM_ENT(ELF, EF_AMDGPU_MACH_R600_RS880),
  LLVM_READOBJ_ENUM_ENT(ELF, EF_AMDGPU_MACH_R600_RV670),
  LLVM_READOBJ_ENUM_ENT(ELF, EF_AMDGPU_MACH_R600_RV710),
  LLVM_READOBJ_ENUM_ENT(ELF, EF_AMDGPU_MACH_R600_RV730),
  LLVM_READOBJ_ENUM_ENT(ELF, EF_AMDGPU_MACH_R600_RV770),
  LLVM_READOBJ_ENUM_ENT(ELF, EF_AMDGPU_MACH_R600_CEDAR),
  LLVM_READOBJ_ENUM_ENT(ELF, EF_AMDGPU_MACH_R600_CYPRESS),
  LLVM_READOBJ_ENUM_ENT(ELF, EF_AMDGPU_MACH_R600_JUNIPER),
  LLVM_READOBJ_ENUM_ENT(ELF, EF_AMDGPU_MACH_R600_REDWOOD),
  LLVM_READOBJ_ENUM_ENT(ELF, EF_AMDGPU_MACH_R600_SUMO),
  LLVM_READOBJ_ENUM_ENT(ELF, EF_AMDGPU_MACH_R600_BARTS),
  LLVM_READOBJ_ENUM_ENT(ELF, EF_AMDGPU_MACH_R600_CAICOS),
  LLVM_READOBJ_ENUM_ENT(ELF, EF_AMDGPU_MACH_R600_CAYMAN),
  LLVM_READOBJ_ENUM_ENT(ELF, EF_AMDGPU_MACH_R600_TURKS),
  LLVM_READOBJ_ENUM_ENT(ELF, EF_AMDGPU_MACH_AMDGCN_GFX600),
  LLVM_READOBJ_ENUM_ENT(ELF, EF_AMDGPU_MACH_AMDGCN_GFX601),
  LLVM_READOBJ_ENUM_ENT(ELF, EF_AMDGPU_MACH_AMDGCN_GFX700),
  LLVM_READOBJ_ENUM_ENT(ELF, EF_AMDGPU_MACH_AMDGCN_GFX701),
  LLVM_READOBJ_ENUM_ENT(ELF, EF_AMDGPU_MACH_AMDGCN_GFX702),
  LLVM_READOBJ_ENUM_ENT(ELF, EF_AMDGPU_MACH_AMDGCN_GFX703),
  LLVM_READOBJ_ENUM_ENT(ELF, EF_AMDGPU_MACH_AMDGCN_GFX704),
  LLVM_READOBJ_ENUM_ENT(ELF, EF_AMDGPU_MACH_AMDGCN_GFX801),
  LLVM_READOBJ_ENUM_ENT(ELF, EF_AMDGPU_MACH_AMDGCN_GFX802),
  LLVM_READOBJ_ENUM_ENT(ELF, EF_AMDGPU_MACH_AMDGCN_GFX803),
  LLVM_READOBJ_ENUM_ENT(ELF, EF_AMDGPU_MACH_AMDGCN_GFX810),
  LLVM_READOBJ_ENUM_ENT(ELF, EF_AMDGPU_MACH_AMDGCN_GFX900),
  LLVM_READOBJ_ENUM_ENT(ELF, EF_AMDGPU_MACH_AMDGCN_GFX902),
  LLVM_READOBJ_ENUM_ENT(ELF, EF_AMDGPU_MACH_AMDGCN_GFX904),
  LLVM_READOBJ_ENUM_ENT(ELF, EF_AMDGPU_MACH_AMDGCN_GFX906),
  LLVM_READOBJ_ENUM_ENT(ELF, EF_AMDGPU_MACH_AMDGCN_GFX908),
  LLVM_READOBJ_ENUM_ENT(ELF, EF_AMDGPU_MACH_AMDGCN_GFX909),
  LLVM_READOBJ_ENUM_ENT(ELF, EF_AMDGPU_MACH_AMDGCN_GFX1010),
  LLVM_READOBJ_ENUM_ENT(ELF, EF_AMDGPU_MACH_AMDGCN_GFX1011),
  LLVM_READOBJ_ENUM_ENT(ELF, EF_AMDGPU_MACH_AMDGCN_GFX1012),
  LLVM_READOBJ_ENUM_ENT(ELF, EF_AMDGPU_XNACK),
  LLVM_READOBJ_ENUM_ENT(ELF, EF_AMDGPU_SRAM_ECC)
};

static const EnumEntry<unsigned> ElfHeaderRISCVFlags[] = {
  ENUM_ENT(EF_RISCV_RVC, "RVC"),
  ENUM_ENT(EF_RISCV_FLOAT_ABI_SINGLE, "single-float ABI"),
  ENUM_ENT(EF_RISCV_FLOAT_ABI_DOUBLE, "double-float ABI"),
  ENUM_ENT(EF_RISCV_FLOAT_ABI_QUAD, "quad-float ABI"),
  ENUM_ENT(EF_RISCV_RVE, "RVE")
};

static const EnumEntry<unsigned> ElfSymOtherFlags[] = {
  LLVM_READOBJ_ENUM_ENT(ELF, STV_INTERNAL),
  LLVM_READOBJ_ENUM_ENT(ELF, STV_HIDDEN),
  LLVM_READOBJ_ENUM_ENT(ELF, STV_PROTECTED)
};

static const EnumEntry<unsigned> ElfMipsSymOtherFlags[] = {
  LLVM_READOBJ_ENUM_ENT(ELF, STO_MIPS_OPTIONAL),
  LLVM_READOBJ_ENUM_ENT(ELF, STO_MIPS_PLT),
  LLVM_READOBJ_ENUM_ENT(ELF, STO_MIPS_PIC),
  LLVM_READOBJ_ENUM_ENT(ELF, STO_MIPS_MICROMIPS)
};

static const EnumEntry<unsigned> ElfMips16SymOtherFlags[] = {
  LLVM_READOBJ_ENUM_ENT(ELF, STO_MIPS_OPTIONAL),
  LLVM_READOBJ_ENUM_ENT(ELF, STO_MIPS_PLT),
  LLVM_READOBJ_ENUM_ENT(ELF, STO_MIPS_MIPS16)
};

static const char *getElfMipsOptionsOdkType(unsigned Odk) {
  switch (Odk) {
  LLVM_READOBJ_ENUM_CASE(ELF, ODK_NULL);
  LLVM_READOBJ_ENUM_CASE(ELF, ODK_REGINFO);
  LLVM_READOBJ_ENUM_CASE(ELF, ODK_EXCEPTIONS);
  LLVM_READOBJ_ENUM_CASE(ELF, ODK_PAD);
  LLVM_READOBJ_ENUM_CASE(ELF, ODK_HWPATCH);
  LLVM_READOBJ_ENUM_CASE(ELF, ODK_FILL);
  LLVM_READOBJ_ENUM_CASE(ELF, ODK_TAGS);
  LLVM_READOBJ_ENUM_CASE(ELF, ODK_HWAND);
  LLVM_READOBJ_ENUM_CASE(ELF, ODK_HWOR);
  LLVM_READOBJ_ENUM_CASE(ELF, ODK_GP_GROUP);
  LLVM_READOBJ_ENUM_CASE(ELF, ODK_IDENT);
  LLVM_READOBJ_ENUM_CASE(ELF, ODK_PAGESIZE);
  default:
    return "Unknown";
  }
}

template <typename ELFT>
void ELFDumper<ELFT>::loadDynamicTable(const ELFFile<ELFT> *Obj) {
  // Try to locate the PT_DYNAMIC header.
  const Elf_Phdr *DynamicPhdr = nullptr;
  for (const Elf_Phdr &Phdr : unwrapOrError(Obj->program_headers())) {
    if (Phdr.p_type != ELF::PT_DYNAMIC)
      continue;
    DynamicPhdr = &Phdr;
    break;
  }

  // Try to locate the .dynamic section in the sections header table.
  const Elf_Shdr *DynamicSec = nullptr;
  for (const Elf_Shdr &Sec : unwrapOrError(Obj->sections())) {
    if (Sec.sh_type != ELF::SHT_DYNAMIC)
      continue;
    DynamicSec = &Sec;
    break;
  }

  // Information in the section header has priority over the information
  // in a PT_DYNAMIC header.
  // Ignore sh_entsize and use the expected value for entry size explicitly.
  // This allows us to dump the dynamic sections with a broken sh_entsize
  // field.
  if (DynamicSec) {
    DynamicTable = checkDRI({ObjF->getELFFile()->base() + DynamicSec->sh_offset,
                             DynamicSec->sh_size, sizeof(Elf_Dyn)});
    parseDynamicTable();
  }

  // If we have a PT_DYNAMIC header, we will either check the found dynamic
  // section or take the dynamic table data directly from the header.
  if (!DynamicPhdr)
    return;

  if (DynamicPhdr->p_offset + DynamicPhdr->p_filesz >
      ObjF->getMemoryBufferRef().getBufferSize())
    reportError(
        "PT_DYNAMIC segment offset + size exceeds the size of the file");

  if (!DynamicSec) {
    DynamicTable = createDRIFrom(DynamicPhdr, sizeof(Elf_Dyn));
    parseDynamicTable();
    return;
  }

  StringRef Name = unwrapOrError(Obj->getSectionName(DynamicSec));
  if (DynamicSec->sh_addr + DynamicSec->sh_size >
          DynamicPhdr->p_vaddr + DynamicPhdr->p_memsz ||
      DynamicSec->sh_addr < DynamicPhdr->p_vaddr)
    reportWarning("The SHT_DYNAMIC section '" + Name +
                  "' is not contained within the "
                  "PT_DYNAMIC segment");

  if (DynamicSec->sh_addr != DynamicPhdr->p_vaddr)
    reportWarning("The SHT_DYNAMIC section '" + Name +
                  "' is not at the start of "
                  "PT_DYNAMIC segment");
}

template <typename ELFT>
ELFDumper<ELFT>::ELFDumper(const object::ELFObjectFile<ELFT> *ObjF,
    ScopedPrinter &Writer)
    : ObjDumper(Writer), ObjF(ObjF) {
  const ELFFile<ELFT> *Obj = ObjF->getELFFile();

  for (const Elf_Shdr &Sec : unwrapOrError(Obj->sections())) {
    switch (Sec.sh_type) {
    case ELF::SHT_SYMTAB:
      if (!DotSymtabSec)
        DotSymtabSec = &Sec;
      break;
    case ELF::SHT_DYNSYM:
      if (!DynSymRegion.Size) {
        DynSymRegion = createDRIFrom(&Sec);
        // This is only used (if Elf_Shdr present)for naming section in GNU
        // style
        DynSymtabName = unwrapOrError(Obj->getSectionName(&Sec));

        if (Expected<StringRef> E = Obj->getStringTableForSymtab(Sec))
          DynamicStringTable = *E;
        else
          warn(E.takeError());
      }
      break;
    case ELF::SHT_SYMTAB_SHNDX:
      ShndxTable = unwrapOrError(Obj->getSHNDXTable(Sec));
      break;
    case ELF::SHT_GNU_versym:
      if (!SymbolVersionSection)
        SymbolVersionSection = &Sec;
      break;
    case ELF::SHT_GNU_verdef:
      if (!SymbolVersionDefSection)
        SymbolVersionDefSection = &Sec;
      break;
    case ELF::SHT_GNU_verneed:
      if (!SymbolVersionNeedSection)
        SymbolVersionNeedSection = &Sec;
      break;
    case ELF::SHT_LLVM_CALL_GRAPH_PROFILE:
      if (!DotCGProfileSec)
        DotCGProfileSec = &Sec;
      break;
    case ELF::SHT_LLVM_ADDRSIG:
      if (!DotAddrsigSec)
        DotAddrsigSec = &Sec;
      break;
    }
  }

  loadDynamicTable(Obj);

  if (opts::Output == opts::GNU)
    ELFDumperStyle.reset(new GNUStyle<ELFT>(Writer, this));
  else
    ELFDumperStyle.reset(new LLVMStyle<ELFT>(Writer, this));
}

static const char *getTypeString(unsigned Arch, uint64_t Type) {
#define DYNAMIC_TAG(n, v)
  switch (Arch) {

  case EM_AARCH64:
    switch (Type) {
#define AARCH64_DYNAMIC_TAG(name, value)                                       \
    case DT_##name:                                                            \
      return #name;
#include "llvm/BinaryFormat/DynamicTags.def"
#undef AARCH64_DYNAMIC_TAG
    }
    break;

  case EM_HEXAGON:
    switch (Type) {
#define HEXAGON_DYNAMIC_TAG(name, value)                                       \
  case DT_##name:                                                              \
    return #name;
#include "llvm/BinaryFormat/DynamicTags.def"
#undef HEXAGON_DYNAMIC_TAG
    }
    break;

  case EM_MIPS:
    switch (Type) {
#define MIPS_DYNAMIC_TAG(name, value)                                          \
  case DT_##name:                                                              \
    return #name;
#include "llvm/BinaryFormat/DynamicTags.def"
#undef MIPS_DYNAMIC_TAG
    }
    break;

  case EM_PPC64:
    switch (Type) {
#define PPC64_DYNAMIC_TAG(name, value)                                         \
  case DT_##name:                                                              \
    return #name;
#include "llvm/BinaryFormat/DynamicTags.def"
#undef PPC64_DYNAMIC_TAG
    }
    break;
  }
#undef DYNAMIC_TAG
  switch (Type) {
// Now handle all dynamic tags except the architecture specific ones
#define AARCH64_DYNAMIC_TAG(name, value)
#define MIPS_DYNAMIC_TAG(name, value)
#define HEXAGON_DYNAMIC_TAG(name, value)
#define PPC64_DYNAMIC_TAG(name, value)
// Also ignore marker tags such as DT_HIOS (maps to DT_VERNEEDNUM), etc.
#define DYNAMIC_TAG_MARKER(name, value)
#define DYNAMIC_TAG(name, value)                                               \
  case DT_##name:                                                              \
    return #name;
#include "llvm/BinaryFormat/DynamicTags.def"
#undef DYNAMIC_TAG
#undef AARCH64_DYNAMIC_TAG
#undef MIPS_DYNAMIC_TAG
#undef HEXAGON_DYNAMIC_TAG
#undef PPC64_DYNAMIC_TAG
#undef DYNAMIC_TAG_MARKER
  default:
    return "unknown";
  }
}

template <typename ELFT> void ELFDumper<ELFT>::parseDynamicTable() {
  auto toMappedAddr = [&](uint64_t Tag, uint64_t VAddr) -> const uint8_t * {
    auto MappedAddrOrError = ObjF->getELFFile()->toMappedAddr(VAddr);
    if (!MappedAddrOrError) {
      reportWarning("Unable to parse DT_" +
                    Twine(getTypeString(
                        ObjF->getELFFile()->getHeader()->e_machine, Tag)) +
                    ": " + llvm::toString(MappedAddrOrError.takeError()));
      return nullptr;
    }
    return MappedAddrOrError.get();
  };

  uint64_t SONameOffset = 0;
  const char *StringTableBegin = nullptr;
  uint64_t StringTableSize = 0;
  for (const Elf_Dyn &Dyn : dynamic_table()) {
    switch (Dyn.d_tag) {
    case ELF::DT_HASH:
      HashTable = reinterpret_cast<const Elf_Hash *>(
          toMappedAddr(Dyn.getTag(), Dyn.getPtr()));
      break;
    case ELF::DT_GNU_HASH:
      GnuHashTable = reinterpret_cast<const Elf_GnuHash *>(
          toMappedAddr(Dyn.getTag(), Dyn.getPtr()));
      break;
    case ELF::DT_STRTAB:
      StringTableBegin = reinterpret_cast<const char *>(
          toMappedAddr(Dyn.getTag(), Dyn.getPtr()));
      break;
    case ELF::DT_STRSZ:
      StringTableSize = Dyn.getVal();
      break;
    case ELF::DT_SYMTAB:
      DynSymRegion.Addr = toMappedAddr(Dyn.getTag(), Dyn.getPtr());
      DynSymRegion.EntSize = sizeof(Elf_Sym);
      break;
    case ELF::DT_RELA:
      DynRelaRegion.Addr = toMappedAddr(Dyn.getTag(), Dyn.getPtr());
      break;
    case ELF::DT_RELASZ:
      DynRelaRegion.Size = Dyn.getVal();
      break;
    case ELF::DT_RELAENT:
      DynRelaRegion.EntSize = Dyn.getVal();
      break;
    case ELF::DT_SONAME:
      SONameOffset = Dyn.getVal();
      break;
    case ELF::DT_REL:
      DynRelRegion.Addr = toMappedAddr(Dyn.getTag(), Dyn.getPtr());
      break;
    case ELF::DT_RELSZ:
      DynRelRegion.Size = Dyn.getVal();
      break;
    case ELF::DT_RELENT:
      DynRelRegion.EntSize = Dyn.getVal();
      break;
    case ELF::DT_RELR:
    case ELF::DT_ANDROID_RELR:
      DynRelrRegion.Addr = toMappedAddr(Dyn.getTag(), Dyn.getPtr());
      break;
    case ELF::DT_RELRSZ:
    case ELF::DT_ANDROID_RELRSZ:
      DynRelrRegion.Size = Dyn.getVal();
      break;
    case ELF::DT_RELRENT:
    case ELF::DT_ANDROID_RELRENT:
      DynRelrRegion.EntSize = Dyn.getVal();
      break;
    case ELF::DT_PLTREL:
      if (Dyn.getVal() == DT_REL)
        DynPLTRelRegion.EntSize = sizeof(Elf_Rel);
      else if (Dyn.getVal() == DT_RELA)
        DynPLTRelRegion.EntSize = sizeof(Elf_Rela);
      else
        reportError(Twine("unknown DT_PLTREL value of ") +
                    Twine((uint64_t)Dyn.getVal()));
      break;
    case ELF::DT_JMPREL:
      DynPLTRelRegion.Addr = toMappedAddr(Dyn.getTag(), Dyn.getPtr());
      break;
    case ELF::DT_PLTRELSZ:
      DynPLTRelRegion.Size = Dyn.getVal();
      break;
    }
  }
  if (StringTableBegin)
    DynamicStringTable = StringRef(StringTableBegin, StringTableSize);
  if (SONameOffset && SONameOffset < DynamicStringTable.size())
    SOName = DynamicStringTable.data() + SONameOffset;
}

template <typename ELFT>
typename ELFDumper<ELFT>::Elf_Rel_Range ELFDumper<ELFT>::dyn_rels() const {
  return DynRelRegion.getAsArrayRef<Elf_Rel>();
}

template <typename ELFT>
typename ELFDumper<ELFT>::Elf_Rela_Range ELFDumper<ELFT>::dyn_relas() const {
  return DynRelaRegion.getAsArrayRef<Elf_Rela>();
}

template <typename ELFT>
typename ELFDumper<ELFT>::Elf_Relr_Range ELFDumper<ELFT>::dyn_relrs() const {
  return DynRelrRegion.getAsArrayRef<Elf_Relr>();
}

template <class ELFT> void ELFDumper<ELFT>::printFileHeaders() {
  ELFDumperStyle->printFileHeaders(ObjF->getELFFile());
}

template <class ELFT> void ELFDumper<ELFT>::printSectionHeaders() {
  ELFDumperStyle->printSectionHeaders(ObjF->getELFFile());
}

template <class ELFT> void ELFDumper<ELFT>::printRelocations() {
  ELFDumperStyle->printRelocations(ObjF->getELFFile());
}

template <class ELFT>
void ELFDumper<ELFT>::printProgramHeaders(
    bool PrintProgramHeaders, cl::boolOrDefault PrintSectionMapping) {
  ELFDumperStyle->printProgramHeaders(ObjF->getELFFile(), PrintProgramHeaders,
                                      PrintSectionMapping);
}

template <typename ELFT> void ELFDumper<ELFT>::printVersionInfo() {
  // Dump version symbol section.
  ELFDumperStyle->printVersionSymbolSection(ObjF->getELFFile(),
                                            SymbolVersionSection);

  // Dump version definition section.
  ELFDumperStyle->printVersionDefinitionSection(ObjF->getELFFile(),
                                                SymbolVersionDefSection);

  // Dump version dependency section.
  ELFDumperStyle->printVersionDependencySection(ObjF->getELFFile(),
                                                SymbolVersionNeedSection);
}

template <class ELFT> void ELFDumper<ELFT>::printDynamicRelocations() {
  ELFDumperStyle->printDynamicRelocations(ObjF->getELFFile());
}

template <class ELFT>
void ELFDumper<ELFT>::printSymbols(bool PrintSymbols,
                                   bool PrintDynamicSymbols) {
  ELFDumperStyle->printSymbols(ObjF->getELFFile(), PrintSymbols,
                               PrintDynamicSymbols);
}

template <class ELFT> void ELFDumper<ELFT>::printHashSymbols() {
  ELFDumperStyle->printHashSymbols(ObjF->getELFFile());
}

template <class ELFT> void ELFDumper<ELFT>::printHashHistogram() {
  ELFDumperStyle->printHashHistogram(ObjF->getELFFile());
}

template <class ELFT> void ELFDumper<ELFT>::printCGProfile() {
  ELFDumperStyle->printCGProfile(ObjF->getELFFile());
}

template <class ELFT> void ELFDumper<ELFT>::printNotes() {
  ELFDumperStyle->printNotes(ObjF->getELFFile());
}

template <class ELFT> void ELFDumper<ELFT>::printELFLinkerOptions() {
  ELFDumperStyle->printELFLinkerOptions(ObjF->getELFFile());
}

#define LLVM_READOBJ_DT_FLAG_ENT(prefix, enum)                                 \
  { #enum, prefix##_##enum }

static const EnumEntry<unsigned> ElfDynamicDTFlags[] = {
  LLVM_READOBJ_DT_FLAG_ENT(DF, ORIGIN),
  LLVM_READOBJ_DT_FLAG_ENT(DF, SYMBOLIC),
  LLVM_READOBJ_DT_FLAG_ENT(DF, TEXTREL),
  LLVM_READOBJ_DT_FLAG_ENT(DF, BIND_NOW),
  LLVM_READOBJ_DT_FLAG_ENT(DF, STATIC_TLS)
};

static const EnumEntry<unsigned> ElfDynamicDTFlags1[] = {
  LLVM_READOBJ_DT_FLAG_ENT(DF_1, NOW),
  LLVM_READOBJ_DT_FLAG_ENT(DF_1, GLOBAL),
  LLVM_READOBJ_DT_FLAG_ENT(DF_1, GROUP),
  LLVM_READOBJ_DT_FLAG_ENT(DF_1, NODELETE),
  LLVM_READOBJ_DT_FLAG_ENT(DF_1, LOADFLTR),
  LLVM_READOBJ_DT_FLAG_ENT(DF_1, INITFIRST),
  LLVM_READOBJ_DT_FLAG_ENT(DF_1, NOOPEN),
  LLVM_READOBJ_DT_FLAG_ENT(DF_1, ORIGIN),
  LLVM_READOBJ_DT_FLAG_ENT(DF_1, DIRECT),
  LLVM_READOBJ_DT_FLAG_ENT(DF_1, TRANS),
  LLVM_READOBJ_DT_FLAG_ENT(DF_1, INTERPOSE),
  LLVM_READOBJ_DT_FLAG_ENT(DF_1, NODEFLIB),
  LLVM_READOBJ_DT_FLAG_ENT(DF_1, NODUMP),
  LLVM_READOBJ_DT_FLAG_ENT(DF_1, CONFALT),
  LLVM_READOBJ_DT_FLAG_ENT(DF_1, ENDFILTEE),
  LLVM_READOBJ_DT_FLAG_ENT(DF_1, DISPRELDNE),
  LLVM_READOBJ_DT_FLAG_ENT(DF_1, DISPRELPND),
  LLVM_READOBJ_DT_FLAG_ENT(DF_1, NODIRECT),
  LLVM_READOBJ_DT_FLAG_ENT(DF_1, IGNMULDEF),
  LLVM_READOBJ_DT_FLAG_ENT(DF_1, NOKSYMS),
  LLVM_READOBJ_DT_FLAG_ENT(DF_1, NOHDR),
  LLVM_READOBJ_DT_FLAG_ENT(DF_1, EDITED),
  LLVM_READOBJ_DT_FLAG_ENT(DF_1, NORELOC),
  LLVM_READOBJ_DT_FLAG_ENT(DF_1, SYMINTPOSE),
  LLVM_READOBJ_DT_FLAG_ENT(DF_1, GLOBAUDIT),
  LLVM_READOBJ_DT_FLAG_ENT(DF_1, SINGLETON)
};

static const EnumEntry<unsigned> ElfDynamicDTMipsFlags[] = {
  LLVM_READOBJ_DT_FLAG_ENT(RHF, NONE),
  LLVM_READOBJ_DT_FLAG_ENT(RHF, QUICKSTART),
  LLVM_READOBJ_DT_FLAG_ENT(RHF, NOTPOT),
  LLVM_READOBJ_DT_FLAG_ENT(RHS, NO_LIBRARY_REPLACEMENT),
  LLVM_READOBJ_DT_FLAG_ENT(RHF, NO_MOVE),
  LLVM_READOBJ_DT_FLAG_ENT(RHF, SGI_ONLY),
  LLVM_READOBJ_DT_FLAG_ENT(RHF, GUARANTEE_INIT),
  LLVM_READOBJ_DT_FLAG_ENT(RHF, DELTA_C_PLUS_PLUS),
  LLVM_READOBJ_DT_FLAG_ENT(RHF, GUARANTEE_START_INIT),
  LLVM_READOBJ_DT_FLAG_ENT(RHF, PIXIE),
  LLVM_READOBJ_DT_FLAG_ENT(RHF, DEFAULT_DELAY_LOAD),
  LLVM_READOBJ_DT_FLAG_ENT(RHF, REQUICKSTART),
  LLVM_READOBJ_DT_FLAG_ENT(RHF, REQUICKSTARTED),
  LLVM_READOBJ_DT_FLAG_ENT(RHF, CORD),
  LLVM_READOBJ_DT_FLAG_ENT(RHF, NO_UNRES_UNDEF),
  LLVM_READOBJ_DT_FLAG_ENT(RHF, RLD_ORDER_SAFE)
};

#undef LLVM_READOBJ_DT_FLAG_ENT

template <typename T, typename TFlag>
void printFlags(T Value, ArrayRef<EnumEntry<TFlag>> Flags, raw_ostream &OS) {
  using FlagEntry = EnumEntry<TFlag>;
  using FlagVector = SmallVector<FlagEntry, 10>;
  FlagVector SetFlags;

  for (const auto &Flag : Flags) {
    if (Flag.Value == 0)
      continue;

    if ((Value & Flag.Value) == Flag.Value)
      SetFlags.push_back(Flag);
  }

  for (const auto &Flag : SetFlags) {
    OS << Flag.Name << " ";
  }
}

template <class ELFT>
void ELFDumper<ELFT>::printDynamicEntry(raw_ostream &OS, uint64_t Type,
                                        uint64_t Value) const {
  const char *ConvChar =
      (opts::Output == opts::GNU) ? "0x%" PRIx64 : "0x%" PRIX64;

  // Handle custom printing of architecture specific tags
  switch (ObjF->getELFFile()->getHeader()->e_machine) {
  case EM_AARCH64:
    switch (Type) {
    case DT_AARCH64_BTI_PLT:
    case DT_AARCH64_PAC_PLT:
      OS << Value;
      return;
    default:
      break;
    }
    break;
  case EM_HEXAGON:
    switch (Type) {
    case DT_HEXAGON_VER:
      OS << Value;
      return;
    case DT_HEXAGON_SYMSZ:
    case DT_HEXAGON_PLT:
      OS << format(ConvChar, Value);
      return;
    default:
      break;
    }
    break;
  case EM_MIPS:
    switch (Type) {
    case DT_MIPS_RLD_VERSION:
    case DT_MIPS_LOCAL_GOTNO:
    case DT_MIPS_SYMTABNO:
    case DT_MIPS_UNREFEXTNO:
      OS << Value;
      return;
    case DT_MIPS_TIME_STAMP:
    case DT_MIPS_ICHECKSUM:
    case DT_MIPS_IVERSION:
    case DT_MIPS_BASE_ADDRESS:
    case DT_MIPS_MSYM:
    case DT_MIPS_CONFLICT:
    case DT_MIPS_LIBLIST:
    case DT_MIPS_CONFLICTNO:
    case DT_MIPS_LIBLISTNO:
    case DT_MIPS_GOTSYM:
    case DT_MIPS_HIPAGENO:
    case DT_MIPS_RLD_MAP:
    case DT_MIPS_DELTA_CLASS:
    case DT_MIPS_DELTA_CLASS_NO:
    case DT_MIPS_DELTA_INSTANCE:
    case DT_MIPS_DELTA_RELOC:
    case DT_MIPS_DELTA_RELOC_NO:
    case DT_MIPS_DELTA_SYM:
    case DT_MIPS_DELTA_SYM_NO:
    case DT_MIPS_DELTA_CLASSSYM:
    case DT_MIPS_DELTA_CLASSSYM_NO:
    case DT_MIPS_CXX_FLAGS:
    case DT_MIPS_PIXIE_INIT:
    case DT_MIPS_SYMBOL_LIB:
    case DT_MIPS_LOCALPAGE_GOTIDX:
    case DT_MIPS_LOCAL_GOTIDX:
    case DT_MIPS_HIDDEN_GOTIDX:
    case DT_MIPS_PROTECTED_GOTIDX:
    case DT_MIPS_OPTIONS:
    case DT_MIPS_INTERFACE:
    case DT_MIPS_DYNSTR_ALIGN:
    case DT_MIPS_INTERFACE_SIZE:
    case DT_MIPS_RLD_TEXT_RESOLVE_ADDR:
    case DT_MIPS_PERF_SUFFIX:
    case DT_MIPS_COMPACT_SIZE:
    case DT_MIPS_GP_VALUE:
    case DT_MIPS_AUX_DYNAMIC:
    case DT_MIPS_PLTGOT:
    case DT_MIPS_RWPLT:
    case DT_MIPS_RLD_MAP_REL:
      OS << format(ConvChar, Value);
      return;
    case DT_MIPS_FLAGS:
      printFlags(Value, makeArrayRef(ElfDynamicDTMipsFlags), OS);
      return;
    default:
      break;
    }
    break;
  default:
    break;
  }

  switch (Type) {
  case DT_PLTREL:
    if (Value == DT_REL) {
      OS << "REL";
      break;
    } else if (Value == DT_RELA) {
      OS << "RELA";
      break;
    }
    LLVM_FALLTHROUGH;
  case DT_PLTGOT:
  case DT_HASH:
  case DT_STRTAB:
  case DT_SYMTAB:
  case DT_RELA:
  case DT_INIT:
  case DT_FINI:
  case DT_REL:
  case DT_JMPREL:
  case DT_INIT_ARRAY:
  case DT_FINI_ARRAY:
  case DT_PREINIT_ARRAY:
  case DT_DEBUG:
  case DT_VERDEF:
  case DT_VERNEED:
  case DT_VERSYM:
  case DT_GNU_HASH:
  case DT_NULL:
    OS << format(ConvChar, Value);
    break;
  case DT_RELACOUNT:
  case DT_RELCOUNT:
  case DT_VERDEFNUM:
  case DT_VERNEEDNUM:
    OS << Value;
    break;
  case DT_PLTRELSZ:
  case DT_RELASZ:
  case DT_RELAENT:
  case DT_STRSZ:
  case DT_SYMENT:
  case DT_RELSZ:
  case DT_RELENT:
  case DT_INIT_ARRAYSZ:
  case DT_FINI_ARRAYSZ:
  case DT_PREINIT_ARRAYSZ:
  case DT_ANDROID_RELSZ:
  case DT_ANDROID_RELASZ:
    OS << Value << " (bytes)";
    break;
  case DT_NEEDED:
  case DT_SONAME:
  case DT_AUXILIARY:
  case DT_USED:
  case DT_FILTER:
  case DT_RPATH:
  case DT_RUNPATH: {
    const std::map<uint64_t, const char*> TagNames = {
      {DT_NEEDED,    "Shared library"},
      {DT_SONAME,    "Library soname"},
      {DT_AUXILIARY, "Auxiliary library"},
      {DT_USED,      "Not needed object"},
      {DT_FILTER,    "Filter library"},
      {DT_RPATH,     "Library rpath"},
      {DT_RUNPATH,   "Library runpath"},
    };
    OS << TagNames.at(Type) << ": ";
    if (DynamicStringTable.empty())
      OS << "<String table is empty or was not found> ";
    else if (Value < DynamicStringTable.size())
      OS << "[" << StringRef(DynamicStringTable.data() + Value) << "]";
    else
      OS << "<Invalid offset 0x" << utohexstr(Value) << ">";
    break;
  }
  case DT_FLAGS:
    printFlags(Value, makeArrayRef(ElfDynamicDTFlags), OS);
    break;
  case DT_FLAGS_1:
    printFlags(Value, makeArrayRef(ElfDynamicDTFlags1), OS);
    break;
  default:
    OS << format(ConvChar, Value);
    break;
  }
}

template <class ELFT> void ELFDumper<ELFT>::printUnwindInfo() {
  DwarfCFIEH::PrinterContext<ELFT> Ctx(W, ObjF);
  Ctx.printUnwindInformation();
}

namespace {

template <> void ELFDumper<ELF32LE>::printUnwindInfo() {
  const ELFFile<ELF32LE> *Obj = ObjF->getELFFile();
  const unsigned Machine = Obj->getHeader()->e_machine;
  if (Machine == EM_ARM) {
    ARM::EHABI::PrinterContext<ELF32LE> Ctx(W, Obj, DotSymtabSec);
    Ctx.PrintUnwindInformation();
  }
  DwarfCFIEH::PrinterContext<ELF32LE> Ctx(W, ObjF);
  Ctx.printUnwindInformation();
}

} // end anonymous namespace

template <class ELFT> void ELFDumper<ELFT>::printDynamicTable() {
  ELFDumperStyle->printDynamic(ObjF->getELFFile());
}

template <class ELFT> void ELFDumper<ELFT>::printNeededLibraries() {
  ListScope D(W, "NeededLibraries");

  using LibsTy = std::vector<StringRef>;
  LibsTy Libs;

  for (const auto &Entry : dynamic_table())
    if (Entry.d_tag == ELF::DT_NEEDED) {
      uint64_t Value = Entry.d_un.d_val;
      if (Value < DynamicStringTable.size())
        Libs.push_back(StringRef(DynamicStringTable.data() + Value));
      else
        Libs.push_back("<Library name index out of range>");
    }

  llvm::stable_sort(Libs);

  for (const auto &L : Libs)
    W.startLine() << L << "\n";
}

template <typename ELFT> void ELFDumper<ELFT>::printHashTable() {
  DictScope D(W, "HashTable");
  if (!HashTable)
    return;
  W.printNumber("Num Buckets", HashTable->nbucket);
  W.printNumber("Num Chains", HashTable->nchain);
  W.printList("Buckets", HashTable->buckets());
  W.printList("Chains", HashTable->chains());
}

template <typename ELFT> void ELFDumper<ELFT>::printGnuHashTable() {
  DictScope D(W, "GnuHashTable");
  if (!GnuHashTable)
    return;
  W.printNumber("Num Buckets", GnuHashTable->nbuckets);
  W.printNumber("First Hashed Symbol Index", GnuHashTable->symndx);
  W.printNumber("Num Mask Words", GnuHashTable->maskwords);
  W.printNumber("Shift Count", GnuHashTable->shift2);
  W.printHexList("Bloom Filter", GnuHashTable->filter());
  W.printList("Buckets", GnuHashTable->buckets());
  Elf_Sym_Range Syms = dynamic_symbols();
  unsigned NumSyms = std::distance(Syms.begin(), Syms.end());
  if (!NumSyms)
    reportError("No dynamic symbol section");
  W.printHexList("Values", GnuHashTable->values(NumSyms));
}

template <typename ELFT> void ELFDumper<ELFT>::printLoadName() {
  W.printString("LoadName", SOName);
}

template <class ELFT> void ELFDumper<ELFT>::printAttributes() {
  W.startLine() << "Attributes not implemented.\n";
}

namespace {

template <> void ELFDumper<ELF32LE>::printAttributes() {
  const ELFFile<ELF32LE> *Obj = ObjF->getELFFile();
  if (Obj->getHeader()->e_machine != EM_ARM) {
    W.startLine() << "Attributes not implemented.\n";
    return;
  }

  DictScope BA(W, "BuildAttributes");
  for (const ELFO::Elf_Shdr &Sec : unwrapOrError(Obj->sections())) {
    if (Sec.sh_type != ELF::SHT_ARM_ATTRIBUTES)
      continue;

    ArrayRef<uint8_t> Contents = unwrapOrError(Obj->getSectionContents(&Sec));
    if (Contents[0] != ARMBuildAttrs::Format_Version) {
      errs() << "unrecognised FormatVersion: 0x"
             << Twine::utohexstr(Contents[0]) << '\n';
      continue;
    }

    W.printHex("FormatVersion", Contents[0]);
    if (Contents.size() == 1)
      continue;

    ARMAttributeParser(&W).Parse(Contents, true);
  }
}

template <class ELFT> class MipsGOTParser {
public:
  TYPEDEF_ELF_TYPES(ELFT)
  using Entry = typename ELFO::Elf_Addr;
  using Entries = ArrayRef<Entry>;

  const bool IsStatic;
  const ELFO * const Obj;

  MipsGOTParser(const ELFO *Obj, Elf_Dyn_Range DynTable, Elf_Sym_Range DynSyms);

  bool hasGot() const { return !GotEntries.empty(); }
  bool hasPlt() const { return !PltEntries.empty(); }

  uint64_t getGp() const;

  const Entry *getGotLazyResolver() const;
  const Entry *getGotModulePointer() const;
  const Entry *getPltLazyResolver() const;
  const Entry *getPltModulePointer() const;

  Entries getLocalEntries() const;
  Entries getGlobalEntries() const;
  Entries getOtherEntries() const;
  Entries getPltEntries() const;

  uint64_t getGotAddress(const Entry * E) const;
  int64_t getGotOffset(const Entry * E) const;
  const Elf_Sym *getGotSym(const Entry *E) const;

  uint64_t getPltAddress(const Entry * E) const;
  const Elf_Sym *getPltSym(const Entry *E) const;

  StringRef getPltStrTable() const { return PltStrTable; }

private:
  const Elf_Shdr *GotSec;
  size_t LocalNum;
  size_t GlobalNum;

  const Elf_Shdr *PltSec;
  const Elf_Shdr *PltRelSec;
  const Elf_Shdr *PltSymTable;
  Elf_Sym_Range GotDynSyms;
  StringRef PltStrTable;

  Entries GotEntries;
  Entries PltEntries;
};

} // end anonymous namespace

template <class ELFT>
MipsGOTParser<ELFT>::MipsGOTParser(const ELFO *Obj, Elf_Dyn_Range DynTable,
                                   Elf_Sym_Range DynSyms)
    : IsStatic(DynTable.empty()), Obj(Obj), GotSec(nullptr), LocalNum(0),
      GlobalNum(0), PltSec(nullptr), PltRelSec(nullptr), PltSymTable(nullptr) {
  // See "Global Offset Table" in Chapter 5 in the following document
  // for detailed GOT description.
  // ftp://www.linux-mips.org/pub/linux/mips/doc/ABI/mipsabi.pdf

  // Find static GOT secton.
  if (IsStatic) {
    GotSec = findSectionByName(*Obj, ".got");
    if (!GotSec)
      reportError("Cannot find .got section");

    ArrayRef<uint8_t> Content = unwrapOrError(Obj->getSectionContents(GotSec));
    GotEntries = Entries(reinterpret_cast<const Entry *>(Content.data()),
                         Content.size() / sizeof(Entry));
    LocalNum = GotEntries.size();
    return;
  }

  // Lookup dynamic table tags which define GOT/PLT layouts.
  Optional<uint64_t> DtPltGot;
  Optional<uint64_t> DtLocalGotNum;
  Optional<uint64_t> DtGotSym;
  Optional<uint64_t> DtMipsPltGot;
  Optional<uint64_t> DtJmpRel;
  for (const auto &Entry : DynTable) {
    switch (Entry.getTag()) {
    case ELF::DT_PLTGOT:
      DtPltGot = Entry.getVal();
      break;
    case ELF::DT_MIPS_LOCAL_GOTNO:
      DtLocalGotNum = Entry.getVal();
      break;
    case ELF::DT_MIPS_GOTSYM:
      DtGotSym = Entry.getVal();
      break;
    case ELF::DT_MIPS_PLTGOT:
      DtMipsPltGot = Entry.getVal();
      break;
    case ELF::DT_JMPREL:
      DtJmpRel = Entry.getVal();
      break;
    }
  }

  // Find dynamic GOT section.
  if (DtPltGot || DtLocalGotNum || DtGotSym) {
    if (!DtPltGot)
      report_fatal_error("Cannot find PLTGOT dynamic table tag.");
    if (!DtLocalGotNum)
      report_fatal_error("Cannot find MIPS_LOCAL_GOTNO dynamic table tag.");
    if (!DtGotSym)
      report_fatal_error("Cannot find MIPS_GOTSYM dynamic table tag.");

    size_t DynSymTotal = DynSyms.size();
    if (*DtGotSym > DynSymTotal)
      reportError("MIPS_GOTSYM exceeds a number of dynamic symbols");

    GotSec = findNotEmptySectionByAddress(Obj, *DtPltGot);
    if (!GotSec)
      reportError("There is no not empty GOT section at 0x" +
                  Twine::utohexstr(*DtPltGot));

    LocalNum = *DtLocalGotNum;
    GlobalNum = DynSymTotal - *DtGotSym;

    ArrayRef<uint8_t> Content = unwrapOrError(Obj->getSectionContents(GotSec));
    GotEntries = Entries(reinterpret_cast<const Entry *>(Content.data()),
                         Content.size() / sizeof(Entry));
    GotDynSyms = DynSyms.drop_front(*DtGotSym);
  }

  // Find PLT section.
  if (DtMipsPltGot || DtJmpRel) {
    if (!DtMipsPltGot)
      report_fatal_error("Cannot find MIPS_PLTGOT dynamic table tag.");
    if (!DtJmpRel)
      report_fatal_error("Cannot find JMPREL dynamic table tag.");

    PltSec = findNotEmptySectionByAddress(Obj, *DtMipsPltGot);
    if (!PltSec)
      report_fatal_error("There is no not empty PLTGOT section at 0x " +
                         Twine::utohexstr(*DtMipsPltGot));

    PltRelSec = findNotEmptySectionByAddress(Obj, *DtJmpRel);
    if (!PltRelSec)
      report_fatal_error("There is no not empty RELPLT section at 0x" +
                         Twine::utohexstr(*DtJmpRel));

    ArrayRef<uint8_t> PltContent =
        unwrapOrError(Obj->getSectionContents(PltSec));
    PltEntries = Entries(reinterpret_cast<const Entry *>(PltContent.data()),
                         PltContent.size() / sizeof(Entry));

    PltSymTable = unwrapOrError(Obj->getSection(PltRelSec->sh_link));
    PltStrTable = unwrapOrError(Obj->getStringTableForSymtab(*PltSymTable));
  }
}

template <class ELFT> uint64_t MipsGOTParser<ELFT>::getGp() const {
  return GotSec->sh_addr + 0x7ff0;
}

template <class ELFT>
const typename MipsGOTParser<ELFT>::Entry *
MipsGOTParser<ELFT>::getGotLazyResolver() const {
  return LocalNum > 0 ? &GotEntries[0] : nullptr;
}

template <class ELFT>
const typename MipsGOTParser<ELFT>::Entry *
MipsGOTParser<ELFT>::getGotModulePointer() const {
  if (LocalNum < 2)
    return nullptr;
  const Entry &E = GotEntries[1];
  if ((E >> (sizeof(Entry) * 8 - 1)) == 0)
    return nullptr;
  return &E;
}

template <class ELFT>
typename MipsGOTParser<ELFT>::Entries
MipsGOTParser<ELFT>::getLocalEntries() const {
  size_t Skip = getGotModulePointer() ? 2 : 1;
  if (LocalNum - Skip <= 0)
    return Entries();
  return GotEntries.slice(Skip, LocalNum - Skip);
}

template <class ELFT>
typename MipsGOTParser<ELFT>::Entries
MipsGOTParser<ELFT>::getGlobalEntries() const {
  if (GlobalNum == 0)
    return Entries();
  return GotEntries.slice(LocalNum, GlobalNum);
}

template <class ELFT>
typename MipsGOTParser<ELFT>::Entries
MipsGOTParser<ELFT>::getOtherEntries() const {
  size_t OtherNum = GotEntries.size() - LocalNum - GlobalNum;
  if (OtherNum == 0)
    return Entries();
  return GotEntries.slice(LocalNum + GlobalNum, OtherNum);
}

template <class ELFT>
uint64_t MipsGOTParser<ELFT>::getGotAddress(const Entry *E) const {
  int64_t Offset = std::distance(GotEntries.data(), E) * sizeof(Entry);
  return GotSec->sh_addr + Offset;
}

template <class ELFT>
int64_t MipsGOTParser<ELFT>::getGotOffset(const Entry *E) const {
  int64_t Offset = std::distance(GotEntries.data(), E) * sizeof(Entry);
  return Offset - 0x7ff0;
}

template <class ELFT>
const typename MipsGOTParser<ELFT>::Elf_Sym *
MipsGOTParser<ELFT>::getGotSym(const Entry *E) const {
  int64_t Offset = std::distance(GotEntries.data(), E);
  return &GotDynSyms[Offset - LocalNum];
}

template <class ELFT>
const typename MipsGOTParser<ELFT>::Entry *
MipsGOTParser<ELFT>::getPltLazyResolver() const {
  return PltEntries.empty() ? nullptr : &PltEntries[0];
}

template <class ELFT>
const typename MipsGOTParser<ELFT>::Entry *
MipsGOTParser<ELFT>::getPltModulePointer() const {
  return PltEntries.size() < 2 ? nullptr : &PltEntries[1];
}

template <class ELFT>
typename MipsGOTParser<ELFT>::Entries
MipsGOTParser<ELFT>::getPltEntries() const {
  if (PltEntries.size() <= 2)
    return Entries();
  return PltEntries.slice(2, PltEntries.size() - 2);
}

template <class ELFT>
uint64_t MipsGOTParser<ELFT>::getPltAddress(const Entry *E) const {
  int64_t Offset = std::distance(PltEntries.data(), E) * sizeof(Entry);
  return PltSec->sh_addr + Offset;
}

template <class ELFT>
const typename MipsGOTParser<ELFT>::Elf_Sym *
MipsGOTParser<ELFT>::getPltSym(const Entry *E) const {
  int64_t Offset = std::distance(getPltEntries().data(), E);
  if (PltRelSec->sh_type == ELF::SHT_REL) {
    Elf_Rel_Range Rels = unwrapOrError(Obj->rels(PltRelSec));
    return unwrapOrError(Obj->getRelocationSymbol(&Rels[Offset], PltSymTable));
  } else {
    Elf_Rela_Range Rels = unwrapOrError(Obj->relas(PltRelSec));
    return unwrapOrError(Obj->getRelocationSymbol(&Rels[Offset], PltSymTable));
  }
}

template <class ELFT> void ELFDumper<ELFT>::printMipsPLTGOT() {
  const ELFFile<ELFT> *Obj = ObjF->getELFFile();
  if (Obj->getHeader()->e_machine != EM_MIPS)
    reportError("MIPS PLT GOT is available for MIPS targets only");

  MipsGOTParser<ELFT> Parser(Obj, dynamic_table(), dynamic_symbols());
  if (Parser.hasGot())
    ELFDumperStyle->printMipsGOT(Parser);
  if (Parser.hasPlt())
    ELFDumperStyle->printMipsPLT(Parser);
}

static const EnumEntry<unsigned> ElfMipsISAExtType[] = {
  {"None",                    Mips::AFL_EXT_NONE},
  {"Broadcom SB-1",           Mips::AFL_EXT_SB1},
  {"Cavium Networks Octeon",  Mips::AFL_EXT_OCTEON},
  {"Cavium Networks Octeon2", Mips::AFL_EXT_OCTEON2},
  {"Cavium Networks OcteonP", Mips::AFL_EXT_OCTEONP},
  {"Cavium Networks Octeon3", Mips::AFL_EXT_OCTEON3},
  {"LSI R4010",               Mips::AFL_EXT_4010},
  {"Loongson 2E",             Mips::AFL_EXT_LOONGSON_2E},
  {"Loongson 2F",             Mips::AFL_EXT_LOONGSON_2F},
  {"Loongson 3A",             Mips::AFL_EXT_LOONGSON_3A},
  {"MIPS R4650",              Mips::AFL_EXT_4650},
  {"MIPS R5900",              Mips::AFL_EXT_5900},
  {"MIPS R10000",             Mips::AFL_EXT_10000},
  {"NEC VR4100",              Mips::AFL_EXT_4100},
  {"NEC VR4111/VR4181",       Mips::AFL_EXT_4111},
  {"NEC VR4120",              Mips::AFL_EXT_4120},
  {"NEC VR5400",              Mips::AFL_EXT_5400},
  {"NEC VR5500",              Mips::AFL_EXT_5500},
  {"RMI Xlr",                 Mips::AFL_EXT_XLR},
  {"Toshiba R3900",           Mips::AFL_EXT_3900}
};

static const EnumEntry<unsigned> ElfMipsASEFlags[] = {
  {"DSP",                Mips::AFL_ASE_DSP},
  {"DSPR2",              Mips::AFL_ASE_DSPR2},
  {"Enhanced VA Scheme", Mips::AFL_ASE_EVA},
  {"MCU",                Mips::AFL_ASE_MCU},
  {"MDMX",               Mips::AFL_ASE_MDMX},
  {"MIPS-3D",            Mips::AFL_ASE_MIPS3D},
  {"MT",                 Mips::AFL_ASE_MT},
  {"SmartMIPS",          Mips::AFL_ASE_SMARTMIPS},
  {"VZ",                 Mips::AFL_ASE_VIRT},
  {"MSA",                Mips::AFL_ASE_MSA},
  {"MIPS16",             Mips::AFL_ASE_MIPS16},
  {"microMIPS",          Mips::AFL_ASE_MICROMIPS},
  {"XPA",                Mips::AFL_ASE_XPA},
  {"CRC",                Mips::AFL_ASE_CRC},
  {"GINV",               Mips::AFL_ASE_GINV},
};

static const EnumEntry<unsigned> ElfMipsFpABIType[] = {
  {"Hard or soft float",                  Mips::Val_GNU_MIPS_ABI_FP_ANY},
  {"Hard float (double precision)",       Mips::Val_GNU_MIPS_ABI_FP_DOUBLE},
  {"Hard float (single precision)",       Mips::Val_GNU_MIPS_ABI_FP_SINGLE},
  {"Soft float",                          Mips::Val_GNU_MIPS_ABI_FP_SOFT},
  {"Hard float (MIPS32r2 64-bit FPU 12 callee-saved)",
   Mips::Val_GNU_MIPS_ABI_FP_OLD_64},
  {"Hard float (32-bit CPU, Any FPU)",    Mips::Val_GNU_MIPS_ABI_FP_XX},
  {"Hard float (32-bit CPU, 64-bit FPU)", Mips::Val_GNU_MIPS_ABI_FP_64},
  {"Hard float compat (32-bit CPU, 64-bit FPU)",
   Mips::Val_GNU_MIPS_ABI_FP_64A}
};

static const EnumEntry<unsigned> ElfMipsFlags1[] {
  {"ODDSPREG", Mips::AFL_FLAGS1_ODDSPREG},
};

static int getMipsRegisterSize(uint8_t Flag) {
  switch (Flag) {
  case Mips::AFL_REG_NONE:
    return 0;
  case Mips::AFL_REG_32:
    return 32;
  case Mips::AFL_REG_64:
    return 64;
  case Mips::AFL_REG_128:
    return 128;
  default:
    return -1;
  }
}

template <class ELFT> void ELFDumper<ELFT>::printMipsABIFlags() {
  const ELFFile<ELFT> *Obj = ObjF->getELFFile();
  const Elf_Shdr *Shdr = findSectionByName(*Obj, ".MIPS.abiflags");
  if (!Shdr) {
    W.startLine() << "There is no .MIPS.abiflags section in the file.\n";
    return;
  }
  ArrayRef<uint8_t> Sec = unwrapOrError(Obj->getSectionContents(Shdr));
  if (Sec.size() != sizeof(Elf_Mips_ABIFlags<ELFT>)) {
    W.startLine() << "The .MIPS.abiflags section has a wrong size.\n";
    return;
  }

  auto *Flags = reinterpret_cast<const Elf_Mips_ABIFlags<ELFT> *>(Sec.data());

  raw_ostream &OS = W.getOStream();
  DictScope GS(W, "MIPS ABI Flags");

  W.printNumber("Version", Flags->version);
  W.startLine() << "ISA: ";
  if (Flags->isa_rev <= 1)
    OS << format("MIPS%u", Flags->isa_level);
  else
    OS << format("MIPS%ur%u", Flags->isa_level, Flags->isa_rev);
  OS << "\n";
  W.printEnum("ISA Extension", Flags->isa_ext, makeArrayRef(ElfMipsISAExtType));
  W.printFlags("ASEs", Flags->ases, makeArrayRef(ElfMipsASEFlags));
  W.printEnum("FP ABI", Flags->fp_abi, makeArrayRef(ElfMipsFpABIType));
  W.printNumber("GPR size", getMipsRegisterSize(Flags->gpr_size));
  W.printNumber("CPR1 size", getMipsRegisterSize(Flags->cpr1_size));
  W.printNumber("CPR2 size", getMipsRegisterSize(Flags->cpr2_size));
  W.printFlags("Flags 1", Flags->flags1, makeArrayRef(ElfMipsFlags1));
  W.printHex("Flags 2", Flags->flags2);
}

template <class ELFT>
static void printMipsReginfoData(ScopedPrinter &W,
                                 const Elf_Mips_RegInfo<ELFT> &Reginfo) {
  W.printHex("GP", Reginfo.ri_gp_value);
  W.printHex("General Mask", Reginfo.ri_gprmask);
  W.printHex("Co-Proc Mask0", Reginfo.ri_cprmask[0]);
  W.printHex("Co-Proc Mask1", Reginfo.ri_cprmask[1]);
  W.printHex("Co-Proc Mask2", Reginfo.ri_cprmask[2]);
  W.printHex("Co-Proc Mask3", Reginfo.ri_cprmask[3]);
}

template <class ELFT> void ELFDumper<ELFT>::printMipsReginfo() {
  const ELFFile<ELFT> *Obj = ObjF->getELFFile();
  const Elf_Shdr *Shdr = findSectionByName(*Obj, ".reginfo");
  if (!Shdr) {
    W.startLine() << "There is no .reginfo section in the file.\n";
    return;
  }
  ArrayRef<uint8_t> Sec = unwrapOrError(Obj->getSectionContents(Shdr));
  if (Sec.size() != sizeof(Elf_Mips_RegInfo<ELFT>)) {
    W.startLine() << "The .reginfo section has a wrong size.\n";
    return;
  }

  DictScope GS(W, "MIPS RegInfo");
  auto *Reginfo = reinterpret_cast<const Elf_Mips_RegInfo<ELFT> *>(Sec.data());
  printMipsReginfoData(W, *Reginfo);
}

template <class ELFT> void ELFDumper<ELFT>::printMipsOptions() {
  const ELFFile<ELFT> *Obj = ObjF->getELFFile();
  const Elf_Shdr *Shdr = findSectionByName(*Obj, ".MIPS.options");
  if (!Shdr) {
    W.startLine() << "There is no .MIPS.options section in the file.\n";
    return;
  }

  DictScope GS(W, "MIPS Options");

  ArrayRef<uint8_t> Sec = unwrapOrError(Obj->getSectionContents(Shdr));
  while (!Sec.empty()) {
    if (Sec.size() < sizeof(Elf_Mips_Options<ELFT>)) {
      W.startLine() << "The .MIPS.options section has a wrong size.\n";
      return;
    }
    auto *O = reinterpret_cast<const Elf_Mips_Options<ELFT> *>(Sec.data());
    DictScope GS(W, getElfMipsOptionsOdkType(O->kind));
    switch (O->kind) {
    case ODK_REGINFO:
      printMipsReginfoData(W, O->getRegInfo());
      break;
    default:
      W.startLine() << "Unsupported MIPS options tag.\n";
      break;
    }
    Sec = Sec.slice(O->size);
  }
}

template <class ELFT> void ELFDumper<ELFT>::printStackMap() const {
  const ELFFile<ELFT> *Obj = ObjF->getELFFile();
  const Elf_Shdr *StackMapSection = nullptr;
  for (const auto &Sec : unwrapOrError(Obj->sections())) {
    StringRef Name = unwrapOrError(Obj->getSectionName(&Sec));
    if (Name == ".llvm_stackmaps") {
      StackMapSection = &Sec;
      break;
    }
  }

  if (!StackMapSection)
    return;

  ArrayRef<uint8_t> StackMapContentsArray =
      unwrapOrError(Obj->getSectionContents(StackMapSection));

  prettyPrintStackMap(
      W, StackMapParser<ELFT::TargetEndianness>(StackMapContentsArray));
}

template <class ELFT> void ELFDumper<ELFT>::printGroupSections() {
  ELFDumperStyle->printGroupSections(ObjF->getELFFile());
}

template <class ELFT> void ELFDumper<ELFT>::printAddrsig() {
  ELFDumperStyle->printAddrsig(ObjF->getELFFile());
}

static inline void printFields(formatted_raw_ostream &OS, StringRef Str1,
                               StringRef Str2) {
  OS.PadToColumn(2u);
  OS << Str1;
  OS.PadToColumn(37u);
  OS << Str2 << "\n";
  OS.flush();
}

template <class ELFT>
static std::string getSectionHeadersNumString(const ELFFile<ELFT> *Obj) {
  const typename ELFT::Ehdr *ElfHeader = Obj->getHeader();
  if (ElfHeader->e_shnum != 0)
    return to_string(ElfHeader->e_shnum);

  ArrayRef<typename ELFT::Shdr> Arr = unwrapOrError(Obj->sections());
  if (Arr.empty())
    return "0";
  return "0 (" + to_string(Arr[0].sh_size) + ")";
}

template <class ELFT>
static std::string getSectionHeaderTableIndexString(const ELFFile<ELFT> *Obj) {
  const typename ELFT::Ehdr *ElfHeader = Obj->getHeader();
  if (ElfHeader->e_shstrndx != SHN_XINDEX)
    return to_string(ElfHeader->e_shstrndx);

  ArrayRef<typename ELFT::Shdr> Arr = unwrapOrError(Obj->sections());
  if (Arr.empty())
    return "65535 (corrupt: out of range)";
  return to_string(ElfHeader->e_shstrndx) + " (" + to_string(Arr[0].sh_link) +
         ")";
}

template <class ELFT> void GNUStyle<ELFT>::printFileHeaders(const ELFO *Obj) {
  const Elf_Ehdr *e = Obj->getHeader();
  OS << "ELF Header:\n";
  OS << "  Magic:  ";
  std::string Str;
  for (int i = 0; i < ELF::EI_NIDENT; i++)
    OS << format(" %02x", static_cast<int>(e->e_ident[i]));
  OS << "\n";
  Str = printEnum(e->e_ident[ELF::EI_CLASS], makeArrayRef(ElfClass));
  printFields(OS, "Class:", Str);
  Str = printEnum(e->e_ident[ELF::EI_DATA], makeArrayRef(ElfDataEncoding));
  printFields(OS, "Data:", Str);
  OS.PadToColumn(2u);
  OS << "Version:";
  OS.PadToColumn(37u);
  OS << to_hexString(e->e_ident[ELF::EI_VERSION]);
  if (e->e_version == ELF::EV_CURRENT)
    OS << " (current)";
  OS << "\n";
  Str = printEnum(e->e_ident[ELF::EI_OSABI], makeArrayRef(ElfOSABI));
  printFields(OS, "OS/ABI:", Str);
  Str = "0x" + to_hexString(e->e_ident[ELF::EI_ABIVERSION]);
  printFields(OS, "ABI Version:", Str);
  Str = printEnum(e->e_type, makeArrayRef(ElfObjectFileType));
  printFields(OS, "Type:", Str);
  Str = printEnum(e->e_machine, makeArrayRef(ElfMachineType));
  printFields(OS, "Machine:", Str);
  Str = "0x" + to_hexString(e->e_version);
  printFields(OS, "Version:", Str);
  Str = "0x" + to_hexString(e->e_entry);
  printFields(OS, "Entry point address:", Str);
  Str = to_string(e->e_phoff) + " (bytes into file)";
  printFields(OS, "Start of program headers:", Str);
  Str = to_string(e->e_shoff) + " (bytes into file)";
  printFields(OS, "Start of section headers:", Str);
  std::string ElfFlags;
  if (e->e_machine == EM_MIPS)
    ElfFlags =
        printFlags(e->e_flags, makeArrayRef(ElfHeaderMipsFlags),
                   unsigned(ELF::EF_MIPS_ARCH), unsigned(ELF::EF_MIPS_ABI),
                   unsigned(ELF::EF_MIPS_MACH));
  else if (e->e_machine == EM_RISCV)
    ElfFlags = printFlags(e->e_flags, makeArrayRef(ElfHeaderRISCVFlags));
  Str = "0x" + to_hexString(e->e_flags);
  if (!ElfFlags.empty())
    Str = Str + ", " + ElfFlags;
  printFields(OS, "Flags:", Str);
  Str = to_string(e->e_ehsize) + " (bytes)";
  printFields(OS, "Size of this header:", Str);
  Str = to_string(e->e_phentsize) + " (bytes)";
  printFields(OS, "Size of program headers:", Str);
  Str = to_string(e->e_phnum);
  printFields(OS, "Number of program headers:", Str);
  Str = to_string(e->e_shentsize) + " (bytes)";
  printFields(OS, "Size of section headers:", Str);
  Str = getSectionHeadersNumString(Obj);
  printFields(OS, "Number of section headers:", Str);
  Str = getSectionHeaderTableIndexString(Obj);
  printFields(OS, "Section header string table index:", Str);
}

namespace {
struct GroupMember {
  StringRef Name;
  uint64_t Index;
};

struct GroupSection {
  StringRef Name;
  std::string Signature;
  uint64_t ShName;
  uint64_t Index;
  uint32_t Link;
  uint32_t Info;
  uint32_t Type;
  std::vector<GroupMember> Members;
};

template <class ELFT>
std::vector<GroupSection> getGroups(const ELFFile<ELFT> *Obj) {
  using Elf_Shdr = typename ELFT::Shdr;
  using Elf_Sym = typename ELFT::Sym;
  using Elf_Word = typename ELFT::Word;

  std::vector<GroupSection> Ret;
  uint64_t I = 0;
  for (const Elf_Shdr &Sec : unwrapOrError(Obj->sections())) {
    ++I;
    if (Sec.sh_type != ELF::SHT_GROUP)
      continue;

    const Elf_Shdr *Symtab = unwrapOrError(Obj->getSection(Sec.sh_link));
    StringRef StrTable = unwrapOrError(Obj->getStringTableForSymtab(*Symtab));
    const Elf_Sym *Sym =
        unwrapOrError(Obj->template getEntry<Elf_Sym>(Symtab, Sec.sh_info));
    auto Data =
        unwrapOrError(Obj->template getSectionContentsAsArray<Elf_Word>(&Sec));

    StringRef Name = unwrapOrError(Obj->getSectionName(&Sec));
    StringRef Signature = StrTable.data() + Sym->st_name;
    Ret.push_back({Name,
                   maybeDemangle(Signature),
                   Sec.sh_name,
                   I - 1,
                   Sec.sh_link,
                   Sec.sh_info,
                   Data[0],
                   {}});

    std::vector<GroupMember> &GM = Ret.back().Members;
    for (uint32_t Ndx : Data.slice(1)) {
      auto Sec = unwrapOrError(Obj->getSection(Ndx));
      const StringRef Name = unwrapOrError(Obj->getSectionName(Sec));
      GM.push_back({Name, Ndx});
    }
  }
  return Ret;
}

DenseMap<uint64_t, const GroupSection *>
mapSectionsToGroups(ArrayRef<GroupSection> Groups) {
  DenseMap<uint64_t, const GroupSection *> Ret;
  for (const GroupSection &G : Groups)
    for (const GroupMember &GM : G.Members)
      Ret.insert({GM.Index, &G});
  return Ret;
}

} // namespace

template <class ELFT> void GNUStyle<ELFT>::printGroupSections(const ELFO *Obj) {
  std::vector<GroupSection> V = getGroups<ELFT>(Obj);
  DenseMap<uint64_t, const GroupSection *> Map = mapSectionsToGroups(V);
  for (const GroupSection &G : V) {
    OS << "\n"
       << getGroupType(G.Type) << " group section ["
       << format_decimal(G.Index, 5) << "] `" << G.Name << "' [" << G.Signature
       << "] contains " << G.Members.size() << " sections:\n"
       << "   [Index]    Name\n";
    for (const GroupMember &GM : G.Members) {
      const GroupSection *MainGroup = Map[GM.Index];
      if (MainGroup != &G) {
        OS.flush();
        errs() << "Error: section [" << format_decimal(GM.Index, 5)
               << "] in group section [" << format_decimal(G.Index, 5)
               << "] already in group section ["
               << format_decimal(MainGroup->Index, 5) << "]";
        errs().flush();
        continue;
      }
      OS << "   [" << format_decimal(GM.Index, 5) << "]   " << GM.Name << "\n";
    }
  }

  if (V.empty())
    OS << "There are no section groups in this file.\n";
}

template <class ELFT>
void GNUStyle<ELFT>::printRelocation(const ELFO *Obj, const Elf_Shdr *SymTab,
                                     const Elf_Rela &R, bool IsRela) {
  const Elf_Sym *Sym = unwrapOrError(Obj->getRelocationSymbol(&R, SymTab));
  std::string TargetName;
  if (Sym && Sym->getType() == ELF::STT_SECTION) {
    const Elf_Shdr *Sec = unwrapOrError(
        Obj->getSection(Sym, SymTab, this->dumper()->getShndxTable()));
    TargetName = unwrapOrError(Obj->getSectionName(Sec));
  } else if (Sym) {
    StringRef StrTable = unwrapOrError(Obj->getStringTableForSymtab(*SymTab));
    TargetName = this->dumper()->getFullSymbolName(
        Sym, StrTable, SymTab->sh_type == SHT_DYNSYM /* IsDynamic */);
  }
  printRelocation(Obj, Sym, TargetName, R, IsRela);
}

template <class ELFT>
void GNUStyle<ELFT>::printRelocation(const ELFO *Obj, const Elf_Sym *Sym,
                                     StringRef SymbolName, const Elf_Rela &R,
                                     bool IsRela) {
  // First two fields are bit width dependent. The rest of them are fixed width.
  unsigned Bias = ELFT::Is64Bits ? 8 : 0;
  Field Fields[5] = {0, 10 + Bias, 19 + 2 * Bias, 42 + 2 * Bias, 53 + 2 * Bias};
  unsigned Width = ELFT::Is64Bits ? 16 : 8;

  Fields[0].Str = to_string(format_hex_no_prefix(R.r_offset, Width));
  Fields[1].Str = to_string(format_hex_no_prefix(R.r_info, Width));

  SmallString<32> RelocName;
  Obj->getRelocationTypeName(R.getType(Obj->isMips64EL()), RelocName);
  Fields[2].Str = RelocName.c_str();

  if (Sym && (!SymbolName.empty() || Sym->getValue() != 0))
    Fields[3].Str = to_string(format_hex_no_prefix(Sym->getValue(), Width));

  Fields[4].Str = SymbolName;
  for (const Field &F : Fields)
    printField(F);

  std::string Addend;
  if (IsRela) {
    int64_t RelAddend = R.r_addend;
    if (!SymbolName.empty()) {
      if (R.r_addend < 0) {
        Addend = " - ";
        RelAddend = std::abs(RelAddend);
      } else
        Addend = " + ";
    }

    Addend += to_hexString(RelAddend, false);
  }
  OS << Addend << "\n";
}

template <class ELFT> void GNUStyle<ELFT>::printRelocHeader(unsigned SType) {
  bool IsRela = SType == ELF::SHT_RELA || SType == ELF::SHT_ANDROID_RELA;
  bool IsRelr = SType == ELF::SHT_RELR || SType == ELF::SHT_ANDROID_RELR;
  if (ELFT::Is64Bits)
    OS << "    ";
  else
    OS << " ";
  if (IsRelr && opts::RawRelr)
    OS << "Data  ";
  else
    OS << "Offset";
  if (ELFT::Is64Bits)
    OS << "             Info             Type"
       << "               Symbol's Value  Symbol's Name";
  else
    OS << "     Info    Type                Sym. Value  Symbol's Name";
  if (IsRela)
    OS << " + Addend";
  OS << "\n";
}

template <class ELFT> void GNUStyle<ELFT>::printRelocations(const ELFO *Obj) {
  bool HasRelocSections = false;
  for (const Elf_Shdr &Sec : unwrapOrError(Obj->sections())) {
    if (Sec.sh_type != ELF::SHT_REL && Sec.sh_type != ELF::SHT_RELA &&
        Sec.sh_type != ELF::SHT_RELR && Sec.sh_type != ELF::SHT_ANDROID_REL &&
        Sec.sh_type != ELF::SHT_ANDROID_RELA &&
        Sec.sh_type != ELF::SHT_ANDROID_RELR)
      continue;
    HasRelocSections = true;
    StringRef Name = unwrapOrError(Obj->getSectionName(&Sec));
    unsigned Entries = Sec.getEntityCount();
    std::vector<Elf_Rela> AndroidRelas;
    if (Sec.sh_type == ELF::SHT_ANDROID_REL ||
        Sec.sh_type == ELF::SHT_ANDROID_RELA) {
      // Android's packed relocation section needs to be unpacked first
      // to get the actual number of entries.
      AndroidRelas = unwrapOrError(Obj->android_relas(&Sec));
      Entries = AndroidRelas.size();
    }
    std::vector<Elf_Rela> RelrRelas;
    if (!opts::RawRelr && (Sec.sh_type == ELF::SHT_RELR ||
                           Sec.sh_type == ELF::SHT_ANDROID_RELR)) {
      // .relr.dyn relative relocation section needs to be unpacked first
      // to get the actual number of entries.
      Elf_Relr_Range Relrs = unwrapOrError(Obj->relrs(&Sec));
      RelrRelas = unwrapOrError(Obj->decode_relrs(Relrs));
      Entries = RelrRelas.size();
    }
    uintX_t Offset = Sec.sh_offset;
    OS << "\nRelocation section '" << Name << "' at offset 0x"
       << to_hexString(Offset, false) << " contains " << Entries
       << " entries:\n";
    printRelocHeader(Sec.sh_type);
    const Elf_Shdr *SymTab = unwrapOrError(Obj->getSection(Sec.sh_link));
    switch (Sec.sh_type) {
    case ELF::SHT_REL:
      for (const auto &R : unwrapOrError(Obj->rels(&Sec))) {
        Elf_Rela Rela;
        Rela.r_offset = R.r_offset;
        Rela.r_info = R.r_info;
        Rela.r_addend = 0;
        printRelocation(Obj, SymTab, Rela, false);
      }
      break;
    case ELF::SHT_RELA:
      for (const auto &R : unwrapOrError(Obj->relas(&Sec)))
        printRelocation(Obj, SymTab, R, true);
      break;
    case ELF::SHT_RELR:
    case ELF::SHT_ANDROID_RELR:
      if (opts::RawRelr)
        for (const auto &R : unwrapOrError(Obj->relrs(&Sec)))
          OS << to_string(format_hex_no_prefix(R, ELFT::Is64Bits ? 16 : 8))
             << "\n";
      else
        for (const auto &R : RelrRelas)
          printRelocation(Obj, SymTab, R, false);
      break;
    case ELF::SHT_ANDROID_REL:
    case ELF::SHT_ANDROID_RELA:
      for (const auto &R : AndroidRelas)
        printRelocation(Obj, SymTab, R, Sec.sh_type == ELF::SHT_ANDROID_RELA);
      break;
    }
  }
  if (!HasRelocSections)
    OS << "\nThere are no relocations in this file.\n";
}

// Print the offset of a particular section from anyone of the ranges:
// [SHT_LOOS, SHT_HIOS], [SHT_LOPROC, SHT_HIPROC], [SHT_LOUSER, SHT_HIUSER].
// If 'Type' does not fall within any of those ranges, then a string is
// returned as '<unknown>' followed by the type value.
static std::string getSectionTypeOffsetString(unsigned Type) {
  if (Type >= SHT_LOOS && Type <= SHT_HIOS)
    return "LOOS+0x" + to_hexString(Type - SHT_LOOS);
  else if (Type >= SHT_LOPROC && Type <= SHT_HIPROC)
    return "LOPROC+0x" + to_hexString(Type - SHT_LOPROC);
  else if (Type >= SHT_LOUSER && Type <= SHT_HIUSER)
    return "LOUSER+0x" + to_hexString(Type - SHT_LOUSER);
  return "0x" + to_hexString(Type) + ": <unknown>";
}

static std::string getSectionTypeString(unsigned Arch, unsigned Type) {
  using namespace ELF;

  switch (Arch) {
  case EM_ARM:
    switch (Type) {
    case SHT_ARM_EXIDX:
      return "ARM_EXIDX";
    case SHT_ARM_PREEMPTMAP:
      return "ARM_PREEMPTMAP";
    case SHT_ARM_ATTRIBUTES:
      return "ARM_ATTRIBUTES";
    case SHT_ARM_DEBUGOVERLAY:
      return "ARM_DEBUGOVERLAY";
    case SHT_ARM_OVERLAYSECTION:
      return "ARM_OVERLAYSECTION";
    }
    break;
  case EM_X86_64:
    switch (Type) {
    case SHT_X86_64_UNWIND:
      return "X86_64_UNWIND";
    }
    break;
  case EM_MIPS:
  case EM_MIPS_RS3_LE:
    switch (Type) {
    case SHT_MIPS_REGINFO:
      return "MIPS_REGINFO";
    case SHT_MIPS_OPTIONS:
      return "MIPS_OPTIONS";
    case SHT_MIPS_DWARF:
      return "MIPS_DWARF";
    case SHT_MIPS_ABIFLAGS:
      return "MIPS_ABIFLAGS";
    }
    break;
  }
  switch (Type) {
  case SHT_NULL:
    return "NULL";
  case SHT_PROGBITS:
    return "PROGBITS";
  case SHT_SYMTAB:
    return "SYMTAB";
  case SHT_STRTAB:
    return "STRTAB";
  case SHT_RELA:
    return "RELA";
  case SHT_HASH:
    return "HASH";
  case SHT_DYNAMIC:
    return "DYNAMIC";
  case SHT_NOTE:
    return "NOTE";
  case SHT_NOBITS:
    return "NOBITS";
  case SHT_REL:
    return "REL";
  case SHT_SHLIB:
    return "SHLIB";
  case SHT_DYNSYM:
    return "DYNSYM";
  case SHT_INIT_ARRAY:
    return "INIT_ARRAY";
  case SHT_FINI_ARRAY:
    return "FINI_ARRAY";
  case SHT_PREINIT_ARRAY:
    return "PREINIT_ARRAY";
  case SHT_GROUP:
    return "GROUP";
  case SHT_SYMTAB_SHNDX:
    return "SYMTAB SECTION INDICES";
  case SHT_ANDROID_REL:
    return "ANDROID_REL";
  case SHT_ANDROID_RELA:
    return "ANDROID_RELA";
  case SHT_RELR:
  case SHT_ANDROID_RELR:
    return "RELR";
  case SHT_LLVM_ODRTAB:
    return "LLVM_ODRTAB";
  case SHT_LLVM_LINKER_OPTIONS:
    return "LLVM_LINKER_OPTIONS";
  case SHT_LLVM_CALL_GRAPH_PROFILE:
    return "LLVM_CALL_GRAPH_PROFILE";
  case SHT_LLVM_ADDRSIG:
    return "LLVM_ADDRSIG";
  case SHT_LLVM_DEPENDENT_LIBRARIES:
    return "LLVM_DEPENDENT_LIBRARIES";
  // FIXME: Parse processor specific GNU attributes
  case SHT_GNU_ATTRIBUTES:
    return "ATTRIBUTES";
  case SHT_GNU_HASH:
    return "GNU_HASH";
  case SHT_GNU_verdef:
    return "VERDEF";
  case SHT_GNU_verneed:
    return "VERNEED";
  case SHT_GNU_versym:
    return "VERSYM";
  default:
    return getSectionTypeOffsetString(Type);
  }
  return "";
}

template <class ELFT>
static StringRef getSectionName(const typename ELFT::Shdr &Sec,
                                const ELFObjectFile<ELFT> &ElfObj,
                                ArrayRef<typename ELFT::Shdr> Sections) {
  const ELFFile<ELFT> &Obj = *ElfObj.getELFFile();
  uint32_t Index = Obj.getHeader()->e_shstrndx;
  if (Index == ELF::SHN_XINDEX)
    Index = Sections[0].sh_link;
  if (!Index) // no section string table.
    return "";
  // TODO: Test a case when the sh_link of the section with index 0 is broken.
  if (Index >= Sections.size())
    reportError(ElfObj.getFileName(),
                createError("section header string table index " +
                            Twine(Index) + " does not exist"));
  StringRef Data = toStringRef(unwrapOrError(
      Obj.template getSectionContentsAsArray<uint8_t>(&Sections[Index])));
  return unwrapOrError(Obj.getSectionName(&Sec, Data));
}

template <class ELFT>
void GNUStyle<ELFT>::printSectionHeaders(const ELFO *Obj) {
  unsigned Bias = ELFT::Is64Bits ? 0 : 8;
  ArrayRef<Elf_Shdr> Sections = unwrapOrError(Obj->sections());
  OS << "There are " << to_string(Sections.size())
     << " section headers, starting at offset "
     << "0x" << to_hexString(Obj->getHeader()->e_shoff, false) << ":\n\n";
  OS << "Section Headers:\n";
  Field Fields[11] = {
      {"[Nr]", 2},        {"Name", 7},        {"Type", 25},
      {"Address", 41},    {"Off", 58 - Bias}, {"Size", 65 - Bias},
      {"ES", 72 - Bias},  {"Flg", 75 - Bias}, {"Lk", 79 - Bias},
      {"Inf", 82 - Bias}, {"Al", 86 - Bias}};
  for (auto &F : Fields)
    printField(F);
  OS << "\n";

  const ELFObjectFile<ELFT> *ElfObj = this->dumper()->getElfObject();
  size_t SectionIndex = 0;
  for (const Elf_Shdr &Sec : Sections) {
    Fields[0].Str = to_string(SectionIndex);
    Fields[1].Str = getSectionName(Sec, *ElfObj, Sections);
    Fields[2].Str =
        getSectionTypeString(Obj->getHeader()->e_machine, Sec.sh_type);
    Fields[3].Str =
        to_string(format_hex_no_prefix(Sec.sh_addr, ELFT::Is64Bits ? 16 : 8));
    Fields[4].Str = to_string(format_hex_no_prefix(Sec.sh_offset, 6));
    Fields[5].Str = to_string(format_hex_no_prefix(Sec.sh_size, 6));
    Fields[6].Str = to_string(format_hex_no_prefix(Sec.sh_entsize, 2));
    Fields[7].Str = getGNUFlags(Sec.sh_flags);
    Fields[8].Str = to_string(Sec.sh_link);
    Fields[9].Str = to_string(Sec.sh_info);
    Fields[10].Str = to_string(Sec.sh_addralign);

    OS.PadToColumn(Fields[0].Column);
    OS << "[" << right_justify(Fields[0].Str, 2) << "]";
    for (int i = 1; i < 7; i++)
      printField(Fields[i]);
    OS.PadToColumn(Fields[7].Column);
    OS << right_justify(Fields[7].Str, 3);
    OS.PadToColumn(Fields[8].Column);
    OS << right_justify(Fields[8].Str, 2);
    OS.PadToColumn(Fields[9].Column);
    OS << right_justify(Fields[9].Str, 3);
    OS.PadToColumn(Fields[10].Column);
    OS << right_justify(Fields[10].Str, 2);
    OS << "\n";
    ++SectionIndex;
  }
  OS << "Key to Flags:\n"
     << "  W (write), A (alloc), X (execute), M (merge), S (strings), l "
        "(large)\n"
     << "  I (info), L (link order), G (group), T (TLS), E (exclude),\
 x (unknown)\n"
     << "  O (extra OS processing required) o (OS specific),\
 p (processor specific)\n";
}

template <class ELFT>
void GNUStyle<ELFT>::printSymtabMessage(const ELFO *Obj, StringRef Name,
                                        size_t Entries) {
  if (!Name.empty())
    OS << "\nSymbol table '" << Name << "' contains " << Entries
       << " entries:\n";
  else
    OS << "\n Symbol table for image:\n";

  if (ELFT::Is64Bits)
    OS << "   Num:    Value          Size Type    Bind   Vis      Ndx Name\n";
  else
    OS << "   Num:    Value  Size Type    Bind   Vis      Ndx Name\n";
}

template <class ELFT>
std::string GNUStyle<ELFT>::getSymbolSectionNdx(const ELFO *Obj,
                                                const Elf_Sym *Symbol,
                                                const Elf_Sym *FirstSym) {
  unsigned SectionIndex = Symbol->st_shndx;
  switch (SectionIndex) {
  case ELF::SHN_UNDEF:
    return "UND";
  case ELF::SHN_ABS:
    return "ABS";
  case ELF::SHN_COMMON:
    return "COM";
  case ELF::SHN_XINDEX:
    return to_string(
        format_decimal(unwrapOrError(object::getExtendedSymbolTableIndex<ELFT>(
                           Symbol, FirstSym, this->dumper()->getShndxTable())),
                       3));
  default:
    // Find if:
    // Processor specific
    if (SectionIndex >= ELF::SHN_LOPROC && SectionIndex <= ELF::SHN_HIPROC)
      return std::string("PRC[0x") +
             to_string(format_hex_no_prefix(SectionIndex, 4)) + "]";
    // OS specific
    if (SectionIndex >= ELF::SHN_LOOS && SectionIndex <= ELF::SHN_HIOS)
      return std::string("OS[0x") +
             to_string(format_hex_no_prefix(SectionIndex, 4)) + "]";
    // Architecture reserved:
    if (SectionIndex >= ELF::SHN_LORESERVE &&
        SectionIndex <= ELF::SHN_HIRESERVE)
      return std::string("RSV[0x") +
             to_string(format_hex_no_prefix(SectionIndex, 4)) + "]";
    // A normal section with an index
    return to_string(format_decimal(SectionIndex, 3));
  }
}

template <class ELFT>
void GNUStyle<ELFT>::printSymbol(const ELFO *Obj, const Elf_Sym *Symbol,
                                 const Elf_Sym *FirstSym, StringRef StrTable,
                                 bool IsDynamic) {
  static int Idx = 0;
  static bool Dynamic = true;

  // If this function was called with a different value from IsDynamic
  // from last call, happens when we move from dynamic to static symbol
  // table, "Num" field should be reset.
  if (!Dynamic != !IsDynamic) {
    Idx = 0;
    Dynamic = false;
  }

  unsigned Bias = ELFT::Is64Bits ? 8 : 0;
  Field Fields[8] = {0,         8,         17 + Bias, 23 + Bias,
                     31 + Bias, 38 + Bias, 47 + Bias, 51 + Bias};
  Fields[0].Str = to_string(format_decimal(Idx++, 6)) + ":";
  Fields[1].Str = to_string(
      format_hex_no_prefix(Symbol->st_value, ELFT::Is64Bits ? 16 : 8));
  Fields[2].Str = to_string(format_decimal(Symbol->st_size, 5));

  unsigned char SymbolType = Symbol->getType();
  if (Obj->getHeader()->e_machine == ELF::EM_AMDGPU &&
      SymbolType >= ELF::STT_LOOS && SymbolType < ELF::STT_HIOS)
    Fields[3].Str = printEnum(SymbolType, makeArrayRef(AMDGPUSymbolTypes));
  else
    Fields[3].Str = printEnum(SymbolType, makeArrayRef(ElfSymbolTypes));

  Fields[4].Str =
      printEnum(Symbol->getBinding(), makeArrayRef(ElfSymbolBindings));
  Fields[5].Str =
      printEnum(Symbol->getVisibility(), makeArrayRef(ElfSymbolVisibilities));
  Fields[6].Str = getSymbolSectionNdx(Obj, Symbol, FirstSym);
  Fields[7].Str =
      this->dumper()->getFullSymbolName(Symbol, StrTable, IsDynamic);
  for (auto &Entry : Fields)
    printField(Entry);
  OS << "\n";
}

template <class ELFT>
void GNUStyle<ELFT>::printHashedSymbol(const ELFO *Obj, const Elf_Sym *FirstSym,
                                       uint32_t Sym, StringRef StrTable,
                                       uint32_t Bucket) {
  unsigned Bias = ELFT::Is64Bits ? 8 : 0;
  Field Fields[9] = {0,         6,         11,        20 + Bias, 25 + Bias,
                     34 + Bias, 41 + Bias, 49 + Bias, 53 + Bias};
  Fields[0].Str = to_string(format_decimal(Sym, 5));
  Fields[1].Str = to_string(format_decimal(Bucket, 3)) + ":";

  const auto Symbol = FirstSym + Sym;
  Fields[2].Str = to_string(
      format_hex_no_prefix(Symbol->st_value, ELFT::Is64Bits ? 18 : 8));
  Fields[3].Str = to_string(format_decimal(Symbol->st_size, 5));

  unsigned char SymbolType = Symbol->getType();
  if (Obj->getHeader()->e_machine == ELF::EM_AMDGPU &&
      SymbolType >= ELF::STT_LOOS && SymbolType < ELF::STT_HIOS)
    Fields[4].Str = printEnum(SymbolType, makeArrayRef(AMDGPUSymbolTypes));
  else
    Fields[4].Str = printEnum(SymbolType, makeArrayRef(ElfSymbolTypes));

  Fields[5].Str =
      printEnum(Symbol->getBinding(), makeArrayRef(ElfSymbolBindings));
  Fields[6].Str =
      printEnum(Symbol->getVisibility(), makeArrayRef(ElfSymbolVisibilities));
  Fields[7].Str = getSymbolSectionNdx(Obj, Symbol, FirstSym);
  Fields[8].Str = this->dumper()->getFullSymbolName(Symbol, StrTable, true);

  for (auto &Entry : Fields)
    printField(Entry);
  OS << "\n";
}

template <class ELFT>
void GNUStyle<ELFT>::printSymbols(const ELFO *Obj, bool PrintSymbols,
                                  bool PrintDynamicSymbols) {
  if (!PrintSymbols && !PrintDynamicSymbols)
    return;
  // GNU readelf prints both the .dynsym and .symtab with --symbols.
  this->dumper()->printSymbolsHelper(true);
  if (PrintSymbols)
    this->dumper()->printSymbolsHelper(false);
}

template <class ELFT> void GNUStyle<ELFT>::printHashSymbols(const ELFO *Obj) {
  if (this->dumper()->getDynamicStringTable().empty())
    return;
  auto StringTable = this->dumper()->getDynamicStringTable();
  auto DynSyms = this->dumper()->dynamic_symbols();

  // Try printing .hash
  if (auto SysVHash = this->dumper()->getHashTable()) {
    OS << "\n Symbol table of .hash for image:\n";
    if (ELFT::Is64Bits)
      OS << "  Num Buc:    Value          Size   Type   Bind Vis      Ndx Name";
    else
      OS << "  Num Buc:    Value  Size   Type   Bind Vis      Ndx Name";
    OS << "\n";

    auto Buckets = SysVHash->buckets();
    auto Chains = SysVHash->chains();
    for (uint32_t Buc = 0; Buc < SysVHash->nbucket; Buc++) {
      if (Buckets[Buc] == ELF::STN_UNDEF)
        continue;
      for (uint32_t Ch = Buckets[Buc]; Ch < SysVHash->nchain; Ch = Chains[Ch]) {
        if (Ch == ELF::STN_UNDEF)
          break;
        printHashedSymbol(Obj, &DynSyms[0], Ch, StringTable, Buc);
      }
    }
  }

  // Try printing .gnu.hash
  if (auto GnuHash = this->dumper()->getGnuHashTable()) {
    OS << "\n Symbol table of .gnu.hash for image:\n";
    if (ELFT::Is64Bits)
      OS << "  Num Buc:    Value          Size   Type   Bind Vis      Ndx Name";
    else
      OS << "  Num Buc:    Value  Size   Type   Bind Vis      Ndx Name";
    OS << "\n";
    auto Buckets = GnuHash->buckets();
    for (uint32_t Buc = 0; Buc < GnuHash->nbuckets; Buc++) {
      if (Buckets[Buc] == ELF::STN_UNDEF)
        continue;
      uint32_t Index = Buckets[Buc];
      uint32_t GnuHashable = Index - GnuHash->symndx;
      // Print whole chain
      while (true) {
        printHashedSymbol(Obj, &DynSyms[0], Index++, StringTable, Buc);
        // Chain ends at symbol with stopper bit
        if ((GnuHash->values(DynSyms.size())[GnuHashable++] & 1) == 1)
          break;
      }
    }
  }
}

static inline std::string printPhdrFlags(unsigned Flag) {
  std::string Str;
  Str = (Flag & PF_R) ? "R" : " ";
  Str += (Flag & PF_W) ? "W" : " ";
  Str += (Flag & PF_X) ? "E" : " ";
  return Str;
}

// SHF_TLS sections are only in PT_TLS, PT_LOAD or PT_GNU_RELRO
// PT_TLS must only have SHF_TLS sections
template <class ELFT>
bool GNUStyle<ELFT>::checkTLSSections(const Elf_Phdr &Phdr,
                                      const Elf_Shdr &Sec) {
  return (((Sec.sh_flags & ELF::SHF_TLS) &&
           ((Phdr.p_type == ELF::PT_TLS) || (Phdr.p_type == ELF::PT_LOAD) ||
            (Phdr.p_type == ELF::PT_GNU_RELRO))) ||
          (!(Sec.sh_flags & ELF::SHF_TLS) && Phdr.p_type != ELF::PT_TLS));
}

// Non-SHT_NOBITS must have its offset inside the segment
// Only non-zero section can be at end of segment
template <class ELFT>
bool GNUStyle<ELFT>::checkoffsets(const Elf_Phdr &Phdr, const Elf_Shdr &Sec) {
  if (Sec.sh_type == ELF::SHT_NOBITS)
    return true;
  bool IsSpecial =
      (Sec.sh_type == ELF::SHT_NOBITS) && ((Sec.sh_flags & ELF::SHF_TLS) != 0);
  // .tbss is special, it only has memory in PT_TLS and has NOBITS properties
  auto SectionSize =
      (IsSpecial && Phdr.p_type != ELF::PT_TLS) ? 0 : Sec.sh_size;
  if (Sec.sh_offset >= Phdr.p_offset)
    return ((Sec.sh_offset + SectionSize <= Phdr.p_filesz + Phdr.p_offset)
            /*only non-zero sized sections at end*/
            && (Sec.sh_offset + 1 <= Phdr.p_offset + Phdr.p_filesz));
  return false;
}

// SHF_ALLOC must have VMA inside segment
// Only non-zero section can be at end of segment
template <class ELFT>
bool GNUStyle<ELFT>::checkVMA(const Elf_Phdr &Phdr, const Elf_Shdr &Sec) {
  if (!(Sec.sh_flags & ELF::SHF_ALLOC))
    return true;
  bool IsSpecial =
      (Sec.sh_type == ELF::SHT_NOBITS) && ((Sec.sh_flags & ELF::SHF_TLS) != 0);
  // .tbss is special, it only has memory in PT_TLS and has NOBITS properties
  auto SectionSize =
      (IsSpecial && Phdr.p_type != ELF::PT_TLS) ? 0 : Sec.sh_size;
  if (Sec.sh_addr >= Phdr.p_vaddr)
    return ((Sec.sh_addr + SectionSize <= Phdr.p_vaddr + Phdr.p_memsz) &&
            (Sec.sh_addr + 1 <= Phdr.p_vaddr + Phdr.p_memsz));
  return false;
}

// No section with zero size must be at start or end of PT_DYNAMIC
template <class ELFT>
bool GNUStyle<ELFT>::checkPTDynamic(const Elf_Phdr &Phdr, const Elf_Shdr &Sec) {
  if (Phdr.p_type != ELF::PT_DYNAMIC || Sec.sh_size != 0 || Phdr.p_memsz == 0)
    return true;
  // Is section within the phdr both based on offset and VMA ?
  return ((Sec.sh_type == ELF::SHT_NOBITS) ||
          (Sec.sh_offset > Phdr.p_offset &&
           Sec.sh_offset < Phdr.p_offset + Phdr.p_filesz)) &&
         (!(Sec.sh_flags & ELF::SHF_ALLOC) ||
          (Sec.sh_addr > Phdr.p_vaddr && Sec.sh_addr < Phdr.p_memsz));
}

template <class ELFT>
void GNUStyle<ELFT>::printProgramHeaders(
    const ELFO *Obj, bool PrintProgramHeaders,
    cl::boolOrDefault PrintSectionMapping) {
  if (PrintProgramHeaders)
    printProgramHeaders(Obj);

  // Display the section mapping along with the program headers, unless
  // -section-mapping is explicitly set to false.
  if (PrintSectionMapping != cl::BOU_FALSE)
    printSectionMapping(Obj);
}

template <class ELFT>
void GNUStyle<ELFT>::printProgramHeaders(const ELFO *Obj) {
  unsigned Bias = ELFT::Is64Bits ? 8 : 0;
  const Elf_Ehdr *Header = Obj->getHeader();
  Field Fields[8] = {2,         17,        26,        37 + Bias,
                     48 + Bias, 56 + Bias, 64 + Bias, 68 + Bias};
  OS << "\nElf file type is "
     << printEnum(Header->e_type, makeArrayRef(ElfObjectFileType)) << "\n"
     << "Entry point " << format_hex(Header->e_entry, 3) << "\n"
     << "There are " << Header->e_phnum << " program headers,"
     << " starting at offset " << Header->e_phoff << "\n\n"
     << "Program Headers:\n";
  if (ELFT::Is64Bits)
    OS << "  Type           Offset   VirtAddr           PhysAddr         "
       << "  FileSiz  MemSiz   Flg Align\n";
  else
    OS << "  Type           Offset   VirtAddr   PhysAddr   FileSiz "
       << "MemSiz  Flg Align\n";

  unsigned Width = ELFT::Is64Bits ? 18 : 10;
  unsigned SizeWidth = ELFT::Is64Bits ? 8 : 7;
  for (const auto &Phdr : unwrapOrError(Obj->program_headers())) {
    Fields[0].Str = getElfPtType(Header->e_machine, Phdr.p_type);
    Fields[1].Str = to_string(format_hex(Phdr.p_offset, 8));
    Fields[2].Str = to_string(format_hex(Phdr.p_vaddr, Width));
    Fields[3].Str = to_string(format_hex(Phdr.p_paddr, Width));
    Fields[4].Str = to_string(format_hex(Phdr.p_filesz, SizeWidth));
    Fields[5].Str = to_string(format_hex(Phdr.p_memsz, SizeWidth));
    Fields[6].Str = printPhdrFlags(Phdr.p_flags);
    Fields[7].Str = to_string(format_hex(Phdr.p_align, 1));
    for (auto Field : Fields)
      printField(Field);
    if (Phdr.p_type == ELF::PT_INTERP) {
      OS << "\n      [Requesting program interpreter: ";
      OS << reinterpret_cast<const char *>(Obj->base()) + Phdr.p_offset << "]";
    }
    OS << "\n";
  }
}

template <class ELFT>
void GNUStyle<ELFT>::printSectionMapping(const ELFO *Obj) {
  OS << "\n Section to Segment mapping:\n  Segment Sections...\n";
  DenseSet<const Elf_Shdr *> BelongsToSegment;
  int Phnum = 0;
  for (const Elf_Phdr &Phdr : unwrapOrError(Obj->program_headers())) {
    std::string Sections;
    OS << format("   %2.2d     ", Phnum++);
    for (const Elf_Shdr &Sec : unwrapOrError(Obj->sections())) {
      // Check if each section is in a segment and then print mapping.
      // readelf additionally makes sure it does not print zero sized sections
      // at end of segments and for PT_DYNAMIC both start and end of section
      // .tbss must only be shown in PT_TLS section.
      bool TbssInNonTLS = (Sec.sh_type == ELF::SHT_NOBITS) &&
                          ((Sec.sh_flags & ELF::SHF_TLS) != 0) &&
                          Phdr.p_type != ELF::PT_TLS;
      if (!TbssInNonTLS && checkTLSSections(Phdr, Sec) &&
          checkoffsets(Phdr, Sec) && checkVMA(Phdr, Sec) &&
          checkPTDynamic(Phdr, Sec) && (Sec.sh_type != ELF::SHT_NULL)) {
        Sections += unwrapOrError(Obj->getSectionName(&Sec)).str() + " ";
        BelongsToSegment.insert(&Sec);
      }
    }
    OS << Sections << "\n";
    OS.flush();
  }

  // Display sections that do not belong to a segment.
  std::string Sections;
  for (const Elf_Shdr &Sec : unwrapOrError(Obj->sections())) {
    if (BelongsToSegment.find(&Sec) == BelongsToSegment.end())
      Sections += unwrapOrError(Obj->getSectionName(&Sec)).str() + ' ';
  }
  if (!Sections.empty()) {
    OS << "   None  " << Sections << '\n';
    OS.flush();
  }
}

template <class ELFT>
void GNUStyle<ELFT>::printDynamicRelocation(const ELFO *Obj, Elf_Rela R,
                                            bool IsRela) {
  uint32_t SymIndex = R.getSymbol(Obj->isMips64EL());
  const Elf_Sym *Sym = this->dumper()->dynamic_symbols().begin() + SymIndex;
  std::string SymbolName = maybeDemangle(
      unwrapOrError(Sym->getName(this->dumper()->getDynamicStringTable())));
  printRelocation(Obj, Sym, SymbolName, R, IsRela);
}

template <class ELFT> void GNUStyle<ELFT>::printDynamic(const ELFO *Obj) {
  Elf_Dyn_Range Table = this->dumper()->dynamic_table();
  if (Table.empty())
    return;

  const DynRegionInfo &DynamicTableRegion =
      this->dumper()->getDynamicTableRegion();

  OS << "Dynamic section at offset "
     << format_hex(reinterpret_cast<const uint8_t *>(DynamicTableRegion.Addr) -
                       Obj->base(),
                   1)
     << " contains " << Table.size() << " entries:\n";

  bool Is64 = ELFT::Is64Bits;
  if (Is64)
    OS << "  Tag                Type                 Name/Value\n";
  else
    OS << "  Tag        Type                 Name/Value\n";
  for (auto Entry : Table) {
    uintX_t Tag = Entry.getTag();
    std::string TypeString = std::string("(") +
                             getTypeString(Obj->getHeader()->e_machine, Tag) +
                             ")";
    OS << "  " << format_hex(Tag, Is64 ? 18 : 10)
       << format(" %-20s ", TypeString.c_str());
    this->dumper()->printDynamicEntry(OS, Tag, Entry.getVal());
    OS << "\n";
  }
}

template <class ELFT>
void GNUStyle<ELFT>::printDynamicRelocations(const ELFO *Obj) {
  const DynRegionInfo &DynRelRegion = this->dumper()->getDynRelRegion();
  const DynRegionInfo &DynRelaRegion = this->dumper()->getDynRelaRegion();
  const DynRegionInfo &DynRelrRegion = this->dumper()->getDynRelrRegion();
  const DynRegionInfo &DynPLTRelRegion = this->dumper()->getDynPLTRelRegion();
  if (DynRelaRegion.Size > 0) {
    OS << "\n'RELA' relocation section at offset "
       << format_hex(reinterpret_cast<const uint8_t *>(DynRelaRegion.Addr) -
                         Obj->base(),
                     1)
       << " contains " << DynRelaRegion.Size << " bytes:\n";
    printRelocHeader(ELF::SHT_RELA);
    for (const Elf_Rela &Rela : this->dumper()->dyn_relas())
      printDynamicRelocation(Obj, Rela, true);
  }
  if (DynRelRegion.Size > 0) {
    OS << "\n'REL' relocation section at offset "
       << format_hex(reinterpret_cast<const uint8_t *>(DynRelRegion.Addr) -
                         Obj->base(),
                     1)
       << " contains " << DynRelRegion.Size << " bytes:\n";
    printRelocHeader(ELF::SHT_REL);
    for (const Elf_Rel &Rel : this->dumper()->dyn_rels()) {
      Elf_Rela Rela;
      Rela.r_offset = Rel.r_offset;
      Rela.r_info = Rel.r_info;
      Rela.r_addend = 0;
      printDynamicRelocation(Obj, Rela, false);
    }
  }
  if (DynRelrRegion.Size > 0) {
    OS << "\n'RELR' relocation section at offset "
       << format_hex(reinterpret_cast<const uint8_t *>(DynRelrRegion.Addr) -
                         Obj->base(),
                     1)
       << " contains " << DynRelrRegion.Size << " bytes:\n";
    printRelocHeader(ELF::SHT_REL);
    Elf_Relr_Range Relrs = this->dumper()->dyn_relrs();
    std::vector<Elf_Rela> RelrRelas = unwrapOrError(Obj->decode_relrs(Relrs));
    for (const Elf_Rela &Rela : RelrRelas) {
      printDynamicRelocation(Obj, Rela, false);
    }
  }
  if (DynPLTRelRegion.Size) {
    OS << "\n'PLT' relocation section at offset "
       << format_hex(reinterpret_cast<const uint8_t *>(DynPLTRelRegion.Addr) -
                         Obj->base(),
                     1)
       << " contains " << DynPLTRelRegion.Size << " bytes:\n";
  }
  if (DynPLTRelRegion.EntSize == sizeof(Elf_Rela)) {
    printRelocHeader(ELF::SHT_RELA);
    for (const Elf_Rela &Rela : DynPLTRelRegion.getAsArrayRef<Elf_Rela>())
      printDynamicRelocation(Obj, Rela, true);
  } else {
    printRelocHeader(ELF::SHT_REL);
    for (const Elf_Rel &Rel : DynPLTRelRegion.getAsArrayRef<Elf_Rel>()) {
      Elf_Rela Rela;
      Rela.r_offset = Rel.r_offset;
      Rela.r_info = Rel.r_info;
      Rela.r_addend = 0;
      printDynamicRelocation(Obj, Rela, false);
    }
  }
}

template <class ELFT>
static void printGNUVersionSectionProlog(formatted_raw_ostream &OS,
                                         const Twine &Name, unsigned EntriesNum,
                                         const ELFFile<ELFT> *Obj,
                                         const typename ELFT::Shdr *Sec) {
  StringRef SecName = unwrapOrError(Obj->getSectionName(Sec));
  OS << Name << " section '" << SecName << "' "
     << "contains " << EntriesNum << " entries:\n";

  const typename ELFT::Shdr *SymTab =
      unwrapOrError(Obj->getSection(Sec->sh_link));
  StringRef SymTabName = unwrapOrError(Obj->getSectionName(SymTab));
  OS << " Addr: " << format_hex_no_prefix(Sec->sh_addr, 16)
     << "  Offset: " << format_hex(Sec->sh_offset, 8)
     << "  Link: " << Sec->sh_link << " (" << SymTabName << ")\n";
}

template <class ELFT>
void GNUStyle<ELFT>::printVersionSymbolSection(const ELFFile<ELFT> *Obj,
                                               const Elf_Shdr *Sec) {
  if (!Sec)
    return;

  unsigned Entries = Sec->sh_size / sizeof(Elf_Versym);
  printGNUVersionSectionProlog(OS, "Version symbols", Entries, Obj, Sec);

  const uint8_t *VersymBuf =
      reinterpret_cast<const uint8_t *>(Obj->base() + Sec->sh_offset);
  const ELFDumper<ELFT> *Dumper = this->dumper();
  StringRef StrTable = Dumper->getDynamicStringTable();

  // readelf prints 4 entries per line.
  for (uint64_t VersymRow = 0; VersymRow < Entries; VersymRow += 4) {
    OS << "  " << format_hex_no_prefix(VersymRow, 3) << ":";

    for (uint64_t VersymIndex = 0;
         (VersymIndex < 4) && (VersymIndex + VersymRow) < Entries;
         ++VersymIndex) {
      const Elf_Versym *Versym =
          reinterpret_cast<const Elf_Versym *>(VersymBuf);
      switch (Versym->vs_index) {
      case 0:
        OS << "   0 (*local*)    ";
        break;
      case 1:
        OS << "   1 (*global*)   ";
        break;
      default:
        OS << format("%4x%c", Versym->vs_index & VERSYM_VERSION,
                     Versym->vs_index & VERSYM_HIDDEN ? 'h' : ' ');

        bool IsDefault = true;
        std::string VersionName = Dumper->getSymbolVersionByIndex(
            StrTable, Versym->vs_index, IsDefault);

        if (!VersionName.empty())
          VersionName = "(" + VersionName + ")";
        else
          VersionName = "(*invalid*)";
        OS << left_justify(VersionName, 13);
      }
      VersymBuf += sizeof(Elf_Versym);
    }
    OS << '\n';
  }
  OS << '\n';
}

static std::string versionFlagToString(unsigned Flags) {
  if (Flags == 0)
    return "none";

  std::string Ret;
  auto AddFlag = [&Ret, &Flags](unsigned Flag, StringRef Name) {
    if (!(Flags & Flag))
      return;
    if (!Ret.empty())
      Ret += " | ";
    Ret += Name;
    Flags &= ~Flag;
  };

  AddFlag(VER_FLG_BASE, "BASE");
  AddFlag(VER_FLG_WEAK, "WEAK");
  AddFlag(VER_FLG_INFO, "INFO");
  AddFlag(~0, "<unknown>");
  return Ret;
}

template <class ELFT>
void GNUStyle<ELFT>::printVersionDefinitionSection(const ELFFile<ELFT> *Obj,
                                                   const Elf_Shdr *Sec) {
  if (!Sec)
    return;

  unsigned VerDefsNum = Sec->sh_info;
  printGNUVersionSectionProlog(OS, "Version definition", VerDefsNum, Obj, Sec);

  const Elf_Shdr *StrTabSec = unwrapOrError(Obj->getSection(Sec->sh_link));
  StringRef StringTable(
      reinterpret_cast<const char *>(Obj->base() + StrTabSec->sh_offset),
      (size_t)StrTabSec->sh_size);

  const uint8_t *VerdefBuf = unwrapOrError(Obj->getSectionContents(Sec)).data();
  const uint8_t *Begin = VerdefBuf;

  while (VerDefsNum--) {
    const Elf_Verdef *Verdef = reinterpret_cast<const Elf_Verdef *>(VerdefBuf);
    OS << format("  0x%04x: Rev: %u  Flags: %s  Index: %u  Cnt: %u",
                 VerdefBuf - Begin, (unsigned)Verdef->vd_version,
                 versionFlagToString(Verdef->vd_flags).c_str(),
                 (unsigned)Verdef->vd_ndx, (unsigned)Verdef->vd_cnt);

    const uint8_t *VerdauxBuf = VerdefBuf + Verdef->vd_aux;
    const Elf_Verdaux *Verdaux =
        reinterpret_cast<const Elf_Verdaux *>(VerdauxBuf);
    OS << format("  Name: %s\n",
                 StringTable.drop_front(Verdaux->vda_name).data());

    for (unsigned I = 1; I < Verdef->vd_cnt; ++I) {
      VerdauxBuf += Verdaux->vda_next;
      Verdaux = reinterpret_cast<const Elf_Verdaux *>(VerdauxBuf);
      OS << format("  0x%04x: Parent %u: %s\n", VerdauxBuf - Begin, I,
                   StringTable.drop_front(Verdaux->vda_name).data());
    }

    VerdefBuf += Verdef->vd_next;
  }
  OS << '\n';
}

template <class ELFT>
void GNUStyle<ELFT>::printVersionDependencySection(const ELFFile<ELFT> *Obj,
                                                   const Elf_Shdr *Sec) {
  if (!Sec)
    return;

  unsigned VerneedNum = Sec->sh_info;
  printGNUVersionSectionProlog(OS, "Version needs", VerneedNum, Obj, Sec);

  ArrayRef<uint8_t> SecData = unwrapOrError(Obj->getSectionContents(Sec));

  const Elf_Shdr *StrTabSec = unwrapOrError(Obj->getSection(Sec->sh_link));
  StringRef StringTable = {
      reinterpret_cast<const char *>(Obj->base() + StrTabSec->sh_offset),
      (size_t)StrTabSec->sh_size};

  const uint8_t *VerneedBuf = SecData.data();
  for (unsigned I = 0; I < VerneedNum; ++I) {
    const Elf_Verneed *Verneed =
        reinterpret_cast<const Elf_Verneed *>(VerneedBuf);

    OS << format("  0x%04x: Version: %u  File: %s  Cnt: %u\n",
                 reinterpret_cast<const uint8_t *>(Verneed) - SecData.begin(),
                 (unsigned)Verneed->vn_version,
                 StringTable.drop_front(Verneed->vn_file).data(),
                 (unsigned)Verneed->vn_cnt);

    const uint8_t *VernauxBuf = VerneedBuf + Verneed->vn_aux;
    for (unsigned J = 0; J < Verneed->vn_cnt; ++J) {
      const Elf_Vernaux *Vernaux =
          reinterpret_cast<const Elf_Vernaux *>(VernauxBuf);

      OS << format("  0x%04x:   Name: %s  Flags: %s  Version: %u\n",
                   reinterpret_cast<const uint8_t *>(Vernaux) - SecData.begin(),
                   StringTable.drop_front(Vernaux->vna_name).data(),
                   versionFlagToString(Vernaux->vna_flags).c_str(),
                   (unsigned)Vernaux->vna_other);
      VernauxBuf += Vernaux->vna_next;
    }
    VerneedBuf += Verneed->vn_next;
  }
  OS << '\n';
}

// Hash histogram shows  statistics of how efficient the hash was for the
// dynamic symbol table. The table shows number of hash buckets for different
// lengths of chains as absolute number and percentage of the total buckets.
// Additionally cumulative coverage of symbols for each set of buckets.
template <class ELFT>
void GNUStyle<ELFT>::printHashHistogram(const ELFFile<ELFT> *Obj) {
  // Print histogram for .hash section
  if (const Elf_Hash *HashTable = this->dumper()->getHashTable()) {
    size_t NBucket = HashTable->nbucket;
    size_t NChain = HashTable->nchain;
    ArrayRef<Elf_Word> Buckets = HashTable->buckets();
    ArrayRef<Elf_Word> Chains = HashTable->chains();
    size_t TotalSyms = 0;
    // If hash table is correct, we have at least chains with 0 length
    size_t MaxChain = 1;
    size_t CumulativeNonZero = 0;

    if (NChain == 0 || NBucket == 0)
      return;

    std::vector<size_t> ChainLen(NBucket, 0);
    // Go over all buckets and and note chain lengths of each bucket (total
    // unique chain lengths).
    for (size_t B = 0; B < NBucket; B++) {
      for (size_t C = Buckets[B]; C > 0 && C < NChain; C = Chains[C])
        if (MaxChain <= ++ChainLen[B])
          MaxChain++;
      TotalSyms += ChainLen[B];
    }

    if (!TotalSyms)
      return;

    std::vector<size_t> Count(MaxChain, 0) ;
    // Count how long is the chain for each bucket
    for (size_t B = 0; B < NBucket; B++)
      ++Count[ChainLen[B]];
    // Print Number of buckets with each chain lengths and their cumulative
    // coverage of the symbols
    OS << "Histogram for bucket list length (total of " << NBucket
       << " buckets)\n"
       << " Length  Number     % of total  Coverage\n";
    for (size_t I = 0; I < MaxChain; I++) {
      CumulativeNonZero += Count[I] * I;
      OS << format("%7lu  %-10lu (%5.1f%%)     %5.1f%%\n", I, Count[I],
                   (Count[I] * 100.0) / NBucket,
                   (CumulativeNonZero * 100.0) / TotalSyms);
    }
  }

  // Print histogram for .gnu.hash section
  if (const Elf_GnuHash *GnuHashTable = this->dumper()->getGnuHashTable()) {
    size_t NBucket = GnuHashTable->nbuckets;
    ArrayRef<Elf_Word> Buckets = GnuHashTable->buckets();
    unsigned NumSyms = this->dumper()->dynamic_symbols().size();
    if (!NumSyms)
      return;
    ArrayRef<Elf_Word> Chains = GnuHashTable->values(NumSyms);
    size_t Symndx = GnuHashTable->symndx;
    size_t TotalSyms = 0;
    size_t MaxChain = 1;
    size_t CumulativeNonZero = 0;

    if (Chains.empty() || NBucket == 0)
      return;

    std::vector<size_t> ChainLen(NBucket, 0);

    for (size_t B = 0; B < NBucket; B++) {
      if (!Buckets[B])
        continue;
      size_t Len = 1;
      for (size_t C = Buckets[B] - Symndx;
           C < Chains.size() && (Chains[C] & 1) == 0; C++)
        if (MaxChain < ++Len)
          MaxChain++;
      ChainLen[B] = Len;
      TotalSyms += Len;
    }
    MaxChain++;

    if (!TotalSyms)
      return;

    std::vector<size_t> Count(MaxChain, 0) ;
    for (size_t B = 0; B < NBucket; B++)
      ++Count[ChainLen[B]];
    // Print Number of buckets with each chain lengths and their cumulative
    // coverage of the symbols
    OS << "Histogram for `.gnu.hash' bucket list length (total of " << NBucket
       << " buckets)\n"
       << " Length  Number     % of total  Coverage\n";
    for (size_t I = 0; I <MaxChain; I++) {
      CumulativeNonZero += Count[I] * I;
      OS << format("%7lu  %-10lu (%5.1f%%)     %5.1f%%\n", I, Count[I],
                   (Count[I] * 100.0) / NBucket,
                   (CumulativeNonZero * 100.0) / TotalSyms);
    }
  }
}

template <class ELFT>
void GNUStyle<ELFT>::printCGProfile(const ELFFile<ELFT> *Obj) {
  OS << "GNUStyle::printCGProfile not implemented\n";
}

template <class ELFT>
void GNUStyle<ELFT>::printAddrsig(const ELFFile<ELFT> *Obj) {
    OS << "GNUStyle::printAddrsig not implemented\n";
}

static StringRef getGenericNoteTypeName(const uint32_t NT) {
  static const struct {
    uint32_t ID;
    const char *Name;
  } Notes[] = {
      {ELF::NT_VERSION, "NT_VERSION (version)"},
      {ELF::NT_ARCH, "NT_ARCH (architecture)"},
      {ELF::NT_GNU_BUILD_ATTRIBUTE_OPEN, "OPEN"},
      {ELF::NT_GNU_BUILD_ATTRIBUTE_FUNC, "func"},
  };

  for (const auto &Note : Notes)
    if (Note.ID == NT)
      return Note.Name;

  return "";
}

static std::string getGNUNoteTypeName(const uint32_t NT) {
  static const struct {
    uint32_t ID;
    const char *Name;
  } Notes[] = {
      {ELF::NT_GNU_ABI_TAG, "NT_GNU_ABI_TAG (ABI version tag)"},
      {ELF::NT_GNU_HWCAP, "NT_GNU_HWCAP (DSO-supplied software HWCAP info)"},
      {ELF::NT_GNU_BUILD_ID, "NT_GNU_BUILD_ID (unique build ID bitstring)"},
      {ELF::NT_GNU_GOLD_VERSION, "NT_GNU_GOLD_VERSION (gold version)"},
      {ELF::NT_GNU_PROPERTY_TYPE_0, "NT_GNU_PROPERTY_TYPE_0 (property note)"},
  };

  for (const auto &Note : Notes)
    if (Note.ID == NT)
      return std::string(Note.Name);

  std::string string;
  raw_string_ostream OS(string);
  OS << format("Unknown note type (0x%08x)", NT);
  return OS.str();
}

static std::string getFreeBSDNoteTypeName(const uint32_t NT) {
  static const struct {
    uint32_t ID;
    const char *Name;
  } Notes[] = {
      {ELF::NT_FREEBSD_THRMISC, "NT_THRMISC (thrmisc structure)"},
      {ELF::NT_FREEBSD_PROCSTAT_PROC, "NT_PROCSTAT_PROC (proc data)"},
      {ELF::NT_FREEBSD_PROCSTAT_FILES, "NT_PROCSTAT_FILES (files data)"},
      {ELF::NT_FREEBSD_PROCSTAT_VMMAP, "NT_PROCSTAT_VMMAP (vmmap data)"},
      {ELF::NT_FREEBSD_PROCSTAT_GROUPS, "NT_PROCSTAT_GROUPS (groups data)"},
      {ELF::NT_FREEBSD_PROCSTAT_UMASK, "NT_PROCSTAT_UMASK (umask data)"},
      {ELF::NT_FREEBSD_PROCSTAT_RLIMIT, "NT_PROCSTAT_RLIMIT (rlimit data)"},
      {ELF::NT_FREEBSD_PROCSTAT_OSREL, "NT_PROCSTAT_OSREL (osreldate data)"},
      {ELF::NT_FREEBSD_PROCSTAT_PSSTRINGS,
       "NT_PROCSTAT_PSSTRINGS (ps_strings data)"},
      {ELF::NT_FREEBSD_PROCSTAT_AUXV, "NT_PROCSTAT_AUXV (auxv data)"},
  };

  for (const auto &Note : Notes)
    if (Note.ID == NT)
      return std::string(Note.Name);

  std::string string;
  raw_string_ostream OS(string);
  OS << format("Unknown note type (0x%08x)", NT);
  return OS.str();
}

static std::string getAMDNoteTypeName(const uint32_t NT) {
  static const struct {
    uint32_t ID;
    const char *Name;
  } Notes[] = {{ELF::NT_AMD_AMDGPU_HSA_METADATA,
                "NT_AMD_AMDGPU_HSA_METADATA (HSA Metadata)"},
               {ELF::NT_AMD_AMDGPU_ISA, "NT_AMD_AMDGPU_ISA (ISA Version)"},
               {ELF::NT_AMD_AMDGPU_PAL_METADATA,
                "NT_AMD_AMDGPU_PAL_METADATA (PAL Metadata)"}};

  for (const auto &Note : Notes)
    if (Note.ID == NT)
      return std::string(Note.Name);

  std::string string;
  raw_string_ostream OS(string);
  OS << format("Unknown note type (0x%08x)", NT);
  return OS.str();
}

static std::string getAMDGPUNoteTypeName(const uint32_t NT) {
  if (NT == ELF::NT_AMDGPU_METADATA)
    return std::string("NT_AMDGPU_METADATA (AMDGPU Metadata)");

  std::string string;
  raw_string_ostream OS(string);
  OS << format("Unknown note type (0x%08x)", NT);
  return OS.str();
}

template <typename ELFT>
static std::string getGNUProperty(uint32_t Type, uint32_t DataSize,
                                  ArrayRef<uint8_t> Data) {
  std::string str;
  raw_string_ostream OS(str);
  uint32_t PrData;
  auto DumpBit = [&](uint32_t Flag, StringRef Name) {
    if (PrData & Flag) {
      PrData &= ~Flag;
      OS << Name;
      if (PrData)
        OS << ", ";
    }
  };

  switch (Type) {
  default:
    OS << format("<application-specific type 0x%x>", Type);
    return OS.str();
  case GNU_PROPERTY_STACK_SIZE: {
    OS << "stack size: ";
    if (DataSize == sizeof(typename ELFT::uint))
      OS << formatv("{0:x}",
                    (uint64_t)(*(const typename ELFT::Addr *)Data.data()));
    else
      OS << format("<corrupt length: 0x%x>", DataSize);
    return OS.str();
  }
  case GNU_PROPERTY_NO_COPY_ON_PROTECTED:
    OS << "no copy on protected";
    if (DataSize)
      OS << format(" <corrupt length: 0x%x>", DataSize);
    return OS.str();
  case GNU_PROPERTY_AARCH64_FEATURE_1_AND:
  case GNU_PROPERTY_X86_FEATURE_1_AND:
    OS << ((Type == GNU_PROPERTY_AARCH64_FEATURE_1_AND) ? "aarch64 feature: "
                                                        : "x86 feature: ");
    if (DataSize != 4) {
      OS << format("<corrupt length: 0x%x>", DataSize);
      return OS.str();
    }
    PrData = support::endian::read32<ELFT::TargetEndianness>(Data.data());
    if (PrData == 0) {
      OS << "<None>";
      return OS.str();
    }
    if (Type == GNU_PROPERTY_AARCH64_FEATURE_1_AND) {
      DumpBit(GNU_PROPERTY_AARCH64_FEATURE_1_BTI, "BTI");
      DumpBit(GNU_PROPERTY_AARCH64_FEATURE_1_PAC, "PAC");
    } else {
      DumpBit(GNU_PROPERTY_X86_FEATURE_1_IBT, "IBT");
      DumpBit(GNU_PROPERTY_X86_FEATURE_1_SHSTK, "SHSTK");
    }
    if (PrData)
      OS << format("<unknown flags: 0x%x>", PrData);
    return OS.str();
  case GNU_PROPERTY_X86_ISA_1_NEEDED:
  case GNU_PROPERTY_X86_ISA_1_USED:
    OS << "x86 ISA "
       << (Type == GNU_PROPERTY_X86_ISA_1_NEEDED ? "needed: " : "used: ");
    if (DataSize != 4) {
      OS << format("<corrupt length: 0x%x>", DataSize);
      return OS.str();
    }
    PrData = support::endian::read32<ELFT::TargetEndianness>(Data.data());
    if (PrData == 0) {
      OS << "<None>";
      return OS.str();
    }
    DumpBit(GNU_PROPERTY_X86_ISA_1_CMOV, "CMOV");
    DumpBit(GNU_PROPERTY_X86_ISA_1_SSE, "SSE");
    DumpBit(GNU_PROPERTY_X86_ISA_1_SSE2, "SSE2");
    DumpBit(GNU_PROPERTY_X86_ISA_1_SSE3, "SSE3");
    DumpBit(GNU_PROPERTY_X86_ISA_1_SSSE3, "SSSE3");
    DumpBit(GNU_PROPERTY_X86_ISA_1_SSE4_1, "SSE4_1");
    DumpBit(GNU_PROPERTY_X86_ISA_1_SSE4_2, "SSE4_2");
    DumpBit(GNU_PROPERTY_X86_ISA_1_AVX, "AVX");
    DumpBit(GNU_PROPERTY_X86_ISA_1_AVX2, "AVX2");
    DumpBit(GNU_PROPERTY_X86_ISA_1_FMA, "FMA");
    DumpBit(GNU_PROPERTY_X86_ISA_1_AVX512F, "AVX512F");
    DumpBit(GNU_PROPERTY_X86_ISA_1_AVX512CD, "AVX512CD");
    DumpBit(GNU_PROPERTY_X86_ISA_1_AVX512ER, "AVX512ER");
    DumpBit(GNU_PROPERTY_X86_ISA_1_AVX512PF, "AVX512PF");
    DumpBit(GNU_PROPERTY_X86_ISA_1_AVX512VL, "AVX512VL");
    DumpBit(GNU_PROPERTY_X86_ISA_1_AVX512DQ, "AVX512DQ");
    DumpBit(GNU_PROPERTY_X86_ISA_1_AVX512BW, "AVX512BW");
    DumpBit(GNU_PROPERTY_X86_ISA_1_AVX512_4FMAPS, "AVX512_4FMAPS");
    DumpBit(GNU_PROPERTY_X86_ISA_1_AVX512_4VNNIW, "AVX512_4VNNIW");
    DumpBit(GNU_PROPERTY_X86_ISA_1_AVX512_BITALG, "AVX512_BITALG");
    DumpBit(GNU_PROPERTY_X86_ISA_1_AVX512_IFMA, "AVX512_IFMA");
    DumpBit(GNU_PROPERTY_X86_ISA_1_AVX512_VBMI, "AVX512_VBMI");
    DumpBit(GNU_PROPERTY_X86_ISA_1_AVX512_VBMI2, "AVX512_VBMI2");
    DumpBit(GNU_PROPERTY_X86_ISA_1_AVX512_VNNI, "AVX512_VNNI");
    if (PrData)
      OS << format("<unknown flags: 0x%x>", PrData);
    return OS.str();
    break;
  case GNU_PROPERTY_X86_FEATURE_2_NEEDED:
  case GNU_PROPERTY_X86_FEATURE_2_USED:
    OS << "x86 feature "
       << (Type == GNU_PROPERTY_X86_FEATURE_2_NEEDED ? "needed: " : "used: ");
    if (DataSize != 4) {
      OS << format("<corrupt length: 0x%x>", DataSize);
      return OS.str();
    }
    PrData = support::endian::read32<ELFT::TargetEndianness>(Data.data());
    if (PrData == 0) {
      OS << "<None>";
      return OS.str();
    }
    DumpBit(GNU_PROPERTY_X86_FEATURE_2_X86, "x86");
    DumpBit(GNU_PROPERTY_X86_FEATURE_2_X87, "x87");
    DumpBit(GNU_PROPERTY_X86_FEATURE_2_MMX, "MMX");
    DumpBit(GNU_PROPERTY_X86_FEATURE_2_XMM, "XMM");
    DumpBit(GNU_PROPERTY_X86_FEATURE_2_YMM, "YMM");
    DumpBit(GNU_PROPERTY_X86_FEATURE_2_ZMM, "ZMM");
    DumpBit(GNU_PROPERTY_X86_FEATURE_2_FXSR, "FXSR");
    DumpBit(GNU_PROPERTY_X86_FEATURE_2_XSAVE, "XSAVE");
    DumpBit(GNU_PROPERTY_X86_FEATURE_2_XSAVEOPT, "XSAVEOPT");
    DumpBit(GNU_PROPERTY_X86_FEATURE_2_XSAVEC, "XSAVEC");
    if (PrData)
      OS << format("<unknown flags: 0x%x>", PrData);
    return OS.str();
  }
}

template <typename ELFT>
static SmallVector<std::string, 4> getGNUPropertyList(ArrayRef<uint8_t> Arr) {
  using Elf_Word = typename ELFT::Word;

  SmallVector<std::string, 4> Properties;
  while (Arr.size() >= 8) {
    uint32_t Type = *reinterpret_cast<const Elf_Word *>(Arr.data());
    uint32_t DataSize = *reinterpret_cast<const Elf_Word *>(Arr.data() + 4);
    Arr = Arr.drop_front(8);

    // Take padding size into account if present.
    uint64_t PaddedSize = alignTo(DataSize, sizeof(typename ELFT::uint));
    std::string str;
    raw_string_ostream OS(str);
    if (Arr.size() < PaddedSize) {
      OS << format("<corrupt type (0x%x) datasz: 0x%x>", Type, DataSize);
      Properties.push_back(OS.str());
      break;
    }
    Properties.push_back(
        getGNUProperty<ELFT>(Type, DataSize, Arr.take_front(PaddedSize)));
    Arr = Arr.drop_front(PaddedSize);
  }

  if (!Arr.empty())
    Properties.push_back("<corrupted GNU_PROPERTY_TYPE_0>");

  return Properties;
}

struct GNUAbiTag {
  std::string OSName;
  std::string ABI;
  bool IsValid;
};

template <typename ELFT> static GNUAbiTag getGNUAbiTag(ArrayRef<uint8_t> Desc) {
  typedef typename ELFT::Word Elf_Word;

  ArrayRef<Elf_Word> Words(reinterpret_cast<const Elf_Word *>(Desc.begin()),
                           reinterpret_cast<const Elf_Word *>(Desc.end()));

  if (Words.size() < 4)
    return {"", "", /*IsValid=*/false};

  static const char *OSNames[] = {
      "Linux", "Hurd", "Solaris", "FreeBSD", "NetBSD", "Syllable", "NaCl",
  };
  StringRef OSName = "Unknown";
  if (Words[0] < array_lengthof(OSNames))
    OSName = OSNames[Words[0]];
  uint32_t Major = Words[1], Minor = Words[2], Patch = Words[3];
  std::string str;
  raw_string_ostream ABI(str);
  ABI << Major << "." << Minor << "." << Patch;
  return {OSName, ABI.str(), /*IsValid=*/true};
}

static std::string getGNUBuildId(ArrayRef<uint8_t> Desc) {
  std::string str;
  raw_string_ostream OS(str);
  for (const auto &B : Desc)
    OS << format_hex_no_prefix(B, 2);
  return OS.str();
}

static StringRef getGNUGoldVersion(ArrayRef<uint8_t> Desc) {
  return StringRef(reinterpret_cast<const char *>(Desc.data()), Desc.size());
}

template <typename ELFT>
static void printGNUNote(raw_ostream &OS, uint32_t NoteType,
                         ArrayRef<uint8_t> Desc) {
  switch (NoteType) {
  default:
    return;
  case ELF::NT_GNU_ABI_TAG: {
    const GNUAbiTag &AbiTag = getGNUAbiTag<ELFT>(Desc);
    if (!AbiTag.IsValid)
      OS << "    <corrupt GNU_ABI_TAG>";
    else
      OS << "    OS: " << AbiTag.OSName << ", ABI: " << AbiTag.ABI;
    break;
  }
  case ELF::NT_GNU_BUILD_ID: {
    OS << "    Build ID: " << getGNUBuildId(Desc);
    break;
  }
  case ELF::NT_GNU_GOLD_VERSION:
    OS << "    Version: " << getGNUGoldVersion(Desc);
    break;
  case ELF::NT_GNU_PROPERTY_TYPE_0:
    OS << "    Properties:";
    for (const auto &Property : getGNUPropertyList<ELFT>(Desc))
      OS << "    " << Property << "\n";
    break;
  }
  OS << '\n';
}

struct AMDNote {
  std::string Type;
  std::string Value;
};

template <typename ELFT>
static AMDNote getAMDNote(uint32_t NoteType, ArrayRef<uint8_t> Desc) {
  switch (NoteType) {
  default:
    return {"", ""};
  case ELF::NT_AMD_AMDGPU_HSA_METADATA:
    return {
        "HSA Metadata",
        std::string(reinterpret_cast<const char *>(Desc.data()), Desc.size())};
  case ELF::NT_AMD_AMDGPU_ISA:
    return {
        "ISA Version",
        std::string(reinterpret_cast<const char *>(Desc.data()), Desc.size())};
  }
}

struct AMDGPUNote {
  std::string Type;
  std::string Value;
};

template <typename ELFT>
static AMDGPUNote getAMDGPUNote(uint32_t NoteType, ArrayRef<uint8_t> Desc) {
  switch (NoteType) {
  default:
    return {"", ""};
  case ELF::NT_AMDGPU_METADATA: {
    auto MsgPackString =
        StringRef(reinterpret_cast<const char *>(Desc.data()), Desc.size());
    msgpack::Document MsgPackDoc;
    if (!MsgPackDoc.readFromBlob(MsgPackString, /*Multi=*/false))
      return {"AMDGPU Metadata", "Invalid AMDGPU Metadata"};

    AMDGPU::HSAMD::V3::MetadataVerifier Verifier(true);
    if (!Verifier.verify(MsgPackDoc.getRoot()))
      return {"AMDGPU Metadata", "Invalid AMDGPU Metadata"};

    std::string HSAMetadataString;
    raw_string_ostream StrOS(HSAMetadataString);
    MsgPackDoc.toYAML(StrOS);

    return {"AMDGPU Metadata", StrOS.str()};
  }
  }
}

template <class ELFT>
void GNUStyle<ELFT>::printNotes(const ELFFile<ELFT> *Obj) {
  auto PrintHeader = [&](const typename ELFT::Off Offset,
                         const typename ELFT::Addr Size) {
    OS << "Displaying notes found at file offset " << format_hex(Offset, 10)
       << " with length " << format_hex(Size, 10) << ":\n"
       << "  Owner                 Data size\tDescription\n";
  };

  auto ProcessNote = [&](const Elf_Note &Note) {
    StringRef Name = Note.getName();
    ArrayRef<uint8_t> Descriptor = Note.getDesc();
    Elf_Word Type = Note.getType();

    OS << "  " << Name << std::string(22 - Name.size(), ' ')
       << format_hex(Descriptor.size(), 10) << '\t';

    if (Name == "GNU") {
      OS << getGNUNoteTypeName(Type) << '\n';
      printGNUNote<ELFT>(OS, Type, Descriptor);
    } else if (Name == "FreeBSD") {
      OS << getFreeBSDNoteTypeName(Type) << '\n';
    } else if (Name == "AMD") {
      OS << getAMDNoteTypeName(Type) << '\n';
      const AMDNote N = getAMDNote<ELFT>(Type, Descriptor);
      if (!N.Type.empty())
        OS << "    " << N.Type << ":\n        " << N.Value << '\n';
    } else if (Name == "AMDGPU") {
      OS << getAMDGPUNoteTypeName(Type) << '\n';
      const AMDGPUNote N = getAMDGPUNote<ELFT>(Type, Descriptor);
      if (!N.Type.empty())
        OS << "    " << N.Type << ":\n        " << N.Value << '\n';
    } else {
      StringRef NoteType = getGenericNoteTypeName(Type);
      if (!NoteType.empty())
        OS << NoteType;
      else
        OS << "Unknown note type: (" << format_hex(Type, 10) << ')';
    }
    OS << '\n';
  };

  if (Obj->getHeader()->e_type == ELF::ET_CORE) {
    for (const auto &P : unwrapOrError(Obj->program_headers())) {
      if (P.p_type != PT_NOTE)
        continue;
      PrintHeader(P.p_offset, P.p_filesz);
      Error Err = Error::success();
      for (const auto &Note : Obj->notes(P, Err))
        ProcessNote(Note);
      if (Err)
        error(std::move(Err));
    }
  } else {
    for (const auto &S : unwrapOrError(Obj->sections())) {
      if (S.sh_type != SHT_NOTE)
        continue;
      PrintHeader(S.sh_offset, S.sh_size);
      Error Err = Error::success();
      for (const auto &Note : Obj->notes(S, Err))
        ProcessNote(Note);
      if (Err)
        error(std::move(Err));
    }
  }
}

template <class ELFT>
void GNUStyle<ELFT>::printELFLinkerOptions(const ELFFile<ELFT> *Obj) {
  OS << "printELFLinkerOptions not implemented!\n";
}

template <class ELFT>
void GNUStyle<ELFT>::printMipsGOT(const MipsGOTParser<ELFT> &Parser) {
  size_t Bias = ELFT::Is64Bits ? 8 : 0;
  auto PrintEntry = [&](const Elf_Addr *E, StringRef Purpose) {
    OS.PadToColumn(2);
    OS << format_hex_no_prefix(Parser.getGotAddress(E), 8 + Bias);
    OS.PadToColumn(11 + Bias);
    OS << format_decimal(Parser.getGotOffset(E), 6) << "(gp)";
    OS.PadToColumn(22 + Bias);
    OS << format_hex_no_prefix(*E, 8 + Bias);
    OS.PadToColumn(31 + 2 * Bias);
    OS << Purpose << "\n";
  };

  OS << (Parser.IsStatic ? "Static GOT:\n" : "Primary GOT:\n");
  OS << " Canonical gp value: "
     << format_hex_no_prefix(Parser.getGp(), 8 + Bias) << "\n\n";

  OS << " Reserved entries:\n";
  if (ELFT::Is64Bits)
    OS << "           Address     Access          Initial Purpose\n";
  else
    OS << "   Address     Access  Initial Purpose\n";
  PrintEntry(Parser.getGotLazyResolver(), "Lazy resolver");
  if (Parser.getGotModulePointer())
    PrintEntry(Parser.getGotModulePointer(), "Module pointer (GNU extension)");

  if (!Parser.getLocalEntries().empty()) {
    OS << "\n";
    OS << " Local entries:\n";
    if (ELFT::Is64Bits)
      OS << "           Address     Access          Initial\n";
    else
      OS << "   Address     Access  Initial\n";
    for (auto &E : Parser.getLocalEntries())
      PrintEntry(&E, "");
  }

  if (Parser.IsStatic)
    return;

  if (!Parser.getGlobalEntries().empty()) {
    OS << "\n";
    OS << " Global entries:\n";
    if (ELFT::Is64Bits)
      OS << "           Address     Access          Initial         Sym.Val."
         << " Type    Ndx Name\n";
    else
      OS << "   Address     Access  Initial Sym.Val. Type    Ndx Name\n";
    for (auto &E : Parser.getGlobalEntries()) {
      const Elf_Sym *Sym = Parser.getGotSym(&E);
      std::string SymName = this->dumper()->getFullSymbolName(
          Sym, this->dumper()->getDynamicStringTable(), false);

      OS.PadToColumn(2);
      OS << to_string(format_hex_no_prefix(Parser.getGotAddress(&E), 8 + Bias));
      OS.PadToColumn(11 + Bias);
      OS << to_string(format_decimal(Parser.getGotOffset(&E), 6)) + "(gp)";
      OS.PadToColumn(22 + Bias);
      OS << to_string(format_hex_no_prefix(E, 8 + Bias));
      OS.PadToColumn(31 + 2 * Bias);
      OS << to_string(format_hex_no_prefix(Sym->st_value, 8 + Bias));
      OS.PadToColumn(40 + 3 * Bias);
      OS << printEnum(Sym->getType(), makeArrayRef(ElfSymbolTypes));
      OS.PadToColumn(48 + 3 * Bias);
      OS << getSymbolSectionNdx(Parser.Obj, Sym,
                                this->dumper()->dynamic_symbols().begin());
      OS.PadToColumn(52 + 3 * Bias);
      OS << SymName << "\n";
    }
  }

  if (!Parser.getOtherEntries().empty())
    OS << "\n Number of TLS and multi-GOT entries "
       << Parser.getOtherEntries().size() << "\n";
}

template <class ELFT>
void GNUStyle<ELFT>::printMipsPLT(const MipsGOTParser<ELFT> &Parser) {
  size_t Bias = ELFT::Is64Bits ? 8 : 0;
  auto PrintEntry = [&](const Elf_Addr *E, StringRef Purpose) {
    OS.PadToColumn(2);
    OS << format_hex_no_prefix(Parser.getPltAddress(E), 8 + Bias);
    OS.PadToColumn(11 + Bias);
    OS << format_hex_no_prefix(*E, 8 + Bias);
    OS.PadToColumn(20 + 2 * Bias);
    OS << Purpose << "\n";
  };

  OS << "PLT GOT:\n\n";

  OS << " Reserved entries:\n";
  OS << "   Address  Initial Purpose\n";
  PrintEntry(Parser.getPltLazyResolver(), "PLT lazy resolver");
  if (Parser.getPltModulePointer())
    PrintEntry(Parser.getPltModulePointer(), "Module pointer");

  if (!Parser.getPltEntries().empty()) {
    OS << "\n";
    OS << " Entries:\n";
    OS << "   Address  Initial Sym.Val. Type    Ndx Name\n";
    for (auto &E : Parser.getPltEntries()) {
      const Elf_Sym *Sym = Parser.getPltSym(&E);
      std::string SymName = this->dumper()->getFullSymbolName(
          Sym, this->dumper()->getDynamicStringTable(), false);

      OS.PadToColumn(2);
      OS << to_string(format_hex_no_prefix(Parser.getPltAddress(&E), 8 + Bias));
      OS.PadToColumn(11 + Bias);
      OS << to_string(format_hex_no_prefix(E, 8 + Bias));
      OS.PadToColumn(20 + 2 * Bias);
      OS << to_string(format_hex_no_prefix(Sym->st_value, 8 + Bias));
      OS.PadToColumn(29 + 3 * Bias);
      OS << printEnum(Sym->getType(), makeArrayRef(ElfSymbolTypes));
      OS.PadToColumn(37 + 3 * Bias);
      OS << getSymbolSectionNdx(Parser.Obj, Sym,
                                this->dumper()->dynamic_symbols().begin());
      OS.PadToColumn(41 + 3 * Bias);
      OS << SymName << "\n";
    }
  }
}

template <class ELFT> void LLVMStyle<ELFT>::printFileHeaders(const ELFO *Obj) {
  const Elf_Ehdr *E = Obj->getHeader();
  {
    DictScope D(W, "ElfHeader");
    {
      DictScope D(W, "Ident");
      W.printBinary("Magic", makeArrayRef(E->e_ident).slice(ELF::EI_MAG0, 4));
      W.printEnum("Class", E->e_ident[ELF::EI_CLASS], makeArrayRef(ElfClass));
      W.printEnum("DataEncoding", E->e_ident[ELF::EI_DATA],
                  makeArrayRef(ElfDataEncoding));
      W.printNumber("FileVersion", E->e_ident[ELF::EI_VERSION]);

      auto OSABI = makeArrayRef(ElfOSABI);
      if (E->e_ident[ELF::EI_OSABI] >= ELF::ELFOSABI_FIRST_ARCH &&
          E->e_ident[ELF::EI_OSABI] <= ELF::ELFOSABI_LAST_ARCH) {
        switch (E->e_machine) {
        case ELF::EM_AMDGPU:
          OSABI = makeArrayRef(AMDGPUElfOSABI);
          break;
        case ELF::EM_ARM:
          OSABI = makeArrayRef(ARMElfOSABI);
          break;
        case ELF::EM_TI_C6000:
          OSABI = makeArrayRef(C6000ElfOSABI);
          break;
        }
      }
      W.printEnum("OS/ABI", E->e_ident[ELF::EI_OSABI], OSABI);
      W.printNumber("ABIVersion", E->e_ident[ELF::EI_ABIVERSION]);
      W.printBinary("Unused", makeArrayRef(E->e_ident).slice(ELF::EI_PAD));
    }

    W.printEnum("Type", E->e_type, makeArrayRef(ElfObjectFileType));
    W.printEnum("Machine", E->e_machine, makeArrayRef(ElfMachineType));
    W.printNumber("Version", E->e_version);
    W.printHex("Entry", E->e_entry);
    W.printHex("ProgramHeaderOffset", E->e_phoff);
    W.printHex("SectionHeaderOffset", E->e_shoff);
    if (E->e_machine == EM_MIPS)
      W.printFlags("Flags", E->e_flags, makeArrayRef(ElfHeaderMipsFlags),
                   unsigned(ELF::EF_MIPS_ARCH), unsigned(ELF::EF_MIPS_ABI),
                   unsigned(ELF::EF_MIPS_MACH));
    else if (E->e_machine == EM_AMDGPU)
      W.printFlags("Flags", E->e_flags, makeArrayRef(ElfHeaderAMDGPUFlags),
                   unsigned(ELF::EF_AMDGPU_MACH));
    else if (E->e_machine == EM_RISCV)
      W.printFlags("Flags", E->e_flags, makeArrayRef(ElfHeaderRISCVFlags));
    else
      W.printFlags("Flags", E->e_flags);
    W.printNumber("HeaderSize", E->e_ehsize);
    W.printNumber("ProgramHeaderEntrySize", E->e_phentsize);
    W.printNumber("ProgramHeaderCount", E->e_phnum);
    W.printNumber("SectionHeaderEntrySize", E->e_shentsize);
    W.printString("SectionHeaderCount", getSectionHeadersNumString(Obj));
    W.printString("StringTableSectionIndex",
                  getSectionHeaderTableIndexString(Obj));
  }
}

template <class ELFT>
void LLVMStyle<ELFT>::printGroupSections(const ELFO *Obj) {
  DictScope Lists(W, "Groups");
  std::vector<GroupSection> V = getGroups<ELFT>(Obj);
  DenseMap<uint64_t, const GroupSection *> Map = mapSectionsToGroups(V);
  for (const GroupSection &G : V) {
    DictScope D(W, "Group");
    W.printNumber("Name", G.Name, G.ShName);
    W.printNumber("Index", G.Index);
    W.printNumber("Link", G.Link);
    W.printNumber("Info", G.Info);
    W.printHex("Type", getGroupType(G.Type), G.Type);
    W.startLine() << "Signature: " << G.Signature << "\n";

    ListScope L(W, "Section(s) in group");
    for (const GroupMember &GM : G.Members) {
      const GroupSection *MainGroup = Map[GM.Index];
      if (MainGroup != &G) {
        W.flush();
        errs() << "Error: " << GM.Name << " (" << GM.Index
               << ") in a group " + G.Name + " (" << G.Index
               << ") is already in a group " + MainGroup->Name + " ("
               << MainGroup->Index << ")\n";
        errs().flush();
        continue;
      }
      W.startLine() << GM.Name << " (" << GM.Index << ")\n";
    }
  }

  if (V.empty())
    W.startLine() << "There are no group sections in the file.\n";
}

template <class ELFT> void LLVMStyle<ELFT>::printRelocations(const ELFO *Obj) {
  ListScope D(W, "Relocations");

  int SectionNumber = -1;
  for (const Elf_Shdr &Sec : unwrapOrError(Obj->sections())) {
    ++SectionNumber;

    if (Sec.sh_type != ELF::SHT_REL && Sec.sh_type != ELF::SHT_RELA &&
        Sec.sh_type != ELF::SHT_RELR && Sec.sh_type != ELF::SHT_ANDROID_REL &&
        Sec.sh_type != ELF::SHT_ANDROID_RELA &&
        Sec.sh_type != ELF::SHT_ANDROID_RELR)
      continue;

    StringRef Name = unwrapOrError(Obj->getSectionName(&Sec));

    W.startLine() << "Section (" << SectionNumber << ") " << Name << " {\n";
    W.indent();

    printRelocations(&Sec, Obj);

    W.unindent();
    W.startLine() << "}\n";
  }
}

template <class ELFT>
void LLVMStyle<ELFT>::printRelocations(const Elf_Shdr *Sec, const ELFO *Obj) {
  const Elf_Shdr *SymTab = unwrapOrError(Obj->getSection(Sec->sh_link));

  switch (Sec->sh_type) {
  case ELF::SHT_REL:
    for (const Elf_Rel &R : unwrapOrError(Obj->rels(Sec))) {
      Elf_Rela Rela;
      Rela.r_offset = R.r_offset;
      Rela.r_info = R.r_info;
      Rela.r_addend = 0;
      printRelocation(Obj, Rela, SymTab);
    }
    break;
  case ELF::SHT_RELA:
    for (const Elf_Rela &R : unwrapOrError(Obj->relas(Sec)))
      printRelocation(Obj, R, SymTab);
    break;
  case ELF::SHT_RELR:
  case ELF::SHT_ANDROID_RELR: {
    Elf_Relr_Range Relrs = unwrapOrError(Obj->relrs(Sec));
    if (opts::RawRelr) {
      for (const Elf_Relr &R : Relrs)
        W.startLine() << W.hex(R) << "\n";
    } else {
      std::vector<Elf_Rela> RelrRelas = unwrapOrError(Obj->decode_relrs(Relrs));
      for (const Elf_Rela &R : RelrRelas)
        printRelocation(Obj, R, SymTab);
    }
    break;
  }
  case ELF::SHT_ANDROID_REL:
  case ELF::SHT_ANDROID_RELA:
    for (const Elf_Rela &R : unwrapOrError(Obj->android_relas(Sec)))
      printRelocation(Obj, R, SymTab);
    break;
  }
}

template <class ELFT>
void LLVMStyle<ELFT>::printRelocation(const ELFO *Obj, Elf_Rela Rel,
                                      const Elf_Shdr *SymTab) {
  SmallString<32> RelocName;
  Obj->getRelocationTypeName(Rel.getType(Obj->isMips64EL()), RelocName);
  std::string TargetName;
  const Elf_Sym *Sym = unwrapOrError(Obj->getRelocationSymbol(&Rel, SymTab));
  if (Sym && Sym->getType() == ELF::STT_SECTION) {
    const Elf_Shdr *Sec = unwrapOrError(
        Obj->getSection(Sym, SymTab, this->dumper()->getShndxTable()));
    TargetName = unwrapOrError(Obj->getSectionName(Sec));
  } else if (Sym) {
    StringRef StrTable = unwrapOrError(Obj->getStringTableForSymtab(*SymTab));
    TargetName = this->dumper()->getFullSymbolName(
        Sym, StrTable, SymTab->sh_type == SHT_DYNSYM /* IsDynamic */);
  }

  if (opts::ExpandRelocs) {
    DictScope Group(W, "Relocation");
    W.printHex("Offset", Rel.r_offset);
    W.printNumber("Type", RelocName, (int)Rel.getType(Obj->isMips64EL()));
    W.printNumber("Symbol", !TargetName.empty() ? TargetName : "-",
                  Rel.getSymbol(Obj->isMips64EL()));
    W.printHex("Addend", Rel.r_addend);
  } else {
    raw_ostream &OS = W.startLine();
    OS << W.hex(Rel.r_offset) << " " << RelocName << " "
       << (!TargetName.empty() ? TargetName : "-") << " " << W.hex(Rel.r_addend)
       << "\n";
  }
}

template <class ELFT>
void LLVMStyle<ELFT>::printSectionHeaders(const ELFO *Obj) {
  ListScope SectionsD(W, "Sections");

  int SectionIndex = -1;
  ArrayRef<Elf_Shdr> Sections = unwrapOrError(Obj->sections());
  const ELFObjectFile<ELFT> *ElfObj = this->dumper()->getElfObject();
  for (const Elf_Shdr &Sec : Sections) {
    StringRef Name = getSectionName(Sec, *ElfObj, Sections);
    DictScope SectionD(W, "Section");
    W.printNumber("Index", ++SectionIndex);
    W.printNumber("Name", Name, Sec.sh_name);
    W.printHex(
        "Type",
        object::getELFSectionTypeName(Obj->getHeader()->e_machine, Sec.sh_type),
        Sec.sh_type);
    std::vector<EnumEntry<unsigned>> SectionFlags(std::begin(ElfSectionFlags),
                                                  std::end(ElfSectionFlags));
    switch (Obj->getHeader()->e_machine) {
    case EM_ARM:
      SectionFlags.insert(SectionFlags.end(), std::begin(ElfARMSectionFlags),
                          std::end(ElfARMSectionFlags));
      break;
    case EM_HEXAGON:
      SectionFlags.insert(SectionFlags.end(),
                          std::begin(ElfHexagonSectionFlags),
                          std::end(ElfHexagonSectionFlags));
      break;
    case EM_MIPS:
      SectionFlags.insert(SectionFlags.end(), std::begin(ElfMipsSectionFlags),
                          std::end(ElfMipsSectionFlags));
      break;
    case EM_X86_64:
      SectionFlags.insert(SectionFlags.end(), std::begin(ElfX86_64SectionFlags),
                          std::end(ElfX86_64SectionFlags));
      break;
    case EM_XCORE:
      SectionFlags.insert(SectionFlags.end(), std::begin(ElfXCoreSectionFlags),
                          std::end(ElfXCoreSectionFlags));
      break;
    default:
      // Nothing to do.
      break;
    }
    W.printFlags("Flags", Sec.sh_flags, makeArrayRef(SectionFlags));
    W.printHex("Address", Sec.sh_addr);
    W.printHex("Offset", Sec.sh_offset);
    W.printNumber("Size", Sec.sh_size);
    W.printNumber("Link", Sec.sh_link);
    W.printNumber("Info", Sec.sh_info);
    W.printNumber("AddressAlignment", Sec.sh_addralign);
    W.printNumber("EntrySize", Sec.sh_entsize);

    if (opts::SectionRelocations) {
      ListScope D(W, "Relocations");
      printRelocations(&Sec, Obj);
    }

    if (opts::SectionSymbols) {
      ListScope D(W, "Symbols");
      const Elf_Shdr *Symtab = this->dumper()->getDotSymtabSec();
      StringRef StrTable = unwrapOrError(Obj->getStringTableForSymtab(*Symtab));

      for (const Elf_Sym &Sym : unwrapOrError(Obj->symbols(Symtab))) {
        const Elf_Shdr *SymSec = unwrapOrError(
            Obj->getSection(&Sym, Symtab, this->dumper()->getShndxTable()));
        if (SymSec == &Sec)
          printSymbol(Obj, &Sym, unwrapOrError(Obj->symbols(Symtab)).begin(),
                      StrTable, false);
      }
    }

    if (opts::SectionData && Sec.sh_type != ELF::SHT_NOBITS) {
      ArrayRef<uint8_t> Data = unwrapOrError(Obj->getSectionContents(&Sec));
      W.printBinaryBlock(
          "SectionData",
          StringRef(reinterpret_cast<const char *>(Data.data()), Data.size()));
    }
  }
}

template <class ELFT>
void LLVMStyle<ELFT>::printSymbol(const ELFO *Obj, const Elf_Sym *Symbol,
                                  const Elf_Sym *First, StringRef StrTable,
                                  bool IsDynamic) {
  unsigned SectionIndex = 0;
  StringRef SectionName;
  this->dumper()->getSectionNameIndex(Symbol, First, SectionName, SectionIndex);
  std::string FullSymbolName =
      this->dumper()->getFullSymbolName(Symbol, StrTable, IsDynamic);
  unsigned char SymbolType = Symbol->getType();

  DictScope D(W, "Symbol");
  W.printNumber("Name", FullSymbolName, Symbol->st_name);
  W.printHex("Value", Symbol->st_value);
  W.printNumber("Size", Symbol->st_size);
  W.printEnum("Binding", Symbol->getBinding(), makeArrayRef(ElfSymbolBindings));
  if (Obj->getHeader()->e_machine == ELF::EM_AMDGPU &&
      SymbolType >= ELF::STT_LOOS && SymbolType < ELF::STT_HIOS)
    W.printEnum("Type", SymbolType, makeArrayRef(AMDGPUSymbolTypes));
  else
    W.printEnum("Type", SymbolType, makeArrayRef(ElfSymbolTypes));
  if (Symbol->st_other == 0)
    // Usually st_other flag is zero. Do not pollute the output
    // by flags enumeration in that case.
    W.printNumber("Other", 0);
  else {
    std::vector<EnumEntry<unsigned>> SymOtherFlags(std::begin(ElfSymOtherFlags),
                                                   std::end(ElfSymOtherFlags));
    if (Obj->getHeader()->e_machine == EM_MIPS) {
      // Someones in their infinite wisdom decided to make STO_MIPS_MIPS16
      // flag overlapped with other ST_MIPS_xxx flags. So consider both
      // cases separately.
      if ((Symbol->st_other & STO_MIPS_MIPS16) == STO_MIPS_MIPS16)
        SymOtherFlags.insert(SymOtherFlags.end(),
                             std::begin(ElfMips16SymOtherFlags),
                             std::end(ElfMips16SymOtherFlags));
      else
        SymOtherFlags.insert(SymOtherFlags.end(),
                             std::begin(ElfMipsSymOtherFlags),
                             std::end(ElfMipsSymOtherFlags));
    }
    W.printFlags("Other", Symbol->st_other, makeArrayRef(SymOtherFlags), 0x3u);
  }
  W.printHex("Section", SectionName, SectionIndex);
}

template <class ELFT>
void LLVMStyle<ELFT>::printSymbols(const ELFO *Obj, bool PrintSymbols,
                                   bool PrintDynamicSymbols) {
  if (PrintSymbols)
    printSymbols(Obj);
  if (PrintDynamicSymbols)
    printDynamicSymbols(Obj);
}

template <class ELFT> void LLVMStyle<ELFT>::printSymbols(const ELFO *Obj) {
  ListScope Group(W, "Symbols");
  this->dumper()->printSymbolsHelper(false);
}

template <class ELFT>
void LLVMStyle<ELFT>::printDynamicSymbols(const ELFO *Obj) {
  ListScope Group(W, "DynamicSymbols");
  this->dumper()->printSymbolsHelper(true);
}

template <class ELFT> void LLVMStyle<ELFT>::printDynamic(const ELFFile<ELFT> *Obj) {
  Elf_Dyn_Range Table = this->dumper()->dynamic_table();
  if (Table.empty())
    return;

  raw_ostream &OS = W.getOStream();
  W.startLine() << "DynamicSection [ (" << Table.size() << " entries)\n";

  bool Is64 = ELFT::Is64Bits;
  if (Is64)
    W.startLine() << "  Tag                Type                 Name/Value\n";
  else
    W.startLine() << "  Tag        Type                 Name/Value\n";
  for (auto Entry : Table) {
    uintX_t Tag = Entry.getTag();
    W.startLine() << "  " << format_hex(Tag, Is64 ? 18 : 10, true) << " "
                  << format("%-21s",
                            getTypeString(Obj->getHeader()->e_machine, Tag));
    this->dumper()->printDynamicEntry(OS, Tag, Entry.getVal());
    OS << "\n";
  }

  W.startLine() << "]\n";
}

template <class ELFT>
void LLVMStyle<ELFT>::printDynamicRelocations(const ELFO *Obj) {
  const DynRegionInfo &DynRelRegion = this->dumper()->getDynRelRegion();
  const DynRegionInfo &DynRelaRegion = this->dumper()->getDynRelaRegion();
  const DynRegionInfo &DynRelrRegion = this->dumper()->getDynRelrRegion();
  const DynRegionInfo &DynPLTRelRegion = this->dumper()->getDynPLTRelRegion();
  if (DynRelRegion.Size && DynRelaRegion.Size)
    report_fatal_error("There are both REL and RELA dynamic relocations");
  W.startLine() << "Dynamic Relocations {\n";
  W.indent();
  if (DynRelaRegion.Size > 0)
    for (const Elf_Rela &Rela : this->dumper()->dyn_relas())
      printDynamicRelocation(Obj, Rela);
  else
    for (const Elf_Rel &Rel : this->dumper()->dyn_rels()) {
      Elf_Rela Rela;
      Rela.r_offset = Rel.r_offset;
      Rela.r_info = Rel.r_info;
      Rela.r_addend = 0;
      printDynamicRelocation(Obj, Rela);
    }
  if (DynRelrRegion.Size > 0) {
    Elf_Relr_Range Relrs = this->dumper()->dyn_relrs();
    std::vector<Elf_Rela> RelrRelas = unwrapOrError(Obj->decode_relrs(Relrs));
    for (const Elf_Rela &Rela : RelrRelas)
      printDynamicRelocation(Obj, Rela);
  }
  if (DynPLTRelRegion.EntSize == sizeof(Elf_Rela))
    for (const Elf_Rela &Rela : DynPLTRelRegion.getAsArrayRef<Elf_Rela>())
      printDynamicRelocation(Obj, Rela);
  else
    for (const Elf_Rel &Rel : DynPLTRelRegion.getAsArrayRef<Elf_Rel>()) {
      Elf_Rela Rela;
      Rela.r_offset = Rel.r_offset;
      Rela.r_info = Rel.r_info;
      Rela.r_addend = 0;
      printDynamicRelocation(Obj, Rela);
    }
  W.unindent();
  W.startLine() << "}\n";
}

template <class ELFT>
void LLVMStyle<ELFT>::printDynamicRelocation(const ELFO *Obj, Elf_Rela Rel) {
  SmallString<32> RelocName;
  Obj->getRelocationTypeName(Rel.getType(Obj->isMips64EL()), RelocName);
  std::string SymbolName;
  uint32_t SymIndex = Rel.getSymbol(Obj->isMips64EL());
  const Elf_Sym *Sym = this->dumper()->dynamic_symbols().begin() + SymIndex;
  SymbolName = maybeDemangle(
      unwrapOrError(Sym->getName(this->dumper()->getDynamicStringTable())));
  if (opts::ExpandRelocs) {
    DictScope Group(W, "Relocation");
    W.printHex("Offset", Rel.r_offset);
    W.printNumber("Type", RelocName, (int)Rel.getType(Obj->isMips64EL()));
    W.printString("Symbol", !SymbolName.empty() ? SymbolName : "-");
    W.printHex("Addend", Rel.r_addend);
  } else {
    raw_ostream &OS = W.startLine();
    OS << W.hex(Rel.r_offset) << " " << RelocName << " "
       << (!SymbolName.empty() ? SymbolName : "-") << " " << W.hex(Rel.r_addend)
       << "\n";
  }
}

template <class ELFT>
void LLVMStyle<ELFT>::printProgramHeaders(
    const ELFO *Obj, bool PrintProgramHeaders,
    cl::boolOrDefault PrintSectionMapping) {
  if (PrintProgramHeaders)
    printProgramHeaders(Obj);
  if (PrintSectionMapping == cl::BOU_TRUE)
    printSectionMapping(Obj);
}

template <class ELFT>
void LLVMStyle<ELFT>::printProgramHeaders(const ELFO *Obj) {
  ListScope L(W, "ProgramHeaders");

  for (const Elf_Phdr &Phdr : unwrapOrError(Obj->program_headers())) {
    DictScope P(W, "ProgramHeader");
    W.printHex("Type",
               getElfSegmentType(Obj->getHeader()->e_machine, Phdr.p_type),
               Phdr.p_type);
    W.printHex("Offset", Phdr.p_offset);
    W.printHex("VirtualAddress", Phdr.p_vaddr);
    W.printHex("PhysicalAddress", Phdr.p_paddr);
    W.printNumber("FileSize", Phdr.p_filesz);
    W.printNumber("MemSize", Phdr.p_memsz);
    W.printFlags("Flags", Phdr.p_flags, makeArrayRef(ElfSegmentFlags));
    W.printNumber("Alignment", Phdr.p_align);
  }
}

template <class ELFT>
void LLVMStyle<ELFT>::printVersionSymbolSection(const ELFFile<ELFT> *Obj,
                                                const Elf_Shdr *Sec) {
  DictScope SS(W, "Version symbols");
  if (!Sec)
    return;

  StringRef SecName = unwrapOrError(Obj->getSectionName(Sec));
  W.printNumber("Section Name", SecName, Sec->sh_name);
  W.printHex("Address", Sec->sh_addr);
  W.printHex("Offset", Sec->sh_offset);
  W.printNumber("Link", Sec->sh_link);

  const uint8_t *VersymBuf =
      reinterpret_cast<const uint8_t *>(Obj->base() + Sec->sh_offset);
  const ELFDumper<ELFT> *Dumper = this->dumper();
  StringRef StrTable = Dumper->getDynamicStringTable();

  // Same number of entries in the dynamic symbol table (DT_SYMTAB).
  ListScope Syms(W, "Symbols");
  for (const Elf_Sym &Sym : Dumper->dynamic_symbols()) {
    DictScope S(W, "Symbol");
    const Elf_Versym *Versym = reinterpret_cast<const Elf_Versym *>(VersymBuf);
    std::string FullSymbolName =
        Dumper->getFullSymbolName(&Sym, StrTable, true /* IsDynamic */);
    W.printNumber("Version", Versym->vs_index & VERSYM_VERSION);
    W.printString("Name", FullSymbolName);
    VersymBuf += sizeof(Elf_Versym);
  }
}

template <class ELFT>
void LLVMStyle<ELFT>::printVersionDefinitionSection(const ELFFile<ELFT> *Obj,
                                                    const Elf_Shdr *Sec) {
  DictScope SD(W, "SHT_GNU_verdef");
  if (!Sec)
    return;

  const uint8_t *SecStartAddress =
      reinterpret_cast<const uint8_t *>(Obj->base() + Sec->sh_offset);
  const uint8_t *SecEndAddress = SecStartAddress + Sec->sh_size;
  const uint8_t *VerdefBuf = SecStartAddress;
  const Elf_Shdr *StrTab = unwrapOrError(Obj->getSection(Sec->sh_link));

  unsigned VerDefsNum = Sec->sh_info;
  while (VerDefsNum--) {
    if (VerdefBuf + sizeof(Elf_Verdef) > SecEndAddress)
      // FIXME: report_fatal_error is not a good way to report error. We should
      // emit a parsing error here and below.
      report_fatal_error("invalid offset in the section");

    const Elf_Verdef *Verdef = reinterpret_cast<const Elf_Verdef *>(VerdefBuf);
    DictScope Def(W, "Definition");
    W.printNumber("Version", Verdef->vd_version);
    W.printEnum("Flags", Verdef->vd_flags, makeArrayRef(SymVersionFlags));
    W.printNumber("Index", Verdef->vd_ndx);
    W.printNumber("Hash", Verdef->vd_hash);
    W.printString("Name", StringRef(reinterpret_cast<const char *>(
                              Obj->base() + StrTab->sh_offset +
                              Verdef->getAux()->vda_name)));
    if (!Verdef->vd_cnt)
      report_fatal_error("at least one definition string must exist");
    if (Verdef->vd_cnt > 2)
      report_fatal_error("more than one predecessor is not expected");

    if (Verdef->vd_cnt == 2) {
      const uint8_t *VerdauxBuf =
          VerdefBuf + Verdef->vd_aux + Verdef->getAux()->vda_next;
      const Elf_Verdaux *Verdaux =
          reinterpret_cast<const Elf_Verdaux *>(VerdauxBuf);
      W.printString("Predecessor",
                    StringRef(reinterpret_cast<const char *>(
                        Obj->base() + StrTab->sh_offset + Verdaux->vda_name)));
    }
    VerdefBuf += Verdef->vd_next;
  }
}

template <class ELFT>
void LLVMStyle<ELFT>::printVersionDependencySection(const ELFFile<ELFT> *Obj,
                                                    const Elf_Shdr *Sec) {
  DictScope SD(W, "SHT_GNU_verneed");
  if (!Sec)
    return;

  const uint8_t *SecData =
      reinterpret_cast<const uint8_t *>(Obj->base() + Sec->sh_offset);
  const Elf_Shdr *StrTab = unwrapOrError(Obj->getSection(Sec->sh_link));

  const uint8_t *VerneedBuf = SecData;
  unsigned VerneedNum = Sec->sh_info;
  for (unsigned I = 0; I < VerneedNum; ++I) {
    const Elf_Verneed *Verneed =
        reinterpret_cast<const Elf_Verneed *>(VerneedBuf);
    DictScope Entry(W, "Dependency");
    W.printNumber("Version", Verneed->vn_version);
    W.printNumber("Count", Verneed->vn_cnt);
    W.printString("FileName",
                  StringRef(reinterpret_cast<const char *>(
                      Obj->base() + StrTab->sh_offset + Verneed->vn_file)));

    const uint8_t *VernauxBuf = VerneedBuf + Verneed->vn_aux;
    ListScope L(W, "Entries");
    for (unsigned J = 0; J < Verneed->vn_cnt; ++J) {
      const Elf_Vernaux *Vernaux =
          reinterpret_cast<const Elf_Vernaux *>(VernauxBuf);
      DictScope Entry(W, "Entry");
      W.printNumber("Hash", Vernaux->vna_hash);
      W.printEnum("Flags", Vernaux->vna_flags, makeArrayRef(SymVersionFlags));
      W.printNumber("Index", Vernaux->vna_other);
      W.printString("Name",
                    StringRef(reinterpret_cast<const char *>(
                        Obj->base() + StrTab->sh_offset + Vernaux->vna_name)));
      VernauxBuf += Vernaux->vna_next;
    }
    VerneedBuf += Verneed->vn_next;
  }
}

template <class ELFT>
void LLVMStyle<ELFT>::printHashHistogram(const ELFFile<ELFT> *Obj) {
  W.startLine() << "Hash Histogram not implemented!\n";
}

template <class ELFT>
void LLVMStyle<ELFT>::printCGProfile(const ELFFile<ELFT> *Obj) {
  ListScope L(W, "CGProfile");
  if (!this->dumper()->getDotCGProfileSec())
    return;
  auto CGProfile =
      unwrapOrError(Obj->template getSectionContentsAsArray<Elf_CGProfile>(
          this->dumper()->getDotCGProfileSec()));
  for (const Elf_CGProfile &CGPE : CGProfile) {
    DictScope D(W, "CGProfileEntry");
    W.printNumber("From", this->dumper()->getStaticSymbolName(CGPE.cgp_from),
                  CGPE.cgp_from);
    W.printNumber("To", this->dumper()->getStaticSymbolName(CGPE.cgp_to),
                  CGPE.cgp_to);
    W.printNumber("Weight", CGPE.cgp_weight);
  }
}

template <class ELFT>
void LLVMStyle<ELFT>::printAddrsig(const ELFFile<ELFT> *Obj) {
  ListScope L(W, "Addrsig");
  if (!this->dumper()->getDotAddrsigSec())
    return;
  ArrayRef<uint8_t> Contents = unwrapOrError(
      Obj->getSectionContents(this->dumper()->getDotAddrsigSec()));
  const uint8_t *Cur = Contents.begin();
  const uint8_t *End = Contents.end();
  while (Cur != End) {
    unsigned Size;
    const char *Err;
    uint64_t SymIndex = decodeULEB128(Cur, &Size, End, &Err);
    if (Err)
      reportError(Err);
    W.printNumber("Sym", this->dumper()->getStaticSymbolName(SymIndex),
                  SymIndex);
    Cur += Size;
  }
}

template <typename ELFT>
static void printGNUNoteLLVMStyle(uint32_t NoteType, ArrayRef<uint8_t> Desc,
                                  ScopedPrinter &W) {
  switch (NoteType) {
  default:
    return;
  case ELF::NT_GNU_ABI_TAG: {
    const GNUAbiTag &AbiTag = getGNUAbiTag<ELFT>(Desc);
    if (!AbiTag.IsValid) {
      W.printString("ABI", "<corrupt GNU_ABI_TAG>");
    } else {
      W.printString("OS", AbiTag.OSName);
      W.printString("ABI", AbiTag.ABI);
    }
    break;
  }
  case ELF::NT_GNU_BUILD_ID: {
    W.printString("Build ID", getGNUBuildId(Desc));
    break;
  }
  case ELF::NT_GNU_GOLD_VERSION:
    W.printString("Version", getGNUGoldVersion(Desc));
    break;
  case ELF::NT_GNU_PROPERTY_TYPE_0:
    ListScope D(W, "Property");
    for (const auto &Property : getGNUPropertyList<ELFT>(Desc))
      W.printString(Property);
    break;
  }
}

template <class ELFT>
void LLVMStyle<ELFT>::printNotes(const ELFFile<ELFT> *Obj) {
  ListScope L(W, "Notes");

  auto PrintHeader = [&](const typename ELFT::Off Offset,
                         const typename ELFT::Addr Size) {
    W.printHex("Offset", Offset);
    W.printHex("Size", Size);
  };

  auto ProcessNote = [&](const Elf_Note &Note) {
    DictScope D2(W, "Note");
    StringRef Name = Note.getName();
    ArrayRef<uint8_t> Descriptor = Note.getDesc();
    Elf_Word Type = Note.getType();

    W.printString("Owner", Name);
    W.printHex("Data size", Descriptor.size());
    if (Name == "GNU") {
      W.printString("Type", getGNUNoteTypeName(Type));
      printGNUNoteLLVMStyle<ELFT>(Type, Descriptor, W);
    } else if (Name == "FreeBSD") {
      W.printString("Type", getFreeBSDNoteTypeName(Type));
    } else if (Name == "AMD") {
      W.printString("Type", getAMDNoteTypeName(Type));
      const AMDNote N = getAMDNote<ELFT>(Type, Descriptor);
      if (!N.Type.empty())
        W.printString(N.Type, N.Value);
    } else if (Name == "AMDGPU") {
      W.printString("Type", getAMDGPUNoteTypeName(Type));
      const AMDGPUNote N = getAMDGPUNote<ELFT>(Type, Descriptor);
      if (!N.Type.empty())
        W.printString(N.Type, N.Value);
    } else {
      StringRef NoteType = getGenericNoteTypeName(Type);
      if (!NoteType.empty())
        W.printString("Type", NoteType);
      else
        W.printString("Type",
                      "Unknown (" + to_string(format_hex(Type, 10)) + ")");
    }
  };

  if (Obj->getHeader()->e_type == ELF::ET_CORE) {
    for (const auto &P : unwrapOrError(Obj->program_headers())) {
      if (P.p_type != PT_NOTE)
        continue;
      DictScope D(W, "NoteSection");
      PrintHeader(P.p_offset, P.p_filesz);
      Error Err = Error::success();
      for (const auto &Note : Obj->notes(P, Err))
        ProcessNote(Note);
      if (Err)
        error(std::move(Err));
    }
  } else {
    for (const auto &S : unwrapOrError(Obj->sections())) {
      if (S.sh_type != SHT_NOTE)
        continue;
      DictScope D(W, "NoteSection");
      PrintHeader(S.sh_offset, S.sh_size);
      Error Err = Error::success();
      for (const auto &Note : Obj->notes(S, Err))
        ProcessNote(Note);
      if (Err)
        error(std::move(Err));
    }
  }
}

template <class ELFT>
void LLVMStyle<ELFT>::printELFLinkerOptions(const ELFFile<ELFT> *Obj) {
  ListScope L(W, "LinkerOptions");

  for (const Elf_Shdr &Shdr : unwrapOrError(Obj->sections())) {
    if (Shdr.sh_type != ELF::SHT_LLVM_LINKER_OPTIONS)
      continue;

    ArrayRef<uint8_t> Contents = unwrapOrError(Obj->getSectionContents(&Shdr));
    for (const uint8_t *P = Contents.begin(), *E = Contents.end(); P < E; ) {
      StringRef Key = StringRef(reinterpret_cast<const char *>(P));
      StringRef Value =
          StringRef(reinterpret_cast<const char *>(P) + Key.size() + 1);

      W.printString(Key, Value);

      P = P + Key.size() + Value.size() + 2;
    }
  }
}

template <class ELFT>
void LLVMStyle<ELFT>::printMipsGOT(const MipsGOTParser<ELFT> &Parser) {
  auto PrintEntry = [&](const Elf_Addr *E) {
    W.printHex("Address", Parser.getGotAddress(E));
    W.printNumber("Access", Parser.getGotOffset(E));
    W.printHex("Initial", *E);
  };

  DictScope GS(W, Parser.IsStatic ? "Static GOT" : "Primary GOT");

  W.printHex("Canonical gp value", Parser.getGp());
  {
    ListScope RS(W, "Reserved entries");
    {
      DictScope D(W, "Entry");
      PrintEntry(Parser.getGotLazyResolver());
      W.printString("Purpose", StringRef("Lazy resolver"));
    }

    if (Parser.getGotModulePointer()) {
      DictScope D(W, "Entry");
      PrintEntry(Parser.getGotModulePointer());
      W.printString("Purpose", StringRef("Module pointer (GNU extension)"));
    }
  }
  {
    ListScope LS(W, "Local entries");
    for (auto &E : Parser.getLocalEntries()) {
      DictScope D(W, "Entry");
      PrintEntry(&E);
    }
  }

  if (Parser.IsStatic)
    return;

  {
    ListScope GS(W, "Global entries");
    for (auto &E : Parser.getGlobalEntries()) {
      DictScope D(W, "Entry");

      PrintEntry(&E);

      const Elf_Sym *Sym = Parser.getGotSym(&E);
      W.printHex("Value", Sym->st_value);
      W.printEnum("Type", Sym->getType(), makeArrayRef(ElfSymbolTypes));

      unsigned SectionIndex = 0;
      StringRef SectionName;
      this->dumper()->getSectionNameIndex(
          Sym, this->dumper()->dynamic_symbols().begin(), SectionName,
          SectionIndex);
      W.printHex("Section", SectionName, SectionIndex);

      std::string SymName = this->dumper()->getFullSymbolName(
          Sym, this->dumper()->getDynamicStringTable(), true);
      W.printNumber("Name", SymName, Sym->st_name);
    }
  }

  W.printNumber("Number of TLS and multi-GOT entries",
                uint64_t(Parser.getOtherEntries().size()));
}

template <class ELFT>
void LLVMStyle<ELFT>::printMipsPLT(const MipsGOTParser<ELFT> &Parser) {
  auto PrintEntry = [&](const Elf_Addr *E) {
    W.printHex("Address", Parser.getPltAddress(E));
    W.printHex("Initial", *E);
  };

  DictScope GS(W, "PLT GOT");

  {
    ListScope RS(W, "Reserved entries");
    {
      DictScope D(W, "Entry");
      PrintEntry(Parser.getPltLazyResolver());
      W.printString("Purpose", StringRef("PLT lazy resolver"));
    }

    if (auto E = Parser.getPltModulePointer()) {
      DictScope D(W, "Entry");
      PrintEntry(E);
      W.printString("Purpose", StringRef("Module pointer"));
    }
  }
  {
    ListScope LS(W, "Entries");
    for (auto &E : Parser.getPltEntries()) {
      DictScope D(W, "Entry");
      PrintEntry(&E);

      const Elf_Sym *Sym = Parser.getPltSym(&E);
      W.printHex("Value", Sym->st_value);
      W.printEnum("Type", Sym->getType(), makeArrayRef(ElfSymbolTypes));

      unsigned SectionIndex = 0;
      StringRef SectionName;
      this->dumper()->getSectionNameIndex(
          Sym, this->dumper()->dynamic_symbols().begin(), SectionName,
          SectionIndex);
      W.printHex("Section", SectionName, SectionIndex);

      std::string SymName =
          this->dumper()->getFullSymbolName(Sym, Parser.getPltStrTable(), true);
      W.printNumber("Name", SymName, Sym->st_name);
    }
  }
}
