//===-- TypeValidator.cpp ---------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//




#include "lldb/DataFormatters/TypeValidator.h"
#include "lldb/Utility/StreamString.h"

using namespace lldb;
using namespace lldb_private;

TypeValidatorImpl::TypeValidatorImpl(const Flags &flags)
    : m_flags(flags), m_my_revision(0) {}

TypeValidatorImpl::~TypeValidatorImpl() {}

TypeValidatorImpl::ValidationResult TypeValidatorImpl::Success() {
  return ValidationResult{TypeValidatorResult::Success, ""};
}

TypeValidatorImpl::ValidationResult
TypeValidatorImpl::Failure(std::string message) {
  return ValidationResult{TypeValidatorResult::Failure, message};
}

TypeValidatorImpl_CXX::TypeValidatorImpl_CXX(
    ValidatorFunction f, std::string d, const TypeValidatorImpl::Flags &flags)
    : TypeValidatorImpl(flags), m_description(d), m_validator_function(f) {}

TypeValidatorImpl_CXX::~TypeValidatorImpl_CXX() {}

TypeValidatorImpl::ValidationResult
TypeValidatorImpl_CXX::FormatObject(ValueObject *valobj) const {
  if (!valobj)
    return Success(); // I guess there's nothing wrong with a null valueobject..

  return m_validator_function(valobj);
}

std::string TypeValidatorImpl_CXX::GetDescription() {
  StreamString sstr;
  sstr.Printf("%s%s%s%s", m_description.c_str(),
              Cascades() ? "" : " (not cascading)",
              SkipsPointers() ? " (skip pointers)" : "",
              SkipsReferences() ? " (skip references)" : "");
  return sstr.GetString();
}
