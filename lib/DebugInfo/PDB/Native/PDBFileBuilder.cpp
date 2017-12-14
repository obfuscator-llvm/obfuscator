//===- PDBFileBuilder.cpp - PDB File Creation -------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "llvm/DebugInfo/PDB/Native/PDBFileBuilder.h"

#include "llvm/ADT/BitVector.h"

#include "llvm/DebugInfo/MSF/MSFBuilder.h"
#include "llvm/DebugInfo/PDB/GenericError.h"
#include "llvm/DebugInfo/PDB/Native/DbiStream.h"
#include "llvm/DebugInfo/PDB/Native/DbiStreamBuilder.h"
#include "llvm/DebugInfo/PDB/Native/InfoStream.h"
#include "llvm/DebugInfo/PDB/Native/InfoStreamBuilder.h"
#include "llvm/DebugInfo/PDB/Native/PDBStringTableBuilder.h"
#include "llvm/DebugInfo/PDB/Native/PublicsStreamBuilder.h"
#include "llvm/DebugInfo/PDB/Native/RawError.h"
#include "llvm/DebugInfo/PDB/Native/TpiStream.h"
#include "llvm/DebugInfo/PDB/Native/TpiStreamBuilder.h"
#include "llvm/Support/BinaryStream.h"
#include "llvm/Support/BinaryStreamWriter.h"

using namespace llvm;
using namespace llvm::codeview;
using namespace llvm::msf;
using namespace llvm::pdb;
using namespace llvm::support;

PDBFileBuilder::PDBFileBuilder(BumpPtrAllocator &Allocator)
    : Allocator(Allocator) {}

PDBFileBuilder::~PDBFileBuilder() {}

Error PDBFileBuilder::initialize(uint32_t BlockSize) {
  auto ExpectedMsf = MSFBuilder::create(Allocator, BlockSize);
  if (!ExpectedMsf)
    return ExpectedMsf.takeError();
  Msf = llvm::make_unique<MSFBuilder>(std::move(*ExpectedMsf));
  return Error::success();
}

MSFBuilder &PDBFileBuilder::getMsfBuilder() { return *Msf; }

InfoStreamBuilder &PDBFileBuilder::getInfoBuilder() {
  if (!Info)
    Info = llvm::make_unique<InfoStreamBuilder>(*Msf, NamedStreams);
  return *Info;
}

DbiStreamBuilder &PDBFileBuilder::getDbiBuilder() {
  if (!Dbi)
    Dbi = llvm::make_unique<DbiStreamBuilder>(*Msf);
  return *Dbi;
}

TpiStreamBuilder &PDBFileBuilder::getTpiBuilder() {
  if (!Tpi)
    Tpi = llvm::make_unique<TpiStreamBuilder>(*Msf, StreamTPI);
  return *Tpi;
}

TpiStreamBuilder &PDBFileBuilder::getIpiBuilder() {
  if (!Ipi)
    Ipi = llvm::make_unique<TpiStreamBuilder>(*Msf, StreamIPI);
  return *Ipi;
}

PDBStringTableBuilder &PDBFileBuilder::getStringTableBuilder() {
  return Strings;
}

PublicsStreamBuilder &PDBFileBuilder::getPublicsBuilder() {
  if (!Publics)
    Publics = llvm::make_unique<PublicsStreamBuilder>(*Msf);
  return *Publics;
}

Error PDBFileBuilder::addNamedStream(StringRef Name, uint32_t Size) {
  auto ExpectedStream = Msf->addStream(Size);
  if (!ExpectedStream)
    return ExpectedStream.takeError();
  NamedStreams.set(Name, *ExpectedStream);
  return Error::success();
}

