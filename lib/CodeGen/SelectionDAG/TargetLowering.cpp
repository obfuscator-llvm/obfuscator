//===-- TargetLowering.cpp - Implement the TargetLowering class -----------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This implements the TargetLowering class.
//
//===----------------------------------------------------------------------===//

#include "llvm/Target/TargetLowering.h"
#include "llvm/ADT/BitVector.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/CodeGen/CallingConvLower.h"
#include "llvm/CodeGen/MachineFrameInfo.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineJumpTableInfo.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/CodeGen/SelectionDAG.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/MC/MCAsmInfo.h"
#include "llvm/MC/MCExpr.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/KnownBits.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/Target/TargetLoweringObjectFile.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Target/TargetRegisterInfo.h"
#include "llvm/Target/TargetSubtargetInfo.h"
#include <cctype>
using namespace llvm;

/// NOTE: The TargetMachine owns TLOF.
TargetLowering::TargetLowering(const TargetMachine &tm)
  : TargetLoweringBase(tm) {}

const char *TargetLowering::getTargetNodeName(unsigned Opcode) const {
  return nullptr;
}

bool TargetLowering::isPositionIndependent() const {
  return getTargetMachine().isPositionIndependent();
}

/// Check whether a given call node is in tail position within its function. If
/// so, it sets Chain to the input chain of the tail call.
bool TargetLowering::isInTailCallPosition(SelectionDAG &DAG, SDNode *Node,
                                          SDValue &Chain) const {
  const Function *F = DAG.getMachineFunction().getFunction();

  // Conservatively require the attributes of the call to match those of
  // the return. Ignore noalias because it doesn't affect the call sequence.
  AttributeList CallerAttrs = F->getAttributes();
  if (AttrBuilder(CallerAttrs, AttributeList::ReturnIndex)
          .removeAttribute(Attribute::NoAlias)
          .hasAttributes())
    return false;

  // It's not safe to eliminate the sign / zero extension of the return value.
  if (CallerAttrs.hasAttribute(AttributeList::ReturnIndex, Attribute::ZExt) ||
      CallerAttrs.hasAttribute(AttributeList::ReturnIndex, Attribute::SExt))
    return false;

  // Check if the only use is a function return node.
  return isUsedByReturnOnly(Node, Chain);
}

bool TargetLowering::parametersInCSRMatch(const MachineRegisterInfo &MRI,
    const uint32_t *CallerPreservedMask,
    const SmallVectorImpl<CCValAssign> &ArgLocs,
    const SmallVectorImpl<SDValue> &OutVals) const {
  for (unsigned I = 0, E = ArgLocs.size(); I != E; ++I) {
    const CCValAssign &ArgLoc = ArgLocs[I];
    if (!ArgLoc.isRegLoc())
      continue;
    unsigned Reg = ArgLoc.getLocReg();
    // Only look at callee saved registers.
    if (MachineOperand::clobbersPhysReg(CallerPreservedMask, Reg))
      continue;
    // Check that we pass the value used for the caller.
    // (We look for a CopyFromReg reading a virtual register that is used
    //  for the function live-in value of register Reg)
    SDValue Value = OutVals[I];
    if (Value->getOpcode() != ISD::CopyFromReg)
      return false;
    unsigned ArgReg = cast<RegisterSDNode>(Value->getOperand(1))->getReg();
    if (MRI.getLiveInPhysReg(ArgReg) != Reg)
      return false;
  }
  return true;
}

/// \brief Set CallLoweringInfo attribute flags based on a call instruction
/// and called function attributes.
void TargetLoweringBase::ArgListEntry::setAttributes(ImmutableCallSite *CS,
                                                     unsigned ArgIdx) {
  IsSExt = CS->paramHasAttr(ArgIdx, Attribute::SExt);
  IsZExt = CS->paramHasAttr(ArgIdx, Attribute::ZExt);
  IsInReg = CS->paramHasAttr(ArgIdx, Attribute::InReg);
  IsSRet = CS->paramHasAttr(ArgIdx, Attribute::StructRet);
  IsNest = CS->paramHasAttr(ArgIdx, Attribute::Nest);
  IsByVal = CS->paramHasAttr(ArgIdx, Attribute::ByVal);
  IsInAlloca = CS->paramHasAttr(ArgIdx, Attribute::InAlloca);
  IsReturned = CS->paramHasAttr(ArgIdx, Attribute::Returned);
  IsSwiftSelf = CS->paramHasAttr(ArgIdx, Attribute::SwiftSelf);
  IsSwiftError = CS->paramHasAttr(ArgIdx, Attribute::SwiftError);
  Alignment  = CS->getParamAlignment(ArgIdx);
}

/// Generate a libcall taking the given operands as arguments and returning a
/// result of type RetVT.
std::pair<SDValue, SDValue>
TargetLowering::makeLibCall(SelectionDAG &DAG, RTLIB::Libcall LC, EVT RetVT,
                            ArrayRef<SDValue> Ops, bool isSigned,
                            const SDLoc &dl, bool doesNotReturn,
                            bool isReturnValueUsed) const {
  TargetLowering::ArgListTy Args;
  Args.reserve(Ops.size());

  TargetLowering::ArgListEntry Entry;
  for (SDValue Op : Ops) {
    Entry.Node = Op;
    Entry.Ty = Entry.Node.getValueType().getTypeForEVT(*DAG.getContext());
    Entry.IsSExt = shouldSignExtendTypeInLibCall(Op.getValueType(), isSigned);
    Entry.IsZExt = !shouldSignExtendTypeInLibCall(Op.getValueType(), isSigned);
    Args.push_back(Entry);
  }

  if (LC == RTLIB::UNKNOWN_LIBCALL)
    report_fatal_error("Unsupported library call operation!");
  SDValue Callee = DAG.getExternalSymbol(getLibcallName(LC),
                                         getPointerTy(DAG.getDataLayout()));

  Type *RetTy = RetVT.getTypeForEVT(*DAG.getContext());
  TargetLowering::CallLoweringInfo CLI(DAG);
  bool signExtend = shouldSignExtendTypeInLibCall(RetVT, isSigned);
  CLI.setDebugLoc(dl)
      .setChain(DAG.getEntryNode())
      .setLibCallee(getLibcallCallingConv(LC), RetTy, Callee, std::move(Args))
      .setNoReturn(doesNotReturn)
      .setDiscardResult(!isReturnValueUsed)
      .setSExtResult(signExtend)
      .setZExtResult(!signExtend);
  return LowerCallTo(CLI);
}

/// Soften the operands of a comparison. This code is shared among BR_CC,
/// SELECT_CC, and SETCC handlers.
void TargetLowering::softenSetCCOperands(SelectionDAG &DAG, EVT VT,
                                         SDValue &NewLHS, SDValue &NewRHS,
                                         ISD::CondCode &CCCode,
                                         const SDLoc &dl) const {
  assert((VT == MVT::f32 || VT == MVT::f64 || VT == MVT::f128 || VT == MVT::ppcf128)
         && "Unsupported setcc type!");

  // Expand into one or more soft-fp libcall(s).
  RTLIB::Libcall LC1 = RTLIB::UNKNOWN_LIBCALL, LC2 = RTLIB::UNKNOWN_LIBCALL;
  bool ShouldInvertCC = false;
  switch (CCCode) {
  case ISD::SETEQ:
  case ISD::SETOEQ:
    LC1 = (VT == MVT::f32) ? RTLIB::OEQ_F32 :
          (VT == MVT::f64) ? RTLIB::OEQ_F64 :
          (VT == MVT::f128) ? RTLIB::OEQ_F128 : RTLIB::OEQ_PPCF128;
    break;
  case ISD::SETNE:
  case ISD::SETUNE:
    LC1 = (VT == MVT::f32) ? RTLIB::UNE_F32 :
          (VT == MVT::f64) ? RTLIB::UNE_F64 :
          (VT == MVT::f128) ? RTLIB::UNE_F128 : RTLIB::UNE_PPCF128;
    break;
  case ISD::SETGE:
  case ISD::SETOGE:
    LC1 = (VT == MVT::f32) ? RTLIB::OGE_F32 :
          (VT == MVT::f64) ? RTLIB::OGE_F64 :
          (VT == MVT::f128) ? RTLIB::OGE_F128 : RTLIB::OGE_PPCF128;
    break;
  case ISD::SETLT:
  case ISD::SETOLT:
    LC1 = (VT == MVT::f32) ? RTLIB::OLT_F32 :
          (VT == MVT::f64) ? RTLIB::OLT_F64 :
          (VT == MVT::f128) ? RTLIB::OLT_F128 : RTLIB::OLT_PPCF128;
    break;
  case ISD::SETLE:
  case ISD::SETOLE:
    LC1 = (VT == MVT::f32) ? RTLIB::OLE_F32 :
          (VT == MVT::f64) ? RTLIB::OLE_F64 :
          (VT == MVT::f128) ? RTLIB::OLE_F128 : RTLIB::OLE_PPCF128;
    break;
  case ISD::SETGT:
  case ISD::SETOGT:
    LC1 = (VT == MVT::f32) ? RTLIB::OGT_F32 :
          (VT == MVT::f64) ? RTLIB::OGT_F64 :
          (VT == MVT::f128) ? RTLIB::OGT_F128 : RTLIB::OGT_PPCF128;
    break;
  case ISD::SETUO:
    LC1 = (VT == MVT::f32) ? RTLIB::UO_F32 :
          (VT == MVT::f64) ? RTLIB::UO_F64 :
          (VT == MVT::f128) ? RTLIB::UO_F128 : RTLIB::UO_PPCF128;
    break;
  case ISD::SETO:
    LC1 = (VT == MVT::f32) ? RTLIB::O_F32 :
          (VT == MVT::f64) ? RTLIB::O_F64 :
          (VT == MVT::f128) ? RTLIB::O_F128 : RTLIB::O_PPCF128;
    break;
  case ISD::SETONE:
    // SETONE = SETOLT | SETOGT
    LC1 = (VT == MVT::f32) ? RTLIB::OLT_F32 :
          (VT == MVT::f64) ? RTLIB::OLT_F64 :
          (VT == MVT::f128) ? RTLIB::OLT_F128 : RTLIB::OLT_PPCF128;
    LC2 = (VT == MVT::f32) ? RTLIB::OGT_F32 :
          (VT == MVT::f64) ? RTLIB::OGT_F64 :
          (VT == MVT::f128) ? RTLIB::OGT_F128 : RTLIB::OGT_PPCF128;
    break;
  case ISD::SETUEQ:
    LC1 = (VT == MVT::f32) ? RTLIB::UO_F32 :
          (VT == MVT::f64) ? RTLIB::UO_F64 :
          (VT == MVT::f128) ? RTLIB::UO_F128 : RTLIB::UO_PPCF128;
    LC2 = (VT == MVT::f32) ? RTLIB::OEQ_F32 :
          (VT == MVT::f64) ? RTLIB::OEQ_F64 :
          (VT == MVT::f128) ? RTLIB::OEQ_F128 : RTLIB::OEQ_PPCF128;
    break;
  default:
    // Invert CC for unordered comparisons
    ShouldInvertCC = true;
    switch (CCCode) {
    case ISD::SETULT:
      LC1 = (VT == MVT::f32) ? RTLIB::OGE_F32 :
            (VT == MVT::f64) ? RTLIB::OGE_F64 :
            (VT == MVT::f128) ? RTLIB::OGE_F128 : RTLIB::OGE_PPCF128;
      break;
    case ISD::SETULE:
      LC1 = (VT == MVT::f32) ? RTLIB::OGT_F32 :
            (VT == MVT::f64) ? RTLIB::OGT_F64 :
            (VT == MVT::f128) ? RTLIB::OGT_F128 : RTLIB::OGT_PPCF128;
      break;
    case ISD::SETUGT:
      LC1 = (VT == MVT::f32) ? RTLIB::OLE_F32 :
            (VT == MVT::f64) ? RTLIB::OLE_F64 :
            (VT == MVT::f128) ? RTLIB::OLE_F128 : RTLIB::OLE_PPCF128;
      break;
    case ISD::SETUGE:
      LC1 = (VT == MVT::f32) ? RTLIB::OLT_F32 :
            (VT == MVT::f64) ? RTLIB::OLT_F64 :
            (VT == MVT::f128) ? RTLIB::OLT_F128 : RTLIB::OLT_PPCF128;
      break;
    default: llvm_unreachable("Do not know how to soften this setcc!");
    }
  }

  // Use the target specific return value for comparions lib calls.
  EVT RetVT = getCmpLibcallReturnType();
  SDValue Ops[2] = {NewLHS, NewRHS};
  NewLHS = makeLibCall(DAG, LC1, RetVT, Ops, false /*sign irrelevant*/,
                       dl).first;
  NewRHS = DAG.getConstant(0, dl, RetVT);

  CCCode = getCmpLibcallCC(LC1);
  if (ShouldInvertCC)
    CCCode = getSetCCInverse(CCCode, /*isInteger=*/true);

  if (LC2 != RTLIB::UNKNOWN_LIBCALL) {
    SDValue Tmp = DAG.getNode(
        ISD::SETCC, dl,
        getSetCCResultType(DAG.getDataLayout(), *DAG.getContext(), RetVT),
        NewLHS, NewRHS, DAG.getCondCode(CCCode));
    NewLHS = makeLibCall(DAG, LC2, RetVT, Ops, false/*sign irrelevant*/,
                         dl).first;
    NewLHS = DAG.getNode(
        ISD::SETCC, dl,
        getSetCCResultType(DAG.getDataLayout(), *DAG.getContext(), RetVT),
        NewLHS, NewRHS, DAG.getCondCode(getCmpLibcallCC(LC2)));
    NewLHS = DAG.getNode(ISD::OR, dl, Tmp.getValueType(), Tmp, NewLHS);
    NewRHS = SDValue();
  }
}

/// Return the entry encoding for a jump table in the current function. The
/// returned value is a member of the MachineJumpTableInfo::JTEntryKind enum.
unsigned TargetLowering::getJumpTableEncoding() const {
  // In non-pic modes, just use the address of a block.
  if (!isPositionIndependent())
    return MachineJumpTableInfo::EK_BlockAddress;

  // In PIC mode, if the target supports a GPRel32 directive, use it.
  if (getTargetMachine().getMCAsmInfo()->getGPRel32Directive() != nullptr)
    return MachineJumpTableInfo::EK_GPRel32BlockAddress;

  // Otherwise, use a label difference.
  return MachineJumpTableInfo::EK_LabelDifference32;
}

SDValue TargetLowering::getPICJumpTableRelocBase(SDValue Table,
                                                 SelectionDAG &DAG) const {
  // If our PIC model is GP relative, use the global offset table as the base.
  unsigned JTEncoding = getJumpTableEncoding();

  if ((JTEncoding == MachineJumpTableInfo::EK_GPRel64BlockAddress) ||
      (JTEncoding == MachineJumpTableInfo::EK_GPRel32BlockAddress))
    return DAG.getGLOBAL_OFFSET_TABLE(getPointerTy(DAG.getDataLayout()));

  return Table;
}

/// This returns the relocation base for the given PIC jumptable, the same as
/// getPICJumpTableRelocBase, but as an MCExpr.
const MCExpr *
TargetLowering::getPICJumpTableRelocBaseExpr(const MachineFunction *MF,
                                             unsigned JTI,MCContext &Ctx) const{
  // The normal PIC reloc base is the label at the start of the jump table.
  return MCSymbolRefExpr::create(MF->getJTISymbol(JTI, Ctx), Ctx);
}

bool
TargetLowering::isOffsetFoldingLegal(const GlobalAddressSDNode *GA) const {
  const TargetMachine &TM = getTargetMachine();
  const GlobalValue *GV = GA->getGlobal();

  // If the address is not even local to this DSO we will have to load it from
  // a got and then add the offset.
  if (!TM.shouldAssumeDSOLocal(*GV->getParent(), GV))
    return false;

  // If the code is position independent we will have to add a base register.
  if (isPositionIndependent())
    return false;

  // Otherwise we can do it.
  return true;
}

//===----------------------------------------------------------------------===//
//  Optimization Methods
//===----------------------------------------------------------------------===//

/// If the specified instruction has a constant integer operand and there are
/// bits set in that constant that are not demanded, then clear those bits and
/// return true.
bool TargetLowering::ShrinkDemandedConstant(SDValue Op, const APInt &Demanded,
                                            TargetLoweringOpt &TLO) const {
  SelectionDAG &DAG = TLO.DAG;
  SDLoc DL(Op);
  unsigned Opcode = Op.getOpcode();

  // Do target-specific constant optimization.
  if (targetShrinkDemandedConstant(Op, Demanded, TLO))
    return TLO.New.getNode();

  // FIXME: ISD::SELECT, ISD::SELECT_CC
  switch (Opcode) {
  default:
    break;
  case ISD::XOR:
  case ISD::AND:
  case ISD::OR: {
    auto *Op1C = dyn_cast<ConstantSDNode>(Op.getOperand(1));
    if (!Op1C)
      return false;

    // If this is a 'not' op, don't touch it because that's a canonical form.
    const APInt &C = Op1C->getAPIntValue();
    if (Opcode == ISD::XOR && Demanded.isSubsetOf(C))
      return false;

    if (!C.isSubsetOf(Demanded)) {
      EVT VT = Op.getValueType();
      SDValue NewC = DAG.getConstant(Demanded & C, DL, VT);
      SDValue NewOp = DAG.getNode(Opcode, DL, VT, Op.getOperand(0), NewC);
      return TLO.CombineTo(Op, NewOp);
    }

    break;
  }
  }

  return false;
}

/// Convert x+y to (VT)((SmallVT)x+(SmallVT)y) if the casts are free.
/// This uses isZExtFree and ZERO_EXTEND for the widening cast, but it could be
/// generalized for targets with other types of implicit widening casts.
bool TargetLowering::ShrinkDemandedOp(SDValue Op, unsigned BitWidth,
                                      const APInt &Demanded,
                                      TargetLoweringOpt &TLO) const {
  assert(Op.getNumOperands() == 2 &&
         "ShrinkDemandedOp only supports binary operators!");
  assert(Op.getNode()->getNumValues() == 1 &&
         "ShrinkDemandedOp only supports nodes with one result!");

  SelectionDAG &DAG = TLO.DAG;
  SDLoc dl(Op);

  // Early return, as this function cannot handle vector types.
  if (Op.getValueType().isVector())
    return false;

  // Don't do this if the node has another user, which may require the
  // full value.
  if (!Op.getNode()->hasOneUse())
    return false;

  // Search for the smallest integer type with free casts to and from
  // Op's type. For expedience, just check power-of-2 integer types.
  const TargetLowering &TLI = DAG.getTargetLoweringInfo();
  unsigned DemandedSize = BitWidth - Demanded.countLeadingZeros();
  unsigned SmallVTBits = DemandedSize;
  if (!isPowerOf2_32(SmallVTBits))
    SmallVTBits = NextPowerOf2(SmallVTBits);
  for (; SmallVTBits < BitWidth; SmallVTBits = NextPowerOf2(SmallVTBits)) {
    EVT SmallVT = EVT::getIntegerVT(*DAG.getContext(), SmallVTBits);
    if (TLI.isTruncateFree(Op.getValueType(), SmallVT) &&
        TLI.isZExtFree(SmallVT, Op.getValueType())) {
      // We found a type with free casts.
      SDValue X = DAG.getNode(
          Op.getOpcode(), dl, SmallVT,
          DAG.getNode(ISD::TRUNCATE, dl, SmallVT, Op.getOperand(0)),
          DAG.getNode(ISD::TRUNCATE, dl, SmallVT, Op.getOperand(1)));
      bool NeedZext = DemandedSize > SmallVTBits;
      SDValue Z = DAG.getNode(NeedZext ? ISD::ZERO_EXTEND : ISD::ANY_EXTEND,
                              dl, Op.getValueType(), X);
      return TLO.CombineTo(Op, Z);
    }
  }
  return false;
}

bool
TargetLowering::SimplifyDemandedBits(SDNode *User, unsigned OpIdx,
                                     const APInt &Demanded,
                                     DAGCombinerInfo &DCI,
                                     TargetLoweringOpt &TLO) const {
  SDValue Op = User->getOperand(OpIdx);
  KnownBits Known;

  if (!SimplifyDemandedBits(Op, Demanded, Known, TLO, 0, true))
    return false;


  // Old will not always be the same as Op.  For example:
  //
  // Demanded = 0xffffff
  // Op = i64 truncate (i32 and x, 0xffffff)
  // In this case simplify demand bits will want to replace the 'and' node
  // with the value 'x', which will give us:
  // Old = i32 and x, 0xffffff
  // New = x
  if (TLO.Old.hasOneUse()) {
    // For the one use case, we just commit the change.
    DCI.CommitTargetLoweringOpt(TLO);
    return true;
  }

  // If Old has more than one use then it must be Op, because the
  // AssumeSingleUse flag is not propogated to recursive calls of
  // SimplifyDemanded bits, so the only node with multiple use that
  // it will attempt to combine will be opt.
  assert(TLO.Old == Op);

  SmallVector <SDValue, 4> NewOps;
  for (unsigned i = 0, e = User->getNumOperands(); i != e; ++i) {
    if (i == OpIdx) {
      NewOps.push_back(TLO.New);
      continue;
    }
    NewOps.push_back(User->getOperand(i));
  }
  TLO.DAG.UpdateNodeOperands(User, NewOps);
  // Op has less users now, so we may be able to perform additional combines
  // with it.
  DCI.AddToWorklist(Op.getNode());
  // User's operands have been updated, so we may be able to do new combines
  // with it.
  DCI.AddToWorklist(User);
  return true;
}

bool TargetLowering::SimplifyDemandedBits(SDValue Op, APInt &DemandedMask,
                                          DAGCombinerInfo &DCI) const {

  SelectionDAG &DAG = DCI.DAG;
  TargetLoweringOpt TLO(DAG, !DCI.isBeforeLegalize(),
                        !DCI.isBeforeLegalizeOps());
  KnownBits Known;

  bool Simplified = SimplifyDemandedBits(Op, DemandedMask, Known, TLO);
  if (Simplified)
    DCI.CommitTargetLoweringOpt(TLO);
  return Simplified;
}

