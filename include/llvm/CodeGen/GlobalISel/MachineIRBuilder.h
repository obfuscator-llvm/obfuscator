//===-- llvm/CodeGen/GlobalISel/MachineIRBuilder.h - MIBuilder --*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
/// \file
/// This file declares the MachineIRBuilder class.
/// This is a helper class to build MachineInstr.
//===----------------------------------------------------------------------===//

#ifndef LLVM_CODEGEN_GLOBALISEL_MACHINEIRBUILDER_H
#define LLVM_CODEGEN_GLOBALISEL_MACHINEIRBUILDER_H

#include "llvm/CodeGen/GlobalISel/Types.h"

#include "llvm/CodeGen/LowLevelType.h"
#include "llvm/CodeGen/MachineBasicBlock.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DebugLoc.h"

#include <queue>

namespace llvm {

// Forward declarations.
class MachineFunction;
class MachineInstr;
class TargetInstrInfo;

/// Helper class to build MachineInstr.
/// It keeps internally the insertion point and debug location for all
/// the new instructions we want to create.
/// This information can be modify via the related setters.
class MachineIRBuilder {
  /// MachineFunction under construction.
  MachineFunction *MF;
  /// Information used to access the description of the opcodes.
  const TargetInstrInfo *TII;
  /// Information used to verify types are consistent and to create virtual registers.
  MachineRegisterInfo *MRI;
  /// Debug location to be set to any instruction we create.
  DebugLoc DL;

  /// \name Fields describing the insertion point.
  /// @{
  MachineBasicBlock *MBB;
  MachineBasicBlock::iterator II;
  /// @}

  std::function<void(MachineInstr *)> InsertedInstr;

  const TargetInstrInfo &getTII() {
    assert(TII && "TargetInstrInfo is not set");
    return *TII;
  }

  void validateTruncExt(unsigned Dst, unsigned Src, bool IsExtend);
  MachineInstrBuilder buildBinaryOp(unsigned Opcode, unsigned Res, unsigned Op0, unsigned Op1);

  unsigned getDestFromArg(unsigned Reg) { return Reg; }
  unsigned getDestFromArg(LLT Ty) {
    return getMF().getRegInfo().createGenericVirtualRegister(Ty);
  }
  unsigned getDestFromArg(const TargetRegisterClass *RC) {
    return getMF().getRegInfo().createVirtualRegister(RC);
  }

  unsigned getRegFromArg(unsigned Reg) { return Reg; }

  unsigned getRegFromArg(const MachineInstrBuilder &MIB) {
    return MIB->getOperand(0).getReg();
  }

public:
  /// Getter for the function we currently build.
  MachineFunction &getMF() {
    assert(MF && "MachineFunction is not set");
    return *MF;
  }

  /// Getter for the basic block we currently build.
  MachineBasicBlock &getMBB() {
    assert(MBB && "MachineBasicBlock is not set");
    return *MBB;
  }

  /// Current insertion point for new instructions.
  MachineBasicBlock::iterator getInsertPt() {
    return II;
  }

  /// Set the insertion point before the specified position.
  /// \pre MBB must be in getMF().
  /// \pre II must be a valid iterator in MBB.
  void setInsertPt(MachineBasicBlock &MBB, MachineBasicBlock::iterator II);
  /// @}

  /// \name Setters for the insertion point.
  /// @{
  /// Set the MachineFunction where to build instructions.
  void setMF(MachineFunction &);

  /// Set the insertion point to the  end of \p MBB.
  /// \pre \p MBB must be contained by getMF().
  void setMBB(MachineBasicBlock &MBB);

  /// Set the insertion point to before MI.
  /// \pre MI must be in getMF().
  void setInstr(MachineInstr &MI);
  /// @}

  /// \name Control where instructions we create are recorded (typically for
  /// visiting again later during legalization).
  /// @{
  void recordInsertions(std::function<void(MachineInstr *)> InsertedInstr);
  void stopRecordingInsertions();
  /// @}

  /// Set the debug location to \p DL for all the next build instructions.
  void setDebugLoc(const DebugLoc &DL) { this->DL = DL; }

  /// Get the current instruction's debug location.
  DebugLoc getDebugLoc() { return DL; }