Expected<msf::MSFLayout> PDBFileBuilder::finalizeMsfLayout() {

  if (Ipi && Ipi->getRecordCount() > 0) {
    // In theory newer PDBs always have an ID stream, but by saying that we're
    // only going to *really* have an ID stream if there is at least one ID
    // record, we leave open the opportunity to test older PDBs such as those
    // that don't have an ID stream.
    auto &Info = getInfoBuilder();
    Info.addFeature(PdbRaw_FeatureSig::VC140);
  }

  uint32_t StringsLen = Strings.calculateSerializedSize();

  if (auto EC = addNamedStream("/names", StringsLen))
    return std::move(EC);
  if (auto EC = addNamedStream("/LinkInfo", 0))
    return std::move(EC);

  if (Info) {
    if (auto EC = Info->finalizeMsfLayout())
      return std::move(EC);
  }
  if (Dbi) {
    if (auto EC = Dbi->finalizeMsfLayout())
      return std::move(EC);
  }
  if (Tpi) {
    if (auto EC = Tpi->finalizeMsfLayout())
      return std::move(EC);
  }
  if (Ipi) {
    if (auto EC = Ipi->finalizeMsfLayout())
      return std::move(EC);
  }
  if (Publics) {
    if (auto EC = Publics->finalizeMsfLayout())
      return std::move(EC);
    if (Dbi) {
      Dbi->setPublicsStreamIndex(Publics->getStreamIndex());
      Dbi->setSymbolRecordStreamIndex(Publics->getRecordStreamIdx());
    }
  }

  return Msf->build();
}

Expected<uint32_t> PDBFileBuilder::getNamedStreamIndex(StringRef Name) const {
  uint32_t SN = 0;
  if (!NamedStreams.get(Name, SN))
    return llvm::make_error<pdb::RawError>(raw_error_code::no_stream);
  return SN;
}

Error PDBFileBuilder::commit(StringRef Filename) {
  assert(!Filename.empty());
  auto ExpectedLayout = finalizeMsfLayout();
  if (!ExpectedLayout)
    return ExpectedLayout.takeError();
  auto &Layout = *ExpectedLayout;

  uint64_t Filesize = Layout.SB->BlockSize * Layout.SB->NumBlocks;
  auto OutFileOrError = FileOutputBuffer::create(Filename, Filesize);
  if (OutFileOrError.getError())
    return llvm::make_error<pdb::GenericError>(generic_error_code::invalid_path,
                                               Filename);
  FileBufferByteStream Buffer(std::move(*OutFileOrError),
                              llvm::support::little);
  BinaryStreamWriter Writer(Buffer);

  if (auto EC = Writer.writeObject(*Layout.SB))
    return EC;
  uint32_t BlockMapOffset =
      msf::blockToOffset(Layout.SB->BlockMapAddr, Layout.SB->BlockSize);
  Writer.setOffset(BlockMapOffset);
  if (auto EC = Writer.writeArray(Layout.DirectoryBlocks))
    return EC;

  auto DirStream = WritableMappedBlockStream::createDirectoryStream(
      Layout, Buffer, Allocator);
  BinaryStreamWriter DW(*DirStream);
  if (auto EC = DW.writeInteger<uint32_t>(Layout.StreamSizes.size()))
    return EC;

  if (auto EC = DW.writeArray(Layout.StreamSizes))
    return EC;

  for (const auto &Blocks : Layout.StreamMap) {
    if (auto EC = DW.writeArray(Blocks))
      return EC;
  }

  auto ExpectedSN = getNamedStreamIndex("/names");
  if (!ExpectedSN)
    return ExpectedSN.takeError();

  auto NS = WritableMappedBlockStream::createIndexedStream(
      Layout, Buffer, *ExpectedSN, Allocator);
  BinaryStreamWriter NSWriter(*NS);
  if (auto EC = Strings.commit(NSWriter))
    return EC;

  if (Info) {
    if (auto EC = Info->commit(Layout, Buffer))
      return EC;
  }

  if (Dbi) {
    if (auto EC = Dbi->commit(Layout, Buffer))
      return EC;
  }

  if (Tpi) {
    if (auto EC = Tpi->commit(Layout, Buffer))
      return EC;
  }

  if (Ipi) {
    if (auto EC = Ipi->commit(Layout, Buffer))
      return EC;
  }

  if (Publics) {
    auto PS = WritableMappedBlockStream::createIndexedStream(
        Layout, Buffer, Publics->getStreamIndex(), Allocator);
    BinaryStreamWriter PSWriter(*PS);
    if (auto EC = Publics->commit(PSWriter))
      return EC;
  }

  return Buffer.commit();
}
