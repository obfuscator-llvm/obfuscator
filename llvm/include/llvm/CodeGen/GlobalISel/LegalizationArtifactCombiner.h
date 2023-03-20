//===-- llvm/CodeGen/GlobalISel/LegalizationArtifactCombiner.h -----*- C++ -*-//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
// This file contains some helper functions which try to cleanup artifacts
// such as G_TRUNCs/G_[ZSA]EXTENDS that were created during legalization to make
// the types match. This file also contains some combines of merges that happens
// at the end of the legalization.
//===----------------------------------------------------------------------===//

#include "llvm/CodeGen/GlobalISel/Legalizer.h"
#include "llvm/CodeGen/GlobalISel/LegalizerInfo.h"
#include "llvm/CodeGen/GlobalISel/MIPatternMatch.h"
#include "llvm/CodeGen/GlobalISel/MachineIRBuilder.h"
#include "llvm/CodeGen/GlobalISel/Utils.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/Support/Debug.h"

#define DEBUG_TYPE "legalizer"
using namespace llvm::MIPatternMatch;

namespace llvm {
class LegalizationArtifactCombiner {
  MachineIRBuilder &Builder;
  MachineRegisterInfo &MRI;
  const LegalizerInfo &LI;

  static bool isArtifactCast(unsigned Opc) {
    switch (Opc) {
    case TargetOpcode::G_TRUNC:
    case TargetOpcode::G_SEXT:
    case TargetOpcode::G_ZEXT:
    case TargetOpcode::G_ANYEXT:
      return true;
    default:
      return false;
    }
  }

public:
  LegalizationArtifactCombiner(MachineIRBuilder &B, MachineRegisterInfo &MRI,
                    const LegalizerInfo &LI)
      : Builder(B), MRI(MRI), LI(LI) {}

  bool tryCombineAnyExt(MachineInstr &MI,
                        SmallVectorImpl<MachineInstr *> &DeadInsts) {
    if (MI.getOpcode() != TargetOpcode::G_ANYEXT)
      return false;

    Builder.setInstr(MI);
    Register DstReg = MI.getOperand(0).getReg();
    Register SrcReg = lookThroughCopyInstrs(MI.getOperand(1).getReg());

    // aext(trunc x) - > aext/copy/trunc x
    Register TruncSrc;
    if (mi_match(SrcReg, MRI, m_GTrunc(m_Reg(TruncSrc)))) {
      LLVM_DEBUG(dbgs() << ".. Combine MI: " << MI;);
      Builder.buildAnyExtOrTrunc(DstReg, TruncSrc);
      markInstAndDefDead(MI, *MRI.getVRegDef(SrcReg), DeadInsts);
      return true;
    }

    // aext([asz]ext x) -> [asz]ext x
    Register ExtSrc;
    MachineInstr *ExtMI;
    if (mi_match(SrcReg, MRI,
                 m_all_of(m_MInstr(ExtMI), m_any_of(m_GAnyExt(m_Reg(ExtSrc)),
                                                    m_GSExt(m_Reg(ExtSrc)),
                                                    m_GZExt(m_Reg(ExtSrc)))))) {
      Builder.buildInstr(ExtMI->getOpcode(), {DstReg}, {ExtSrc});
      markInstAndDefDead(MI, *ExtMI, DeadInsts);
      return true;
    }

    // Try to fold aext(g_constant) when the larger constant type is legal.
    // Can't use MIPattern because we don't have a specific constant in mind.
    auto *SrcMI = MRI.getVRegDef(SrcReg);
    if (SrcMI->getOpcode() == TargetOpcode::G_CONSTANT) {
      const LLT &DstTy = MRI.getType(DstReg);
      if (isInstLegal({TargetOpcode::G_CONSTANT, {DstTy}})) {
        auto &CstVal = SrcMI->getOperand(1);
        Builder.buildConstant(
            DstReg, CstVal.getCImm()->getValue().sext(DstTy.getSizeInBits()));
        markInstAndDefDead(MI, *SrcMI, DeadInsts);
        return true;
      }
    }
    return tryFoldImplicitDef(MI, DeadInsts);
  }

