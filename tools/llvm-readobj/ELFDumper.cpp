//===- ELFDumper.cpp - ELF-specific dumper --------------------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
///
/// \file
/// \brief This file implements the ELF-specific dumper for llvm-readobj.
///
//===----------------------------------------------------------------------===//

#include "ARMEHABIPrinter.h"
#include "Error.h"
#include "ObjDumper.h"
#include "StackMapPrinter.h"
#include "llvm-readobj.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/Optional.h"
#include "llvm/ADT/PointerIntPair.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/Twine.h"
#include "llvm/BinaryFormat/ELF.h"
#include "llvm/Object/ELF.h"
#include "llvm/Object/ELFObjectFile.h"
#include "llvm/Object/ELFTypes.h"
#include "llvm/Object/Error.h"
#include "llvm/Object/ObjectFile.h"
#include "llvm/Object/StackMapParser.h"
#include "llvm/Support/ARMAttributeParser.h"
#include "llvm/Support/ARMBuildAttributes.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/Compiler.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/Format.h"
#include "llvm/Support/FormattedStream.h"
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

#define LLVM_READOBJ_ENUM_CASE(ns, enum) \
  case ns::enum: return #enum;

#define ENUM_ENT(enum, altName) \
  { #enum, altName, ELF::enum }

#define ENUM_ENT_1(enum) \
  { #enum, #enum, ELF::enum }

#define LLVM_READOBJ_PHDR_ENUM(ns, enum)                                       \
  case ns::enum:                                                               \
    return std::string(#enum).substr(3);

#define TYPEDEF_ELF_TYPES(ELFT)                                                \
  using ELFO = ELFFile<ELFT>;                                                  \
  using Elf_Shdr = typename ELFO::Elf_Shdr;                                    \
  using Elf_Sym = typename ELFO::Elf_Sym;                                      \
  using Elf_Dyn = typename ELFO::Elf_Dyn;                                      \
  using Elf_Dyn_Range = typename ELFO::Elf_Dyn_Range;                          \
  using Elf_Rel = typename ELFO::Elf_Rel;                                      \
  using Elf_Rela = typename ELFO::Elf_Rela;                                    \
  using Elf_Rel_Range = typename ELFO::Elf_Rel_Range;                          \
  using Elf_Rela_Range = typename ELFO::Elf_Rela_Range;                        \
  using Elf_Phdr = typename ELFO::Elf_Phdr;                                    \
  using Elf_Half = typename ELFO::Elf_Half;                                    \
  using Elf_Ehdr = typename ELFO::Elf_Ehdr;                                    \
  using Elf_Word = typename ELFO::Elf_Word;                                    \
  using Elf_Hash = typename ELFO::Elf_Hash;                                    \
  using Elf_GnuHash = typename ELFO::Elf_GnuHash;                              \
  using Elf_Sym_Range = typename ELFO::Elf_Sym_Range;                          \
  using Elf_Versym = typename ELFO::Elf_Versym;                                \
  using Elf_Verneed = typename ELFO::Elf_Verneed;                              \
  using Elf_Vernaux = typename ELFO::Elf_Vernaux;                              \
  using Elf_Verdef = typename ELFO::Elf_Verdef;                                \
  using Elf_Verdaux = typename ELFO::Elf_Verdaux;                              \
  using uintX_t = typename ELFO::uintX_t;

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

  /// \brief Address in current address space.
  const void *Addr = nullptr;
  /// \brief Size in bytes of the region.
  uint64_t Size = 0;
  /// \brief Size of each entity in the region.
  uint64_t EntSize = 0;

  template <typename Type> ArrayRef<Type> getAsArrayRef() const {
    const Type *Start = reinterpret_cast<const Type *>(Addr);
    if (!Start)
      return {Start, Start};
    if (EntSize != sizeof(Type) || Size % EntSize)
      reportError("Invalid entity size");
    return {Start, Start + (Size / EntSize)};
  }
};

template<typename ELFT>
class ELFDumper : public ObjDumper {
public:
  ELFDumper(const ELFFile<ELFT> *Obj, ScopedPrinter &Writer);

  void printFileHeaders() override;
  void printSections() override;
  void printRelocations() override;
  void printDynamicRelocations() override;
  void printSymbols() override;
  void printDynamicSymbols() override;
  void printUnwindInfo() override;

  void printDynamicTable() override;
  void printNeededLibraries() override;
  void printProgramHeaders() override;
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

  void printAMDGPUCodeObjectMetadata() override;

  void printStackMap() const override;

  void printHashHistogram() override;

  void printNotes() override;

private:
  std::unique_ptr<DumpStyle<ELFT>> ELFDumperStyle;

  TYPEDEF_ELF_TYPES(ELFT)

  DynRegionInfo checkDRI(DynRegionInfo DRI) {
    if (DRI.Addr < Obj->base() ||
        (const uint8_t *)DRI.Addr + DRI.Size > Obj->base() + Obj->getBufSize())
      error(llvm::object::object_error::parse_failed);
    return DRI;
  }

  DynRegionInfo createDRIFrom(const Elf_Phdr *P, uintX_t EntSize) {
    return checkDRI({Obj->base() + P->p_offset, P->p_filesz, EntSize});
  }

  DynRegionInfo createDRIFrom(const Elf_Shdr *S) {
    return checkDRI({Obj->base() + S->sh_offset, S->sh_size, S->sh_entsize});
  }

  void parseDynamicTable(ArrayRef<const Elf_Phdr *> LoadSegments);

  void printValue(uint64_t Type, uint64_t Value);

  StringRef getDynamicString(uint64_t Offset) const;
  StringRef getSymbolVersion(StringRef StrTab, const Elf_Sym *symb,
                             bool &IsDefault) const;
  void LoadVersionMap() const;
  void LoadVersionNeeds(const Elf_Shdr *ec) const;
  void LoadVersionDefs(const Elf_Shdr *sec) const;

  const ELFO *Obj;
  DynRegionInfo DynRelRegion;
  DynRegionInfo DynRelaRegion;
  DynRegionInfo DynPLTRelRegion;
  DynRegionInfo DynSymRegion;
  DynRegionInfo DynamicTable;
  StringRef DynamicStringTable;
  StringRef SOName;
  const Elf_Hash *HashTable = nullptr;
  const Elf_GnuHash *GnuHashTable = nullptr;
  const Elf_Shdr *DotSymtabSec = nullptr;
  StringRef DynSymtabName;
  ArrayRef<Elf_Word> ShndxTable;

  const Elf_Shdr *dot_gnu_version_sec = nullptr;   // .gnu.version
  const Elf_Shdr *dot_gnu_version_r_sec = nullptr; // .gnu.version_r
  const Elf_Shdr *dot_gnu_version_d_sec = nullptr; // .gnu.version_d

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
    return DynamicTable.getAsArrayRef<Elf_Dyn>();
  }

  Elf_Sym_Range dynamic_symbols() const {
    return DynSymRegion.getAsArrayRef<Elf_Sym>();
  }

  Elf_Rel_Range dyn_rels() const;
  Elf_Rela_Range dyn_relas() const;
  std::string getFullSymbolName(const Elf_Sym *Symbol, StringRef StrTable,
                                bool IsDynamic) const;

  void printSymbolsHelper(bool IsDynamic) const;
  const Elf_Shdr *getDotSymtabSec() const { return DotSymtabSec; }
  ArrayRef<Elf_Word> getShndxTable() const { return ShndxTable; }
  StringRef getDynamicStringTable() const { return DynamicStringTable; }
  const DynRegionInfo &getDynRelRegion() const { return DynRelRegion; }
  const DynRegionInfo &getDynRelaRegion() const { return DynRelaRegion; }
  const DynRegionInfo &getDynPLTRelRegion() const { return DynPLTRelRegion; }
  const Elf_Hash *getHashTable() const { return HashTable; }
  const Elf_GnuHash *getGnuHashTable() const { return GnuHashTable; }
};

template <class ELFT>
void ELFDumper<ELFT>::printSymbolsHelper(bool IsDynamic) const {
  StringRef StrTable, SymtabName;
  size_t Entries = 0;
  Elf_Sym_Range Syms(nullptr, nullptr);
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

template <typename ELFT> class DumpStyle {
public:
  using Elf_Shdr = typename ELFFile<ELFT>::Elf_Shdr;
  using Elf_Sym = typename ELFFile<ELFT>::Elf_Sym;

  DumpStyle(ELFDumper<ELFT> *Dumper) : Dumper(Dumper) {}
  virtual ~DumpStyle() = default;

  virtual void printFileHeaders(const ELFFile<ELFT> *Obj) = 0;
  virtual void printGroupSections(const ELFFile<ELFT> *Obj) = 0;
  virtual void printRelocations(const ELFFile<ELFT> *Obj) = 0;
  virtual void printSections(const ELFFile<ELFT> *Obj) = 0;
  virtual void printSymbols(const ELFFile<ELFT> *Obj) = 0;
  virtual void printDynamicSymbols(const ELFFile<ELFT> *Obj) = 0;
  virtual void printDynamicRelocations(const ELFFile<ELFT> *Obj) = 0;
  virtual void printSymtabMessage(const ELFFile<ELFT> *obj, StringRef Name,
                                  size_t Offset) {}
  virtual void printSymbol(const ELFFile<ELFT> *Obj, const Elf_Sym *Symbol,
                           const Elf_Sym *FirstSym, StringRef StrTable,
                           bool IsDynamic) = 0;
  virtual void printProgramHeaders(const ELFFile<ELFT> *Obj) = 0;
  virtual void printHashHistogram(const ELFFile<ELFT> *Obj) = 0;
  virtual void printNotes(const ELFFile<ELFT> *Obj) = 0;
  const ELFDumper<ELFT> *dumper() const { return Dumper; }

private:
  const ELFDumper<ELFT> *Dumper;
};

template <typename ELFT> class GNUStyle : public DumpStyle<ELFT> {
  formatted_raw_ostream OS;

public:
  TYPEDEF_ELF_TYPES(ELFT)

  GNUStyle(ScopedPrinter &W, ELFDumper<ELFT> *Dumper)
      : DumpStyle<ELFT>(Dumper), OS(W.getOStream()) {}

  void printFileHeaders(const ELFO *Obj) override;
  void printGroupSections(const ELFFile<ELFT> *Obj) override;
  void printRelocations(const ELFO *Obj) override;
  void printSections(const ELFO *Obj) override;
  void printSymbols(const ELFO *Obj) override;
  void printDynamicSymbols(const ELFO *Obj) override;
  void printDynamicRelocations(const ELFO *Obj) override;
  void printSymtabMessage(const ELFO *Obj, StringRef Name,
                          size_t Offset) override;
  void printProgramHeaders(const ELFO *Obj) override;
  void printHashHistogram(const ELFFile<ELFT> *Obj) override;
  void printNotes(const ELFFile<ELFT> *Obj) override;

private:
  struct Field {
    StringRef Str;
    unsigned Column;

    Field(StringRef S, unsigned Col) : Str(S), Column(Col) {}
    Field(unsigned Col) : Str(""), Column(Col) {}
  };

  template <typename T, typename TEnum>
  std::string printEnum(T Value, ArrayRef<EnumEntry<TEnum>> EnumValues) {
    for (const auto &EnumItem : EnumValues)
      if (EnumItem.Value == Value)
        return EnumItem.AltName;
    return to_hexString(Value, false);
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
  void printRelocation(const ELFO *Obj, const Elf_Shdr *SymTab,
                       const Elf_Rela &R, bool IsRela);
  void printSymbol(const ELFO *Obj, const Elf_Sym *Symbol, const Elf_Sym *First,
                   StringRef StrTable, bool IsDynamic) override;
  std::string getSymbolSectionNdx(const ELFO *Obj, const Elf_Sym *Symbol,
                                  const Elf_Sym *FirstSym);
  void printDynamicRelocation(const ELFO *Obj, Elf_Rela R, bool IsRela);
  bool checkTLSSections(const Elf_Phdr &Phdr, const Elf_Shdr &Sec);
  bool checkoffsets(const Elf_Phdr &Phdr, const Elf_Shdr &Sec);
  bool checkVMA(const Elf_Phdr &Phdr, const Elf_Shdr &Sec);
  bool checkPTDynamic(const Elf_Phdr &Phdr, const Elf_Shdr &Sec);
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
  void printSections(const ELFO *Obj) override;
  void printSymbols(const ELFO *Obj) override;
  void printDynamicSymbols(const ELFO *Obj) override;
  void printDynamicRelocations(const ELFO *Obj) override;
  void printProgramHeaders(const ELFO *Obj) override;
  void printHashHistogram(const ELFFile<ELFT> *Obj) override;
  void printNotes(const ELFFile<ELFT> *Obj) override;

private:
  void printRelocation(const ELFO *Obj, Elf_Rela Rel, const Elf_Shdr *SymTab);
  void printDynamicRelocation(const ELFO *Obj, Elf_Rela Rel);
  void printSymbol(const ELFO *Obj, const Elf_Sym *Symbol, const Elf_Sym *First,
                   StringRef StrTable, bool IsDynamic) override;

  ScopedPrinter &W;
};

} // end anonymous namespace

namespace llvm {

template <class ELFT>
static std::error_code createELFDumper(const ELFFile<ELFT> *Obj,
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
    return createELFDumper(ELFObj->getELFFile(), Writer, Result);

  // Big-endian 32-bit
  if (const ELF32BEObjectFile *ELFObj = dyn_cast<ELF32BEObjectFile>(Obj))
    return createELFDumper(ELFObj->getELFFile(), Writer, Result);

  // Little-endian 64-bit
  if (const ELF64LEObjectFile *ELFObj = dyn_cast<ELF64LEObjectFile>(Obj))
    return createELFDumper(ELFObj->getELFFile(), Writer, Result);

  // Big-endian 64-bit
  if (const ELF64BEObjectFile *ELFObj = dyn_cast<ELF64BEObjectFile>(Obj))
    return createELFDumper(ELFObj->getELFFile(), Writer, Result);

  return readobj_error::unsupported_obj_file_format;
}

} // end namespace llvm

// Iterate through the versions needed section, and place each Elf_Vernaux
// in the VersionMap according to its index.
template <class ELFT>
void ELFDumper<ELFT>::LoadVersionNeeds(const Elf_Shdr *sec) const {
  unsigned vn_size = sec->sh_size;  // Size of section in bytes
  unsigned vn_count = sec->sh_info; // Number of Verneed entries
  const char *sec_start = (const char *)Obj->base() + sec->sh_offset;
  const char *sec_end = sec_start + vn_size;
  // The first Verneed entry is at the start of the section.
  const char *p = sec_start;
  for (unsigned i = 0; i < vn_count; i++) {
    if (p + sizeof(Elf_Verneed) > sec_end)
      report_fatal_error("Section ended unexpectedly while scanning "
                         "version needed records.");
    const Elf_Verneed *vn = reinterpret_cast<const Elf_Verneed *>(p);
    if (vn->vn_version != ELF::VER_NEED_CURRENT)
      report_fatal_error("Unexpected verneed version");
    // Iterate through the Vernaux entries
    const char *paux = p + vn->vn_aux;
    for (unsigned j = 0; j < vn->vn_cnt; j++) {
      if (paux + sizeof(Elf_Vernaux) > sec_end)
        report_fatal_error("Section ended unexpected while scanning auxiliary "
                           "version needed records.");
      const Elf_Vernaux *vna = reinterpret_cast<const Elf_Vernaux *>(paux);
      size_t index = vna->vna_other & ELF::VERSYM_VERSION;
      if (index >= VersionMap.size())
        VersionMap.resize(index + 1);
      VersionMap[index] = VersionMapEntry(vna);
      paux += vna->vna_next;
    }
    p += vn->vn_next;
  }
}

// Iterate through the version definitions, and place each Elf_Verdef
// in the VersionMap according to its index.
template <class ELFT>
void ELFDumper<ELFT>::LoadVersionDefs(const Elf_Shdr *sec) const {
  unsigned vd_size = sec->sh_size;  // Size of section in bytes
  unsigned vd_count = sec->sh_info; // Number of Verdef entries
  const char *sec_start = (const char *)Obj->base() + sec->sh_offset;
  const char *sec_end = sec_start + vd_size;
  // The first Verdef entry is at the start of the section.
  const char *p = sec_start;
  for (unsigned i = 0; i < vd_count; i++) {
    if (p + sizeof(Elf_Verdef) > sec_end)
      report_fatal_error("Section ended unexpectedly while scanning "
                         "version definitions.");
    const Elf_Verdef *vd = reinterpret_cast<const Elf_Verdef *>(p);
    if (vd->vd_version != ELF::VER_DEF_CURRENT)
      report_fatal_error("Unexpected verdef version");
    size_t index = vd->vd_ndx & ELF::VERSYM_VERSION;
    if (index >= VersionMap.size())
      VersionMap.resize(index + 1);
    VersionMap[index] = VersionMapEntry(vd);
    p += vd->vd_next;
  }
}

template <class ELFT> void ELFDumper<ELFT>::LoadVersionMap() const {
  // If there is no dynamic symtab or version table, there is nothing to do.
  if (!DynSymRegion.Addr || !dot_gnu_version_sec)
    return;

  // Has the VersionMap already been loaded?
  if (VersionMap.size() > 0)
    return;

  // The first two version indexes are reserved.
  // Index 0 is LOCAL, index 1 is GLOBAL.
  VersionMap.push_back(VersionMapEntry());
  VersionMap.push_back(VersionMapEntry());

  if (dot_gnu_version_d_sec)
    LoadVersionDefs(dot_gnu_version_d_sec);

  if (dot_gnu_version_r_sec)
    LoadVersionNeeds(dot_gnu_version_r_sec);
}

template <typename ELFO, class ELFT>
static void printVersionSymbolSection(ELFDumper<ELFT> *Dumper, const ELFO *Obj,
                                      const typename ELFO::Elf_Shdr *Sec,
                                      ScopedPrinter &W) {
  DictScope SS(W, "Version symbols");
  if (!Sec)
    return;
  StringRef Name = unwrapOrError(Obj->getSectionName(Sec));
  W.printNumber("Section Name", Name, Sec->sh_name);
  W.printHex("Address", Sec->sh_addr);
  W.printHex("Offset", Sec->sh_offset);
  W.printNumber("Link", Sec->sh_link);

  const uint8_t *P = (const uint8_t *)Obj->base() + Sec->sh_offset;
  StringRef StrTable = Dumper->getDynamicStringTable();

  // Same number of entries in the dynamic symbol table (DT_SYMTAB).
  ListScope Syms(W, "Symbols");
  for (const typename ELFO::Elf_Sym &Sym : Dumper->dynamic_symbols()) {
    DictScope S(W, "Symbol");
    std::string FullSymbolName =
        Dumper->getFullSymbolName(&Sym, StrTable, true /* IsDynamic */);
    W.printNumber("Version", *P);
    W.printString("Name", FullSymbolName);
    P += sizeof(typename ELFO::Elf_Half);
  }
}

static const EnumEntry<unsigned> SymVersionFlags[] = {
    {"Base", "BASE", VER_FLG_BASE},
    {"Weak", "WEAK", VER_FLG_WEAK},
    {"Info", "INFO", VER_FLG_INFO}};

template <typename ELFO, class ELFT>
static void printVersionDefinitionSection(ELFDumper<ELFT> *Dumper,
                                          const ELFO *Obj,
                                          const typename ELFO::Elf_Shdr *Sec,
                                          ScopedPrinter &W) {
  using VerDef = typename ELFO::Elf_Verdef;
  using VerdAux = typename ELFO::Elf_Verdaux;

  DictScope SD(W, "SHT_GNU_verdef");
  if (!Sec)
    return;

  // The number of entries in the section SHT_GNU_verdef
  // is determined by DT_VERDEFNUM tag.
  unsigned VerDefsNum = 0;
  for (const typename ELFO::Elf_Dyn &Dyn : Dumper->dynamic_table()) {
    if (Dyn.d_tag == DT_VERDEFNUM)
      VerDefsNum = Dyn.d_un.d_val;
  }
  const uint8_t *SecStartAddress =
      (const uint8_t *)Obj->base() + Sec->sh_offset;
  const uint8_t *SecEndAddress = SecStartAddress + Sec->sh_size;
  const uint8_t *P = SecStartAddress;
  const typename ELFO::Elf_Shdr *StrTab =
      unwrapOrError(Obj->getSection(Sec->sh_link));

  while (VerDefsNum--) {
    if (P + sizeof(VerDef) > SecEndAddress)
      report_fatal_error("invalid offset in the section");

    auto *VD = reinterpret_cast<const VerDef *>(P);
    DictScope Def(W, "Definition");
    W.printNumber("Version", VD->vd_version);
    W.printEnum("Flags", VD->vd_flags, makeArrayRef(SymVersionFlags));
    W.printNumber("Index", VD->vd_ndx);
    W.printNumber("Hash", VD->vd_hash);
    W.printString("Name",
                  StringRef((const char *)(Obj->base() + StrTab->sh_offset +
                                           VD->getAux()->vda_name)));
    if (!VD->vd_cnt)
      report_fatal_error("at least one definition string must exist");
    if (VD->vd_cnt > 2)
      report_fatal_error("more than one predecessor is not expected");

    if (VD->vd_cnt == 2) {
      const uint8_t *PAux = P + VD->vd_aux + VD->getAux()->vda_next;
      const VerdAux *Aux = reinterpret_cast<const VerdAux *>(PAux);
      W.printString("Predecessor",
                    StringRef((const char *)(Obj->base() + StrTab->sh_offset +
                                             Aux->vda_name)));
    }

    P += VD->vd_next;
  }
}

template <typename ELFO, class ELFT>
static void printVersionDependencySection(ELFDumper<ELFT> *Dumper,
                                          const ELFO *Obj,
                                          const typename ELFO::Elf_Shdr *Sec,
                                          ScopedPrinter &W) {
  using VerNeed = typename ELFO::Elf_Verneed;
  using VernAux = typename ELFO::Elf_Vernaux;

  DictScope SD(W, "SHT_GNU_verneed");
  if (!Sec)
    return;

  unsigned VerNeedNum = 0;
  for (const typename ELFO::Elf_Dyn &Dyn : Dumper->dynamic_table())
    if (Dyn.d_tag == DT_VERNEEDNUM)
      VerNeedNum = Dyn.d_un.d_val;

  const uint8_t *SecData = (const uint8_t *)Obj->base() + Sec->sh_offset;
  const typename ELFO::Elf_Shdr *StrTab =
      unwrapOrError(Obj->getSection(Sec->sh_link));

  const uint8_t *P = SecData;
  for (unsigned I = 0; I < VerNeedNum; ++I) {
    const VerNeed *Need = reinterpret_cast<const VerNeed *>(P);
    DictScope Entry(W, "Dependency");
    W.printNumber("Version", Need->vn_version);
    W.printNumber("Count", Need->vn_cnt);
    W.printString("FileName",
                  StringRef((const char *)(Obj->base() + StrTab->sh_offset +
                                           Need->vn_file)));

    const uint8_t *PAux = P + Need->vn_aux;
    for (unsigned J = 0; J < Need->vn_cnt; ++J) {
      const VernAux *Aux = reinterpret_cast<const VernAux *>(PAux);
      DictScope Entry(W, "Entry");
      W.printNumber("Hash", Aux->vna_hash);
      W.printEnum("Flags", Aux->vna_flags, makeArrayRef(SymVersionFlags));
      W.printNumber("Index", Aux->vna_other);
      W.printString("Name",
                    StringRef((const char *)(Obj->base() + StrTab->sh_offset +
                                             Aux->vna_name)));
      PAux += Aux->vna_next;
    }
    P += Need->vn_next;
  }
}

template <typename ELFT> void ELFDumper<ELFT>::printVersionInfo() {
  // Dump version symbol section.
  printVersionSymbolSection(this, Obj, dot_gnu_version_sec, W);

  // Dump version definition section.
  printVersionDefinitionSection(this, Obj, dot_gnu_version_d_sec, W);

  // Dump version dependency section.
  printVersionDependencySection(this, Obj, dot_gnu_version_r_sec, W);
}

template <typename ELFT>
StringRef ELFDumper<ELFT>::getSymbolVersion(StringRef StrTab,
                                            const Elf_Sym *symb,
                                            bool &IsDefault) const {
  // This is a dynamic symbol. Look in the GNU symbol version table.
  if (!dot_gnu_version_sec) {
    // No version table.
    IsDefault = false;
    return StringRef("");
  }

  // Determine the position in the symbol table of this entry.
  size_t entry_index = (reinterpret_cast<uintptr_t>(symb) -
                        reinterpret_cast<uintptr_t>(DynSymRegion.Addr)) /
                       sizeof(Elf_Sym);

  // Get the corresponding version index entry
  const Elf_Versym *vs = unwrapOrError(
      Obj->template getEntry<Elf_Versym>(dot_gnu_version_sec, entry_index));
  size_t version_index = vs->vs_index & ELF::VERSYM_VERSION;

  // Special markers for unversioned symbols.
  if (version_index == ELF::VER_NDX_LOCAL ||
      version_index == ELF::VER_NDX_GLOBAL) {
    IsDefault = false;
    return StringRef("");
  }

  // Lookup this symbol in the version table
  LoadVersionMap();
  if (version_index >= VersionMap.size() || VersionMap[version_index].isNull())
    reportError("Invalid version entry");
  const VersionMapEntry &entry = VersionMap[version_index];

  // Get the version name string
  size_t name_offset;
  if (entry.isVerdef()) {
    // The first Verdaux entry holds the name.
    name_offset = entry.getVerdef()->getAux()->vda_name;
    IsDefault = !(vs->vs_index & ELF::VERSYM_HIDDEN);
  } else {
    name_offset = entry.getVernaux()->vna_name;
    IsDefault = false;
  }
  if (name_offset >= StrTab.size())
    reportError("Invalid string offset");
  return StringRef(StrTab.data() + name_offset);
}

template <typename ELFT>
std::string ELFDumper<ELFT>::getFullSymbolName(const Elf_Sym *Symbol,
                                               StringRef StrTable,
                                               bool IsDynamic) const {
  StringRef SymbolName = unwrapOrError(Symbol->getName(StrTable));
  if (!IsDynamic)
    return SymbolName;

  std::string FullSymbolName(SymbolName);

  bool IsDefault;
  StringRef Version = getSymbolVersion(StrTable, &*Symbol, IsDefault);
  FullSymbolName += (IsDefault ? "@@" : "@");
  FullSymbolName += Version;
  return FullSymbolName;
}

template <typename ELFT>
static void
getSectionNameIndex(const ELFFile<ELFT> &Obj, const typename ELFT::Sym *Symbol,
                    const typename ELFT::Sym *FirstSym,
                    ArrayRef<typename ELFT::Word> ShndxTable,
                    StringRef &SectionName, unsigned &SectionIndex) {
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
    const typename ELFT::Shdr *Sec =
        unwrapOrError(Obj.getSection(SectionIndex));
    SectionName = unwrapOrError(Obj.getSectionName(Sec));
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
  {"C6000_ELFABI", "Bare-metal C6000",     ELF::ELFOSABI_C6000_ELFABI},
  {"C6000_LINUX",  "Linux C6000",          ELF::ELFOSABI_C6000_LINUX},
  {"ARM",          "ARM",                  ELF::ELFOSABI_ARM},
  {"Standalone",   "Standalone App",       ELF::ELFOSABI_STANDALONE}
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
  ENUM_ENT(EM_AVR32,         "Atmel AVR 8-bit microcontroller"),
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
  ENUM_ENT(EM_WEBASSEMBLY,   "EM_WEBASSEMBLY"),
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

static const EnumEntry<unsigned> ElfSymbolTypes[] = {
    {"None",      "NOTYPE",  ELF::STT_NOTYPE},
    {"Object",    "OBJECT",  ELF::STT_OBJECT},
    {"Function",  "FUNC",    ELF::STT_FUNC},
    {"Section",   "SECTION", ELF::STT_SECTION},
    {"File",      "FILE",    ELF::STT_FILE},
    {"Common",    "COMMON",  ELF::STT_COMMON},
    {"TLS",       "TLS",     ELF::STT_TLS},
    {"GNU_IFunc", "IFUNC",   ELF::STT_GNU_IFUNC}};

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
    switch (Type) {
    LLVM_READOBJ_ENUM_CASE(ELF, PT_ARM_EXIDX);
    }
  case ELF::EM_MIPS:
  case ELF::EM_MIPS_RS3_LE:
    switch (Type) {
    LLVM_READOBJ_ENUM_CASE(ELF, PT_MIPS_REGINFO);
    LLVM_READOBJ_ENUM_CASE(ELF, PT_MIPS_RTPROC);
    LLVM_READOBJ_ENUM_CASE(ELF, PT_MIPS_OPTIONS);
    LLVM_READOBJ_ENUM_CASE(ELF, PT_MIPS_ABIFLAGS);
    }
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

  default: return "";
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
      return "";
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
      return "";
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
  LLVM_READOBJ_ENUM_ENT(ELF, EF_MIPS_NOREORDER),
  LLVM_READOBJ_ENUM_ENT(ELF, EF_MIPS_PIC),
  LLVM_READOBJ_ENUM_ENT(ELF, EF_MIPS_CPIC),
  LLVM_READOBJ_ENUM_ENT(ELF, EF_MIPS_ABI2),
  LLVM_READOBJ_ENUM_ENT(ELF, EF_MIPS_32BITMODE),
  LLVM_READOBJ_ENUM_ENT(ELF, EF_MIPS_FP64),
  LLVM_READOBJ_ENUM_ENT(ELF, EF_MIPS_NAN2008),
  LLVM_READOBJ_ENUM_ENT(ELF, EF_MIPS_ABI_O32),
  LLVM_READOBJ_ENUM_ENT(ELF, EF_MIPS_ABI_O64),
  LLVM_READOBJ_ENUM_ENT(ELF, EF_MIPS_ABI_EABI32),
  LLVM_READOBJ_ENUM_ENT(ELF, EF_MIPS_ABI_EABI64),
  LLVM_READOBJ_ENUM_ENT(ELF, EF_MIPS_MACH_3900),
  LLVM_READOBJ_ENUM_ENT(ELF, EF_MIPS_MACH_4010),
  LLVM_READOBJ_ENUM_ENT(ELF, EF_MIPS_MACH_4100),
  LLVM_READOBJ_ENUM_ENT(ELF, EF_MIPS_MACH_4650),
  LLVM_READOBJ_ENUM_ENT(ELF, EF_MIPS_MACH_4120),
  LLVM_READOBJ_ENUM_ENT(ELF, EF_MIPS_MACH_4111),
  LLVM_READOBJ_ENUM_ENT(ELF, EF_MIPS_MACH_SB1),
  LLVM_READOBJ_ENUM_ENT(ELF, EF_MIPS_MACH_OCTEON),
  LLVM_READOBJ_ENUM_ENT(ELF, EF_MIPS_MACH_XLR),
  LLVM_READOBJ_ENUM_ENT(ELF, EF_MIPS_MACH_OCTEON2),
  LLVM_READOBJ_ENUM_ENT(ELF, EF_MIPS_MACH_OCTEON3),
  LLVM_READOBJ_ENUM_ENT(ELF, EF_MIPS_MACH_5400),
  LLVM_READOBJ_ENUM_ENT(ELF, EF_MIPS_MACH_5900),
  LLVM_READOBJ_ENUM_ENT(ELF, EF_MIPS_MACH_5500),
  LLVM_READOBJ_ENUM_ENT(ELF, EF_MIPS_MACH_9000),
  LLVM_READOBJ_ENUM_ENT(ELF, EF_MIPS_MACH_LS2E),
  LLVM_READOBJ_ENUM_ENT(ELF, EF_MIPS_MACH_LS2F),
  LLVM_READOBJ_ENUM_ENT(ELF, EF_MIPS_MACH_LS3A),
  LLVM_READOBJ_ENUM_ENT(ELF, EF_MIPS_MICROMIPS),
  LLVM_READOBJ_ENUM_ENT(ELF, EF_MIPS_ARCH_ASE_M16),
  LLVM_READOBJ_ENUM_ENT(ELF, EF_MIPS_ARCH_ASE_MDMX),
  LLVM_READOBJ_ENUM_ENT(ELF, EF_MIPS_ARCH_1),
  LLVM_READOBJ_ENUM_ENT(ELF, EF_MIPS_ARCH_2),
  LLVM_READOBJ_ENUM_ENT(ELF, EF_MIPS_ARCH_3),
  LLVM_READOBJ_ENUM_ENT(ELF, EF_MIPS_ARCH_4),
  LLVM_READOBJ_ENUM_ENT(ELF, EF_MIPS_ARCH_5),
  LLVM_READOBJ_ENUM_ENT(ELF, EF_MIPS_ARCH_32),
  LLVM_READOBJ_ENUM_ENT(ELF, EF_MIPS_ARCH_64),
  LLVM_READOBJ_ENUM_ENT(ELF, EF_MIPS_ARCH_32R2),
  LLVM_READOBJ_ENUM_ENT(ELF, EF_MIPS_ARCH_64R2),
  LLVM_READOBJ_ENUM_ENT(ELF, EF_MIPS_ARCH_32R6),
  LLVM_READOBJ_ENUM_ENT(ELF, EF_MIPS_ARCH_64R6)
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
ELFDumper<ELFT>::ELFDumper(const ELFFile<ELFT> *Obj, ScopedPrinter &Writer)
    : ObjDumper(Writer), Obj(Obj) {
  SmallVector<const Elf_Phdr *, 4> LoadSegments;
  for (const Elf_Phdr &Phdr : unwrapOrError(Obj->program_headers())) {
    if (Phdr.p_type == ELF::PT_DYNAMIC) {
      DynamicTable = createDRIFrom(&Phdr, sizeof(Elf_Dyn));
      continue;
    }
    if (Phdr.p_type != ELF::PT_LOAD || Phdr.p_filesz == 0)
      continue;
    LoadSegments.push_back(&Phdr);
  }

  for (const Elf_Shdr &Sec : unwrapOrError(Obj->sections())) {
    switch (Sec.sh_type) {
    case ELF::SHT_SYMTAB:
      if (DotSymtabSec != nullptr)
        reportError("Multilpe SHT_SYMTAB");
      DotSymtabSec = &Sec;
      break;
    case ELF::SHT_DYNSYM:
      if (DynSymRegion.Size)
        reportError("Multilpe SHT_DYNSYM");
      DynSymRegion = createDRIFrom(&Sec);
      // This is only used (if Elf_Shdr present)for naming section in GNU style
      DynSymtabName = unwrapOrError(Obj->getSectionName(&Sec));
      break;
    case ELF::SHT_SYMTAB_SHNDX:
      ShndxTable = unwrapOrError(Obj->getSHNDXTable(Sec));
      break;
    case ELF::SHT_GNU_versym:
      if (dot_gnu_version_sec != nullptr)
        reportError("Multiple SHT_GNU_versym");
      dot_gnu_version_sec = &Sec;
      break;
    case ELF::SHT_GNU_verdef:
      if (dot_gnu_version_d_sec != nullptr)
        reportError("Multiple SHT_GNU_verdef");
      dot_gnu_version_d_sec = &Sec;
      break;
    case ELF::SHT_GNU_verneed:
      if (dot_gnu_version_r_sec != nullptr)
        reportError("Multilpe SHT_GNU_verneed");
      dot_gnu_version_r_sec = &Sec;
      break;
    }
  }

  parseDynamicTable(LoadSegments);

  if (opts::Output == opts::GNU)
    ELFDumperStyle.reset(new GNUStyle<ELFT>(Writer, this));
  else
    ELFDumperStyle.reset(new LLVMStyle<ELFT>(Writer, this));
}

template <typename ELFT>
void ELFDumper<ELFT>::parseDynamicTable(
    ArrayRef<const Elf_Phdr *> LoadSegments) {
  auto toMappedAddr = [&](uint64_t VAddr) -> const uint8_t * {
    const Elf_Phdr *const *I = std::upper_bound(
        LoadSegments.begin(), LoadSegments.end(), VAddr, compareAddr<ELFT>);
    if (I == LoadSegments.begin())
      report_fatal_error("Virtual address is not in any segment");
    --I;
    const Elf_Phdr &Phdr = **I;
    uint64_t Delta = VAddr - Phdr.p_vaddr;
    if (Delta >= Phdr.p_filesz)
      report_fatal_error("Virtual address is not in any segment");
    return Obj->base() + Phdr.p_offset + Delta;
  };

  uint64_t SONameOffset = 0;
  const char *StringTableBegin = nullptr;
  uint64_t StringTableSize = 0;
  for (const Elf_Dyn &Dyn : dynamic_table()) {
    switch (Dyn.d_tag) {
    case ELF::DT_HASH:
      HashTable =
          reinterpret_cast<const Elf_Hash *>(toMappedAddr(Dyn.getPtr()));
      break;
    case ELF::DT_GNU_HASH:
      GnuHashTable =
          reinterpret_cast<const Elf_GnuHash *>(toMappedAddr(Dyn.getPtr()));
      break;
    case ELF::DT_STRTAB:
      StringTableBegin = (const char *)toMappedAddr(Dyn.getPtr());
      break;
    case ELF::DT_STRSZ:
      StringTableSize = Dyn.getVal();
      break;
    case ELF::DT_SYMTAB:
      DynSymRegion.Addr = toMappedAddr(Dyn.getPtr());
      DynSymRegion.EntSize = sizeof(Elf_Sym);
      break;
    case ELF::DT_RELA:
      DynRelaRegion.Addr = toMappedAddr(Dyn.getPtr());
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
      DynRelRegion.Addr = toMappedAddr(Dyn.getPtr());
      break;
    case ELF::DT_RELSZ:
      DynRelRegion.Size = Dyn.getVal();
      break;
    case ELF::DT_RELENT:
      DynRelRegion.EntSize = Dyn.getVal();
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
      DynPLTRelRegion.Addr = toMappedAddr(Dyn.getPtr());
      break;
    case ELF::DT_PLTRELSZ:
      DynPLTRelRegion.Size = Dyn.getVal();
      break;
    }
  }
  if (StringTableBegin)
    DynamicStringTable = StringRef(StringTableBegin, StringTableSize);
  if (SONameOffset)
    SOName = getDynamicString(SONameOffset);
}

template <typename ELFT>
typename ELFDumper<ELFT>::Elf_Rel_Range ELFDumper<ELFT>::dyn_rels() const {
  return DynRelRegion.getAsArrayRef<Elf_Rel>();
}

template <typename ELFT>
typename ELFDumper<ELFT>::Elf_Rela_Range ELFDumper<ELFT>::dyn_relas() const {
  return DynRelaRegion.getAsArrayRef<Elf_Rela>();
}

template<class ELFT>
void ELFDumper<ELFT>::printFileHeaders() {
  ELFDumperStyle->printFileHeaders(Obj);
}

template<class ELFT>
void ELFDumper<ELFT>::printSections() {
  ELFDumperStyle->printSections(Obj);
}

template<class ELFT>
void ELFDumper<ELFT>::printRelocations() {
  ELFDumperStyle->printRelocations(Obj);
}

template <class ELFT> void ELFDumper<ELFT>::printProgramHeaders() {
  ELFDumperStyle->printProgramHeaders(Obj);
}

template <class ELFT> void ELFDumper<ELFT>::printDynamicRelocations() {
  ELFDumperStyle->printDynamicRelocations(Obj);
}

template<class ELFT>
void ELFDumper<ELFT>::printSymbols() {
  ELFDumperStyle->printSymbols(Obj);
}

template<class ELFT>
void ELFDumper<ELFT>::printDynamicSymbols() {
  ELFDumperStyle->printDynamicSymbols(Obj);
}

template <class ELFT> void ELFDumper<ELFT>::printHashHistogram() {
  ELFDumperStyle->printHashHistogram(Obj);
}

template <class ELFT> void ELFDumper<ELFT>::printNotes() {
  ELFDumperStyle->printNotes(Obj);
}

#define LLVM_READOBJ_TYPE_CASE(name) \
  case DT_##name: return #name

static const char *getTypeString(unsigned Arch, uint64_t Type) {
  switch (Arch) {
  case EM_HEXAGON:
    switch (Type) {
    LLVM_READOBJ_TYPE_CASE(HEXAGON_SYMSZ);
    LLVM_READOBJ_TYPE_CASE(HEXAGON_VER);
    LLVM_READOBJ_TYPE_CASE(HEXAGON_PLT);
    }
  case EM_MIPS:
    switch (Type) {
    LLVM_READOBJ_TYPE_CASE(MIPS_RLD_MAP_REL);
    LLVM_READOBJ_TYPE_CASE(MIPS_RLD_VERSION);
    LLVM_READOBJ_TYPE_CASE(MIPS_FLAGS);
    LLVM_READOBJ_TYPE_CASE(MIPS_BASE_ADDRESS);
    LLVM_READOBJ_TYPE_CASE(MIPS_LOCAL_GOTNO);
    LLVM_READOBJ_TYPE_CASE(MIPS_SYMTABNO);
    LLVM_READOBJ_TYPE_CASE(MIPS_UNREFEXTNO);
    LLVM_READOBJ_TYPE_CASE(MIPS_GOTSYM);
    LLVM_READOBJ_TYPE_CASE(MIPS_RLD_MAP);
    LLVM_READOBJ_TYPE_CASE(MIPS_PLTGOT);
    LLVM_READOBJ_TYPE_CASE(MIPS_OPTIONS);
    }
  }
  switch (Type) {
  LLVM_READOBJ_TYPE_CASE(BIND_NOW);
  LLVM_READOBJ_TYPE_CASE(DEBUG);
  LLVM_READOBJ_TYPE_CASE(FINI);
  LLVM_READOBJ_TYPE_CASE(FINI_ARRAY);
  LLVM_READOBJ_TYPE_CASE(FINI_ARRAYSZ);
  LLVM_READOBJ_TYPE_CASE(FLAGS);
  LLVM_READOBJ_TYPE_CASE(FLAGS_1);
  LLVM_READOBJ_TYPE_CASE(HASH);
  LLVM_READOBJ_TYPE_CASE(INIT);
  LLVM_READOBJ_TYPE_CASE(INIT_ARRAY);
  LLVM_READOBJ_TYPE_CASE(INIT_ARRAYSZ);
  LLVM_READOBJ_TYPE_CASE(PREINIT_ARRAY);
  LLVM_READOBJ_TYPE_CASE(PREINIT_ARRAYSZ);
  LLVM_READOBJ_TYPE_CASE(JMPREL);
  LLVM_READOBJ_TYPE_CASE(NEEDED);
  LLVM_READOBJ_TYPE_CASE(NULL);
  LLVM_READOBJ_TYPE_CASE(PLTGOT);
  LLVM_READOBJ_TYPE_CASE(PLTREL);
  LLVM_READOBJ_TYPE_CASE(PLTRELSZ);
  LLVM_READOBJ_TYPE_CASE(REL);
  LLVM_READOBJ_TYPE_CASE(RELA);
  LLVM_READOBJ_TYPE_CASE(RELENT);
  LLVM_READOBJ_TYPE_CASE(RELSZ);
  LLVM_READOBJ_TYPE_CASE(RELAENT);
  LLVM_READOBJ_TYPE_CASE(RELASZ);
  LLVM_READOBJ_TYPE_CASE(RPATH);
  LLVM_READOBJ_TYPE_CASE(RUNPATH);
  LLVM_READOBJ_TYPE_CASE(SONAME);
  LLVM_READOBJ_TYPE_CASE(STRSZ);
  LLVM_READOBJ_TYPE_CASE(STRTAB);
  LLVM_READOBJ_TYPE_CASE(SYMBOLIC);
  LLVM_READOBJ_TYPE_CASE(SYMENT);
  LLVM_READOBJ_TYPE_CASE(SYMTAB);
  LLVM_READOBJ_TYPE_CASE(TEXTREL);
  LLVM_READOBJ_TYPE_CASE(VERDEF);
  LLVM_READOBJ_TYPE_CASE(VERDEFNUM);
  LLVM_READOBJ_TYPE_CASE(VERNEED);
  LLVM_READOBJ_TYPE_CASE(VERNEEDNUM);
  LLVM_READOBJ_TYPE_CASE(VERSYM);
  LLVM_READOBJ_TYPE_CASE(RELACOUNT);
  LLVM_READOBJ_TYPE_CASE(RELCOUNT);
  LLVM_READOBJ_TYPE_CASE(GNU_HASH);
  LLVM_READOBJ_TYPE_CASE(TLSDESC_PLT);
  LLVM_READOBJ_TYPE_CASE(TLSDESC_GOT);
  LLVM_READOBJ_TYPE_CASE(AUXILIARY);
  LLVM_READOBJ_TYPE_CASE(FILTER);
  default: return "unknown";
  }
}

#undef LLVM_READOBJ_TYPE_CASE

#define LLVM_READOBJ_DT_FLAG_ENT(prefix, enum) \
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
StringRef ELFDumper<ELFT>::getDynamicString(uint64_t Value) const {
  if (Value >= DynamicStringTable.size())
    reportError("Invalid dynamic string table reference");
  return StringRef(DynamicStringTable.data() + Value);
}

static void printLibrary(raw_ostream &OS, const Twine &Tag, const Twine &Name) {
  OS << Tag << ": [" << Name << "]";
}

template <class ELFT>
void ELFDumper<ELFT>::printValue(uint64_t Type, uint64_t Value) {
  raw_ostream &OS = W.getOStream();
  const char* ConvChar = (opts::Output == opts::GNU) ? "0x%" PRIx64 : "0x%" PRIX64;
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
  case DT_MIPS_BASE_ADDRESS:
  case DT_MIPS_GOTSYM:
  case DT_MIPS_RLD_MAP:
  case DT_MIPS_RLD_MAP_REL:
  case DT_MIPS_PLTGOT:
  case DT_MIPS_OPTIONS:
    OS << format(ConvChar, Value);
    break;
  case DT_RELACOUNT:
  case DT_RELCOUNT:
  case DT_VERDEFNUM:
  case DT_VERNEEDNUM:
  case DT_MIPS_RLD_VERSION:
  case DT_MIPS_LOCAL_GOTNO:
  case DT_MIPS_SYMTABNO:
  case DT_MIPS_UNREFEXTNO:
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
    OS << Value << " (bytes)";
    break;
  case DT_NEEDED:
    printLibrary(OS, "Shared library", getDynamicString(Value));
    break;
  case DT_SONAME:
    printLibrary(OS, "Library soname", getDynamicString(Value));
    break;
  case DT_AUXILIARY:
    printLibrary(OS, "Auxiliary library", getDynamicString(Value));
    break;
  case DT_FILTER:
    printLibrary(OS, "Filter library", getDynamicString(Value));
    break;
  case DT_RPATH:
  case DT_RUNPATH:
    OS << getDynamicString(Value);
    break;
  case DT_MIPS_FLAGS:
    printFlags(Value, makeArrayRef(ElfDynamicDTMipsFlags), OS);
    break;
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

template<class ELFT>
void ELFDumper<ELFT>::printUnwindInfo() {
  W.startLine() << "UnwindInfo not implemented.\n";
}

namespace {

template <> void ELFDumper<ELFType<support::little, false>>::printUnwindInfo() {
  const unsigned Machine = Obj->getHeader()->e_machine;
  if (Machine == EM_ARM) {
    ARM::EHABI::PrinterContext<ELFType<support::little, false>> Ctx(
        W, Obj, DotSymtabSec);
    return Ctx.PrintUnwindInformation();
  }
  W.startLine() << "UnwindInfo not implemented.\n";
}

} // end anonymous namespace

template<class ELFT>
void ELFDumper<ELFT>::printDynamicTable() {
  auto I = dynamic_table().begin();
  auto E = dynamic_table().end();

  if (I == E)
    return;

  --E;
  while (I != E && E->getTag() == ELF::DT_NULL)
    --E;
  if (E->getTag() != ELF::DT_NULL)
    ++E;
  ++E;

  ptrdiff_t Total = std::distance(I, E);
  if (Total == 0)
    return;

  raw_ostream &OS = W.getOStream();
  W.startLine() << "DynamicSection [ (" << Total << " entries)\n";

  bool Is64 = ELFT::Is64Bits;

  W.startLine()
     << "  Tag" << (Is64 ? "                " : "        ") << "Type"
     << "                 " << "Name/Value\n";
  while (I != E) {
    const Elf_Dyn &Entry = *I;
    uintX_t Tag = Entry.getTag();
    ++I;
    W.startLine() << "  " << format_hex(Tag, Is64 ? 18 : 10, opts::Output != opts::GNU) << " "
                  << format("%-21s", getTypeString(Obj->getHeader()->e_machine, Tag));
    printValue(Tag, Entry.getVal());
    OS << "\n";
  }

  W.startLine() << "]\n";
}

template<class ELFT>
void ELFDumper<ELFT>::printNeededLibraries() {
  ListScope D(W, "NeededLibraries");

  using LibsTy = std::vector<StringRef>;
  LibsTy Libs;

  for (const auto &Entry : dynamic_table())
    if (Entry.d_tag == ELF::DT_NEEDED)
      Libs.push_back(getDynamicString(Entry.d_un.d_val));

  std::stable_sort(Libs.begin(), Libs.end());

  for (const auto &L : Libs) {
    outs() << "  " << L << "\n";
  }
}


template <typename ELFT>
void ELFDumper<ELFT>::printHashTable() {
  DictScope D(W, "HashTable");
  if (!HashTable)
    return;
  W.printNumber("Num Buckets", HashTable->nbucket);
  W.printNumber("Num Chains", HashTable->nchain);
  W.printList("Buckets", HashTable->buckets());
  W.printList("Chains", HashTable->chains());
}

template <typename ELFT>
void ELFDumper<ELFT>::printGnuHashTable() {
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
  outs() << "LoadName: " << SOName << '\n';
}

template <class ELFT>
void ELFDumper<ELFT>::printAttributes() {
  W.startLine() << "Attributes not implemented.\n";
}

namespace {

template <> void ELFDumper<ELFType<support::little, false>>::printAttributes() {
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
      errs() << "unrecognised FormatVersion: 0x" << utohexstr(Contents[0])
             << '\n';
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
  using GOTEntry = typename ELFO::Elf_Addr;

  MipsGOTParser(ELFDumper<ELFT> *Dumper, const ELFO *Obj,
                Elf_Dyn_Range DynTable, ScopedPrinter &W);

  void parseGOT();
  void parsePLT();

private:
  ELFDumper<ELFT> *Dumper;
  const ELFO *Obj;
  ScopedPrinter &W;
  Optional<uint64_t> DtPltGot;
  Optional<uint64_t> DtLocalGotNum;
  Optional<uint64_t> DtGotSym;
  Optional<uint64_t> DtMipsPltGot;
  Optional<uint64_t> DtJmpRel;

  std::size_t getGOTTotal(ArrayRef<uint8_t> GOT) const;
  const GOTEntry *makeGOTIter(ArrayRef<uint8_t> GOT, std::size_t EntryNum);

  void printGotEntry(uint64_t GotAddr, const GOTEntry *BeginIt,
                     const GOTEntry *It);
  void printGlobalGotEntry(uint64_t GotAddr, const GOTEntry *BeginIt,
                           const GOTEntry *It, const Elf_Sym *Sym,
                           StringRef StrTable, bool IsDynamic);
  void printPLTEntry(uint64_t PLTAddr, const GOTEntry *BeginIt,
                     const GOTEntry *It, StringRef Purpose);
  void printPLTEntry(uint64_t PLTAddr, const GOTEntry *BeginIt,
                     const GOTEntry *It, StringRef StrTable,
                     const Elf_Sym *Sym);
};

} // end anonymous namespace

template <class ELFT>
MipsGOTParser<ELFT>::MipsGOTParser(ELFDumper<ELFT> *Dumper, const ELFO *Obj,
                                   Elf_Dyn_Range DynTable, ScopedPrinter &W)
    : Dumper(Dumper), Obj(Obj), W(W) {
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
}

template <class ELFT> void MipsGOTParser<ELFT>::parseGOT() {
  // See "Global Offset Table" in Chapter 5 in the following document
  // for detailed GOT description.
  // ftp://www.linux-mips.org/pub/linux/mips/doc/ABI/mipsabi.pdf
  if (!DtPltGot) {
    W.startLine() << "Cannot find PLTGOT dynamic table tag.\n";
    return;
  }
  if (!DtLocalGotNum) {
    W.startLine() << "Cannot find MIPS_LOCAL_GOTNO dynamic table tag.\n";
    return;
  }
  if (!DtGotSym) {
    W.startLine() << "Cannot find MIPS_GOTSYM dynamic table tag.\n";
    return;
  }

  StringRef StrTable = Dumper->getDynamicStringTable();
  const Elf_Sym *DynSymBegin = Dumper->dynamic_symbols().begin();
  const Elf_Sym *DynSymEnd = Dumper->dynamic_symbols().end();
  std::size_t DynSymTotal = std::size_t(std::distance(DynSymBegin, DynSymEnd));

  if (*DtGotSym > DynSymTotal)
    report_fatal_error("MIPS_GOTSYM exceeds a number of dynamic symbols");

  std::size_t GlobalGotNum = DynSymTotal - *DtGotSym;

  if (*DtLocalGotNum + GlobalGotNum == 0) {
    W.startLine() << "GOT is empty.\n";
    return;
  }

  const Elf_Shdr *GOTShdr = findNotEmptySectionByAddress(Obj, *DtPltGot);
  if (!GOTShdr)
    report_fatal_error("There is no not empty GOT section at 0x" +
                       Twine::utohexstr(*DtPltGot));

  ArrayRef<uint8_t> GOT = unwrapOrError(Obj->getSectionContents(GOTShdr));

  if (*DtLocalGotNum + GlobalGotNum > getGOTTotal(GOT))
    report_fatal_error("Number of GOT entries exceeds the size of GOT section");

  const GOTEntry *GotBegin = makeGOTIter(GOT, 0);
  const GOTEntry *GotLocalEnd = makeGOTIter(GOT, *DtLocalGotNum);
  const GOTEntry *It = GotBegin;

  DictScope GS(W, "Primary GOT");

  W.printHex("Canonical gp value", GOTShdr->sh_addr + 0x7ff0);
  {
    ListScope RS(W, "Reserved entries");

    {
      DictScope D(W, "Entry");
      printGotEntry(GOTShdr->sh_addr, GotBegin, It++);
      W.printString("Purpose", StringRef("Lazy resolver"));
    }

    if (It != GotLocalEnd && (*It >> (sizeof(GOTEntry) * 8 - 1)) != 0) {
      DictScope D(W, "Entry");
      printGotEntry(GOTShdr->sh_addr, GotBegin, It++);
      W.printString("Purpose", StringRef("Module pointer (GNU extension)"));
    }
  }
  {
    ListScope LS(W, "Local entries");
    for (; It != GotLocalEnd; ++It) {
      DictScope D(W, "Entry");
      printGotEntry(GOTShdr->sh_addr, GotBegin, It);
    }
  }
  {
    ListScope GS(W, "Global entries");

    const GOTEntry *GotGlobalEnd =
        makeGOTIter(GOT, *DtLocalGotNum + GlobalGotNum);
    const Elf_Sym *GotDynSym = DynSymBegin + *DtGotSym;
    for (; It != GotGlobalEnd; ++It) {
      DictScope D(W, "Entry");
      printGlobalGotEntry(GOTShdr->sh_addr, GotBegin, It, GotDynSym++, StrTable,
                          true);
    }
  }

  std::size_t SpecGotNum = getGOTTotal(GOT) - *DtLocalGotNum - GlobalGotNum;
  W.printNumber("Number of TLS and multi-GOT entries", uint64_t(SpecGotNum));
}

template <class ELFT> void MipsGOTParser<ELFT>::parsePLT() {
  if (!DtMipsPltGot) {
    W.startLine() << "Cannot find MIPS_PLTGOT dynamic table tag.\n";
    return;
  }
  if (!DtJmpRel) {
    W.startLine() << "Cannot find JMPREL dynamic table tag.\n";
    return;
  }

  const Elf_Shdr *PLTShdr = findNotEmptySectionByAddress(Obj, *DtMipsPltGot);
  if (!PLTShdr)
    report_fatal_error("There is no not empty PLTGOT section at 0x " +
                       Twine::utohexstr(*DtMipsPltGot));
  ArrayRef<uint8_t> PLT = unwrapOrError(Obj->getSectionContents(PLTShdr));

  const Elf_Shdr *PLTRelShdr = findNotEmptySectionByAddress(Obj, *DtJmpRel);
  if (!PLTRelShdr)
    report_fatal_error("There is no not empty RELPLT section at 0x" +
                       Twine::utohexstr(*DtJmpRel));
  const Elf_Shdr *SymTable =
      unwrapOrError(Obj->getSection(PLTRelShdr->sh_link));
  StringRef StrTable = unwrapOrError(Obj->getStringTableForSymtab(*SymTable));

  const GOTEntry *PLTBegin = makeGOTIter(PLT, 0);
  const GOTEntry *PLTEnd = makeGOTIter(PLT, getGOTTotal(PLT));
  const GOTEntry *It = PLTBegin;

  DictScope GS(W, "PLT GOT");
  {
    ListScope RS(W, "Reserved entries");
    printPLTEntry(PLTShdr->sh_addr, PLTBegin, It++, "PLT lazy resolver");
    if (It != PLTEnd)
      printPLTEntry(PLTShdr->sh_addr, PLTBegin, It++, "Module pointer");
  }
  {
    ListScope GS(W, "Entries");

    switch (PLTRelShdr->sh_type) {
    case ELF::SHT_REL:
      for (const Elf_Rel &Rel : unwrapOrError(Obj->rels(PLTRelShdr))) {
        const Elf_Sym *Sym =
            unwrapOrError(Obj->getRelocationSymbol(&Rel, SymTable));
        printPLTEntry(PLTShdr->sh_addr, PLTBegin, It, StrTable, Sym);
        if (++It == PLTEnd)
          break;
      }
      break;
    case ELF::SHT_RELA:
      for (const Elf_Rela &Rel : unwrapOrError(Obj->relas(PLTRelShdr))) {
        const Elf_Sym *Sym =
            unwrapOrError(Obj->getRelocationSymbol(&Rel, SymTable));
        printPLTEntry(PLTShdr->sh_addr, PLTBegin, It, StrTable, Sym);
        if (++It == PLTEnd)
          break;
      }
      break;
    }
  }
}

template <class ELFT>
std::size_t MipsGOTParser<ELFT>::getGOTTotal(ArrayRef<uint8_t> GOT) const {
  return GOT.size() / sizeof(GOTEntry);
}

template <class ELFT>
const typename MipsGOTParser<ELFT>::GOTEntry *
MipsGOTParser<ELFT>::makeGOTIter(ArrayRef<uint8_t> GOT, std::size_t EntryNum) {
  const char *Data = reinterpret_cast<const char *>(GOT.data());
  return reinterpret_cast<const GOTEntry *>(Data + EntryNum * sizeof(GOTEntry));
}

template <class ELFT>
void MipsGOTParser<ELFT>::printGotEntry(uint64_t GotAddr,
                                        const GOTEntry *BeginIt,
                                        const GOTEntry *It) {
  int64_t Offset = std::distance(BeginIt, It) * sizeof(GOTEntry);
  W.printHex("Address", GotAddr + Offset);
  W.printNumber("Access", Offset - 0x7ff0);
  W.printHex("Initial", *It);
}

template <class ELFT>
void MipsGOTParser<ELFT>::printGlobalGotEntry(
    uint64_t GotAddr, const GOTEntry *BeginIt, const GOTEntry *It,
    const Elf_Sym *Sym, StringRef StrTable, bool IsDynamic) {
  printGotEntry(GotAddr, BeginIt, It);

  W.printHex("Value", Sym->st_value);
  W.printEnum("Type", Sym->getType(), makeArrayRef(ElfSymbolTypes));

  unsigned SectionIndex = 0;
  StringRef SectionName;
  getSectionNameIndex(*Obj, Sym, Dumper->dynamic_symbols().begin(),
                      Dumper->getShndxTable(), SectionName, SectionIndex);
  W.printHex("Section", SectionName, SectionIndex);

  std::string FullSymbolName =
      Dumper->getFullSymbolName(Sym, StrTable, IsDynamic);
  W.printNumber("Name", FullSymbolName, Sym->st_name);
}

template <class ELFT>
void MipsGOTParser<ELFT>::printPLTEntry(uint64_t PLTAddr,
                                        const GOTEntry *BeginIt,
                                        const GOTEntry *It, StringRef Purpose) {
  DictScope D(W, "Entry");
  int64_t Offset = std::distance(BeginIt, It) * sizeof(GOTEntry);
  W.printHex("Address", PLTAddr + Offset);
  W.printHex("Initial", *It);
  W.printString("Purpose", Purpose);
}

template <class ELFT>
void MipsGOTParser<ELFT>::printPLTEntry(uint64_t PLTAddr,
                                        const GOTEntry *BeginIt,
                                        const GOTEntry *It, StringRef StrTable,
                                        const Elf_Sym *Sym) {
  DictScope D(W, "Entry");
  int64_t Offset = std::distance(BeginIt, It) * sizeof(GOTEntry);
  W.printHex("Address", PLTAddr + Offset);
  W.printHex("Initial", *It);
  W.printHex("Value", Sym->st_value);
  W.printEnum("Type", Sym->getType(), makeArrayRef(ElfSymbolTypes));

  unsigned SectionIndex = 0;
  StringRef SectionName;
  getSectionNameIndex(*Obj, Sym, Dumper->dynamic_symbols().begin(),
                      Dumper->getShndxTable(), SectionName, SectionIndex);
  W.printHex("Section", SectionName, SectionIndex);

  std::string FullSymbolName = Dumper->getFullSymbolName(Sym, StrTable, true);
  W.printNumber("Name", FullSymbolName, Sym->st_name);
}

template <class ELFT> void ELFDumper<ELFT>::printMipsPLTGOT() {
  if (Obj->getHeader()->e_machine != EM_MIPS) {
    W.startLine() << "MIPS PLT GOT is available for MIPS targets only.\n";
    return;
  }

  MipsGOTParser<ELFT> GOTParser(this, Obj, dynamic_table(), W);
  GOTParser.parseGOT();
  GOTParser.parsePLT();
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
  {"XPA",                Mips::AFL_ASE_XPA}
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

template <class ELFT> void ELFDumper<ELFT>::printAMDGPUCodeObjectMetadata() {
  const Elf_Shdr *Shdr = findSectionByName(*Obj, ".note");
  if (!Shdr) {
    W.startLine() << "There is no .note section in the file.\n";
    return;
  }
  ArrayRef<uint8_t> Sec = unwrapOrError(Obj->getSectionContents(Shdr));

  const uint32_t CodeObjectMetadataNoteType = 10;
  for (auto I = reinterpret_cast<const Elf_Word *>(&Sec[0]),
       E = I + Sec.size()/4; I != E;) {
    uint32_t NameSZ = I[0];
    uint32_t DescSZ = I[1];
    uint32_t Type = I[2];
    I += 3;

    StringRef Name;
    if (NameSZ) {
      Name = StringRef(reinterpret_cast<const char *>(I), NameSZ - 1);
      I += alignTo<4>(NameSZ)/4;
    }

    if (Name == "AMD" && Type == CodeObjectMetadataNoteType) {
      StringRef Desc(reinterpret_cast<const char *>(I), DescSZ);
      W.printString(Desc);
    }
    I += alignTo<4>(DescSZ)/4;
  }
}

template <class ELFT> void ELFDumper<ELFT>::printStackMap() const {
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

  StringRef StackMapContents;
  ArrayRef<uint8_t> StackMapContentsArray =
      unwrapOrError(Obj->getSectionContents(StackMapSection));

  prettyPrintStackMap(outs(), StackMapV2Parser<ELFT::TargetEndianness>(
                                  StackMapContentsArray));
}

template <class ELFT> void ELFDumper<ELFT>::printGroupSections() {
  ELFDumperStyle->printGroupSections(Obj);
}

static inline void printFields(formatted_raw_ostream &OS, StringRef Str1,
                               StringRef Str2) {
  OS.PadToColumn(2u);
  OS << Str1;
  OS.PadToColumn(37u);
  OS << Str2 << "\n";
  OS.flush();
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
  Str = "0x" + to_hexString(e->e_flags);
  printFields(OS, "Flags:", Str);
  Str = to_string(e->e_ehsize) + " (bytes)";
  printFields(OS, "Size of this header:", Str);
  Str = to_string(e->e_phentsize) + " (bytes)";
  printFields(OS, "Size of program headers:", Str);
  Str = to_string(e->e_phnum);
  printFields(OS, "Number of program headers:", Str);
  Str = to_string(e->e_shentsize) + " (bytes)";
  printFields(OS, "Size of section headers:", Str);
  Str = to_string(e->e_shnum);
  printFields(OS, "Number of section headers:", Str);
  Str = to_string(e->e_shstrndx);
  printFields(OS, "Section header string table index:", Str);
}

template <class ELFT> void GNUStyle<ELFT>::printGroupSections(const ELFO *Obj) {
  uint32_t SectionIndex = 0;
  bool HasGroups = false;
  for (const Elf_Shdr &Sec : unwrapOrError(Obj->sections())) {
    if (Sec.sh_type == ELF::SHT_GROUP) {
      HasGroups = true;
      const Elf_Shdr *Symtab = unwrapOrError(Obj->getSection(Sec.sh_link));
      StringRef StrTable = unwrapOrError(Obj->getStringTableForSymtab(*Symtab));
      const Elf_Sym *Signature =
          unwrapOrError(Obj->template getEntry<Elf_Sym>(Symtab, Sec.sh_info));
      ArrayRef<Elf_Word> Data = unwrapOrError(
          Obj->template getSectionContentsAsArray<Elf_Word>(&Sec));
      StringRef Name = unwrapOrError(Obj->getSectionName(&Sec));
      OS << "\n" << getGroupType(Data[0]) << " group section ["
         << format_decimal(SectionIndex, 5) << "] `" << Name << "' ["
         << StrTable.data() + Signature->st_name << "] contains "
         << (Data.size() - 1) << " sections:\n"
         << "   [Index]    Name\n";
      for (auto &Ndx : Data.slice(1)) {
        auto Sec = unwrapOrError(Obj->getSection(Ndx));
        const StringRef Name = unwrapOrError(Obj->getSectionName(Sec));
        OS << "   [" << format_decimal(Ndx, 5) << "]   " << Name
           << "\n";
      }
    }
    ++SectionIndex;
  }
  if (!HasGroups)
    OS << "There are no section groups in this file.\n";
}

template <class ELFT>
void GNUStyle<ELFT>::printRelocation(const ELFO *Obj, const Elf_Shdr *SymTab,
                                     const Elf_Rela &R, bool IsRela) {
  std::string Offset, Info, Addend, Value;
  SmallString<32> RelocName;
  StringRef StrTable = unwrapOrError(Obj->getStringTableForSymtab(*SymTab));
  StringRef TargetName;
  const Elf_Sym *Sym = nullptr;
  unsigned Width = ELFT::Is64Bits ? 16 : 8;
  unsigned Bias = ELFT::Is64Bits ? 8 : 0;

  // First two fields are bit width dependent. The rest of them are after are
  // fixed width.
  Field Fields[5] = {0, 10 + Bias, 19 + 2 * Bias, 42 + 2 * Bias, 53 + 2 * Bias};
  Obj->getRelocationTypeName(R.getType(Obj->isMips64EL()), RelocName);
  Sym = unwrapOrError(Obj->getRelocationSymbol(&R, SymTab));
  if (Sym && Sym->getType() == ELF::STT_SECTION) {
    const Elf_Shdr *Sec = unwrapOrError(
        Obj->getSection(Sym, SymTab, this->dumper()->getShndxTable()));
    TargetName = unwrapOrError(Obj->getSectionName(Sec));
  } else if (Sym) {
    TargetName = unwrapOrError(Sym->getName(StrTable));
  }

  if (Sym && IsRela) {
    if (R.r_addend < 0)
      Addend = " - ";
    else
      Addend = " + ";
  }

  Offset = to_string(format_hex_no_prefix(R.r_offset, Width));
  Info = to_string(format_hex_no_prefix(R.r_info, Width));

  int64_t RelAddend = R.r_addend;
  if (IsRela)
    Addend += to_hexString(std::abs(RelAddend), false);

  if (Sym)
    Value = to_string(format_hex_no_prefix(Sym->getValue(), Width));

  Fields[0].Str = Offset;
  Fields[1].Str = Info;
  Fields[2].Str = RelocName;
  Fields[3].Str = Value;
  Fields[4].Str = TargetName;
  for (auto &field : Fields)
    printField(field);
  OS << Addend;
  OS << "\n";
}

static inline void printRelocHeader(raw_ostream &OS, bool Is64, bool IsRela) {
  if (Is64)
    OS << "    Offset             Info             Type"
       << "               Symbol's Value  Symbol's Name";
  else
    OS << " Offset     Info    Type                Sym. Value  "
       << "Symbol's Name";
  if (IsRela)
    OS << (IsRela ? " + Addend" : "");
  OS << "\n";
}

template <class ELFT> void GNUStyle<ELFT>::printRelocations(const ELFO *Obj) {
  bool HasRelocSections = false;
  for (const Elf_Shdr &Sec : unwrapOrError(Obj->sections())) {
    if (Sec.sh_type != ELF::SHT_REL && Sec.sh_type != ELF::SHT_RELA)
      continue;
    HasRelocSections = true;
    StringRef Name = unwrapOrError(Obj->getSectionName(&Sec));
    unsigned Entries = Sec.getEntityCount();
    uintX_t Offset = Sec.sh_offset;
    OS << "\nRelocation section '" << Name << "' at offset 0x"
       << to_hexString(Offset, false) << " contains " << Entries
       << " entries:\n";
    printRelocHeader(OS,  ELFT::Is64Bits, (Sec.sh_type == ELF::SHT_RELA));
    const Elf_Shdr *SymTab = unwrapOrError(Obj->getSection(Sec.sh_link));
    if (Sec.sh_type == ELF::SHT_REL) {
      for (const auto &R : unwrapOrError(Obj->rels(&Sec))) {
        Elf_Rela Rela;
        Rela.r_offset = R.r_offset;
        Rela.r_info = R.r_info;
        Rela.r_addend = 0;
        printRelocation(Obj, SymTab, Rela, false);
      }
    } else {
      for (const auto &R : unwrapOrError(Obj->relas(&Sec)))
        printRelocation(Obj, SymTab, R, true);
    }
  }
  if (!HasRelocSections)
    OS << "\nThere are no relocations in this file.\n";
}

std::string getSectionTypeString(unsigned Arch, unsigned Type) {
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
  case EM_X86_64:
    switch (Type) {
    case SHT_X86_64_UNWIND:
      return "X86_64_UNWIND";
    }
  case EM_MIPS:
  case EM_MIPS_RS3_LE:
    switch (Type) {
    case SHT_MIPS_REGINFO:
      return "MIPS_REGINFO";
    case SHT_MIPS_OPTIONS:
      return "MIPS_OPTIONS";
    case SHT_MIPS_ABIFLAGS:
      return "MIPS_ABIFLAGS";
    case SHT_MIPS_DWARF:
      return "SHT_MIPS_DWARF";
    }
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
  case SHT_LLVM_ODRTAB:
    return "LLVM_ODRTAB";
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
    return "";
  }
  return "";
}

template <class ELFT> void GNUStyle<ELFT>::printSections(const ELFO *Obj) {
  size_t SectionIndex = 0;
  std::string Number, Type, Size, Address, Offset, Flags, Link, Info, EntrySize,
      Alignment;
  unsigned Bias;
  unsigned Width;

  if (ELFT::Is64Bits) {
    Bias = 0;
    Width = 16;
  } else {
    Bias = 8;
    Width = 8;
  }
  OS << "There are " << to_string(Obj->getHeader()->e_shnum)
     << " section headers, starting at offset "
     << "0x" << to_hexString(Obj->getHeader()->e_shoff, false) << ":\n\n";
  OS << "Section Headers:\n";
  Field Fields[11] = {{"[Nr]", 2},
                      {"Name", 7},
                      {"Type", 25},
                      {"Address", 41},
                      {"Off", 58 - Bias},
                      {"Size", 65 - Bias},
                      {"ES", 72 - Bias},
                      {"Flg", 75 - Bias},
                      {"Lk", 79 - Bias},
                      {"Inf", 82 - Bias},
                      {"Al", 86 - Bias}};
  for (auto &f : Fields)
    printField(f);
  OS << "\n";

  for (const Elf_Shdr &Sec : unwrapOrError(Obj->sections())) {
    Number = to_string(SectionIndex);
    Fields[0].Str = Number;
    Fields[1].Str = unwrapOrError(Obj->getSectionName(&Sec));
    Type = getSectionTypeString(Obj->getHeader()->e_machine, Sec.sh_type);
    Fields[2].Str = Type;
    Address = to_string(format_hex_no_prefix(Sec.sh_addr, Width));
    Fields[3].Str = Address;
    Offset = to_string(format_hex_no_prefix(Sec.sh_offset, 6));
    Fields[4].Str = Offset;
    Size = to_string(format_hex_no_prefix(Sec.sh_size, 6));
    Fields[5].Str = Size;
    EntrySize = to_string(format_hex_no_prefix(Sec.sh_entsize, 2));
    Fields[6].Str = EntrySize;
    Flags = getGNUFlags(Sec.sh_flags);
    Fields[7].Str = Flags;
    Link = to_string(Sec.sh_link);
    Fields[8].Str = Link;
    Info = to_string(Sec.sh_info);
    Fields[9].Str = Info;
    Alignment = to_string(Sec.sh_addralign);
    Fields[10].Str = Alignment;
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
    SectionIndex = unwrapOrError(object::getExtendedSymbolTableIndex<ELFT>(
        Symbol, FirstSym, this->dumper()->getShndxTable()));
    LLVM_FALLTHROUGH;
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
  size_t Width;

  // If this function was called with a different value from IsDynamic
  // from last call, happens when we move from dynamic to static symbol
  // table, "Num" field should be reset.
  if (!Dynamic != !IsDynamic) {
    Idx = 0;
    Dynamic = false;
  }
  std::string Num, Name, Value, Size, Binding, Type, Visibility, Section;
  unsigned Bias = 0;
  if (ELFT::Is64Bits) {
    Bias = 8;
    Width = 16;
  } else {
    Bias = 0;
    Width = 8;
  }
  Field Fields[8] = {0,         8,         17 + Bias, 23 + Bias,
                     31 + Bias, 38 + Bias, 47 + Bias, 51 + Bias};
  Num = to_string(format_decimal(Idx++, 6)) + ":";
  Value = to_string(format_hex_no_prefix(Symbol->st_value, Width));
  Size = to_string(format_decimal(Symbol->st_size, 5));
  unsigned char SymbolType = Symbol->getType();
  if (Obj->getHeader()->e_machine == ELF::EM_AMDGPU &&
      SymbolType >= ELF::STT_LOOS && SymbolType < ELF::STT_HIOS)
    Type = printEnum(SymbolType, makeArrayRef(AMDGPUSymbolTypes));
  else
    Type = printEnum(SymbolType, makeArrayRef(ElfSymbolTypes));
  unsigned Vis = Symbol->getVisibility();
  Binding = printEnum(Symbol->getBinding(), makeArrayRef(ElfSymbolBindings));
  Visibility = printEnum(Vis, makeArrayRef(ElfSymbolVisibilities));
  Section = getSymbolSectionNdx(Obj, Symbol, FirstSym);
  Name = this->dumper()->getFullSymbolName(Symbol, StrTable, IsDynamic);
  Fields[0].Str = Num;
  Fields[1].Str = Value;
  Fields[2].Str = Size;
  Fields[3].Str = Type;
  Fields[4].Str = Binding;
  Fields[5].Str = Visibility;
  Fields[6].Str = Section;
  Fields[7].Str = Name;
  for (auto &Entry : Fields)
    printField(Entry);
  OS << "\n";
}
template <class ELFT>
void GNUStyle<ELFT>::printHashedSymbol(const ELFO *Obj, const Elf_Sym *FirstSym,
                                       uint32_t Sym, StringRef StrTable,
                                       uint32_t Bucket) {
  std::string Num, Buc, Name, Value, Size, Binding, Type, Visibility, Section;
  unsigned Width, Bias = 0;
  if (ELFT::Is64Bits) {
    Bias = 8;
    Width = 16;
  } else {
    Bias = 0;
    Width = 8;
  }
  Field Fields[9] = {0,         6,         11,        20 + Bias, 25 + Bias,
                     34 + Bias, 41 + Bias, 49 + Bias, 53 + Bias};
  Num = to_string(format_decimal(Sym, 5));
  Buc = to_string(format_decimal(Bucket, 3)) + ":";

  const auto Symbol = FirstSym + Sym;
  Value = to_string(format_hex_no_prefix(Symbol->st_value, Width));
  Size = to_string(format_decimal(Symbol->st_size, 5));
  unsigned char SymbolType = Symbol->getType();
  if (Obj->getHeader()->e_machine == ELF::EM_AMDGPU &&
      SymbolType >= ELF::STT_LOOS && SymbolType < ELF::STT_HIOS)
    Type = printEnum(SymbolType, makeArrayRef(AMDGPUSymbolTypes));
  else
    Type = printEnum(SymbolType, makeArrayRef(ElfSymbolTypes));
  unsigned Vis = Symbol->getVisibility();
  Binding = printEnum(Symbol->getBinding(), makeArrayRef(ElfSymbolBindings));
  Visibility = printEnum(Vis, makeArrayRef(ElfSymbolVisibilities));
  Section = getSymbolSectionNdx(Obj, Symbol, FirstSym);
  Name = this->dumper()->getFullSymbolName(Symbol, StrTable, true);
  Fields[0].Str = Num;
  Fields[1].Str = Buc;
  Fields[2].Str = Value;
  Fields[3].Str = Size;
  Fields[4].Str = Type;
  Fields[5].Str = Binding;
  Fields[6].Str = Visibility;
  Fields[7].Str = Section;
  Fields[8].Str = Name;
  for (auto &Entry : Fields)
    printField(Entry);
  OS << "\n";
}

template <class ELFT> void GNUStyle<ELFT>::printSymbols(const ELFO *Obj) {
  if (opts::DynamicSymbols)
    return;
  this->dumper()->printSymbolsHelper(true);
  this->dumper()->printSymbolsHelper(false);
}

template <class ELFT>
void GNUStyle<ELFT>::printDynamicSymbols(const ELFO *Obj) {
  if (this->dumper()->getDynamicStringTable().empty())
    return;
  auto StringTable = this->dumper()->getDynamicStringTable();
  auto DynSyms = this->dumper()->dynamic_symbols();
  auto GnuHash = this->dumper()->getGnuHashTable();
  auto SysVHash = this->dumper()->getHashTable();

  // If no hash or .gnu.hash found, try using symbol table
  if (GnuHash == nullptr && SysVHash == nullptr)
    this->dumper()->printSymbolsHelper(true);

  // Try printing .hash
  if (this->dumper()->getHashTable()) {
    OS << "\n Symbol table of .hash for image:\n";
    if (ELFT::Is64Bits)
      OS << "  Num Buc:    Value          Size   Type   Bind Vis      Ndx Name";
    else
      OS << "  Num Buc:    Value  Size   Type   Bind Vis      Ndx Name";
    OS << "\n";

    uint32_t NBuckets = SysVHash->nbucket;
    uint32_t NChains = SysVHash->nchain;
    auto Buckets = SysVHash->buckets();
    auto Chains = SysVHash->chains();
    for (uint32_t Buc = 0; Buc < NBuckets; Buc++) {
      if (Buckets[Buc] == ELF::STN_UNDEF)
        continue;
      for (uint32_t Ch = Buckets[Buc]; Ch < NChains; Ch = Chains[Ch]) {
        if (Ch == ELF::STN_UNDEF)
          break;
        printHashedSymbol(Obj, &DynSyms[0], Ch, StringTable, Buc);
      }
    }
  }

  // Try printing .gnu.hash
  if (GnuHash) {
    OS << "\n Symbol table of .gnu.hash for image:\n";
    if (ELFT::Is64Bits)
      OS << "  Num Buc:    Value          Size   Type   Bind Vis      Ndx Name";
    else
      OS << "  Num Buc:    Value  Size   Type   Bind Vis      Ndx Name";
    OS << "\n";
    uint32_t NBuckets = GnuHash->nbuckets;
    auto Buckets = GnuHash->buckets();
    for (uint32_t Buc = 0; Buc < NBuckets; Buc++) {
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
            /*only non-zero sized sections at end*/ &&
            (Sec.sh_offset + 1 <= Phdr.p_offset + Phdr.p_filesz));
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
void GNUStyle<ELFT>::printProgramHeaders(const ELFO *Obj) {
  unsigned Bias = ELFT::Is64Bits ? 8 : 0;
  unsigned Width = ELFT::Is64Bits ? 18 : 10;
  unsigned SizeWidth = ELFT::Is64Bits ? 8 : 7;
  std::string Type, Offset, VMA, LMA, FileSz, MemSz, Flag, Align;

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
  for (const auto &Phdr : unwrapOrError(Obj->program_headers())) {
    Type = getElfPtType(Header->e_machine, Phdr.p_type);
    Offset = to_string(format_hex(Phdr.p_offset, 8));
    VMA = to_string(format_hex(Phdr.p_vaddr, Width));
    LMA = to_string(format_hex(Phdr.p_paddr, Width));
    FileSz = to_string(format_hex(Phdr.p_filesz, SizeWidth));
    MemSz = to_string(format_hex(Phdr.p_memsz, SizeWidth));
    Flag = printPhdrFlags(Phdr.p_flags);
    Align = to_string(format_hex(Phdr.p_align, 1));
    Fields[0].Str = Type;
    Fields[1].Str = Offset;
    Fields[2].Str = VMA;
    Fields[3].Str = LMA;
    Fields[4].Str = FileSz;
    Fields[5].Str = MemSz;
    Fields[6].Str = Flag;
    Fields[7].Str = Align;
    for (auto Field : Fields)
      printField(Field);
    if (Phdr.p_type == ELF::PT_INTERP) {
      OS << "\n      [Requesting program interpreter: ";
      OS << reinterpret_cast<const char *>(Obj->base()) + Phdr.p_offset << "]";
    }
    OS << "\n";
  }
  OS << "\n Section to Segment mapping:\n  Segment Sections...\n";
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
          checkPTDynamic(Phdr, Sec) && (Sec.sh_type != ELF::SHT_NULL))
        Sections += unwrapOrError(Obj->getSectionName(&Sec)).str() + " ";
    }
    OS << Sections << "\n";
    OS.flush();
  }
}

template <class ELFT>
void GNUStyle<ELFT>::printDynamicRelocation(const ELFO *Obj, Elf_Rela R,
                                            bool IsRela) {
  SmallString<32> RelocName;
  StringRef SymbolName;
  unsigned Width = ELFT::Is64Bits ? 16 : 8;
  unsigned Bias = ELFT::Is64Bits ? 8 : 0;
  // First two fields are bit width dependent. The rest of them are after are
  // fixed width.
  Field Fields[5] = {0, 10 + Bias, 19 + 2 * Bias, 42 + 2 * Bias, 53 + 2 * Bias};

  uint32_t SymIndex = R.getSymbol(Obj->isMips64EL());
  const Elf_Sym *Sym = this->dumper()->dynamic_symbols().begin() + SymIndex;
  Obj->getRelocationTypeName(R.getType(Obj->isMips64EL()), RelocName);
  SymbolName =
      unwrapOrError(Sym->getName(this->dumper()->getDynamicStringTable()));
  std::string Addend, Info, Offset, Value;
  Offset = to_string(format_hex_no_prefix(R.r_offset, Width));
  Info = to_string(format_hex_no_prefix(R.r_info, Width));
  Value = to_string(format_hex_no_prefix(Sym->getValue(), Width));
  int64_t RelAddend = R.r_addend;
  if (!SymbolName.empty() && IsRela) {
    if (R.r_addend < 0)
      Addend = " - ";
    else
      Addend = " + ";
  }

  if (SymbolName.empty() && Sym->getValue() == 0)
    Value = "";

  if (IsRela)
    Addend += to_string(format_hex_no_prefix(std::abs(RelAddend), 1));


  Fields[0].Str = Offset;
  Fields[1].Str = Info;
  Fields[2].Str = RelocName.c_str();
  Fields[3].Str = Value;
  Fields[4].Str = SymbolName;
  for (auto &Field : Fields)
    printField(Field);
  OS << Addend;
  OS << "\n";
}

template <class ELFT>
void GNUStyle<ELFT>::printDynamicRelocations(const ELFO *Obj) {
  const DynRegionInfo &DynRelRegion = this->dumper()->getDynRelRegion();
  const DynRegionInfo &DynRelaRegion = this->dumper()->getDynRelaRegion();
  const DynRegionInfo &DynPLTRelRegion = this->dumper()->getDynPLTRelRegion();
  if (DynRelaRegion.Size > 0) {
    OS << "\n'RELA' relocation section at offset "
       << format_hex(reinterpret_cast<const uint8_t *>(DynRelaRegion.Addr) -
                         Obj->base(),
                     1) << " contains " << DynRelaRegion.Size << " bytes:\n";
    printRelocHeader(OS, ELFT::Is64Bits, true);
    for (const Elf_Rela &Rela : this->dumper()->dyn_relas())
      printDynamicRelocation(Obj, Rela, true);
  }
  if (DynRelRegion.Size > 0) {
    OS << "\n'REL' relocation section at offset "
       << format_hex(reinterpret_cast<const uint8_t *>(DynRelRegion.Addr) -
                         Obj->base(),
                     1) << " contains " << DynRelRegion.Size << " bytes:\n";
    printRelocHeader(OS, ELFT::Is64Bits, false);
    for (const Elf_Rel &Rel : this->dumper()->dyn_rels()) {
      Elf_Rela Rela;
      Rela.r_offset = Rel.r_offset;
      Rela.r_info = Rel.r_info;
      Rela.r_addend = 0;
      printDynamicRelocation(Obj, Rela, false);
    }
  }
  if (DynPLTRelRegion.Size) {
    OS << "\n'PLT' relocation section at offset "
       << format_hex(reinterpret_cast<const uint8_t *>(DynPLTRelRegion.Addr) -
                         Obj->base(),
                     1) << " contains " << DynPLTRelRegion.Size << " bytes:\n";
  }
  if (DynPLTRelRegion.EntSize == sizeof(Elf_Rela)) {
    printRelocHeader(OS, ELFT::Is64Bits, true);
    for (const Elf_Rela &Rela : DynPLTRelRegion.getAsArrayRef<Elf_Rela>())
      printDynamicRelocation(Obj, Rela, true);
  } else {
    printRelocHeader(OS, ELFT::Is64Bits, false);
    for (const Elf_Rel &Rel : DynPLTRelRegion.getAsArrayRef<Elf_Rel>()) {
      Elf_Rela Rela;
      Rela.r_offset = Rel.r_offset;
      Rela.r_info = Rel.r_info;
      Rela.r_addend = 0;
      printDynamicRelocation(Obj, Rela, false);
    }
  }
}

// Hash histogram shows  statistics of how efficient the hash was for the
// dynamic symbol table. The table shows number of hash buckets for different
// lengths of chains as absolute number and percentage of the total buckets.
// Additionally cumulative coverage of symbols for each set of buckets.
template <class ELFT>
void GNUStyle<ELFT>::printHashHistogram(const ELFFile<ELFT> *Obj) {

  const Elf_Hash *HashTable = this->dumper()->getHashTable();
  const Elf_GnuHash *GnuHashTable = this->dumper()->getGnuHashTable();

  // Print histogram for .hash section
  if (HashTable) {
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
  if (GnuHashTable) {
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

static std::string getGNUNoteTypeName(const uint32_t NT) {
  static const struct {
    uint32_t ID;
    const char *Name;
  } Notes[] = {
      {ELF::NT_GNU_ABI_TAG, "NT_GNU_ABI_TAG (ABI version tag)"},
      {ELF::NT_GNU_HWCAP, "NT_GNU_HWCAP (DSO-supplied software HWCAP info)"},
      {ELF::NT_GNU_BUILD_ID, "NT_GNU_BUILD_ID (unique build ID bitstring)"},
      {ELF::NT_GNU_GOLD_VERSION, "NT_GNU_GOLD_VERSION (gold version)"},
  };

  for (const auto &Note : Notes)
    if (Note.ID == NT)
      return std::string(Note.Name);

  std::string string;
  raw_string_ostream OS(string);
  OS << format("Unknown note type (0x%08x)", NT);
  return string;
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
  return string;
}

template <typename ELFT>
static void printGNUNote(raw_ostream &OS, uint32_t NoteType,
                         ArrayRef<typename ELFFile<ELFT>::Elf_Word> Words,
                         size_t Size) {
  switch (NoteType) {
  default:
    return;
  case ELF::NT_GNU_ABI_TAG: {
    static const char *OSNames[] = {
        "Linux", "Hurd", "Solaris", "FreeBSD", "NetBSD", "Syllable", "NaCl",
    };

    StringRef OSName = "Unknown";
    if (Words[0] < array_lengthof(OSNames))
      OSName = OSNames[Words[0]];
    uint32_t Major = Words[1], Minor = Words[2], Patch = Words[3];

    if (Words.size() < 4)
      OS << "    <corrupt GNU_ABI_TAG>";
    else
      OS << "    OS: " << OSName << ", ABI: " << Major << "." << Minor << "."
         << Patch;
    break;
  }
  case ELF::NT_GNU_BUILD_ID: {
    OS << "    Build ID: ";
    ArrayRef<uint8_t> ID(reinterpret_cast<const uint8_t *>(Words.data()), Size);
    for (const auto &B : ID)
      OS << format_hex_no_prefix(B, 2);
    break;
  }
  case ELF::NT_GNU_GOLD_VERSION:
    OS << "    Version: "
       << StringRef(reinterpret_cast<const char *>(Words.data()), Size);
    break;
  }

  OS << '\n';
}

template <class ELFT>
void GNUStyle<ELFT>::printNotes(const ELFFile<ELFT> *Obj) {
  const Elf_Ehdr *e = Obj->getHeader();
  bool IsCore = e->e_type == ELF::ET_CORE;

  auto process = [&](const typename ELFFile<ELFT>::Elf_Off Offset,
                     const typename ELFFile<ELFT>::Elf_Addr Size) {
    if (Size <= 0)
      return;

    const auto *P = static_cast<const uint8_t *>(Obj->base() + Offset);
    const auto *E = P + Size;

    OS << "Displaying notes found at file offset " << format_hex(Offset, 10)
       << " with length " << format_hex(Size, 10) << ":\n"
       << "  Owner                 Data size\tDescription\n";

    while (P < E) {
      const Elf_Word *Words = reinterpret_cast<const Elf_Word *>(&P[0]);

      uint32_t NameSize = Words[0];
      uint32_t DescriptorSize = Words[1];
      uint32_t Type = Words[2];

      ArrayRef<Elf_Word> Descriptor(&Words[3 + (alignTo<4>(NameSize) / 4)],
                                    alignTo<4>(DescriptorSize) / 4);

      StringRef Name;
      if (NameSize)
        Name =
            StringRef(reinterpret_cast<const char *>(&Words[3]), NameSize - 1);

      OS << "  " << Name << std::string(22 - NameSize, ' ')
         << format_hex(DescriptorSize, 10) << '\t';

      if (Name == "GNU") {
        OS << getGNUNoteTypeName(Type) << '\n';
        printGNUNote<ELFT>(OS, Type, Descriptor, DescriptorSize);
      } else if (Name == "FreeBSD") {
        OS << getFreeBSDNoteTypeName(Type) << '\n';
      } else {
        OS << "Unknown note type: (" << format_hex(Type, 10) << ')';
      }
      OS << '\n';

      P = P + 3 * sizeof(Elf_Word) + alignTo<4>(NameSize) +
          alignTo<4>(DescriptorSize);
    }
  };

  if (IsCore) {
    for (const auto &P : unwrapOrError(Obj->program_headers()))
      if (P.p_type == PT_NOTE)
        process(P.p_offset, P.p_filesz);
  } else {
    for (const auto &S : unwrapOrError(Obj->sections()))
      if (S.sh_type == SHT_NOTE)
        process(S.sh_offset, S.sh_size);
  }
}

template <class ELFT> void LLVMStyle<ELFT>::printFileHeaders(const ELFO *Obj) {
  const Elf_Ehdr *e = Obj->getHeader();
  {
    DictScope D(W, "ElfHeader");
    {
      DictScope D(W, "Ident");
      W.printBinary("Magic", makeArrayRef(e->e_ident).slice(ELF::EI_MAG0, 4));
      W.printEnum("Class", e->e_ident[ELF::EI_CLASS], makeArrayRef(ElfClass));
      W.printEnum("DataEncoding", e->e_ident[ELF::EI_DATA],
                  makeArrayRef(ElfDataEncoding));
      W.printNumber("FileVersion", e->e_ident[ELF::EI_VERSION]);

      // Handle architecture specific OS/ABI values.
      if (e->e_machine == ELF::EM_AMDGPU &&
          e->e_ident[ELF::EI_OSABI] == ELF::ELFOSABI_AMDGPU_HSA)
        W.printHex("OS/ABI", "AMDGPU_HSA", ELF::ELFOSABI_AMDGPU_HSA);
      else
        W.printEnum("OS/ABI", e->e_ident[ELF::EI_OSABI],
                    makeArrayRef(ElfOSABI));
      W.printNumber("ABIVersion", e->e_ident[ELF::EI_ABIVERSION]);
      W.printBinary("Unused", makeArrayRef(e->e_ident).slice(ELF::EI_PAD));
    }

    W.printEnum("Type", e->e_type, makeArrayRef(ElfObjectFileType));
    W.printEnum("Machine", e->e_machine, makeArrayRef(ElfMachineType));
    W.printNumber("Version", e->e_version);
    W.printHex("Entry", e->e_entry);
    W.printHex("ProgramHeaderOffset", e->e_phoff);
    W.printHex("SectionHeaderOffset", e->e_shoff);
    if (e->e_machine == EM_MIPS)
      W.printFlags("Flags", e->e_flags, makeArrayRef(ElfHeaderMipsFlags),
                   unsigned(ELF::EF_MIPS_ARCH), unsigned(ELF::EF_MIPS_ABI),
                   unsigned(ELF::EF_MIPS_MACH));
    else
      W.printFlags("Flags", e->e_flags);
    W.printNumber("HeaderSize", e->e_ehsize);
    W.printNumber("ProgramHeaderEntrySize", e->e_phentsize);
    W.printNumber("ProgramHeaderCount", e->e_phnum);
    W.printNumber("SectionHeaderEntrySize", e->e_shentsize);
    W.printNumber("SectionHeaderCount", e->e_shnum);
    W.printNumber("StringTableSectionIndex", e->e_shstrndx);
  }
}

template <class ELFT>
void LLVMStyle<ELFT>::printGroupSections(const ELFO *Obj) {
  DictScope Lists(W, "Groups");
  uint32_t SectionIndex = 0;
  bool HasGroups = false;
  for (const Elf_Shdr &Sec : unwrapOrError(Obj->sections())) {
    if (Sec.sh_type == ELF::SHT_GROUP) {
      HasGroups = true;
      const Elf_Shdr *Symtab = unwrapOrError(Obj->getSection(Sec.sh_link));
      StringRef StrTable = unwrapOrError(Obj->getStringTableForSymtab(*Symtab));
      const Elf_Sym *Sym =
          unwrapOrError(Obj->template getEntry<Elf_Sym>(Symtab, Sec.sh_info));
      auto Data = unwrapOrError(
          Obj->template getSectionContentsAsArray<Elf_Word>(&Sec));
      DictScope D(W, "Group");
      StringRef Name = unwrapOrError(Obj->getSectionName(&Sec));
      W.printNumber("Name", Name, Sec.sh_name);
      W.printNumber("Index", SectionIndex);
      W.printHex("Type", getGroupType(Data[0]), Data[0]);
      W.startLine() << "Signature: " << StrTable.data() + Sym->st_name << "\n";
      {
        ListScope L(W, "Section(s) in group");
        size_t Member = 1;
        while (Member < Data.size()) {
          auto Sec = unwrapOrError(Obj->getSection(Data[Member]));
          const StringRef Name = unwrapOrError(Obj->getSectionName(Sec));
          W.startLine() << Name << " (" << Data[Member++] << ")\n";
        }
      }
    }
    ++SectionIndex;
  }
  if (!HasGroups)
    W.startLine() << "There are no group sections in the file.\n";
}

template <class ELFT> void LLVMStyle<ELFT>::printRelocations(const ELFO *Obj) {
  ListScope D(W, "Relocations");

  int SectionNumber = -1;
  for (const Elf_Shdr &Sec : unwrapOrError(Obj->sections())) {
    ++SectionNumber;

    if (Sec.sh_type != ELF::SHT_REL && Sec.sh_type != ELF::SHT_RELA)
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
  }
}

template <class ELFT>
void LLVMStyle<ELFT>::printRelocation(const ELFO *Obj, Elf_Rela Rel,
                                      const Elf_Shdr *SymTab) {
  SmallString<32> RelocName;
  Obj->getRelocationTypeName(Rel.getType(Obj->isMips64EL()), RelocName);
  StringRef TargetName;
  const Elf_Sym *Sym = unwrapOrError(Obj->getRelocationSymbol(&Rel, SymTab));
  if (Sym && Sym->getType() == ELF::STT_SECTION) {
    const Elf_Shdr *Sec = unwrapOrError(
        Obj->getSection(Sym, SymTab, this->dumper()->getShndxTable()));
    TargetName = unwrapOrError(Obj->getSectionName(Sec));
  } else if (Sym) {
    StringRef StrTable = unwrapOrError(Obj->getStringTableForSymtab(*SymTab));
    TargetName = unwrapOrError(Sym->getName(StrTable));
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
       << (!TargetName.empty() ? TargetName : "-") << " "
       << W.hex(Rel.r_addend) << "\n";
  }
}

template <class ELFT> void LLVMStyle<ELFT>::printSections(const ELFO *Obj) {
  ListScope SectionsD(W, "Sections");

  int SectionIndex = -1;
  for (const Elf_Shdr &Sec : unwrapOrError(Obj->sections())) {
    ++SectionIndex;

    StringRef Name = unwrapOrError(Obj->getSectionName(&Sec));

    DictScope SectionD(W, "Section");
    W.printNumber("Index", SectionIndex);
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
      W.printBinaryBlock("SectionData",
                         StringRef((const char *)Data.data(), Data.size()));
    }
  }
}

template <class ELFT>
void LLVMStyle<ELFT>::printSymbol(const ELFO *Obj, const Elf_Sym *Symbol,
                                  const Elf_Sym *First, StringRef StrTable,
                                  bool IsDynamic) {
  unsigned SectionIndex = 0;
  StringRef SectionName;
  getSectionNameIndex(*Obj, Symbol, First, this->dumper()->getShndxTable(),
                      SectionName, SectionIndex);
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

template <class ELFT> void LLVMStyle<ELFT>::printSymbols(const ELFO *Obj) {
  ListScope Group(W, "Symbols");
  this->dumper()->printSymbolsHelper(false);
}

template <class ELFT>
void LLVMStyle<ELFT>::printDynamicSymbols(const ELFO *Obj) {
  ListScope Group(W, "DynamicSymbols");
  this->dumper()->printSymbolsHelper(true);
}

template <class ELFT>
void LLVMStyle<ELFT>::printDynamicRelocations(const ELFO *Obj) {
  const DynRegionInfo &DynRelRegion = this->dumper()->getDynRelRegion();
  const DynRegionInfo &DynRelaRegion = this->dumper()->getDynRelaRegion();
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
  StringRef SymbolName;
  uint32_t SymIndex = Rel.getSymbol(Obj->isMips64EL());
  const Elf_Sym *Sym = this->dumper()->dynamic_symbols().begin() + SymIndex;
  SymbolName =
      unwrapOrError(Sym->getName(this->dumper()->getDynamicStringTable()));
  if (opts::ExpandRelocs) {
    DictScope Group(W, "Relocation");
    W.printHex("Offset", Rel.r_offset);
    W.printNumber("Type", RelocName, (int)Rel.getType(Obj->isMips64EL()));
    W.printString("Symbol", !SymbolName.empty() ? SymbolName : "-");
    W.printHex("Addend", Rel.r_addend);
  } else {
    raw_ostream &OS = W.startLine();
    OS << W.hex(Rel.r_offset) << " " << RelocName << " "
       << (!SymbolName.empty() ? SymbolName : "-") << " "
       << W.hex(Rel.r_addend) << "\n";
  }
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
void LLVMStyle<ELFT>::printHashHistogram(const ELFFile<ELFT> *Obj) {
  W.startLine() << "Hash Histogram not implemented!\n";
}

template <class ELFT>
void LLVMStyle<ELFT>::printNotes(const ELFFile<ELFT> *Obj) {
  W.startLine() << "printNotes not implemented!\n";
}
