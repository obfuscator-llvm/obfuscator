//===- TargetSubtargetInfo.cpp - General Target Information ----------------==//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
/// \file This file describes the general parts of a Subtarget.
//
//===----------------------------------------------------------------------===//

#include "llvm/ADT/Optional.h"
#include "llvm/CodeGen/MachineInstr.h"
#include "llvm/CodeGen/TargetSchedule.h"
#include "llvm/MC/MCInst.h"
#include "llvm/Target/TargetSubtargetInfo.h"
#include "llvm/Support/Format.h"
#include "llvm/Support/raw_ostream.h"
#include <string>

using namespace llvm;

TargetSubtargetInfo::TargetSubtargetInfo(
    const Triple &TT, StringRef CPU, StringRef FS,
    ArrayRef<SubtargetFeatureKV> PF, ArrayRef<SubtargetFeatureKV> PD,
    const SubtargetInfoKV *ProcSched, const MCWriteProcResEntry *WPR,
    const MCWriteLatencyEntry *WL, const MCReadAdvanceEntry *RA,
    const InstrStage *IS, const unsigned *OC, const unsigned *FP)
    : MCSubtargetInfo(TT, CPU, FS, PF, PD, ProcSched, WPR, WL, RA, IS, OC, FP) {
}

TargetSubtargetInfo::~TargetSubtargetInfo() = default;

bool TargetSubtargetInfo::enableAtomicExpand() const {
  return true;
}

bool TargetSubtargetInfo::enableMachineScheduler() const {
  return false;
}

bool TargetSubtargetInfo::enableJoinGlobalCopies() const {
  return enableMachineScheduler();
}

bool TargetSubtargetInfo::enableRALocalReassignment(
    CodeGenOpt::Level OptLevel) const {
  return true;
}

bool TargetSubtargetInfo::enablePostRAScheduler() const {
  return getSchedModel().PostRAScheduler;
}

bool TargetSubtargetInfo::useAA() const {
  return false;
}

static std::string createSchedInfoStr(unsigned Latency,
                                     Optional<double> RThroughput) {
  static const char *SchedPrefix = " sched: [";
  std::string Comment;
  raw_string_ostream CS(Comment);
  if (Latency > 0 && RThroughput.hasValue())
    CS << SchedPrefix << Latency << format(":%2.2f", RThroughput.getValue())
       << "]";
  else if (Latency > 0)
    CS << SchedPrefix << Latency << ":?]";
  else if (RThroughput.hasValue())
    CS << SchedPrefix << "?:" << RThroughput.getValue() << "]";
  CS.flush();
  return Comment;
}

/// Returns string representation of scheduler comment
std::string TargetSubtargetInfo::getSchedInfoStr(const MachineInstr &MI) const {
  if (MI.isPseudo() || MI.isTerminator())
    return std::string();
  // We don't cache TSchedModel because it depends on TargetInstrInfo
  // that could be changed during the compilation
  TargetSchedModel TSchedModel;
  TSchedModel.init(getSchedModel(), this, getInstrInfo());
  unsigned Latency = TSchedModel.computeInstrLatency(&MI);
  Optional<double> RThroughput = TSchedModel.computeInstrRThroughput(&MI);
  return createSchedInfoStr(Latency, RThroughput);
}

/// Returns string representation of scheduler comment
std::string TargetSubtargetInfo::getSchedInfoStr(MCInst const &MCI) const {
  // We don't cache TSchedModel because it depends on TargetInstrInfo
  // that could be changed during the compilation
  TargetSchedModel TSchedModel;
  TSchedModel.init(getSchedModel(), this, getInstrInfo());
  if (!TSchedModel.hasInstrSchedModel())
    return std::string();
  unsigned Latency = TSchedModel.computeInstrLatency(MCI.getOpcode());
  Optional<double> RThroughput =
      TSchedModel.computeInstrRThroughput(MCI.getOpcode());
  return createSchedInfoStr(Latency, RThroughput);
}
