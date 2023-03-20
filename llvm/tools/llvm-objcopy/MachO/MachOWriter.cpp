//===- MachOWriter.cpp ------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "MachOWriter.h"
#include "Object.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/BinaryFormat/MachO.h"
#include "llvm/Object/MachO.h"
#include "llvm/Support/Errc.h"
#include "llvm/Support/ErrorHandling.h"
#include <memory>

namespace llvm {
namespace objcopy {
namespace macho {

size_t MachOWriter::headerSize() const {
  return Is64Bit ? sizeof(MachO::mach_header_64) : sizeof(MachO::mach_header);
}

size_t MachOWriter::loadCommandsSize() const { return O.Header.SizeOfCmds; }

size_t MachOWriter::symTableSize() const {
  return O.SymTable.Symbols.size() *
         (Is64Bit ? sizeof(MachO::nlist_64) : sizeof(MachO::nlist));
}

size_t MachOWriter::totalSize() const {
  // Going from tail to head and looking for an appropriate "anchor" to
  // calculate the total size assuming that all the offsets are either valid
  // ("true") or 0 (0 indicates that the corresponding part is missing).

  SmallVector<size_t, 7> Ends;
  if (O.SymTabCommandIndex) {
    const MachO::symtab_command &SymTabCommand =
        O.LoadCommands[*O.SymTabCommandIndex]
            .MachOLoadCommand.symtab_command_data;
    if (SymTabCommand.symoff) {
      assert((SymTabCommand.nsyms == O.SymTable.Symbols.size()) &&
             "Incorrect number of symbols");
      Ends.push_back(SymTabCommand.symoff + symTableSize());
    }
    if (SymTabCommand.stroff) {
      assert((SymTabCommand.strsize == StrTableBuilder.getSize()) &&
             "Incorrect string table size");
      Ends.push_back(SymTabCommand.stroff + SymTabCommand.strsize);
    }
  }
  if (O.DyLdInfoCommandIndex) {
    const MachO::dyld_info_command &DyLdInfoCommand =
        O.LoadCommands[*O.DyLdInfoCommandIndex]
            .MachOLoadCommand.dyld_info_command_data;
    if (DyLdInfoCommand.rebase_off) {
      assert((DyLdInfoCommand.rebase_size == O.Rebases.Opcodes.size()) &&
             "Incorrect rebase opcodes size");
      Ends.push_back(DyLdInfoCommand.rebase_off + DyLdInfoCommand.rebase_size);
    }
    if (DyLdInfoCommand.bind_off) {
      assert((DyLdInfoCommand.bind_size == O.Binds.Opcodes.size()) &&
             "Incorrect bind opcodes size");
      Ends.push_back(DyLdInfoCommand.bind_off + DyLdInfoCommand.bind_size);
    }
    if (DyLdInfoCommand.weak_bind_off) {
      assert((DyLdInfoCommand.weak_bind_size == O.WeakBinds.Opcodes.size()) &&
             "Incorrect weak bind opcodes size");
      Ends.push_back(DyLdInfoCommand.weak_bind_off +
                     DyLdInfoCommand.weak_bind_size);
    }
    if (DyLdInfoCommand.lazy_bind_off) {
      assert((DyLdInfoCommand.lazy_bind_size == O.LazyBinds.Opcodes.size()) &&
             "Incorrect lazy bind opcodes size");
      Ends.push_back(DyLdInfoCommand.lazy_bind_off +
                     DyLdInfoCommand.lazy_bind_size);
    }
    if (DyLdInfoCommand.export_off) {
      assert((DyLdInfoCommand.export_size == O.Exports.Trie.size()) &&
             "Incorrect trie size");
      Ends.push_back(DyLdInfoCommand.export_off + DyLdInfoCommand.export_size);
    }
  }

  // Otherwise, use the last section / reloction.
  for (const auto &LC : O.LoadCommands)
    for (const auto &S : LC.Sections) {
      Ends.push_back(S.Offset + S.Size);
      if (S.RelOff)
        Ends.push_back(S.RelOff +
                       S.NReloc * sizeof(MachO::any_relocation_info));
    }

  if (!Ends.empty())
    return *std::max_element(Ends.begin(), Ends.end());

  // Otherwise, we have only Mach header and load commands.
  return headerSize() + loadCommandsSize();
}

void MachOWriter::writeHeader() {
  MachO::mach_header_64 Header;

  Header.magic = O.Header.Magic;
  Header.cputype = O.Header.CPUType;
  Header.cpusubtype = O.Header.CPUSubType;
  Header.filetype = O.Header.FileType;
  Header.ncmds = O.Header.NCmds;
  Header.sizeofcmds = O.Header.SizeOfCmds;
  Header.flags = O.Header.Flags;
  Header.reserved = O.Header.Reserved;

  if (IsLittleEndian != sys::IsLittleEndianHost)
    MachO::swapStruct(Header);

  auto HeaderSize =
      Is64Bit ? sizeof(MachO::mach_header_64) : sizeof(MachO::mach_header);
  memcpy(B.getBufferStart(), &Header, HeaderSize);
}

void MachOWriter::updateSymbolIndexes() {
  uint32_t Index = 0;
  for (auto &Symbol : O.SymTable.Symbols) {
    Symbol->Index = Index;
    Index++;
  }
}

void MachOWriter::writeLoadCommands() {
  uint8_t *Begin = B.getBufferStart() + headerSize();
  for (const auto &LC : O.LoadCommands) {
    // Construct a load command.
    MachO::macho_load_command MLC = LC.MachOLoadCommand;
    switch (MLC.load_command_data.cmd) {
    case MachO::LC_SEGMENT:
      if (IsLittleEndian != sys::IsLittleEndianHost)
        MachO::swapStruct(MLC.segment_command_data);
      memcpy(Begin, &MLC.segment_command_data, sizeof(MachO::segment_command));
      Begin += sizeof(MachO::segment_command);

      for (const auto &Sec : LC.Sections)
        writeSectionInLoadCommand<MachO::section>(Sec, Begin);
      continue;
    case MachO::LC_SEGMENT_64:
      if (IsLittleEndian != sys::IsLittleEndianHost)
        MachO::swapStruct(MLC.segment_command_64_data);
      memcpy(Begin, &MLC.segment_command_64_data,
             sizeof(MachO::segment_command_64));
      Begin += sizeof(MachO::segment_command_64);

      for (const auto &Sec : LC.Sections)
        writeSectionInLoadCommand<MachO::section_64>(Sec, Begin);
      continue;
    }

#define HANDLE_LOAD_COMMAND(LCName, LCValue, LCStruct)                         \
  case MachO::LCName:                                                          \
    assert(sizeof(MachO::LCStruct) + LC.Payload.size() ==                      \
           MLC.load_command_data.cmdsize);                                     \
    if (IsLittleEndian != sys::IsLittleEndianHost)                             \
      MachO::swapStruct(MLC.LCStruct##_data);                                  \
    memcpy(Begin, &MLC.LCStruct##_data, sizeof(MachO::LCStruct));              \
    Begin += sizeof(MachO::LCStruct);                                          \
    memcpy(Begin, LC.Payload.data(), LC.Payload.size());                       \
    Begin += LC.Payload.size();                                                \
    break;

    // Copy the load command as it is.
    switch (MLC.load_command_data.cmd) {
    default:
      assert(sizeof(MachO::load_command) + LC.Payload.size() ==
             MLC.load_command_data.cmdsize);
      if (IsLittleEndian != sys::IsLittleEndianHost)
        MachO::swapStruct(MLC.load_command_data);
      memcpy(Begin, &MLC.load_command_data, sizeof(MachO::load_command));
      Begin += sizeof(MachO::load_command);
      memcpy(Begin, LC.Payload.data(), LC.Payload.size());
      Begin += LC.Payload.size();
      break;
#include "llvm/BinaryFormat/MachO.def"
    }
  }
}

template <typename StructType>
void MachOWriter::writeSectionInLoadCommand(const Section &Sec, uint8_t *&Out) {
  StructType Temp;
  assert(Sec.Segname.size() <= sizeof(Temp.segname) && "too long segment name");
  assert(Sec.Sectname.size() <= sizeof(Temp.sectname) &&
         "too long section name");
  memset(&Temp, 0, sizeof(StructType));
  memcpy(Temp.segname, Sec.Segname.data(), Sec.Segname.size());
  memcpy(Temp.sectname, Sec.Sectname.data(), Sec.Sectname.size());
  Temp.addr = Sec.Addr;
  Temp.size = Sec.Size;
  Temp.offset = Sec.Offset;
  Temp.align = Sec.Align;
  Temp.reloff = Sec.RelOff;
  Temp.nreloc = Sec.NReloc;
  Temp.flags = Sec.Flags;
  Temp.reserved1 = Sec.Reserved1;
  Temp.reserved2 = Sec.Reserved2;

  if (IsLittleEndian != sys::IsLittleEndianHost)
    MachO::swapStruct(Temp);
  memcpy(Out, &Temp, sizeof(StructType));
  Out += sizeof(StructType);
}

void MachOWriter::writeSections() {
  for (const auto &LC : O.LoadCommands)
    for (const auto &Sec : LC.Sections) {
      if (Sec.isVirtualSection())
        continue;

      assert(Sec.Offset && "Section offset can not be zero");
      assert((Sec.Size == Sec.Content.size()) && "Incorrect section size");
      memcpy(B.getBufferStart() + Sec.Offset, Sec.Content.data(),
             Sec.Content.size());
      for (size_t Index = 0; Index < Sec.Relocations.size(); ++Index) {
        auto RelocInfo = Sec.Relocations[Index];
        if (!RelocInfo.Scattered) {
          auto *Info =
              reinterpret_cast<MachO::relocation_info *>(&RelocInfo.Info);
          Info->r_symbolnum = RelocInfo.Symbol->Index;
        }

        if (IsLittleEndian != sys::IsLittleEndianHost)
          MachO::swapStruct(
              reinterpret_cast<MachO::any_relocation_info &>(RelocInfo.Info));
        memcpy(B.getBufferStart() + Sec.RelOff +
                   Index * sizeof(MachO::any_relocation_info),
               &RelocInfo.Info, sizeof(RelocInfo.Info));
      }
    }
}

template <typename NListType>
void writeNListEntry(const SymbolEntry &SE, bool IsLittleEndian, char *&Out,
                     uint32_t Nstrx) {
  NListType ListEntry;
  ListEntry.n_strx = Nstrx;
  ListEntry.n_type = SE.n_type;
  ListEntry.n_sect = SE.n_sect;
  ListEntry.n_desc = SE.n_desc;
  ListEntry.n_value = SE.n_value;

  if (IsLittleEndian != sys::IsLittleEndianHost)
    MachO::swapStruct(ListEntry);
  memcpy(Out, reinterpret_cast<const char *>(&ListEntry), sizeof(NListType));
  Out += sizeof(NListType);
}

void MachOWriter::writeSymbolTable() {
  if (!O.SymTabCommandIndex)
    return;
  const MachO::symtab_command &SymTabCommand =
      O.LoadCommands[*O.SymTabCommandIndex]
          .MachOLoadCommand.symtab_command_data;

  uint8_t *StrTable = (uint8_t *)B.getBufferStart() + SymTabCommand.stroff;
  StrTableBuilder.write(StrTable);
}

void MachOWriter::writeStringTable() {
  if (!O.SymTabCommandIndex)
    return;
  const MachO::symtab_command &SymTabCommand =
      O.LoadCommands[*O.SymTabCommandIndex]
          .MachOLoadCommand.symtab_command_data;

  char *SymTable = (char *)B.getBufferStart() + SymTabCommand.symoff;
  for (auto Iter = O.SymTable.Symbols.begin(), End = O.SymTable.Symbols.end();
       Iter != End; Iter++) {
    SymbolEntry *Sym = Iter->get();
    auto Nstrx = StrTableBuilder.getOffset(Sym->Name);

    if (Is64Bit)
      writeNListEntry<MachO::nlist_64>(*Sym, IsLittleEndian, SymTable, Nstrx);
    else
      writeNListEntry<MachO::nlist>(*Sym, IsLittleEndian, SymTable, Nstrx);
  }
}

void MachOWriter::writeRebaseInfo() {
  if (!O.DyLdInfoCommandIndex)
    return;
  const MachO::dyld_info_command &DyLdInfoCommand =
      O.LoadCommands[*O.DyLdInfoCommandIndex]
          .MachOLoadCommand.dyld_info_command_data;
  char *Out = (char *)B.getBufferStart() + DyLdInfoCommand.rebase_off;
  assert((DyLdInfoCommand.rebase_size == O.Rebases.Opcodes.size()) &&
         "Incorrect rebase opcodes size");
  memcpy(Out, O.Rebases.Opcodes.data(), O.Rebases.Opcodes.size());
}

void MachOWriter::writeBindInfo() {
  if (!O.DyLdInfoCommandIndex)
    return;
  const MachO::dyld_info_command &DyLdInfoCommand =
      O.LoadCommands[*O.DyLdInfoCommandIndex]
          .MachOLoadCommand.dyld_info_command_data;
  char *Out = (char *)B.getBufferStart() + DyLdInfoCommand.bind_off;
  assert((DyLdInfoCommand.bind_size == O.Binds.Opcodes.size()) &&
         "Incorrect bind opcodes size");
  memcpy(Out, O.Binds.Opcodes.data(), O.Binds.Opcodes.size());
}

void MachOWriter::writeWeakBindInfo() {
  if (!O.DyLdInfoCommandIndex)
    return;
  const MachO::dyld_info_command &DyLdInfoCommand =
      O.LoadCommands[*O.DyLdInfoCommandIndex]
          .MachOLoadCommand.dyld_info_command_data;
  char *Out = (char *)B.getBufferStart() + DyLdInfoCommand.weak_bind_off;
  assert((DyLdInfoCommand.weak_bind_size == O.WeakBinds.Opcodes.size()) &&
         "Incorrect weak bind opcodes size");
  memcpy(Out, O.WeakBinds.Opcodes.data(), O.WeakBinds.Opcodes.size());
}

void MachOWriter::writeLazyBindInfo() {
  if (!O.DyLdInfoCommandIndex)
    return;
  const MachO::dyld_info_command &DyLdInfoCommand =
      O.LoadCommands[*O.DyLdInfoCommandIndex]
          .MachOLoadCommand.dyld_info_command_data;
  char *Out = (char *)B.getBufferStart() + DyLdInfoCommand.lazy_bind_off;
  assert((DyLdInfoCommand.lazy_bind_size == O.LazyBinds.Opcodes.size()) &&
         "Incorrect lazy bind opcodes size");
  memcpy(Out, O.LazyBinds.Opcodes.data(), O.LazyBinds.Opcodes.size());
}

void MachOWriter::writeExportInfo() {
  if (!O.DyLdInfoCommandIndex)
    return;
  const MachO::dyld_info_command &DyLdInfoCommand =
      O.LoadCommands[*O.DyLdInfoCommandIndex]
          .MachOLoadCommand.dyld_info_command_data;
  char *Out = (char *)B.getBufferStart() + DyLdInfoCommand.export_off;
  assert((DyLdInfoCommand.export_size == O.Exports.Trie.size()) &&
         "Incorrect export trie size");
  memcpy(Out, O.Exports.Trie.data(), O.Exports.Trie.size());
}

void MachOWriter::writeTail() {
  typedef void (MachOWriter::*WriteHandlerType)(void);
  typedef std::pair<uint64_t, WriteHandlerType> WriteOperation;
  SmallVector<WriteOperation, 7> Queue;

  if (O.SymTabCommandIndex) {
    const MachO::symtab_command &SymTabCommand =
        O.LoadCommands[*O.SymTabCommandIndex]
            .MachOLoadCommand.symtab_command_data;
    if (SymTabCommand.symoff)
      Queue.push_back({SymTabCommand.symoff, &MachOWriter::writeSymbolTable});
    if (SymTabCommand.stroff)
      Queue.push_back({SymTabCommand.stroff, &MachOWriter::writeStringTable});
  }

  if (O.DyLdInfoCommandIndex) {
    const MachO::dyld_info_command &DyLdInfoCommand =
        O.LoadCommands[*O.DyLdInfoCommandIndex]
            .MachOLoadCommand.dyld_info_command_data;
    if (DyLdInfoCommand.rebase_off)
      Queue.push_back(
          {DyLdInfoCommand.rebase_off, &MachOWriter::writeRebaseInfo});
    if (DyLdInfoCommand.bind_off)
      Queue.push_back({DyLdInfoCommand.bind_off, &MachOWriter::writeBindInfo});
    if (DyLdInfoCommand.weak_bind_off)
      Queue.push_back(
          {DyLdInfoCommand.weak_bind_off, &MachOWriter::writeWeakBindInfo});
    if (DyLdInfoCommand.lazy_bind_off)
      Queue.push_back(
          {DyLdInfoCommand.lazy_bind_off, &MachOWriter::writeLazyBindInfo});
    if (DyLdInfoCommand.export_off)
      Queue.push_back(
          {DyLdInfoCommand.export_off, &MachOWriter::writeExportInfo});
  }

  llvm::sort(Queue, [](const WriteOperation &LHS, const WriteOperation &RHS) {
    return LHS.first < RHS.first;
  });

  for (auto WriteOp : Queue)
    (this->*WriteOp.second)();
}

void MachOWriter::updateSizeOfCmds() {
  auto Size = 0;
  for (const auto &LC : O.LoadCommands) {
    auto &MLC = LC.MachOLoadCommand;
    auto cmd = MLC.load_command_data.cmd;

    switch (cmd) {
    case MachO::LC_SEGMENT:
      Size += sizeof(MachO::segment_command) +
              sizeof(MachO::section) * LC.Sections.size();
      continue;
    case MachO::LC_SEGMENT_64:
      Size += sizeof(MachO::segment_command_64) +
              sizeof(MachO::section_64) * LC.Sections.size();
      continue;
    }

    switch (cmd) {
#define HANDLE_LOAD_COMMAND(LCName, LCValue, LCStruct)                         \
  case MachO::LCName:                                                          \
    Size += sizeof(MachO::LCStruct);                                           \
    break;
#include "llvm/BinaryFormat/MachO.def"
#undef HANDLE_LOAD_COMMAND
    }
  }

  O.Header.SizeOfCmds = Size;
}

// Updates the index and the number of local/external/undefined symbols. Here we
// assume that MLC is a LC_DYSYMTAB and the nlist entries in the symbol table
// are already sorted by the those types.
void MachOWriter::updateDySymTab(MachO::macho_load_command &MLC) {
  uint32_t NumLocalSymbols = 0;
  auto Iter = O.SymTable.Symbols.begin();
  auto End = O.SymTable.Symbols.end();
  for (; Iter != End; Iter++) {
    if ((*Iter)->n_type & (MachO::N_EXT | MachO::N_PEXT))
      break;

    NumLocalSymbols++;
  }

  uint32_t NumExtDefSymbols = 0;
  for (; Iter != End; Iter++) {
    if (((*Iter)->n_type & MachO::N_TYPE) == MachO::N_UNDF)
      break;

    NumExtDefSymbols++;
  }

  MLC.dysymtab_command_data.ilocalsym = 0;
  MLC.dysymtab_command_data.nlocalsym = NumLocalSymbols;
  MLC.dysymtab_command_data.iextdefsym = NumLocalSymbols;
  MLC.dysymtab_command_data.nextdefsym = NumExtDefSymbols;
  MLC.dysymtab_command_data.iundefsym = NumLocalSymbols + NumExtDefSymbols;
  MLC.dysymtab_command_data.nundefsym =
      O.SymTable.Symbols.size() - (NumLocalSymbols + NumExtDefSymbols);
}

// Recomputes and updates offset and size fields in load commands and sections
// since they could be modified.
Error MachOWriter::layout() {
  auto SizeOfCmds = loadCommandsSize();
  auto Offset = headerSize() + SizeOfCmds;
  O.Header.NCmds = O.LoadCommands.size();
  O.Header.SizeOfCmds = SizeOfCmds;

  // Lay out sections.
  for (auto &LC : O.LoadCommands) {
    uint64_t FileOff = Offset;
    uint64_t VMSize = 0;
    uint64_t FileOffsetInSegment = 0;
    for (auto &Sec : LC.Sections) {
      if (!Sec.isVirtualSection()) {
        auto FilePaddingSize =
            OffsetToAlignment(FileOffsetInSegment, 1ull << Sec.Align);
        Sec.Offset = Offset + FileOffsetInSegment + FilePaddingSize;
        Sec.Size = Sec.Content.size();
        FileOffsetInSegment += FilePaddingSize + Sec.Size;
      }

      VMSize = std::max(VMSize, Sec.Addr + Sec.Size);
    }

    // TODO: Handle the __PAGEZERO segment.
    auto &MLC = LC.MachOLoadCommand;
    switch (MLC.load_command_data.cmd) {
    case MachO::LC_SEGMENT:
      MLC.segment_command_data.cmdsize =
          sizeof(MachO::segment_command) +
          sizeof(MachO::section) * LC.Sections.size();
      MLC.segment_command_data.nsects = LC.Sections.size();
      MLC.segment_command_data.fileoff = FileOff;
      MLC.segment_command_data.vmsize = VMSize;
      MLC.segment_command_data.filesize = FileOffsetInSegment;
      break;
    case MachO::LC_SEGMENT_64:
      MLC.segment_command_64_data.cmdsize =
          sizeof(MachO::segment_command_64) +
          sizeof(MachO::section_64) * LC.Sections.size();
      MLC.segment_command_64_data.nsects = LC.Sections.size();
      MLC.segment_command_64_data.fileoff = FileOff;
      MLC.segment_command_64_data.vmsize = VMSize;
      MLC.segment_command_64_data.filesize = FileOffsetInSegment;
      break;
    }

    Offset += FileOffsetInSegment;
  }

  // Lay out relocations.
  for (auto &LC : O.LoadCommands)
    for (auto &Sec : LC.Sections) {
      Sec.RelOff = Sec.Relocations.empty() ? 0 : Offset;
      Sec.NReloc = Sec.Relocations.size();
      Offset += sizeof(MachO::any_relocation_info) * Sec.NReloc;
    }

  // Lay out tail stuff.
  auto NListSize = Is64Bit ? sizeof(MachO::nlist_64) : sizeof(MachO::nlist);
  for (auto &LC : O.LoadCommands) {
    auto &MLC = LC.MachOLoadCommand;
    auto cmd = MLC.load_command_data.cmd;
    switch (cmd) {
    case MachO::LC_SYMTAB:
      MLC.symtab_command_data.nsyms = O.SymTable.Symbols.size();
      MLC.symtab_command_data.strsize = StrTableBuilder.getSize();
      MLC.symtab_command_data.symoff = Offset;
      Offset += NListSize * MLC.symtab_command_data.nsyms;
      MLC.symtab_command_data.stroff = Offset;
      Offset += MLC.symtab_command_data.strsize;
      break;
    case MachO::LC_DYSYMTAB: {
      if (MLC.dysymtab_command_data.ntoc != 0 ||
          MLC.dysymtab_command_data.nmodtab != 0 ||
          MLC.dysymtab_command_data.nextrefsyms != 0 ||
          MLC.dysymtab_command_data.nlocrel != 0 ||
          MLC.dysymtab_command_data.nextrel != 0)
        return createStringError(llvm::errc::not_supported,
                                 "shared library is not yet supported");

      if (MLC.dysymtab_command_data.nindirectsyms != 0)
        return createStringError(llvm::errc::not_supported,
                                 "indirect symbol table is not yet supported");

      updateDySymTab(MLC);
      break;
    }
    case MachO::LC_SEGMENT:
    case MachO::LC_SEGMENT_64:
    case MachO::LC_VERSION_MIN_MACOSX:
    case MachO::LC_BUILD_VERSION:
    case MachO::LC_ID_DYLIB:
    case MachO::LC_LOAD_DYLIB:
    case MachO::LC_UUID:
    case MachO::LC_SOURCE_VERSION:
      // Nothing to update.
      break;
    default:
      // Abort if it's unsupported in order to prevent corrupting the object.
      return createStringError(llvm::errc::not_supported,
                               "unsupported load command (cmd=0x%x)", cmd);
    }
  }

  return Error::success();
}

void MachOWriter::constructStringTable() {
  for (std::unique_ptr<SymbolEntry> &Sym : O.SymTable.Symbols)
    StrTableBuilder.add(Sym->Name);
  StrTableBuilder.finalize();
}

Error MachOWriter::finalize() {
  updateSizeOfCmds();
  constructStringTable();

  if (auto E = layout())
    return E;

  return Error::success();
}

Error MachOWriter::write() {
  if (Error E = B.allocate(totalSize()))
    return E;
  memset(B.getBufferStart(), 0, totalSize());
  writeHeader();
  updateSymbolIndexes();
  writeLoadCommands();
  writeSections();
  writeTail();
  return B.commit();
}

} // end namespace macho
} // end namespace objcopy
} // end namespace llvm
