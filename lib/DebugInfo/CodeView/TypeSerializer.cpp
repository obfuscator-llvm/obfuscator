//===- TypeSerialzier.cpp -------------------------------------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "llvm/DebugInfo/CodeView/TypeSerializer.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/DebugInfo/CodeView/CodeView.h"
#include "llvm/DebugInfo/CodeView/RecordSerialization.h"
#include "llvm/DebugInfo/CodeView/TypeIndex.h"
#include "llvm/Support/Allocator.h"
#include "llvm/Support/BinaryByteStream.h"
#include "llvm/Support/BinaryStreamWriter.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/Error.h"
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstring>

using namespace llvm;
using namespace llvm::codeview;

namespace {

struct HashedType {
  uint64_t Hash;
  const uint8_t *Data;
  unsigned Size; // FIXME: Go to uint16_t?
  TypeIndex Index;
};

/// Wrapper around a poitner to a HashedType. Hash and equality operations are
/// based on data in the pointee.
struct HashedTypePtr {
  HashedTypePtr() = default;
  HashedTypePtr(HashedType *Ptr) : Ptr(Ptr) {}

  HashedType *Ptr = nullptr;
};

} // end anonymous namespace

namespace llvm {

template <> struct DenseMapInfo<HashedTypePtr> {
  static inline HashedTypePtr getEmptyKey() { return HashedTypePtr(nullptr); }

  static inline HashedTypePtr getTombstoneKey() {
    return HashedTypePtr(reinterpret_cast<HashedType *>(1));
  }

  static unsigned getHashValue(HashedTypePtr Val) {
    assert(Val.Ptr != getEmptyKey().Ptr && Val.Ptr != getTombstoneKey().Ptr);
    return Val.Ptr->Hash;
  }

  static bool isEqual(HashedTypePtr LHSP, HashedTypePtr RHSP) {
    HashedType *LHS = LHSP.Ptr;
    HashedType *RHS = RHSP.Ptr;
    if (RHS == getEmptyKey().Ptr || RHS == getTombstoneKey().Ptr)
      return LHS == RHS;
    if (LHS->Hash != RHS->Hash || LHS->Size != RHS->Size)
      return false;
    return ::memcmp(LHS->Data, RHS->Data, LHS->Size) == 0;
  }
};

} // end namespace llvm

/// Private implementation so that we don't leak our DenseMap instantiations to
/// users.
class llvm::codeview::TypeHasher {
private:
  /// Storage for type record provided by the caller. Records will outlive the
  /// hasher object, so they should be allocated here.
  BumpPtrAllocator &RecordStorage;

  /// Storage for hash keys. These only need to live as long as the hashing
  /// operation.
  BumpPtrAllocator KeyStorage;

  /// Hash table. We really want a DenseMap<ArrayRef<uint8_t>, TypeIndex> here,
  /// but DenseMap is inefficient when the keys are long (like type records)
  /// because it recomputes the hash value of every key when it grows. This
  /// value type stores the hash out of line in KeyStorage, so that table
  /// entries are small and easy to rehash.
  DenseSet<HashedTypePtr> HashedRecords;

public:
  TypeHasher(BumpPtrAllocator &RecordStorage) : RecordStorage(RecordStorage) {}

  void reset() { HashedRecords.clear(); }

  /// Takes the bytes of type record, inserts them into the hash table, saves
  /// them, and returns a pointer to an identical stable type record along with
  /// its type index in the destination stream.
  TypeIndex getOrCreateRecord(ArrayRef<uint8_t> &Record, TypeIndex TI);
};

TypeIndex TypeHasher::getOrCreateRecord(ArrayRef<uint8_t> &Record,
                                        TypeIndex TI) {
  assert(Record.size() < UINT32_MAX && "Record too big");
  assert(Record.size() % 4 == 0 && "Record is not aligned to 4 bytes!");

  // Compute the hash up front so we can store it in the key.
  HashedType TempHashedType = {hash_value(Record), Record.data(),
                               unsigned(Record.size()), TI};
  auto Result = HashedRecords.insert(HashedTypePtr(&TempHashedType));
  HashedType *&Hashed = Result.first->Ptr;

  if (Result.second) {
    // This was a new type record. We need stable storage for both the key and
    // the record. The record should outlive the hashing operation.
    Hashed = KeyStorage.Allocate<HashedType>();
    *Hashed = TempHashedType;

    uint8_t *Stable = RecordStorage.Allocate<uint8_t>(Record.size());
    memcpy(Stable, Record.data(), Record.size());
    Hashed->Data = Stable;
    assert(Hashed->Size == Record.size());
  }

  // Update the caller's copy of Record to point a stable copy.
  Record = ArrayRef<uint8_t>(Hashed->Data, Hashed->Size);
  return Hashed->Index;
}

TypeIndex TypeSerializer::nextTypeIndex() const {
  return TypeIndex::fromArrayIndex(SeenRecords.size());
}

bool TypeSerializer::isInFieldList() const {
  return TypeKind.hasValue() && *TypeKind == TypeLeafKind::LF_FIELDLIST;
}

