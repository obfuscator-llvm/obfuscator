//===- SampleProfWriter.h - Write LLVM sample profile data ------*- C++ -*-===//
//
//                      The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file contains definitions needed for writing sample profiles.
//
//===----------------------------------------------------------------------===//
#ifndef LLVM_PROFILEDATA_SAMPLEPROFWRITER_H
#define LLVM_PROFILEDATA_SAMPLEPROFWRITER_H

#include "llvm/ADT/MapVector.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/IR/ProfileSummary.h"
#include "llvm/ProfileData/SampleProf.h"
#include "llvm/Support/ErrorOr.h"
#include "llvm/Support/raw_ostream.h"
#include <algorithm>
#include <cstdint>
#include <memory>
#include <system_error>

namespace llvm {
namespace sampleprof {

enum SampleProfileFormat { SPF_None = 0, SPF_Text, SPF_Binary, SPF_GCC };

/// \brief Sample-based profile writer. Base class.
class SampleProfileWriter {
public:
  virtual ~SampleProfileWriter() = default;

  /// Write sample profiles in \p S.
  ///
  /// \returns status code of the file update operation.
  virtual std::error_code write(const FunctionSamples &S) = 0;

  /// Write all the sample profiles in the given map of samples.
  ///
  /// \returns status code of the file update operation.
  std::error_code write(const StringMap<FunctionSamples> &ProfileMap);

  raw_ostream &getOutputStream() { return *OutputStream; }

  /// Profile writer factory.
  ///
  /// Create a new file writer based on the value of \p Format.
  static ErrorOr<std::unique_ptr<SampleProfileWriter>>
  create(StringRef Filename, SampleProfileFormat Format);

  /// Create a new stream writer based on the value of \p Format.
  /// For testing.
  static ErrorOr<std::unique_ptr<SampleProfileWriter>>
  create(std::unique_ptr<raw_ostream> &OS, SampleProfileFormat Format);

protected:
  SampleProfileWriter(std::unique_ptr<raw_ostream> &OS)
      : OutputStream(std::move(OS)) {}

  /// \brief Write a file header for the profile file.
  virtual std::error_code
  writeHeader(const StringMap<FunctionSamples> &ProfileMap) = 0;

  /// \brief Output stream where to emit the profile to.
  std::unique_ptr<raw_ostream> OutputStream;

  /// \brief Profile summary.
  std::unique_ptr<ProfileSummary> Summary;

  /// \brief Compute summary for this profile.
  void computeSummary(const StringMap<FunctionSamples> &ProfileMap);
};

/// \brief Sample-based profile writer (text format).
class SampleProfileWriterText : public SampleProfileWriter {
public:
  std::error_code write(const FunctionSamples &S) override;

protected:
  SampleProfileWriterText(std::unique_ptr<raw_ostream> &OS)
      : SampleProfileWriter(OS), Indent(0) {}

  std::error_code
  writeHeader(const StringMap<FunctionSamples> &ProfileMap) override {
    return sampleprof_error::success;
  }

private:
  /// Indent level to use when writing.
  ///
  /// This is used when printing inlined callees.
  unsigned Indent;

  friend ErrorOr<std::unique_ptr<SampleProfileWriter>>
  SampleProfileWriter::create(std::unique_ptr<raw_ostream> &OS,
                              SampleProfileFormat Format);
};

/// \brief Sample-based profile writer (binary format).
class SampleProfileWriterBinary : public SampleProfileWriter {
public:
  std::error_code write(const FunctionSamples &S) override;

protected:
  SampleProfileWriterBinary(std::unique_ptr<raw_ostream> &OS)
      : SampleProfileWriter(OS) {}

  std::error_code
  writeHeader(const StringMap<FunctionSamples> &ProfileMap) override;
  std::error_code writeSummary();
  std::error_code writeNameIdx(StringRef FName);
  std::error_code writeBody(const FunctionSamples &S);

private:
  void addName(StringRef FName);
  void addNames(const FunctionSamples &S);

  MapVector<StringRef, uint32_t> NameTable;

  friend ErrorOr<std::unique_ptr<SampleProfileWriter>>
  SampleProfileWriter::create(std::unique_ptr<raw_ostream> &OS,
                              SampleProfileFormat Format);
};

} // end namespace sampleprof
} // end namespace llvm

#endif // LLVM_PROFILEDATA_SAMPLEPROFWRITER_H
