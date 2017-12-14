//=-- SystemZHazardRecognizer.h - SystemZ Hazard Recognizer -----*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file declares a hazard recognizer for the SystemZ scheduler.
//
// This class is used by the SystemZ scheduling strategy to maintain
// the state during scheduling, and provide cost functions for
// scheduling candidates. This includes:
//
// * Decoder grouping. A decoder group can maximally hold 3 uops, and
// instructions that always begin a new group should be scheduled when
// the current decoder group is empty.
// * Processor resources usage. It is beneficial to balance the use of
// resources.
//
// ===---------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_SYSTEMZ_SYSTEMZHAZARDRECOGNIZER_H
#define LLVM_LIB_TARGET_SYSTEMZ_SYSTEMZHAZARDRECOGNIZER_H

#include "SystemZSubtarget.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineScheduler.h"
#include "llvm/CodeGen/ScheduleHazardRecognizer.h"
#include "llvm/MC/MCInstrDesc.h"
#include "llvm/Support/raw_ostream.h"
#include <string>

namespace llvm {

/// SystemZHazardRecognizer maintains the state during scheduling.
class SystemZHazardRecognizer : public ScheduleHazardRecognizer {

  ScheduleDAGMI *DAG;
  const TargetSchedModel *SchedModel;

  /// Keep track of the number of decoder slots used in the current
  /// decoder group.
  unsigned CurrGroupSize;

  /// The tracking of resources here are quite similar to the common
  /// code use of a critical resource. However, z13 differs in the way
  /// that it has two processor sides which may be interesting to
  /// model in the future (a work in progress).

  /// Counters for the number of uops scheduled per processor
  /// resource.
  SmallVector<int, 0> ProcResourceCounters;

  /// This is the resource with the greatest queue, which the
  /// scheduler tries to avoid.
  unsigned CriticalResourceIdx;

  /// Return the number of decoder slots MI requires.
  inline unsigned getNumDecoderSlots(SUnit *SU) const;

  /// Return true if MI fits into current decoder group.
  bool fitsIntoCurrentGroup(SUnit *SU) const;

  /// Two decoder groups per cycle are formed (for z13), meaning 2x3
  /// instructions. This function returns a number between 0 and 5,
  /// representing the current decoder slot of the current cycle.
  unsigned getCurrCycleIdx();
  
  /// LastFPdOpCycleIdx stores the numbeer returned by getCurrCycleIdx()
  /// when a stalling operation is scheduled (which uses the FPd resource).
  unsigned LastFPdOpCycleIdx;

  /// A counter of decoder groups scheduled.
  unsigned GrpCount;

  unsigned getCurrGroupSize() {return CurrGroupSize;};

  /// Start next decoder group.
  void nextGroup(bool DbgOutput = true);

  /// Clear all counters for processor resources.
  void clearProcResCounters();

  /// With the goal of alternating processor sides for stalling (FPd)
  /// ops, return true if it seems good to schedule an FPd op next.
  bool isFPdOpPreferred_distance(const SUnit *SU);

public:
  SystemZHazardRecognizer(const MachineSchedContext *C);

  void setDAG(ScheduleDAGMI *dag) {
    DAG = dag;
    SchedModel = dag->getSchedModel();
  }
  
  HazardType getHazardType(SUnit *m, int Stalls = 0) override;    
  void Reset() override;
  void EmitInstruction(SUnit *SU) override;

  // Cost functions used by SystemZPostRASchedStrategy while
  // evaluating candidates.

  /// Return the cost of decoder grouping for SU. If SU must start a
  /// new decoder group, this is negative if this fits the schedule or
  /// positive if it would mean ending a group prematurely. For normal
  /// instructions this returns 0.
  int groupingCost(SUnit *SU) const; 

  /// Return the cost of SU in regards to processor resources usage.
  /// A positive value means it would be better to wait with SU, while
  /// a negative value means it would be good to schedule SU next.
  int resourcesCost(SUnit *SU);

#ifndef NDEBUG
  // Debug dumping.
  std::string CurGroupDbg; // current group as text
  void dumpSU(SUnit *SU, raw_ostream &OS) const;
  void dumpCurrGroup(std::string Msg = "") const;
  void dumpProcResourceCounters() const;
#endif
};

} // namespace llvm

#endif /* LLVM_LIB_TARGET_SYSTEMZ_SYSTEMZHAZARDRECOGNIZER_H */