  bool tryCombineZExt(MachineInstr &MI,
                      SmallVectorImpl<MachineInstr *> &DeadInsts) {

    if (MI.getOpcode() != TargetOpcode::G_ZEXT)
      return false;

    Builder.setInstr(MI);
    Register DstReg = MI.getOperand(0).getReg();
    Register SrcReg = lookThroughCopyInstrs(MI.getOperand(1).getReg());

    // zext(trunc x) - > and (aext/copy/trunc x), mask
    Register TruncSrc;
    if (mi_match(SrcReg, MRI, m_GTrunc(m_Reg(TruncSrc)))) {
      LLT DstTy = MRI.getType(DstReg);
      if (isInstUnsupported({TargetOpcode::G_AND, {DstTy}}) ||
          isConstantUnsupported(DstTy))
        return false;
      LLVM_DEBUG(dbgs() << ".. Combine MI: " << MI;);
      LLT SrcTy = MRI.getType(SrcReg);
      APInt Mask = APInt::getAllOnesValue(SrcTy.getScalarSizeInBits());
      auto MIBMask = Builder.buildConstant(DstTy, Mask.getZExtValue());
      Builder.buildAnd(DstReg, Builder.buildAnyExtOrTrunc(DstTy, TruncSrc),
                       MIBMask);
      markInstAndDefDead(MI, *MRI.getVRegDef(SrcReg), DeadInsts);
      return true;
    }

    // Try to fold zext(g_constant) when the larger constant type is legal.
    // Can't use MIPattern because we don't have a specific constant in mind.
    auto *SrcMI = MRI.getVRegDef(SrcReg);
    if (SrcMI->getOpcode() == TargetOpcode::G_CONSTANT) {
      const LLT &DstTy = MRI.getType(DstReg);
      if (isInstLegal({TargetOpcode::G_CONSTANT, {DstTy}})) {
        auto &CstVal = SrcMI->getOperand(1);
        Builder.buildConstant(
            DstReg, CstVal.getCImm()->getValue().zext(DstTy.getSizeInBits()));
        markInstAndDefDead(MI, *SrcMI, DeadInsts);
        return true;
      }
    }
    return tryFoldImplicitDef(MI, DeadInsts);
  }

  bool tryCombineSExt(MachineInstr &MI,
                      SmallVectorImpl<MachineInstr *> &DeadInsts) {

    if (MI.getOpcode() != TargetOpcode::G_SEXT)
      return false;

    Builder.setInstr(MI);
    Register DstReg = MI.getOperand(0).getReg();
    Register SrcReg = lookThroughCopyInstrs(MI.getOperand(1).getReg());

    // sext(trunc x) - > ashr (shl (aext/copy/trunc x), c), c
    Register TruncSrc;
    if (mi_match(SrcReg, MRI, m_GTrunc(m_Reg(TruncSrc)))) {
      LLT DstTy = MRI.getType(DstReg);
      // Guess on the RHS shift amount type, which should be re-legalized if
      // applicable.
      if (isInstUnsupported({TargetOpcode::G_SHL, {DstTy, DstTy}}) ||
          isInstUnsupported({TargetOpcode::G_ASHR, {DstTy, DstTy}}) ||
          isConstantUnsupported(DstTy))
        return false;
      LLVM_DEBUG(dbgs() << ".. Combine MI: " << MI;);
      LLT SrcTy = MRI.getType(SrcReg);
      unsigned ShAmt = DstTy.getScalarSizeInBits() - SrcTy.getScalarSizeInBits();
      auto MIBShAmt = Builder.buildConstant(DstTy, ShAmt);
      auto MIBShl = Builder.buildInstr(
          TargetOpcode::G_SHL, {DstTy},
          {Builder.buildAnyExtOrTrunc(DstTy, TruncSrc), MIBShAmt});
      Builder.buildInstr(TargetOpcode::G_ASHR, {DstReg}, {MIBShl, MIBShAmt});
      markInstAndDefDead(MI, *MRI.getVRegDef(SrcReg), DeadInsts);
      return true;
    }
    return tryFoldImplicitDef(MI, DeadInsts);
  }