/// Look at Op. At this point, we know that only the DemandedMask bits of the
/// result of Op are ever used downstream. If we can use this information to
/// simplify Op, create a new simplified DAG node and return true, returning the
/// original and new nodes in Old and New. Otherwise, analyze the expression and
/// return a mask of Known bits for the expression (used to simplify the
/// caller).  The Known bits may only be accurate for those bits in the
/// DemandedMask.
bool TargetLowering::SimplifyDemandedBits(SDValue Op,
                                          const APInt &DemandedMask,
                                          KnownBits &Known,
                                          TargetLoweringOpt &TLO,
                                          unsigned Depth,
                                          bool AssumeSingleUse) const {
  unsigned BitWidth = DemandedMask.getBitWidth();
  assert(Op.getScalarValueSizeInBits() == BitWidth &&
         "Mask size mismatches value type size!");
  APInt NewMask = DemandedMask;
  SDLoc dl(Op);
  auto &DL = TLO.DAG.getDataLayout();

  // Don't know anything.
  Known = KnownBits(BitWidth);

  // Other users may use these bits.
  if (!Op.getNode()->hasOneUse() && !AssumeSingleUse) {
    if (Depth != 0) {
      // If not at the root, Just compute the Known bits to
      // simplify things downstream.
      TLO.DAG.computeKnownBits(Op, Known, Depth);
      return false;
    }
    // If this is the root being simplified, allow it to have multiple uses,
    // just set the NewMask to all bits.
    NewMask = APInt::getAllOnesValue(BitWidth);
  } else if (DemandedMask == 0) {
    // Not demanding any bits from Op.
    if (!Op.isUndef())
      return TLO.CombineTo(Op, TLO.DAG.getUNDEF(Op.getValueType()));
    return false;
  } else if (Depth == 6) {        // Limit search depth.
    return false;
  }

  KnownBits Known2, KnownOut;
  switch (Op.getOpcode()) {
  case ISD::Constant:
    // We know all of the bits for a constant!
    Known.One = cast<ConstantSDNode>(Op)->getAPIntValue();
    Known.Zero = ~Known.One;
    return false;   // Don't fall through, will infinitely loop.
  case ISD::BUILD_VECTOR:
    // Collect the known bits that are shared by every constant vector element.
    Known.Zero.setAllBits(); Known.One.setAllBits();
    for (SDValue SrcOp : Op->ops()) {
      if (!isa<ConstantSDNode>(SrcOp)) {
        // We can only handle all constant values - bail out with no known bits.
        Known = KnownBits(BitWidth);
        return false;
      }
      Known2.One = cast<ConstantSDNode>(SrcOp)->getAPIntValue();
      Known2.Zero = ~Known2.One;

      // BUILD_VECTOR can implicitly truncate sources, we must handle this.
      if (Known2.One.getBitWidth() != BitWidth) {
        assert(Known2.getBitWidth() > BitWidth &&
               "Expected BUILD_VECTOR implicit truncation");
        Known2 = Known2.trunc(BitWidth);
      }

      // Known bits are the values that are shared by every element.
      // TODO: support per-element known bits.
      Known.One &= Known2.One;
      Known.Zero &= Known2.Zero;
    }
    return false;   // Don't fall through, will infinitely loop.
  case ISD::AND:
    // If the RHS is a constant, check to see if the LHS would be zero without
    // using the bits from the RHS.  Below, we use knowledge about the RHS to
    // simplify the LHS, here we're using information from the LHS to simplify
    // the RHS.
    if (ConstantSDNode *RHSC = isConstOrConstSplat(Op.getOperand(1))) {
      SDValue Op0 = Op.getOperand(0);
      KnownBits LHSKnown;
      // Do not increment Depth here; that can cause an infinite loop.
      TLO.DAG.computeKnownBits(Op0, LHSKnown, Depth);
      // If the LHS already has zeros where RHSC does, this and is dead.
      if ((LHSKnown.Zero & NewMask) == (~RHSC->getAPIntValue() & NewMask))
        return TLO.CombineTo(Op, Op0);

      // If any of the set bits in the RHS are known zero on the LHS, shrink
      // the constant.
      if (ShrinkDemandedConstant(Op, ~LHSKnown.Zero & NewMask, TLO))
        return true;

      // Bitwise-not (xor X, -1) is a special case: we don't usually shrink its
      // constant, but if this 'and' is only clearing bits that were just set by
      // the xor, then this 'and' can be eliminated by shrinking the mask of
      // the xor. For example, for a 32-bit X:
      // and (xor (srl X, 31), -1), 1 --> xor (srl X, 31), 1
      if (isBitwiseNot(Op0) && Op0.hasOneUse() &&
          LHSKnown.One == ~RHSC->getAPIntValue()) {
        SDValue Xor = TLO.DAG.getNode(ISD::XOR, dl, Op.getValueType(),
                                      Op0.getOperand(0), Op.getOperand(1));
        return TLO.CombineTo(Op, Xor);
      }
    }

    if (SimplifyDemandedBits(Op.getOperand(1), NewMask, Known, TLO, Depth+1))
      return true;
    assert(!Known.hasConflict() && "Bits known to be one AND zero?");
    if (SimplifyDemandedBits(Op.getOperand(0), ~Known.Zero & NewMask,
                             Known2, TLO, Depth+1))
      return true;
    assert(!Known2.hasConflict() && "Bits known to be one AND zero?");

    // If all of the demanded bits are known one on one side, return the other.
    // These bits cannot contribute to the result of the 'and'.
    if (NewMask.isSubsetOf(Known2.Zero | Known.One))
      return TLO.CombineTo(Op, Op.getOperand(0));
    if (NewMask.isSubsetOf(Known.Zero | Known2.One))
      return TLO.CombineTo(Op, Op.getOperand(1));
    // If all of the demanded bits in the inputs are known zeros, return zero.
    if (NewMask.isSubsetOf(Known.Zero | Known2.Zero))
      return TLO.CombineTo(Op, TLO.DAG.getConstant(0, dl, Op.getValueType()));
    // If the RHS is a constant, see if we can simplify it.
    if (ShrinkDemandedConstant(Op, ~Known2.Zero & NewMask, TLO))
      return true;
    // If the operation can be done in a smaller type, do so.
    if (ShrinkDemandedOp(Op, BitWidth, NewMask, TLO))
      return true;

    // Output known-1 bits are only known if set in both the LHS & RHS.
    Known.One &= Known2.One;
    // Output known-0 are known to be clear if zero in either the LHS | RHS.
    Known.Zero |= Known2.Zero;
    break;
  case ISD::OR:
    if (SimplifyDemandedBits(Op.getOperand(1), NewMask, Known, TLO, Depth+1))
      return true;
    assert(!Known.hasConflict() && "Bits known to be one AND zero?");
    if (SimplifyDemandedBits(Op.getOperand(0), ~Known.One & NewMask,
                             Known2, TLO, Depth+1))
      return true;
    assert(!Known2.hasConflict() && "Bits known to be one AND zero?");

    // If all of the demanded bits are known zero on one side, return the other.
    // These bits cannot contribute to the result of the 'or'.
    if (NewMask.isSubsetOf(Known2.One | Known.Zero))
      return TLO.CombineTo(Op, Op.getOperand(0));
    if (NewMask.isSubsetOf(Known.One | Known2.Zero))
      return TLO.CombineTo(Op, Op.getOperand(1));
    // If the RHS is a constant, see if we can simplify it.
    if (ShrinkDemandedConstant(Op, NewMask, TLO))
      return true;
    // If the operation can be done in a smaller type, do so.
    if (ShrinkDemandedOp(Op, BitWidth, NewMask, TLO))
      return true;

    // Output known-0 bits are only known if clear in both the LHS & RHS.
    Known.Zero &= Known2.Zero;
    // Output known-1 are known to be set if set in either the LHS | RHS.
    Known.One |= Known2.One;
    break;
  case ISD::XOR: {
    if (SimplifyDemandedBits(Op.getOperand(1), NewMask, Known, TLO, Depth+1))
      return true;
    assert(!Known.hasConflict() && "Bits known to be one AND zero?");
    if (SimplifyDemandedBits(Op.getOperand(0), NewMask, Known2, TLO, Depth+1))
      return true;
    assert(!Known2.hasConflict() && "Bits known to be one AND zero?");

    // If all of the demanded bits are known zero on one side, return the other.
    // These bits cannot contribute to the result of the 'xor'.
    if (NewMask.isSubsetOf(Known.Zero))
      return TLO.CombineTo(Op, Op.getOperand(0));
    if (NewMask.isSubsetOf(Known2.Zero))
      return TLO.CombineTo(Op, Op.getOperand(1));
    // If the operation can be done in a smaller type, do so.
    if (ShrinkDemandedOp(Op, BitWidth, NewMask, TLO))
      return true;

    // If all of the unknown bits are known to be zero on one side or the other
    // (but not both) turn this into an *inclusive* or.
    //    e.g. (A & C1)^(B & C2) -> (A & C1)|(B & C2) iff C1&C2 == 0
    if ((NewMask & ~Known.Zero & ~Known2.Zero) == 0)
      return TLO.CombineTo(Op, TLO.DAG.getNode(ISD::OR, dl, Op.getValueType(),
                                               Op.getOperand(0),
                                               Op.getOperand(1)));

    // Output known-0 bits are known if clear or set in both the LHS & RHS.
    KnownOut.Zero = (Known.Zero & Known2.Zero) | (Known.One & Known2.One);
    // Output known-1 are known to be set if set in only one of the LHS, RHS.
    KnownOut.One = (Known.Zero & Known2.One) | (Known.One & Known2.Zero);

    // If all of the demanded bits on one side are known, and all of the set
    // bits on that side are also known to be set on the other side, turn this
    // into an AND, as we know the bits will be cleared.
    //    e.g. (X | C1) ^ C2 --> (X | C1) & ~C2 iff (C1&C2) == C2
    // NB: it is okay if more bits are known than are requested
    if (NewMask.isSubsetOf(Known.Zero|Known.One)) { // all known on one side
      if (Known.One == Known2.One) { // set bits are the same on both sides
        EVT VT = Op.getValueType();
        SDValue ANDC = TLO.DAG.getConstant(~Known.One & NewMask, dl, VT);
        return TLO.CombineTo(Op, TLO.DAG.getNode(ISD::AND, dl, VT,
                                                 Op.getOperand(0), ANDC));
      }
    }

    // If the RHS is a constant, see if we can change it. Don't alter a -1
    // constant because that's a 'not' op, and that is better for combining and
    // codegen.
    ConstantSDNode *C = isConstOrConstSplat(Op.getOperand(1));
    if (C && !C->isAllOnesValue()) {
      if (NewMask.isSubsetOf(C->getAPIntValue())) {
        // We're flipping all demanded bits. Flip the undemanded bits too.
        SDValue New = TLO.DAG.getNOT(dl, Op.getOperand(0), Op.getValueType());
        return TLO.CombineTo(Op, New);
      }
      // If we can't turn this into a 'not', try to shrink the constant.
      if (ShrinkDemandedConstant(Op, NewMask, TLO))
        return true;
    }

    Known = std::move(KnownOut);
    break;
  }
  case ISD::SELECT:
    if (SimplifyDemandedBits(Op.getOperand(2), NewMask, Known, TLO, Depth+1))
      return true;
    if (SimplifyDemandedBits(Op.getOperand(1), NewMask, Known2, TLO, Depth+1))
      return true;
    assert(!Known.hasConflict() && "Bits known to be one AND zero?");
    assert(!Known2.hasConflict() && "Bits known to be one AND zero?");

    // If the operands are constants, see if we can simplify them.
    if (ShrinkDemandedConstant(Op, NewMask, TLO))
      return true;

    // Only known if known in both the LHS and RHS.
    Known.One &= Known2.One;
    Known.Zero &= Known2.Zero;
    break;
  case ISD::SELECT_CC:
    if (SimplifyDemandedBits(Op.getOperand(3), NewMask, Known, TLO, Depth+1))
      return true;
    if (SimplifyDemandedBits(Op.getOperand(2), NewMask, Known2, TLO, Depth+1))
      return true;
    assert(!Known.hasConflict() && "Bits known to be one AND zero?");
    assert(!Known2.hasConflict() && "Bits known to be one AND zero?");

    // If the operands are constants, see if we can simplify them.
    if (ShrinkDemandedConstant(Op, NewMask, TLO))
      return true;

    // Only known if known in both the LHS and RHS.
    Known.One &= Known2.One;
    Known.Zero &= Known2.Zero;
    break;
  case ISD::SETCC: {
    SDValue Op0 = Op.getOperand(0);
    SDValue Op1 = Op.getOperand(1);
    ISD::CondCode CC = cast<CondCodeSDNode>(Op.getOperand(2))->get();
    // If (1) we only need the sign-bit, (2) the setcc operands are the same
    // width as the setcc result, and (3) the result of a setcc conforms to 0 or
    // -1, we may be able to bypass the setcc.
    if (NewMask.isSignMask() && Op0.getScalarValueSizeInBits() == BitWidth &&
        getBooleanContents(Op.getValueType()) ==
            BooleanContent::ZeroOrNegativeOneBooleanContent) {
      // If we're testing X < 0, then this compare isn't needed - just use X!
      // FIXME: We're limiting to integer types here, but this should also work
      // if we don't care about FP signed-zero. The use of SETLT with FP means
      // that we don't care about NaNs.
      if (CC == ISD::SETLT && Op1.getValueType().isInteger() &&
          (isNullConstant(Op1) || ISD::isBuildVectorAllZeros(Op1.getNode())))
        return TLO.CombineTo(Op, Op0);

      // TODO: Should we check for other forms of sign-bit comparisons?
      // Examples: X <= -1, X >= 0
    }
    if (getBooleanContents(Op0.getValueType()) ==
            TargetLowering::ZeroOrOneBooleanContent &&
        BitWidth > 1)
      Known.Zero.setBitsFrom(1);
    break;
  }
  case ISD::SHL:
    if (ConstantSDNode *SA = dyn_cast<ConstantSDNode>(Op.getOperand(1))) {
      unsigned ShAmt = SA->getZExtValue();
      SDValue InOp = Op.getOperand(0);

      // If the shift count is an invalid immediate, don't do anything.
      if (ShAmt >= BitWidth)
        break;

      // If this is ((X >>u C1) << ShAmt), see if we can simplify this into a
      // single shift.  We can do this if the bottom bits (which are shifted
      // out) are never demanded.
      if (InOp.getOpcode() == ISD::SRL &&
          isa<ConstantSDNode>(InOp.getOperand(1))) {
        if (ShAmt && (NewMask & APInt::getLowBitsSet(BitWidth, ShAmt)) == 0) {
          unsigned C1= cast<ConstantSDNode>(InOp.getOperand(1))->getZExtValue();
          unsigned Opc = ISD::SHL;
          int Diff = ShAmt-C1;
          if (Diff < 0) {
            Diff = -Diff;
            Opc = ISD::SRL;
          }

          SDValue NewSA =
            TLO.DAG.getConstant(Diff, dl, Op.getOperand(1).getValueType());
          EVT VT = Op.getValueType();
          return TLO.CombineTo(Op, TLO.DAG.getNode(Opc, dl, VT,
                                                   InOp.getOperand(0), NewSA));
        }
      }

      if (SimplifyDemandedBits(InOp, NewMask.lshr(ShAmt), Known, TLO, Depth+1))
        return true;

      // Convert (shl (anyext x, c)) to (anyext (shl x, c)) if the high bits
      // are not demanded. This will likely allow the anyext to be folded away.
      if (InOp.getNode()->getOpcode() == ISD::ANY_EXTEND) {
        SDValue InnerOp = InOp.getOperand(0);
        EVT InnerVT = InnerOp.getValueType();
        unsigned InnerBits = InnerVT.getSizeInBits();
        if (ShAmt < InnerBits && NewMask.getActiveBits() <= InnerBits &&
            isTypeDesirableForOp(ISD::SHL, InnerVT)) {
          EVT ShTy = getShiftAmountTy(InnerVT, DL);
          if (!APInt(BitWidth, ShAmt).isIntN(ShTy.getSizeInBits()))
            ShTy = InnerVT;
          SDValue NarrowShl =
            TLO.DAG.getNode(ISD::SHL, dl, InnerVT, InnerOp,
                            TLO.DAG.getConstant(ShAmt, dl, ShTy));
          return
            TLO.CombineTo(Op,
                          TLO.DAG.getNode(ISD::ANY_EXTEND, dl, Op.getValueType(),
                                          NarrowShl));
        }
        // Repeat the SHL optimization above in cases where an extension
        // intervenes: (shl (anyext (shr x, c1)), c2) to
        // (shl (anyext x), c2-c1).  This requires that the bottom c1 bits
        // aren't demanded (as above) and that the shifted upper c1 bits of
        // x aren't demanded.
        if (InOp.hasOneUse() &&
            InnerOp.getOpcode() == ISD::SRL &&
            InnerOp.hasOneUse() &&
            isa<ConstantSDNode>(InnerOp.getOperand(1))) {
          unsigned InnerShAmt = cast<ConstantSDNode>(InnerOp.getOperand(1))
            ->getZExtValue();
          if (InnerShAmt < ShAmt &&
              InnerShAmt < InnerBits &&
              NewMask.getActiveBits() <= (InnerBits - InnerShAmt + ShAmt) &&
              NewMask.countTrailingZeros() >= ShAmt) {
            SDValue NewSA =
              TLO.DAG.getConstant(ShAmt - InnerShAmt, dl,
                                  Op.getOperand(1).getValueType());
            EVT VT = Op.getValueType();
            SDValue NewExt = TLO.DAG.getNode(ISD::ANY_EXTEND, dl, VT,
                                             InnerOp.getOperand(0));
            return TLO.CombineTo(Op, TLO.DAG.getNode(ISD::SHL, dl, VT,
                                                     NewExt, NewSA));
          }
        }
      }

      Known.Zero <<= SA->getZExtValue();
      Known.One  <<= SA->getZExtValue();
      // low bits known zero.
      Known.Zero.setLowBits(SA->getZExtValue());
    }
    break;
  case ISD::SRL:
    if (ConstantSDNode *SA = dyn_cast<ConstantSDNode>(Op.getOperand(1))) {
      EVT VT = Op.getValueType();
      unsigned ShAmt = SA->getZExtValue();
      unsigned VTSize = VT.getSizeInBits();
      SDValue InOp = Op.getOperand(0);

      // If the shift count is an invalid immediate, don't do anything.
      if (ShAmt >= BitWidth)
        break;

      APInt InDemandedMask = (NewMask << ShAmt);

      // If the shift is exact, then it does demand the low bits (and knows that
      // they are zero).
      if (Op->getFlags().hasExact())
        InDemandedMask.setLowBits(ShAmt);

      // If this is ((X << C1) >>u ShAmt), see if we can simplify this into a
      // single shift.  We can do this if the top bits (which are shifted out)
      // are never demanded.
      if (InOp.getOpcode() == ISD::SHL &&
          isa<ConstantSDNode>(InOp.getOperand(1))) {
        if (ShAmt && (NewMask & APInt::getHighBitsSet(VTSize, ShAmt)) == 0) {
          unsigned C1= cast<ConstantSDNode>(InOp.getOperand(1))->getZExtValue();
          unsigned Opc = ISD::SRL;
          int Diff = ShAmt-C1;
          if (Diff < 0) {
            Diff = -Diff;
            Opc = ISD::SHL;
          }

          SDValue NewSA =
            TLO.DAG.getConstant(Diff, dl, Op.getOperand(1).getValueType());
          return TLO.CombineTo(Op, TLO.DAG.getNode(Opc, dl, VT,
                                                   InOp.getOperand(0), NewSA));
        }
      }

      // Compute the new bits that are at the top now.
      if (SimplifyDemandedBits(InOp, InDemandedMask, Known, TLO, Depth+1))
        return true;
      assert(!Known.hasConflict() && "Bits known to be one AND zero?");
      Known.Zero.lshrInPlace(ShAmt);
      Known.One.lshrInPlace(ShAmt);

      Known.Zero.setHighBits(ShAmt);  // High bits known zero.
    }
    break;
  case ISD::SRA:
    // If this is an arithmetic shift right and only the low-bit is set, we can
    // always convert this into a logical shr, even if the shift amount is
    // variable.  The low bit of the shift cannot be an input sign bit unless
    // the shift amount is >= the size of the datatype, which is undefined.
    if (NewMask.isOneValue())
      return TLO.CombineTo(Op,
                           TLO.DAG.getNode(ISD::SRL, dl, Op.getValueType(),
                                           Op.getOperand(0), Op.getOperand(1)));

    if (ConstantSDNode *SA = dyn_cast<ConstantSDNode>(Op.getOperand(1))) {
      EVT VT = Op.getValueType();
      unsigned ShAmt = SA->getZExtValue();

      // If the shift count is an invalid immediate, don't do anything.
      if (ShAmt >= BitWidth)
        break;

      APInt InDemandedMask = (NewMask << ShAmt);

      // If the shift is exact, then it does demand the low bits (and knows that
      // they are zero).
      if (Op->getFlags().hasExact())
        InDemandedMask.setLowBits(ShAmt);

      // If any of the demanded bits are produced by the sign extension, we also
      // demand the input sign bit.
      if (NewMask.countLeadingZeros() < ShAmt)
        InDemandedMask.setSignBit();

      if (SimplifyDemandedBits(Op.getOperand(0), InDemandedMask, Known, TLO,
                               Depth+1))
        return true;
      assert(!Known.hasConflict() && "Bits known to be one AND zero?");
      Known.Zero.lshrInPlace(ShAmt);
      Known.One.lshrInPlace(ShAmt);

      // If the input sign bit is known to be zero, or if none of the top bits
      // are demanded, turn this into an unsigned shift right.
      if (Known.Zero[BitWidth - ShAmt - 1] ||
          NewMask.countLeadingZeros() >= ShAmt) {
        SDNodeFlags Flags;
        Flags.setExact(Op->getFlags().hasExact());
        return TLO.CombineTo(Op,
                             TLO.DAG.getNode(ISD::SRL, dl, VT, Op.getOperand(0),
                                             Op.getOperand(1), Flags));
      }

      int Log2 = NewMask.exactLogBase2();
      if (Log2 >= 0) {
        // The bit must come from the sign.
        SDValue NewSA =
          TLO.DAG.getConstant(BitWidth - 1 - Log2, dl,
                              Op.getOperand(1).getValueType());
        return TLO.CombineTo(Op, TLO.DAG.getNode(ISD::SRL, dl, VT,
                                                 Op.getOperand(0), NewSA));
      }

      if (Known.One[BitWidth - ShAmt - 1])
        // New bits are known one.
        Known.One.setHighBits(ShAmt);
    }
    break;
  case ISD::SIGN_EXTEND_INREG: {
    EVT ExVT = cast<VTSDNode>(Op.getOperand(1))->getVT();

    APInt MsbMask = APInt::getHighBitsSet(BitWidth, 1);
    // If we only care about the highest bit, don't bother shifting right.
    if (MsbMask == NewMask) {
      unsigned ShAmt = ExVT.getScalarSizeInBits();
      SDValue InOp = Op.getOperand(0);
      unsigned VTBits = Op->getValueType(0).getScalarSizeInBits();
      bool AlreadySignExtended =
        TLO.DAG.ComputeNumSignBits(InOp) >= VTBits-ShAmt+1;
      // However if the input is already sign extended we expect the sign
      // extension to be dropped altogether later and do not simplify.
      if (!AlreadySignExtended) {
        // Compute the correct shift amount type, which must be getShiftAmountTy
        // for scalar types after legalization.
        EVT ShiftAmtTy = Op.getValueType();
        if (TLO.LegalTypes() && !ShiftAmtTy.isVector())
          ShiftAmtTy = getShiftAmountTy(ShiftAmtTy, DL);

        SDValue ShiftAmt = TLO.DAG.getConstant(BitWidth - ShAmt, dl,
                                               ShiftAmtTy);
        return TLO.CombineTo(Op, TLO.DAG.getNode(ISD::SHL, dl,
                                                 Op.getValueType(), InOp,
                                                 ShiftAmt));
      }
    }

    // Sign extension.  Compute the demanded bits in the result that are not
    // present in the input.
    APInt NewBits =
      APInt::getHighBitsSet(BitWidth,
                            BitWidth - ExVT.getScalarSizeInBits());

    // If none of the extended bits are demanded, eliminate the sextinreg.
    if ((NewBits & NewMask) == 0)
      return TLO.CombineTo(Op, Op.getOperand(0));

    APInt InSignBit =
      APInt::getSignMask(ExVT.getScalarSizeInBits()).zext(BitWidth);
    APInt InputDemandedBits =
      APInt::getLowBitsSet(BitWidth,
                           ExVT.getScalarSizeInBits()) &
      NewMask;

    // Since the sign extended bits are demanded, we know that the sign
    // bit is demanded.
    InputDemandedBits |= InSignBit;

    if (SimplifyDemandedBits(Op.getOperand(0), InputDemandedBits,
                             Known, TLO, Depth+1))
      return true;
    assert(!Known.hasConflict() && "Bits known to be one AND zero?");

    // If the sign bit of the input is known set or clear, then we know the
    // top bits of the result.

    // If the input sign bit is known zero, convert this into a zero extension.
    if (Known.Zero.intersects(InSignBit))
      return TLO.CombineTo(Op, TLO.DAG.getZeroExtendInReg(
                                   Op.getOperand(0), dl, ExVT.getScalarType()));

    if (Known.One.intersects(InSignBit)) {    // Input sign bit known set
      Known.One |= NewBits;
      Known.Zero &= ~NewBits;
    } else {                       // Input sign bit unknown
      Known.Zero &= ~NewBits;
      Known.One &= ~NewBits;
    }
    break;
  }
  case ISD::BUILD_PAIR: {
    EVT HalfVT = Op.getOperand(0).getValueType();
    unsigned HalfBitWidth = HalfVT.getScalarSizeInBits();

    APInt MaskLo = NewMask.getLoBits(HalfBitWidth).trunc(HalfBitWidth);
    APInt MaskHi = NewMask.getHiBits(HalfBitWidth).trunc(HalfBitWidth);

    KnownBits KnownLo, KnownHi;

    if (SimplifyDemandedBits(Op.getOperand(0), MaskLo, KnownLo, TLO, Depth + 1))
      return true;

    if (SimplifyDemandedBits(Op.getOperand(1), MaskHi, KnownHi, TLO, Depth + 1))
      return true;

    Known.Zero = KnownLo.Zero.zext(BitWidth) |
                KnownHi.Zero.zext(BitWidth).shl(HalfBitWidth);

    Known.One = KnownLo.One.zext(BitWidth) |
               KnownHi.One.zext(BitWidth).shl(HalfBitWidth);
    break;
  }
  case ISD::ZERO_EXTEND: {
    unsigned OperandBitWidth = Op.getOperand(0).getScalarValueSizeInBits();
    APInt InMask = NewMask.trunc(OperandBitWidth);

    // If none of the top bits are demanded, convert this into an any_extend.
    APInt NewBits =
      APInt::getHighBitsSet(BitWidth, BitWidth - OperandBitWidth) & NewMask;
    if (!NewBits.intersects(NewMask))
      return TLO.CombineTo(Op, TLO.DAG.getNode(ISD::ANY_EXTEND, dl,
                                               Op.getValueType(),
                                               Op.getOperand(0)));

    if (SimplifyDemandedBits(Op.getOperand(0), InMask, Known, TLO, Depth+1))
      return true;
    assert(!Known.hasConflict() && "Bits known to be one AND zero?");
    Known = Known.zext(BitWidth);
    Known.Zero |= NewBits;
    break;
  }
  case ISD::SIGN_EXTEND: {
    EVT InVT = Op.getOperand(0).getValueType();
    unsigned InBits = InVT.getScalarSizeInBits();
    APInt InMask    = APInt::getLowBitsSet(BitWidth, InBits);
    APInt InSignBit = APInt::getOneBitSet(BitWidth, InBits - 1);
    APInt NewBits   = ~InMask & NewMask;

    // If none of the top bits are demanded, convert this into an any_extend.
    if (NewBits == 0)
      return TLO.CombineTo(Op,TLO.DAG.getNode(ISD::ANY_EXTEND, dl,
                                              Op.getValueType(),
                                              Op.getOperand(0)));

    // Since some of the sign extended bits are demanded, we know that the sign
    // bit is demanded.
    APInt InDemandedBits = InMask & NewMask;
    InDemandedBits |= InSignBit;
    InDemandedBits = InDemandedBits.trunc(InBits);

    if (SimplifyDemandedBits(Op.getOperand(0), InDemandedBits, Known, TLO,
                             Depth+1))
      return true;
    Known = Known.zext(BitWidth);

    // If the sign bit is known zero, convert this to a zero extend.
    if (Known.Zero.intersects(InSignBit))
      return TLO.CombineTo(Op, TLO.DAG.getNode(ISD::ZERO_EXTEND, dl,
                                               Op.getValueType(),
                                               Op.getOperand(0)));

    // If the sign bit is known one, the top bits match.
    if (Known.One.intersects(InSignBit)) {
      Known.One |= NewBits;
      assert((Known.Zero & NewBits) == 0);
    } else {   // Otherwise, top bits aren't known.
      assert((Known.One & NewBits) == 0);
      assert((Known.Zero & NewBits) == 0);
    }
    break;
  }
  case ISD::ANY_EXTEND: {
    unsigned OperandBitWidth = Op.getOperand(0).getScalarValueSizeInBits();
    APInt InMask = NewMask.trunc(OperandBitWidth);
    if (SimplifyDemandedBits(Op.getOperand(0), InMask, Known, TLO, Depth+1))
      return true;
    assert(!Known.hasConflict() && "Bits known to be one AND zero?");
    Known = Known.zext(BitWidth);
    break;
  }
  case ISD::TRUNCATE: {
    // Simplify the input, using demanded bit information, and compute the known
    // zero/one bits live out.
    unsigned OperandBitWidth = Op.getOperand(0).getScalarValueSizeInBits();
    APInt TruncMask = NewMask.zext(OperandBitWidth);
    if (SimplifyDemandedBits(Op.getOperand(0), TruncMask, Known, TLO, Depth+1))
      return true;
    Known = Known.trunc(BitWidth);

    // If the input is only used by this truncate, see if we can shrink it based
    // on the known demanded bits.
    if (Op.getOperand(0).getNode()->hasOneUse()) {
      SDValue In = Op.getOperand(0);
      switch (In.getOpcode()) {
      default: break;
      case ISD::SRL:
        // Shrink SRL by a constant if none of the high bits shifted in are
        // demanded.
        if (TLO.LegalTypes() &&
            !isTypeDesirableForOp(ISD::SRL, Op.getValueType()))
          // Do not turn (vt1 truncate (vt2 srl)) into (vt1 srl) if vt1 is
          // undesirable.
          break;
        ConstantSDNode *ShAmt = dyn_cast<ConstantSDNode>(In.getOperand(1));
        if (!ShAmt)
          break;
        SDValue Shift = In.getOperand(1);
        if (TLO.LegalTypes()) {
          uint64_t ShVal = ShAmt->getZExtValue();
          Shift = TLO.DAG.getConstant(ShVal, dl,
                                      getShiftAmountTy(Op.getValueType(), DL));
        }

        if (ShAmt->getZExtValue() < BitWidth) {
          APInt HighBits = APInt::getHighBitsSet(OperandBitWidth,
                                                 OperandBitWidth - BitWidth);
          HighBits.lshrInPlace(ShAmt->getZExtValue());
          HighBits = HighBits.trunc(BitWidth);

          if (!(HighBits & NewMask)) {
            // None of the shifted in bits are needed.  Add a truncate of the
            // shift input, then shift it.
            SDValue NewTrunc = TLO.DAG.getNode(ISD::TRUNCATE, dl,
                                               Op.getValueType(),
                                               In.getOperand(0));
            return TLO.CombineTo(Op, TLO.DAG.getNode(ISD::SRL, dl,
                                                     Op.getValueType(),
                                                     NewTrunc,
                                                     Shift));
          }
        }
        break;
      }
    }

    assert(!Known.hasConflict() && "Bits known to be one AND zero?");
    break;
  }
  case ISD::AssertZext: {
    // AssertZext demands all of the high bits, plus any of the low bits
    // demanded by its users.
    EVT VT = cast<VTSDNode>(Op.getOperand(1))->getVT();
    APInt InMask = APInt::getLowBitsSet(BitWidth,
                                        VT.getSizeInBits());
    if (SimplifyDemandedBits(Op.getOperand(0), ~InMask | NewMask,
                             Known, TLO, Depth+1))
      return true;
    assert(!Known.hasConflict() && "Bits known to be one AND zero?");

    Known.Zero |= ~InMask;
    break;
  }
  case ISD::BITCAST:
    // If this is an FP->Int bitcast and if the sign bit is the only
    // thing demanded, turn this into a FGETSIGN.
    if (!TLO.LegalOperations() &&
        !Op.getValueType().isVector() &&
        !Op.getOperand(0).getValueType().isVector() &&
        NewMask == APInt::getSignMask(Op.getValueSizeInBits()) &&
        Op.getOperand(0).getValueType().isFloatingPoint()) {
      bool OpVTLegal = isOperationLegalOrCustom(ISD::FGETSIGN, Op.getValueType());
      bool i32Legal  = isOperationLegalOrCustom(ISD::FGETSIGN, MVT::i32);
      if ((OpVTLegal || i32Legal) && Op.getValueType().isSimple() &&
           Op.getOperand(0).getValueType() != MVT::f128) {
        // Cannot eliminate/lower SHL for f128 yet.
        EVT Ty = OpVTLegal ? Op.getValueType() : MVT::i32;
        // Make a FGETSIGN + SHL to move the sign bit into the appropriate
        // place.  We expect the SHL to be eliminated by other optimizations.
        SDValue Sign = TLO.DAG.getNode(ISD::FGETSIGN, dl, Ty, Op.getOperand(0));
        unsigned OpVTSizeInBits = Op.getValueSizeInBits();
        if (!OpVTLegal && OpVTSizeInBits > 32)
          Sign = TLO.DAG.getNode(ISD::ZERO_EXTEND, dl, Op.getValueType(), Sign);
        unsigned ShVal = Op.getValueSizeInBits() - 1;
        SDValue ShAmt = TLO.DAG.getConstant(ShVal, dl, Op.getValueType());
        return TLO.CombineTo(Op, TLO.DAG.getNode(ISD::SHL, dl,
                                                 Op.getValueType(),
                                                 Sign, ShAmt));
      }
    }
    break;
  case ISD::ADD:
  case ISD::MUL:
  case ISD::SUB: {
    // Add, Sub, and Mul don't demand any bits in positions beyond that
    // of the highest bit demanded of them.
    APInt LoMask = APInt::getLowBitsSet(BitWidth,
                                        BitWidth - NewMask.countLeadingZeros());
    if (SimplifyDemandedBits(Op.getOperand(0), LoMask, Known2, TLO, Depth+1) ||
        SimplifyDemandedBits(Op.getOperand(1), LoMask, Known2, TLO, Depth+1) ||
        // See if the operation should be performed at a smaller bit width.
        ShrinkDemandedOp(Op, BitWidth, NewMask, TLO)) {
      SDNodeFlags Flags = Op.getNode()->getFlags();
      if (Flags.hasNoSignedWrap() || Flags.hasNoUnsignedWrap()) {
        // Disable the nsw and nuw flags. We can no longer guarantee that we
        // won't wrap after simplification.
        Flags.setNoSignedWrap(false);
        Flags.setNoUnsignedWrap(false);
        SDValue NewOp = TLO.DAG.getNode(Op.getOpcode(), dl, Op.getValueType(),
                                        Op.getOperand(0), Op.getOperand(1),
                                        Flags);
        return TLO.CombineTo(Op, NewOp);
      }
      return true;
    }
    LLVM_FALLTHROUGH;
  }
  default:
    // Just use computeKnownBits to compute output bits.
    TLO.DAG.computeKnownBits(Op, Known, Depth);
    break;
  }

  // If we know the value of all of the demanded bits, return this as a
  // constant.
  if (NewMask.isSubsetOf(Known.Zero|Known.One)) {
    // Avoid folding to a constant if any OpaqueConstant is involved.
    const SDNode *N = Op.getNode();
    for (SDNodeIterator I = SDNodeIterator::begin(N),
         E = SDNodeIterator::end(N); I != E; ++I) {
      SDNode *Op = *I;
      if (ConstantSDNode *C = dyn_cast<ConstantSDNode>(Op))
        if (C->isOpaque())
          return false;
    }
    return TLO.CombineTo(Op,
                         TLO.DAG.getConstant(Known.One, dl, Op.getValueType()));
  }

  return false;
}

