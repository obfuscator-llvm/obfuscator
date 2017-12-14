//===-- SIISelLowering.h - SI DAG Lowering Interface ------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
/// \file
/// \brief SI DAG Lowering interface definition
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_AMDGPU_SIISELLOWERING_H
#define LLVM_LIB_TARGET_AMDGPU_SIISELLOWERING_H

#include "AMDGPUISelLowering.h"
#include "SIInstrInfo.h"

namespace llvm {

class SITargetLowering final : public AMDGPUTargetLowering {
  SDValue lowerKernArgParameterPtr(SelectionDAG &DAG, const SDLoc &SL,
                                   SDValue Chain, uint64_t Offset) const;
  SDValue lowerKernargMemParameter(SelectionDAG &DAG, EVT VT, EVT MemVT,
                                   const SDLoc &SL, SDValue Chain,
                                   uint64_t Offset, bool Signed,
                                   const ISD::InputArg *Arg = nullptr) const;

  SDValue lowerStackParameter(SelectionDAG &DAG, CCValAssign &VA,
                              const SDLoc &SL, SDValue Chain,
                              const ISD::InputArg &Arg) const;

  SDValue LowerGlobalAddress(AMDGPUMachineFunction *MFI, SDValue Op,
                             SelectionDAG &DAG) const override;
  SDValue lowerImplicitZextParam(SelectionDAG &DAG, SDValue Op,
                                 MVT VT, unsigned Offset) const;

  SDValue LowerINTRINSIC_WO_CHAIN(SDValue Op, SelectionDAG &DAG) const;
  SDValue LowerINTRINSIC_W_CHAIN(SDValue Op, SelectionDAG &DAG) const;
  SDValue LowerINTRINSIC_VOID(SDValue Op, SelectionDAG &DAG) const;
  SDValue LowerLOAD(SDValue Op, SelectionDAG &DAG) const;
  SDValue LowerSELECT(SDValue Op, SelectionDAG &DAG) const;
  SDValue lowerFastUnsafeFDIV(SDValue Op, SelectionDAG &DAG) const;
  SDValue lowerFDIV_FAST(SDValue Op, SelectionDAG &DAG) const;
  SDValue LowerFDIV16(SDValue Op, SelectionDAG &DAG) const;
  SDValue LowerFDIV32(SDValue Op, SelectionDAG &DAG) const;
  SDValue LowerFDIV64(SDValue Op, SelectionDAG &DAG) const;
  SDValue LowerFDIV(SDValue Op, SelectionDAG &DAG) const;
  SDValue LowerINT_TO_FP(SDValue Op, SelectionDAG &DAG, bool Signed) const;
  SDValue LowerSTORE(SDValue Op, SelectionDAG &DAG) const;
  SDValue LowerTrig(SDValue Op, SelectionDAG &DAG) const;
  SDValue LowerATOMIC_CMP_SWAP(SDValue Op, SelectionDAG &DAG) const;
  SDValue LowerBRCOND(SDValue Op, SelectionDAG &DAG) const;

  /// \brief Converts \p Op, which must be of floating point type, to the
  /// floating point type \p VT, by either extending or truncating it.
  SDValue getFPExtOrFPTrunc(SelectionDAG &DAG,
                            SDValue Op,
                            const SDLoc &DL,
                            EVT VT) const;

  SDValue convertArgType(
    SelectionDAG &DAG, EVT VT, EVT MemVT, const SDLoc &SL, SDValue Val,
    bool Signed, const ISD::InputArg *Arg = nullptr) const;

  /// \brief Custom lowering for ISD::FP_ROUND for MVT::f16.
  SDValue lowerFP_ROUND(SDValue Op, SelectionDAG &DAG) const;

  SDValue getSegmentAperture(unsigned AS, const SDLoc &DL,
                             SelectionDAG &DAG) const;

  SDValue lowerADDRSPACECAST(SDValue Op, SelectionDAG &DAG) const;
  SDValue lowerINSERT_VECTOR_ELT(SDValue Op, SelectionDAG &DAG) const;
  SDValue lowerEXTRACT_VECTOR_ELT(SDValue Op, SelectionDAG &DAG) const;
  SDValue lowerTRAP(SDValue Op, SelectionDAG &DAG) const;

  void adjustWritemask(MachineSDNode *&N, SelectionDAG &DAG) const;

