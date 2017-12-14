//===- NativeSession.h - Native implementation of IPDBSession ---*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_DEBUGINFO_PDB_NATIVE_NATIVESESSION_H
#define LLVM_DEBUGINFO_PDB_NATIVE_NATIVESESSION_H

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/DebugInfo/CodeView/TypeIndex.h"
#include "llvm/DebugInfo/PDB/IPDBRawSymbol.h"
#include "llvm/DebugInfo/PDB/IPDBSession.h"
#include "llvm/DebugInfo/PDB/Native/DbiModuleDescriptor.h"
#include "llvm/DebugInfo/PDB/Native/NativeBuiltinSymbol.h"
#include "llvm/DebugInfo/PDB/Native/NativeRawSymbol.h"
#include "llvm/Support/Allocator.h"
#include "llvm/Support/Error.h"

namespace llvm {
namespace pdb {
class PDBFile;

class NativeSession : public IPDBSession {
public:
  NativeSession(std::unique_ptr<PDBFile> PdbFile,
                std::unique_ptr<BumpPtrAllocator> Allocator);
  ~NativeSession() override;

  static Error createFromPdb(StringRef Path,
                             std::unique_ptr<IPDBSession> &Session);
  static Error createFromExe(StringRef Path,
                             std::unique_ptr<IPDBSession> &Session);

  std::unique_ptr<PDBSymbolCompiland>
  createCompilandSymbol(DbiModuleDescriptor MI);

  SymIndexId findSymbolByTypeIndex(codeview::TypeIndex TI);

  uint64_t getLoadAddress() const override;
  void setLoadAddress(uint64_t Address) override;
  std::unique_ptr<PDBSymbolExe> getGlobalScope() override;
  std::unique_ptr<PDBSymbol> getSymbolById(uint32_t SymbolId) const override;

  std::unique_ptr<PDBSymbol>
  findSymbolByAddress(uint64_t Address, PDB_SymType Type) const override;

  std::unique_ptr<IPDBEnumLineNumbers>
  findLineNumbers(const PDBSymbolCompiland &Compiland,
                  const IPDBSourceFile &File) const override;
  std::unique_ptr<IPDBEnumLineNumbers>
  findLineNumbersByAddress(uint64_t Address, uint32_t Length) const override;

  std::unique_ptr<IPDBEnumSourceFiles>
  findSourceFiles(const PDBSymbolCompiland *Compiland, llvm::StringRef Pattern,
                  PDB_NameSearchFlags Flags) const override;
  std::unique_ptr<IPDBSourceFile>
  findOneSourceFile(const PDBSymbolCompiland *Compiland,
                    llvm::StringRef Pattern,
                    PDB_NameSearchFlags Flags) const override;
  std::unique_ptr<IPDBEnumChildren<PDBSymbolCompiland>>
  findCompilandsForSourceFile(llvm::StringRef Pattern,
                              PDB_NameSearchFlags Flags) const override;
  std::unique_ptr<PDBSymbolCompiland>
  findOneCompilandForSourceFile(llvm::StringRef Pattern,
                                PDB_NameSearchFlags Flags) const override;
  std::unique_ptr<IPDBEnumSourceFiles> getAllSourceFiles() const override;
  std::unique_ptr<IPDBEnumSourceFiles> getSourceFilesForCompiland(
      const PDBSymbolCompiland &Compiland) const override;
  std::unique_ptr<IPDBSourceFile>
  getSourceFileById(uint32_t FileId) const override;

  std::unique_ptr<IPDBEnumDataStreams> getDebugStreams() const override;

  PDBFile &getPDBFile() { return *Pdb; }
  const PDBFile &getPDBFile() const { return *Pdb; }

private:
  std::unique_ptr<PDBFile> Pdb;
  std::unique_ptr<BumpPtrAllocator> Allocator;
  std::vector<std::unique_ptr<NativeRawSymbol>> SymbolCache;
  DenseMap<codeview::TypeIndex, SymIndexId> TypeIndexToSymbolId;
};
}
}

#endif
