//===-- SIShrinkInstructions.cpp - Shrink Instructions --------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
/// The pass tries to use the 32-bit encoding for instructions when possible.
//===----------------------------------------------------------------------===//
//

#include "AMDGPU.h"
#include "AMDGPUMCInstLower.h"
#include "AMDGPUSubtarget.h"
#include "SIInstrInfo.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Target/TargetMachine.h"

#define DEBUG_TYPE "si-shrink-instructions"

STATISTIC(NumInstructionsShrunk,
          "Number of 64-bit instruction reduced to 32-bit.");
STATISTIC(NumLiteralConstantsFolded,
          "Number of literal constants folded into 32-bit instructions.");

using namespace llvm;

namespace {

class SIShrinkInstructions : public MachineFunctionPass {
public:
  static char ID;

public:
  SIShrinkInstructions() : MachineFunctionPass(ID) {
  }

  bool runOnMachineFunction(MachineFunction &MF) override;

  StringRef getPassName() const override { return "SI Shrink Instructions"; }

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.setPreservesCFG();
    MachineFunctionPass::getAnalysisUsage(AU);
  }
};

} // End anonymous namespace.

INITIALIZE_PASS(SIShrinkInstructions, DEBUG_TYPE,
                "SI Shrink Instructions", false, false)

char SIShrinkInstructions::ID = 0;

FunctionPass *llvm::createSIShrinkInstructionsPass() {
  return new SIShrinkInstructions();
}

static bool isVGPR(const MachineOperand *MO, const SIRegisterInfo &TRI,
                   const MachineRegisterInfo &MRI) {
  if (!MO->isReg())
    return false;

  if (TargetRegisterInfo::isVirtualRegister(MO->getReg()))
    return TRI.hasVGPRs(MRI.getRegClass(MO->getReg()));

  return TRI.hasVGPRs(TRI.getPhysRegClass(MO->getReg()));
}

static bool canShrink(MachineInstr &MI, const SIInstrInfo *TII,
                      const SIRegisterInfo &TRI,
                      const MachineRegisterInfo &MRI) {

  const MachineOperand *Src2 = TII->getNamedOperand(MI, AMDGPU::OpName::src2);
  // Can't shrink instruction with three operands.
  // FIXME: v_cndmask_b32 has 3 operands and is shrinkable, but we need to add
  // a special case for it.  It can only be shrunk if the third operand
  // is vcc.  We should handle this the same way we handle vopc, by addding
  // a register allocation hint pre-regalloc and then do the shrinking
  // post-regalloc.
  if (Src2) {
    switch (MI.getOpcode()) {
      default: return false;

      case AMDGPU::V_ADDC_U32_e64:
      case AMDGPU::V_SUBB_U32_e64:
        if (TII->getNamedOperand(MI, AMDGPU::OpName::src1)->isImm())
          return false;
        // Additional verification is needed for sdst/src2.
        return true;

      case AMDGPU::V_MAC_F32_e64:
      case AMDGPU::V_MAC_F16_e64:
        if (!isVGPR(Src2, TRI, MRI) ||
            TII->hasModifiersSet(MI, AMDGPU::OpName::src2_modifiers))
          return false;
        break;

      case AMDGPU::V_CNDMASK_B32_e64:
        break;
    }
  }

  const MachineOperand *Src1 = TII->getNamedOperand(MI, AMDGPU::OpName::src1);
  if (Src1 && (!isVGPR(Src1, TRI, MRI) ||
               TII->hasModifiersSet(MI, AMDGPU::OpName::src1_modifiers)))
    return false;

  // We don't need to check src0, all input types are legal, so just make sure
  // src0 isn't using any modifiers.
  if (TII->hasModifiersSet(MI, AMDGPU::OpName::src0_modifiers))
    return false;

  // Check output modifiers
  return !TII->hasModifiersSet(MI, AMDGPU::OpName::omod) &&
         !TII->hasModifiersSet(MI, AMDGPU::OpName::clamp);
}

