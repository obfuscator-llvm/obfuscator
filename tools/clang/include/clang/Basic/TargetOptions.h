//===--- TargetOptions.h ----------------------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
///
/// \file
/// \brief Defines the clang::TargetOptions class.
///
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_BASIC_TARGETOPTIONS_H
#define LLVM_CLANG_BASIC_TARGETOPTIONS_H

#include <string>
#include <vector>
#include "clang/Basic/OpenCLOptions.h"
#include "llvm/Target/TargetOptions.h"

namespace clang {

/// \brief Options for controlling the target.
class TargetOptions {
public:
  /// The name of the target triple to compile for.
  std::string Triple;

  /// When compiling for the device side, contains the triple used to compile
  /// for the host.
  std::string HostTriple;

  /// If given, the name of the target CPU to generate code for.
  std::string CPU;

  /// If given, the unit to use for floating point math.
  std::string FPMath;

  /// If given, the name of the target ABI to use.
  std::string ABI;

  /// The EABI version to use
  llvm::EABI EABIVersion;

  /// If given, the version string of the linker in use.
  std::string LinkerVersion;

  /// \brief The list of target specific features to enable or disable, as written on the command line.
  std::vector<std::string> FeaturesAsWritten;

  /// The list of target specific features to enable or disable -- this should
  /// be a list of strings starting with by '+' or '-'.
  std::vector<std::string> Features;

  std::vector<std::string> Reciprocals;

  /// Supported OpenCL extensions and optional core features.
  OpenCLOptions SupportedOpenCLOptions;

  /// \brief The list of OpenCL extensions to enable or disable, as written on
  /// the command line.
  std::vector<std::string> OpenCLExtensionsAsWritten;
};

}  // end namespace clang

#endif
