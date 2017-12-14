//===- lib/CodeGen/GlobalISel/LegalizerInfo.cpp - Legalizer ---------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// Implement an interface to specify and query how an illegal operation on a
// given type should be expanded.
//
// Issues to be resolved:
//   + Make it fast.
//   + Support weird types like i3, <7 x i3>, ...
//   + Operations with more than one type (ICMP, CMPXCHG, intrinsics, ...)
//
//===----------------------------------------------------------------------===//

#include "llvm/CodeGen/GlobalISel/LegalizerInfo.h"
#include "llvm/ADT/SmallBitVector.h"
#include "llvm/CodeGen/MachineInstr.h"
#include "llvm/CodeGen/MachineOperand.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/MC/MCInstrDesc.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/LowLevelTypeImpl.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/Target/TargetOpcodes.h"
#include <algorithm>
#include <cassert>
#include <tuple>
#include <utility>

using namespace llvm;

LegalizerInfo::LegalizerInfo() {
  DefaultActions[TargetOpcode::G_IMPLICIT_DEF] = NarrowScalar;

  // FIXME: these two can be legalized to the fundamental load/store Jakob
  // proposed. Once loads & stores are supported.
  DefaultActions[TargetOpcode::G_ANYEXT] = Legal;
  DefaultActions[TargetOpcode::G_TRUNC] = Legal;

  DefaultActions[TargetOpcode::G_INTRINSIC] = Legal;
  DefaultActions[TargetOpcode::G_INTRINSIC_W_SIDE_EFFECTS] = Legal;

  DefaultActions[TargetOpcode::G_ADD] = NarrowScalar;
  DefaultActions[TargetOpcode::G_LOAD] = NarrowScalar;
  DefaultActions[TargetOpcode::G_STORE] = NarrowScalar;

  DefaultActions[TargetOpcode::G_BRCOND] = WidenScalar;
  DefaultActions[TargetOpcode::G_INSERT] = NarrowScalar;
  DefaultActions[TargetOpcode::G_EXTRACT] = NarrowScalar;
  DefaultActions[TargetOpcode::G_FNEG] = Lower;
}

void LegalizerInfo::computeTables() {
  for (unsigned Opcode = 0; Opcode <= LastOp - FirstOp; ++Opcode) {
    for (unsigned Idx = 0; Idx != Actions[Opcode].size(); ++Idx) {
      for (auto &Action : Actions[Opcode][Idx]) {
        LLT Ty = Action.first;
        if (!Ty.isVector())
          continue;

        auto &Entry = MaxLegalVectorElts[std::make_pair(Opcode + FirstOp,
                                                        Ty.getElementType())];
        Entry = std::max(Entry, Ty.getNumElements());
      }
    }
  }

  TablesInitialized = true;
}