/// \brief This function checks \p MI for operands defined by a move immediate
/// instruction and then folds the literal constant into the instruction if it
/// can. This function assumes that \p MI is a VOP1, VOP2, or VOPC instructions.
static bool foldImmediates(MachineInstr &MI, const SIInstrInfo *TII,
                           MachineRegisterInfo &MRI, bool TryToCommute = true) {
  assert(TII->isVOP1(MI) || TII->isVOP2(MI) || TII->isVOPC(MI));

  int Src0Idx = AMDGPU::getNamedOperandIdx(MI.getOpcode(), AMDGPU::OpName::src0);

  // Try to fold Src0
  MachineOperand &Src0 = MI.getOperand(Src0Idx);
  if (Src0.isReg()) {
    unsigned Reg = Src0.getReg();
    if (TargetRegisterInfo::isVirtualRegister(Reg) && MRI.hasOneUse(Reg)) {
      MachineInstr *Def = MRI.getUniqueVRegDef(Reg);
      if (Def && Def->isMoveImmediate()) {
        MachineOperand &MovSrc = Def->getOperand(1);
        bool ConstantFolded = false;

        if (MovSrc.isImm() && (isInt<32>(MovSrc.getImm()) ||
                               isUInt<32>(MovSrc.getImm()))) {
          // It's possible to have only one component of a super-reg defined by
          // a single mov, so we need to clear any subregister flag.
          Src0.setSubReg(0);
          Src0.ChangeToImmediate(MovSrc.getImm());
          ConstantFolded = true;
        } else if (MovSrc.isFI()) {
          Src0.setSubReg(0);
          Src0.ChangeToFrameIndex(MovSrc.getIndex());
          ConstantFolded = true;
        }

        if (ConstantFolded) {
          assert(MRI.use_empty(Reg));
          Def->eraseFromParent();
          ++NumLiteralConstantsFolded;
          return true;
        }
      }
    }
  }

  // We have failed to fold src0, so commute the instruction and try again.
  if (TryToCommute && MI.isCommutable()) {
    if (TII->commuteInstruction(MI)) {
      if (foldImmediates(MI, TII, MRI, false))
        return true;

      // Commute back.
      TII->commuteInstruction(MI);
    }
  }

  return false;
}

// Copy MachineOperand with all flags except setting it as implicit.
static void copyFlagsToImplicitVCC(MachineInstr &MI,
                                   const MachineOperand &Orig) {

  for (MachineOperand &Use : MI.implicit_operands()) {
    if (Use.isUse() && Use.getReg() == AMDGPU::VCC) {
      Use.setIsUndef(Orig.isUndef());
      Use.setIsKill(Orig.isKill());
      return;
    }
  }
}

static bool isKImmOperand(const SIInstrInfo *TII, const MachineOperand &Src) {
  return isInt<16>(Src.getImm()) &&
    !TII->isInlineConstant(*Src.getParent(),
                           Src.getParent()->getOperandNo(&Src));
}

static bool isKUImmOperand(const SIInstrInfo *TII, const MachineOperand &Src) {
  return isUInt<16>(Src.getImm()) &&
    !TII->isInlineConstant(*Src.getParent(),
                           Src.getParent()->getOperandNo(&Src));
}

static bool isKImmOrKUImmOperand(const SIInstrInfo *TII,
                                 const MachineOperand &Src,
                                 bool &IsUnsigned) {
  if (isInt<16>(Src.getImm())) {
    IsUnsigned = false;
    return !TII->isInlineConstant(Src);
  }

  if (isUInt<16>(Src.getImm())) {
    IsUnsigned = true;
    return !TII->isInlineConstant(Src);
  }

  return false;
}

/// \returns true if the constant in \p Src should be replaced with a bitreverse
/// of an inline immediate.
static bool isReverseInlineImm(const SIInstrInfo *TII,
                               const MachineOperand &Src,
                               int32_t &ReverseImm) {
  if (!isInt<32>(Src.getImm()) || TII->isInlineConstant(Src))
    return false;

  ReverseImm = reverseBits<int32_t>(static_cast<int32_t>(Src.getImm()));
  return ReverseImm >= -16 && ReverseImm <= 64;
}