/// Determine which of the bits specified in Mask are known to be either zero or
/// one and return them in the Known.
void TargetLowering::computeKnownBitsForTargetNode(const SDValue Op,
                                                   KnownBits &Known,
                                                   const APInt &DemandedElts,
                                                   const SelectionDAG &DAG,
                                                   unsigned Depth) const {
  assert((Op.getOpcode() >= ISD::BUILTIN_OP_END ||
          Op.getOpcode() == ISD::INTRINSIC_WO_CHAIN ||
          Op.getOpcode() == ISD::INTRINSIC_W_CHAIN ||
          Op.getOpcode() == ISD::INTRINSIC_VOID) &&
         "Should use MaskedValueIsZero if you don't know whether Op"
         " is a target node!");
  Known.resetAll();
}

/// This method can be implemented by targets that want to expose additional
/// information about sign bits to the DAG Combiner.
unsigned TargetLowering::ComputeNumSignBitsForTargetNode(SDValue Op,
                                                         const APInt &,
                                                         const SelectionDAG &,
                                                         unsigned Depth) const {
  assert((Op.getOpcode() >= ISD::BUILTIN_OP_END ||
          Op.getOpcode() == ISD::INTRINSIC_WO_CHAIN ||
          Op.getOpcode() == ISD::INTRINSIC_W_CHAIN ||
          Op.getOpcode() == ISD::INTRINSIC_VOID) &&
         "Should use ComputeNumSignBits if you don't know whether Op"
         " is a target node!");
  return 1;
}

// FIXME: Ideally, this would use ISD::isConstantSplatVector(), but that must
// work with truncating build vectors and vectors with elements of less than
// 8 bits.
bool TargetLowering::isConstTrueVal(const SDNode *N) const {
  if (!N)
    return false;

  APInt CVal;
  if (auto *CN = dyn_cast<ConstantSDNode>(N)) {
    CVal = CN->getAPIntValue();
  } else if (auto *BV = dyn_cast<BuildVectorSDNode>(N)) {
    auto *CN = BV->getConstantSplatNode();
    if (!CN)
      return false;

    // If this is a truncating build vector, truncate the splat value.
    // Otherwise, we may fail to match the expected values below.
    unsigned BVEltWidth = BV->getValueType(0).getScalarSizeInBits();
    CVal = CN->getAPIntValue();
    if (BVEltWidth < CVal.getBitWidth())
      CVal = CVal.trunc(BVEltWidth);
  } else {
    return false;
  }

  switch (getBooleanContents(N->getValueType(0))) {
  case UndefinedBooleanContent:
    return CVal[0];
  case ZeroOrOneBooleanContent:
    return CVal.isOneValue();
  case ZeroOrNegativeOneBooleanContent:
    return CVal.isAllOnesValue();
  }

  llvm_unreachable("Invalid boolean contents");
}

SDValue TargetLowering::getConstTrueVal(SelectionDAG &DAG, EVT VT,
                                        const SDLoc &DL) const {
  unsigned ElementWidth = VT.getScalarSizeInBits();
  APInt TrueInt =
      getBooleanContents(VT) == TargetLowering::ZeroOrOneBooleanContent
          ? APInt(ElementWidth, 1)
          : APInt::getAllOnesValue(ElementWidth);
  return DAG.getConstant(TrueInt, DL, VT);
}

bool TargetLowering::isConstFalseVal(const SDNode *N) const {
  if (!N)
    return false;

  const ConstantSDNode *CN = dyn_cast<ConstantSDNode>(N);
  if (!CN) {
    const BuildVectorSDNode *BV = dyn_cast<BuildVectorSDNode>(N);
    if (!BV)
      return false;

    // Only interested in constant splats, we don't care about undef
    // elements in identifying boolean constants and getConstantSplatNode
    // returns NULL if all ops are undef;
    CN = BV->getConstantSplatNode();
    if (!CN)
      return false;
  }

  if (getBooleanContents(N->getValueType(0)) == UndefinedBooleanContent)
    return !CN->getAPIntValue()[0];

  return CN->isNullValue();
}

bool TargetLowering::isExtendedTrueVal(const ConstantSDNode *N, EVT VT,
                                       bool SExt) const {
  if (VT == MVT::i1)
    return N->isOne();

  TargetLowering::BooleanContent Cnt = getBooleanContents(VT);
  switch (Cnt) {
  case TargetLowering::ZeroOrOneBooleanContent:
    // An extended value of 1 is always true, unless its original type is i1,
    // in which case it will be sign extended to -1.
    return (N->isOne() && !SExt) || (SExt && (N->getValueType(0) != MVT::i1));
  case TargetLowering::UndefinedBooleanContent:
  case TargetLowering::ZeroOrNegativeOneBooleanContent:
    return N->isAllOnesValue() && SExt;
  }
  llvm_unreachable("Unexpected enumeration.");
}

/// This helper function of SimplifySetCC tries to optimize the comparison when
/// either operand of the SetCC node is a bitwise-and instruction.
SDValue TargetLowering::simplifySetCCWithAnd(EVT VT, SDValue N0, SDValue N1,
                                             ISD::CondCode Cond,
                                             DAGCombinerInfo &DCI,
                                             const SDLoc &DL) const {
  // Match these patterns in any of their permutations:
  // (X & Y) == Y
  // (X & Y) != Y
  if (N1.getOpcode() == ISD::AND && N0.getOpcode() != ISD::AND)
    std::swap(N0, N1);

  EVT OpVT = N0.getValueType();
  if (N0.getOpcode() != ISD::AND || !OpVT.isInteger() ||
      (Cond != ISD::SETEQ && Cond != ISD::SETNE))
    return SDValue();

  SDValue X, Y;
  if (N0.getOperand(0) == N1) {
    X = N0.getOperand(1);
    Y = N0.getOperand(0);
  } else if (N0.getOperand(1) == N1) {
    X = N0.getOperand(0);
    Y = N0.getOperand(1);
  } else {
    return SDValue();
  }

  SelectionDAG &DAG = DCI.DAG;
  SDValue Zero = DAG.getConstant(0, DL, OpVT);
  if (DAG.isKnownToBeAPowerOfTwo(Y)) {
    // Simplify X & Y == Y to X & Y != 0 if Y has exactly one bit set.
    // Note that where Y is variable and is known to have at most one bit set
    // (for example, if it is Z & 1) we cannot do this; the expressions are not
    // equivalent when Y == 0.
    Cond = ISD::getSetCCInverse(Cond, /*isInteger=*/true);
    if (DCI.isBeforeLegalizeOps() ||
        isCondCodeLegal(Cond, N0.getSimpleValueType()))
      return DAG.getSetCC(DL, VT, N0, Zero, Cond);
  } else if (N0.hasOneUse() && hasAndNotCompare(Y)) {
    // If the target supports an 'and-not' or 'and-complement' logic operation,
    // try to use that to make a comparison operation more efficient.
    // But don't do this transform if the mask is a single bit because there are
    // more efficient ways to deal with that case (for example, 'bt' on x86 or
    // 'rlwinm' on PPC).

    // Bail out if the compare operand that we want to turn into a zero is
    // already a zero (otherwise, infinite loop).
    auto *YConst = dyn_cast<ConstantSDNode>(Y);
    if (YConst && YConst->isNullValue())
      return SDValue();

    // Transform this into: ~X & Y == 0.
    SDValue NotX = DAG.getNOT(SDLoc(X), X, OpVT);
    SDValue NewAnd = DAG.getNode(ISD::AND, SDLoc(N0), OpVT, NotX, Y);
    return DAG.getSetCC(DL, VT, NewAnd, Zero, Cond);
  }

  return SDValue();
}

