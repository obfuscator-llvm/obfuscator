//===-- llvm/lib/Target/ARM/ARMCallLowering.cpp - Call lowering -----------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
///
/// \file
/// This file implements the lowering of LLVM calls to machine code calls for
/// GlobalISel.
///
//===----------------------------------------------------------------------===//

#include "ARMCallLowering.h"

#include "ARMBaseInstrInfo.h"
#include "ARMISelLowering.h"
#include "ARMSubtarget.h"

#include "llvm/CodeGen/Analysis.h"
#include "llvm/CodeGen/GlobalISel/MachineIRBuilder.h"
#include "llvm/CodeGen/GlobalISel/Utils.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"

using namespace llvm;

#ifndef LLVM_BUILD_GLOBAL_ISEL
#error "This shouldn't be built without GISel"
#endif

ARMCallLowering::ARMCallLowering(const ARMTargetLowering &TLI)
    : CallLowering(&TLI) {}

static bool isSupportedType(const DataLayout &DL, const ARMTargetLowering &TLI,
                            Type *T) {
  if (T->isArrayTy())
    return true;

  if (T->isStructTy()) {
    // For now we only allow homogeneous structs that we can manipulate with
    // G_MERGE_VALUES and G_UNMERGE_VALUES
    auto StructT = cast<StructType>(T);
    for (unsigned i = 1, e = StructT->getNumElements(); i != e; ++i)
      if (StructT->getElementType(i) != StructT->getElementType(0))
        return false;
    return true;
  }

  EVT VT = TLI.getValueType(DL, T, true);
  if (!VT.isSimple() || VT.isVector() ||
      !(VT.isInteger() || VT.isFloatingPoint()))
    return false;

  unsigned VTSize = VT.getSimpleVT().getSizeInBits();

  if (VTSize == 64)
    // FIXME: Support i64 too
    return VT.isFloatingPoint();

  return VTSize == 1 || VTSize == 8 || VTSize == 16 || VTSize == 32;
}

namespace {
/// Helper class for values going out through an ABI boundary (used for handling
/// function return values and call parameters).
struct OutgoingValueHandler : public CallLowering::ValueHandler {
  OutgoingValueHandler(MachineIRBuilder &MIRBuilder, MachineRegisterInfo &MRI,
                       MachineInstrBuilder &MIB, CCAssignFn *AssignFn)
      : ValueHandler(MIRBuilder, MRI, AssignFn), MIB(MIB), StackSize(0) {}

  unsigned getStackAddress(uint64_t Size, int64_t Offset,
                           MachinePointerInfo &MPO) override {
    assert((Size == 1 || Size == 2 || Size == 4 || Size == 8) &&
           "Unsupported size");

    LLT p0 = LLT::pointer(0, 32);
    LLT s32 = LLT::scalar(32);
    unsigned SPReg = MRI.createGenericVirtualRegister(p0);
    MIRBuilder.buildCopy(SPReg, ARM::SP);

    unsigned OffsetReg = MRI.createGenericVirtualRegister(s32);
    MIRBuilder.buildConstant(OffsetReg, Offset);

    unsigned AddrReg = MRI.createGenericVirtualRegister(p0);
    MIRBuilder.buildGEP(AddrReg, SPReg, OffsetReg);

    MPO = MachinePointerInfo::getStack(MIRBuilder.getMF(), Offset);
    return AddrReg;
  }

  void assignValueToReg(unsigned ValVReg, unsigned PhysReg,
                        CCValAssign &VA) override {
    assert(VA.isRegLoc() && "Value shouldn't be assigned to reg");
    assert(VA.getLocReg() == PhysReg && "Assigning to the wrong reg?");

    assert(VA.getValVT().getSizeInBits() <= 64 && "Unsupported value size");
    assert(VA.getLocVT().getSizeInBits() <= 64 && "Unsupported location size");

    unsigned ExtReg = extendRegister(ValVReg, VA);
    MIRBuilder.buildCopy(PhysReg, ExtReg);
    MIB.addUse(PhysReg, RegState::Implicit);
  }

  void assignValueToAddress(unsigned ValVReg, unsigned Addr, uint64_t Size,
                            MachinePointerInfo &MPO, CCValAssign &VA) override {
    assert((Size == 1 || Size == 2 || Size == 4 || Size == 8) &&
           "Unsupported size");

    unsigned ExtReg = extendRegister(ValVReg, VA);
    auto MMO = MIRBuilder.getMF().getMachineMemOperand(
        MPO, MachineMemOperand::MOStore, VA.getLocVT().getStoreSize(),
        /* Alignment */ 0);
    MIRBuilder.buildStore(ExtReg, Addr, *MMO);
  }

