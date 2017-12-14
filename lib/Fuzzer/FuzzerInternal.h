//===- FuzzerInternal.h - Internal header for the Fuzzer --------*- C++ -* ===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
// Define the main class fuzzer::Fuzzer and most functions.
//===----------------------------------------------------------------------===//

#ifndef LLVM_FUZZER_INTERNAL_H
#define LLVM_FUZZER_INTERNAL_H

#include "FuzzerDefs.h"
#include "FuzzerExtFunctions.h"
#include "FuzzerInterface.h"
#include "FuzzerOptions.h"
#include "FuzzerSHA1.h"
#include "FuzzerValueBitMap.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <climits>
#include <cstdlib>
#include <string.h>

namespace fuzzer {

using namespace std::chrono;

class Fuzzer {
public:

  Fuzzer(UserCallback CB, InputCorpus &Corpus, MutationDispatcher &MD,
         FuzzingOptions Options);
  ~Fuzzer();
  void Loop();
  void MinimizeCrashLoop(const Unit &U);
  void ShuffleAndMinimize(UnitVector *V);
  void RereadOutputCorpus(size_t MaxSize);

  size_t secondsSinceProcessStartUp() {
    return duration_cast<seconds>(system_clock::now() - ProcessStartTime)
        .count();
  }

  bool TimedOut() {
    return Options.MaxTotalTimeSec > 0 &&
           secondsSinceProcessStartUp() >
               static_cast<size_t>(Options.MaxTotalTimeSec);
  }

  size_t execPerSec() {
    size_t Seconds = secondsSinceProcessStartUp();
    return Seconds ? TotalNumberOfRuns / Seconds : 0;
  }

  size_t getTotalNumberOfRuns() { return TotalNumberOfRuns; }

  static void StaticAlarmCallback();
  static void StaticCrashSignalCallback();
  static void StaticInterruptCallback();
  static void StaticFileSizeExceedCallback();

  void ExecuteCallback(const uint8_t *Data, size_t Size);
  bool RunOne(const uint8_t *Data, size_t Size, bool MayDeleteFile = false,
              InputInfo *II = nullptr);

  // Merge Corpora[1:] into Corpora[0].
  void Merge(const std::vector<std::string> &Corpora);
  void CrashResistantMerge(const std::vector<std::string> &Args,
                           const std::vector<std::string> &Corpora,
                           const char *CoverageSummaryInputPathOrNull,
                           const char *CoverageSummaryOutputPathOrNull);
  void CrashResistantMergeInternalStep(const std::string &ControlFilePath);
  MutationDispatcher &GetMD() { return MD; }
  void PrintFinalStats();
  void SetMaxInputLen(size_t MaxInputLen);
  void SetMaxMutationLen(size_t MaxMutationLen);
  void RssLimitCallback();

  bool InFuzzingThread() const { return IsMyThread; }
  size_t GetCurrentUnitInFuzzingThead(const uint8_t **Data) const;
  void TryDetectingAMemoryLeak(const uint8_t *Data, size_t Size,
                               bool DuringInitialCorpusExecution);

  void HandleMalloc(size_t Size);
  void AnnounceOutput(const uint8_t *Data, size_t Size);

private:
  void AlarmCallback();
  void CrashCallback();
  void CrashOnOverwrittenData();
  void InterruptCallback();
  void MutateAndTestOne();
  void ReportNewCoverage(InputInfo *II, const Unit &U);
  void PrintPulseAndReportSlowInput(const uint8_t *Data, size_t Size);
  void WriteToOutputCorpus(const Unit &U);
  void WriteUnitToFileWithPrefix(const Unit &U, const char *Prefix);
  void PrintStats(const char *Where, const char *End = "\n", size_t Units = 0);
  void PrintStatusForNewUnit(const Unit &U, const char *Text);
  void ShuffleCorpus(UnitVector *V);
  void CheckExitOnSrcPosOrItem();

  static void StaticDeathCallback();
  void DumpCurrentUnit(const char *Prefix);
  void DeathCallback();

  void AllocateCurrentUnitData();
  uint8_t *CurrentUnitData = nullptr;
  std::atomic<size_t> CurrentUnitSize;
  uint8_t BaseSha1[kSHA1NumBytes];  // Checksum of the base unit.
  bool RunningCB = false;

  size_t TotalNumberOfRuns = 0;
  size_t NumberOfNewUnitsAdded = 0;

  bool HasMoreMallocsThanFrees = false;
  size_t NumberOfLeakDetectionAttempts = 0;

  UserCallback CB;
  InputCorpus &Corpus;
  MutationDispatcher &MD;
  FuzzingOptions Options;

  system_clock::time_point ProcessStartTime = system_clock::now();
  system_clock::time_point UnitStartTime, UnitStopTime;
  long TimeOfLongestUnitInSeconds = 0;
  long EpochOfLastReadOfOutputCorpus = 0;

  size_t MaxInputLen = 0;
  size_t MaxMutationLen = 0;

  std::vector<uint32_t> UniqFeatureSetTmp;

  // Need to know our own thread.
  static thread_local bool IsMyThread;
};

} // namespace fuzzer

#endif // LLVM_FUZZER_INTERNAL_H
