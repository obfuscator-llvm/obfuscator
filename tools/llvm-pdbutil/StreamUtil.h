//===- Streamutil.h - PDB stream utilities ----------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TOOLS_LLVMPDBDUMP_STREAMUTIL_H
#define LLVM_TOOLS_LLVMPDBDUMP_STREAMUTIL_H

#include "llvm/ADT/SmallVector.h"

#include <string>

namespace llvm {
namespace pdb {
class PDBFile;
enum class StreamPurpose { NamedStream, ModuleStream, Other };

void discoverStreamPurposes(PDBFile &File,
                            SmallVectorImpl<std::string> &Purposes);
void discoverStreamPurposes(
    PDBFile &File,
    SmallVectorImpl<std::pair<StreamPurpose, std::string>> &Purposes);
}
}

#endif