  unsigned assignCustomValue(const CallLowering::ArgInfo &Arg,
                             ArrayRef<CCValAssign> VAs) override {
    CCValAssign VA = VAs[0];
    assert(VA.needsCustom() && "Value doesn't need custom handling");
    assert(VA.getValVT() == MVT::f64 && "Unsupported type");

    CCValAssign NextVA = VAs[1];
    assert(NextVA.needsCustom() && "Value doesn't need custom handling");
    assert(NextVA.getValVT() == MVT::f64 && "Unsupported type");

    assert(VA.getValNo() == NextVA.getValNo() &&
           "Values belong to different arguments");

    assert(VA.isRegLoc() && "Value should be in reg");
    assert(NextVA.isRegLoc() && "Value should be in reg");

    unsigned NewRegs[] = {MRI.createGenericVirtualRegister(LLT::scalar(32)),
                          MRI.createGenericVirtualRegister(LLT::scalar(32))};
    MIRBuilder.buildUnmerge(NewRegs, Arg.Reg);

    bool IsLittle = MIRBuilder.getMF().getSubtarget<ARMSubtarget>().isLittle();
    if (!IsLittle)
      std::swap(NewRegs[0], NewRegs[1]);

    assignValueToReg(NewRegs[0], VA.getLocReg(), VA);
    assignValueToReg(NewRegs[1], NextVA.getLocReg(), NextVA);

    return 1;
  }

  bool assignArg(unsigned ValNo, MVT ValVT, MVT LocVT,
                 CCValAssign::LocInfo LocInfo,
                 const CallLowering::ArgInfo &Info, CCState &State) override {
    if (AssignFn(ValNo, ValVT, LocVT, LocInfo, Info.Flags, State))
      return true;

    StackSize =
        std::max(StackSize, static_cast<uint64_t>(State.getNextStackOffset()));
    return false;
  }

  MachineInstrBuilder &MIB;
  uint64_t StackSize;
};
} // End anonymous namespace.

void ARMCallLowering::splitToValueTypes(
    const ArgInfo &OrigArg, SmallVectorImpl<ArgInfo> &SplitArgs,
    MachineFunction &MF, const SplitArgTy &PerformArgSplit) const {
  const ARMTargetLowering &TLI = *getTLI<ARMTargetLowering>();
  LLVMContext &Ctx = OrigArg.Ty->getContext();
  const DataLayout &DL = MF.getDataLayout();
  MachineRegisterInfo &MRI = MF.getRegInfo();
  const Function *F = MF.getFunction();

  SmallVector<EVT, 4> SplitVTs;
  SmallVector<uint64_t, 4> Offsets;
  ComputeValueVTs(TLI, DL, OrigArg.Ty, SplitVTs, &Offsets, 0);

  if (SplitVTs.size() == 1) {
    // Even if there is no splitting to do, we still want to replace the
    // original type (e.g. pointer type -> integer).
    auto Flags = OrigArg.Flags;
    unsigned OriginalAlignment = DL.getABITypeAlignment(OrigArg.Ty);
    Flags.setOrigAlign(OriginalAlignment);
    SplitArgs.emplace_back(OrigArg.Reg, SplitVTs[0].getTypeForEVT(Ctx), Flags,
                           OrigArg.IsFixed);
    return;
  }

  unsigned FirstRegIdx = SplitArgs.size();
  for (unsigned i = 0, e = SplitVTs.size(); i != e; ++i) {
    EVT SplitVT = SplitVTs[i];
    Type *SplitTy = SplitVT.getTypeForEVT(Ctx);
    auto Flags = OrigArg.Flags;

    unsigned OriginalAlignment = DL.getABITypeAlignment(SplitTy);
    Flags.setOrigAlign(OriginalAlignment);

    bool NeedsConsecutiveRegisters =
        TLI.functionArgumentNeedsConsecutiveRegisters(
            SplitTy, F->getCallingConv(), F->isVarArg());
    if (NeedsConsecutiveRegisters) {
      Flags.setInConsecutiveRegs();
      if (i == e - 1)
        Flags.setInConsecutiveRegsLast();
    }

    SplitArgs.push_back(
        ArgInfo{MRI.createGenericVirtualRegister(getLLTForType(*SplitTy, DL)),
                SplitTy, Flags, OrigArg.IsFixed});
  }

  for (unsigned i = 0; i < Offsets.size(); ++i)
    PerformArgSplit(SplitArgs[FirstRegIdx + i].Reg, Offsets[i] * 8);
}