MutableArrayRef<uint8_t> TypeSerializer::getCurrentSubRecordData() {
  assert(isInFieldList());
  return getCurrentRecordData().drop_front(CurrentSegment.length());
}

MutableArrayRef<uint8_t> TypeSerializer::getCurrentRecordData() {
  return MutableArrayRef<uint8_t>(RecordBuffer).take_front(Writer.getOffset());
}

Error TypeSerializer::writeRecordPrefix(TypeLeafKind Kind) {
  RecordPrefix Prefix;
  Prefix.RecordKind = Kind;
  Prefix.RecordLen = 0;
  if (auto EC = Writer.writeObject(Prefix))
    return EC;
  return Error::success();
}

Expected<MutableArrayRef<uint8_t>>
TypeSerializer::addPadding(MutableArrayRef<uint8_t> Record) {
  uint32_t Align = Record.size() % 4;
  if (Align == 0)
    return Record;

  int PaddingBytes = 4 - Align;
  int N = PaddingBytes;
  while (PaddingBytes > 0) {
    uint8_t Pad = static_cast<uint8_t>(LF_PAD0 + PaddingBytes);
    if (auto EC = Writer.writeInteger(Pad))
      return std::move(EC);
    --PaddingBytes;
  }
  return MutableArrayRef<uint8_t>(Record.data(), Record.size() + N);
}

TypeSerializer::TypeSerializer(BumpPtrAllocator &Storage, bool Hash)
    : RecordStorage(Storage), RecordBuffer(MaxRecordLength * 2),
      Stream(RecordBuffer, support::little), Writer(Stream),
      Mapping(Writer) {
  // RecordBuffer needs to be able to hold enough data so that if we are 1
  // byte short of MaxRecordLen, and then we try to write MaxRecordLen bytes,
  // we won't overflow.
  if (Hash)
    Hasher = llvm::make_unique<TypeHasher>(Storage);
}

TypeSerializer::~TypeSerializer() = default;

ArrayRef<ArrayRef<uint8_t>> TypeSerializer::records() const {
  return SeenRecords;
}

void TypeSerializer::reset() {
  if (Hasher)
    Hasher->reset();
  Writer.setOffset(0);
  CurrentSegment = RecordSegment();
  FieldListSegments.clear();
  TypeKind.reset();
  MemberKind.reset();
  SeenRecords.clear();
}

TypeIndex TypeSerializer::insertRecordBytes(ArrayRef<uint8_t> &Record) {
  assert(!TypeKind.hasValue() && "Already in a type mapping!");
  assert(Writer.getOffset() == 0 && "Stream has data already!");

  if (Hasher) {
    TypeIndex ActualTI = Hasher->getOrCreateRecord(Record, nextTypeIndex());
    if (nextTypeIndex() == ActualTI)
      SeenRecords.push_back(Record);
    return ActualTI;
  }

  TypeIndex NewTI = nextTypeIndex();
  uint8_t *Stable = RecordStorage.Allocate<uint8_t>(Record.size());
  memcpy(Stable, Record.data(), Record.size());
  Record = ArrayRef<uint8_t>(Stable, Record.size());
  SeenRecords.push_back(Record);
  return NewTI;
}

TypeIndex TypeSerializer::insertRecord(const RemappedType &Record) {
  assert(!TypeKind.hasValue() && "Already in a type mapping!");
  assert(Writer.getOffset() == 0 && "Stream has data already!");

  TypeIndex TI;
  ArrayRef<uint8_t> OriginalData = Record.OriginalRecord.RecordData;
  if (Record.Mappings.empty()) {
    // This record did not remap any type indices.  Just write it.
    return insertRecordBytes(OriginalData);
  }

  // At least one type index was remapped.  Before we can hash it we have to
  // copy the full record bytes, re-write each type index, then hash the copy.
  // We do this in temporary storage since only the DenseMap can decide whether
  // this record already exists, and if it does we don't want the memory to
  // stick around.
  RemapStorage.resize(OriginalData.size());
  ::memcpy(&RemapStorage[0], OriginalData.data(), OriginalData.size());
  uint8_t *ContentBegin = RemapStorage.data() + sizeof(RecordPrefix);
  for (const auto &M : Record.Mappings) {
    // First 4 bytes of every record are the record prefix, but the mapping
    // offset is relative to the content which starts after.
    *(TypeIndex *)(ContentBegin + M.first) = M.second;
  }
  auto RemapRef = makeArrayRef(RemapStorage);
  return insertRecordBytes(RemapRef);
}

Error TypeSerializer::visitTypeBegin(CVType &Record) {
  assert(!TypeKind.hasValue() && "Already in a type mapping!");
  assert(Writer.getOffset() == 0 && "Stream has data already!");

  if (auto EC = writeRecordPrefix(Record.kind()))
    return EC;

  TypeKind = Record.kind();
  if (auto EC = Mapping.visitTypeBegin(Record))
    return EC;

  return Error::success();
}