/// Copy implicit register operands from specified instruction to this
/// instruction that are not part of the instruction definition.
static void copyExtraImplicitOps(MachineInstr &NewMI, MachineFunction &MF,
                                 const MachineInstr &MI) {
  for (unsigned i = MI.getDesc().getNumOperands() +
         MI.getDesc().getNumImplicitUses() +
         MI.getDesc().getNumImplicitDefs(), e = MI.getNumOperands();
       i != e; ++i) {
    const MachineOperand &MO = MI.getOperand(i);
    if ((MO.isReg() && MO.isImplicit()) || MO.isRegMask())
      NewMI.addOperand(MF, MO);
  }
}

static void shrinkScalarCompare(const SIInstrInfo *TII, MachineInstr &MI) {
  // cmpk instructions do scc = dst <cc op> imm16, so commute the instruction to
  // get constants on the RHS.
  if (!MI.getOperand(0).isReg())
    TII->commuteInstruction(MI, false, 0, 1);

  const MachineOperand &Src1 = MI.getOperand(1);
  if (!Src1.isImm())
    return;

  int SOPKOpc = AMDGPU::getSOPKOp(MI.getOpcode());
  if (SOPKOpc == -1)
    return;

  // eq/ne is special because the imm16 can be treated as signed or unsigned,
  // and initially selectd to the unsigned versions.
  if (SOPKOpc == AMDGPU::S_CMPK_EQ_U32 || SOPKOpc == AMDGPU::S_CMPK_LG_U32) {
    bool HasUImm;
    if (isKImmOrKUImmOperand(TII, Src1, HasUImm)) {
      if (!HasUImm) {
        SOPKOpc = (SOPKOpc == AMDGPU::S_CMPK_EQ_U32) ?
          AMDGPU::S_CMPK_EQ_I32 : AMDGPU::S_CMPK_LG_I32;
      }

      MI.setDesc(TII->get(SOPKOpc));
    }

    return;
  }

  const MCInstrDesc &NewDesc = TII->get(SOPKOpc);

  if ((TII->sopkIsZext(SOPKOpc) && isKUImmOperand(TII, Src1)) ||
      (!TII->sopkIsZext(SOPKOpc) && isKImmOperand(TII, Src1))) {
    MI.setDesc(NewDesc);
  }
}

