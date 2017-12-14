//===- DeclOpenMP.h - Classes for representing OpenMP directives -*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
///
/// \file
/// \brief This file defines OpenMP nodes for declarative directives.
///
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_AST_DECLOPENMP_H
#define LLVM_CLANG_AST_DECLOPENMP_H

#include "clang/AST/Decl.h"
#include "clang/AST/Expr.h"
#include "clang/AST/ExternalASTSource.h"
#include "clang/AST/Type.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/Support/TrailingObjects.h"

namespace clang {

/// \brief This represents '#pragma omp threadprivate ...' directive.
/// For example, in the following, both 'a' and 'A::b' are threadprivate:
///
/// \code
/// int a;
/// #pragma omp threadprivate(a)
/// struct A {
///   static int b;
/// #pragma omp threadprivate(b)
/// };
/// \endcode
///
class OMPThreadPrivateDecl final
    : public Decl,
      private llvm::TrailingObjects<OMPThreadPrivateDecl, Expr *> {
  friend class ASTDeclReader;
  friend TrailingObjects;

  unsigned NumVars;

  virtual void anchor();

  OMPThreadPrivateDecl(Kind DK, DeclContext *DC, SourceLocation L) :
    Decl(DK, DC, L), NumVars(0) { }

  ArrayRef<const Expr *> getVars() const {
    return llvm::makeArrayRef(getTrailingObjects<Expr *>(), NumVars);
  }

  MutableArrayRef<Expr *> getVars() {
    return MutableArrayRef<Expr *>(getTrailingObjects<Expr *>(), NumVars);
  }

  void setVars(ArrayRef<Expr *> VL);

public:
  static OMPThreadPrivateDecl *Create(ASTContext &C, DeclContext *DC,
                                      SourceLocation L,
                                      ArrayRef<Expr *> VL);
  static OMPThreadPrivateDecl *CreateDeserialized(ASTContext &C,
                                                  unsigned ID, unsigned N);

  typedef MutableArrayRef<Expr *>::iterator varlist_iterator;
  typedef ArrayRef<const Expr *>::iterator varlist_const_iterator;
  typedef llvm::iterator_range<varlist_iterator> varlist_range;
  typedef llvm::iterator_range<varlist_const_iterator> varlist_const_range;

  unsigned varlist_size() const { return NumVars; }
  bool varlist_empty() const { return NumVars == 0; }

  varlist_range varlists() {
    return varlist_range(varlist_begin(), varlist_end());
  }
  varlist_const_range varlists() const {
    return varlist_const_range(varlist_begin(), varlist_end());
  }
  varlist_iterator varlist_begin() { return getVars().begin(); }
  varlist_iterator varlist_end() { return getVars().end(); }
  varlist_const_iterator varlist_begin() const { return getVars().begin(); }
  varlist_const_iterator varlist_end() const { return getVars().end(); }

  static bool classof(const Decl *D) { return classofKind(D->getKind()); }
  static bool classofKind(Kind K) { return K == OMPThreadPrivate; }
};

/// \brief This represents '#pragma omp declare reduction ...' directive.
/// For example, in the following, declared reduction 'foo' for types 'int' and
/// 'float':
///
/// \code
/// #pragma omp declare reduction (foo : int,float : omp_out += omp_in) \
///                     initializer (omp_priv = 0)
/// \endcode
///
/// Here 'omp_out += omp_in' is a combiner and 'omp_priv = 0' is an initializer.
class OMPDeclareReductionDecl final : public ValueDecl, public DeclContext {
private:
  friend class ASTDeclReader;
  /// \brief Combiner for declare reduction construct.
  Expr *Combiner;
  /// \brief Initializer for declare reduction construct.
  Expr *Initializer;
  /// \brief Reference to the previous declare reduction construct in the same
  /// scope with the same name. Required for proper templates instantiation if
  /// the declare reduction construct is declared inside compound statement.
  LazyDeclPtr PrevDeclInScope;