Expected<TypeIndex> TypeSerializer::visitTypeEndGetIndex(CVType &Record) {
  assert(TypeKind.hasValue() && "Not in a type mapping!");
  if (auto EC = Mapping.visitTypeEnd(Record))
    return std::move(EC);

  // Update the record's length and fill out the CVType members to point to
  // the stable memory holding the record's data.
  auto ThisRecordData = getCurrentRecordData();
  auto ExpectedData = addPadding(ThisRecordData);
  if (!ExpectedData)
    return ExpectedData.takeError();
  ThisRecordData = *ExpectedData;

  RecordPrefix *Prefix =
      reinterpret_cast<RecordPrefix *>(ThisRecordData.data());
  Prefix->RecordLen = ThisRecordData.size() - sizeof(uint16_t);

  Record.Type = *TypeKind;
  Record.RecordData = ThisRecordData;

  // insertRecordBytes assumes we're not in a mapping, so do this first.
  TypeKind.reset();
  Writer.setOffset(0);

  TypeIndex InsertedTypeIndex = insertRecordBytes(Record.RecordData);

  // Write out each additional segment in reverse order, and update each
  // record's continuation index to point to the previous one.
  for (auto X : reverse(FieldListSegments)) {
    auto CIBytes = X.take_back(sizeof(uint32_t));
    support::ulittle32_t *CI =
        reinterpret_cast<support::ulittle32_t *>(CIBytes.data());
    assert(*CI == 0xB0C0B0C0 && "Invalid TypeIndex placeholder");
    *CI = InsertedTypeIndex.getIndex();
    InsertedTypeIndex = insertRecordBytes(X);
  }

  FieldListSegments.clear();
  CurrentSegment.SubRecords.clear();

  return InsertedTypeIndex;
}

Error TypeSerializer::visitTypeEnd(CVType &Record) {
  auto ExpectedIndex = visitTypeEndGetIndex(Record);
  if (!ExpectedIndex)
    return ExpectedIndex.takeError();
  return Error::success();
}

Error TypeSerializer::visitMemberBegin(CVMemberRecord &Record) {
  assert(isInFieldList() && "Not in a field list!");
  assert(!MemberKind.hasValue() && "Already in a member record!");
  MemberKind = Record.Kind;

  if (auto EC = Mapping.visitMemberBegin(Record))
    return EC;

  return Error::success();
}

Error TypeSerializer::visitMemberEnd(CVMemberRecord &Record) {
  if (auto EC = Mapping.visitMemberEnd(Record))
    return EC;

  // Check if this subrecord makes the current segment not fit in 64K minus
  // the space for a continuation record (8 bytes). If the segment does not
  // fit, insert a continuation record.
  if (Writer.getOffset() > MaxRecordLength - ContinuationLength) {
    MutableArrayRef<uint8_t> Data = getCurrentRecordData();
    SubRecord LastSubRecord = CurrentSegment.SubRecords.back();
    uint32_t CopySize = CurrentSegment.length() - LastSubRecord.Size;
    auto CopyData = Data.take_front(CopySize);
    auto LeftOverData = Data.drop_front(CopySize);
    assert(LastSubRecord.Size == LeftOverData.size());

    // Allocate stable storage for the record and copy the old record plus
    // continuation over.
    uint16_t LengthWithSize = CopySize + ContinuationLength;
    assert(LengthWithSize <= MaxRecordLength);
    RecordPrefix *Prefix = reinterpret_cast<RecordPrefix *>(CopyData.data());
    Prefix->RecordLen = LengthWithSize - sizeof(uint16_t);

    uint8_t *SegmentBytes = RecordStorage.Allocate<uint8_t>(LengthWithSize);
    auto SavedSegment = MutableArrayRef<uint8_t>(SegmentBytes, LengthWithSize);
    MutableBinaryByteStream CS(SavedSegment, support::little);
    BinaryStreamWriter CW(CS);
    if (auto EC = CW.writeBytes(CopyData))
      return EC;
    if (auto EC = CW.writeEnum(TypeLeafKind::LF_INDEX))
      return EC;
    if (auto EC = CW.writeInteger<uint16_t>(0))
      return EC;
    if (auto EC = CW.writeInteger<uint32_t>(0xB0C0B0C0))
      return EC;
    FieldListSegments.push_back(SavedSegment);

    // Write a new placeholder record prefix to mark the start of this new
    // top-level record.
    Writer.setOffset(0);
    if (auto EC = writeRecordPrefix(TypeLeafKind::LF_FIELDLIST))
      return EC;

    // Then move over the subrecord that overflowed the old segment to the
    // beginning of this segment.  Note that we have to use memmove here
    // instead of Writer.writeBytes(), because the new and old locations
    // could overlap.
    ::memmove(Stream.data().data() + sizeof(RecordPrefix), LeftOverData.data(),
              LeftOverData.size());
    // And point the segment writer at the end of that subrecord.
    Writer.setOffset(LeftOverData.size() + sizeof(RecordPrefix));

    CurrentSegment.SubRecords.clear();
    CurrentSegment.SubRecords.push_back(LastSubRecord);
  }

  // Update the CVMemberRecord since we may have shifted around or gotten
  // padded.
  Record.Data = getCurrentSubRecordData();

  MemberKind.reset();
  return Error::success();
}