  /// Build and insert <empty> = \p Opcode <empty>.
  /// The insertion point is the one set by the last call of either
  /// setBasicBlock or setMI.
  ///
  /// \pre setBasicBlock or setMI must have been called.
  ///
  /// \return a MachineInstrBuilder for the newly created instruction.
  MachineInstrBuilder buildInstr(unsigned Opcode);

  /// DAG like Generic method for building arbitrary instructions as above.
  /// \Opc opcode for the instruction.
  /// \Ty Either LLT/TargetRegisterClass/unsigned types for Dst
  /// \Args Variadic list of uses of types(unsigned/MachineInstrBuilder)
  /// Uses of type MachineInstrBuilder will perform
  /// getOperand(0).getReg() to convert to register.
  template <typename DstTy, typename... UseArgsTy>
  MachineInstrBuilder buildInstr(unsigned Opc, DstTy &&Ty,
                                 UseArgsTy &&... Args) {
    auto MIB = buildInstr(Opc).addDef(getDestFromArg(Ty));
    unsigned It[] = {(getRegFromArg(Args))...};
    for (const auto &i : It)
      MIB.addUse(i);
    return MIB;
  }

  /// Build but don't insert <empty> = \p Opcode <empty>.
  ///
  /// \pre setMF, setBasicBlock or setMI  must have been called.
  ///
  /// \return a MachineInstrBuilder for the newly created instruction.
  MachineInstrBuilder buildInstrNoInsert(unsigned Opcode);

  /// Insert an existing instruction at the insertion point.
  MachineInstrBuilder insertInstr(MachineInstrBuilder MIB);

  /// Build and insert a DBG_VALUE instruction expressing the fact that the
  /// associated \p Variable lives in \p Reg (suitably modified by \p Expr).
  MachineInstrBuilder buildDirectDbgValue(unsigned Reg, const MDNode *Variable,
                                          const MDNode *Expr);

  /// Build and insert a DBG_VALUE instruction expressing the fact that the
  /// associated \p Variable lives in memory at \p Reg + \p Offset (suitably
  /// modified by \p Expr).
  MachineInstrBuilder buildIndirectDbgValue(unsigned Reg, unsigned Offset,
                                            const MDNode *Variable,
                                            const MDNode *Expr);
  /// Build and insert a DBG_VALUE instruction expressing the fact that the
  /// associated \p Variable lives in the stack slot specified by \p FI
  /// (suitably modified by \p Expr).
  MachineInstrBuilder buildFIDbgValue(int FI, const MDNode *Variable,
                                      const MDNode *Expr);

  /// Build and insert a DBG_VALUE instructions specifying that \p Variable is
  /// given by \p C (suitably modified by \p Expr).
  MachineInstrBuilder buildConstDbgValue(const Constant &C, unsigned Offset,
                                         const MDNode *Variable,
                                         const MDNode *Expr);

  /// Build and insert \p Res<def> = G_FRAME_INDEX \p Idx
  ///
  /// G_FRAME_INDEX materializes the address of an alloca value or other
  /// stack-based object.
  ///
  /// \pre setBasicBlock or setMI must have been called.
  /// \pre \p Res must be a generic virtual register with pointer type.
  ///
  /// \return a MachineInstrBuilder for the newly created instruction.
  MachineInstrBuilder buildFrameIndex(unsigned Res, int Idx);

  /// Build and insert \p Res<def> = G_GLOBAL_VALUE \p GV
  ///
  /// G_GLOBAL_VALUE materializes the address of the specified global
  /// into \p Res.
  ///
  /// \pre setBasicBlock or setMI must have been called.
  /// \pre \p Res must be a generic virtual register with pointer type
  ///      in the same address space as \p GV.
  ///
  /// \return a MachineInstrBuilder for the newly created instruction.
  MachineInstrBuilder buildGlobalValue(unsigned Res, const GlobalValue *GV);

  /// Build and insert \p Res<def> = G_ADD \p Op0, \p Op1
  ///
  /// G_ADD sets \p Res to the sum of integer parameters \p Op0 and \p Op1,
  /// truncated to their width.
  ///
  /// \pre setBasicBlock or setMI must have been called.
  /// \pre \p Res, \p Op0 and \p Op1 must be generic virtual registers
  ///      with the same (scalar or vector) type).
  ///
  /// \return a MachineInstrBuilder for the newly created instruction.
  MachineInstrBuilder buildAdd(unsigned Res, unsigned Op0,
                               unsigned Op1);
  template <typename DstTy, typename... UseArgsTy>
  MachineInstrBuilder buildAdd(DstTy &&Ty, UseArgsTy &&... UseArgs) {
    unsigned Res = getDestFromArg(Ty);
    return buildAdd(Res, (getRegFromArg(UseArgs))...);
  }

