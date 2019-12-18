//===-- BenchmarkResult.h ---------------------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Defines classes to represent measurements and serialize/deserialize them to
//  Yaml.
///
//===----------------------------------------------------------------------===//

#ifndef LLVM_TOOLS_LLVM_EXEGESIS_BENCHMARKRESULT_H
#define LLVM_TOOLS_LLVM_EXEGESIS_BENCHMARKRESULT_H

#include "BenchmarkCode.h"
#include "LlvmState.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/MC/MCInst.h"
#include "llvm/MC/MCInstBuilder.h"
#include "llvm/Support/YAMLTraits.h"
#include <limits>
#include <string>
#include <unordered_map>
#include <vector>

namespace llvm {
namespace exegesis {

struct InstructionBenchmarkKey {
  // The LLVM opcode name.
  std::vector<llvm::MCInst> Instructions;
  // The initial values of the registers.
  std::vector<RegisterValue> RegisterInitialValues;
  // An opaque configuration, that can be used to separate several benchmarks of
  // the same instruction under different configurations.
  std::string Config;
};

struct BenchmarkMeasure {
  // A helper to create an unscaled BenchmarkMeasure.
  static BenchmarkMeasure Create(std::string Key, double Value) {
    return {Key, Value, Value};
  }
  std::string Key;
  // This is the per-instruction value, i.e. measured quantity scaled per
  // instruction.
  double PerInstructionValue;
  // This is the per-snippet value, i.e. measured quantity for one repetition of
  // the whole snippet.
  double PerSnippetValue;
};

// The result of an instruction benchmark.
struct InstructionBenchmark {
  InstructionBenchmarkKey Key;
  enum ModeE { Unknown, Latency, Uops };
  ModeE Mode;
  std::string CpuName;
  std::string LLVMTriple;
  // The number of instructions inside the repeated snippet. For example, if a
  // snippet of 3 instructions is repeated 4 times, this is 12.
  int NumRepetitions = 0;
  // Note that measurements are per instruction.
  std::vector<BenchmarkMeasure> Measurements;
  std::string Error;
  std::string Info;
  std::vector<uint8_t> AssembledSnippet;

  // Read functions.
  static llvm::Expected<InstructionBenchmark>
  readYaml(const LLVMState &State, llvm::StringRef Filename);

  static llvm::Expected<std::vector<InstructionBenchmark>>
  readYamls(const LLVMState &State, llvm::StringRef Filename);

  void readYamlFrom(const LLVMState &State, llvm::StringRef InputContent);

  // Write functions, non-const because of YAML traits.
  void writeYamlTo(const LLVMState &State, llvm::raw_ostream &S);

  llvm::Error writeYaml(const LLVMState &State, const llvm::StringRef Filename);
};

//------------------------------------------------------------------------------
// Utilities to work with Benchmark measures.

// A class that measures stats over benchmark measures.
class PerInstructionStats {
public:
  void push(const BenchmarkMeasure &BM);

  double avg() const {
    assert(NumValues);
    return SumValues / NumValues;
  }
  double min() const { return MinValue; }
  double max() const { return MaxValue; }

  const std::string &key() const { return Key; }

private:
  std::string Key;
  double SumValues = 0.0;
  int NumValues = 0;
  double MaxValue = std::numeric_limits<double>::min();
  double MinValue = std::numeric_limits<double>::max();
};

} // namespace exegesis
} // namespace llvm

#endif // LLVM_TOOLS_LLVM_EXEGESIS_BENCHMARKRESULT_H