/// Try to simplify a setcc built with the specified operands and cc. If it is
/// unable to simplify it, return a null SDValue.
SDValue TargetLowering::SimplifySetCC(EVT VT, SDValue N0, SDValue N1,
                                      ISD::CondCode Cond, bool foldBooleans,
                                      DAGCombinerInfo &DCI,
                                      const SDLoc &dl) const {
  SelectionDAG &DAG = DCI.DAG;

  // These setcc operations always fold.
  switch (Cond) {
  default: break;
  case ISD::SETFALSE:
  case ISD::SETFALSE2: return DAG.getConstant(0, dl, VT);
  case ISD::SETTRUE:
  case ISD::SETTRUE2: {
    TargetLowering::BooleanContent Cnt =
        getBooleanContents(N0->getValueType(0));
    return DAG.getConstant(
        Cnt == TargetLowering::ZeroOrNegativeOneBooleanContent ? -1ULL : 1, dl,
        VT);
  }
  }

  // Ensure that the constant occurs on the RHS and fold constant comparisons.
  ISD::CondCode SwappedCC = ISD::getSetCCSwappedOperands(Cond);
  if (isa<ConstantSDNode>(N0.getNode()) &&
      (DCI.isBeforeLegalizeOps() ||
       isCondCodeLegal(SwappedCC, N0.getSimpleValueType())))
    return DAG.getSetCC(dl, VT, N1, N0, SwappedCC);

  if (auto *N1C = dyn_cast<ConstantSDNode>(N1.getNode())) {
    const APInt &C1 = N1C->getAPIntValue();

    // If the LHS is '(srl (ctlz x), 5)', the RHS is 0/1, and this is an
    // equality comparison, then we're just comparing whether X itself is
    // zero.
    if (N0.getOpcode() == ISD::SRL && (C1.isNullValue() || C1.isOneValue()) &&
        N0.getOperand(0).getOpcode() == ISD::CTLZ &&
        N0.getOperand(1).getOpcode() == ISD::Constant) {
      const APInt &ShAmt
        = cast<ConstantSDNode>(N0.getOperand(1))->getAPIntValue();
      if ((Cond == ISD::SETEQ || Cond == ISD::SETNE) &&
          ShAmt == Log2_32(N0.getValueSizeInBits())) {
        if ((C1 == 0) == (Cond == ISD::SETEQ)) {
          // (srl (ctlz x), 5) == 0  -> X != 0
          // (srl (ctlz x), 5) != 1  -> X != 0
          Cond = ISD::SETNE;
        } else {
          // (srl (ctlz x), 5) != 0  -> X == 0
          // (srl (ctlz x), 5) == 1  -> X == 0
          Cond = ISD::SETEQ;
        }
        SDValue Zero = DAG.getConstant(0, dl, N0.getValueType());
        return DAG.getSetCC(dl, VT, N0.getOperand(0).getOperand(0),
                            Zero, Cond);
      }
    }

    SDValue CTPOP = N0;
    // Look through truncs that don't change the value of a ctpop.
    if (N0.hasOneUse() && N0.getOpcode() == ISD::TRUNCATE)
      CTPOP = N0.getOperand(0);

    if (CTPOP.hasOneUse() && CTPOP.getOpcode() == ISD::CTPOP &&
        (N0 == CTPOP ||
         N0.getValueSizeInBits() > Log2_32_Ceil(CTPOP.getValueSizeInBits()))) {
      EVT CTVT = CTPOP.getValueType();
      SDValue CTOp = CTPOP.getOperand(0);

      // (ctpop x) u< 2 -> (x & x-1) == 0
      // (ctpop x) u> 1 -> (x & x-1) != 0
      if ((Cond == ISD::SETULT && C1 == 2) || (Cond == ISD::SETUGT && C1 == 1)){
        SDValue Sub = DAG.getNode(ISD::SUB, dl, CTVT, CTOp,
                                  DAG.getConstant(1, dl, CTVT));
        SDValue And = DAG.getNode(ISD::AND, dl, CTVT, CTOp, Sub);
        ISD::CondCode CC = Cond == ISD::SETULT ? ISD::SETEQ : ISD::SETNE;
        return DAG.getSetCC(dl, VT, And, DAG.getConstant(0, dl, CTVT), CC);
      }

      // TODO: (ctpop x) == 1 -> x && (x & x-1) == 0 iff ctpop is illegal.
    }

    // (zext x) == C --> x == (trunc C)
    // (sext x) == C --> x == (trunc C)
    if ((Cond == ISD::SETEQ || Cond == ISD::SETNE) &&
        DCI.isBeforeLegalize() && N0->hasOneUse()) {
      unsigned MinBits = N0.getValueSizeInBits();
      SDValue PreExt;
      bool Signed = false;
      if (N0->getOpcode() == ISD::ZERO_EXTEND) {
        // ZExt
        MinBits = N0->getOperand(0).getValueSizeInBits();
        PreExt = N0->getOperand(0);
      } else if (N0->getOpcode() == ISD::AND) {
        // DAGCombine turns costly ZExts into ANDs
        if (auto *C = dyn_cast<ConstantSDNode>(N0->getOperand(1)))
          if ((C->getAPIntValue()+1).isPowerOf2()) {
            MinBits = C->getAPIntValue().countTrailingOnes();
            PreExt = N0->getOperand(0);
          }
      } else if (N0->getOpcode() == ISD::SIGN_EXTEND) {
        // SExt
        MinBits = N0->getOperand(0).getValueSizeInBits();
        PreExt = N0->getOperand(0);
        Signed = true;
      } else if (auto *LN0 = dyn_cast<LoadSDNode>(N0)) {
        // ZEXTLOAD / SEXTLOAD
        if (LN0->getExtensionType() == ISD::ZEXTLOAD) {
          MinBits = LN0->getMemoryVT().getSizeInBits();
          PreExt = N0;
        } else if (LN0->getExtensionType() == ISD::SEXTLOAD) {
          Signed = true;
          MinBits = LN0->getMemoryVT().getSizeInBits();
          PreExt = N0;
        }
      }

      // Figure out how many bits we need to preserve this constant.
      unsigned ReqdBits = Signed ?
        C1.getBitWidth() - C1.getNumSignBits() + 1 :
        C1.getActiveBits();

      // Make sure we're not losing bits from the constant.
      if (MinBits > 0 &&
          MinBits < C1.getBitWidth() &&
          MinBits >= ReqdBits) {
        EVT MinVT = EVT::getIntegerVT(*DAG.getContext(), MinBits);
        if (isTypeDesirableForOp(ISD::SETCC, MinVT)) {
          // Will get folded away.
          SDValue Trunc = DAG.getNode(ISD::TRUNCATE, dl, MinVT, PreExt);
          if (MinBits == 1 && C1 == 1)
            // Invert the condition.
            return DAG.getSetCC(dl, VT, Trunc, DAG.getConstant(0, dl, MVT::i1),
                                Cond == ISD::SETEQ ? ISD::SETNE : ISD::SETEQ);
          SDValue C = DAG.getConstant(C1.trunc(MinBits), dl, MinVT);
          return DAG.getSetCC(dl, VT, Trunc, C, Cond);
        }

        // If truncating the setcc operands is not desirable, we can still
        // simplify the expression in some cases:
        // setcc ([sz]ext (setcc x, y, cc)), 0, setne) -> setcc (x, y, cc)
        // setcc ([sz]ext (setcc x, y, cc)), 0, seteq) -> setcc (x, y, inv(cc))
        // setcc (zext (setcc x, y, cc)), 1, setne) -> setcc (x, y, inv(cc))
        // setcc (zext (setcc x, y, cc)), 1, seteq) -> setcc (x, y, cc)
        // setcc (sext (setcc x, y, cc)), -1, setne) -> setcc (x, y, inv(cc))
        // setcc (sext (setcc x, y, cc)), -1, seteq) -> setcc (x, y, cc)
        SDValue TopSetCC = N0->getOperand(0);
        unsigned N0Opc = N0->getOpcode();
        bool SExt = (N0Opc == ISD::SIGN_EXTEND);
        if (TopSetCC.getValueType() == MVT::i1 && VT == MVT::i1 &&
            TopSetCC.getOpcode() == ISD::SETCC &&
            (N0Opc == ISD::ZERO_EXTEND || N0Opc == ISD::SIGN_EXTEND) &&
            (isConstFalseVal(N1C) ||
             isExtendedTrueVal(N1C, N0->getValueType(0), SExt))) {

          bool Inverse = (N1C->isNullValue() && Cond == ISD::SETEQ) ||
                         (!N1C->isNullValue() && Cond == ISD::SETNE);

          if (!Inverse)
            return TopSetCC;

          ISD::CondCode InvCond = ISD::getSetCCInverse(
              cast<CondCodeSDNode>(TopSetCC.getOperand(2))->get(),
              TopSetCC.getOperand(0).getValueType().isInteger());
          return DAG.getSetCC(dl, VT, TopSetCC.getOperand(0),
                                      TopSetCC.getOperand(1),
                                      InvCond);
        }
      }
    }

    // If the LHS is '(and load, const)', the RHS is 0, the test is for
    // equality or unsigned, and all 1 bits of the const are in the same
    // partial word, see if we can shorten the load.
    if (DCI.isBeforeLegalize() &&
        !ISD::isSignedIntSetCC(Cond) &&
        N0.getOpcode() == ISD::AND && C1 == 0 &&
        N0.getNode()->hasOneUse() &&
        isa<LoadSDNode>(N0.getOperand(0)) &&
        N0.getOperand(0).getNode()->hasOneUse() &&
        isa<ConstantSDNode>(N0.getOperand(1))) {
      LoadSDNode *Lod = cast<LoadSDNode>(N0.getOperand(0));
      APInt bestMask;
      unsigned bestWidth = 0, bestOffset = 0;
      if (!Lod->isVolatile() && Lod->isUnindexed()) {
        unsigned origWidth = N0.getValueSizeInBits();
        unsigned maskWidth = origWidth;
        // We can narrow (e.g.) 16-bit extending loads on 32-bit target to
        // 8 bits, but have to be careful...
        if (Lod->getExtensionType() != ISD::NON_EXTLOAD)
          origWidth = Lod->getMemoryVT().getSizeInBits();
        const APInt &Mask =
          cast<ConstantSDNode>(N0.getOperand(1))->getAPIntValue();
        for (unsigned width = origWidth / 2; width>=8; width /= 2) {
          APInt newMask = APInt::getLowBitsSet(maskWidth, width);
          for (unsigned offset=0; offset<origWidth/width; offset++) {
            if (Mask.isSubsetOf(newMask)) {
              if (DAG.getDataLayout().isLittleEndian())
                bestOffset = (uint64_t)offset * (width/8);
              else
                bestOffset = (origWidth/width - offset - 1) * (width/8);
              bestMask = Mask.lshr(offset * (width/8) * 8);
              bestWidth = width;
              break;
            }
            newMask <<= width;
          }
        }
      }
      if (bestWidth) {
        EVT newVT = EVT::getIntegerVT(*DAG.getContext(), bestWidth);
        if (newVT.isRound()) {
          EVT PtrType = Lod->getOperand(1).getValueType();
          SDValue Ptr = Lod->getBasePtr();
          if (bestOffset != 0)
            Ptr = DAG.getNode(ISD::ADD, dl, PtrType, Lod->getBasePtr(),
                              DAG.getConstant(bestOffset, dl, PtrType));
          unsigned NewAlign = MinAlign(Lod->getAlignment(), bestOffset);
          SDValue NewLoad = DAG.getLoad(
              newVT, dl, Lod->getChain(), Ptr,
              Lod->getPointerInfo().getWithOffset(bestOffset), NewAlign);
          return DAG.getSetCC(dl, VT,
                              DAG.getNode(ISD::AND, dl, newVT, NewLoad,
                                      DAG.getConstant(bestMask.trunc(bestWidth),
                                                      dl, newVT)),
                              DAG.getConstant(0LL, dl, newVT), Cond);
        }
      }
    }

    // If the LHS is a ZERO_EXTEND, perform the comparison on the input.
    if (N0.getOpcode() == ISD::ZERO_EXTEND) {
      unsigned InSize = N0.getOperand(0).getValueSizeInBits();

      // If the comparison constant has bits in the upper part, the
      // zero-extended value could never match.
      if (C1.intersects(APInt::getHighBitsSet(C1.getBitWidth(),
                                              C1.getBitWidth() - InSize))) {
        switch (Cond) {
        case ISD::SETUGT:
        case ISD::SETUGE:
        case ISD::SETEQ:
          return DAG.getConstant(0, dl, VT);
        case ISD::SETULT:
        case ISD::SETULE:
        case ISD::SETNE:
          return DAG.getConstant(1, dl, VT);
        case ISD::SETGT:
        case ISD::SETGE:
          // True if the sign bit of C1 is set.
          return DAG.getConstant(C1.isNegative(), dl, VT);
        case ISD::SETLT:
        case ISD::SETLE:
          // True if the sign bit of C1 isn't set.
          return DAG.getConstant(C1.isNonNegative(), dl, VT);
        default:
          break;
        }
      }

      // Otherwise, we can perform the comparison with the low bits.
      switch (Cond) {
      case ISD::SETEQ:
      case ISD::SETNE:
      case ISD::SETUGT:
      case ISD::SETUGE:
      case ISD::SETULT:
      case ISD::SETULE: {
        EVT newVT = N0.getOperand(0).getValueType();
        if (DCI.isBeforeLegalizeOps() ||
            (isOperationLegal(ISD::SETCC, newVT) &&
             getCondCodeAction(Cond, newVT.getSimpleVT()) == Legal)) {
          EVT NewSetCCVT =
              getSetCCResultType(DAG.getDataLayout(), *DAG.getContext(), newVT);
          SDValue NewConst = DAG.getConstant(C1.trunc(InSize), dl, newVT);

          SDValue NewSetCC = DAG.getSetCC(dl, NewSetCCVT, N0.getOperand(0),
                                          NewConst, Cond);
          return DAG.getBoolExtOrTrunc(NewSetCC, dl, VT, N0.getValueType());
        }
        break;
      }
      default:
        break;   // todo, be more careful with signed comparisons
      }
    } else if (N0.getOpcode() == ISD::SIGN_EXTEND_INREG &&
               (Cond == ISD::SETEQ || Cond == ISD::SETNE)) {
      EVT ExtSrcTy = cast<VTSDNode>(N0.getOperand(1))->getVT();
      unsigned ExtSrcTyBits = ExtSrcTy.getSizeInBits();
      EVT ExtDstTy = N0.getValueType();
      unsigned ExtDstTyBits = ExtDstTy.getSizeInBits();

      // If the constant doesn't fit into the number of bits for the source of
      // the sign extension, it is impossible for both sides to be equal.
      if (C1.getMinSignedBits() > ExtSrcTyBits)
        return DAG.getConstant(Cond == ISD::SETNE, dl, VT);

      SDValue ZextOp;
      EVT Op0Ty = N0.getOperand(0).getValueType();
      if (Op0Ty == ExtSrcTy) {
        ZextOp = N0.getOperand(0);
      } else {
        APInt Imm = APInt::getLowBitsSet(ExtDstTyBits, ExtSrcTyBits);
        ZextOp = DAG.getNode(ISD::AND, dl, Op0Ty, N0.getOperand(0),
                              DAG.getConstant(Imm, dl, Op0Ty));
      }
      if (!DCI.isCalledByLegalizer())
        DCI.AddToWorklist(ZextOp.getNode());
      // Otherwise, make this a use of a zext.
      return DAG.getSetCC(dl, VT, ZextOp,
                          DAG.getConstant(C1 & APInt::getLowBitsSet(
                                                              ExtDstTyBits,
                                                              ExtSrcTyBits),
                                          dl, ExtDstTy),
                          Cond);
    } else if ((N1C->isNullValue() || N1C->isOne()) &&
                (Cond == ISD::SETEQ || Cond == ISD::SETNE)) {
      // SETCC (SETCC), [0|1], [EQ|NE]  -> SETCC
      if (N0.getOpcode() == ISD::SETCC &&
          isTypeLegal(VT) && VT.bitsLE(N0.getValueType())) {
        bool TrueWhenTrue = (Cond == ISD::SETEQ) ^ (!N1C->isOne());
        if (TrueWhenTrue)
          return DAG.getNode(ISD::TRUNCATE, dl, VT, N0);
        // Invert the condition.
        ISD::CondCode CC = cast<CondCodeSDNode>(N0.getOperand(2))->get();
        CC = ISD::getSetCCInverse(CC,
                                  N0.getOperand(0).getValueType().isInteger());
        if (DCI.isBeforeLegalizeOps() ||
            isCondCodeLegal(CC, N0.getOperand(0).getSimpleValueType()))
          return DAG.getSetCC(dl, VT, N0.getOperand(0), N0.getOperand(1), CC);
      }

      if ((N0.getOpcode() == ISD::XOR ||
           (N0.getOpcode() == ISD::AND &&
            N0.getOperand(0).getOpcode() == ISD::XOR &&
            N0.getOperand(1) == N0.getOperand(0).getOperand(1))) &&
          isa<ConstantSDNode>(N0.getOperand(1)) &&
          cast<ConstantSDNode>(N0.getOperand(1))->isOne()) {
        // If this is (X^1) == 0/1, swap the RHS and eliminate the xor.  We
        // can only do this if the top bits are known zero.
        unsigned BitWidth = N0.getValueSizeInBits();
        if (DAG.MaskedValueIsZero(N0,
                                  APInt::getHighBitsSet(BitWidth,
                                                        BitWidth-1))) {
          // Okay, get the un-inverted input value.
          SDValue Val;
          if (N0.getOpcode() == ISD::XOR) {
            Val = N0.getOperand(0);
          } else {
            assert(N0.getOpcode() == ISD::AND &&
                    N0.getOperand(0).getOpcode() == ISD::XOR);
            // ((X^1)&1)^1 -> X & 1
            Val = DAG.getNode(ISD::AND, dl, N0.getValueType(),
                              N0.getOperand(0).getOperand(0),
                              N0.getOperand(1));
          }

          return DAG.getSetCC(dl, VT, Val, N1,
                              Cond == ISD::SETEQ ? ISD::SETNE : ISD::SETEQ);
        }
      } else if (N1C->isOne() &&
                 (VT == MVT::i1 ||
                  getBooleanContents(N0->getValueType(0)) ==
                      ZeroOrOneBooleanContent)) {
        SDValue Op0 = N0;
        if (Op0.getOpcode() == ISD::TRUNCATE)
          Op0 = Op0.getOperand(0);

        if ((Op0.getOpcode() == ISD::XOR) &&
            Op0.getOperand(0).getOpcode() == ISD::SETCC &&
            Op0.getOperand(1).getOpcode() == ISD::SETCC) {
          // (xor (setcc), (setcc)) == / != 1 -> (setcc) != / == (setcc)
          Cond = (Cond == ISD::SETEQ) ? ISD::SETNE : ISD::SETEQ;
          return DAG.getSetCC(dl, VT, Op0.getOperand(0), Op0.getOperand(1),
                              Cond);
        }
        if (Op0.getOpcode() == ISD::AND &&
            isa<ConstantSDNode>(Op0.getOperand(1)) &&
            cast<ConstantSDNode>(Op0.getOperand(1))->isOne()) {
          // If this is (X&1) == / != 1, normalize it to (X&1) != / == 0.
          if (Op0.getValueType().bitsGT(VT))
            Op0 = DAG.getNode(ISD::AND, dl, VT,
                          DAG.getNode(ISD::TRUNCATE, dl, VT, Op0.getOperand(0)),
                          DAG.getConstant(1, dl, VT));
          else if (Op0.getValueType().bitsLT(VT))
            Op0 = DAG.getNode(ISD::AND, dl, VT,
                        DAG.getNode(ISD::ANY_EXTEND, dl, VT, Op0.getOperand(0)),
                        DAG.getConstant(1, dl, VT));

          return DAG.getSetCC(dl, VT, Op0,
                              DAG.getConstant(0, dl, Op0.getValueType()),
                              Cond == ISD::SETEQ ? ISD::SETNE : ISD::SETEQ);
        }
        if (Op0.getOpcode() == ISD::AssertZext &&
            cast<VTSDNode>(Op0.getOperand(1))->getVT() == MVT::i1)
          return DAG.getSetCC(dl, VT, Op0,
                              DAG.getConstant(0, dl, Op0.getValueType()),
                              Cond == ISD::SETEQ ? ISD::SETNE : ISD::SETEQ);
      }
    }

    APInt MinVal, MaxVal;
    unsigned OperandBitSize = N1C->getValueType(0).getSizeInBits();
    if (ISD::isSignedIntSetCC(Cond)) {
      MinVal = APInt::getSignedMinValue(OperandBitSize);
      MaxVal = APInt::getSignedMaxValue(OperandBitSize);
    } else {
      MinVal = APInt::getMinValue(OperandBitSize);
      MaxVal = APInt::getMaxValue(OperandBitSize);
    }

    // Canonicalize GE/LE comparisons to use GT/LT comparisons.
    if (Cond == ISD::SETGE || Cond == ISD::SETUGE) {
      // X >= MIN --> true
      if (C1 == MinVal)
        return DAG.getConstant(1, dl, VT);

      // X >= C0 --> X > (C0 - 1)
      APInt C = C1 - 1;
      ISD::CondCode NewCC = (Cond == ISD::SETGE) ? ISD::SETGT : ISD::SETUGT;
      if ((DCI.isBeforeLegalizeOps() ||
           isCondCodeLegal(NewCC, VT.getSimpleVT())) &&
          (!N1C->isOpaque() || (N1C->isOpaque() && C.getBitWidth() <= 64 &&
                                isLegalICmpImmediate(C.getSExtValue())))) {
        return DAG.getSetCC(dl, VT, N0,
                            DAG.getConstant(C, dl, N1.getValueType()),
                            NewCC);
      }
    }

    if (Cond == ISD::SETLE || Cond == ISD::SETULE) {
      // X <= MAX --> true
      if (C1 == MaxVal)
          return DAG.getConstant(1, dl, VT);

      // X <= C0 --> X < (C0 + 1)
      APInt C = C1 + 1;
      ISD::CondCode NewCC = (Cond == ISD::SETLE) ? ISD::SETLT : ISD::SETULT;
      if ((DCI.isBeforeLegalizeOps() ||
           isCondCodeLegal(NewCC, VT.getSimpleVT())) &&
          (!N1C->isOpaque() || (N1C->isOpaque() && C.getBitWidth() <= 64 &&
                                isLegalICmpImmediate(C.getSExtValue())))) {
        return DAG.getSetCC(dl, VT, N0,
                            DAG.getConstant(C, dl, N1.getValueType()),
                            NewCC);
      }
    }

    if ((Cond == ISD::SETLT || Cond == ISD::SETULT) && C1 == MinVal)
      return DAG.getConstant(0, dl, VT);      // X < MIN --> false
    if ((Cond == ISD::SETGE || Cond == ISD::SETUGE) && C1 == MinVal)
      return DAG.getConstant(1, dl, VT);      // X >= MIN --> true
    if ((Cond == ISD::SETGT || Cond == ISD::SETUGT) && C1 == MaxVal)
      return DAG.getConstant(0, dl, VT);      // X > MAX --> false
    if ((Cond == ISD::SETLE || Cond == ISD::SETULE) && C1 == MaxVal)
      return DAG.getConstant(1, dl, VT);      // X <= MAX --> true

    // Canonicalize setgt X, Min --> setne X, Min
    if ((Cond == ISD::SETGT || Cond == ISD::SETUGT) && C1 == MinVal)
      return DAG.getSetCC(dl, VT, N0, N1, ISD::SETNE);
    // Canonicalize setlt X, Max --> setne X, Max
    if ((Cond == ISD::SETLT || Cond == ISD::SETULT) && C1 == MaxVal)
      return DAG.getSetCC(dl, VT, N0, N1, ISD::SETNE);

    // If we have setult X, 1, turn it into seteq X, 0
    if ((Cond == ISD::SETLT || Cond == ISD::SETULT) && C1 == MinVal+1)
      return DAG.getSetCC(dl, VT, N0,
                          DAG.getConstant(MinVal, dl, N0.getValueType()),
                          ISD::SETEQ);
    // If we have setugt X, Max-1, turn it into seteq X, Max
    if ((Cond == ISD::SETGT || Cond == ISD::SETUGT) && C1 == MaxVal-1)
      return DAG.getSetCC(dl, VT, N0,
                          DAG.getConstant(MaxVal, dl, N0.getValueType()),
                          ISD::SETEQ);

    // If we have "setcc X, C0", check to see if we can shrink the immediate
    // by changing cc.

    // SETUGT X, SINTMAX  -> SETLT X, 0
    if (Cond == ISD::SETUGT &&
        C1 == APInt::getSignedMaxValue(OperandBitSize))
      return DAG.getSetCC(dl, VT, N0,
                          DAG.getConstant(0, dl, N1.getValueType()),
                          ISD::SETLT);

    // SETULT X, SINTMIN  -> SETGT X, -1
    if (Cond == ISD::SETULT &&
        C1 == APInt::getSignedMinValue(OperandBitSize)) {
      SDValue ConstMinusOne =
          DAG.getConstant(APInt::getAllOnesValue(OperandBitSize), dl,
                          N1.getValueType());
      return DAG.getSetCC(dl, VT, N0, ConstMinusOne, ISD::SETGT);
    }

    // Fold bit comparisons when we can.
    if ((Cond == ISD::SETEQ || Cond == ISD::SETNE) &&
        (VT == N0.getValueType() ||
         (isTypeLegal(VT) && VT.bitsLE(N0.getValueType()))) &&
        N0.getOpcode() == ISD::AND) {
      auto &DL = DAG.getDataLayout();
      if (auto *AndRHS = dyn_cast<ConstantSDNode>(N0.getOperand(1))) {
        EVT ShiftTy = DCI.isBeforeLegalize()
                          ? getPointerTy(DL)
                          : getShiftAmountTy(N0.getValueType(), DL);
        if (Cond == ISD::SETNE && C1 == 0) {// (X & 8) != 0  -->  (X & 8) >> 3
          // Perform the xform if the AND RHS is a single bit.
          if (AndRHS->getAPIntValue().isPowerOf2()) {
            return DAG.getNode(ISD::TRUNCATE, dl, VT,
                              DAG.getNode(ISD::SRL, dl, N0.getValueType(), N0,
                   DAG.getConstant(AndRHS->getAPIntValue().logBase2(), dl,
                                   ShiftTy)));
          }
        } else if (Cond == ISD::SETEQ && C1 == AndRHS->getAPIntValue()) {
          // (X & 8) == 8  -->  (X & 8) >> 3
          // Perform the xform if C1 is a single bit.
          if (C1.isPowerOf2()) {
            return DAG.getNode(ISD::TRUNCATE, dl, VT,
                               DAG.getNode(ISD::SRL, dl, N0.getValueType(), N0,
                                      DAG.getConstant(C1.logBase2(), dl,
                                                      ShiftTy)));
          }
        }
      }
    }

    if (C1.getMinSignedBits() <= 64 &&
        !isLegalICmpImmediate(C1.getSExtValue())) {
      // (X & -256) == 256 -> (X >> 8) == 1
      if ((Cond == ISD::SETEQ || Cond == ISD::SETNE) &&
          N0.getOpcode() == ISD::AND && N0.hasOneUse()) {
        if (auto *AndRHS = dyn_cast<ConstantSDNode>(N0.getOperand(1))) {
          const APInt &AndRHSC = AndRHS->getAPIntValue();
          if ((-AndRHSC).isPowerOf2() && (AndRHSC & C1) == C1) {
            unsigned ShiftBits = AndRHSC.countTrailingZeros();
            auto &DL = DAG.getDataLayout();
            EVT ShiftTy = DCI.isBeforeLegalize()
                              ? getPointerTy(DL)
                              : getShiftAmountTy(N0.getValueType(), DL);
            EVT CmpTy = N0.getValueType();
            SDValue Shift = DAG.getNode(ISD::SRL, dl, CmpTy, N0.getOperand(0),
                                        DAG.getConstant(ShiftBits, dl,
                                                        ShiftTy));
            SDValue CmpRHS = DAG.getConstant(C1.lshr(ShiftBits), dl, CmpTy);
            return DAG.getSetCC(dl, VT, Shift, CmpRHS, Cond);
          }
        }
      } else if (Cond == ISD::SETULT || Cond == ISD::SETUGE ||
                 Cond == ISD::SETULE || Cond == ISD::SETUGT) {
        bool AdjOne = (Cond == ISD::SETULE || Cond == ISD::SETUGT);
        // X <  0x100000000 -> (X >> 32) <  1
        // X >= 0x100000000 -> (X >> 32) >= 1
        // X <= 0x0ffffffff -> (X >> 32) <  1
        // X >  0x0ffffffff -> (X >> 32) >= 1
        unsigned ShiftBits;
        APInt NewC = C1;
        ISD::CondCode NewCond = Cond;
        if (AdjOne) {
          ShiftBits = C1.countTrailingOnes();
          NewC = NewC + 1;
          NewCond = (Cond == ISD::SETULE) ? ISD::SETULT : ISD::SETUGE;
        } else {
          ShiftBits = C1.countTrailingZeros();
        }
        NewC.lshrInPlace(ShiftBits);
        if (ShiftBits && NewC.getMinSignedBits() <= 64 &&
          isLegalICmpImmediate(NewC.getSExtValue())) {
          auto &DL = DAG.getDataLayout();
          EVT ShiftTy = DCI.isBeforeLegalize()
                            ? getPointerTy(DL)
                            : getShiftAmountTy(N0.getValueType(), DL);
          EVT CmpTy = N0.getValueType();
          SDValue Shift = DAG.getNode(ISD::SRL, dl, CmpTy, N0,
                                      DAG.getConstant(ShiftBits, dl, ShiftTy));
          SDValue CmpRHS = DAG.getConstant(NewC, dl, CmpTy);
          return DAG.getSetCC(dl, VT, Shift, CmpRHS, NewCond);
        }
      }
    }
  }

  if (isa<ConstantFPSDNode>(N0.getNode())) {
    // Constant fold or commute setcc.
    SDValue O = DAG.FoldSetCC(VT, N0, N1, Cond, dl);
    if (O.getNode()) return O;
  } else if (auto *CFP = dyn_cast<ConstantFPSDNode>(N1.getNode())) {
    // If the RHS of an FP comparison is a constant, simplify it away in
    // some cases.
    if (CFP->getValueAPF().isNaN()) {
      // If an operand is known to be a nan, we can fold it.
      switch (ISD::getUnorderedFlavor(Cond)) {
      default: llvm_unreachable("Unknown flavor!");
      case 0:  // Known false.
        return DAG.getConstant(0, dl, VT);
      case 1:  // Known true.
        return DAG.getConstant(1, dl, VT);
      case 2:  // Undefined.
        return DAG.getUNDEF(VT);
      }
    }

    // Otherwise, we know the RHS is not a NaN.  Simplify the node to drop the
    // constant if knowing that the operand is non-nan is enough.  We prefer to
    // have SETO(x,x) instead of SETO(x, 0.0) because this avoids having to
    // materialize 0.0.
    if (Cond == ISD::SETO || Cond == ISD::SETUO)
      return DAG.getSetCC(dl, VT, N0, N0, Cond);

    // setcc (fneg x), C -> setcc swap(pred) x, -C
    if (N0.getOpcode() == ISD::FNEG) {
      ISD::CondCode SwapCond = ISD::getSetCCSwappedOperands(Cond);
      if (DCI.isBeforeLegalizeOps() ||
          isCondCodeLegal(SwapCond, N0.getSimpleValueType())) {
        SDValue NegN1 = DAG.getNode(ISD::FNEG, dl, N0.getValueType(), N1);
        return DAG.getSetCC(dl, VT, N0.getOperand(0), NegN1, SwapCond);
      }
    }

    // If the condition is not legal, see if we can find an equivalent one
    // which is legal.
    if (!isCondCodeLegal(Cond, N0.getSimpleValueType())) {
      // If the comparison was an awkward floating-point == or != and one of
      // the comparison operands is infinity or negative infinity, convert the
      // condition to a less-awkward <= or >=.
      if (CFP->getValueAPF().isInfinity()) {
        if (CFP->getValueAPF().isNegative()) {
          if (Cond == ISD::SETOEQ &&
              isCondCodeLegal(ISD::SETOLE, N0.getSimpleValueType()))
            return DAG.getSetCC(dl, VT, N0, N1, ISD::SETOLE);
          if (Cond == ISD::SETUEQ &&
              isCondCodeLegal(ISD::SETOLE, N0.getSimpleValueType()))
            return DAG.getSetCC(dl, VT, N0, N1, ISD::SETULE);
          if (Cond == ISD::SETUNE &&
              isCondCodeLegal(ISD::SETUGT, N0.getSimpleValueType()))
            return DAG.getSetCC(dl, VT, N0, N1, ISD::SETUGT);
          if (Cond == ISD::SETONE &&
              isCondCodeLegal(ISD::SETUGT, N0.getSimpleValueType()))
            return DAG.getSetCC(dl, VT, N0, N1, ISD::SETOGT);
        } else {
          if (Cond == ISD::SETOEQ &&
              isCondCodeLegal(ISD::SETOGE, N0.getSimpleValueType()))
            return DAG.getSetCC(dl, VT, N0, N1, ISD::SETOGE);
          if (Cond == ISD::SETUEQ &&
              isCondCodeLegal(ISD::SETOGE, N0.getSimpleValueType()))
            return DAG.getSetCC(dl, VT, N0, N1, ISD::SETUGE);
          if (Cond == ISD::SETUNE &&
              isCondCodeLegal(ISD::SETULT, N0.getSimpleValueType()))
            return DAG.getSetCC(dl, VT, N0, N1, ISD::SETULT);
          if (Cond == ISD::SETONE &&
              isCondCodeLegal(ISD::SETULT, N0.getSimpleValueType()))
            return DAG.getSetCC(dl, VT, N0, N1, ISD::SETOLT);
        }
      }
    }
  }

  if (N0 == N1) {
    // The sext(setcc()) => setcc() optimization relies on the appropriate
    // constant being emitted.
    uint64_t EqVal = 0;
    switch (getBooleanContents(N0.getValueType())) {
    case UndefinedBooleanContent:
    case ZeroOrOneBooleanContent:
      EqVal = ISD::isTrueWhenEqual(Cond);
      break;
    case ZeroOrNegativeOneBooleanContent:
      EqVal = ISD::isTrueWhenEqual(Cond) ? -1 : 0;
      break;
    }

    // We can always fold X == X for integer setcc's.
    if (N0.getValueType().isInteger()) {
      return DAG.getConstant(EqVal, dl, VT);
    }
    unsigned UOF = ISD::getUnorderedFlavor(Cond);
    if (UOF == 2)   // FP operators that are undefined on NaNs.
      return DAG.getConstant(EqVal, dl, VT);
    if (UOF == unsigned(ISD::isTrueWhenEqual(Cond)))
      return DAG.getConstant(EqVal, dl, VT);
    // Otherwise, we can't fold it.  However, we can simplify it to SETUO/SETO
    // if it is not already.
    ISD::CondCode NewCond = UOF == 0 ? ISD::SETO : ISD::SETUO;
    if (NewCond != Cond && (DCI.isBeforeLegalizeOps() ||
          getCondCodeAction(NewCond, N0.getSimpleValueType()) == Legal))
      return DAG.getSetCC(dl, VT, N0, N1, NewCond);
  }

  if ((Cond == ISD::SETEQ || Cond == ISD::SETNE) &&
      N0.getValueType().isInteger()) {
    if (N0.getOpcode() == ISD::ADD || N0.getOpcode() == ISD::SUB ||
        N0.getOpcode() == ISD::XOR) {
      // Simplify (X+Y) == (X+Z) -->  Y == Z
      if (N0.getOpcode() == N1.getOpcode()) {
        if (N0.getOperand(0) == N1.getOperand(0))
          return DAG.getSetCC(dl, VT, N0.getOperand(1), N1.getOperand(1), Cond);
        if (N0.getOperand(1) == N1.getOperand(1))
          return DAG.getSetCC(dl, VT, N0.getOperand(0), N1.getOperand(0), Cond);
        if (isCommutativeBinOp(N0.getOpcode())) {
          // If X op Y == Y op X, try other combinations.
          if (N0.getOperand(0) == N1.getOperand(1))
            return DAG.getSetCC(dl, VT, N0.getOperand(1), N1.getOperand(0),
                                Cond);
          if (N0.getOperand(1) == N1.getOperand(0))
            return DAG.getSetCC(dl, VT, N0.getOperand(0), N1.getOperand(1),
                                Cond);
        }
      }

      // If RHS is a legal immediate value for a compare instruction, we need
      // to be careful about increasing register pressure needlessly.
      bool LegalRHSImm = false;

      if (auto *RHSC = dyn_cast<ConstantSDNode>(N1)) {
        if (auto *LHSR = dyn_cast<ConstantSDNode>(N0.getOperand(1))) {
          // Turn (X+C1) == C2 --> X == C2-C1
          if (N0.getOpcode() == ISD::ADD && N0.getNode()->hasOneUse()) {
            return DAG.getSetCC(dl, VT, N0.getOperand(0),
                                DAG.getConstant(RHSC->getAPIntValue()-
                                                LHSR->getAPIntValue(),
                                dl, N0.getValueType()), Cond);
          }

          // Turn (X^C1) == C2 into X == C1^C2 iff X&~C1 = 0.
          if (N0.getOpcode() == ISD::XOR)
            // If we know that all of the inverted bits are zero, don't bother
            // performing the inversion.
            if (DAG.MaskedValueIsZero(N0.getOperand(0), ~LHSR->getAPIntValue()))
              return
                DAG.getSetCC(dl, VT, N0.getOperand(0),
                             DAG.getConstant(LHSR->getAPIntValue() ^
                                               RHSC->getAPIntValue(),
                                             dl, N0.getValueType()),
                             Cond);
        }

        // Turn (C1-X) == C2 --> X == C1-C2
        if (auto *SUBC = dyn_cast<ConstantSDNode>(N0.getOperand(0))) {
          if (N0.getOpcode() == ISD::SUB && N0.getNode()->hasOneUse()) {
            return
              DAG.getSetCC(dl, VT, N0.getOperand(1),
                           DAG.getConstant(SUBC->getAPIntValue() -
                                             RHSC->getAPIntValue(),
                                           dl, N0.getValueType()),
                           Cond);
          }
        }

        // Could RHSC fold directly into a compare?
        if (RHSC->getValueType(0).getSizeInBits() <= 64)
          LegalRHSImm = isLegalICmpImmediate(RHSC->getSExtValue());
      }

      // Simplify (X+Z) == X -->  Z == 0
      // Don't do this if X is an immediate that can fold into a cmp
      // instruction and X+Z has other uses. It could be an induction variable
      // chain, and the transform would increase register pressure.
      if (!LegalRHSImm || N0.getNode()->hasOneUse()) {
        if (N0.getOperand(0) == N1)
          return DAG.getSetCC(dl, VT, N0.getOperand(1),
                              DAG.getConstant(0, dl, N0.getValueType()), Cond);
        if (N0.getOperand(1) == N1) {
          if (isCommutativeBinOp(N0.getOpcode()))
            return DAG.getSetCC(dl, VT, N0.getOperand(0),
                                DAG.getConstant(0, dl, N0.getValueType()),
                                Cond);
          if (N0.getNode()->hasOneUse()) {
            assert(N0.getOpcode() == ISD::SUB && "Unexpected operation!");
            auto &DL = DAG.getDataLayout();
            // (Z-X) == X  --> Z == X<<1
            SDValue SH = DAG.getNode(
                ISD::SHL, dl, N1.getValueType(), N1,
                DAG.getConstant(1, dl,
                                getShiftAmountTy(N1.getValueType(), DL)));
            if (!DCI.isCalledByLegalizer())
              DCI.AddToWorklist(SH.getNode());
            return DAG.getSetCC(dl, VT, N0.getOperand(0), SH, Cond);
          }
        }
      }
    }

    if (N1.getOpcode() == ISD::ADD || N1.getOpcode() == ISD::SUB ||
        N1.getOpcode() == ISD::XOR) {
      // Simplify  X == (X+Z) -->  Z == 0
      if (N1.getOperand(0) == N0)
        return DAG.getSetCC(dl, VT, N1.getOperand(1),
                        DAG.getConstant(0, dl, N1.getValueType()), Cond);
      if (N1.getOperand(1) == N0) {
        if (isCommutativeBinOp(N1.getOpcode()))
          return DAG.getSetCC(dl, VT, N1.getOperand(0),
                          DAG.getConstant(0, dl, N1.getValueType()), Cond);
        if (N1.getNode()->hasOneUse()) {
          assert(N1.getOpcode() == ISD::SUB && "Unexpected operation!");
          auto &DL = DAG.getDataLayout();
          // X == (Z-X)  --> X<<1 == Z
          SDValue SH = DAG.getNode(
              ISD::SHL, dl, N1.getValueType(), N0,
              DAG.getConstant(1, dl, getShiftAmountTy(N0.getValueType(), DL)));
          if (!DCI.isCalledByLegalizer())
            DCI.AddToWorklist(SH.getNode());
          return DAG.getSetCC(dl, VT, SH, N1.getOperand(0), Cond);
        }
      }
    }

    if (SDValue V = simplifySetCCWithAnd(VT, N0, N1, Cond, DCI, dl))
      return V;
  }

  // Fold away ALL boolean setcc's.
  SDValue Temp;
  if (N0.getValueType() == MVT::i1 && foldBooleans) {
    switch (Cond) {
    default: llvm_unreachable("Unknown integer setcc!");
    case ISD::SETEQ:  // X == Y  -> ~(X^Y)
      Temp = DAG.getNode(ISD::XOR, dl, MVT::i1, N0, N1);
      N0 = DAG.getNOT(dl, Temp, MVT::i1);
      if (!DCI.isCalledByLegalizer())
        DCI.AddToWorklist(Temp.getNode());
      break;
    case ISD::SETNE:  // X != Y   -->  (X^Y)
      N0 = DAG.getNode(ISD::XOR, dl, MVT::i1, N0, N1);
      break;
    case ISD::SETGT:  // X >s Y   -->  X == 0 & Y == 1  -->  ~X & Y
    case ISD::SETULT: // X <u Y   -->  X == 0 & Y == 1  -->  ~X & Y
      Temp = DAG.getNOT(dl, N0, MVT::i1);
      N0 = DAG.getNode(ISD::AND, dl, MVT::i1, N1, Temp);
      if (!DCI.isCalledByLegalizer())
        DCI.AddToWorklist(Temp.getNode());
      break;
    case ISD::SETLT:  // X <s Y   --> X == 1 & Y == 0  -->  ~Y & X
    case ISD::SETUGT: // X >u Y   --> X == 1 & Y == 0  -->  ~Y & X
      Temp = DAG.getNOT(dl, N1, MVT::i1);
      N0 = DAG.getNode(ISD::AND, dl, MVT::i1, N0, Temp);
      if (!DCI.isCalledByLegalizer())
        DCI.AddToWorklist(Temp.getNode());
      break;
    case ISD::SETULE: // X <=u Y  --> X == 0 | Y == 1  -->  ~X | Y
    case ISD::SETGE:  // X >=s Y  --> X == 0 | Y == 1  -->  ~X | Y
      Temp = DAG.getNOT(dl, N0, MVT::i1);
      N0 = DAG.getNode(ISD::OR, dl, MVT::i1, N1, Temp);
      if (!DCI.isCalledByLegalizer())
        DCI.AddToWorklist(Temp.getNode());
      break;
    case ISD::SETUGE: // X >=u Y  --> X == 1 | Y == 0  -->  ~Y | X
    case ISD::SETLE:  // X <=s Y  --> X == 1 | Y == 0  -->  ~Y | X
      Temp = DAG.getNOT(dl, N1, MVT::i1);
      N0 = DAG.getNode(ISD::OR, dl, MVT::i1, N0, Temp);
      break;
    }
    if (VT != MVT::i1) {
      if (!DCI.isCalledByLegalizer())
        DCI.AddToWorklist(N0.getNode());
      // FIXME: If running after legalize, we probably can't do this.
      N0 = DAG.getNode(ISD::ZERO_EXTEND, dl, VT, N0);
    }
    return N0;
  }

  // Could not fold it.
  return SDValue();
}