  SDValue performUCharToFloatCombine(SDNode *N,
                                     DAGCombinerInfo &DCI) const;
  SDValue performSHLPtrCombine(SDNode *N,
                               unsigned AS,
                               DAGCombinerInfo &DCI) const;

  SDValue performMemSDNodeCombine(MemSDNode *N, DAGCombinerInfo &DCI) const;

  SDValue splitBinaryBitConstantOp(DAGCombinerInfo &DCI, const SDLoc &SL,
                                   unsigned Opc, SDValue LHS,
                                   const ConstantSDNode *CRHS) const;

  SDValue performAndCombine(SDNode *N, DAGCombinerInfo &DCI) const;
  SDValue performOrCombine(SDNode *N, DAGCombinerInfo &DCI) const;
  SDValue performXorCombine(SDNode *N, DAGCombinerInfo &DCI) const;
  SDValue performZeroExtendCombine(SDNode *N, DAGCombinerInfo &DCI) const;
  SDValue performClassCombine(SDNode *N, DAGCombinerInfo &DCI) const;
  SDValue performFCanonicalizeCombine(SDNode *N, DAGCombinerInfo &DCI) const;

  SDValue performFPMed3ImmCombine(SelectionDAG &DAG, const SDLoc &SL,
                                  SDValue Op0, SDValue Op1) const;
  SDValue performIntMed3ImmCombine(SelectionDAG &DAG, const SDLoc &SL,
                                   SDValue Op0, SDValue Op1, bool Signed) const;
  SDValue performMinMaxCombine(SDNode *N, DAGCombinerInfo &DCI) const;
  SDValue performFMed3Combine(SDNode *N, DAGCombinerInfo &DCI) const;
  SDValue performCvtPkRTZCombine(SDNode *N, DAGCombinerInfo &DCI) const;
  SDValue performExtractVectorEltCombine(SDNode *N, DAGCombinerInfo &DCI) const;

  unsigned getFusedOpcode(const SelectionDAG &DAG,
                          const SDNode *N0, const SDNode *N1) const;
  SDValue performAddCombine(SDNode *N, DAGCombinerInfo &DCI) const;
  SDValue performAddCarrySubCarryCombine(SDNode *N, DAGCombinerInfo &DCI) const;
  SDValue performSubCombine(SDNode *N, DAGCombinerInfo &DCI) const;
  SDValue performFAddCombine(SDNode *N, DAGCombinerInfo &DCI) const;
  SDValue performFSubCombine(SDNode *N, DAGCombinerInfo &DCI) const;
  SDValue performSetCCCombine(SDNode *N, DAGCombinerInfo &DCI) const;
  SDValue performCvtF32UByteNCombine(SDNode *N, DAGCombinerInfo &DCI) const;

  bool isLegalFlatAddressingMode(const AddrMode &AM) const;
  bool isLegalMUBUFAddressingMode(const AddrMode &AM) const;

  unsigned isCFIntrinsic(const SDNode *Intr) const;

  void createDebuggerPrologueStackObjects(MachineFunction &MF) const;

  /// \returns True if fixup needs to be emitted for given global value \p GV,
  /// false otherwise.
  bool shouldEmitFixup(const GlobalValue *GV) const;

  /// \returns True if GOT relocation needs to be emitted for given global value
  /// \p GV, false otherwise.
  bool shouldEmitGOTReloc(const GlobalValue *GV) const;

  /// \returns True if PC-relative relocation needs to be emitted for given
  /// global value \p GV, false otherwise.
  bool shouldEmitPCReloc(const GlobalValue *GV) const;

public:
  SITargetLowering(const TargetMachine &tm, const SISubtarget &STI);

  const SISubtarget *getSubtarget() const;

  bool isShuffleMaskLegal(const SmallVectorImpl<int> &/*Mask*/,
                          EVT /*VT*/) const override;

  bool getTgtMemIntrinsic(IntrinsicInfo &, const CallInst &,
                          unsigned IntrinsicID) const override;

  bool getAddrModeArguments(IntrinsicInst * /*I*/,
                            SmallVectorImpl<Value*> &/*Ops*/,
                            Type *&/*AccessTy*/) const override;

  bool isLegalAddressingMode(const DataLayout &DL, const AddrMode &AM, Type *Ty,
                             unsigned AS) const override;