  virtual void anchor();

  OMPDeclareReductionDecl(Kind DK, DeclContext *DC, SourceLocation L,
                          DeclarationName Name, QualType Ty,
                          OMPDeclareReductionDecl *PrevDeclInScope)
      : ValueDecl(DK, DC, L, Name, Ty), DeclContext(DK), Combiner(nullptr),
        Initializer(nullptr), PrevDeclInScope(PrevDeclInScope) {}

  void setPrevDeclInScope(OMPDeclareReductionDecl *Prev) {
    PrevDeclInScope = Prev;
  }

public:
  /// \brief Create declare reduction node.
  static OMPDeclareReductionDecl *
  Create(ASTContext &C, DeclContext *DC, SourceLocation L, DeclarationName Name,
         QualType T, OMPDeclareReductionDecl *PrevDeclInScope);
  /// \brief Create deserialized declare reduction node.
  static OMPDeclareReductionDecl *CreateDeserialized(ASTContext &C,
                                                     unsigned ID);

  /// \brief Get combiner expression of the declare reduction construct.
  Expr *getCombiner() { return Combiner; }
  const Expr *getCombiner() const { return Combiner; }
  /// \brief Set combiner expression for the declare reduction construct.
  void setCombiner(Expr *E) { Combiner = E; }

  /// \brief Get initializer expression (if specified) of the declare reduction
  /// construct.
  Expr *getInitializer() { return Initializer; }
  const Expr *getInitializer() const { return Initializer; }
  /// \brief Set initializer expression for the declare reduction construct.
  void setInitializer(Expr *E) { Initializer = E; }

  /// \brief Get reference to previous declare reduction construct in the same
  /// scope with the same name.
  OMPDeclareReductionDecl *getPrevDeclInScope();
  const OMPDeclareReductionDecl *getPrevDeclInScope() const;

  static bool classof(const Decl *D) { return classofKind(D->getKind()); }
  static bool classofKind(Kind K) { return K == OMPDeclareReduction; }
  static DeclContext *castToDeclContext(const OMPDeclareReductionDecl *D) {
    return static_cast<DeclContext *>(const_cast<OMPDeclareReductionDecl *>(D));
  }
  static OMPDeclareReductionDecl *castFromDeclContext(const DeclContext *DC) {
    return static_cast<OMPDeclareReductionDecl *>(
        const_cast<DeclContext *>(DC));
  }
};

/// Pseudo declaration for capturing expressions. Also is used for capturing of
/// non-static data members in non-static member functions.
///
/// Clang supports capturing of variables only, but OpenMP 4.5 allows to
/// privatize non-static members of current class in non-static member
/// functions. This pseudo-declaration allows properly handle this kind of
/// capture by wrapping captured expression into a variable-like declaration.
class OMPCapturedExprDecl final : public VarDecl {
  friend class ASTDeclReader;
  void anchor() override;

  OMPCapturedExprDecl(ASTContext &C, DeclContext *DC, IdentifierInfo *Id,
                      QualType Type, SourceLocation StartLoc)
      : VarDecl(OMPCapturedExpr, C, DC, StartLoc, SourceLocation(), Id, Type,
                nullptr, SC_None) {
    setImplicit();
  }

public:
  static OMPCapturedExprDecl *Create(ASTContext &C, DeclContext *DC,
                                     IdentifierInfo *Id, QualType T,
                                     SourceLocation StartLoc);

  static OMPCapturedExprDecl *CreateDeserialized(ASTContext &C, unsigned ID);

  SourceRange getSourceRange() const override LLVM_READONLY;

  // Implement isa/cast/dyncast/etc.
  static bool classof(const Decl *D) { return classofKind(D->getKind()); }
  static bool classofKind(Kind K) { return K == OMPCapturedExpr; }
};

} // end namespace clang

#endif
