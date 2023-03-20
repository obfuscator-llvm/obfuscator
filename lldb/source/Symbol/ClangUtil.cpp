//===-- ClangUtil.cpp -------------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
// A collection of helper methods and data structures for manipulating clang
// types and decls.
//===----------------------------------------------------------------------===//

#include "lldb/Symbol/ClangUtil.h"
#include "lldb/Symbol/ClangASTContext.h"

using namespace clang;
using namespace lldb_private;

bool ClangUtil::IsClangType(const CompilerType &ct) {
  if (llvm::dyn_cast_or_null<ClangASTContext>(ct.GetTypeSystem()) == nullptr)
    return false;

  if (!ct.GetOpaqueQualType())
    return false;

  return true;
}

QualType ClangUtil::GetQualType(const CompilerType &ct) {
  // Make sure we have a clang type before making a clang::QualType
  if (!IsClangType(ct))
    return QualType();

  return QualType::getFromOpaquePtr(ct.GetOpaqueQualType());
}

QualType ClangUtil::GetCanonicalQualType(const CompilerType &ct) {
  if (!IsClangType(ct))
    return QualType();

  return GetQualType(ct).getCanonicalType();
}

CompilerType ClangUtil::RemoveFastQualifiers(const CompilerType &ct) {
  if (!IsClangType(ct))
    return ct;

  QualType qual_type(GetQualType(ct));
  qual_type.removeLocalFastQualifiers();
  return CompilerType(ct.GetTypeSystem(), qual_type.getAsOpaquePtr());
}

clang::TagDecl *ClangUtil::GetAsTagDecl(const CompilerType &type) {
  clang::QualType qual_type = ClangUtil::GetCanonicalQualType(type);
  if (qual_type.isNull())
    return nullptr;

  return qual_type->getAsTagDecl();
}
