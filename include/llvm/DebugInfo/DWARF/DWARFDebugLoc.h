//===- DWARFDebugLoc.h ------------------------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_DEBUGINFO_DWARF_DWARFDEBUGLOC_H
#define LLVM_DEBUGINFO_DWARF_DWARFDEBUGLOC_H

#include "llvm/ADT/SmallVector.h"
#include "llvm/DebugInfo/DWARF/DWARFDataExtractor.h"
#include "llvm/DebugInfo/DWARF/DWARFRelocMap.h"
#include <cstdint>

namespace llvm {

class raw_ostream;

class DWARFDebugLoc {
  /// A single location within a location list.
  struct Entry {
    /// The beginning address of the instruction range.
    uint64_t Begin;
    /// The ending address of the instruction range.
    uint64_t End;
    /// The location of the variable within the specified range.
    SmallVector<unsigned char, 4> Loc;
  };

  /// A list of locations that contain one variable.
  struct LocationList {
    /// The beginning offset where this location list is stored in the debug_loc
    /// section.
    unsigned Offset;
    /// All the locations in which the variable is stored.
    SmallVector<Entry, 2> Entries;
  };

  using LocationLists = SmallVector<LocationList, 4>;

  /// A list of all the variables in the debug_loc section, each one describing
  /// the locations in which the variable is stored.
  LocationLists Locations;

public:
  /// Print the location lists found within the debug_loc section.
  void dump(raw_ostream &OS) const;

  /// Parse the debug_loc section accessible via the 'data' parameter using the
  /// address size also given in 'data' to interpret the address ranges.
  void parse(const DWARFDataExtractor &data);
};

class DWARFDebugLocDWO {
  struct Entry {
    uint64_t Start;
    uint32_t Length;
    SmallVector<unsigned char, 4> Loc;
  };

  struct LocationList {
    unsigned Offset;
    SmallVector<Entry, 2> Entries;
  };

  using LocationLists = SmallVector<LocationList, 4>;

  LocationLists Locations;

public:
  void parse(DataExtractor data);
  void dump(raw_ostream &OS) const;
};

} // end namespace llvm

#endif // LLVM_DEBUGINFO_DWARF_DWARFDEBUGLOC_H