/// Lower the return value for the already existing \p Ret. This assumes that
/// \p MIRBuilder's insertion point is correct.
bool ARMCallLowering::lowerReturnVal(MachineIRBuilder &MIRBuilder,
                                     const Value *Val, unsigned VReg,
                                     MachineInstrBuilder &Ret) const {
  if (!Val)
    // Nothing to do here.
    return true;

  auto &MF = MIRBuilder.getMF();
  const auto &F = *MF.getFunction();

  auto DL = MF.getDataLayout();
  auto &TLI = *getTLI<ARMTargetLowering>();
  if (!isSupportedType(DL, TLI, Val->getType()))
    return false;

  SmallVector<ArgInfo, 4> SplitVTs;
  SmallVector<unsigned, 4> Regs;
  ArgInfo RetInfo(VReg, Val->getType());
  setArgFlags(RetInfo, AttributeList::ReturnIndex, DL, F);
  splitToValueTypes(RetInfo, SplitVTs, MF, [&](unsigned Reg, uint64_t Offset) {
    Regs.push_back(Reg);
  });

  if (Regs.size() > 1)
    MIRBuilder.buildUnmerge(Regs, VReg);

  CCAssignFn *AssignFn =
      TLI.CCAssignFnForReturn(F.getCallingConv(), F.isVarArg());

  OutgoingValueHandler RetHandler(MIRBuilder, MF.getRegInfo(), Ret, AssignFn);
  return handleAssignments(MIRBuilder, SplitVTs, RetHandler);
}

bool ARMCallLowering::lowerReturn(MachineIRBuilder &MIRBuilder,
                                  const Value *Val, unsigned VReg) const {
  assert(!Val == !VReg && "Return value without a vreg");

  auto const &ST = MIRBuilder.getMF().getSubtarget<ARMSubtarget>();
  unsigned Opcode = ST.getReturnOpcode();
  auto Ret = MIRBuilder.buildInstrNoInsert(Opcode).add(predOps(ARMCC::AL));

  if (!lowerReturnVal(MIRBuilder, Val, VReg, Ret))
    return false;

  MIRBuilder.insertInstr(Ret);
  return true;
}

namespace {
/// Helper class for values coming in through an ABI boundary (used for handling
/// formal arguments and call return values).
struct IncomingValueHandler : public CallLowering::ValueHandler {
  IncomingValueHandler(MachineIRBuilder &MIRBuilder, MachineRegisterInfo &MRI,
                       CCAssignFn AssignFn)
      : ValueHandler(MIRBuilder, MRI, AssignFn) {}

  unsigned getStackAddress(uint64_t Size, int64_t Offset,
                           MachinePointerInfo &MPO) override {
    assert((Size == 1 || Size == 2 || Size == 4 || Size == 8) &&
           "Unsupported size");

    auto &MFI = MIRBuilder.getMF().getFrameInfo();

    int FI = MFI.CreateFixedObject(Size, Offset, true);
    MPO = MachinePointerInfo::getFixedStack(MIRBuilder.getMF(), FI);

    unsigned AddrReg =
        MRI.createGenericVirtualRegister(LLT::pointer(MPO.getAddrSpace(), 32));
    MIRBuilder.buildFrameIndex(AddrReg, FI);

    return AddrReg;
  }

  void assignValueToAddress(unsigned ValVReg, unsigned Addr, uint64_t Size,
                            MachinePointerInfo &MPO, CCValAssign &VA) override {
    assert((Size == 1 || Size == 2 || Size == 4 || Size == 8) &&
           "Unsupported size");

    if (VA.getLocInfo() == CCValAssign::SExt ||
        VA.getLocInfo() == CCValAssign::ZExt) {
      // If the value is zero- or sign-extended, its size becomes 4 bytes, so
      // that's what we should load.
      Size = 4;
      assert(MRI.getType(ValVReg).isScalar() && "Only scalars supported atm");

      auto LoadVReg = MRI.createGenericVirtualRegister(LLT::scalar(32));
      buildLoad(LoadVReg, Addr, Size, /* Alignment */ 0, MPO);
      MIRBuilder.buildTrunc(ValVReg, LoadVReg);
    } else {
      // If the value is not extended, a simple load will suffice.
      buildLoad(ValVReg, Addr, Size, /* Alignment */ 0, MPO);
    }
  }

  void buildLoad(unsigned Val, unsigned Addr, uint64_t Size, unsigned Alignment,
                 MachinePointerInfo &MPO) {
    auto MMO = MIRBuilder.getMF().getMachineMemOperand(
        MPO, MachineMemOperand::MOLoad, Size, Alignment);
    MIRBuilder.buildLoad(Val, Addr, *MMO);
  }

