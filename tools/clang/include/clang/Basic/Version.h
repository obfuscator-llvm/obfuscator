//===- Version.h - Clang Version Number -------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
///
/// \file
/// \brief Defines version macros and version-related utility functions
/// for Clang.
///
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_BASIC_VERSION_H
#define LLVM_CLANG_BASIC_VERSION_H

#include "clang/Basic/Version.inc"
#include "llvm/ADT/StringRef.h"

namespace clang {
  /// \brief Retrieves the repository path (e.g., Subversion path) that
  /// identifies the particular Clang branch, tag, or trunk from which this
  /// Clang was built.
  std::string getClangRepositoryPath();

  /// \brief Retrieves the repository path from which LLVM was built.
  ///
  /// This supports LLVM residing in a separate repository from clang.
  std::string getLLVMRepositoryPath();

  /// \brief Retrieves the repository revision number (or identifer) from which
  /// this Clang was built.
  std::string getClangRevision();

  /// \brief Retrieves the repository revision number (or identifer) from which
  /// LLVM was built.
  ///
  /// If Clang and LLVM are in the same repository, this returns the same
  /// string as getClangRevision.
  std::string getLLVMRevision();

  /// \brief Retrieves the full repository version that is an amalgamation of
  /// the information in getClangRepositoryPath() and getClangRevision().
  std::string getClangFullRepositoryVersion();

  /// \brief Retrieves a string representing the complete clang version,
  /// which includes the clang version number, the repository version,
  /// and the vendor tag.
  std::string getClangFullVersion();

  /// \brief Like getClangFullVersion(), but with a custom tool name.
  std::string getClangToolFullVersion(llvm::StringRef ToolName);

  /// \brief Retrieves a string representing the complete clang version suitable
  /// for use in the CPP __VERSION__ macro, which includes the clang version
  /// number, the repository version, and the vendor tag.
  std::string getClangFullCPPVersion();
}

#endif // LLVM_CLANG_BASIC_VERSION_H