  /// Try to fold G_[ASZ]EXT (G_IMPLICIT_DEF).
  bool tryFoldImplicitDef(MachineInstr &MI,
                          SmallVectorImpl<MachineInstr *> &DeadInsts) {
    unsigned Opcode = MI.getOpcode();
    if (Opcode != TargetOpcode::G_ANYEXT && Opcode != TargetOpcode::G_ZEXT &&
        Opcode != TargetOpcode::G_SEXT)
      return false;

    if (MachineInstr *DefMI = getOpcodeDef(TargetOpcode::G_IMPLICIT_DEF,
                                           MI.getOperand(1).getReg(), MRI)) {
      Builder.setInstr(MI);
      Register DstReg = MI.getOperand(0).getReg();
      LLT DstTy = MRI.getType(DstReg);

      if (Opcode == TargetOpcode::G_ANYEXT) {
        // G_ANYEXT (G_IMPLICIT_DEF) -> G_IMPLICIT_DEF
        if (isInstUnsupported({TargetOpcode::G_IMPLICIT_DEF, {DstTy}}))
          return false;
        LLVM_DEBUG(dbgs() << ".. Combine G_ANYEXT(G_IMPLICIT_DEF): " << MI;);
        Builder.buildInstr(TargetOpcode::G_IMPLICIT_DEF, {DstReg}, {});
      } else {
        // G_[SZ]EXT (G_IMPLICIT_DEF) -> G_CONSTANT 0 because the top
        // bits will be 0 for G_ZEXT and 0/1 for the G_SEXT.
        if (isConstantUnsupported(DstTy))
          return false;
        LLVM_DEBUG(dbgs() << ".. Combine G_[SZ]EXT(G_IMPLICIT_DEF): " << MI;);
        Builder.buildConstant(DstReg, 0);
      }

      markInstAndDefDead(MI, *DefMI, DeadInsts);
      return true;
    }
    return false;
  }

  static unsigned getMergeOpcode(LLT OpTy, LLT DestTy) {
    if (OpTy.isVector() && DestTy.isVector())
      return TargetOpcode::G_CONCAT_VECTORS;

    if (OpTy.isVector() && !DestTy.isVector())
      return TargetOpcode::G_BUILD_VECTOR;

    return TargetOpcode::G_MERGE_VALUES;
  }