  /// Build and insert \p Res<def> = G_SUB \p Op0, \p Op1
  ///
  /// G_SUB sets \p Res to the sum of integer parameters \p Op0 and \p Op1,
  /// truncated to their width.
  ///
  /// \pre setBasicBlock or setMI must have been called.
  /// \pre \p Res, \p Op0 and \p Op1 must be generic virtual registers
  ///      with the same (scalar or vector) type).
  ///
  /// \return a MachineInstrBuilder for the newly created instruction.
  MachineInstrBuilder buildSub(unsigned Res, unsigned Op0,
                               unsigned Op1);

  /// Build and insert \p Res<def> = G_MUL \p Op0, \p Op1
  ///
  /// G_MUL sets \p Res to the sum of integer parameters \p Op0 and \p Op1,
  /// truncated to their width.
  ///
  /// \pre setBasicBlock or setMI must have been called.
  /// \pre \p Res, \p Op0 and \p Op1 must be generic virtual registers
  ///      with the same (scalar or vector) type).
  ///
  /// \return a MachineInstrBuilder for the newly created instruction.
  MachineInstrBuilder buildMul(unsigned Res, unsigned Op0,
                               unsigned Op1);

  /// Build and insert \p Res<def> = G_GEP \p Op0, \p Op1
  ///
  /// G_GEP adds \p Op1 bytes to the pointer specified by \p Op0,
  /// storing the resulting pointer in \p Res.
  ///
  /// \pre setBasicBlock or setMI must have been called.
  /// \pre \p Res and \p Op0 must be generic virtual registers with pointer
  ///      type.
  /// \pre \p Op1 must be a generic virtual register with scalar type.
  ///
  /// \return a MachineInstrBuilder for the newly created instruction.
  MachineInstrBuilder buildGEP(unsigned Res, unsigned Op0,
                               unsigned Op1);

  /// Materialize and insert \p Res<def> = G_GEP \p Op0, (G_CONSTANT \p Value)
  ///
  /// G_GEP adds \p Value bytes to the pointer specified by \p Op0,
  /// storing the resulting pointer in \p Res. If \p Value is zero then no
  /// G_GEP or G_CONSTANT will be created and \pre Op0 will be assigned to
  /// \p Res.
  ///
  /// \pre setBasicBlock or setMI must have been called.
  /// \pre \p Op0 must be a generic virtual register with pointer type.
  /// \pre \p ValueTy must be a scalar type.
  /// \pre \p Res must be 0. This is to detect confusion between
  ///      materializeGEP() and buildGEP().
  /// \post \p Res will either be a new generic virtual register of the same
  ///       type as \p Op0 or \p Op0 itself.
  ///
  /// \return a MachineInstrBuilder for the newly created instruction.
  Optional<MachineInstrBuilder> materializeGEP(unsigned &Res, unsigned Op0,
                                               const LLT &ValueTy,
                                               uint64_t Value);

  /// Build and insert \p Res<def> = G_PTR_MASK \p Op0, \p NumBits
  ///
  /// G_PTR_MASK clears the low bits of a pointer operand without destroying its
  /// pointer properties. This has the effect of rounding the address *down* to
  /// a specified alignment in bits.
  ///
  /// \pre setBasicBlock or setMI must have been called.
  /// \pre \p Res and \p Op0 must be generic virtual registers with pointer
  ///      type.
  /// \pre \p NumBits must be an integer representing the number of low bits to
  ///      be cleared in \p Op0.
  ///
  /// \return a MachineInstrBuilder for the newly created instruction.
  MachineInstrBuilder buildPtrMask(unsigned Res, unsigned Op0,
                                   uint32_t NumBits);

