//===--- AtomicChange.h - AtomicChange class --------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
//  This file defines AtomicChange which is used to create a set of source
//  changes, e.g. replacements and header insertions.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLING_REFACTOR_ATOMICCHANGE_H
#define LLVM_CLANG_TOOLING_REFACTOR_ATOMICCHANGE_H

#include "clang/Basic/SourceManager.h"
#include "clang/Tooling/Core/Replacement.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Error.h"

namespace clang {
namespace tooling {

/// \brief An atomic change is used to create and group a set of source edits,
/// e.g. replacements or header insertions. Edits in an AtomicChange should be
/// related, e.g. replacements for the same type reference and the corresponding
/// header insertion/deletion.
///
/// An AtomicChange is uniquely identified by a key and will either be fully
/// applied or not applied at all.
///
/// Calling setError on an AtomicChange stores the error message and marks it as
/// bad, i.e. none of its source edits will be applied.
class AtomicChange {
public:
  /// \brief Creates an atomic change around \p KeyPosition with the key being a
  /// concatenation of the file name and the offset of \p KeyPosition.
  /// \p KeyPosition should be the location of the key syntactical element that
  /// is being changed, e.g. the call to a refactored method.
  AtomicChange(const SourceManager &SM, SourceLocation KeyPosition);

  /// \brief Creates an atomic change for \p FilePath with a customized key.
  AtomicChange(llvm::StringRef FilePath, llvm::StringRef Key)
      : Key(Key), FilePath(FilePath) {}

  /// \brief Returns the atomic change as a YAML string.
  std::string toYAMLString();

  /// \brief Converts a YAML-encoded automic change to AtomicChange.
  static AtomicChange convertFromYAML(llvm::StringRef YAMLContent);

  /// \brief Returns the key of this change, which is a concatenation of the
  /// file name and offset of the key position.
  const std::string &getKey() const { return Key; }

  /// \brief Returns the path of the file containing this atomic change.
  const std::string &getFilePath() const { return FilePath; }

  /// \brief If this change could not be created successfully, e.g. because of
  /// conflicts among replacements, use this to set an error description.
  /// Thereby, places that cannot be fixed automatically can be gathered when
  /// applying changes.
  void setError(llvm::StringRef Error) { this->Error = Error; }

  /// \brief Returns whether an error has been set on this list.
  bool hasError() const { return !Error.empty(); }

  /// \brief Returns the error message or an empty string if it does not exist.
  const std::string &getError() const { return Error; }

  /// \brief Adds a replacement that replaces the given Range with
  /// ReplacementText.
  /// \returns An llvm::Error carrying ReplacementError on error.
  llvm::Error replace(const SourceManager &SM, const CharSourceRange &Range,
                      llvm::StringRef ReplacementText);

  /// \brief Adds a replacement that replaces range [Loc, Loc+Length) with
  /// \p Text.
  /// \returns An llvm::Error carrying ReplacementError on error.
  llvm::Error replace(const SourceManager &SM, SourceLocation Loc,
                      unsigned Length, llvm::StringRef Text);

  /// \brief Adds a replacement that inserts \p Text at \p Loc. If this
  /// insertion conflicts with an existing insertion (at the same position),
  /// this will be inserted before/after the existing insertion depending on
  /// \p InsertAfter. Users should use `replace` with `Length=0` instead if they
  /// do not want conflict resolving by default. If the conflicting replacement
  /// is not an insertion, an error is returned.
  ///
  /// \returns An llvm::Error carrying ReplacementError on error.
  llvm::Error insert(const SourceManager &SM, SourceLocation Loc,
                     llvm::StringRef Text, bool InsertAfter = true);

  /// \brief Adds a header into the file that contains the key position.
  /// Header can be in angle brackets or double quotation marks. By default
  /// (header is not quoted), header will be surrounded with double quotes.
  void addHeader(llvm::StringRef Header);

  /// \brief Removes a header from the file that contains the key position.
  void removeHeader(llvm::StringRef Header);

  /// \brief Returns a const reference to existing replacements.
  const Replacements &getReplacements() const { return Replaces; }

  llvm::ArrayRef<std::string> getInsertedHeaders() const {
    return InsertedHeaders;
  }

  llvm::ArrayRef<std::string> getRemovedHeaders() const {
    return RemovedHeaders;
  }

private:
  AtomicChange() {}

  AtomicChange(std::string Key, std::string FilePath, std::string Error,
               std::vector<std::string> InsertedHeaders,
               std::vector<std::string> RemovedHeaders,
               clang::tooling::Replacements Replaces);

  // This uniquely identifies an AtomicChange.
  std::string Key;
  std::string FilePath;
  std::string Error;
  std::vector<std::string> InsertedHeaders;
  std::vector<std::string> RemovedHeaders;
  tooling::Replacements Replaces;
};

} // end namespace tooling
} // end namespace clang

#endif // LLVM_CLANG_TOOLING_REFACTOR_ATOMICCHANGE_H