  bool tryCombineMerges(MachineInstr &MI,
                        SmallVectorImpl<MachineInstr *> &DeadInsts) {

    if (MI.getOpcode() != TargetOpcode::G_UNMERGE_VALUES)
      return false;

    unsigned NumDefs = MI.getNumOperands() - 1;
    MachineInstr *SrcDef =
        getDefIgnoringCopies(MI.getOperand(NumDefs).getReg(), MRI);
    if (!SrcDef)
      return false;

    LLT OpTy = MRI.getType(MI.getOperand(NumDefs).getReg());
    LLT DestTy = MRI.getType(MI.getOperand(0).getReg());
    MachineInstr *MergeI = SrcDef;
    unsigned ConvertOp = 0;

    // Handle intermediate conversions
    unsigned SrcOp = SrcDef->getOpcode();
    if (isArtifactCast(SrcOp)) {
      ConvertOp = SrcOp;
      MergeI = getDefIgnoringCopies(SrcDef->getOperand(1).getReg(), MRI);
    }

    // FIXME: Handle scalarizing concat_vectors (scalar result type with vector
    // source)
    unsigned MergingOpcode = getMergeOpcode(OpTy, DestTy);
    if (!MergeI || MergeI->getOpcode() != MergingOpcode)
      return false;

    const unsigned NumMergeRegs = MergeI->getNumOperands() - 1;

    if (NumMergeRegs < NumDefs) {
      if (ConvertOp != 0 || NumDefs % NumMergeRegs != 0)
        return false;

      Builder.setInstr(MI);
      // Transform to UNMERGEs, for example
      //   %1 = G_MERGE_VALUES %4, %5
      //   %9, %10, %11, %12 = G_UNMERGE_VALUES %1
      // to
      //   %9, %10 = G_UNMERGE_VALUES %4
      //   %11, %12 = G_UNMERGE_VALUES %5

      const unsigned NewNumDefs = NumDefs / NumMergeRegs;
      for (unsigned Idx = 0; Idx < NumMergeRegs; ++Idx) {
        SmallVector<Register, 2> DstRegs;
        for (unsigned j = 0, DefIdx = Idx * NewNumDefs; j < NewNumDefs;
             ++j, ++DefIdx)
          DstRegs.push_back(MI.getOperand(DefIdx).getReg());

        Builder.buildUnmerge(DstRegs, MergeI->getOperand(Idx + 1).getReg());
      }

    } else if (NumMergeRegs > NumDefs) {
      if (ConvertOp != 0 || NumMergeRegs % NumDefs != 0)
        return false;

      Builder.setInstr(MI);
      // Transform to MERGEs
      //   %6 = G_MERGE_VALUES %17, %18, %19, %20
      //   %7, %8 = G_UNMERGE_VALUES %6
      // to
      //   %7 = G_MERGE_VALUES %17, %18
      //   %8 = G_MERGE_VALUES %19, %20

      const unsigned NumRegs = NumMergeRegs / NumDefs;
      for (unsigned DefIdx = 0; DefIdx < NumDefs; ++DefIdx) {
        SmallVector<Register, 2> Regs;
        for (unsigned j = 0, Idx = NumRegs * DefIdx + 1; j < NumRegs;
             ++j, ++Idx)
          Regs.push_back(MergeI->getOperand(Idx).getReg());

        Builder.buildMerge(MI.getOperand(DefIdx).getReg(), Regs);
      }

    } else {
      LLT MergeSrcTy = MRI.getType(MergeI->getOperand(1).getReg());
      if (ConvertOp) {
        Builder.setInstr(MI);

        for (unsigned Idx = 0; Idx < NumDefs; ++Idx) {
          Register MergeSrc = MergeI->getOperand(Idx + 1).getReg();
          Builder.buildInstr(ConvertOp, {MI.getOperand(Idx).getReg()},
                             {MergeSrc});
        }

        markInstAndDefDead(MI, *MergeI, DeadInsts);
        return true;
      }
      // FIXME: is a COPY appropriate if the types mismatch? We know both
      // registers are allocatable by now.
      if (DestTy != MergeSrcTy)
        return false;

      for (unsigned Idx = 0; Idx < NumDefs; ++Idx)
        MRI.replaceRegWith(MI.getOperand(Idx).getReg(),
                           MergeI->getOperand(Idx + 1).getReg());
    }

    markInstAndDefDead(MI, *MergeI, DeadInsts);
    return true;
  }

  static bool isMergeLikeOpcode(unsigned Opc) {
    switch (Opc) {
    case TargetOpcode::G_MERGE_VALUES:
    case TargetOpcode::G_BUILD_VECTOR:
    case TargetOpcode::G_CONCAT_VECTORS:
      return true;
    default:
      return false;
    }
  }

