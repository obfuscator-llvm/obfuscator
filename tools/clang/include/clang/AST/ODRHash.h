//===-- ODRHash.h - Hashing to diagnose ODR failures ------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
///
/// \file
/// This file contains the declaration of the ODRHash class, which calculates
/// a hash based on AST nodes, which is stable across different runs.
///
//===----------------------------------------------------------------------===//

#include "clang/AST/DeclarationName.h"
#include "clang/AST/Type.h"
#include "clang/AST/TemplateBase.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/FoldingSet.h"
#include "llvm/ADT/PointerUnion.h"
#include "llvm/ADT/SmallVector.h"

namespace clang {

class Decl;
class IdentifierInfo;
class NestedNameSpecifier;
class Stmt;
class TemplateParameterList;

// ODRHash is used to calculate a hash based on AST node contents that
// does not rely on pointer addresses.  This allows the hash to not vary
// between runs and is usable to detect ODR problems in modules.  To use,
// construct an ODRHash object, then call Add* methods over the nodes that
// need to be hashed.  Then call CalculateHash to get the hash value.
// Typically, only one Add* call is needed.  clear can be called to reuse the
// object.
class ODRHash {
  // Use DenseMaps to convert between Decl and Type pointers and an index value.
  llvm::DenseMap<const Decl*, unsigned> DeclMap;
  llvm::DenseMap<const Type*, unsigned> TypeMap;

  // Save space by processing bools at the end.
  llvm::SmallVector<bool, 128> Bools;

  llvm::FoldingSetNodeID ID;

public:
  ODRHash() {}

  // Use this for ODR checking classes between modules.  This method compares
  // more information than the AddDecl class.
  void AddCXXRecordDecl(const CXXRecordDecl *Record);

  // Process SubDecls of the main Decl.  This method calls the DeclVisitor
  // while AddDecl does not.
  void AddSubDecl(const Decl *D);

  // Reset the object for reuse.
  void clear();

  // Add booleans to ID and uses it to calculate the hash.
  unsigned CalculateHash();

  // Add AST nodes that need to be processed.
  void AddDecl(const Decl *D);
  void AddType(const Type *T);
  void AddQualType(QualType T);
  void AddStmt(const Stmt *S);
  void AddIdentifierInfo(const IdentifierInfo *II);
  void AddNestedNameSpecifier(const NestedNameSpecifier *NNS);
  void AddTemplateName(TemplateName Name);
  void AddDeclarationName(DeclarationName Name);
  void AddTemplateArgument(TemplateArgument TA);
  void AddTemplateParameterList(const TemplateParameterList *TPL);

  // Save booleans until the end to lower the size of data to process.
  void AddBoolean(bool value);

  static bool isWhitelistedDecl(const Decl* D, const CXXRecordDecl *Record);
};

}  // end namespace clang
