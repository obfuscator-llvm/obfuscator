//===- NativeCompilandSymbol.cpp - Native impl for compilands ---*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "llvm/DebugInfo/PDB/Native/NativeCompilandSymbol.h"

#include "llvm/ADT/STLExtras.h"

namespace llvm {
namespace pdb {

NativeCompilandSymbol::NativeCompilandSymbol(NativeSession &Session,
                                             SymIndexId SymbolId,
                                             DbiModuleDescriptor MI)
    : NativeRawSymbol(Session, SymbolId), Module(MI) {}

PDB_SymType NativeCompilandSymbol::getSymTag() const {
  return PDB_SymType::Compiland;
}

std::unique_ptr<NativeRawSymbol> NativeCompilandSymbol::clone() const {
  return llvm::make_unique<NativeCompilandSymbol>(Session, SymbolId, Module);
}

bool NativeCompilandSymbol::isEditAndContinueEnabled() const {
  return Module.hasECInfo();
}

uint32_t NativeCompilandSymbol::getLexicalParentId() const { return 0; }

// The usage of getObjFileName for getLibraryName and getModuleName for getName
// may seem backwards, but it is consistent with DIA, which is what this API
// was modeled after.  We may rename these methods later to try to eliminate
// this potential confusion.

std::string NativeCompilandSymbol::getLibraryName() const {
  return Module.getObjFileName();
}

std::string NativeCompilandSymbol::getName() const {
  return Module.getModuleName();
}

} // namespace pdb
} // namespace llvm