  /// Build and insert \p Res<def>, \p CarryOut<def> = G_UADDE \p Op0,
  /// \p Op1, \p CarryIn
  ///
  /// G_UADDE sets \p Res to \p Op0 + \p Op1 + \p CarryIn (truncated to the bit
  /// width) and sets \p CarryOut to 1 if the result overflowed in unsigned
  /// arithmetic.
  ///
  /// \pre setBasicBlock or setMI must have been called.
  /// \pre \p Res, \p Op0 and \p Op1 must be generic virtual registers
  ///      with the same scalar type.
  /// \pre \p CarryOut and \p CarryIn must be generic virtual
  ///      registers with the same scalar type (typically s1)
  ///
  /// \return The newly created instruction.
  MachineInstrBuilder buildUAdde(unsigned Res, unsigned CarryOut, unsigned Op0,
                                 unsigned Op1, unsigned CarryIn);

  /// Build and insert \p Res<def> = G_AND \p Op0, \p Op1
  ///
  /// G_AND sets \p Res to the bitwise and of integer parameters \p Op0 and \p
  /// Op1.
  ///
  /// \pre setBasicBlock or setMI must have been called.
  /// \pre \p Res, \p Op0 and \p Op1 must be generic virtual registers
  ///      with the same (scalar or vector) type).
  ///
  /// \return a MachineInstrBuilder for the newly created instruction.
  MachineInstrBuilder buildAnd(unsigned Res, unsigned Op0,
                               unsigned Op1);

  /// Build and insert \p Res<def> = G_OR \p Op0, \p Op1
  ///
  /// G_OR sets \p Res to the bitwise or of integer parameters \p Op0 and \p
  /// Op1.
  ///
  /// \pre setBasicBlock or setMI must have been called.
  /// \pre \p Res, \p Op0 and \p Op1 must be generic virtual registers
  ///      with the same (scalar or vector) type).
  ///
  /// \return a MachineInstrBuilder for the newly created instruction.
  MachineInstrBuilder buildOr(unsigned Res, unsigned Op0, unsigned Op1);

  /// Build and insert \p Res<def> = G_ANYEXT \p Op0
  ///
  /// G_ANYEXT produces a register of the specified width, with bits 0 to
  /// sizeof(\p Ty) * 8 set to \p Op. The remaining bits are unspecified
  /// (i.e. this is neither zero nor sign-extension). For a vector register,
  /// each element is extended individually.
  ///
  /// \pre setBasicBlock or setMI must have been called.
  /// \pre \p Res must be a generic virtual register with scalar or vector type.
  /// \pre \p Op must be a generic virtual register with scalar or vector type.
  /// \pre \p Op must be smaller than \p Res
  ///
  /// \return The newly created instruction.
  MachineInstrBuilder buildAnyExt(unsigned Res, unsigned Op);

  /// Build and insert \p Res<def> = G_SEXT \p Op
  ///
  /// G_SEXT produces a register of the specified width, with bits 0 to
  /// sizeof(\p Ty) * 8 set to \p Op. The remaining bits are duplicated from the
  /// high bit of \p Op (i.e. 2s-complement sign extended).
  ///
  /// \pre setBasicBlock or setMI must have been called.
  /// \pre \p Res must be a generic virtual register with scalar or vector type.
  /// \pre \p Op must be a generic virtual register with scalar or vector type.
  /// \pre \p Op must be smaller than \p Res
  ///
  /// \return The newly created instruction.
  MachineInstrBuilder buildSExt(unsigned Res, unsigned Op);

  /// Build and insert \p Res<def> = G_ZEXT \p Op
  ///
  /// G_ZEXT produces a register of the specified width, with bits 0 to
  /// sizeof(\p Ty) * 8 set to \p Op. The remaining bits are 0. For a vector
  /// register, each element is extended individually.
  ///
  /// \pre setBasicBlock or setMI must have been called.
  /// \pre \p Res must be a generic virtual register with scalar or vector type.
  /// \pre \p Op must be a generic virtual register with scalar or vector type.
  /// \pre \p Op must be smaller than \p Res
  ///
  /// \return The newly created instruction.
  MachineInstrBuilder buildZExt(unsigned Res, unsigned Op);

  /// Build and insert \p Res<def> = G_SEXT \p Op, \p Res = G_TRUNC \p Op, or
  /// \p Res = COPY \p Op depending on the differing sizes of \p Res and \p Op.
  ///  ///
  /// \pre setBasicBlock or setMI must have been called.
  /// \pre \p Res must be a generic virtual register with scalar or vector type.
  /// \pre \p Op must be a generic virtual register with scalar or vector type.
  ///
  /// \return The newly created instruction.
  MachineInstrBuilder buildSExtOrTrunc(unsigned Res, unsigned Op);