/// Returns true (and the GlobalValue and the offset) if the node is a
/// GlobalAddress + offset.
bool TargetLowering::isGAPlusOffset(SDNode *N, const GlobalValue *&GA,
                                    int64_t &Offset) const {
  if (auto *GASD = dyn_cast<GlobalAddressSDNode>(N)) {
    GA = GASD->getGlobal();
    Offset += GASD->getOffset();
    return true;
  }

  if (N->getOpcode() == ISD::ADD) {
    SDValue N1 = N->getOperand(0);
    SDValue N2 = N->getOperand(1);
    if (isGAPlusOffset(N1.getNode(), GA, Offset)) {
      if (auto *V = dyn_cast<ConstantSDNode>(N2)) {
        Offset += V->getSExtValue();
        return true;
      }
    } else if (isGAPlusOffset(N2.getNode(), GA, Offset)) {
      if (auto *V = dyn_cast<ConstantSDNode>(N1)) {
        Offset += V->getSExtValue();
        return true;
      }
    }
  }

  return false;
}

SDValue TargetLowering::PerformDAGCombine(SDNode *N,
                                          DAGCombinerInfo &DCI) const {
  // Default implementation: no optimization.
  return SDValue();
}

//===----------------------------------------------------------------------===//
//  Inline Assembler Implementation Methods
//===----------------------------------------------------------------------===//

