//===--- Relation.h ----------------------------------------------*- C++-*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANGD_INDEX_RELATION_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANGD_INDEX_RELATION_H

#include "SymbolID.h"
#include "SymbolLocation.h"
#include "clang/Index/IndexSymbol.h"
#include "llvm/ADT/iterator_range.h"
#include <cstdint>
#include <utility>

namespace clang {
namespace clangd {

/// Represents a relation between two symbols.
/// For an example "A is a base class of B" may be represented
/// as { Subject = A, Predicate = RelationBaseOf, Object = B }.
struct Relation {
  SymbolID Subject;
  index::SymbolRole Predicate;
  SymbolID Object;

  bool operator==(const Relation &Other) const {
    return std::tie(Subject, Predicate, Object) ==
           std::tie(Other.Subject, Other.Predicate, Other.Object);
  }
  // SPO order
  bool operator<(const Relation &Other) const {
    return std::tie(Subject, Predicate, Object) <
           std::tie(Other.Subject, Other.Predicate, Other.Object);
  }
};

class RelationSlab {
public:
  using value_type = Relation;
  using const_iterator = std::vector<value_type>::const_iterator;
  using iterator = const_iterator;

  RelationSlab() = default;
  RelationSlab(RelationSlab &&Slab) = default;
  RelationSlab &operator=(RelationSlab &&RHS) = default;

  const_iterator begin() const { return Relations.begin(); }
  const_iterator end() const { return Relations.end(); }
  size_t size() const { return Relations.size(); }
  bool empty() const { return Relations.empty(); }

  size_t bytes() const {
    return sizeof(*this) + sizeof(value_type) * Relations.capacity();
  }

  /// Lookup all relations matching the given subject and predicate.
  llvm::iterator_range<iterator> lookup(const SymbolID &Subject,
                                        index::SymbolRole Predicate) const;

  /// RelationSlab::Builder is a mutable container that can 'freeze' to
  /// RelationSlab.
  class Builder {
  public:
    /// Adds a relation to the slab.
    void insert(const Relation &R) { Relations.push_back(R); }

    /// Consumes the builder to finalize the slab.
    RelationSlab build() &&;

  private:
    std::vector<Relation> Relations;
  };

private:
  RelationSlab(std::vector<Relation> Relations)
      : Relations(std::move(Relations)) {}

  std::vector<Relation> Relations;
};

} // namespace clangd
} // namespace clang

namespace llvm {

// Support index::SymbolRole as a DenseMap key for the purpose of looking up
// relations.
template <> struct DenseMapInfo<clang::index::SymbolRole> {
  static inline clang::index::SymbolRole getEmptyKey() {
    // Choose an enumerator that's not a relation.
    return clang::index::SymbolRole::Declaration;
  }

  static inline clang::index::SymbolRole getTombstoneKey() {
    // Choose another enumerator that's not a relation.
    return clang::index::SymbolRole::Definition;
  }

  static unsigned getHashValue(const clang::index::SymbolRole &Key) {
    return hash_value(Key);
  }

  static bool isEqual(const clang::index::SymbolRole &LHS,
                      const clang::index::SymbolRole &RHS) {
    return LHS == RHS;
  }
};

} // namespace llvm

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANGD_INDEX_RELATION_H