// FIXME: inefficient implementation for now. Without ComputeValueVTs we're
// probably going to need specialized lookup structures for various types before
// we have any hope of doing well with something like <13 x i3>. Even the common
// cases should do better than what we have now.
std::pair<LegalizerInfo::LegalizeAction, LLT>
LegalizerInfo::getAction(const InstrAspect &Aspect) const {
  assert(TablesInitialized && "backend forgot to call computeTables");
  // These *have* to be implemented for now, they're the fundamental basis of
  // how everything else is transformed.

  // FIXME: the long-term plan calls for expansion in terms of load/store (if
  // they're not legal).
  if (Aspect.Opcode == TargetOpcode::G_MERGE_VALUES ||
      Aspect.Opcode == TargetOpcode::G_UNMERGE_VALUES)
    return std::make_pair(Legal, Aspect.Type);

  LLT Ty = Aspect.Type;
  LegalizeAction Action = findInActions(Aspect);
  // LegalizerHelper is not able to handle non-power-of-2 types right now, so do
  // not try to legalize them unless they are marked as Legal or Custom.
  // FIXME: This is a temporary hack until the general non-power-of-2
  // legalization works.
  if (!isPowerOf2_64(Ty.getSizeInBits()) &&
      !(Action == Legal || Action == Custom))
    return std::make_pair(Unsupported, LLT());

  if (Action != NotFound)
    return findLegalAction(Aspect, Action);

  unsigned Opcode = Aspect.Opcode;
  if (!Ty.isVector()) {
    auto DefaultAction = DefaultActions.find(Aspect.Opcode);
    if (DefaultAction != DefaultActions.end() && DefaultAction->second == Legal)
      return std::make_pair(Legal, Ty);

    if (DefaultAction != DefaultActions.end() && DefaultAction->second == Lower)
      return std::make_pair(Lower, Ty);

    if (DefaultAction == DefaultActions.end() ||
        DefaultAction->second != NarrowScalar)
      return std::make_pair(Unsupported, LLT());
    return findLegalAction(Aspect, NarrowScalar);
  }

  LLT EltTy = Ty.getElementType();
  int NumElts = Ty.getNumElements();

  auto ScalarAction = ScalarInVectorActions.find(std::make_pair(Opcode, EltTy));
  if (ScalarAction != ScalarInVectorActions.end() &&
      ScalarAction->second != Legal)
    return findLegalAction(Aspect, ScalarAction->second);

  // The element type is legal in principle, but the number of elements is
  // wrong.
  auto MaxLegalElts = MaxLegalVectorElts.lookup(std::make_pair(Opcode, EltTy));
  if (MaxLegalElts > NumElts)
    return findLegalAction(Aspect, MoreElements);

  if (MaxLegalElts == 0) {
    // Scalarize if there's no legal vector type, which is just a special case
    // of FewerElements.
    return std::make_pair(FewerElements, EltTy);
  }

  return findLegalAction(Aspect, FewerElements);
}

std::tuple<LegalizerInfo::LegalizeAction, unsigned, LLT>
LegalizerInfo::getAction(const MachineInstr &MI,
                         const MachineRegisterInfo &MRI) const {
  SmallBitVector SeenTypes(8);
  const MCOperandInfo *OpInfo = MI.getDesc().OpInfo;
  for (unsigned i = 0; i < MI.getDesc().getNumOperands(); ++i) {
    if (!OpInfo[i].isGenericType())
      continue;

    // We don't want to repeatedly check the same operand index, that
    // could get expensive.
    unsigned TypeIdx = OpInfo[i].getGenericTypeIndex();
    if (SeenTypes[TypeIdx])
      continue;

    SeenTypes.set(TypeIdx);

    LLT Ty = MRI.getType(MI.getOperand(i).getReg());
    auto Action = getAction({MI.getOpcode(), TypeIdx, Ty});
    if (Action.first != Legal)
      return std::make_tuple(Action.first, TypeIdx, Action.second);
  }
  return std::make_tuple(Legal, 0, LLT{});
}

bool LegalizerInfo::isLegal(const MachineInstr &MI,
                            const MachineRegisterInfo &MRI) const {
  return std::get<0>(getAction(MI, MRI)) == Legal;
}

Optional<LLT> LegalizerInfo::findLegalType(const InstrAspect &Aspect,
                                 LegalizeAction Action) const {
  switch(Action) {
  default:
    llvm_unreachable("Cannot find legal type");
  case Legal:
  case Lower:
  case Libcall:
  case Custom:
    return Aspect.Type;
  case NarrowScalar: {
    return findLegalizableSize(
        Aspect, [&](LLT Ty) -> LLT { return Ty.halfScalarSize(); });
  }
  case WidenScalar: {
    return findLegalizableSize(Aspect, [&](LLT Ty) -> LLT {
      return Ty.getSizeInBits() < 8 ? LLT::scalar(8) : Ty.doubleScalarSize();
    });
  }
  case FewerElements: {
    return findLegalizableSize(
        Aspect, [&](LLT Ty) -> LLT { return Ty.halfElements(); });
  }
  case MoreElements: {
    return findLegalizableSize(
        Aspect, [&](LLT Ty) -> LLT { return Ty.doubleElements(); });
  }
  }
}

bool LegalizerInfo::legalizeCustom(MachineInstr &MI,
                                   MachineRegisterInfo &MRI,
                                   MachineIRBuilder &MIRBuilder) const {
  return false;
}
