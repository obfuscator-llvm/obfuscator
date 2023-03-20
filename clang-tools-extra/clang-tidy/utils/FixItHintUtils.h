//===--- FixItHintUtils.h - clang-tidy---------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANG_TIDY_UTILS_FIXITHINTUTILS_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANG_TIDY_UTILS_FIXITHINTUTILS_H

#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"

namespace clang {
namespace tidy {
namespace utils {
namespace fixit {

/// \brief Creates fix to make ``VarDecl`` a reference by adding ``&``.
FixItHint changeVarDeclToReference(const VarDecl &Var, ASTContext &Context);

/// \brief Creates fix to make ``VarDecl`` const qualified.
FixItHint changeVarDeclToConst(const VarDecl &Var);

} // namespace fixit
} // namespace utils
} // namespace tidy
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_TIDY_UTILS_FIXITHINTUTILS_H