TargetLowering::ConstraintType
TargetLowering::getConstraintType(StringRef Constraint) const {
  unsigned S = Constraint.size();

  if (S == 1) {
    switch (Constraint[0]) {
    default: break;
    case 'r': return C_RegisterClass;
    case 'm':    // memory
    case 'o':    // offsetable
    case 'V':    // not offsetable
      return C_Memory;
    case 'i':    // Simple Integer or Relocatable Constant
    case 'n':    // Simple Integer
    case 'E':    // Floating Point Constant
    case 'F':    // Floating Point Constant
    case 's':    // Relocatable Constant
    case 'p':    // Address.
    case 'X':    // Allow ANY value.
    case 'I':    // Target registers.
    case 'J':
    case 'K':
    case 'L':
    case 'M':
    case 'N':
    case 'O':
    case 'P':
    case '<':
    case '>':
      return C_Other;
    }
  }

  if (S > 1 && Constraint[0] == '{' && Constraint[S-1] == '}') {
    if (S == 8 && Constraint.substr(1, 6) == "memory") // "{memory}"
      return C_Memory;
    return C_Register;
  }
  return C_Unknown;
}

/// Try to replace an X constraint, which matches anything, with another that
/// has more specific requirements based on the type of the corresponding
/// operand.
const char *TargetLowering::LowerXConstraint(EVT ConstraintVT) const{
  if (ConstraintVT.isInteger())
    return "r";
  if (ConstraintVT.isFloatingPoint())
    return "f";      // works for many targets
  return nullptr;
}

/// Lower the specified operand into the Ops vector.
/// If it is invalid, don't add anything to Ops.
void TargetLowering::LowerAsmOperandForConstraint(SDValue Op,
                                                  std::string &Constraint,
                                                  std::vector<SDValue> &Ops,
                                                  SelectionDAG &DAG) const {

  if (Constraint.length() > 1) return;

  char ConstraintLetter = Constraint[0];
  switch (ConstraintLetter) {
  default: break;
  case 'X':     // Allows any operand; labels (basic block) use this.
    if (Op.getOpcode() == ISD::BasicBlock) {
      Ops.push_back(Op);
      return;
    }
    LLVM_FALLTHROUGH;
  case 'i':    // Simple Integer or Relocatable Constant
  case 'n':    // Simple Integer
  case 's': {  // Relocatable Constant
    // These operands are interested in values of the form (GV+C), where C may
    // be folded in as an offset of GV, or it may be explicitly added.  Also, it
    // is possible and fine if either GV or C are missing.
    ConstantSDNode *C = dyn_cast<ConstantSDNode>(Op);
    GlobalAddressSDNode *GA = dyn_cast<GlobalAddressSDNode>(Op);

    // If we have "(add GV, C)", pull out GV/C
    if (Op.getOpcode() == ISD::ADD) {
      C = dyn_cast<ConstantSDNode>(Op.getOperand(1));
      GA = dyn_cast<GlobalAddressSDNode>(Op.getOperand(0));
      if (!C || !GA) {
        C = dyn_cast<ConstantSDNode>(Op.getOperand(0));
        GA = dyn_cast<GlobalAddressSDNode>(Op.getOperand(1));
      }
      if (!C || !GA) {
        C = nullptr;
        GA = nullptr;
      }
    }

    // If we find a valid operand, map to the TargetXXX version so that the
    // value itself doesn't get selected.
    if (GA) {   // Either &GV   or   &GV+C
      if (ConstraintLetter != 'n') {
        int64_t Offs = GA->getOffset();
        if (C) Offs += C->getZExtValue();
        Ops.push_back(DAG.getTargetGlobalAddress(GA->getGlobal(),
                                                 C ? SDLoc(C) : SDLoc(),
                                                 Op.getValueType(), Offs));
      }
      return;
    }
    if (C) {   // just C, no GV.
      // Simple constants are not allowed for 's'.
      if (ConstraintLetter != 's') {
        // gcc prints these as sign extended.  Sign extend value to 64 bits
        // now; without this it would get ZExt'd later in
        // ScheduleDAGSDNodes::EmitNode, which is very generic.
        Ops.push_back(DAG.getTargetConstant(C->getSExtValue(),
                                            SDLoc(C), MVT::i64));
      }
      return;
    }
    break;
  }
  }
}

std::pair<unsigned, const TargetRegisterClass *>
TargetLowering::getRegForInlineAsmConstraint(const TargetRegisterInfo *RI,
                                             StringRef Constraint,
                                             MVT VT) const {
  if (Constraint.empty() || Constraint[0] != '{')
    return std::make_pair(0u, static_cast<TargetRegisterClass*>(nullptr));
  assert(*(Constraint.end()-1) == '}' && "Not a brace enclosed constraint?");

  // Remove the braces from around the name.
  StringRef RegName(Constraint.data()+1, Constraint.size()-2);

  std::pair<unsigned, const TargetRegisterClass*> R =
    std::make_pair(0u, static_cast<const TargetRegisterClass*>(nullptr));

  // Figure out which register class contains this reg.
  for (const TargetRegisterClass *RC : RI->regclasses()) {
    // If none of the value types for this register class are valid, we
    // can't use it.  For example, 64-bit reg classes on 32-bit targets.
    if (!isLegalRC(*RI, *RC))
      continue;

    for (TargetRegisterClass::iterator I = RC->begin(), E = RC->end();
         I != E; ++I) {
      if (RegName.equals_lower(RI->getRegAsmName(*I))) {
        std::pair<unsigned, const TargetRegisterClass*> S =
          std::make_pair(*I, RC);

        // If this register class has the requested value type, return it,
        // otherwise keep searching and return the first class found
        // if no other is found which explicitly has the requested type.
        if (RI->isTypeLegalForClass(*RC, VT))
          return S;
        if (!R.second)
          R = S;
      }
    }
  }

  return R;
}

//===----------------------------------------------------------------------===//
// Constraint Selection.

/// Return true of this is an input operand that is a matching constraint like
/// "4".
bool TargetLowering::AsmOperandInfo::isMatchingInputConstraint() const {
  assert(!ConstraintCode.empty() && "No known constraint!");
  return isdigit(static_cast<unsigned char>(ConstraintCode[0]));
}

/// If this is an input matching constraint, this method returns the output
/// operand it matches.
unsigned TargetLowering::AsmOperandInfo::getMatchedOperand() const {
  assert(!ConstraintCode.empty() && "No known constraint!");
  return atoi(ConstraintCode.c_str());
}

/// Split up the constraint string from the inline assembly value into the
/// specific constraints and their prefixes, and also tie in the associated
/// operand values.
/// If this returns an empty vector, and if the constraint string itself
/// isn't empty, there was an error parsing.
TargetLowering::AsmOperandInfoVector
TargetLowering::ParseConstraints(const DataLayout &DL,
                                 const TargetRegisterInfo *TRI,
                                 ImmutableCallSite CS) const {
  /// Information about all of the constraints.
  AsmOperandInfoVector ConstraintOperands;
  const InlineAsm *IA = cast<InlineAsm>(CS.getCalledValue());
  unsigned maCount = 0; // Largest number of multiple alternative constraints.

  // Do a prepass over the constraints, canonicalizing them, and building up the
  // ConstraintOperands list.
  unsigned ArgNo = 0;   // ArgNo - The argument of the CallInst.
  unsigned ResNo = 0;   // ResNo - The result number of the next output.

  for (InlineAsm::ConstraintInfo &CI : IA->ParseConstraints()) {
    ConstraintOperands.emplace_back(std::move(CI));
    AsmOperandInfo &OpInfo = ConstraintOperands.back();

    // Update multiple alternative constraint count.
    if (OpInfo.multipleAlternatives.size() > maCount)
      maCount = OpInfo.multipleAlternatives.size();

    OpInfo.ConstraintVT = MVT::Other;

    // Compute the value type for each operand.
    switch (OpInfo.Type) {
    case InlineAsm::isOutput:
      // Indirect outputs just consume an argument.
      if (OpInfo.isIndirect) {
        OpInfo.CallOperandVal = const_cast<Value *>(CS.getArgument(ArgNo++));
        break;
      }

      // The return value of the call is this value.  As such, there is no
      // corresponding argument.
      assert(!CS.getType()->isVoidTy() &&
             "Bad inline asm!");
      if (StructType *STy = dyn_cast<StructType>(CS.getType())) {
        OpInfo.ConstraintVT =
            getSimpleValueType(DL, STy->getElementType(ResNo));
      } else {
        assert(ResNo == 0 && "Asm only has one result!");
        OpInfo.ConstraintVT = getSimpleValueType(DL, CS.getType());
      }
      ++ResNo;
      break;
    case InlineAsm::isInput:
      OpInfo.CallOperandVal = const_cast<Value *>(CS.getArgument(ArgNo++));
      break;
    case InlineAsm::isClobber:
      // Nothing to do.
      break;
    }

    if (OpInfo.CallOperandVal) {
      llvm::Type *OpTy = OpInfo.CallOperandVal->getType();
      if (OpInfo.isIndirect) {
        llvm::PointerType *PtrTy = dyn_cast<PointerType>(OpTy);
        if (!PtrTy)
          report_fatal_error("Indirect operand for inline asm not a pointer!");
        OpTy = PtrTy->getElementType();
      }

      // Look for vector wrapped in a struct. e.g. { <16 x i8> }.
      if (StructType *STy = dyn_cast<StructType>(OpTy))
        if (STy->getNumElements() == 1)
          OpTy = STy->getElementType(0);

      // If OpTy is not a single value, it may be a struct/union that we
      // can tile with integers.
      if (!OpTy->isSingleValueType() && OpTy->isSized()) {
        unsigned BitSize = DL.getTypeSizeInBits(OpTy);
        switch (BitSize) {
        default: break;
        case 1:
        case 8:
        case 16:
        case 32:
        case 64:
        case 128:
          OpInfo.ConstraintVT =
            MVT::getVT(IntegerType::get(OpTy->getContext(), BitSize), true);
          break;
        }
      } else if (PointerType *PT = dyn_cast<PointerType>(OpTy)) {
        unsigned PtrSize = DL.getPointerSizeInBits(PT->getAddressSpace());
        OpInfo.ConstraintVT = MVT::getIntegerVT(PtrSize);
      } else {
        OpInfo.ConstraintVT = MVT::getVT(OpTy, true);
      }
    }
  }

  // If we have multiple alternative constraints, select the best alternative.
  if (!ConstraintOperands.empty()) {
    if (maCount) {
      unsigned bestMAIndex = 0;
      int bestWeight = -1;
      // weight:  -1 = invalid match, and 0 = so-so match to 5 = good match.
      int weight = -1;
      unsigned maIndex;
      // Compute the sums of the weights for each alternative, keeping track
      // of the best (highest weight) one so far.
      for (maIndex = 0; maIndex < maCount; ++maIndex) {
        int weightSum = 0;
        for (unsigned cIndex = 0, eIndex = ConstraintOperands.size();
            cIndex != eIndex; ++cIndex) {
          AsmOperandInfo& OpInfo = ConstraintOperands[cIndex];
          if (OpInfo.Type == InlineAsm::isClobber)
            continue;

          // If this is an output operand with a matching input operand,
          // look up the matching input. If their types mismatch, e.g. one
          // is an integer, the other is floating point, or their sizes are
          // different, flag it as an maCantMatch.
          if (OpInfo.hasMatchingInput()) {
            AsmOperandInfo &Input = ConstraintOperands[OpInfo.MatchingInput];
            if (OpInfo.ConstraintVT != Input.ConstraintVT) {
              if ((OpInfo.ConstraintVT.isInteger() !=
                   Input.ConstraintVT.isInteger()) ||
                  (OpInfo.ConstraintVT.getSizeInBits() !=
                   Input.ConstraintVT.getSizeInBits())) {
                weightSum = -1;  // Can't match.
                break;
              }
            }
          }
          weight = getMultipleConstraintMatchWeight(OpInfo, maIndex);
          if (weight == -1) {
            weightSum = -1;
            break;
          }
          weightSum += weight;
        }
        // Update best.
        if (weightSum > bestWeight) {
          bestWeight = weightSum;
          bestMAIndex = maIndex;
        }
      }

      // Now select chosen alternative in each constraint.
      for (unsigned cIndex = 0, eIndex = ConstraintOperands.size();
          cIndex != eIndex; ++cIndex) {
        AsmOperandInfo& cInfo = ConstraintOperands[cIndex];
        if (cInfo.Type == InlineAsm::isClobber)
          continue;
        cInfo.selectAlternative(bestMAIndex);
      }
    }
  }

  // Check and hook up tied operands, choose constraint code to use.
  for (unsigned cIndex = 0, eIndex = ConstraintOperands.size();
      cIndex != eIndex; ++cIndex) {
    AsmOperandInfo& OpInfo = ConstraintOperands[cIndex];

    // If this is an output operand with a matching input operand, look up the
    // matching input. If their types mismatch, e.g. one is an integer, the
    // other is floating point, or their sizes are different, flag it as an
    // error.
    if (OpInfo.hasMatchingInput()) {
      AsmOperandInfo &Input = ConstraintOperands[OpInfo.MatchingInput];

      if (OpInfo.ConstraintVT != Input.ConstraintVT) {
        std::pair<unsigned, const TargetRegisterClass *> MatchRC =
            getRegForInlineAsmConstraint(TRI, OpInfo.ConstraintCode,
                                         OpInfo.ConstraintVT);
        std::pair<unsigned, const TargetRegisterClass *> InputRC =
            getRegForInlineAsmConstraint(TRI, Input.ConstraintCode,
                                         Input.ConstraintVT);
        if ((OpInfo.ConstraintVT.isInteger() !=
             Input.ConstraintVT.isInteger()) ||
            (MatchRC.second != InputRC.second)) {
          report_fatal_error("Unsupported asm: input constraint"
                             " with a matching output constraint of"
                             " incompatible type!");
        }
      }
    }
  }

  return ConstraintOperands;
}

/// Return an integer indicating how general CT is.
static unsigned getConstraintGenerality(TargetLowering::ConstraintType CT) {
  switch (CT) {
  case TargetLowering::C_Other:
  case TargetLowering::C_Unknown:
    return 0;
  case TargetLowering::C_Register:
    return 1;
  case TargetLowering::C_RegisterClass:
    return 2;
  case TargetLowering::C_Memory:
    return 3;
  }
  llvm_unreachable("Invalid constraint type");
}

/// Examine constraint type and operand type and determine a weight value.
/// This object must already have been set up with the operand type
/// and the current alternative constraint selected.
TargetLowering::ConstraintWeight
  TargetLowering::getMultipleConstraintMatchWeight(
    AsmOperandInfo &info, int maIndex) const {
  InlineAsm::ConstraintCodeVector *rCodes;
  if (maIndex >= (int)info.multipleAlternatives.size())
    rCodes = &info.Codes;
  else
    rCodes = &info.multipleAlternatives[maIndex].Codes;
  ConstraintWeight BestWeight = CW_Invalid;

  // Loop over the options, keeping track of the most general one.
  for (unsigned i = 0, e = rCodes->size(); i != e; ++i) {
    ConstraintWeight weight =
      getSingleConstraintMatchWeight(info, (*rCodes)[i].c_str());
    if (weight > BestWeight)
      BestWeight = weight;
  }

  return BestWeight;
}

/// Examine constraint type and operand type and determine a weight value.
/// This object must already have been set up with the operand type
/// and the current alternative constraint selected.
TargetLowering::ConstraintWeight
  TargetLowering::getSingleConstraintMatchWeight(
    AsmOperandInfo &info, const char *constraint) const {
  ConstraintWeight weight = CW_Invalid;
  Value *CallOperandVal = info.CallOperandVal;
    // If we don't have a value, we can't do a match,
    // but allow it at the lowest weight.
  if (!CallOperandVal)
    return CW_Default;
  // Look at the constraint type.
  switch (*constraint) {
    case 'i': // immediate integer.
    case 'n': // immediate integer with a known value.
      if (isa<ConstantInt>(CallOperandVal))
        weight = CW_Constant;
      break;
    case 's': // non-explicit intregal immediate.
      if (isa<GlobalValue>(CallOperandVal))
        weight = CW_Constant;
      break;
    case 'E': // immediate float if host format.
    case 'F': // immediate float.
      if (isa<ConstantFP>(CallOperandVal))
        weight = CW_Constant;
      break;
    case '<': // memory operand with autodecrement.
    case '>': // memory operand with autoincrement.
    case 'm': // memory operand.
    case 'o': // offsettable memory operand
    case 'V': // non-offsettable memory operand
      weight = CW_Memory;
      break;
    case 'r': // general register.
    case 'g': // general register, memory operand or immediate integer.
              // note: Clang converts "g" to "imr".
      if (CallOperandVal->getType()->isIntegerTy())
        weight = CW_Register;
      break;
    case 'X': // any operand.
    default:
      weight = CW_Default;
      break;
  }
  return weight;
}

/// If there are multiple different constraints that we could pick for this
/// operand (e.g. "imr") try to pick the 'best' one.
/// This is somewhat tricky: constraints fall into four classes:
///    Other         -> immediates and magic values
///    Register      -> one specific register
///    RegisterClass -> a group of regs
///    Memory        -> memory
/// Ideally, we would pick the most specific constraint possible: if we have
/// something that fits into a register, we would pick it.  The problem here
/// is that if we have something that could either be in a register or in
/// memory that use of the register could cause selection of *other*
/// operands to fail: they might only succeed if we pick memory.  Because of
/// this the heuristic we use is:
///
///  1) If there is an 'other' constraint, and if the operand is valid for
///     that constraint, use it.  This makes us take advantage of 'i'
///     constraints when available.
///  2) Otherwise, pick the most general constraint present.  This prefers
///     'm' over 'r', for example.
///
static void ChooseConstraint(TargetLowering::AsmOperandInfo &OpInfo,
                             const TargetLowering &TLI,
                             SDValue Op, SelectionDAG *DAG) {
  assert(OpInfo.Codes.size() > 1 && "Doesn't have multiple constraint options");
  unsigned BestIdx = 0;
  TargetLowering::ConstraintType BestType = TargetLowering::C_Unknown;
  int BestGenerality = -1;

  // Loop over the options, keeping track of the most general one.
  for (unsigned i = 0, e = OpInfo.Codes.size(); i != e; ++i) {
    TargetLowering::ConstraintType CType =
      TLI.getConstraintType(OpInfo.Codes[i]);

    // If this is an 'other' constraint, see if the operand is valid for it.
    // For example, on X86 we might have an 'rI' constraint.  If the operand
    // is an integer in the range [0..31] we want to use I (saving a load
    // of a register), otherwise we must use 'r'.
    if (CType == TargetLowering::C_Other && Op.getNode()) {
      assert(OpInfo.Codes[i].size() == 1 &&
             "Unhandled multi-letter 'other' constraint");
      std::vector<SDValue> ResultOps;
      TLI.LowerAsmOperandForConstraint(Op, OpInfo.Codes[i],
                                       ResultOps, *DAG);
      if (!ResultOps.empty()) {
        BestType = CType;
        BestIdx = i;
        break;
      }
    }

    // Things with matching constraints can only be registers, per gcc
    // documentation.  This mainly affects "g" constraints.
    if (CType == TargetLowering::C_Memory && OpInfo.hasMatchingInput())
      continue;

    // This constraint letter is more general than the previous one, use it.
    int Generality = getConstraintGenerality(CType);
    if (Generality > BestGenerality) {
      BestType = CType;
      BestIdx = i;
      BestGenerality = Generality;
    }
  }

  OpInfo.ConstraintCode = OpInfo.Codes[BestIdx];
  OpInfo.ConstraintType = BestType;
}

/// Determines the constraint code and constraint type to use for the specific
/// AsmOperandInfo, setting OpInfo.ConstraintCode and OpInfo.ConstraintType.
void TargetLowering::ComputeConstraintToUse(AsmOperandInfo &OpInfo,
                                            SDValue Op,
                                            SelectionDAG *DAG) const {
  assert(!OpInfo.Codes.empty() && "Must have at least one constraint");

  // Single-letter constraints ('r') are very common.
  if (OpInfo.Codes.size() == 1) {
    OpInfo.ConstraintCode = OpInfo.Codes[0];
    OpInfo.ConstraintType = getConstraintType(OpInfo.ConstraintCode);
  } else {
    ChooseConstraint(OpInfo, *this, Op, DAG);
  }

  // 'X' matches anything.
  if (OpInfo.ConstraintCode == "X" && OpInfo.CallOperandVal) {
    // Labels and constants are handled elsewhere ('X' is the only thing
    // that matches labels).  For Functions, the type here is the type of
    // the result, which is not what we want to look at; leave them alone.
    Value *v = OpInfo.CallOperandVal;
    if (isa<BasicBlock>(v) || isa<ConstantInt>(v) || isa<Function>(v)) {
      OpInfo.CallOperandVal = v;
      return;
    }

    // Otherwise, try to resolve it to something we know about by looking at
    // the actual operand type.
    if (const char *Repl = LowerXConstraint(OpInfo.ConstraintVT)) {
      OpInfo.ConstraintCode = Repl;
      OpInfo.ConstraintType = getConstraintType(OpInfo.ConstraintCode);
    }
  }
}

/// \brief Given an exact SDIV by a constant, create a multiplication
/// with the multiplicative inverse of the constant.
static SDValue BuildExactSDIV(const TargetLowering &TLI, SDValue Op1, APInt d,
                              const SDLoc &dl, SelectionDAG &DAG,
                              std::vector<SDNode *> &Created) {
  assert(d != 0 && "Division by zero!");

  // Shift the value upfront if it is even, so the LSB is one.
  unsigned ShAmt = d.countTrailingZeros();
  if (ShAmt) {
    // TODO: For UDIV use SRL instead of SRA.
    SDValue Amt =
        DAG.getConstant(ShAmt, dl, TLI.getShiftAmountTy(Op1.getValueType(),
                                                        DAG.getDataLayout()));
    SDNodeFlags Flags;
    Flags.setExact(true);
    Op1 = DAG.getNode(ISD::SRA, dl, Op1.getValueType(), Op1, Amt, Flags);
    Created.push_back(Op1.getNode());
    d.ashrInPlace(ShAmt);
  }

  // Calculate the multiplicative inverse, using Newton's method.
  APInt t, xn = d;
  while ((t = d*xn) != 1)
    xn *= APInt(d.getBitWidth(), 2) - t;

  SDValue Op2 = DAG.getConstant(xn, dl, Op1.getValueType());
  SDValue Mul = DAG.getNode(ISD::MUL, dl, Op1.getValueType(), Op1, Op2);
  Created.push_back(Mul.getNode());
  return Mul;
}

SDValue TargetLowering::BuildSDIVPow2(SDNode *N, const APInt &Divisor,
                                      SelectionDAG &DAG,
                                      std::vector<SDNode *> *Created) const {
  AttributeList Attr = DAG.getMachineFunction().getFunction()->getAttributes();
  const TargetLowering &TLI = DAG.getTargetLoweringInfo();
  if (TLI.isIntDivCheap(N->getValueType(0), Attr))
    return SDValue(N,0); // Lower SDIV as SDIV
  return SDValue();
}