  /// Build and insert \p Res<def> = G_ZEXT \p Op, \p Res = G_TRUNC \p Op, or
  /// \p Res = COPY \p Op depending on the differing sizes of \p Res and \p Op.
  ///  ///
  /// \pre setBasicBlock or setMI must have been called.
  /// \pre \p Res must be a generic virtual register with scalar or vector type.
  /// \pre \p Op must be a generic virtual register with scalar or vector type.
  ///
  /// \return The newly created instruction.
  MachineInstrBuilder buildZExtOrTrunc(unsigned Res, unsigned Op);

  /// Build and insert an appropriate cast between two registers of equal size.
  MachineInstrBuilder buildCast(unsigned Dst, unsigned Src);

  /// Build and insert G_BR \p Dest
  ///
  /// G_BR is an unconditional branch to \p Dest.
  ///
  /// \pre setBasicBlock or setMI must have been called.
  ///
  /// \return a MachineInstrBuilder for the newly created instruction.
  MachineInstrBuilder buildBr(MachineBasicBlock &BB);

  /// Build and insert G_BRCOND \p Tst, \p Dest
  ///
  /// G_BRCOND is a conditional branch to \p Dest.
  ///
  /// \pre setBasicBlock or setMI must have been called.
  /// \pre \p Tst must be a generic virtual register with scalar
  ///      type. At the beginning of legalization, this will be a single
  ///      bit (s1). Targets with interesting flags registers may change
  ///      this. For a wider type, whether the branch is taken must only
  ///      depend on bit 0 (for now).
  ///
  /// \return The newly created instruction.
  MachineInstrBuilder buildBrCond(unsigned Tst, MachineBasicBlock &BB);

  /// Build and insert G_BRINDIRECT \p Tgt
  ///
  /// G_BRINDIRECT is an indirect branch to \p Tgt.
  ///
  /// \pre setBasicBlock or setMI must have been called.
  /// \pre \p Tgt must be a generic virtual register with pointer type.
  ///
  /// \return a MachineInstrBuilder for the newly created instruction.
  MachineInstrBuilder buildBrIndirect(unsigned Tgt);

  /// Build and insert \p Res = G_CONSTANT \p Val
  ///
  /// G_CONSTANT is an integer constant with the specified size and value. \p
  /// Val will be extended or truncated to the size of \p Reg.
  ///
  /// \pre setBasicBlock or setMI must have been called.
  /// \pre \p Res must be a generic virtual register with scalar or pointer
  ///      type.
  ///
  /// \return The newly created instruction.
  MachineInstrBuilder buildConstant(unsigned Res, const ConstantInt &Val);

  /// Build and insert \p Res = G_CONSTANT \p Val
  ///
  /// G_CONSTANT is an integer constant with the specified size and value.
  ///
  /// \pre setBasicBlock or setMI must have been called.
  /// \pre \p Res must be a generic virtual register with scalar type.
  ///
  /// \return The newly created instruction.
  MachineInstrBuilder buildConstant(unsigned Res, int64_t Val);

  template <typename DstType>
  MachineInstrBuilder buildConstant(DstType &&Res, int64_t Val) {
    return buildConstant(getDestFromArg(Res), Val);
  }
  /// Build and insert \p Res = G_FCONSTANT \p Val
  ///
  /// G_FCONSTANT is a floating-point constant with the specified size and
  /// value.
  ///
  /// \pre setBasicBlock or setMI must have been called.
  /// \pre \p Res must be a generic virtual register with scalar type.
  ///
  /// \return The newly created instruction.
  MachineInstrBuilder buildFConstant(unsigned Res, const ConstantFP &Val);

  /// Build and insert \p Res<def> = COPY Op
  ///
  /// Register-to-register COPY sets \p Res to \p Op.
  ///
  /// \pre setBasicBlock or setMI must have been called.
  ///
  /// \return a MachineInstrBuilder for the newly created instruction.
  MachineInstrBuilder buildCopy(unsigned Res, unsigned Op);

