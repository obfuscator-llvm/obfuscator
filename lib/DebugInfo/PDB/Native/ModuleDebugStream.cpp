//===- ModuleDebugStream.cpp - PDB Module Info Stream Access --------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "llvm/DebugInfo/PDB/Native/ModuleDebugStream.h"
#include "llvm/ADT/iterator_range.h"
#include "llvm/DebugInfo/CodeView/CodeView.h"
#include "llvm/DebugInfo/CodeView/DebugChecksumsSubsection.h"
#include "llvm/DebugInfo/CodeView/SymbolRecord.h"
#include "llvm/DebugInfo/PDB/Native/DbiModuleDescriptor.h"
#include "llvm/DebugInfo/PDB/Native/RawError.h"
#include "llvm/Support/BinaryStreamReader.h"
#include "llvm/Support/BinaryStreamRef.h"
#include "llvm/Support/Error.h"
#include <algorithm>
#include <cstdint>

using namespace llvm;
using namespace llvm::codeview;
using namespace llvm::msf;
using namespace llvm::pdb;

ModuleDebugStreamRef::ModuleDebugStreamRef(
    const DbiModuleDescriptor &Module,
    std::unique_ptr<MappedBlockStream> Stream)
    : Mod(Module), Stream(std::move(Stream)) {}

ModuleDebugStreamRef::~ModuleDebugStreamRef() = default;

Error ModuleDebugStreamRef::reload() {
  BinaryStreamReader Reader(*Stream);

  uint32_t SymbolSize = Mod.getSymbolDebugInfoByteSize();
  uint32_t C11Size = Mod.getC11LineInfoByteSize();
  uint32_t C13Size = Mod.getC13LineInfoByteSize();

  if (C11Size > 0 && C13Size > 0)
    return make_error<RawError>(raw_error_code::corrupt_file,
                                "Module has both C11 and C13 line info");

  BinaryStreamRef S;

  if (auto EC = Reader.readInteger(Signature))
    return EC;
  if (auto EC = Reader.readSubstream(SymbolsSubstream, SymbolSize - 4))
    return EC;
  if (auto EC = Reader.readSubstream(C11LinesSubstream, C11Size))
    return EC;
  if (auto EC = Reader.readSubstream(C13LinesSubstream, C13Size))
    return EC;

  BinaryStreamReader SymbolReader(SymbolsSubstream.StreamData);
  if (auto EC =
          SymbolReader.readArray(SymbolArray, SymbolReader.bytesRemaining()))
    return EC;

  BinaryStreamReader SubsectionsReader(C13LinesSubstream.StreamData);
  if (auto EC = SubsectionsReader.readArray(Subsections,
                                            SubsectionsReader.bytesRemaining()))
    return EC;

  uint32_t GlobalRefsSize;
  if (auto EC = Reader.readInteger(GlobalRefsSize))
    return EC;
  if (auto EC = Reader.readSubstream(GlobalRefsSubstream, GlobalRefsSize))
    return EC;
  if (Reader.bytesRemaining() > 0)
    return make_error<RawError>(raw_error_code::corrupt_file,
                                "Unexpected bytes in module stream.");

  return Error::success();
}

BinarySubstreamRef ModuleDebugStreamRef::getSymbolsSubstream() const {
  return SymbolsSubstream;
}

BinarySubstreamRef ModuleDebugStreamRef::getC11LinesSubstream() const {
  return C11LinesSubstream;
}

BinarySubstreamRef ModuleDebugStreamRef::getC13LinesSubstream() const {
  return C13LinesSubstream;
}

BinarySubstreamRef ModuleDebugStreamRef::getGlobalRefsSubstream() const {
  return GlobalRefsSubstream;
}

iterator_range<codeview::CVSymbolArray::Iterator>
ModuleDebugStreamRef::symbols(bool *HadError) const {
  return make_range(SymbolArray.begin(HadError), SymbolArray.end());
}

iterator_range<ModuleDebugStreamRef::DebugSubsectionIterator>
ModuleDebugStreamRef::subsections() const {
  return make_range(Subsections.begin(), Subsections.end());
}

bool ModuleDebugStreamRef::hasDebugSubsections() const {
  return !C13LinesSubstream.empty();
}

Error ModuleDebugStreamRef::commit() { return Error::success(); }

Expected<codeview::DebugChecksumsSubsectionRef>
ModuleDebugStreamRef::findChecksumsSubsection() const {
  codeview::DebugChecksumsSubsectionRef Result;
  for (const auto &SS : subsections()) {
    if (SS.kind() != DebugSubsectionKind::FileChecksums)
      continue;

    if (auto EC = Result.initialize(SS.getRecordData()))
      return std::move(EC);
    return Result;
  }
  return Result;
}
