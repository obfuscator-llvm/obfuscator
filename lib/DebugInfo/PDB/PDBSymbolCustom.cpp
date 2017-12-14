//===- PDBSymbolCustom.cpp - compiler-specific types ------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "llvm/DebugInfo/PDB/PDBSymbolCustom.h"

#include "llvm/DebugInfo/PDB/IPDBRawSymbol.h"
#include "llvm/DebugInfo/PDB/PDBSymDumper.h"
#include "llvm/DebugInfo/PDB/PDBSymbol.h"

#include <utility>

using namespace llvm;
using namespace llvm::pdb;

PDBSymbolCustom::PDBSymbolCustom(const IPDBSession &PDBSession,
                                 std::unique_ptr<IPDBRawSymbol> CustomSymbol)
    : PDBSymbol(PDBSession, std::move(CustomSymbol)) {
  assert(RawSymbol->getSymTag() == PDB_SymType::Custom);
}

void PDBSymbolCustom::getDataBytes(llvm::SmallVector<uint8_t, 32> &bytes) {
  RawSymbol->getDataBytes(bytes);
}

void PDBSymbolCustom::dump(PDBSymDumper &Dumper) const { Dumper.dump(*this); }