  bool canMergeStoresTo(unsigned AS, EVT MemVT,
                        const SelectionDAG &DAG) const override;

  bool allowsMisalignedMemoryAccesses(EVT VT, unsigned AS,
                                      unsigned Align,
                                      bool *IsFast) const override;

  EVT getOptimalMemOpType(uint64_t Size, unsigned DstAlign,
                          unsigned SrcAlign, bool IsMemset,
                          bool ZeroMemset,
                          bool MemcpyStrSrc,
                          MachineFunction &MF) const override;

  bool isMemOpUniform(const SDNode *N) const;
  bool isMemOpHasNoClobberedMemOperand(const SDNode *N) const;
  bool isNoopAddrSpaceCast(unsigned SrcAS, unsigned DestAS) const override;
  bool isCheapAddrSpaceCast(unsigned SrcAS, unsigned DestAS) const override;

  TargetLoweringBase::LegalizeTypeAction
  getPreferredVectorAction(EVT VT) const override;

  bool shouldConvertConstantLoadToIntImm(const APInt &Imm,
                                        Type *Ty) const override;

  bool isTypeDesirableForOp(unsigned Op, EVT VT) const override;

  bool isOffsetFoldingLegal(const GlobalAddressSDNode *GA) const override;

  SDValue LowerFormalArguments(SDValue Chain, CallingConv::ID CallConv,
                               bool isVarArg,
                               const SmallVectorImpl<ISD::InputArg> &Ins,
                               const SDLoc &DL, SelectionDAG &DAG,
                               SmallVectorImpl<SDValue> &InVals) const override;

  bool CanLowerReturn(CallingConv::ID CallConv,
                      MachineFunction &MF, bool isVarArg,
                      const SmallVectorImpl<ISD::OutputArg> &Outs,
                      LLVMContext &Context) const override;

  SDValue LowerReturn(SDValue Chain, CallingConv::ID CallConv, bool IsVarArg,
                      const SmallVectorImpl<ISD::OutputArg> &Outs,
                      const SmallVectorImpl<SDValue> &OutVals, const SDLoc &DL,
                      SelectionDAG &DAG) const override;

  unsigned getRegisterByName(const char* RegName, EVT VT,
                             SelectionDAG &DAG) const override;

  MachineBasicBlock *splitKillBlock(MachineInstr &MI,
                                    MachineBasicBlock *BB) const;

  MachineBasicBlock *
  EmitInstrWithCustomInserter(MachineInstr &MI,
                              MachineBasicBlock *BB) const override;
  bool enableAggressiveFMAFusion(EVT VT) const override;
  EVT getSetCCResultType(const DataLayout &DL, LLVMContext &Context,
                         EVT VT) const override;
  MVT getScalarShiftAmountTy(const DataLayout &, EVT) const override;
  bool isFMAFasterThanFMulAndFAdd(EVT VT) const override;
  SDValue LowerOperation(SDValue Op, SelectionDAG &DAG) const override;
  void ReplaceNodeResults(SDNode *N, SmallVectorImpl<SDValue> &Results,
                          SelectionDAG &DAG) const override;

  SDValue PerformDAGCombine(SDNode *N, DAGCombinerInfo &DCI) const override;
  SDNode *PostISelFolding(MachineSDNode *N, SelectionDAG &DAG) const override;
  void AdjustInstrPostInstrSelection(MachineInstr &MI,
                                     SDNode *Node) const override;

  SDNode *legalizeTargetIndependentNode(SDNode *Node, SelectionDAG &DAG) const;

  MachineSDNode *wrapAddr64Rsrc(SelectionDAG &DAG, const SDLoc &DL,
                                SDValue Ptr) const;
  MachineSDNode *buildRSRC(SelectionDAG &DAG, const SDLoc &DL, SDValue Ptr,
                           uint32_t RsrcDword1, uint64_t RsrcDword2And3) const;
  std::pair<unsigned, const TargetRegisterClass *>
  getRegForInlineAsmConstraint(const TargetRegisterInfo *TRI,
                               StringRef Constraint, MVT VT) const override;
  ConstraintType getConstraintType(StringRef Constraint) const override;
  SDValue copyToM0(SelectionDAG &DAG, SDValue Chain, const SDLoc &DL,
                   SDValue V) const;

  void finalizeLowering(MachineFunction &MF) const override;
};

} // End namespace llvm

#endif