  bool tryCombineExtract(MachineInstr &MI,
                         SmallVectorImpl<MachineInstr *> &DeadInsts) {
    assert(MI.getOpcode() == TargetOpcode::G_EXTRACT);

    // Try to use the source registers from a G_MERGE_VALUES
    //
    // %2 = G_MERGE_VALUES %0, %1
    // %3 = G_EXTRACT %2, N
    // =>
    //
    // for N < %2.getSizeInBits() / 2
    //     %3 = G_EXTRACT %0, N
    //
    // for N >= %2.getSizeInBits() / 2
    //    %3 = G_EXTRACT %1, (N - %0.getSizeInBits()

    unsigned Src = lookThroughCopyInstrs(MI.getOperand(1).getReg());
    MachineInstr *MergeI = MRI.getVRegDef(Src);
    if (!MergeI || !isMergeLikeOpcode(MergeI->getOpcode()))
      return false;

    LLT DstTy = MRI.getType(MI.getOperand(0).getReg());
    LLT SrcTy = MRI.getType(Src);

    // TODO: Do we need to check if the resulting extract is supported?
    unsigned ExtractDstSize = DstTy.getSizeInBits();
    unsigned Offset = MI.getOperand(2).getImm();
    unsigned NumMergeSrcs = MergeI->getNumOperands() - 1;
    unsigned MergeSrcSize = SrcTy.getSizeInBits() / NumMergeSrcs;
    unsigned MergeSrcIdx = Offset / MergeSrcSize;

    // Compute the offset of the last bit the extract needs.
    unsigned EndMergeSrcIdx = (Offset + ExtractDstSize - 1) / MergeSrcSize;

    // Can't handle the case where the extract spans multiple inputs.
    if (MergeSrcIdx != EndMergeSrcIdx)
      return false;

    // TODO: We could modify MI in place in most cases.
    Builder.setInstr(MI);
    Builder.buildExtract(
      MI.getOperand(0).getReg(),
      MergeI->getOperand(MergeSrcIdx + 1).getReg(),
      Offset - MergeSrcIdx * MergeSrcSize);
    markInstAndDefDead(MI, *MergeI, DeadInsts);
    return true;
  }

  /// Try to combine away MI.
  /// Returns true if it combined away the MI.
  /// Adds instructions that are dead as a result of the combine
  /// into DeadInsts, which can include MI.
  bool tryCombineInstruction(MachineInstr &MI,
                             SmallVectorImpl<MachineInstr *> &DeadInsts,
                             GISelObserverWrapper &WrapperObserver) {
    // This might be a recursive call, and we might have DeadInsts already
    // populated. To avoid bad things happening later with multiple vreg defs
    // etc, process the dead instructions now if any.
    if (!DeadInsts.empty())
      deleteMarkedDeadInsts(DeadInsts, WrapperObserver);
    switch (MI.getOpcode()) {
    default:
      return false;
    case TargetOpcode::G_ANYEXT:
      return tryCombineAnyExt(MI, DeadInsts);
    case TargetOpcode::G_ZEXT:
      return tryCombineZExt(MI, DeadInsts);
    case TargetOpcode::G_SEXT:
      return tryCombineSExt(MI, DeadInsts);
    case TargetOpcode::G_UNMERGE_VALUES:
      return tryCombineMerges(MI, DeadInsts);
    case TargetOpcode::G_EXTRACT:
      return tryCombineExtract(MI, DeadInsts);
    case TargetOpcode::G_TRUNC: {
      bool Changed = false;
      for (auto &Use : MRI.use_instructions(MI.getOperand(0).getReg()))
        Changed |= tryCombineInstruction(Use, DeadInsts, WrapperObserver);
      return Changed;
    }
    }
  }

private:

  static unsigned getArtifactSrcReg(const MachineInstr &MI) {
    switch (MI.getOpcode()) {
    case TargetOpcode::COPY:
    case TargetOpcode::G_TRUNC:
    case TargetOpcode::G_ZEXT:
    case TargetOpcode::G_ANYEXT:
    case TargetOpcode::G_SEXT:
    case TargetOpcode::G_UNMERGE_VALUES:
      return MI.getOperand(MI.getNumOperands() - 1).getReg();
    case TargetOpcode::G_EXTRACT:
      return MI.getOperand(1).getReg();
    default:
      llvm_unreachable("Not a legalization artifact happen");
    }
  }

