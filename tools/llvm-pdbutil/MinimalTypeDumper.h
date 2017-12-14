//===- MinimalTypeDumper.h ------------------------------------ *- C++ --*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TOOLS_LLVMPDBUTIL_MINIMAL_TYPE_DUMPER_H
#define LLVM_TOOLS_LLVMPDBUTIL_MINIMAL_TYPE_DUMPER_H

#include "llvm/DebugInfo/CodeView/TypeVisitorCallbacks.h"
#include "llvm/Support/BinaryStreamArray.h"

namespace llvm {
namespace codeview {
class LazyRandomTypeCollection;
}

namespace pdb {
class LinePrinter;

class MinimalTypeDumpVisitor : public codeview::TypeVisitorCallbacks {
public:
  MinimalTypeDumpVisitor(LinePrinter &P, uint32_t Width, bool RecordBytes,
                         bool Hashes, codeview::LazyRandomTypeCollection &Types,
                         uint32_t NumHashBuckets,
                         FixedStreamArray<support::ulittle32_t> HashValues)
      : P(P), Width(Width), RecordBytes(RecordBytes), Hashes(Hashes),
        Types(Types), NumHashBuckets(NumHashBuckets), HashValues(HashValues) {}

  Error visitTypeBegin(codeview::CVType &Record,
                       codeview::TypeIndex Index) override;
  Error visitTypeEnd(codeview::CVType &Record) override;
  Error visitMemberBegin(codeview::CVMemberRecord &Record) override;
  Error visitMemberEnd(codeview::CVMemberRecord &Record) override;

#define TYPE_RECORD(EnumName, EnumVal, Name)                                   \
  Error visitKnownRecord(codeview::CVType &CVR,                                \
                         codeview::Name##Record &Record) override;
#define MEMBER_RECORD(EnumName, EnumVal, Name)                                 \
  Error visitKnownMember(codeview::CVMemberRecord &CVR,                        \
                         codeview::Name##Record &Record) override;
#define TYPE_RECORD_ALIAS(EnumName, EnumVal, Name, AliasName)
#define MEMBER_RECORD_ALIAS(EnumName, EnumVal, Name, AliasName)
#include "llvm/DebugInfo/CodeView/CodeViewTypes.def"

private:
  StringRef getTypeName(codeview::TypeIndex TI) const;

  LinePrinter &P;
  uint32_t Width;
  bool RecordBytes = false;
  bool Hashes = false;
  codeview::LazyRandomTypeCollection &Types;
  uint32_t NumHashBuckets;
  FixedStreamArray<support::ulittle32_t> HashValues;
};
} // namespace pdb
} // namespace llvm

#endif