  /// Build and insert `Res<def> = G_LOAD Addr, MMO`.
  ///
  /// Loads the value stored at \p Addr. Puts the result in \p Res.
  ///
  /// \pre setBasicBlock or setMI must have been called.
  /// \pre \p Res must be a generic virtual register.
  /// \pre \p Addr must be a generic virtual register with pointer type.
  ///
  /// \return a MachineInstrBuilder for the newly created instruction.
  MachineInstrBuilder buildLoad(unsigned Res, unsigned Addr,
                                MachineMemOperand &MMO);

  /// Build and insert `G_STORE Val, Addr, MMO`.
  ///
  /// Stores the value \p Val to \p Addr.
  ///
  /// \pre setBasicBlock or setMI must have been called.
  /// \pre \p Val must be a generic virtual register.
  /// \pre \p Addr must be a generic virtual register with pointer type.
  ///
  /// \return a MachineInstrBuilder for the newly created instruction.
  MachineInstrBuilder buildStore(unsigned Val, unsigned Addr,
                                 MachineMemOperand &MMO);

  /// Build and insert `Res0<def>, ... = G_EXTRACT Src, Idx0`.
  ///
  /// \pre setBasicBlock or setMI must have been called.
  /// \pre \p Res and \p Src must be generic virtual registers.
  ///
  /// \return a MachineInstrBuilder for the newly created instruction.
  MachineInstrBuilder buildExtract(unsigned Res, unsigned Src, uint64_t Index);

  /// Build and insert \p Res = IMPLICIT_DEF.
  MachineInstrBuilder buildUndef(unsigned Dst);

  /// Build and insert instructions to put \p Ops together at the specified p
  /// Indices to form a larger register.
  ///
  /// If the types of the input registers are uniform and cover the entirity of
  /// \p Res then a G_MERGE_VALUES will be produced. Otherwise an IMPLICIT_DEF
  /// followed by a sequence of G_INSERT instructions.
  ///
  /// \pre setBasicBlock or setMI must have been called.
  /// \pre The final element of the sequence must not extend past the end of the
  ///      destination register.
  /// \pre The bits defined by each Op (derived from index and scalar size) must
  ///      not overlap.
  /// \pre \p Indices must be in ascending order of bit position.
  void buildSequence(unsigned Res, ArrayRef<unsigned> Ops,
                     ArrayRef<uint64_t> Indices);

  /// Build and insert \p Res<def> = G_MERGE_VALUES \p Op0, ...
  ///
  /// G_MERGE_VALUES combines the input elements contiguously into a larger
  /// register.
  ///
  /// \pre setBasicBlock or setMI must have been called.
  /// \pre The entire register \p Res (and no more) must be covered by the input
  ///      registers.
  /// \pre The type of all \p Ops registers must be identical.
  ///
  /// \return a MachineInstrBuilder for the newly created instruction.
  MachineInstrBuilder buildMerge(unsigned Res, ArrayRef<unsigned> Ops);

  /// Build and insert \p Res0<def>, ... = G_UNMERGE_VALUES \p Op
  ///
  /// G_UNMERGE_VALUES splits contiguous bits of the input into multiple
  ///
  /// \pre setBasicBlock or setMI must have been called.
  /// \pre The entire register \p Res (and no more) must be covered by the input
  ///      registers.
  /// \pre The type of all \p Res registers must be identical.
  ///
  /// \return a MachineInstrBuilder for the newly created instruction.
  MachineInstrBuilder buildUnmerge(ArrayRef<unsigned> Res, unsigned Op);

  MachineInstrBuilder buildInsert(unsigned Res, unsigned Src,
                                  unsigned Op, unsigned Index);

  /// Build and insert either a G_INTRINSIC (if \p HasSideEffects is false) or
  /// G_INTRINSIC_W_SIDE_EFFECTS instruction. Its first operand will be the
  /// result register definition unless \p Reg is NoReg (== 0). The second
  /// operand will be the intrinsic's ID.
  ///
  /// Callers are expected to add the required definitions and uses afterwards.
  ///
  /// \pre setBasicBlock or setMI must have been called.
  ///
  /// \return a MachineInstrBuilder for the newly created instruction.
  MachineInstrBuilder buildIntrinsic(Intrinsic::ID ID, unsigned Res,
                                     bool HasSideEffects);

