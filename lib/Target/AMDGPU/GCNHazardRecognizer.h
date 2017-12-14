//===-- GCNHazardRecognizers.h - GCN Hazard Recognizers ---------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file defines hazard recognizers for scheduling on GCN processors.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_AMDGPUHAZARDRECOGNIZERS_H
#define LLVM_LIB_TARGET_AMDGPUHAZARDRECOGNIZERS_H

#include "llvm/ADT/STLExtras.h"
#include "llvm/CodeGen/ScheduleHazardRecognizer.h"
#include <list>

namespace llvm {

class MachineFunction;
class MachineInstr;
class ScheduleDAG;
class SIInstrInfo;
class SISubtarget;

class GCNHazardRecognizer final : public ScheduleHazardRecognizer {
  // This variable stores the instruction that has been emitted this cycle. It
  // will be added to EmittedInstrs, when AdvanceCycle() or RecedeCycle() is
  // called.
  MachineInstr *CurrCycleInstr;
  std::list<MachineInstr*> EmittedInstrs;
  const MachineFunction &MF;
  const SISubtarget &ST;
  const SIInstrInfo &TII;

  int getWaitStatesSince(function_ref<bool(MachineInstr *)> IsHazard);
  int getWaitStatesSinceDef(unsigned Reg,
                            function_ref<bool(MachineInstr *)> IsHazardDef =
                                [](MachineInstr *) { return true; });
  int getWaitStatesSinceSetReg(function_ref<bool(MachineInstr *)> IsHazard);

  int checkSMEMSoftClauseHazards(MachineInstr *SMEM);
  int checkSMRDHazards(MachineInstr *SMRD);
  int checkVMEMHazards(MachineInstr* VMEM);
  int checkDPPHazards(MachineInstr *DPP);
  int checkDivFMasHazards(MachineInstr *DivFMas);
  int checkGetRegHazards(MachineInstr *GetRegInstr);
  int checkSetRegHazards(MachineInstr *SetRegInstr);
  int createsVALUHazard(const MachineInstr &MI);
  int checkVALUHazards(MachineInstr *VALU);
  int checkRWLaneHazards(MachineInstr *RWLane);
  int checkRFEHazards(MachineInstr *RFE);
  int checkAnyInstHazards(MachineInstr *MI);
  int checkReadM0Hazards(MachineInstr *SMovRel);
public:
  GCNHazardRecognizer(const MachineFunction &MF);
  // We can only issue one instruction per cycle.
  bool atIssueLimit() const override { return true; }
  void EmitInstruction(SUnit *SU) override;
  void EmitInstruction(MachineInstr *MI) override;
  HazardType getHazardType(SUnit *SU, int Stalls) override;
  void EmitNoop() override;
  unsigned PreEmitNoops(SUnit *SU) override;
  unsigned PreEmitNoops(MachineInstr *) override;
  void AdvanceCycle() override;
  void RecedeCycle() override;
};

} // end namespace llvm

#endif //LLVM_LIB_TARGET_AMDGPUHAZARDRECOGNIZERS_H