/// \brief Given an ISD::SDIV node expressing a divide by constant,
/// return a DAG expression to select that will generate the same value by
/// multiplying by a magic number.
/// Ref: "Hacker's Delight" or "The PowerPC Compiler Writer's Guide".
SDValue TargetLowering::BuildSDIV(SDNode *N, const APInt &Divisor,
                                  SelectionDAG &DAG, bool IsAfterLegalization,
                                  std::vector<SDNode *> *Created) const {
  assert(Created && "No vector to hold sdiv ops.");

  EVT VT = N->getValueType(0);
  SDLoc dl(N);

  // Check to see if we can do this.
  // FIXME: We should be more aggressive here.
  if (!isTypeLegal(VT))
    return SDValue();

  // If the sdiv has an 'exact' bit we can use a simpler lowering.
  if (N->getFlags().hasExact())
    return BuildExactSDIV(*this, N->getOperand(0), Divisor, dl, DAG, *Created);

  APInt::ms magics = Divisor.magic();

  // Multiply the numerator (operand 0) by the magic value
  // FIXME: We should support doing a MUL in a wider type
  SDValue Q;
  if (IsAfterLegalization ? isOperationLegal(ISD::MULHS, VT) :
                            isOperationLegalOrCustom(ISD::MULHS, VT))
    Q = DAG.getNode(ISD::MULHS, dl, VT, N->getOperand(0),
                    DAG.getConstant(magics.m, dl, VT));
  else if (IsAfterLegalization ? isOperationLegal(ISD::SMUL_LOHI, VT) :
                                 isOperationLegalOrCustom(ISD::SMUL_LOHI, VT))
    Q = SDValue(DAG.getNode(ISD::SMUL_LOHI, dl, DAG.getVTList(VT, VT),
                              N->getOperand(0),
                              DAG.getConstant(magics.m, dl, VT)).getNode(), 1);
  else
    return SDValue();       // No mulhs or equvialent
  // If d > 0 and m < 0, add the numerator
  if (Divisor.isStrictlyPositive() && magics.m.isNegative()) {
    Q = DAG.getNode(ISD::ADD, dl, VT, Q, N->getOperand(0));
    Created->push_back(Q.getNode());
  }
  // If d < 0 and m > 0, subtract the numerator.
  if (Divisor.isNegative() && magics.m.isStrictlyPositive()) {
    Q = DAG.getNode(ISD::SUB, dl, VT, Q, N->getOperand(0));
    Created->push_back(Q.getNode());
  }
  auto &DL = DAG.getDataLayout();
  // Shift right algebraic if shift value is nonzero
  if (magics.s > 0) {
    Q = DAG.getNode(
        ISD::SRA, dl, VT, Q,
        DAG.getConstant(magics.s, dl, getShiftAmountTy(Q.getValueType(), DL)));
    Created->push_back(Q.getNode());
  }
  // Extract the sign bit and add it to the quotient
  SDValue T =
      DAG.getNode(ISD::SRL, dl, VT, Q,
                  DAG.getConstant(VT.getScalarSizeInBits() - 1, dl,
                                  getShiftAmountTy(Q.getValueType(), DL)));
  Created->push_back(T.getNode());
  return DAG.getNode(ISD::ADD, dl, VT, Q, T);
}

/// \brief Given an ISD::UDIV node expressing a divide by constant,
/// return a DAG expression to select that will generate the same value by
/// multiplying by a magic number.
/// Ref: "Hacker's Delight" or "The PowerPC Compiler Writer's Guide".
SDValue TargetLowering::BuildUDIV(SDNode *N, const APInt &Divisor,
                                  SelectionDAG &DAG, bool IsAfterLegalization,
                                  std::vector<SDNode *> *Created) const {
  assert(Created && "No vector to hold udiv ops.");

  EVT VT = N->getValueType(0);
  SDLoc dl(N);
  auto &DL = DAG.getDataLayout();

  // Check to see if we can do this.
  // FIXME: We should be more aggressive here.
  if (!isTypeLegal(VT))
    return SDValue();

  // FIXME: We should use a narrower constant when the upper
  // bits are known to be zero.
  APInt::mu magics = Divisor.magicu();

  SDValue Q = N->getOperand(0);

  // If the divisor is even, we can avoid using the expensive fixup by shifting
  // the divided value upfront.
  if (magics.a != 0 && !Divisor[0]) {
    unsigned Shift = Divisor.countTrailingZeros();
    Q = DAG.getNode(
        ISD::SRL, dl, VT, Q,
        DAG.getConstant(Shift, dl, getShiftAmountTy(Q.getValueType(), DL)));
    Created->push_back(Q.getNode());

    // Get magic number for the shifted divisor.
    magics = Divisor.lshr(Shift).magicu(Shift);
    assert(magics.a == 0 && "Should use cheap fixup now");
  }

  // Multiply the numerator (operand 0) by the magic value
  // FIXME: We should support doing a MUL in a wider type
  if (IsAfterLegalization ? isOperationLegal(ISD::MULHU, VT) :
                            isOperationLegalOrCustom(ISD::MULHU, VT))
    Q = DAG.getNode(ISD::MULHU, dl, VT, Q, DAG.getConstant(magics.m, dl, VT));
  else if (IsAfterLegalization ? isOperationLegal(ISD::UMUL_LOHI, VT) :
                                 isOperationLegalOrCustom(ISD::UMUL_LOHI, VT))
    Q = SDValue(DAG.getNode(ISD::UMUL_LOHI, dl, DAG.getVTList(VT, VT), Q,
                            DAG.getConstant(magics.m, dl, VT)).getNode(), 1);
  else
    return SDValue();       // No mulhu or equivalent

  Created->push_back(Q.getNode());

  if (magics.a == 0) {
    assert(magics.s < Divisor.getBitWidth() &&
           "We shouldn't generate an undefined shift!");
    return DAG.getNode(
        ISD::SRL, dl, VT, Q,
        DAG.getConstant(magics.s, dl, getShiftAmountTy(Q.getValueType(), DL)));
  } else {
    SDValue NPQ = DAG.getNode(ISD::SUB, dl, VT, N->getOperand(0), Q);
    Created->push_back(NPQ.getNode());
    NPQ = DAG.getNode(
        ISD::SRL, dl, VT, NPQ,
        DAG.getConstant(1, dl, getShiftAmountTy(NPQ.getValueType(), DL)));
    Created->push_back(NPQ.getNode());
    NPQ = DAG.getNode(ISD::ADD, dl, VT, NPQ, Q);
    Created->push_back(NPQ.getNode());
    return DAG.getNode(
        ISD::SRL, dl, VT, NPQ,
        DAG.getConstant(magics.s - 1, dl,
                        getShiftAmountTy(NPQ.getValueType(), DL)));
  }
}

bool TargetLowering::
verifyReturnAddressArgumentIsConstant(SDValue Op, SelectionDAG &DAG) const {
  if (!isa<ConstantSDNode>(Op.getOperand(0))) {
    DAG.getContext()->emitError("argument to '__builtin_return_address' must "
                                "be a constant integer");
    return true;
  }

  return false;
}

//===----------------------------------------------------------------------===//
// Legalization Utilities
//===----------------------------------------------------------------------===//

bool TargetLowering::expandMUL_LOHI(unsigned Opcode, EVT VT, SDLoc dl,
                                    SDValue LHS, SDValue RHS,
                                    SmallVectorImpl<SDValue> &Result,
                                    EVT HiLoVT, SelectionDAG &DAG,
                                    MulExpansionKind Kind, SDValue LL,
                                    SDValue LH, SDValue RL, SDValue RH) const {
  assert(Opcode == ISD::MUL || Opcode == ISD::UMUL_LOHI ||
         Opcode == ISD::SMUL_LOHI);

  bool HasMULHS = (Kind == MulExpansionKind::Always) ||
                  isOperationLegalOrCustom(ISD::MULHS, HiLoVT);
  bool HasMULHU = (Kind == MulExpansionKind::Always) ||
                  isOperationLegalOrCustom(ISD::MULHU, HiLoVT);
  bool HasSMUL_LOHI = (Kind == MulExpansionKind::Always) ||
                      isOperationLegalOrCustom(ISD::SMUL_LOHI, HiLoVT);
  bool HasUMUL_LOHI = (Kind == MulExpansionKind::Always) ||
                      isOperationLegalOrCustom(ISD::UMUL_LOHI, HiLoVT);

  if (!HasMULHU && !HasMULHS && !HasUMUL_LOHI && !HasSMUL_LOHI)
    return false;

  unsigned OuterBitSize = VT.getScalarSizeInBits();
  unsigned InnerBitSize = HiLoVT.getScalarSizeInBits();
  unsigned LHSSB = DAG.ComputeNumSignBits(LHS);
  unsigned RHSSB = DAG.ComputeNumSignBits(RHS);

  // LL, LH, RL, and RH must be either all NULL or all set to a value.
  assert((LL.getNode() && LH.getNode() && RL.getNode() && RH.getNode()) ||
         (!LL.getNode() && !LH.getNode() && !RL.getNode() && !RH.getNode()));

  SDVTList VTs = DAG.getVTList(HiLoVT, HiLoVT);
  auto MakeMUL_LOHI = [&](SDValue L, SDValue R, SDValue &Lo, SDValue &Hi,
                          bool Signed) -> bool {
    if ((Signed && HasSMUL_LOHI) || (!Signed && HasUMUL_LOHI)) {
      Lo = DAG.getNode(Signed ? ISD::SMUL_LOHI : ISD::UMUL_LOHI, dl, VTs, L, R);
      Hi = SDValue(Lo.getNode(), 1);
      return true;
    }
    if ((Signed && HasMULHS) || (!Signed && HasMULHU)) {
      Lo = DAG.getNode(ISD::MUL, dl, HiLoVT, L, R);
      Hi = DAG.getNode(Signed ? ISD::MULHS : ISD::MULHU, dl, HiLoVT, L, R);
      return true;
    }
    return false;
  };

  SDValue Lo, Hi;

  if (!LL.getNode() && !RL.getNode() &&
      isOperationLegalOrCustom(ISD::TRUNCATE, HiLoVT)) {
    LL = DAG.getNode(ISD::TRUNCATE, dl, HiLoVT, LHS);
    RL = DAG.getNode(ISD::TRUNCATE, dl, HiLoVT, RHS);
  }

  if (!LL.getNode())
    return false;

  APInt HighMask = APInt::getHighBitsSet(OuterBitSize, InnerBitSize);
  if (DAG.MaskedValueIsZero(LHS, HighMask) &&
      DAG.MaskedValueIsZero(RHS, HighMask)) {
    // The inputs are both zero-extended.
    if (MakeMUL_LOHI(LL, RL, Lo, Hi, false)) {
      Result.push_back(Lo);
      Result.push_back(Hi);
      if (Opcode != ISD::MUL) {
        SDValue Zero = DAG.getConstant(0, dl, HiLoVT);
        Result.push_back(Zero);
        Result.push_back(Zero);
      }
      return true;
    }
  }

  if (!VT.isVector() && Opcode == ISD::MUL && LHSSB > InnerBitSize &&
      RHSSB > InnerBitSize) {
    // The input values are both sign-extended.
    // TODO non-MUL case?
    if (MakeMUL_LOHI(LL, RL, Lo, Hi, true)) {
      Result.push_back(Lo);
      Result.push_back(Hi);
      return true;
    }
  }

  unsigned ShiftAmount = OuterBitSize - InnerBitSize;
  EVT ShiftAmountTy = getShiftAmountTy(VT, DAG.getDataLayout());
  if (APInt::getMaxValue(ShiftAmountTy.getSizeInBits()).ult(ShiftAmount)) {
    // FIXME getShiftAmountTy does not always return a sensible result when VT
    // is an illegal type, and so the type may be too small to fit the shift
    // amount. Override it with i32. The shift will have to be legalized.
    ShiftAmountTy = MVT::i32;
  }
  SDValue Shift = DAG.getConstant(ShiftAmount, dl, ShiftAmountTy);

  if (!LH.getNode() && !RH.getNode() &&
      isOperationLegalOrCustom(ISD::SRL, VT) &&
      isOperationLegalOrCustom(ISD::TRUNCATE, HiLoVT)) {
    LH = DAG.getNode(ISD::SRL, dl, VT, LHS, Shift);
    LH = DAG.getNode(ISD::TRUNCATE, dl, HiLoVT, LH);
    RH = DAG.getNode(ISD::SRL, dl, VT, RHS, Shift);
    RH = DAG.getNode(ISD::TRUNCATE, dl, HiLoVT, RH);
  }

  if (!LH.getNode())
    return false;

  if (!MakeMUL_LOHI(LL, RL, Lo, Hi, false))
    return false;

  Result.push_back(Lo);

  if (Opcode == ISD::MUL) {
    RH = DAG.getNode(ISD::MUL, dl, HiLoVT, LL, RH);
    LH = DAG.getNode(ISD::MUL, dl, HiLoVT, LH, RL);
    Hi = DAG.getNode(ISD::ADD, dl, HiLoVT, Hi, RH);
    Hi = DAG.getNode(ISD::ADD, dl, HiLoVT, Hi, LH);
    Result.push_back(Hi);
    return true;
  }

  // Compute the full width result.
  auto Merge = [&](SDValue Lo, SDValue Hi) -> SDValue {
    Lo = DAG.getNode(ISD::ZERO_EXTEND, dl, VT, Lo);
    Hi = DAG.getNode(ISD::ZERO_EXTEND, dl, VT, Hi);
    Hi = DAG.getNode(ISD::SHL, dl, VT, Hi, Shift);
    return DAG.getNode(ISD::OR, dl, VT, Lo, Hi);
  };

  SDValue Next = DAG.getNode(ISD::ZERO_EXTEND, dl, VT, Hi);
  if (!MakeMUL_LOHI(LL, RH, Lo, Hi, false))
    return false;

  // This is effectively the add part of a multiply-add of half-sized operands,
  // so it cannot overflow.
  Next = DAG.getNode(ISD::ADD, dl, VT, Next, Merge(Lo, Hi));

  if (!MakeMUL_LOHI(LH, RL, Lo, Hi, false))
    return false;

  Next = DAG.getNode(ISD::ADDC, dl, DAG.getVTList(VT, MVT::Glue), Next,
                     Merge(Lo, Hi));

  SDValue Carry = Next.getValue(1);
  Result.push_back(DAG.getNode(ISD::TRUNCATE, dl, HiLoVT, Next));
  Next = DAG.getNode(ISD::SRL, dl, VT, Next, Shift);

  if (!MakeMUL_LOHI(LH, RH, Lo, Hi, Opcode == ISD::SMUL_LOHI))
    return false;

  SDValue Zero = DAG.getConstant(0, dl, HiLoVT);
  Hi = DAG.getNode(ISD::ADDE, dl, DAG.getVTList(HiLoVT, MVT::Glue), Hi, Zero,
                   Carry);
  Next = DAG.getNode(ISD::ADD, dl, VT, Next, Merge(Lo, Hi));

  if (Opcode == ISD::SMUL_LOHI) {
    SDValue NextSub = DAG.getNode(ISD::SUB, dl, VT, Next,
                                  DAG.getNode(ISD::ZERO_EXTEND, dl, VT, RL));
    Next = DAG.getSelectCC(dl, LH, Zero, NextSub, Next, ISD::SETLT);

    NextSub = DAG.getNode(ISD::SUB, dl, VT, Next,
                          DAG.getNode(ISD::ZERO_EXTEND, dl, VT, LL));
    Next = DAG.getSelectCC(dl, RH, Zero, NextSub, Next, ISD::SETLT);
  }

  Result.push_back(DAG.getNode(ISD::TRUNCATE, dl, HiLoVT, Next));
  Next = DAG.getNode(ISD::SRL, dl, VT, Next, Shift);
  Result.push_back(DAG.getNode(ISD::TRUNCATE, dl, HiLoVT, Next));
  return true;
}

bool TargetLowering::expandMUL(SDNode *N, SDValue &Lo, SDValue &Hi, EVT HiLoVT,
                               SelectionDAG &DAG, MulExpansionKind Kind,
                               SDValue LL, SDValue LH, SDValue RL,
                               SDValue RH) const {
  SmallVector<SDValue, 2> Result;
  bool Ok = expandMUL_LOHI(N->getOpcode(), N->getValueType(0), N,
                           N->getOperand(0), N->getOperand(1), Result, HiLoVT,
                           DAG, Kind, LL, LH, RL, RH);
  if (Ok) {
    assert(Result.size() == 2);
    Lo = Result[0];
    Hi = Result[1];
  }
  return Ok;
}

bool TargetLowering::expandFP_TO_SINT(SDNode *Node, SDValue &Result,
                               SelectionDAG &DAG) const {
  EVT VT = Node->getOperand(0).getValueType();
  EVT NVT = Node->getValueType(0);
  SDLoc dl(SDValue(Node, 0));

  // FIXME: Only f32 to i64 conversions are supported.
  if (VT != MVT::f32 || NVT != MVT::i64)
    return false;

  // Expand f32 -> i64 conversion
  // This algorithm comes from compiler-rt's implementation of fixsfdi:
  // https://github.com/llvm-mirror/compiler-rt/blob/master/lib/builtins/fixsfdi.c
  EVT IntVT = EVT::getIntegerVT(*DAG.getContext(),
                                VT.getSizeInBits());
  SDValue ExponentMask = DAG.getConstant(0x7F800000, dl, IntVT);
  SDValue ExponentLoBit = DAG.getConstant(23, dl, IntVT);
  SDValue Bias = DAG.getConstant(127, dl, IntVT);
  SDValue SignMask = DAG.getConstant(APInt::getSignMask(VT.getSizeInBits()), dl,
                                     IntVT);
  SDValue SignLowBit = DAG.getConstant(VT.getSizeInBits() - 1, dl, IntVT);
  SDValue MantissaMask = DAG.getConstant(0x007FFFFF, dl, IntVT);

  SDValue Bits = DAG.getNode(ISD::BITCAST, dl, IntVT, Node->getOperand(0));

  auto &DL = DAG.getDataLayout();
  SDValue ExponentBits = DAG.getNode(
      ISD::SRL, dl, IntVT, DAG.getNode(ISD::AND, dl, IntVT, Bits, ExponentMask),
      DAG.getZExtOrTrunc(ExponentLoBit, dl, getShiftAmountTy(IntVT, DL)));
  SDValue Exponent = DAG.getNode(ISD::SUB, dl, IntVT, ExponentBits, Bias);

  SDValue Sign = DAG.getNode(
      ISD::SRA, dl, IntVT, DAG.getNode(ISD::AND, dl, IntVT, Bits, SignMask),
      DAG.getZExtOrTrunc(SignLowBit, dl, getShiftAmountTy(IntVT, DL)));
  Sign = DAG.getSExtOrTrunc(Sign, dl, NVT);

  SDValue R = DAG.getNode(ISD::OR, dl, IntVT,
      DAG.getNode(ISD::AND, dl, IntVT, Bits, MantissaMask),
      DAG.getConstant(0x00800000, dl, IntVT));

  R = DAG.getZExtOrTrunc(R, dl, NVT);

  R = DAG.getSelectCC(
      dl, Exponent, ExponentLoBit,
      DAG.getNode(ISD::SHL, dl, NVT, R,
                  DAG.getZExtOrTrunc(
                      DAG.getNode(ISD::SUB, dl, IntVT, Exponent, ExponentLoBit),
                      dl, getShiftAmountTy(IntVT, DL))),
      DAG.getNode(ISD::SRL, dl, NVT, R,
                  DAG.getZExtOrTrunc(
                      DAG.getNode(ISD::SUB, dl, IntVT, ExponentLoBit, Exponent),
                      dl, getShiftAmountTy(IntVT, DL))),
      ISD::SETGT);

  SDValue Ret = DAG.getNode(ISD::SUB, dl, NVT,
      DAG.getNode(ISD::XOR, dl, NVT, R, Sign),
      Sign);

  Result = DAG.getSelectCC(dl, Exponent, DAG.getConstant(0, dl, IntVT),
      DAG.getConstant(0, dl, NVT), Ret, ISD::SETLT);
  return true;
}

SDValue TargetLowering::scalarizeVectorLoad(LoadSDNode *LD,
                                            SelectionDAG &DAG) const {
  SDLoc SL(LD);
  SDValue Chain = LD->getChain();
  SDValue BasePTR = LD->getBasePtr();
  EVT SrcVT = LD->getMemoryVT();
  ISD::LoadExtType ExtType = LD->getExtensionType();

  unsigned NumElem = SrcVT.getVectorNumElements();

  EVT SrcEltVT = SrcVT.getScalarType();
  EVT DstEltVT = LD->getValueType(0).getScalarType();

  unsigned Stride = SrcEltVT.getSizeInBits() / 8;
  assert(SrcEltVT.isByteSized());

  EVT PtrVT = BasePTR.getValueType();

  SmallVector<SDValue, 8> Vals;
  SmallVector<SDValue, 8> LoadChains;

  for (unsigned Idx = 0; Idx < NumElem; ++Idx) {
    SDValue ScalarLoad =
        DAG.getExtLoad(ExtType, SL, DstEltVT, Chain, BasePTR,
                       LD->getPointerInfo().getWithOffset(Idx * Stride),
                       SrcEltVT, MinAlign(LD->getAlignment(), Idx * Stride),
                       LD->getMemOperand()->getFlags(), LD->getAAInfo());

    BasePTR = DAG.getNode(ISD::ADD, SL, PtrVT, BasePTR,
                          DAG.getConstant(Stride, SL, PtrVT));

    Vals.push_back(ScalarLoad.getValue(0));
    LoadChains.push_back(ScalarLoad.getValue(1));
  }

  SDValue NewChain = DAG.getNode(ISD::TokenFactor, SL, MVT::Other, LoadChains);
  SDValue Value = DAG.getBuildVector(LD->getValueType(0), SL, Vals);

  return DAG.getMergeValues({ Value, NewChain }, SL);
}

