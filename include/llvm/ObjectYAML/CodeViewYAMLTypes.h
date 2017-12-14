//==- CodeViewYAMLTypes.h - CodeView YAMLIO Type implementation --*- C++ -*-==//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file defines classes for handling the YAML representation of CodeView
// Debug Info.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_OBJECTYAML_CODEVIEWYAMLTYPES_H
#define LLVM_OBJECTYAML_CODEVIEWYAMLTYPES_H

#include "llvm/ADT/ArrayRef.h"
#include "llvm/DebugInfo/CodeView/TypeRecord.h"
#include "llvm/Support/Allocator.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/YAMLTraits.h"
#include <cstdint>
#include <memory>
#include <vector>

namespace llvm {

namespace codeview {

class TypeTableBuilder;

} // end namespace codeview

namespace CodeViewYAML {

namespace detail {

struct LeafRecordBase;
struct MemberRecordBase;

} // end namespace detail

struct MemberRecord {
  std::shared_ptr<detail::MemberRecordBase> Member;
};

struct LeafRecord {
  std::shared_ptr<detail::LeafRecordBase> Leaf;

  codeview::CVType toCodeViewRecord(BumpPtrAllocator &Allocator) const;
  codeview::CVType toCodeViewRecord(codeview::TypeTableBuilder &TS) const;
  static Expected<LeafRecord> fromCodeViewRecord(codeview::CVType Type);
};

std::vector<LeafRecord> fromDebugT(ArrayRef<uint8_t> DebugT);
ArrayRef<uint8_t> toDebugT(ArrayRef<LeafRecord>, BumpPtrAllocator &Alloc);

} // end namespace CodeViewYAML

} // end namespace llvm

LLVM_YAML_DECLARE_SCALAR_TRAITS(codeview::GUID, true)

LLVM_YAML_DECLARE_MAPPING_TRAITS(CodeViewYAML::LeafRecord)
LLVM_YAML_DECLARE_MAPPING_TRAITS(CodeViewYAML::MemberRecord)

LLVM_YAML_IS_SEQUENCE_VECTOR(CodeViewYAML::LeafRecord)
LLVM_YAML_IS_SEQUENCE_VECTOR(CodeViewYAML::MemberRecord)

#endif // LLVM_OBJECTYAML_CODEVIEWYAMLTYPES_H