bool SIShrinkInstructions::runOnMachineFunction(MachineFunction &MF) {
  if (skipFunction(*MF.getFunction()))
    return false;

  MachineRegisterInfo &MRI = MF.getRegInfo();
  const SISubtarget &ST = MF.getSubtarget<SISubtarget>();
  const SIInstrInfo *TII = ST.getInstrInfo();
  const SIRegisterInfo &TRI = TII->getRegisterInfo();

  std::vector<unsigned> I1Defs;

  for (MachineFunction::iterator BI = MF.begin(), BE = MF.end();
                                                  BI != BE; ++BI) {

    MachineBasicBlock &MBB = *BI;
    MachineBasicBlock::iterator I, Next;
    for (I = MBB.begin(); I != MBB.end(); I = Next) {
      Next = std::next(I);
      MachineInstr &MI = *I;

      if (MI.getOpcode() == AMDGPU::V_MOV_B32_e32) {
        // If this has a literal constant source that is the same as the
        // reversed bits of an inline immediate, replace with a bitreverse of
        // that constant. This saves 4 bytes in the common case of materializing
        // sign bits.

        // Test if we are after regalloc. We only want to do this after any
        // optimizations happen because this will confuse them.
        // XXX - not exactly a check for post-regalloc run.
        MachineOperand &Src = MI.getOperand(1);
        if (Src.isImm() &&
            TargetRegisterInfo::isPhysicalRegister(MI.getOperand(0).getReg())) {
          int32_t ReverseImm;
          if (isReverseInlineImm(TII, Src, ReverseImm)) {
            MI.setDesc(TII->get(AMDGPU::V_BFREV_B32_e32));
            Src.setImm(ReverseImm);
            continue;
          }
        }
      }

      // Combine adjacent s_nops to use the immediate operand encoding how long
      // to wait.
      //
      // s_nop N
      // s_nop M
      //  =>
      // s_nop (N + M)
      if (MI.getOpcode() == AMDGPU::S_NOP &&
          Next != MBB.end() &&
          (*Next).getOpcode() == AMDGPU::S_NOP) {

        MachineInstr &NextMI = *Next;
        // The instruction encodes the amount to wait with an offset of 1,
        // i.e. 0 is wait 1 cycle. Convert both to cycles and then convert back
        // after adding.
        uint8_t Nop0 = MI.getOperand(0).getImm() + 1;
        uint8_t Nop1 = NextMI.getOperand(0).getImm() + 1;

        // Make sure we don't overflow the bounds.
        if (Nop0 + Nop1 <= 8) {
          NextMI.getOperand(0).setImm(Nop0 + Nop1 - 1);
          MI.eraseFromParent();
        }

        continue;
      }

      // FIXME: We also need to consider movs of constant operands since
      // immediate operands are not folded if they have more than one use, and
      // the operand folding pass is unaware if the immediate will be free since
      // it won't know if the src == dest constraint will end up being
      // satisfied.
      if (MI.getOpcode() == AMDGPU::S_ADD_I32 ||
          MI.getOpcode() == AMDGPU::S_MUL_I32) {
        const MachineOperand *Dest = &MI.getOperand(0);
        MachineOperand *Src0 = &MI.getOperand(1);
        MachineOperand *Src1 = &MI.getOperand(2);

        if (!Src0->isReg() && Src1->isReg()) {
          if (TII->commuteInstruction(MI, false, 1, 2))
            std::swap(Src0, Src1);
        }

        // FIXME: This could work better if hints worked with subregisters. If
        // we have a vector add of a constant, we usually don't get the correct
        // allocation due to the subregister usage.
        if (TargetRegisterInfo::isVirtualRegister(Dest->getReg()) &&
            Src0->isReg()) {
          MRI.setRegAllocationHint(Dest->getReg(), 0, Src0->getReg());
          MRI.setRegAllocationHint(Src0->getReg(), 0, Dest->getReg());
          continue;
        }

        if (Src0->isReg() && Src0->getReg() == Dest->getReg()) {
          if (Src1->isImm() && isKImmOperand(TII, *Src1)) {
            unsigned Opc = (MI.getOpcode() == AMDGPU::S_ADD_I32) ?
              AMDGPU::S_ADDK_I32 : AMDGPU::S_MULK_I32;

            MI.setDesc(TII->get(Opc));
            MI.tieOperands(0, 1);
          }
        }
      }

      // Try to use s_cmpk_*
      if (MI.isCompare() && TII->isSOPC(MI)) {
        shrinkScalarCompare(TII, MI);
        continue;
      }

      // Try to use S_MOVK_I32, which will save 4 bytes for small immediates.
      if (MI.getOpcode() == AMDGPU::S_MOV_B32) {
        const MachineOperand &Dst = MI.getOperand(0);
        MachineOperand &Src = MI.getOperand(1);

        if (Src.isImm() &&
            TargetRegisterInfo::isPhysicalRegister(Dst.getReg())) {
          int32_t ReverseImm;
          if (isKImmOperand(TII, Src))
            MI.setDesc(TII->get(AMDGPU::S_MOVK_I32));
          else if (isReverseInlineImm(TII, Src, ReverseImm)) {
            MI.setDesc(TII->get(AMDGPU::S_BREV_B32));
            Src.setImm(ReverseImm);
          }
        }

        continue;
      }

      if (!TII->hasVALU32BitEncoding(MI.getOpcode()))
        continue;

      if (!canShrink(MI, TII, TRI, MRI)) {
        // Try commuting the instruction and see if that enables us to shrink
        // it.
        if (!MI.isCommutable() || !TII->commuteInstruction(MI) ||
            !canShrink(MI, TII, TRI, MRI))
          continue;
      }

      // getVOPe32 could be -1 here if we started with an instruction that had
      // a 32-bit encoding and then commuted it to an instruction that did not.
      if (!TII->hasVALU32BitEncoding(MI.getOpcode()))
        continue;

      int Op32 = AMDGPU::getVOPe32(MI.getOpcode());

      if (TII->isVOPC(Op32)) {
        unsigned DstReg = MI.getOperand(0).getReg();
        if (TargetRegisterInfo::isVirtualRegister(DstReg)) {
          // VOPC instructions can only write to the VCC register. We can't
          // force them to use VCC here, because this is only one register and
          // cannot deal with sequences which would require multiple copies of
          // VCC, e.g. S_AND_B64 (vcc = V_CMP_...), (vcc = V_CMP_...)
          //
          // So, instead of forcing the instruction to write to VCC, we provide
          // a hint to the register allocator to use VCC and then we we will run
          // this pass again after RA and shrink it if it outputs to VCC.
          MRI.setRegAllocationHint(MI.getOperand(0).getReg(), 0, AMDGPU::VCC);
          continue;
        }
        if (DstReg != AMDGPU::VCC)
          continue;
      }

      if (Op32 == AMDGPU::V_CNDMASK_B32_e32) {
        // We shrink V_CNDMASK_B32_e64 using regalloc hints like we do for VOPC
        // instructions.
        const MachineOperand *Src2 =
            TII->getNamedOperand(MI, AMDGPU::OpName::src2);
        if (!Src2->isReg())
          continue;
        unsigned SReg = Src2->getReg();
        if (TargetRegisterInfo::isVirtualRegister(SReg)) {
          MRI.setRegAllocationHint(SReg, 0, AMDGPU::VCC);
          continue;
        }
        if (SReg != AMDGPU::VCC)
          continue;
      }

      // Check for the bool flag output for instructions like V_ADD_I32_e64.
      const MachineOperand *SDst = TII->getNamedOperand(MI,
                                                        AMDGPU::OpName::sdst);

      // Check the carry-in operand for v_addc_u32_e64.
      const MachineOperand *Src2 = TII->getNamedOperand(MI,
                                                        AMDGPU::OpName::src2);

      if (SDst) {
        if (SDst->getReg() != AMDGPU::VCC) {
          if (TargetRegisterInfo::isVirtualRegister(SDst->getReg()))
            MRI.setRegAllocationHint(SDst->getReg(), 0, AMDGPU::VCC);
          continue;
        }

        // All of the instructions with carry outs also have an SGPR input in
        // src2.
        if (Src2 && Src2->getReg() != AMDGPU::VCC) {
          if (TargetRegisterInfo::isVirtualRegister(Src2->getReg()))
            MRI.setRegAllocationHint(Src2->getReg(), 0, AMDGPU::VCC);

          continue;
        }
      }

      // We can shrink this instruction
      DEBUG(dbgs() << "Shrinking " << MI);

      MachineInstrBuilder Inst32 =
          BuildMI(MBB, I, MI.getDebugLoc(), TII->get(Op32));

      // Add the dst operand if the 32-bit encoding also has an explicit $vdst.
      // For VOPC instructions, this is replaced by an implicit def of vcc.
      int Op32DstIdx = AMDGPU::getNamedOperandIdx(Op32, AMDGPU::OpName::vdst);
      if (Op32DstIdx != -1) {
        // dst
        Inst32.add(MI.getOperand(0));
      } else {
        assert(MI.getOperand(0).getReg() == AMDGPU::VCC &&
               "Unexpected case");
      }


      Inst32.add(*TII->getNamedOperand(MI, AMDGPU::OpName::src0));

      const MachineOperand *Src1 =
          TII->getNamedOperand(MI, AMDGPU::OpName::src1);
      if (Src1)
        Inst32.add(*Src1);

      if (Src2) {
        int Op32Src2Idx = AMDGPU::getNamedOperandIdx(Op32, AMDGPU::OpName::src2);
        if (Op32Src2Idx != -1) {
          Inst32.add(*Src2);
        } else {
          // In the case of V_CNDMASK_B32_e32, the explicit operand src2 is
          // replaced with an implicit read of vcc. This was already added
          // during the initial BuildMI, so find it to preserve the flags.
          copyFlagsToImplicitVCC(*Inst32, *Src2);
        }
      }

      ++NumInstructionsShrunk;

      // Copy extra operands not present in the instruction definition.
      copyExtraImplicitOps(*Inst32, MF, MI);

      MI.eraseFromParent();
      foldImmediates(*Inst32, TII, MRI);

      DEBUG(dbgs() << "e32 MI = " << *Inst32 << '\n');


    }
  }
  return false;
}
