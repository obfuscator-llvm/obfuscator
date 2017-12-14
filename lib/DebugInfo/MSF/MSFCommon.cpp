//===- MSFCommon.cpp - Common types and functions for MSF files -----------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "llvm/DebugInfo/MSF/MSFCommon.h"
#include "llvm/DebugInfo/MSF/MSFError.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/Error.h"
#include <cstdint>
#include <cstring>

using namespace llvm;
using namespace llvm::msf;

Error llvm::msf::validateSuperBlock(const SuperBlock &SB) {
  // Check the magic bytes.
  if (std::memcmp(SB.MagicBytes, Magic, sizeof(Magic)) != 0)
    return make_error<MSFError>(msf_error_code::invalid_format,
                                "MSF magic header doesn't match");

  if (!isValidBlockSize(SB.BlockSize))
    return make_error<MSFError>(msf_error_code::invalid_format,
                                "Unsupported block size.");

  // We don't support directories whose sizes aren't a multiple of four bytes.
  if (SB.NumDirectoryBytes % sizeof(support::ulittle32_t) != 0)
    return make_error<MSFError>(msf_error_code::invalid_format,
                                "Directory size is not multiple of 4.");

  // The number of blocks which comprise the directory is a simple function of
  // the number of bytes it contains.
  uint64_t NumDirectoryBlocks =
      bytesToBlocks(SB.NumDirectoryBytes, SB.BlockSize);

  // The directory, as we understand it, is a block which consists of a list of
  // block numbers.  It is unclear what would happen if the number of blocks
  // couldn't fit on a single block.
  if (NumDirectoryBlocks > SB.BlockSize / sizeof(support::ulittle32_t))
    return make_error<MSFError>(msf_error_code::invalid_format,
                                "Too many directory blocks.");

  if (SB.BlockMapAddr == 0)
    return make_error<MSFError>(msf_error_code::invalid_format,
                                "Block 0 is reserved");

  if (SB.BlockMapAddr >= SB.NumBlocks)
    return make_error<MSFError>(msf_error_code::invalid_format,
                                "Block map address is invalid.");

  if (SB.FreeBlockMapBlock != 1 && SB.FreeBlockMapBlock != 2)
    return make_error<MSFError>(
        msf_error_code::invalid_format,
        "The free block map isn't at block 1 or block 2.");

  return Error::success();
}
