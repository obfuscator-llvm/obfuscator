//==- NativeEnumModules.h - Native Module Enumerator impl --------*- C++ -*-==//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_DEBUGINFO_PDB_NATIVE_NATIVEENUMMODULES_H
#define LLVM_DEBUGINFO_PDB_NATIVE_NATIVEENUMMODULES_H

#include "llvm/DebugInfo/PDB/IPDBEnumChildren.h"
#include "llvm/DebugInfo/PDB/Native/DbiModuleDescriptor.h"
#include "llvm/DebugInfo/PDB/PDBSymbol.h"
namespace llvm {
namespace pdb {

class DbiModuleList;
class NativeSession;

class NativeEnumModules : public IPDBEnumChildren<PDBSymbol> {
public:
  NativeEnumModules(NativeSession &Session, const DbiModuleList &Modules,
                    uint32_t Index = 0);

  uint32_t getChildCount() const override;
  std::unique_ptr<PDBSymbol> getChildAtIndex(uint32_t Index) const override;
  std::unique_ptr<PDBSymbol> getNext() override;
  void reset() override;
  NativeEnumModules *clone() const override;

private:
  NativeSession &Session;
  const DbiModuleList &Modules;
  uint32_t Index;
};
}
}

#endif