  /// Build and insert \p Res<def> = G_FPTRUNC \p Op
  ///
  /// G_FPTRUNC converts a floating-point value into one with a smaller type.
  ///
  /// \pre setBasicBlock or setMI must have been called.
  /// \pre \p Res must be a generic virtual register with scalar or vector type.
  /// \pre \p Op must be a generic virtual register with scalar or vector type.
  /// \pre \p Res must be smaller than \p Op
  ///
  /// \return The newly created instruction.
  MachineInstrBuilder buildFPTrunc(unsigned Res, unsigned Op);

  /// Build and insert \p Res<def> = G_TRUNC \p Op
  ///
  /// G_TRUNC extracts the low bits of a type. For a vector type each element is
  /// truncated independently before being packed into the destination.
  ///
  /// \pre setBasicBlock or setMI must have been called.
  /// \pre \p Res must be a generic virtual register with scalar or vector type.
  /// \pre \p Op must be a generic virtual register with scalar or vector type.
  /// \pre \p Res must be smaller than \p Op
  ///
  /// \return The newly created instruction.
  MachineInstrBuilder buildTrunc(unsigned Res, unsigned Op);

  /// Build and insert a \p Res = G_ICMP \p Pred, \p Op0, \p Op1
  ///
  /// \pre setBasicBlock or setMI must have been called.

  /// \pre \p Res must be a generic virtual register with scalar or
  ///      vector type. Typically this starts as s1 or <N x s1>.
  /// \pre \p Op0 and Op1 must be generic virtual registers with the
  ///      same number of elements as \p Res. If \p Res is a scalar,
  ///      \p Op0 must be either a scalar or pointer.
  /// \pre \p Pred must be an integer predicate.
  ///
  /// \return a MachineInstrBuilder for the newly created instruction.
  MachineInstrBuilder buildICmp(CmpInst::Predicate Pred,
                                unsigned Res, unsigned Op0, unsigned Op1);

  /// Build and insert a \p Res = G_FCMP \p Pred\p Op0, \p Op1
  ///
  /// \pre setBasicBlock or setMI must have been called.

  /// \pre \p Res must be a generic virtual register with scalar or
  ///      vector type. Typically this starts as s1 or <N x s1>.
  /// \pre \p Op0 and Op1 must be generic virtual registers with the
  ///      same number of elements as \p Res (or scalar, if \p Res is
  ///      scalar).
  /// \pre \p Pred must be a floating-point predicate.
  ///
  /// \return a MachineInstrBuilder for the newly created instruction.
  MachineInstrBuilder buildFCmp(CmpInst::Predicate Pred,
                                unsigned Res, unsigned Op0, unsigned Op1);

  /// Build and insert a \p Res = G_SELECT \p Tst, \p Op0, \p Op1
  ///
  /// \pre setBasicBlock or setMI must have been called.
  /// \pre \p Res, \p Op0 and \p Op1 must be generic virtual registers
  ///      with the same type.
  /// \pre \p Tst must be a generic virtual register with scalar, pointer or
  ///      vector type. If vector then it must have the same number of
  ///      elements as the other parameters.
  ///
  /// \return a MachineInstrBuilder for the newly created instruction.
  MachineInstrBuilder buildSelect(unsigned Res, unsigned Tst,
                                  unsigned Op0, unsigned Op1);

  /// Build and insert \p Res<def> = G_INSERT_VECTOR_ELT \p Val,
  /// \p Elt, \p Idx
  ///
  /// \pre setBasicBlock or setMI must have been called.
  /// \pre \p Res and \p Val must be a generic virtual register
  //       with the same vector type.
  /// \pre \p Elt and \p Idx must be a generic virtual register
  ///      with scalar type.
  ///
  /// \return The newly created instruction.
  MachineInstrBuilder buildInsertVectorElement(unsigned Res, unsigned Val,
                                               unsigned Elt, unsigned Idx);

  /// Build and insert \p Res<def> = G_EXTRACT_VECTOR_ELT \p Val, \p Idx
  ///
  /// \pre setBasicBlock or setMI must have been called.
  /// \pre \p Res must be a generic virtual register with scalar type.
  /// \pre \p Val must be a generic virtual register with vector type.
  /// \pre \p Idx must be a generic virtual register with scalar type.
  ///
  /// \return The newly created instruction.
  MachineInstrBuilder buildExtractVectorElement(unsigned Res, unsigned Val,
                                                unsigned Idx);
};

} // End namespace llvm.
#endif // LLVM_CODEGEN_GLOBALISEL_MACHINEIRBUILDER_H