  void assignValueToReg(unsigned ValVReg, unsigned PhysReg,
                        CCValAssign &VA) override {
    assert(VA.isRegLoc() && "Value shouldn't be assigned to reg");
    assert(VA.getLocReg() == PhysReg && "Assigning to the wrong reg?");

    assert(VA.getValVT().getSizeInBits() <= 64 && "Unsupported value size");
    assert(VA.getLocVT().getSizeInBits() <= 64 && "Unsupported location size");

    // The necessary extensions are handled on the other side of the ABI
    // boundary.
    markPhysRegUsed(PhysReg);
    MIRBuilder.buildCopy(ValVReg, PhysReg);
  }

  unsigned assignCustomValue(const ARMCallLowering::ArgInfo &Arg,
                             ArrayRef<CCValAssign> VAs) override {
    CCValAssign VA = VAs[0];
    assert(VA.needsCustom() && "Value doesn't need custom handling");
    assert(VA.getValVT() == MVT::f64 && "Unsupported type");

    CCValAssign NextVA = VAs[1];
    assert(NextVA.needsCustom() && "Value doesn't need custom handling");
    assert(NextVA.getValVT() == MVT::f64 && "Unsupported type");

    assert(VA.getValNo() == NextVA.getValNo() &&
           "Values belong to different arguments");

    assert(VA.isRegLoc() && "Value should be in reg");
    assert(NextVA.isRegLoc() && "Value should be in reg");

    unsigned NewRegs[] = {MRI.createGenericVirtualRegister(LLT::scalar(32)),
                          MRI.createGenericVirtualRegister(LLT::scalar(32))};

    assignValueToReg(NewRegs[0], VA.getLocReg(), VA);
    assignValueToReg(NewRegs[1], NextVA.getLocReg(), NextVA);

    bool IsLittle = MIRBuilder.getMF().getSubtarget<ARMSubtarget>().isLittle();
    if (!IsLittle)
      std::swap(NewRegs[0], NewRegs[1]);

    MIRBuilder.buildMerge(Arg.Reg, NewRegs);

    return 1;
  }

  /// Marking a physical register as used is different between formal
  /// parameters, where it's a basic block live-in, and call returns, where it's
  /// an implicit-def of the call instruction.
  virtual void markPhysRegUsed(unsigned PhysReg) = 0;
};

struct FormalArgHandler : public IncomingValueHandler {
  FormalArgHandler(MachineIRBuilder &MIRBuilder, MachineRegisterInfo &MRI,
                   CCAssignFn AssignFn)
      : IncomingValueHandler(MIRBuilder, MRI, AssignFn) {}

  void markPhysRegUsed(unsigned PhysReg) override {
    MIRBuilder.getMBB().addLiveIn(PhysReg);
  }
};
} // End anonymous namespace

bool ARMCallLowering::lowerFormalArguments(MachineIRBuilder &MIRBuilder,
                                           const Function &F,
                                           ArrayRef<unsigned> VRegs) const {
  // Quick exit if there aren't any args
  if (F.arg_empty())
    return true;

  if (F.isVarArg())
    return false;

  auto &MF = MIRBuilder.getMF();
  auto &MBB = MIRBuilder.getMBB();
  auto DL = MF.getDataLayout();
  auto &TLI = *getTLI<ARMTargetLowering>();

  auto Subtarget = TLI.getSubtarget();

  if (Subtarget->isThumb())
    return false;

  for (auto &Arg : F.args())
    if (!isSupportedType(DL, TLI, Arg.getType()))
      return false;

  CCAssignFn *AssignFn =
      TLI.CCAssignFnForCall(F.getCallingConv(), F.isVarArg());

  FormalArgHandler ArgHandler(MIRBuilder, MIRBuilder.getMF().getRegInfo(),
                              AssignFn);

  SmallVector<ArgInfo, 8> ArgInfos;
  SmallVector<unsigned, 4> SplitRegs;
  unsigned Idx = 0;
  for (auto &Arg : F.args()) {
    ArgInfo AInfo(VRegs[Idx], Arg.getType());
    setArgFlags(AInfo, Idx + AttributeList::FirstArgIndex, DL, F);

    SplitRegs.clear();

    splitToValueTypes(AInfo, ArgInfos, MF, [&](unsigned Reg, uint64_t Offset) {
      SplitRegs.push_back(Reg);
    });

    if (!SplitRegs.empty())
      MIRBuilder.buildMerge(VRegs[Idx], SplitRegs);

    Idx++;
  }

  if (!MBB.empty())
    MIRBuilder.setInstr(*MBB.begin());

  return handleAssignments(MIRBuilder, ArgInfos, ArgHandler);
}