  /// Mark MI as dead. If a def of one of MI's operands, DefMI, would also be
  /// dead due to MI being killed, then mark DefMI as dead too.
  /// Some of the combines (extends(trunc)), try to walk through redundant
  /// copies in between the extends and the truncs, and this attempts to collect
  /// the in between copies if they're dead.
  void markInstAndDefDead(MachineInstr &MI, MachineInstr &DefMI,
                          SmallVectorImpl<MachineInstr *> &DeadInsts) {
    DeadInsts.push_back(&MI);

    // Collect all the copy instructions that are made dead, due to deleting
    // this instruction. Collect all of them until the Trunc(DefMI).
    // Eg,
    // %1(s1) = G_TRUNC %0(s32)
    // %2(s1) = COPY %1(s1)
    // %3(s1) = COPY %2(s1)
    // %4(s32) = G_ANYEXT %3(s1)
    // In this case, we would have replaced %4 with a copy of %0,
    // and as a result, %3, %2, %1 are dead.
    MachineInstr *PrevMI = &MI;
    while (PrevMI != &DefMI) {
      unsigned PrevRegSrc = getArtifactSrcReg(*PrevMI);

      MachineInstr *TmpDef = MRI.getVRegDef(PrevRegSrc);
      if (MRI.hasOneUse(PrevRegSrc)) {
        if (TmpDef != &DefMI) {
          assert((TmpDef->getOpcode() == TargetOpcode::COPY ||
                  isArtifactCast(TmpDef->getOpcode())) &&
                 "Expecting copy or artifact cast here");

          DeadInsts.push_back(TmpDef);
        }
      } else
        break;
      PrevMI = TmpDef;
    }
    if (PrevMI == &DefMI && MRI.hasOneUse(DefMI.getOperand(0).getReg()))
      DeadInsts.push_back(&DefMI);
  }

  /// Erase the dead instructions in the list and call the observer hooks.
  /// Normally the Legalizer will deal with erasing instructions that have been
  /// marked dead. However, for the trunc(ext(x)) cases we can end up trying to
  /// process instructions which have been marked dead, but otherwise break the
  /// MIR by introducing multiple vreg defs. For those cases, allow the combines
  /// to explicitly delete the instructions before we run into trouble.
  void deleteMarkedDeadInsts(SmallVectorImpl<MachineInstr *> &DeadInsts,
                             GISelObserverWrapper &WrapperObserver) {
    for (auto *DeadMI : DeadInsts) {
      LLVM_DEBUG(dbgs() << *DeadMI << "Is dead, eagerly deleting\n");
      WrapperObserver.erasingInstr(*DeadMI);
      DeadMI->eraseFromParentAndMarkDBGValuesForRemoval();
    }
    DeadInsts.clear();
  }

  /// Checks if the target legalizer info has specified anything about the
  /// instruction, or if unsupported.
  bool isInstUnsupported(const LegalityQuery &Query) const {
    using namespace LegalizeActions;
    auto Step = LI.getAction(Query);
    return Step.Action == Unsupported || Step.Action == NotFound;
  }

  bool isInstLegal(const LegalityQuery &Query) const {
    return LI.getAction(Query).Action == LegalizeActions::Legal;
  }

  bool isConstantUnsupported(LLT Ty) const {
    if (!Ty.isVector())
      return isInstUnsupported({TargetOpcode::G_CONSTANT, {Ty}});

    LLT EltTy = Ty.getElementType();
    return isInstUnsupported({TargetOpcode::G_CONSTANT, {EltTy}}) ||
           isInstUnsupported({TargetOpcode::G_BUILD_VECTOR, {Ty, EltTy}});
  }

  /// Looks through copy instructions and returns the actual
  /// source register.
  unsigned lookThroughCopyInstrs(Register Reg) {
    Register TmpReg;
    while (mi_match(Reg, MRI, m_Copy(m_Reg(TmpReg)))) {
      if (MRI.getType(TmpReg).isValid())
        Reg = TmpReg;
      else
        break;
    }
    return Reg;
  }
};

} // namespace llvm
