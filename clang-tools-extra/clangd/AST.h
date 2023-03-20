//===--- AST.h - Utility AST functions  -------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Various code that examines C++ source code using AST.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANGD_AST_H_
#define LLVM_CLANG_TOOLS_EXTRA_CLANGD_AST_H_

#include "index/SymbolID.h"
#include "clang/AST/Decl.h"
#include "clang/Basic/SourceLocation.h"
#include "clang/Lex/MacroInfo.h"

namespace clang {
class SourceManager;
class Decl;

namespace clangd {

/// Returns true if the declaration is considered implementation detail based on
/// heuristics. For example, a declaration whose name is not explicitly spelled
/// in code is considered implementation detail.
bool isImplementationDetail(const Decl *D);

/// Find the identifier source location of the given D.
///
/// The returned location is usually the spelling location where the name of the
/// decl occurs in the code.
SourceLocation findNameLoc(const clang::Decl *D);

/// Returns the qualified name of ND. The scope doesn't contain unwritten scopes
/// like inline namespaces.
std::string printQualifiedName(const NamedDecl &ND);

/// Returns the first enclosing namespace scope starting from \p DC.
std::string printNamespaceScope(const DeclContext &DC);

/// Prints unqualified name of the decl for the purpose of displaying it to the
/// user. Anonymous decls return names of the form "(anonymous {kind})", e.g.
/// "(anonymous struct)" or "(anonymous namespace)".
std::string printName(const ASTContext &Ctx, const NamedDecl &ND);

/// Prints template arguments of a decl as written in the source code, including
/// enclosing '<' and '>', e.g for a partial specialization like: template
/// <typename U> struct Foo<int, U> will return '<int, U>'. Returns an empty
/// string if decl is not a template specialization.
std::string printTemplateSpecializationArgs(const NamedDecl &ND);

/// Gets the symbol ID for a declaration, if possible.
llvm::Optional<SymbolID> getSymbolID(const Decl *D);

/// Gets the symbol ID for a macro, if possible.
/// Currently, this is an encoded USR of the macro, which incorporates macro
/// locations (e.g. file name, offset in file).
/// FIXME: the USR semantics might not be stable enough as the ID for index
/// macro (e.g. a change in definition offset can result in a different USR). We
/// could change these semantics in the future by reimplementing this funcure
/// (e.g. avoid USR for macros).
llvm::Optional<SymbolID> getSymbolID(const IdentifierInfo &II,
                                     const MacroInfo *MI,
                                     const SourceManager &SM);

/// Returns a QualType as string.
std::string printType(const QualType QT, const DeclContext & Context);

/// Try to shorten the OriginalName by removing namespaces from the left of
/// the string that are redundant in the CurrentNamespace. This way the type
/// idenfier become shorter and easier to read.
/// Limitation: It only handles the qualifier of the type itself, not that of
/// templates.
/// FIXME: change type of parameter CurrentNamespace to DeclContext ,
/// take in to account using directives etc
/// Example: shortenNamespace("ns1::MyClass<ns1::OtherClass>", "ns1")
///    --> "MyClass<ns1::OtherClass>"
std::string  shortenNamespace(const llvm::StringRef OriginalName,
                              const llvm::StringRef CurrentNamespace);


} // namespace clangd
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANGD_AST_H_
