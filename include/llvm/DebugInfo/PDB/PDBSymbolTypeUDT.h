//===- PDBSymbolTypeUDT.h - UDT type info -----------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_DEBUGINFO_PDB_PDBSYMBOLTYPEUDT_H
#define LLVM_DEBUGINFO_PDB_PDBSYMBOLTYPEUDT_H

#include "IPDBSession.h"
#include "PDBSymbol.h"
#include "PDBSymbolTypeBaseClass.h"
#include "PDBTypes.h"

namespace llvm {

class raw_ostream;

namespace pdb {

class PDBSymbolTypeUDT : public PDBSymbol {
public:
  PDBSymbolTypeUDT(const IPDBSession &PDBSession,
                   std::unique_ptr<IPDBRawSymbol> UDTSymbol);

  std::unique_ptr<PDBSymbolTypeUDT> clone() const {
    return getSession().getConcreteSymbolById<PDBSymbolTypeUDT>(
        getSymIndexId());
  }

  DECLARE_PDB_SYMBOL_CONCRETE_TYPE(PDB_SymType::UDT)

  void dump(PDBSymDumper &Dumper) const override;

  FORWARD_SYMBOL_ID_METHOD(getClassParent)
  FORWARD_SYMBOL_ID_METHOD(getUnmodifiedType)
  FORWARD_SYMBOL_METHOD(hasConstructor)
  FORWARD_SYMBOL_METHOD(isConstType)
  FORWARD_SYMBOL_METHOD(hasAssignmentOperator)
  FORWARD_SYMBOL_METHOD(hasCastOperator)
  FORWARD_SYMBOL_METHOD(hasNestedTypes)
  FORWARD_SYMBOL_METHOD(getLength)
  FORWARD_SYMBOL_ID_METHOD(getLexicalParent)
  FORWARD_SYMBOL_METHOD(getName)
  FORWARD_SYMBOL_METHOD(isNested)
  FORWARD_SYMBOL_METHOD(hasOverloadedOperator)
  FORWARD_SYMBOL_METHOD(isPacked)
  FORWARD_SYMBOL_METHOD(isScoped)
  FORWARD_SYMBOL_METHOD(getUdtKind)
  FORWARD_SYMBOL_METHOD(isUnalignedType)
  FORWARD_SYMBOL_ID_METHOD(getVirtualTableShape)
  FORWARD_SYMBOL_METHOD(isVolatileType)
};
}
} // namespace llvm

#endif // LLVM_DEBUGINFO_PDB_PDBSYMBOLTYPEUDT_H