namespace {
struct CallReturnHandler : public IncomingValueHandler {
  CallReturnHandler(MachineIRBuilder &MIRBuilder, MachineRegisterInfo &MRI,
                    MachineInstrBuilder MIB, CCAssignFn *AssignFn)
      : IncomingValueHandler(MIRBuilder, MRI, AssignFn), MIB(MIB) {}

  void markPhysRegUsed(unsigned PhysReg) override {
    MIB.addDef(PhysReg, RegState::Implicit);
  }

  MachineInstrBuilder MIB;
};
} // End anonymous namespace.

bool ARMCallLowering::lowerCall(MachineIRBuilder &MIRBuilder,
                                CallingConv::ID CallConv,
                                const MachineOperand &Callee,
                                const ArgInfo &OrigRet,
                                ArrayRef<ArgInfo> OrigArgs) const {
  MachineFunction &MF = MIRBuilder.getMF();
  const auto &TLI = *getTLI<ARMTargetLowering>();
  const auto &DL = MF.getDataLayout();
  const auto &STI = MF.getSubtarget();
  const TargetRegisterInfo *TRI = STI.getRegisterInfo();
  MachineRegisterInfo &MRI = MF.getRegInfo();

  if (MF.getSubtarget<ARMSubtarget>().genLongCalls())
    return false;

  auto CallSeqStart = MIRBuilder.buildInstr(ARM::ADJCALLSTACKDOWN);

  // Create the call instruction so we can add the implicit uses of arg
  // registers, but don't insert it yet.
  auto MIB = MIRBuilder.buildInstrNoInsert(ARM::BLX).add(Callee).addRegMask(
      TRI->getCallPreservedMask(MF, CallConv));
  if (Callee.isReg()) {
    auto CalleeReg = Callee.getReg();
    if (CalleeReg && !TRI->isPhysicalRegister(CalleeReg))
      MIB->getOperand(0).setReg(constrainOperandRegClass(
          MF, *TRI, MRI, *STI.getInstrInfo(), *STI.getRegBankInfo(),
          *MIB.getInstr(), MIB->getDesc(), CalleeReg, 0));
  }

  SmallVector<ArgInfo, 8> ArgInfos;
  for (auto Arg : OrigArgs) {
    if (!isSupportedType(DL, TLI, Arg.Ty))
      return false;

    if (!Arg.IsFixed)
      return false;

    SmallVector<unsigned, 8> Regs;
    splitToValueTypes(Arg, ArgInfos, MF, [&](unsigned Reg, uint64_t Offset) {
      Regs.push_back(Reg);
    });

    if (Regs.size() > 1)
      MIRBuilder.buildUnmerge(Regs, Arg.Reg);
  }

  auto ArgAssignFn = TLI.CCAssignFnForCall(CallConv, /*IsVarArg=*/false);
  OutgoingValueHandler ArgHandler(MIRBuilder, MRI, MIB, ArgAssignFn);
  if (!handleAssignments(MIRBuilder, ArgInfos, ArgHandler))
    return false;

  // Now we can add the actual call instruction to the correct basic block.
  MIRBuilder.insertInstr(MIB);

  if (!OrigRet.Ty->isVoidTy()) {
    if (!isSupportedType(DL, TLI, OrigRet.Ty))
      return false;

    ArgInfos.clear();
    SmallVector<unsigned, 8> SplitRegs;
    splitToValueTypes(OrigRet, ArgInfos, MF,
                      [&](unsigned Reg, uint64_t Offset) {
                        SplitRegs.push_back(Reg);
                      });

    auto RetAssignFn = TLI.CCAssignFnForReturn(CallConv, /*IsVarArg=*/false);
    CallReturnHandler RetHandler(MIRBuilder, MRI, MIB, RetAssignFn);
    if (!handleAssignments(MIRBuilder, ArgInfos, RetHandler))
      return false;

    if (!SplitRegs.empty()) {
      // We have split the value and allocated each individual piece, now build
      // it up again.
      MIRBuilder.buildMerge(OrigRet.Reg, SplitRegs);
    }
  }

  // We now know the size of the stack - update the ADJCALLSTACKDOWN
  // accordingly.
  CallSeqStart.addImm(ArgHandler.StackSize).addImm(0).add(predOps(ARMCC::AL));

  MIRBuilder.buildInstr(ARM::ADJCALLSTACKUP)
      .addImm(ArgHandler.StackSize)
      .addImm(0)
      .add(predOps(ARMCC::AL));

  return true;
}
