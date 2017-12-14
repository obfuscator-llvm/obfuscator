//===- TpiStream.cpp - PDB Type Info (TPI) Stream 2 Access ----------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "llvm/DebugInfo/PDB/Native/TpiStream.h"

#include "llvm/ADT/iterator_range.h"
#include "llvm/DebugInfo/CodeView/LazyRandomTypeCollection.h"
#include "llvm/DebugInfo/CodeView/TypeRecord.h"
#include "llvm/DebugInfo/MSF/MappedBlockStream.h"
#include "llvm/DebugInfo/PDB/Native/PDBFile.h"
#include "llvm/DebugInfo/PDB/Native/RawConstants.h"
#include "llvm/DebugInfo/PDB/Native/RawError.h"
#include "llvm/DebugInfo/PDB/Native/RawTypes.h"
#include "llvm/DebugInfo/PDB/Native/TpiHashing.h"
#include "llvm/Support/BinaryStreamReader.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/Error.h"
#include <algorithm>
#include <cstdint>
#include <vector>

using namespace llvm;
using namespace llvm::codeview;
using namespace llvm::support;
using namespace llvm::msf;
using namespace llvm::pdb;

TpiStream::TpiStream(PDBFile &File, std::unique_ptr<MappedBlockStream> Stream)
    : Pdb(File), Stream(std::move(Stream)) {}

TpiStream::~TpiStream() = default;

Error TpiStream::reload() {
  BinaryStreamReader Reader(*Stream);

  if (Reader.bytesRemaining() < sizeof(TpiStreamHeader))
    return make_error<RawError>(raw_error_code::corrupt_file,
                                "TPI Stream does not contain a header.");

  if (Reader.readObject(Header))
    return make_error<RawError>(raw_error_code::corrupt_file,
                                "TPI Stream does not contain a header.");

  if (Header->Version != PdbTpiV80)
    return make_error<RawError>(raw_error_code::corrupt_file,
                                "Unsupported TPI Version.");

  if (Header->HeaderSize != sizeof(TpiStreamHeader))
    return make_error<RawError>(raw_error_code::corrupt_file,
                                "Corrupt TPI Header size.");

  if (Header->HashKeySize != sizeof(ulittle32_t))
    return make_error<RawError>(raw_error_code::corrupt_file,
                                "TPI Stream expected 4 byte hash key size.");

  if (Header->NumHashBuckets < MinTpiHashBuckets ||
      Header->NumHashBuckets > MaxTpiHashBuckets)
    return make_error<RawError>(raw_error_code::corrupt_file,
                                "TPI Stream Invalid number of hash buckets.");

  // The actual type records themselves come from this stream
  if (auto EC =
          Reader.readSubstream(TypeRecordsSubstream, Header->TypeRecordBytes))
    return EC;

  BinaryStreamReader RecordReader(TypeRecordsSubstream.StreamData);
  if (auto EC =
          RecordReader.readArray(TypeRecords, TypeRecordsSubstream.size()))
    return EC;

  // Hash indices, hash values, etc come from the hash stream.
  if (Header->HashStreamIndex != kInvalidStreamIndex) {
    if (Header->HashStreamIndex >= Pdb.getNumStreams())
      return make_error<RawError>(raw_error_code::corrupt_file,
                                  "Invalid TPI hash stream index.");

    auto HS = MappedBlockStream::createIndexedStream(
        Pdb.getMsfLayout(), Pdb.getMsfBuffer(), Header->HashStreamIndex,
        Pdb.getAllocator());
    BinaryStreamReader HSR(*HS);

    // There should be a hash value for every type record, or no hashes at all.
    uint32_t NumHashValues =
        Header->HashValueBuffer.Length / sizeof(ulittle32_t);
    if (NumHashValues != getNumTypeRecords() && NumHashValues != 0)
      return make_error<RawError>(
          raw_error_code::corrupt_file,
          "TPI hash count does not match with the number of type records.");
    HSR.setOffset(Header->HashValueBuffer.Off);
    if (auto EC = HSR.readArray(HashValues, NumHashValues))
      return EC;

    HSR.setOffset(Header->IndexOffsetBuffer.Off);
    uint32_t NumTypeIndexOffsets =
        Header->IndexOffsetBuffer.Length / sizeof(TypeIndexOffset);
    if (auto EC = HSR.readArray(TypeIndexOffsets, NumTypeIndexOffsets))
      return EC;

    if (Header->HashAdjBuffer.Length > 0) {
      HSR.setOffset(Header->HashAdjBuffer.Off);
      if (auto EC = HashAdjusters.load(HSR))
        return EC;
    }

    HashStream = std::move(HS);
  }

  Types = llvm::make_unique<LazyRandomTypeCollection>(
      TypeRecords, getNumTypeRecords(), getTypeIndexOffsets());
  return Error::success();
}

PdbRaw_TpiVer TpiStream::getTpiVersion() const {
  uint32_t Value = Header->Version;
  return static_cast<PdbRaw_TpiVer>(Value);
}

uint32_t TpiStream::TypeIndexBegin() const { return Header->TypeIndexBegin; }

uint32_t TpiStream::TypeIndexEnd() const { return Header->TypeIndexEnd; }

uint32_t TpiStream::getNumTypeRecords() const {
  return TypeIndexEnd() - TypeIndexBegin();
}

uint16_t TpiStream::getTypeHashStreamIndex() const {
  return Header->HashStreamIndex;
}

uint16_t TpiStream::getTypeHashStreamAuxIndex() const {
  return Header->HashAuxStreamIndex;
}

uint32_t TpiStream::getNumHashBuckets() const { return Header->NumHashBuckets; }
uint32_t TpiStream::getHashKeySize() const { return Header->HashKeySize; }

BinarySubstreamRef TpiStream::getTypeRecordsSubstream() const {
  return TypeRecordsSubstream;
}

FixedStreamArray<support::ulittle32_t> TpiStream::getHashValues() const {
  return HashValues;
}

FixedStreamArray<TypeIndexOffset> TpiStream::getTypeIndexOffsets() const {
  return TypeIndexOffsets;
}

HashTable &TpiStream::getHashAdjusters() { return HashAdjusters; }

CVTypeRange TpiStream::types(bool *HadError) const {
  return make_range(TypeRecords.begin(HadError), TypeRecords.end());
}

Error TpiStream::commit() { return Error::success(); }
