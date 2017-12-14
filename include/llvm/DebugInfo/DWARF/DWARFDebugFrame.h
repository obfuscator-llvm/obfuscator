//===- DWARFDebugFrame.h - Parsing of .debug_frame --------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_DEBUGINFO_DWARF_DWARFDEBUGFRAME_H
#define LLVM_DEBUGINFO_DWARF_DWARFDEBUGFRAME_H

#include "llvm/Support/DataExtractor.h"
#include <memory>
#include <vector>

namespace llvm {

class FrameEntry;
class raw_ostream;

/// \brief A parsed .debug_frame or .eh_frame section
///
class DWARFDebugFrame {
  // True if this is parsing an eh_frame section.
  bool IsEH;

public:
  DWARFDebugFrame(bool IsEH);
  ~DWARFDebugFrame();

  /// \brief Dump the section data into the given stream.
  void dump(raw_ostream &OS) const;

  /// \brief Parse the section from raw data.
  /// data is assumed to be pointing to the beginning of the section.
  void parse(DataExtractor Data);

private:
  std::vector<std::unique_ptr<FrameEntry>> Entries;
};

} // end namespace llvm

#endif // LLVM_DEBUGINFO_DWARF_DWARFDEBUGFRAME_H