// FIXME: This relies on each element having a byte size, otherwise the stride
// is 0 and just overwrites the same location. ExpandStore currently expects
// this broken behavior.
SDValue TargetLowering::scalarizeVectorStore(StoreSDNode *ST,
                                             SelectionDAG &DAG) const {
  SDLoc SL(ST);

  SDValue Chain = ST->getChain();
  SDValue BasePtr = ST->getBasePtr();
  SDValue Value = ST->getValue();
  EVT StVT = ST->getMemoryVT();

  // The type of the data we want to save
  EVT RegVT = Value.getValueType();
  EVT RegSclVT = RegVT.getScalarType();

  // The type of data as saved in memory.
  EVT MemSclVT = StVT.getScalarType();

  EVT PtrVT = BasePtr.getValueType();

  // Store Stride in bytes
  unsigned Stride = MemSclVT.getSizeInBits() / 8;
  EVT IdxVT = getVectorIdxTy(DAG.getDataLayout());
  unsigned NumElem = StVT.getVectorNumElements();

  // Extract each of the elements from the original vector and save them into
  // memory individually.
  SmallVector<SDValue, 8> Stores;
  for (unsigned Idx = 0; Idx < NumElem; ++Idx) {
    SDValue Elt = DAG.getNode(ISD::EXTRACT_VECTOR_ELT, SL, RegSclVT, Value,
                              DAG.getConstant(Idx, SL, IdxVT));

    SDValue Ptr = DAG.getNode(ISD::ADD, SL, PtrVT, BasePtr,
                              DAG.getConstant(Idx * Stride, SL, PtrVT));

    // This scalar TruncStore may be illegal, but we legalize it later.
    SDValue Store = DAG.getTruncStore(
        Chain, SL, Elt, Ptr, ST->getPointerInfo().getWithOffset(Idx * Stride),
        MemSclVT, MinAlign(ST->getAlignment(), Idx * Stride),
        ST->getMemOperand()->getFlags(), ST->getAAInfo());

    Stores.push_back(Store);
  }

  return DAG.getNode(ISD::TokenFactor, SL, MVT::Other, Stores);
}

std::pair<SDValue, SDValue>
TargetLowering::expandUnalignedLoad(LoadSDNode *LD, SelectionDAG &DAG) const {
  assert(LD->getAddressingMode() == ISD::UNINDEXED &&
         "unaligned indexed loads not implemented!");
  SDValue Chain = LD->getChain();
  SDValue Ptr = LD->getBasePtr();
  EVT VT = LD->getValueType(0);
  EVT LoadedVT = LD->getMemoryVT();
  SDLoc dl(LD);
  if (VT.isFloatingPoint() || VT.isVector()) {
    EVT intVT = EVT::getIntegerVT(*DAG.getContext(), LoadedVT.getSizeInBits());
    if (isTypeLegal(intVT) && isTypeLegal(LoadedVT)) {
      if (!isOperationLegalOrCustom(ISD::LOAD, intVT)) {
        // Scalarize the load and let the individual components be handled.
        SDValue Scalarized = scalarizeVectorLoad(LD, DAG);
        return std::make_pair(Scalarized.getValue(0), Scalarized.getValue(1));
      }

      // Expand to a (misaligned) integer load of the same size,
      // then bitconvert to floating point or vector.
      SDValue newLoad = DAG.getLoad(intVT, dl, Chain, Ptr,
                                    LD->getMemOperand());
      SDValue Result = DAG.getNode(ISD::BITCAST, dl, LoadedVT, newLoad);
      if (LoadedVT != VT)
        Result = DAG.getNode(VT.isFloatingPoint() ? ISD::FP_EXTEND :
                             ISD::ANY_EXTEND, dl, VT, Result);

      return std::make_pair(Result, newLoad.getValue(1));
    }

    // Copy the value to a (aligned) stack slot using (unaligned) integer
    // loads and stores, then do a (aligned) load from the stack slot.
    MVT RegVT = getRegisterType(*DAG.getContext(), intVT);
    unsigned LoadedBytes = LoadedVT.getSizeInBits() / 8;
    unsigned RegBytes = RegVT.getSizeInBits() / 8;
    unsigned NumRegs = (LoadedBytes + RegBytes - 1) / RegBytes;

    // Make sure the stack slot is also aligned for the register type.
    SDValue StackBase = DAG.CreateStackTemporary(LoadedVT, RegVT);

    SmallVector<SDValue, 8> Stores;
    SDValue StackPtr = StackBase;
    unsigned Offset = 0;

    EVT PtrVT = Ptr.getValueType();
    EVT StackPtrVT = StackPtr.getValueType();

    SDValue PtrIncrement = DAG.getConstant(RegBytes, dl, PtrVT);
    SDValue StackPtrIncrement = DAG.getConstant(RegBytes, dl, StackPtrVT);

    // Do all but one copies using the full register width.
    for (unsigned i = 1; i < NumRegs; i++) {
      // Load one integer register's worth from the original location.
      SDValue Load = DAG.getLoad(
          RegVT, dl, Chain, Ptr, LD->getPointerInfo().getWithOffset(Offset),
          MinAlign(LD->getAlignment(), Offset), LD->getMemOperand()->getFlags(),
          LD->getAAInfo());
      // Follow the load with a store to the stack slot.  Remember the store.
      Stores.push_back(DAG.getStore(Load.getValue(1), dl, Load, StackPtr,
                                    MachinePointerInfo()));
      // Increment the pointers.
      Offset += RegBytes;
      Ptr = DAG.getNode(ISD::ADD, dl, PtrVT, Ptr, PtrIncrement);
      StackPtr = DAG.getNode(ISD::ADD, dl, StackPtrVT, StackPtr,
                             StackPtrIncrement);
    }

    // The last copy may be partial.  Do an extending load.
    EVT MemVT = EVT::getIntegerVT(*DAG.getContext(),
                                  8 * (LoadedBytes - Offset));
    SDValue Load =
        DAG.getExtLoad(ISD::EXTLOAD, dl, RegVT, Chain, Ptr,
                       LD->getPointerInfo().getWithOffset(Offset), MemVT,
                       MinAlign(LD->getAlignment(), Offset),
                       LD->getMemOperand()->getFlags(), LD->getAAInfo());
    // Follow the load with a store to the stack slot.  Remember the store.
    // On big-endian machines this requires a truncating store to ensure
    // that the bits end up in the right place.
    Stores.push_back(DAG.getTruncStore(Load.getValue(1), dl, Load, StackPtr,
                                       MachinePointerInfo(), MemVT));

    // The order of the stores doesn't matter - say it with a TokenFactor.
    SDValue TF = DAG.getNode(ISD::TokenFactor, dl, MVT::Other, Stores);

    // Finally, perform the original load only redirected to the stack slot.
    Load = DAG.getExtLoad(LD->getExtensionType(), dl, VT, TF, StackBase,
                          MachinePointerInfo(), LoadedVT);

    // Callers expect a MERGE_VALUES node.
    return std::make_pair(Load, TF);
  }

  assert(LoadedVT.isInteger() && !LoadedVT.isVector() &&
         "Unaligned load of unsupported type.");

  // Compute the new VT that is half the size of the old one.  This is an
  // integer MVT.
  unsigned NumBits = LoadedVT.getSizeInBits();
  EVT NewLoadedVT;
  NewLoadedVT = EVT::getIntegerVT(*DAG.getContext(), NumBits/2);
  NumBits >>= 1;

  unsigned Alignment = LD->getAlignment();
  unsigned IncrementSize = NumBits / 8;
  ISD::LoadExtType HiExtType = LD->getExtensionType();

  // If the original load is NON_EXTLOAD, the hi part load must be ZEXTLOAD.
  if (HiExtType == ISD::NON_EXTLOAD)
    HiExtType = ISD::ZEXTLOAD;

  // Load the value in two parts
  SDValue Lo, Hi;
  if (DAG.getDataLayout().isLittleEndian()) {
    Lo = DAG.getExtLoad(ISD::ZEXTLOAD, dl, VT, Chain, Ptr, LD->getPointerInfo(),
                        NewLoadedVT, Alignment, LD->getMemOperand()->getFlags(),
                        LD->getAAInfo());
    Ptr = DAG.getNode(ISD::ADD, dl, Ptr.getValueType(), Ptr,
                      DAG.getConstant(IncrementSize, dl, Ptr.getValueType()));
    Hi = DAG.getExtLoad(HiExtType, dl, VT, Chain, Ptr,
                        LD->getPointerInfo().getWithOffset(IncrementSize),
                        NewLoadedVT, MinAlign(Alignment, IncrementSize),
                        LD->getMemOperand()->getFlags(), LD->getAAInfo());
  } else {
    Hi = DAG.getExtLoad(HiExtType, dl, VT, Chain, Ptr, LD->getPointerInfo(),
                        NewLoadedVT, Alignment, LD->getMemOperand()->getFlags(),
                        LD->getAAInfo());
    Ptr = DAG.getNode(ISD::ADD, dl, Ptr.getValueType(), Ptr,
                      DAG.getConstant(IncrementSize, dl, Ptr.getValueType()));
    Lo = DAG.getExtLoad(ISD::ZEXTLOAD, dl, VT, Chain, Ptr,
                        LD->getPointerInfo().getWithOffset(IncrementSize),
                        NewLoadedVT, MinAlign(Alignment, IncrementSize),
                        LD->getMemOperand()->getFlags(), LD->getAAInfo());
  }

  // aggregate the two parts
  SDValue ShiftAmount =
      DAG.getConstant(NumBits, dl, getShiftAmountTy(Hi.getValueType(),
                                                    DAG.getDataLayout()));
  SDValue Result = DAG.getNode(ISD::SHL, dl, VT, Hi, ShiftAmount);
  Result = DAG.getNode(ISD::OR, dl, VT, Result, Lo);

  SDValue TF = DAG.getNode(ISD::TokenFactor, dl, MVT::Other, Lo.getValue(1),
                             Hi.getValue(1));

  return std::make_pair(Result, TF);
}

SDValue TargetLowering::expandUnalignedStore(StoreSDNode *ST,
                                             SelectionDAG &DAG) const {
  assert(ST->getAddressingMode() == ISD::UNINDEXED &&
         "unaligned indexed stores not implemented!");
  SDValue Chain = ST->getChain();
  SDValue Ptr = ST->getBasePtr();
  SDValue Val = ST->getValue();
  EVT VT = Val.getValueType();
  int Alignment = ST->getAlignment();

  SDLoc dl(ST);
  if (ST->getMemoryVT().isFloatingPoint() ||
      ST->getMemoryVT().isVector()) {
    EVT intVT = EVT::getIntegerVT(*DAG.getContext(), VT.getSizeInBits());
    if (isTypeLegal(intVT)) {
      if (!isOperationLegalOrCustom(ISD::STORE, intVT)) {
        // Scalarize the store and let the individual components be handled.
        SDValue Result = scalarizeVectorStore(ST, DAG);

        return Result;
      }
      // Expand to a bitconvert of the value to the integer type of the
      // same size, then a (misaligned) int store.
      // FIXME: Does not handle truncating floating point stores!
      SDValue Result = DAG.getNode(ISD::BITCAST, dl, intVT, Val);
      Result = DAG.getStore(Chain, dl, Result, Ptr, ST->getPointerInfo(),
                            Alignment, ST->getMemOperand()->getFlags());
      return Result;
    }
    // Do a (aligned) store to a stack slot, then copy from the stack slot
    // to the final destination using (unaligned) integer loads and stores.
    EVT StoredVT = ST->getMemoryVT();
    MVT RegVT =
      getRegisterType(*DAG.getContext(),
                      EVT::getIntegerVT(*DAG.getContext(),
                                        StoredVT.getSizeInBits()));
    EVT PtrVT = Ptr.getValueType();
    unsigned StoredBytes = StoredVT.getSizeInBits() / 8;
    unsigned RegBytes = RegVT.getSizeInBits() / 8;
    unsigned NumRegs = (StoredBytes + RegBytes - 1) / RegBytes;

    // Make sure the stack slot is also aligned for the register type.
    SDValue StackPtr = DAG.CreateStackTemporary(StoredVT, RegVT);

    // Perform the original store, only redirected to the stack slot.
    SDValue Store = DAG.getTruncStore(Chain, dl, Val, StackPtr,
                                      MachinePointerInfo(), StoredVT);

    EVT StackPtrVT = StackPtr.getValueType();

    SDValue PtrIncrement = DAG.getConstant(RegBytes, dl, PtrVT);
    SDValue StackPtrIncrement = DAG.getConstant(RegBytes, dl, StackPtrVT);
    SmallVector<SDValue, 8> Stores;
    unsigned Offset = 0;

    // Do all but one copies using the full register width.
    for (unsigned i = 1; i < NumRegs; i++) {
      // Load one integer register's worth from the stack slot.
      SDValue Load =
          DAG.getLoad(RegVT, dl, Store, StackPtr, MachinePointerInfo());
      // Store it to the final location.  Remember the store.
      Stores.push_back(DAG.getStore(Load.getValue(1), dl, Load, Ptr,
                                    ST->getPointerInfo().getWithOffset(Offset),
                                    MinAlign(ST->getAlignment(), Offset),
                                    ST->getMemOperand()->getFlags()));
      // Increment the pointers.
      Offset += RegBytes;
      StackPtr = DAG.getNode(ISD::ADD, dl, StackPtrVT,
                             StackPtr, StackPtrIncrement);
      Ptr = DAG.getNode(ISD::ADD, dl, PtrVT, Ptr, PtrIncrement);
    }

    // The last store may be partial.  Do a truncating store.  On big-endian
    // machines this requires an extending load from the stack slot to ensure
    // that the bits are in the right place.
    EVT MemVT = EVT::getIntegerVT(*DAG.getContext(),
                                  8 * (StoredBytes - Offset));

    // Load from the stack slot.
    SDValue Load = DAG.getExtLoad(ISD::EXTLOAD, dl, RegVT, Store, StackPtr,
                                  MachinePointerInfo(), MemVT);

    Stores.push_back(
        DAG.getTruncStore(Load.getValue(1), dl, Load, Ptr,
                          ST->getPointerInfo().getWithOffset(Offset), MemVT,
                          MinAlign(ST->getAlignment(), Offset),
                          ST->getMemOperand()->getFlags(), ST->getAAInfo()));
    // The order of the stores doesn't matter - say it with a TokenFactor.
    SDValue Result = DAG.getNode(ISD::TokenFactor, dl, MVT::Other, Stores);
    return Result;
  }

  assert(ST->getMemoryVT().isInteger() &&
         !ST->getMemoryVT().isVector() &&
         "Unaligned store of unknown type.");
  // Get the half-size VT
  EVT NewStoredVT = ST->getMemoryVT().getHalfSizedIntegerVT(*DAG.getContext());
  int NumBits = NewStoredVT.getSizeInBits();
  int IncrementSize = NumBits / 8;

  // Divide the stored value in two parts.
  SDValue ShiftAmount =
      DAG.getConstant(NumBits, dl, getShiftAmountTy(Val.getValueType(),
                                                    DAG.getDataLayout()));
  SDValue Lo = Val;
  SDValue Hi = DAG.getNode(ISD::SRL, dl, VT, Val, ShiftAmount);

  // Store the two parts
  SDValue Store1, Store2;
  Store1 = DAG.getTruncStore(Chain, dl,
                             DAG.getDataLayout().isLittleEndian() ? Lo : Hi,
                             Ptr, ST->getPointerInfo(), NewStoredVT, Alignment,
                             ST->getMemOperand()->getFlags());

  EVT PtrVT = Ptr.getValueType();
  Ptr = DAG.getNode(ISD::ADD, dl, PtrVT, Ptr,
                    DAG.getConstant(IncrementSize, dl, PtrVT));
  Alignment = MinAlign(Alignment, IncrementSize);
  Store2 = DAG.getTruncStore(
      Chain, dl, DAG.getDataLayout().isLittleEndian() ? Hi : Lo, Ptr,
      ST->getPointerInfo().getWithOffset(IncrementSize), NewStoredVT, Alignment,
      ST->getMemOperand()->getFlags(), ST->getAAInfo());

  SDValue Result =
    DAG.getNode(ISD::TokenFactor, dl, MVT::Other, Store1, Store2);
  return Result;
}

SDValue
TargetLowering::IncrementMemoryAddress(SDValue Addr, SDValue Mask,
                                       const SDLoc &DL, EVT DataVT,
                                       SelectionDAG &DAG,
                                       bool IsCompressedMemory) const {
  SDValue Increment;
  EVT AddrVT = Addr.getValueType();
  EVT MaskVT = Mask.getValueType();
  assert(DataVT.getVectorNumElements() == MaskVT.getVectorNumElements() &&
         "Incompatible types of Data and Mask");
  if (IsCompressedMemory) {
    // Incrementing the pointer according to number of '1's in the mask.
    EVT MaskIntVT = EVT::getIntegerVT(*DAG.getContext(), MaskVT.getSizeInBits());
    SDValue MaskInIntReg = DAG.getBitcast(MaskIntVT, Mask);
    if (MaskIntVT.getSizeInBits() < 32) {
      MaskInIntReg = DAG.getNode(ISD::ZERO_EXTEND, DL, MVT::i32, MaskInIntReg);
      MaskIntVT = MVT::i32;
    }

    // Count '1's with POPCNT.
    Increment = DAG.getNode(ISD::CTPOP, DL, MaskIntVT, MaskInIntReg);
    Increment = DAG.getZExtOrTrunc(Increment, DL, AddrVT);
    // Scale is an element size in bytes.
    SDValue Scale = DAG.getConstant(DataVT.getScalarSizeInBits() / 8, DL,
                                    AddrVT);
    Increment = DAG.getNode(ISD::MUL, DL, AddrVT, Increment, Scale);
  } else
    Increment = DAG.getConstant(DataVT.getSizeInBits() / 8, DL, AddrVT);

  return DAG.getNode(ISD::ADD, DL, AddrVT, Addr, Increment);
}

static SDValue clampDynamicVectorIndex(SelectionDAG &DAG,
                                       SDValue Idx,
                                       EVT VecVT,
                                       const SDLoc &dl) {
  if (isa<ConstantSDNode>(Idx))
    return Idx;

  EVT IdxVT = Idx.getValueType();
  unsigned NElts = VecVT.getVectorNumElements();
  if (isPowerOf2_32(NElts)) {
    APInt Imm = APInt::getLowBitsSet(IdxVT.getSizeInBits(),
                                     Log2_32(NElts));
    return DAG.getNode(ISD::AND, dl, IdxVT, Idx,
                       DAG.getConstant(Imm, dl, IdxVT));
  }

  return DAG.getNode(ISD::UMIN, dl, IdxVT, Idx,
                     DAG.getConstant(NElts - 1, dl, IdxVT));
}

SDValue TargetLowering::getVectorElementPointer(SelectionDAG &DAG,
                                                SDValue VecPtr, EVT VecVT,
                                                SDValue Index) const {
  SDLoc dl(Index);
  // Make sure the index type is big enough to compute in.
  Index = DAG.getZExtOrTrunc(Index, dl, getPointerTy(DAG.getDataLayout()));

  EVT EltVT = VecVT.getVectorElementType();

  // Calculate the element offset and add it to the pointer.
  unsigned EltSize = EltVT.getSizeInBits() / 8; // FIXME: should be ABI size.
  assert(EltSize * 8 == EltVT.getSizeInBits() &&
         "Converting bits to bytes lost precision");

  Index = clampDynamicVectorIndex(DAG, Index, VecVT, dl);

  EVT IdxVT = Index.getValueType();

  Index = DAG.getNode(ISD::MUL, dl, IdxVT, Index,
                      DAG.getConstant(EltSize, dl, IdxVT));
  return DAG.getNode(ISD::ADD, dl, IdxVT, Index, VecPtr);
}

//===----------------------------------------------------------------------===//
// Implementation of Emulated TLS Model
//===----------------------------------------------------------------------===//

SDValue TargetLowering::LowerToTLSEmulatedModel(const GlobalAddressSDNode *GA,
                                                SelectionDAG &DAG) const {
  // Access to address of TLS varialbe xyz is lowered to a function call:
  //   __emutls_get_address( address of global variable named "__emutls_v.xyz" )
  EVT PtrVT = getPointerTy(DAG.getDataLayout());
  PointerType *VoidPtrType = Type::getInt8PtrTy(*DAG.getContext());
  SDLoc dl(GA);

  ArgListTy Args;
  ArgListEntry Entry;
  std::string NameString = ("__emutls_v." + GA->getGlobal()->getName()).str();
  Module *VariableModule = const_cast<Module*>(GA->getGlobal()->getParent());
  StringRef EmuTlsVarName(NameString);
  GlobalVariable *EmuTlsVar = VariableModule->getNamedGlobal(EmuTlsVarName);
  assert(EmuTlsVar && "Cannot find EmuTlsVar ");
  Entry.Node = DAG.getGlobalAddress(EmuTlsVar, dl, PtrVT);
  Entry.Ty = VoidPtrType;
  Args.push_back(Entry);

  SDValue EmuTlsGetAddr = DAG.getExternalSymbol("__emutls_get_address", PtrVT);

  TargetLowering::CallLoweringInfo CLI(DAG);
  CLI.setDebugLoc(dl).setChain(DAG.getEntryNode());
  CLI.setLibCallee(CallingConv::C, VoidPtrType, EmuTlsGetAddr, std::move(Args));
  std::pair<SDValue, SDValue> CallResult = LowerCallTo(CLI);

  // TLSADDR will be codegen'ed as call. Inform MFI that function has calls.
  // At last for X86 targets, maybe good for other targets too?
  MachineFrameInfo &MFI = DAG.getMachineFunction().getFrameInfo();
  MFI.setAdjustsStack(true);  // Is this only for X86 target?
  MFI.setHasCalls(true);

  assert((GA->getOffset() == 0) &&
         "Emulated TLS must have zero offset in GlobalAddressSDNode");
  return CallResult.first;
}

SDValue TargetLowering::lowerCmpEqZeroToCtlzSrl(SDValue Op,
                                                SelectionDAG &DAG) const {
  assert((Op->getOpcode() == ISD::SETCC) && "Input has to be a SETCC node.");
  if (!isCtlzFast())
    return SDValue();
  ISD::CondCode CC = cast<CondCodeSDNode>(Op.getOperand(2))->get();
  SDLoc dl(Op);
  if (ConstantSDNode *C = dyn_cast<ConstantSDNode>(Op.getOperand(1))) {
    if (C->isNullValue() && CC == ISD::SETEQ) {
      EVT VT = Op.getOperand(0).getValueType();
      SDValue Zext = Op.getOperand(0);
      if (VT.bitsLT(MVT::i32)) {
        VT = MVT::i32;
        Zext = DAG.getNode(ISD::ZERO_EXTEND, dl, VT, Op.getOperand(0));
      }
      unsigned Log2b = Log2_32(VT.getSizeInBits());
      SDValue Clz = DAG.getNode(ISD::CTLZ, dl, VT, Zext);
      SDValue Scc = DAG.getNode(ISD::SRL, dl, VT, Clz,
                                DAG.getConstant(Log2b, dl, MVT::i32));
      return DAG.getNode(ISD::TRUNCATE, dl, MVT::i32, Scc);
    }
  }
  return SDValue();
}
