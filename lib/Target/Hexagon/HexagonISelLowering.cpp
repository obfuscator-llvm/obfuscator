//===-- HexagonISelLowering.cpp - Hexagon DAG Lowering Implementation -----===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implements the interfaces that Hexagon uses to lower LLVM code
// into a selection DAG.
//
//===----------------------------------------------------------------------===//

#include "HexagonISelLowering.h"
#include "Hexagon.h"
#include "HexagonMachineFunctionInfo.h"
#include "HexagonRegisterInfo.h"
#include "HexagonSubtarget.h"
#include "HexagonTargetMachine.h"
#include "HexagonTargetObjectFile.h"
#include "llvm/ADT/APInt.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/CodeGen/CallingConvLower.h"
#include "llvm/CodeGen/MachineFrameInfo.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineMemOperand.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/CodeGen/RuntimeLibcalls.h"
#include "llvm/CodeGen/SelectionDAG.h"
#include "llvm/CodeGen/ValueTypes.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/CallingConv.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalValue.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Type.h"
#include "llvm/IR/Value.h"
#include "llvm/MC/MCRegisterInfo.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/CodeGen.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Target/TargetCallingConv.h"
#include "llvm/Target/TargetMachine.h"
#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <utility>

using namespace llvm;

#define DEBUG_TYPE "hexagon-lowering"

static cl::opt<bool> EmitJumpTables("hexagon-emit-jump-tables",
  cl::init(true), cl::Hidden,
  cl::desc("Control jump table emission on Hexagon target"));

static cl::opt<bool> EnableHexSDNodeSched("enable-hexagon-sdnode-sched",
  cl::Hidden, cl::ZeroOrMore, cl::init(false),
  cl::desc("Enable Hexagon SDNode scheduling"));

static cl::opt<bool> EnableFastMath("ffast-math",
  cl::Hidden, cl::ZeroOrMore, cl::init(false),
  cl::desc("Enable Fast Math processing"));

static cl::opt<int> MinimumJumpTables("minimum-jump-tables",
  cl::Hidden, cl::ZeroOrMore, cl::init(5),
  cl::desc("Set minimum jump tables"));

static cl::opt<int> MaxStoresPerMemcpyCL("max-store-memcpy",
  cl::Hidden, cl::ZeroOrMore, cl::init(6),
  cl::desc("Max #stores to inline memcpy"));

static cl::opt<int> MaxStoresPerMemcpyOptSizeCL("max-store-memcpy-Os",
  cl::Hidden, cl::ZeroOrMore, cl::init(4),
  cl::desc("Max #stores to inline memcpy"));

static cl::opt<int> MaxStoresPerMemmoveCL("max-store-memmove",
  cl::Hidden, cl::ZeroOrMore, cl::init(6),
  cl::desc("Max #stores to inline memmove"));

static cl::opt<int> MaxStoresPerMemmoveOptSizeCL("max-store-memmove-Os",
  cl::Hidden, cl::ZeroOrMore, cl::init(4),
  cl::desc("Max #stores to inline memmove"));

static cl::opt<int> MaxStoresPerMemsetCL("max-store-memset",
  cl::Hidden, cl::ZeroOrMore, cl::init(8),
  cl::desc("Max #stores to inline memset"));

static cl::opt<int> MaxStoresPerMemsetOptSizeCL("max-store-memset-Os",
  cl::Hidden, cl::ZeroOrMore, cl::init(4),
  cl::desc("Max #stores to inline memset"));


namespace {

  class HexagonCCState : public CCState {
    unsigned NumNamedVarArgParams;

  public:
    HexagonCCState(CallingConv::ID CC, bool isVarArg, MachineFunction &MF,
                   SmallVectorImpl<CCValAssign> &locs, LLVMContext &C,
                   int NumNamedVarArgParams)
        : CCState(CC, isVarArg, MF, locs, C),
          NumNamedVarArgParams(NumNamedVarArgParams) {}

    unsigned getNumNamedVarArgParams() const { return NumNamedVarArgParams; }
  };

  enum StridedLoadKind {
    Even = 0,
    Odd,
    NoPattern
  };

} // end anonymous namespace

// Implement calling convention for Hexagon.

static bool isHvxVectorType(MVT ty);

static bool
CC_Hexagon(unsigned ValNo, MVT ValVT,
           MVT LocVT, CCValAssign::LocInfo LocInfo,
           ISD::ArgFlagsTy ArgFlags, CCState &State);

static bool
CC_Hexagon32(unsigned ValNo, MVT ValVT,
             MVT LocVT, CCValAssign::LocInfo LocInfo,
             ISD::ArgFlagsTy ArgFlags, CCState &State);

static bool
CC_Hexagon64(unsigned ValNo, MVT ValVT,
             MVT LocVT, CCValAssign::LocInfo LocInfo,
             ISD::ArgFlagsTy ArgFlags, CCState &State);

static bool
CC_HexagonVector(unsigned ValNo, MVT ValVT,
                 MVT LocVT, CCValAssign::LocInfo LocInfo,
                 ISD::ArgFlagsTy ArgFlags, CCState &State);

static bool
RetCC_Hexagon(unsigned ValNo, MVT ValVT,
              MVT LocVT, CCValAssign::LocInfo LocInfo,
              ISD::ArgFlagsTy ArgFlags, CCState &State);

static bool
RetCC_Hexagon32(unsigned ValNo, MVT ValVT,
                MVT LocVT, CCValAssign::LocInfo LocInfo,
                ISD::ArgFlagsTy ArgFlags, CCState &State);

static bool
RetCC_Hexagon64(unsigned ValNo, MVT ValVT,
                MVT LocVT, CCValAssign::LocInfo LocInfo,
                ISD::ArgFlagsTy ArgFlags, CCState &State);

static bool
RetCC_HexagonVector(unsigned ValNo, MVT ValVT,
                    MVT LocVT, CCValAssign::LocInfo LocInfo,
                    ISD::ArgFlagsTy ArgFlags, CCState &State);

static bool
CC_Hexagon_VarArg (unsigned ValNo, MVT ValVT,
            MVT LocVT, CCValAssign::LocInfo LocInfo,
            ISD::ArgFlagsTy ArgFlags, CCState &State) {
  HexagonCCState &HState = static_cast<HexagonCCState &>(State);

  if (ValNo < HState.getNumNamedVarArgParams()) {
    // Deal with named arguments.
    return CC_Hexagon(ValNo, ValVT, LocVT, LocInfo, ArgFlags, State);
  }

  // Deal with un-named arguments.
  unsigned Offset;
  if (ArgFlags.isByVal()) {
    // If pass-by-value, the size allocated on stack is decided
    // by ArgFlags.getByValSize(), not by the size of LocVT.
    Offset = State.AllocateStack(ArgFlags.getByValSize(),
                                 ArgFlags.getByValAlign());
    State.addLoc(CCValAssign::getMem(ValNo, ValVT, Offset, LocVT, LocInfo));
    return false;
  }
  if (LocVT == MVT::i1 || LocVT == MVT::i8 || LocVT == MVT::i16) {
    LocVT = MVT::i32;
    ValVT = MVT::i32;
    if (ArgFlags.isSExt())
      LocInfo = CCValAssign::SExt;
    else if (ArgFlags.isZExt())
      LocInfo = CCValAssign::ZExt;
    else
      LocInfo = CCValAssign::AExt;
  }
  if (LocVT == MVT::i32 || LocVT == MVT::f32) {
    Offset = State.AllocateStack(4, 4);
    State.addLoc(CCValAssign::getMem(ValNo, ValVT, Offset, LocVT, LocInfo));
    return false;
  }
  if (LocVT == MVT::i64 || LocVT == MVT::f64) {
    Offset = State.AllocateStack(8, 8);
    State.addLoc(CCValAssign::getMem(ValNo, ValVT, Offset, LocVT, LocInfo));
    return false;
  }
  if (LocVT == MVT::v2i64 || LocVT == MVT::v4i32 || LocVT == MVT::v8i16 ||
      LocVT == MVT::v16i8) {
    Offset = State.AllocateStack(16, 16);
    State.addLoc(CCValAssign::getMem(ValNo, ValVT, Offset, LocVT, LocInfo));
    return false;
  }
  if (LocVT == MVT::v4i64 || LocVT == MVT::v8i32 || LocVT == MVT::v16i16 ||
      LocVT == MVT::v32i8) {
    Offset = State.AllocateStack(32, 32);
    State.addLoc(CCValAssign::getMem(ValNo, ValVT, Offset, LocVT, LocInfo));
    return false;
  }
  if (LocVT == MVT::v8i64 || LocVT == MVT::v16i32 || LocVT == MVT::v32i16 ||
      LocVT == MVT::v64i8 || LocVT == MVT::v512i1) {
    Offset = State.AllocateStack(64, 64);
    State.addLoc(CCValAssign::getMem(ValNo, ValVT, Offset, LocVT, LocInfo));
    return false;
  }
  if (LocVT == MVT::v16i64 || LocVT == MVT::v32i32 || LocVT == MVT::v64i16 ||
      LocVT == MVT::v128i8 || LocVT == MVT::v1024i1) {
    Offset = State.AllocateStack(128, 128);
    State.addLoc(CCValAssign::getMem(ValNo, ValVT, Offset, LocVT, LocInfo));
    return false;
  }
  if (LocVT == MVT::v32i64 || LocVT == MVT::v64i32 || LocVT == MVT::v128i16 ||
      LocVT == MVT::v256i8) {
    Offset = State.AllocateStack(256, 256);
    State.addLoc(CCValAssign::getMem(ValNo, ValVT, Offset, LocVT, LocInfo));
    return false;
  }

  llvm_unreachable(nullptr);
}

static bool CC_Hexagon (unsigned ValNo, MVT ValVT, MVT LocVT,
      CCValAssign::LocInfo LocInfo, ISD::ArgFlagsTy ArgFlags, CCState &State) {
  if (ArgFlags.isByVal()) {
    // Passed on stack.
    unsigned Offset = State.AllocateStack(ArgFlags.getByValSize(),
                                          ArgFlags.getByValAlign());
    State.addLoc(CCValAssign::getMem(ValNo, ValVT, Offset, LocVT, LocInfo));
    return false;
  }

  if (LocVT == MVT::i1) {
    LocVT = MVT::i32;
  } else if (LocVT == MVT::i8 || LocVT == MVT::i16) {
    LocVT = MVT::i32;
    ValVT = MVT::i32;
    if (ArgFlags.isSExt())
      LocInfo = CCValAssign::SExt;
    else if (ArgFlags.isZExt())
      LocInfo = CCValAssign::ZExt;
    else
      LocInfo = CCValAssign::AExt;
  } else if (LocVT == MVT::v4i8 || LocVT == MVT::v2i16) {
    LocVT = MVT::i32;
    LocInfo = CCValAssign::BCvt;
  } else if (LocVT == MVT::v8i8 || LocVT == MVT::v4i16 || LocVT == MVT::v2i32) {
    LocVT = MVT::i64;
    LocInfo = CCValAssign::BCvt;
  }

  if (LocVT == MVT::i32 || LocVT == MVT::f32) {
    if (!CC_Hexagon32(ValNo, ValVT, LocVT, LocInfo, ArgFlags, State))
      return false;
  }

  if (LocVT == MVT::i64 || LocVT == MVT::f64) {
    if (!CC_Hexagon64(ValNo, ValVT, LocVT, LocInfo, ArgFlags, State))
      return false;
  }

  if (LocVT == MVT::v8i32 || LocVT == MVT::v16i16 || LocVT == MVT::v32i8) {
    unsigned Offset = State.AllocateStack(ArgFlags.getByValSize(), 32);
    State.addLoc(CCValAssign::getMem(ValNo, ValVT, Offset, LocVT, LocInfo));
    return false;
  }

  if (isHvxVectorType(LocVT)) {
    if (!CC_HexagonVector(ValNo, ValVT, LocVT, LocInfo, ArgFlags, State))
      return false;
  }

  return true;  // CC didn't match.
}


static bool CC_Hexagon32(unsigned ValNo, MVT ValVT,
                         MVT LocVT, CCValAssign::LocInfo LocInfo,
                         ISD::ArgFlagsTy ArgFlags, CCState &State) {
  static const MCPhysReg RegList[] = {
    Hexagon::R0, Hexagon::R1, Hexagon::R2, Hexagon::R3, Hexagon::R4,
    Hexagon::R5
  };
  if (unsigned Reg = State.AllocateReg(RegList)) {
    State.addLoc(CCValAssign::getReg(ValNo, ValVT, Reg, LocVT, LocInfo));
    return false;
  }

  unsigned Offset = State.AllocateStack(4, 4);
  State.addLoc(CCValAssign::getMem(ValNo, ValVT, Offset, LocVT, LocInfo));
  return false;
}

static bool CC_Hexagon64(unsigned ValNo, MVT ValVT,
                         MVT LocVT, CCValAssign::LocInfo LocInfo,
                         ISD::ArgFlagsTy ArgFlags, CCState &State) {
  if (unsigned Reg = State.AllocateReg(Hexagon::D0)) {
    State.addLoc(CCValAssign::getReg(ValNo, ValVT, Reg, LocVT, LocInfo));
    return false;
  }

  static const MCPhysReg RegList1[] = {
    Hexagon::D1, Hexagon::D2
  };
  static const MCPhysReg RegList2[] = {
    Hexagon::R1, Hexagon::R3
  };
  if (unsigned Reg = State.AllocateReg(RegList1, RegList2)) {
    State.addLoc(CCValAssign::getReg(ValNo, ValVT, Reg, LocVT, LocInfo));
    return false;
  }

  unsigned Offset = State.AllocateStack(8, 8, Hexagon::D2);
  State.addLoc(CCValAssign::getMem(ValNo, ValVT, Offset, LocVT, LocInfo));
  return false;
}

static bool CC_HexagonVector(unsigned ValNo, MVT ValVT,
                             MVT LocVT, CCValAssign::LocInfo LocInfo,
                             ISD::ArgFlagsTy ArgFlags, CCState &State) {
  static const MCPhysReg VecLstS[] = {
      Hexagon::V0, Hexagon::V1, Hexagon::V2, Hexagon::V3, Hexagon::V4,
      Hexagon::V5, Hexagon::V6, Hexagon::V7, Hexagon::V8, Hexagon::V9,
      Hexagon::V10, Hexagon::V11, Hexagon::V12, Hexagon::V13, Hexagon::V14,
      Hexagon::V15
  };
  static const MCPhysReg VecLstD[] = {
      Hexagon::W0, Hexagon::W1, Hexagon::W2, Hexagon::W3, Hexagon::W4,
      Hexagon::W5, Hexagon::W6, Hexagon::W7
  };
  auto &MF = State.getMachineFunction();
  auto &HST = MF.getSubtarget<HexagonSubtarget>();
  bool UseHVX = HST.useHVXOps();
  bool UseHVXDbl = HST.useHVXDblOps();

  if ((UseHVX && !UseHVXDbl) &&
      (LocVT == MVT::v8i64 || LocVT == MVT::v16i32 || LocVT == MVT::v32i16 ||
       LocVT == MVT::v64i8 || LocVT == MVT::v512i1)) {
    if (unsigned Reg = State.AllocateReg(VecLstS)) {
      State.addLoc(CCValAssign::getReg(ValNo, ValVT, Reg, LocVT, LocInfo));
      return false;
    }
    unsigned Offset = State.AllocateStack(64, 64);
    State.addLoc(CCValAssign::getMem(ValNo, ValVT, Offset, LocVT, LocInfo));
    return false;
  }
  if ((UseHVX && !UseHVXDbl) &&
      (LocVT == MVT::v16i64 || LocVT == MVT::v32i32 || LocVT == MVT::v64i16 ||
       LocVT == MVT::v128i8)) {
    if (unsigned Reg = State.AllocateReg(VecLstD)) {
      State.addLoc(CCValAssign::getReg(ValNo, ValVT, Reg, LocVT, LocInfo));
      return false;
    }
    unsigned Offset = State.AllocateStack(128, 128);
    State.addLoc(CCValAssign::getMem(ValNo, ValVT, Offset, LocVT, LocInfo));
    return false;
  }
  // 128B Mode
  if ((UseHVX && UseHVXDbl) &&
      (LocVT == MVT::v32i64 || LocVT == MVT::v64i32 || LocVT == MVT::v128i16 ||
       LocVT == MVT::v256i8)) {
    if (unsigned Reg = State.AllocateReg(VecLstD)) {
      State.addLoc(CCValAssign::getReg(ValNo, ValVT, Reg, LocVT, LocInfo));
      return false;
    }
    unsigned Offset = State.AllocateStack(256, 256);
    State.addLoc(CCValAssign::getMem(ValNo, ValVT, Offset, LocVT, LocInfo));
    return false;
  }
  if ((UseHVX && UseHVXDbl) &&
      (LocVT == MVT::v16i64 || LocVT == MVT::v32i32 || LocVT == MVT::v64i16 ||
       LocVT == MVT::v128i8 || LocVT == MVT::v1024i1)) {
    if (unsigned Reg = State.AllocateReg(VecLstS)) {
      State.addLoc(CCValAssign::getReg(ValNo, ValVT, Reg, LocVT, LocInfo));
      return false;
    }
    unsigned Offset = State.AllocateStack(128, 128);
    State.addLoc(CCValAssign::getMem(ValNo, ValVT, Offset, LocVT, LocInfo));
    return false;
  }
  return true;
}

static bool RetCC_Hexagon(unsigned ValNo, MVT ValVT,
                          MVT LocVT, CCValAssign::LocInfo LocInfo,
                          ISD::ArgFlagsTy ArgFlags, CCState &State) {
  auto &MF = State.getMachineFunction();
  auto &HST = MF.getSubtarget<HexagonSubtarget>();
  bool UseHVX = HST.useHVXOps();
  bool UseHVXDbl = HST.useHVXDblOps();

  if (LocVT == MVT::i1) {
    // Return values of type MVT::i1 still need to be assigned to R0, but
    // the value type needs to remain i1. LowerCallResult will deal with it,
    // but it needs to recognize i1 as the value type.
    LocVT = MVT::i32;
  } else if (LocVT == MVT::i8 || LocVT == MVT::i16) {
    LocVT = MVT::i32;
    ValVT = MVT::i32;
    if (ArgFlags.isSExt())
      LocInfo = CCValAssign::SExt;
    else if (ArgFlags.isZExt())
      LocInfo = CCValAssign::ZExt;
    else
      LocInfo = CCValAssign::AExt;
  } else if (LocVT == MVT::v4i8 || LocVT == MVT::v2i16) {
    LocVT = MVT::i32;
    LocInfo = CCValAssign::BCvt;
  } else if (LocVT == MVT::v8i8 || LocVT == MVT::v4i16 || LocVT == MVT::v2i32) {
    LocVT = MVT::i64;
    LocInfo = CCValAssign::BCvt;
  } else if (LocVT == MVT::v64i8 || LocVT == MVT::v32i16 ||
             LocVT == MVT::v16i32 || LocVT == MVT::v8i64 ||
             LocVT == MVT::v512i1) {
    LocVT = MVT::v16i32;
    ValVT = MVT::v16i32;
    LocInfo = CCValAssign::Full;
  } else if (LocVT == MVT::v128i8 || LocVT == MVT::v64i16 ||
             LocVT == MVT::v32i32 || LocVT == MVT::v16i64 ||
             (LocVT == MVT::v1024i1 && UseHVX && UseHVXDbl)) {
    LocVT = MVT::v32i32;
    ValVT = MVT::v32i32;
    LocInfo = CCValAssign::Full;
  } else if (LocVT == MVT::v256i8 || LocVT == MVT::v128i16 ||
             LocVT == MVT::v64i32 || LocVT == MVT::v32i64) {
    LocVT = MVT::v64i32;
    ValVT = MVT::v64i32;
    LocInfo = CCValAssign::Full;
  }
  if (LocVT == MVT::i32 || LocVT == MVT::f32) {
    if (!RetCC_Hexagon32(ValNo, ValVT, LocVT, LocInfo, ArgFlags, State))
      return false;
  }

  if (LocVT == MVT::i64 || LocVT == MVT::f64) {
    if (!RetCC_Hexagon64(ValNo, ValVT, LocVT, LocInfo, ArgFlags, State))
      return false;
  }
  if (LocVT == MVT::v16i32 || LocVT == MVT::v32i32 || LocVT == MVT::v64i32) {
    if (!RetCC_HexagonVector(ValNo, ValVT, LocVT, LocInfo, ArgFlags, State))
      return false;
  }
  return true;  // CC didn't match.
}

static bool RetCC_Hexagon32(unsigned ValNo, MVT ValVT,
                            MVT LocVT, CCValAssign::LocInfo LocInfo,
                            ISD::ArgFlagsTy ArgFlags, CCState &State) {
  if (LocVT == MVT::i32 || LocVT == MVT::f32) {
    // Note that use of registers beyond R1 is not ABI compliant. However there
    // are (experimental) IR passes which generate internal functions that
    // return structs using these additional registers.
    static const uint16_t RegList[] = { Hexagon::R0, Hexagon::R1,
                                        Hexagon::R2, Hexagon::R3,
                                        Hexagon::R4, Hexagon::R5 };
    if (unsigned Reg = State.AllocateReg(RegList)) {
      State.addLoc(CCValAssign::getReg(ValNo, ValVT, Reg, LocVT, LocInfo));
      return false;
    }
  }

  return true;
}

static bool RetCC_Hexagon64(unsigned ValNo, MVT ValVT,
                            MVT LocVT, CCValAssign::LocInfo LocInfo,
                            ISD::ArgFlagsTy ArgFlags, CCState &State) {
  if (LocVT == MVT::i64 || LocVT == MVT::f64) {
    if (unsigned Reg = State.AllocateReg(Hexagon::D0)) {
      State.addLoc(CCValAssign::getReg(ValNo, ValVT, Reg, LocVT, LocInfo));
      return false;
    }
  }

  return true;
}

static bool RetCC_HexagonVector(unsigned ValNo, MVT ValVT,
                                MVT LocVT, CCValAssign::LocInfo LocInfo,
                                ISD::ArgFlagsTy ArgFlags, CCState &State) {
  auto &MF = State.getMachineFunction();
  auto &HST = MF.getSubtarget<HexagonSubtarget>();
  bool UseHVX = HST.useHVXOps();
  bool UseHVXDbl = HST.useHVXDblOps();

  if (LocVT == MVT::v16i32) {
    if (unsigned Reg = State.AllocateReg(Hexagon::V0)) {
      State.addLoc(CCValAssign::getReg(ValNo, ValVT, Reg, LocVT, LocInfo));
      return false;
    }
  } else if (LocVT == MVT::v32i32) {
    unsigned Req = (UseHVX && UseHVXDbl) ? Hexagon::V0 : Hexagon::W0;
    if (unsigned Reg = State.AllocateReg(Req)) {
      State.addLoc(CCValAssign::getReg(ValNo, ValVT, Reg, LocVT, LocInfo));
      return false;
    }
  } else if (LocVT == MVT::v64i32) {
    if (unsigned Reg = State.AllocateReg(Hexagon::W0)) {
      State.addLoc(CCValAssign::getReg(ValNo, ValVT, Reg, LocVT, LocInfo));
      return false;
    }
  }

  return true;
}

void HexagonTargetLowering::promoteLdStType(MVT VT, MVT PromotedLdStVT) {
  if (VT != PromotedLdStVT) {
    setOperationAction(ISD::LOAD, VT, Promote);
    AddPromotedToType(ISD::LOAD, VT, PromotedLdStVT);

    setOperationAction(ISD::STORE, VT, Promote);
    AddPromotedToType(ISD::STORE, VT, PromotedLdStVT);
  }
}

SDValue
HexagonTargetLowering::LowerINTRINSIC_WO_CHAIN(SDValue Op, SelectionDAG &DAG)
      const {
  return SDValue();
}

/// CreateCopyOfByValArgument - Make a copy of an aggregate at address specified
/// by "Src" to address "Dst" of size "Size".  Alignment information is
/// specified by the specific parameter attribute. The copy will be passed as
/// a byval function parameter.  Sometimes what we are copying is the end of a
/// larger object, the part that does not fit in registers.
static SDValue CreateCopyOfByValArgument(SDValue Src, SDValue Dst,
                                         SDValue Chain, ISD::ArgFlagsTy Flags,
                                         SelectionDAG &DAG, const SDLoc &dl) {
  SDValue SizeNode = DAG.getConstant(Flags.getByValSize(), dl, MVT::i32);
  return DAG.getMemcpy(Chain, dl, Dst, Src, SizeNode, Flags.getByValAlign(),
                       /*isVolatile=*/false, /*AlwaysInline=*/false,
                       /*isTailCall=*/false,
                       MachinePointerInfo(), MachinePointerInfo());
}

static bool isHvxVectorType(MVT Ty) {
  switch (Ty.SimpleTy) {
  case MVT::v8i64:
  case MVT::v16i32:
  case MVT::v32i16:
  case MVT::v64i8:
  case MVT::v16i64:
  case MVT::v32i32:
  case MVT::v64i16:
  case MVT::v128i8:
  case MVT::v32i64:
  case MVT::v64i32:
  case MVT::v128i16:
  case MVT::v256i8:
  case MVT::v512i1:
  case MVT::v1024i1:
    return true;
  default:
    return false;
  }
}

bool
HexagonTargetLowering::CanLowerReturn(
    CallingConv::ID CallConv, MachineFunction &MF, bool isVarArg,
    const SmallVectorImpl<ISD::OutputArg> &Outs,
    LLVMContext &Context) const {
  SmallVector<CCValAssign, 16> RVLocs;
  CCState CCInfo(CallConv, isVarArg, MF, RVLocs, Context);
  return CCInfo.CheckReturn(Outs, RetCC_Hexagon);
}

// LowerReturn - Lower ISD::RET. If a struct is larger than 8 bytes and is
// passed by value, the function prototype is modified to return void and
// the value is stored in memory pointed by a pointer passed by caller.
SDValue
HexagonTargetLowering::LowerReturn(SDValue Chain, CallingConv::ID CallConv,
                                   bool isVarArg,
                                   const SmallVectorImpl<ISD::OutputArg> &Outs,
                                   const SmallVectorImpl<SDValue> &OutVals,
                                   const SDLoc &dl, SelectionDAG &DAG) const {
  // CCValAssign - represent the assignment of the return value to locations.
  SmallVector<CCValAssign, 16> RVLocs;

  // CCState - Info about the registers and stack slot.
  CCState CCInfo(CallConv, isVarArg, DAG.getMachineFunction(), RVLocs,
                 *DAG.getContext());

  // Analyze return values of ISD::RET
  CCInfo.AnalyzeReturn(Outs, RetCC_Hexagon);

  SDValue Flag;
  SmallVector<SDValue, 4> RetOps(1, Chain);

  // Copy the result values into the output registers.
  for (unsigned i = 0; i != RVLocs.size(); ++i) {
    CCValAssign &VA = RVLocs[i];

    Chain = DAG.getCopyToReg(Chain, dl, VA.getLocReg(), OutVals[i], Flag);

    // Guarantee that all emitted copies are stuck together with flags.
    Flag = Chain.getValue(1);
    RetOps.push_back(DAG.getRegister(VA.getLocReg(), VA.getLocVT()));
  }

  RetOps[0] = Chain;  // Update chain.

  // Add the flag if we have it.
  if (Flag.getNode())
    RetOps.push_back(Flag);

  return DAG.getNode(HexagonISD::RET_FLAG, dl, MVT::Other, RetOps);
}

bool HexagonTargetLowering::mayBeEmittedAsTailCall(const CallInst *CI) const {
  // If either no tail call or told not to tail call at all, don't.
  auto Attr =
      CI->getParent()->getParent()->getFnAttribute("disable-tail-calls");
  if (!CI->isTailCall() || Attr.getValueAsString() == "true")
    return false;

  return true;
}

/// LowerCallResult - Lower the result values of an ISD::CALL into the
/// appropriate copies out of appropriate physical registers.  This assumes that
/// Chain/Glue are the input chain/glue to use, and that TheCall is the call
/// being lowered. Returns a SDNode with the same number of values as the
/// ISD::CALL.
SDValue HexagonTargetLowering::LowerCallResult(
    SDValue Chain, SDValue Glue, CallingConv::ID CallConv, bool isVarArg,
    const SmallVectorImpl<ISD::InputArg> &Ins, const SDLoc &dl,
    SelectionDAG &DAG, SmallVectorImpl<SDValue> &InVals,
    const SmallVectorImpl<SDValue> &OutVals, SDValue Callee) const {
  // Assign locations to each value returned by this call.
  SmallVector<CCValAssign, 16> RVLocs;

  CCState CCInfo(CallConv, isVarArg, DAG.getMachineFunction(), RVLocs,
                 *DAG.getContext());

  CCInfo.AnalyzeCallResult(Ins, RetCC_Hexagon);

  // Copy all of the result registers out of their specified physreg.
  for (unsigned i = 0; i != RVLocs.size(); ++i) {
    SDValue RetVal;
    if (RVLocs[i].getValVT() == MVT::i1) {
      // Return values of type MVT::i1 require special handling. The reason
      // is that MVT::i1 is associated with the PredRegs register class, but
      // values of that type are still returned in R0. Generate an explicit
      // copy into a predicate register from R0, and treat the value of the
      // predicate register as the call result.
      auto &MRI = DAG.getMachineFunction().getRegInfo();
      SDValue FR0 = DAG.getCopyFromReg(Chain, dl, RVLocs[i].getLocReg(),
                                       MVT::i32, Glue);
      // FR0 = (Value, Chain, Glue)
      unsigned PredR = MRI.createVirtualRegister(&Hexagon::PredRegsRegClass);
      SDValue TPR = DAG.getCopyToReg(FR0.getValue(1), dl, PredR,
                                     FR0.getValue(0), FR0.getValue(2));
      // TPR = (Chain, Glue)
      // Don't glue this CopyFromReg, because it copies from a virtual
      // register. If it is glued to the call, InstrEmitter will add it
      // as an implicit def to the call (EmitMachineNode).
      RetVal = DAG.getCopyFromReg(TPR.getValue(0), dl, PredR, MVT::i1);
      Glue = TPR.getValue(1);
    } else {
      RetVal = DAG.getCopyFromReg(Chain, dl, RVLocs[i].getLocReg(),
                                  RVLocs[i].getValVT(), Glue);
      Glue = RetVal.getValue(2);
    }
    InVals.push_back(RetVal.getValue(0));
    Chain = RetVal.getValue(1);
  }

  return Chain;
}

/// LowerCall - Functions arguments are copied from virtual regs to
/// (physical regs)/(stack frame), CALLSEQ_START and CALLSEQ_END are emitted.
SDValue
HexagonTargetLowering::LowerCall(TargetLowering::CallLoweringInfo &CLI,
                                 SmallVectorImpl<SDValue> &InVals) const {
  SelectionDAG &DAG                     = CLI.DAG;
  SDLoc &dl                             = CLI.DL;
  SmallVectorImpl<ISD::OutputArg> &Outs = CLI.Outs;
  SmallVectorImpl<SDValue> &OutVals     = CLI.OutVals;
  SmallVectorImpl<ISD::InputArg> &Ins   = CLI.Ins;
  SDValue Chain                         = CLI.Chain;
  SDValue Callee                        = CLI.Callee;
  bool &IsTailCall                      = CLI.IsTailCall;
  CallingConv::ID CallConv              = CLI.CallConv;
  bool IsVarArg                         = CLI.IsVarArg;
  bool DoesNotReturn                    = CLI.DoesNotReturn;

  bool IsStructRet    = (Outs.empty()) ? false : Outs[0].Flags.isSRet();
  MachineFunction &MF = DAG.getMachineFunction();
  MachineFrameInfo &MFI = MF.getFrameInfo();
  auto PtrVT = getPointerTy(MF.getDataLayout());

  // Check for varargs.
  unsigned NumNamedVarArgParams = -1U;
  if (GlobalAddressSDNode *GAN = dyn_cast<GlobalAddressSDNode>(Callee)) {
    const GlobalValue *GV = GAN->getGlobal();
    Callee = DAG.getTargetGlobalAddress(GV, dl, MVT::i32);
    if (const Function* F = dyn_cast<Function>(GV)) {
      // If a function has zero args and is a vararg function, that's
      // disallowed so it must be an undeclared function.  Do not assume
      // varargs if the callee is undefined.
      if (F->isVarArg() && F->getFunctionType()->getNumParams() != 0)
        NumNamedVarArgParams = F->getFunctionType()->getNumParams();
    }
  }

  // Analyze operands of the call, assigning locations to each operand.
  SmallVector<CCValAssign, 16> ArgLocs;
  HexagonCCState CCInfo(CallConv, IsVarArg, DAG.getMachineFunction(), ArgLocs,
                        *DAG.getContext(), NumNamedVarArgParams);

  if (IsVarArg)
    CCInfo.AnalyzeCallOperands(Outs, CC_Hexagon_VarArg);
  else
    CCInfo.AnalyzeCallOperands(Outs, CC_Hexagon);

  auto Attr = MF.getFunction()->getFnAttribute("disable-tail-calls");
  if (Attr.getValueAsString() == "true")
    IsTailCall = false;

  if (IsTailCall) {
    bool StructAttrFlag = MF.getFunction()->hasStructRetAttr();
    IsTailCall = IsEligibleForTailCallOptimization(Callee, CallConv,
                                                   IsVarArg, IsStructRet,
                                                   StructAttrFlag,
                                                   Outs, OutVals, Ins, DAG);
    for (unsigned i = 0, e = ArgLocs.size(); i != e; ++i) {
      CCValAssign &VA = ArgLocs[i];
      if (VA.isMemLoc()) {
        IsTailCall = false;
        break;
      }
    }
    DEBUG(dbgs() << (IsTailCall ? "Eligible for Tail Call\n"
                                : "Argument must be passed on stack. "
                                  "Not eligible for Tail Call\n"));
  }
  // Get a count of how many bytes are to be pushed on the stack.
  unsigned NumBytes = CCInfo.getNextStackOffset();
  SmallVector<std::pair<unsigned, SDValue>, 16> RegsToPass;
  SmallVector<SDValue, 8> MemOpChains;

  auto &HRI = *Subtarget.getRegisterInfo();
  SDValue StackPtr =
      DAG.getCopyFromReg(Chain, dl, HRI.getStackRegister(), PtrVT);

  bool NeedsArgAlign = false;
  unsigned LargestAlignSeen = 0;
  // Walk the register/memloc assignments, inserting copies/loads.
  for (unsigned i = 0, e = ArgLocs.size(); i != e; ++i) {
    CCValAssign &VA = ArgLocs[i];
    SDValue Arg = OutVals[i];
    ISD::ArgFlagsTy Flags = Outs[i].Flags;
    // Record if we need > 8 byte alignment on an argument.
    bool ArgAlign = isHvxVectorType(VA.getValVT());
    NeedsArgAlign |= ArgAlign;

    // Promote the value if needed.
    switch (VA.getLocInfo()) {
      default:
        // Loc info must be one of Full, SExt, ZExt, or AExt.
        llvm_unreachable("Unknown loc info!");
      case CCValAssign::BCvt:
      case CCValAssign::Full:
        break;
      case CCValAssign::SExt:
        Arg = DAG.getNode(ISD::SIGN_EXTEND, dl, VA.getLocVT(), Arg);
        break;
      case CCValAssign::ZExt:
        Arg = DAG.getNode(ISD::ZERO_EXTEND, dl, VA.getLocVT(), Arg);
        break;
      case CCValAssign::AExt:
        Arg = DAG.getNode(ISD::ANY_EXTEND, dl, VA.getLocVT(), Arg);
        break;
    }

    if (VA.isMemLoc()) {
      unsigned LocMemOffset = VA.getLocMemOffset();
      SDValue MemAddr = DAG.getConstant(LocMemOffset, dl,
                                        StackPtr.getValueType());
      MemAddr = DAG.getNode(ISD::ADD, dl, MVT::i32, StackPtr, MemAddr);
      if (ArgAlign)
        LargestAlignSeen = std::max(LargestAlignSeen,
                                    VA.getLocVT().getStoreSizeInBits() >> 3);
      if (Flags.isByVal()) {
        // The argument is a struct passed by value. According to LLVM, "Arg"
        // is is pointer.
        MemOpChains.push_back(CreateCopyOfByValArgument(Arg, MemAddr, Chain,
                                                        Flags, DAG, dl));
      } else {
        MachinePointerInfo LocPI = MachinePointerInfo::getStack(
            DAG.getMachineFunction(), LocMemOffset);
        SDValue S = DAG.getStore(Chain, dl, Arg, MemAddr, LocPI);
        MemOpChains.push_back(S);
      }
      continue;
    }

    // Arguments that can be passed on register must be kept at RegsToPass
    // vector.
    if (VA.isRegLoc())
      RegsToPass.push_back(std::make_pair(VA.getLocReg(), Arg));
  }

  if (NeedsArgAlign && Subtarget.hasV60TOps()) {
    DEBUG(dbgs() << "Function needs byte stack align due to call args\n");
    // V6 vectors passed by value have 64 or 128 byte alignment depending
    // on whether we are 64 byte vector mode or 128 byte.
    bool UseHVXDbl = Subtarget.useHVXDblOps();
    assert(Subtarget.useHVXOps());
    const unsigned ObjAlign = UseHVXDbl ? 128 : 64;
    LargestAlignSeen = std::max(LargestAlignSeen, ObjAlign);
    MFI.ensureMaxAlignment(LargestAlignSeen);
  }
  // Transform all store nodes into one single node because all store
  // nodes are independent of each other.
  if (!MemOpChains.empty())
    Chain = DAG.getNode(ISD::TokenFactor, dl, MVT::Other, MemOpChains);

  SDValue Glue;
  if (!IsTailCall) {
    Chain = DAG.getCALLSEQ_START(Chain, NumBytes, 0, dl);
    Glue = Chain.getValue(1);
  }

  // Build a sequence of copy-to-reg nodes chained together with token
  // chain and flag operands which copy the outgoing args into registers.
  // The Glue is necessary since all emitted instructions must be
  // stuck together.
  if (!IsTailCall) {
    for (unsigned i = 0, e = RegsToPass.size(); i != e; ++i) {
      Chain = DAG.getCopyToReg(Chain, dl, RegsToPass[i].first,
                               RegsToPass[i].second, Glue);
      Glue = Chain.getValue(1);
    }
  } else {
    // For tail calls lower the arguments to the 'real' stack slot.
    //
    // Force all the incoming stack arguments to be loaded from the stack
    // before any new outgoing arguments are stored to the stack, because the
    // outgoing stack slots may alias the incoming argument stack slots, and
    // the alias isn't otherwise explicit. This is slightly more conservative
    // than necessary, because it means that each store effectively depends
    // on every argument instead of just those arguments it would clobber.
    //
    // Do not flag preceding copytoreg stuff together with the following stuff.
    Glue = SDValue();
    for (unsigned i = 0, e = RegsToPass.size(); i != e; ++i) {
      Chain = DAG.getCopyToReg(Chain, dl, RegsToPass[i].first,
                               RegsToPass[i].second, Glue);
      Glue = Chain.getValue(1);
    }
    Glue = SDValue();
  }

  bool LongCalls = MF.getSubtarget<HexagonSubtarget>().useLongCalls();
  unsigned Flags = LongCalls ? HexagonII::HMOTF_ConstExtended : 0;

  // If the callee is a GlobalAddress/ExternalSymbol node (quite common, every
  // direct call is) turn it into a TargetGlobalAddress/TargetExternalSymbol
  // node so that legalize doesn't hack it.
  if (GlobalAddressSDNode *G = dyn_cast<GlobalAddressSDNode>(Callee)) {
    Callee = DAG.getTargetGlobalAddress(G->getGlobal(), dl, PtrVT, 0, Flags);
  } else if (ExternalSymbolSDNode *S =
             dyn_cast<ExternalSymbolSDNode>(Callee)) {
    Callee = DAG.getTargetExternalSymbol(S->getSymbol(), PtrVT, Flags);
  }

  // Returns a chain & a flag for retval copy to use.
  SDVTList NodeTys = DAG.getVTList(MVT::Other, MVT::Glue);
  SmallVector<SDValue, 8> Ops;
  Ops.push_back(Chain);
  Ops.push_back(Callee);

  // Add argument registers to the end of the list so that they are
  // known live into the call.
  for (unsigned i = 0, e = RegsToPass.size(); i != e; ++i) {
    Ops.push_back(DAG.getRegister(RegsToPass[i].first,
                                  RegsToPass[i].second.getValueType()));
  }

  const uint32_t *Mask = HRI.getCallPreservedMask(MF, CallConv);
  assert(Mask && "Missing call preserved mask for calling convention");
  Ops.push_back(DAG.getRegisterMask(Mask));

  if (Glue.getNode())
    Ops.push_back(Glue);

  if (IsTailCall) {
    MFI.setHasTailCall();
    return DAG.getNode(HexagonISD::TC_RETURN, dl, NodeTys, Ops);
  }

  // Set this here because we need to know this for "hasFP" in frame lowering.
  // The target-independent code calls getFrameRegister before setting it, and
  // getFrameRegister uses hasFP to determine whether the function has FP.
  MFI.setHasCalls(true);

  unsigned OpCode = DoesNotReturn ? HexagonISD::CALLnr : HexagonISD::CALL;
  Chain = DAG.getNode(OpCode, dl, NodeTys, Ops);
  Glue = Chain.getValue(1);

  // Create the CALLSEQ_END node.
  Chain = DAG.getCALLSEQ_END(Chain, DAG.getIntPtrConstant(NumBytes, dl, true),
                             DAG.getIntPtrConstant(0, dl, true), Glue, dl);
  Glue = Chain.getValue(1);

  // Handle result values, copying them out of physregs into vregs that we
  // return.
  return LowerCallResult(Chain, Glue, CallConv, IsVarArg, Ins, dl, DAG,
                         InVals, OutVals, Callee);
}

static bool getIndexedAddressParts(SDNode *Ptr, EVT VT,
                                   SDValue &Base, SDValue &Offset,
                                   bool &IsInc, SelectionDAG &DAG) {
  if (Ptr->getOpcode() != ISD::ADD)
    return false;

  auto &HST = static_cast<const HexagonSubtarget&>(DAG.getSubtarget());
  bool UseHVX = HST.useHVXOps();
  bool UseHVXDbl = HST.useHVXDblOps();

  bool ValidHVXDblType =
    (UseHVX && UseHVXDbl) && (VT == MVT::v32i32 || VT == MVT::v16i64 ||
                              VT == MVT::v64i16 || VT == MVT::v128i8);
  bool ValidHVXType =
    UseHVX && !UseHVXDbl && (VT == MVT::v16i32 || VT == MVT::v8i64 ||
                             VT == MVT::v32i16 || VT == MVT::v64i8);

  if (ValidHVXDblType || ValidHVXType ||
      VT == MVT::i64 || VT == MVT::i32 || VT == MVT::i16 || VT == MVT::i8) {
    IsInc = (Ptr->getOpcode() == ISD::ADD);
    Base = Ptr->getOperand(0);
    Offset = Ptr->getOperand(1);
    // Ensure that Offset is a constant.
    return isa<ConstantSDNode>(Offset);
  }

  return false;
}

/// getPostIndexedAddressParts - returns true by value, base pointer and
/// offset pointer and addressing mode by reference if this node can be
/// combined with a load / store to form a post-indexed load / store.
bool HexagonTargetLowering::getPostIndexedAddressParts(SDNode *N, SDNode *Op,
                                                       SDValue &Base,
                                                       SDValue &Offset,
                                                       ISD::MemIndexedMode &AM,
                                                       SelectionDAG &DAG) const
{
  EVT VT;
  SDValue Ptr;

  if (LoadSDNode *LD = dyn_cast<LoadSDNode>(N)) {
    VT  = LD->getMemoryVT();
  } else if (StoreSDNode *ST = dyn_cast<StoreSDNode>(N)) {
    VT  = ST->getMemoryVT();
    if (ST->getValue().getValueType() == MVT::i64 && ST->isTruncatingStore())
      return false;
  } else {
    return false;
  }

  bool IsInc = false;
  bool isLegal = getIndexedAddressParts(Op, VT, Base, Offset, IsInc, DAG);
  if (isLegal) {
    auto &HII = *Subtarget.getInstrInfo();
    int32_t OffsetVal = cast<ConstantSDNode>(Offset.getNode())->getSExtValue();
    if (HII.isValidAutoIncImm(VT, OffsetVal)) {
      AM = IsInc ? ISD::POST_INC : ISD::POST_DEC;
      return true;
    }
  }

  return false;
}

SDValue
HexagonTargetLowering::LowerINLINEASM(SDValue Op, SelectionDAG &DAG) const {
  MachineFunction &MF = DAG.getMachineFunction();
  auto &HMFI = *MF.getInfo<HexagonMachineFunctionInfo>();
  const HexagonRegisterInfo &HRI = *Subtarget.getRegisterInfo();
  unsigned LR = HRI.getRARegister();

  if (Op.getOpcode() != ISD::INLINEASM || HMFI.hasClobberLR())
    return Op;

  unsigned NumOps = Op.getNumOperands();
  if (Op.getOperand(NumOps-1).getValueType() == MVT::Glue)
    --NumOps;  // Ignore the flag operand.

  for (unsigned i = InlineAsm::Op_FirstOperand; i != NumOps;) {
    unsigned Flags = cast<ConstantSDNode>(Op.getOperand(i))->getZExtValue();
    unsigned NumVals = InlineAsm::getNumOperandRegisters(Flags);
    ++i;  // Skip the ID value.

    switch (InlineAsm::getKind(Flags)) {
      default:
        llvm_unreachable("Bad flags!");
      case InlineAsm::Kind_RegUse:
      case InlineAsm::Kind_Imm:
      case InlineAsm::Kind_Mem:
        i += NumVals;
        break;
      case InlineAsm::Kind_Clobber:
      case InlineAsm::Kind_RegDef:
      case InlineAsm::Kind_RegDefEarlyClobber: {
        for (; NumVals; --NumVals, ++i) {
          unsigned Reg = cast<RegisterSDNode>(Op.getOperand(i))->getReg();
          if (Reg != LR)
            continue;
          HMFI.setHasClobberLR(true);
          return Op;
        }
        break;
      }
    }
  }

  return Op;
}

// Need to transform ISD::PREFETCH into something that doesn't inherit
// all of the properties of ISD::PREFETCH, specifically SDNPMayLoad and
// SDNPMayStore.
SDValue HexagonTargetLowering::LowerPREFETCH(SDValue Op,
                                             SelectionDAG &DAG) const {
  SDValue Chain = Op.getOperand(0);
  SDValue Addr = Op.getOperand(1);
  // Lower it to DCFETCH($reg, #0).  A "pat" will try to merge the offset in,
  // if the "reg" is fed by an "add".
  SDLoc DL(Op);
  SDValue Zero = DAG.getConstant(0, DL, MVT::i32);
  return DAG.getNode(HexagonISD::DCFETCH, DL, MVT::Other, Chain, Addr, Zero);
}

// Custom-handle ISD::READCYCLECOUNTER because the target-independent SDNode
// is marked as having side-effects, while the register read on Hexagon does
// not have any. TableGen refuses to accept the direct pattern from that node
// to the A4_tfrcpp.
SDValue HexagonTargetLowering::LowerREADCYCLECOUNTER(SDValue Op,
                                                     SelectionDAG &DAG) const {
  SDValue Chain = Op.getOperand(0);
  SDLoc dl(Op);
  SDVTList VTs = DAG.getVTList(MVT::i32, MVT::Other);
  return DAG.getNode(HexagonISD::READCYCLE, dl, VTs, Chain);
}

SDValue HexagonTargetLowering::LowerINTRINSIC_VOID(SDValue Op,
      SelectionDAG &DAG) const {
  SDValue Chain = Op.getOperand(0);
  unsigned IntNo = cast<ConstantSDNode>(Op.getOperand(1))->getZExtValue();
  // Lower the hexagon_prefetch builtin to DCFETCH, as above.
  if (IntNo == Intrinsic::hexagon_prefetch) {
    SDValue Addr = Op.getOperand(2);
    SDLoc DL(Op);
    SDValue Zero = DAG.getConstant(0, DL, MVT::i32);
    return DAG.getNode(HexagonISD::DCFETCH, DL, MVT::Other, Chain, Addr, Zero);
  }
  return SDValue();
}

SDValue
HexagonTargetLowering::LowerDYNAMIC_STACKALLOC(SDValue Op,
                                               SelectionDAG &DAG) const {
  SDValue Chain = Op.getOperand(0);
  SDValue Size = Op.getOperand(1);
  SDValue Align = Op.getOperand(2);
  SDLoc dl(Op);

  ConstantSDNode *AlignConst = dyn_cast<ConstantSDNode>(Align);
  assert(AlignConst && "Non-constant Align in LowerDYNAMIC_STACKALLOC");

  unsigned A = AlignConst->getSExtValue();
  auto &HFI = *Subtarget.getFrameLowering();
  // "Zero" means natural stack alignment.
  if (A == 0)
    A = HFI.getStackAlignment();

  DEBUG({
    dbgs () << __func__ << " Align: " << A << " Size: ";
    Size.getNode()->dump(&DAG);
    dbgs() << "\n";
  });

  SDValue AC = DAG.getConstant(A, dl, MVT::i32);
  SDVTList VTs = DAG.getVTList(MVT::i32, MVT::Other);
  SDValue AA = DAG.getNode(HexagonISD::ALLOCA, dl, VTs, Chain, Size, AC);

  DAG.ReplaceAllUsesOfValueWith(Op, AA);
  return AA;
}

SDValue HexagonTargetLowering::LowerFormalArguments(
    SDValue Chain, CallingConv::ID CallConv, bool isVarArg,
    const SmallVectorImpl<ISD::InputArg> &Ins, const SDLoc &dl,
    SelectionDAG &DAG, SmallVectorImpl<SDValue> &InVals) const {
  MachineFunction &MF = DAG.getMachineFunction();
  MachineFrameInfo &MFI = MF.getFrameInfo();
  MachineRegisterInfo &RegInfo = MF.getRegInfo();
  auto &FuncInfo = *MF.getInfo<HexagonMachineFunctionInfo>();

  // Assign locations to all of the incoming arguments.
  SmallVector<CCValAssign, 16> ArgLocs;
  CCState CCInfo(CallConv, isVarArg, DAG.getMachineFunction(), ArgLocs,
                 *DAG.getContext());

  CCInfo.AnalyzeFormalArguments(Ins, CC_Hexagon);

  // For LLVM, in the case when returning a struct by value (>8byte),
  // the first argument is a pointer that points to the location on caller's
  // stack where the return value will be stored. For Hexagon, the location on
  // caller's stack is passed only when the struct size is smaller than (and
  // equal to) 8 bytes. If not, no address will be passed into callee and
  // callee return the result direclty through R0/R1.

  SmallVector<SDValue, 8> MemOps;
  bool UseHVX = Subtarget.useHVXOps(), UseHVXDbl = Subtarget.useHVXDblOps();

  for (unsigned i = 0, e = ArgLocs.size(); i != e; ++i) {
    CCValAssign &VA = ArgLocs[i];
    ISD::ArgFlagsTy Flags = Ins[i].Flags;
    unsigned ObjSize;
    unsigned StackLocation;
    int FI;

    if (   (VA.isRegLoc() && !Flags.isByVal())
        || (VA.isRegLoc() && Flags.isByVal() && Flags.getByValSize() > 8)) {
      // Arguments passed in registers
      // 1. int, long long, ptr args that get allocated in register.
      // 2. Large struct that gets an register to put its address in.
      EVT RegVT = VA.getLocVT();
      if (RegVT == MVT::i8 || RegVT == MVT::i16 ||
          RegVT == MVT::i32 || RegVT == MVT::f32) {
        unsigned VReg = 
          RegInfo.createVirtualRegister(&Hexagon::IntRegsRegClass);
        RegInfo.addLiveIn(VA.getLocReg(), VReg);
        SDValue Copy = DAG.getCopyFromReg(Chain, dl, VReg, RegVT);
        // Treat values of type MVT::i1 specially: they are passed in
        // registers of type i32, but they need to remain as values of
        // type i1 for consistency of the argument lowering.
        if (VA.getValVT() == MVT::i1) {
          // Generate a copy into a predicate register and use the value
          // of the register as the "InVal".
          unsigned PReg =
            RegInfo.createVirtualRegister(&Hexagon::PredRegsRegClass);
          SDNode *T = DAG.getMachineNode(Hexagon::C2_tfrrp, dl, MVT::i1,
                                         Copy.getValue(0));
          Copy = DAG.getCopyToReg(Copy.getValue(1), dl, PReg, SDValue(T, 0));
          Copy = DAG.getCopyFromReg(Copy, dl, PReg, MVT::i1);
        }
        InVals.push_back(Copy);
        Chain = Copy.getValue(1);
      } else if (RegVT == MVT::i64 || RegVT == MVT::f64) {
        unsigned VReg =
          RegInfo.createVirtualRegister(&Hexagon::DoubleRegsRegClass);
        RegInfo.addLiveIn(VA.getLocReg(), VReg);
        InVals.push_back(DAG.getCopyFromReg(Chain, dl, VReg, RegVT));

      // Single Vector
      } else if ((RegVT == MVT::v8i64 || RegVT == MVT::v16i32 ||
                  RegVT == MVT::v32i16 || RegVT == MVT::v64i8)) {
        unsigned VReg =
          RegInfo.createVirtualRegister(&Hexagon::VectorRegsRegClass);
        RegInfo.addLiveIn(VA.getLocReg(), VReg);
        InVals.push_back(DAG.getCopyFromReg(Chain, dl, VReg, RegVT));
    } else if (UseHVX && UseHVXDbl &&
               ((RegVT == MVT::v16i64 || RegVT == MVT::v32i32 ||
                 RegVT == MVT::v64i16 || RegVT == MVT::v128i8))) {
        unsigned VReg =
          RegInfo.createVirtualRegister(&Hexagon::VectorRegs128BRegClass);
        RegInfo.addLiveIn(VA.getLocReg(), VReg);
        InVals.push_back(DAG.getCopyFromReg(Chain, dl, VReg, RegVT));

      // Double Vector
      } else if ((RegVT == MVT::v16i64 || RegVT == MVT::v32i32 ||
                  RegVT == MVT::v64i16 || RegVT == MVT::v128i8)) {
        unsigned VReg =
          RegInfo.createVirtualRegister(&Hexagon::VecDblRegsRegClass);
        RegInfo.addLiveIn(VA.getLocReg(), VReg);
        InVals.push_back(DAG.getCopyFromReg(Chain, dl, VReg, RegVT));
      } else if (UseHVX && UseHVXDbl &&
                ((RegVT == MVT::v32i64 || RegVT == MVT::v64i32 ||
                  RegVT == MVT::v128i16 || RegVT == MVT::v256i8))) {
        unsigned VReg =
          RegInfo.createVirtualRegister(&Hexagon::VecDblRegs128BRegClass);
        RegInfo.addLiveIn(VA.getLocReg(), VReg);
        InVals.push_back(DAG.getCopyFromReg(Chain, dl, VReg, RegVT));
      } else if (RegVT == MVT::v512i1 || RegVT == MVT::v1024i1) {
        assert(0 && "need to support VecPred regs");
        unsigned VReg =
          RegInfo.createVirtualRegister(&Hexagon::VecPredRegsRegClass);
        RegInfo.addLiveIn(VA.getLocReg(), VReg);
        InVals.push_back(DAG.getCopyFromReg(Chain, dl, VReg, RegVT));
      } else {
        assert (0);
      }
    } else if (VA.isRegLoc() && Flags.isByVal() && Flags.getByValSize() <= 8) {
      assert (0 && "ByValSize must be bigger than 8 bytes");
    } else {
      // Sanity check.
      assert(VA.isMemLoc());

      if (Flags.isByVal()) {
        // If it's a byval parameter, then we need to compute the
        // "real" size, not the size of the pointer.
        ObjSize = Flags.getByValSize();
      } else {
        ObjSize = VA.getLocVT().getStoreSizeInBits() >> 3;
      }

      StackLocation = HEXAGON_LRFP_SIZE + VA.getLocMemOffset();
      // Create the frame index object for this incoming parameter...
      FI = MFI.CreateFixedObject(ObjSize, StackLocation, true);

      // Create the SelectionDAG nodes cordl, responding to a load
      // from this parameter.
      SDValue FIN = DAG.getFrameIndex(FI, MVT::i32);

      if (Flags.isByVal()) {
        // If it's a pass-by-value aggregate, then do not dereference the stack
        // location. Instead, we should generate a reference to the stack
        // location.
        InVals.push_back(FIN);
      } else {
        InVals.push_back(
            DAG.getLoad(VA.getValVT(), dl, Chain, FIN, MachinePointerInfo()));
      }
    }
  }

  if (!MemOps.empty())
    Chain = DAG.getNode(ISD::TokenFactor, dl, MVT::Other, MemOps);

  if (isVarArg) {
    // This will point to the next argument passed via stack.
    int FrameIndex = MFI.CreateFixedObject(Hexagon_PointerSize,
                                           HEXAGON_LRFP_SIZE +
                                           CCInfo.getNextStackOffset(),
                                           true);
    FuncInfo.setVarArgsFrameIndex(FrameIndex);
  }

  return Chain;
}

SDValue
HexagonTargetLowering::LowerVASTART(SDValue Op, SelectionDAG &DAG) const {
  // VASTART stores the address of the VarArgsFrameIndex slot into the
  // memory location argument.
  MachineFunction &MF = DAG.getMachineFunction();
  HexagonMachineFunctionInfo *QFI = MF.getInfo<HexagonMachineFunctionInfo>();
  SDValue Addr = DAG.getFrameIndex(QFI->getVarArgsFrameIndex(), MVT::i32);
  const Value *SV = cast<SrcValueSDNode>(Op.getOperand(2))->getValue();
  return DAG.getStore(Op.getOperand(0), SDLoc(Op), Addr, Op.getOperand(1),
                      MachinePointerInfo(SV));
}

static bool isSExtFree(SDValue N) {
  // A sign-extend of a truncate of a sign-extend is free.
  if (N.getOpcode() == ISD::TRUNCATE &&
      N.getOperand(0).getOpcode() == ISD::AssertSext)
    return true;
  // We have sign-extended loads.
  if (N.getOpcode() == ISD::LOAD)
    return true;
  return false;
}

SDValue HexagonTargetLowering::LowerSETCC(SDValue Op, SelectionDAG &DAG) const {
  SDLoc dl(Op);

  SDValue LHS = Op.getOperand(0);
  SDValue RHS = Op.getOperand(1);
  SDValue Cmp = Op.getOperand(2);
  ISD::CondCode CC = cast<CondCodeSDNode>(Cmp)->get();

  EVT VT = Op.getValueType();
  EVT LHSVT = LHS.getValueType();
  EVT RHSVT = RHS.getValueType();

  if (LHSVT == MVT::v2i16) {
    assert(ISD::isSignedIntSetCC(CC) || ISD::isUnsignedIntSetCC(CC));
    unsigned ExtOpc = ISD::isSignedIntSetCC(CC) ? ISD::SIGN_EXTEND
                                                : ISD::ZERO_EXTEND;
    SDValue LX = DAG.getNode(ExtOpc, dl, MVT::v2i32, LHS);
    SDValue RX = DAG.getNode(ExtOpc, dl, MVT::v2i32, RHS);
    SDValue SC = DAG.getNode(ISD::SETCC, dl, MVT::v2i1, LX, RX, Cmp);
    return SC;
  }

  // Treat all other vector types as legal.
  if (VT.isVector())
    return Op;

  // Equals and not equals should use sign-extend, not zero-extend, since
  // we can represent small negative values in the compare instructions.
  // The LLVM default is to use zero-extend arbitrarily in these cases.
  if ((CC == ISD::SETEQ || CC == ISD::SETNE) &&
      (RHSVT == MVT::i8 || RHSVT == MVT::i16) &&
      (LHSVT == MVT::i8 || LHSVT == MVT::i16)) {
    ConstantSDNode *C = dyn_cast<ConstantSDNode>(RHS);
    if (C && C->getAPIntValue().isNegative()) {
      LHS = DAG.getNode(ISD::SIGN_EXTEND, dl, MVT::i32, LHS);
      RHS = DAG.getNode(ISD::SIGN_EXTEND, dl, MVT::i32, RHS);
      return DAG.getNode(ISD::SETCC, dl, Op.getValueType(),
                         LHS, RHS, Op.getOperand(2));
    }
    if (isSExtFree(LHS) || isSExtFree(RHS)) {
      LHS = DAG.getNode(ISD::SIGN_EXTEND, dl, MVT::i32, LHS);
      RHS = DAG.getNode(ISD::SIGN_EXTEND, dl, MVT::i32, RHS);
      return DAG.getNode(ISD::SETCC, dl, Op.getValueType(),
                         LHS, RHS, Op.getOperand(2));
    }
  }
  return SDValue();
}

SDValue
HexagonTargetLowering::LowerVSELECT(SDValue Op, SelectionDAG &DAG) const {
  SDValue PredOp = Op.getOperand(0);
  SDValue Op1 = Op.getOperand(1), Op2 = Op.getOperand(2);
  EVT OpVT = Op1.getValueType();
  SDLoc DL(Op);

  if (OpVT == MVT::v2i16) {
    SDValue X1 = DAG.getNode(ISD::ZERO_EXTEND, DL, MVT::v2i32, Op1);
    SDValue X2 = DAG.getNode(ISD::ZERO_EXTEND, DL, MVT::v2i32, Op2);
    SDValue SL = DAG.getNode(ISD::VSELECT, DL, MVT::v2i32, PredOp, X1, X2);
    SDValue TR = DAG.getNode(ISD::TRUNCATE, DL, MVT::v2i16, SL);
    return TR;
  }

  return SDValue();
}

SDValue
HexagonTargetLowering::LowerConstantPool(SDValue Op, SelectionDAG &DAG) const {
  EVT ValTy = Op.getValueType();
  ConstantPoolSDNode *CPN = cast<ConstantPoolSDNode>(Op);
  unsigned Align = CPN->getAlignment();
  bool IsPositionIndependent = isPositionIndependent();
  unsigned char TF = IsPositionIndependent ? HexagonII::MO_PCREL : 0;

  unsigned Offset = 0;
  SDValue T;
  if (CPN->isMachineConstantPoolEntry())
    T = DAG.getTargetConstantPool(CPN->getMachineCPVal(), ValTy, Align, Offset,
                                  TF);
  else
    T = DAG.getTargetConstantPool(CPN->getConstVal(), ValTy, Align, Offset,
                                  TF);

  assert(cast<ConstantPoolSDNode>(T)->getTargetFlags() == TF &&
         "Inconsistent target flag encountered");

  if (IsPositionIndependent)
    return DAG.getNode(HexagonISD::AT_PCREL, SDLoc(Op), ValTy, T);
  return DAG.getNode(HexagonISD::CP, SDLoc(Op), ValTy, T);
}

SDValue
HexagonTargetLowering::LowerJumpTable(SDValue Op, SelectionDAG &DAG) const {
  EVT VT = Op.getValueType();
  int Idx = cast<JumpTableSDNode>(Op)->getIndex();
  if (isPositionIndependent()) {
    SDValue T = DAG.getTargetJumpTable(Idx, VT, HexagonII::MO_PCREL);
    return DAG.getNode(HexagonISD::AT_PCREL, SDLoc(Op), VT, T);
  }

  SDValue T = DAG.getTargetJumpTable(Idx, VT);
  return DAG.getNode(HexagonISD::JT, SDLoc(Op), VT, T);
}

SDValue
HexagonTargetLowering::LowerRETURNADDR(SDValue Op, SelectionDAG &DAG) const {
  const HexagonRegisterInfo &HRI = *Subtarget.getRegisterInfo();
  MachineFunction &MF = DAG.getMachineFunction();
  MachineFrameInfo &MFI = MF.getFrameInfo();
  MFI.setReturnAddressIsTaken(true);

  if (verifyReturnAddressArgumentIsConstant(Op, DAG))
    return SDValue();

  EVT VT = Op.getValueType();
  SDLoc dl(Op);
  unsigned Depth = cast<ConstantSDNode>(Op.getOperand(0))->getZExtValue();
  if (Depth) {
    SDValue FrameAddr = LowerFRAMEADDR(Op, DAG);
    SDValue Offset = DAG.getConstant(4, dl, MVT::i32);
    return DAG.getLoad(VT, dl, DAG.getEntryNode(),
                       DAG.getNode(ISD::ADD, dl, VT, FrameAddr, Offset),
                       MachinePointerInfo());
  }

  // Return LR, which contains the return address. Mark it an implicit live-in.
  unsigned Reg = MF.addLiveIn(HRI.getRARegister(), getRegClassFor(MVT::i32));
  return DAG.getCopyFromReg(DAG.getEntryNode(), dl, Reg, VT);
}

SDValue
HexagonTargetLowering::LowerFRAMEADDR(SDValue Op, SelectionDAG &DAG) const {
  const HexagonRegisterInfo &HRI = *Subtarget.getRegisterInfo();
  MachineFrameInfo &MFI = DAG.getMachineFunction().getFrameInfo();
  MFI.setFrameAddressIsTaken(true);

  EVT VT = Op.getValueType();
  SDLoc dl(Op);
  unsigned Depth = cast<ConstantSDNode>(Op.getOperand(0))->getZExtValue();
  SDValue FrameAddr = DAG.getCopyFromReg(DAG.getEntryNode(), dl,
                                         HRI.getFrameRegister(), VT);
  while (Depth--)
    FrameAddr = DAG.getLoad(VT, dl, DAG.getEntryNode(), FrameAddr,
                            MachinePointerInfo());
  return FrameAddr;
}

SDValue
HexagonTargetLowering::LowerATOMIC_FENCE(SDValue Op, SelectionDAG& DAG) const {
  SDLoc dl(Op);
  return DAG.getNode(HexagonISD::BARRIER, dl, MVT::Other, Op.getOperand(0));
}

SDValue
HexagonTargetLowering::LowerGLOBALADDRESS(SDValue Op, SelectionDAG &DAG) const {
  SDLoc dl(Op);
  auto *GAN = cast<GlobalAddressSDNode>(Op);
  auto PtrVT = getPointerTy(DAG.getDataLayout());
  auto *GV = GAN->getGlobal();
  int64_t Offset = GAN->getOffset();

  auto &HLOF = *HTM.getObjFileLowering();
  Reloc::Model RM = HTM.getRelocationModel();

  if (RM == Reloc::Static) {
    SDValue GA = DAG.getTargetGlobalAddress(GV, dl, PtrVT, Offset);
    const GlobalObject *GO = GV->getBaseObject();
    if (GO && HLOF.isGlobalInSmallSection(GO, HTM))
      return DAG.getNode(HexagonISD::CONST32_GP, dl, PtrVT, GA);
    return DAG.getNode(HexagonISD::CONST32, dl, PtrVT, GA);
  }

  bool UsePCRel = getTargetMachine().shouldAssumeDSOLocal(*GV->getParent(), GV);
  if (UsePCRel) {
    SDValue GA = DAG.getTargetGlobalAddress(GV, dl, PtrVT, Offset,
                                            HexagonII::MO_PCREL);
    return DAG.getNode(HexagonISD::AT_PCREL, dl, PtrVT, GA);
  }

  // Use GOT index.
  SDValue GOT = DAG.getGLOBAL_OFFSET_TABLE(PtrVT);
  SDValue GA = DAG.getTargetGlobalAddress(GV, dl, PtrVT, 0, HexagonII::MO_GOT);
  SDValue Off = DAG.getConstant(Offset, dl, MVT::i32);
  return DAG.getNode(HexagonISD::AT_GOT, dl, PtrVT, GOT, GA, Off);
}

// Specifies that for loads and stores VT can be promoted to PromotedLdStVT.
SDValue
HexagonTargetLowering::LowerBlockAddress(SDValue Op, SelectionDAG &DAG) const {
  const BlockAddress *BA = cast<BlockAddressSDNode>(Op)->getBlockAddress();
  SDLoc dl(Op);
  EVT PtrVT = getPointerTy(DAG.getDataLayout());

  Reloc::Model RM = HTM.getRelocationModel();
  if (RM == Reloc::Static) {
    SDValue A = DAG.getTargetBlockAddress(BA, PtrVT);
    return DAG.getNode(HexagonISD::CONST32_GP, dl, PtrVT, A);
  }

  SDValue A = DAG.getTargetBlockAddress(BA, PtrVT, 0, HexagonII::MO_PCREL);
  return DAG.getNode(HexagonISD::AT_PCREL, dl, PtrVT, A);
}

SDValue
HexagonTargetLowering::LowerGLOBAL_OFFSET_TABLE(SDValue Op, SelectionDAG &DAG)
      const {
  EVT PtrVT = getPointerTy(DAG.getDataLayout());
  SDValue GOTSym = DAG.getTargetExternalSymbol(HEXAGON_GOT_SYM_NAME, PtrVT,
                                               HexagonII::MO_PCREL);
  return DAG.getNode(HexagonISD::AT_PCREL, SDLoc(Op), PtrVT, GOTSym);
}

SDValue
HexagonTargetLowering::GetDynamicTLSAddr(SelectionDAG &DAG, SDValue Chain,
      GlobalAddressSDNode *GA, SDValue Glue, EVT PtrVT, unsigned ReturnReg,
      unsigned char OperandFlags) const {
  MachineFunction &MF = DAG.getMachineFunction();
  MachineFrameInfo &MFI = MF.getFrameInfo();
  SDVTList NodeTys = DAG.getVTList(MVT::Other, MVT::Glue);
  SDLoc dl(GA);
  SDValue TGA = DAG.getTargetGlobalAddress(GA->getGlobal(), dl,
                                           GA->getValueType(0),
                                           GA->getOffset(),
                                           OperandFlags);
  // Create Operands for the call.The Operands should have the following:
  // 1. Chain SDValue
  // 2. Callee which in this case is the Global address value.
  // 3. Registers live into the call.In this case its R0, as we
  //    have just one argument to be passed.
  // 4. Glue.
  // Note: The order is important.

  const auto &HRI = *Subtarget.getRegisterInfo();
  const uint32_t *Mask = HRI.getCallPreservedMask(MF, CallingConv::C);
  assert(Mask && "Missing call preserved mask for calling convention");
  SDValue Ops[] = { Chain, TGA, DAG.getRegister(Hexagon::R0, PtrVT),
                    DAG.getRegisterMask(Mask), Glue };
  Chain = DAG.getNode(HexagonISD::CALL, dl, NodeTys, Ops);

  // Inform MFI that function has calls.
  MFI.setAdjustsStack(true);

  Glue = Chain.getValue(1);
  return DAG.getCopyFromReg(Chain, dl, ReturnReg, PtrVT, Glue);
}

//
// Lower using the intial executable model for TLS addresses
//
SDValue
HexagonTargetLowering::LowerToTLSInitialExecModel(GlobalAddressSDNode *GA,
      SelectionDAG &DAG) const {
  SDLoc dl(GA);
  int64_t Offset = GA->getOffset();
  auto PtrVT = getPointerTy(DAG.getDataLayout());

  // Get the thread pointer.
  SDValue TP = DAG.getCopyFromReg(DAG.getEntryNode(), dl, Hexagon::UGP, PtrVT);

  bool IsPositionIndependent = isPositionIndependent();
  unsigned char TF =
      IsPositionIndependent ? HexagonII::MO_IEGOT : HexagonII::MO_IE;

  // First generate the TLS symbol address
  SDValue TGA = DAG.getTargetGlobalAddress(GA->getGlobal(), dl, PtrVT,
                                           Offset, TF);

  SDValue Sym = DAG.getNode(HexagonISD::CONST32, dl, PtrVT, TGA);

  if (IsPositionIndependent) {
    // Generate the GOT pointer in case of position independent code
    SDValue GOT = LowerGLOBAL_OFFSET_TABLE(Sym, DAG);

    // Add the TLS Symbol address to GOT pointer.This gives
    // GOT relative relocation for the symbol.
    Sym = DAG.getNode(ISD::ADD, dl, PtrVT, GOT, Sym);
  }

  // Load the offset value for TLS symbol.This offset is relative to
  // thread pointer.
  SDValue LoadOffset =
      DAG.getLoad(PtrVT, dl, DAG.getEntryNode(), Sym, MachinePointerInfo());

  // Address of the thread local variable is the add of thread
  // pointer and the offset of the variable.
  return DAG.getNode(ISD::ADD, dl, PtrVT, TP, LoadOffset);
}

//
// Lower using the local executable model for TLS addresses
//
SDValue
HexagonTargetLowering::LowerToTLSLocalExecModel(GlobalAddressSDNode *GA,
      SelectionDAG &DAG) const {
  SDLoc dl(GA);
  int64_t Offset = GA->getOffset();
  auto PtrVT = getPointerTy(DAG.getDataLayout());

  // Get the thread pointer.
  SDValue TP = DAG.getCopyFromReg(DAG.getEntryNode(), dl, Hexagon::UGP, PtrVT);
  // Generate the TLS symbol address
  SDValue TGA = DAG.getTargetGlobalAddress(GA->getGlobal(), dl, PtrVT, Offset,
                                           HexagonII::MO_TPREL);
  SDValue Sym = DAG.getNode(HexagonISD::CONST32, dl, PtrVT, TGA);

  // Address of the thread local variable is the add of thread
  // pointer and the offset of the variable.
  return DAG.getNode(ISD::ADD, dl, PtrVT, TP, Sym);
}

//
// Lower using the general dynamic model for TLS addresses
//
SDValue
HexagonTargetLowering::LowerToTLSGeneralDynamicModel(GlobalAddressSDNode *GA,
      SelectionDAG &DAG) const {
  SDLoc dl(GA);
  int64_t Offset = GA->getOffset();
  auto PtrVT = getPointerTy(DAG.getDataLayout());

  // First generate the TLS symbol address
  SDValue TGA = DAG.getTargetGlobalAddress(GA->getGlobal(), dl, PtrVT, Offset,
                                           HexagonII::MO_GDGOT);

  // Then, generate the GOT pointer
  SDValue GOT = LowerGLOBAL_OFFSET_TABLE(TGA, DAG);

  // Add the TLS symbol and the GOT pointer
  SDValue Sym = DAG.getNode(HexagonISD::CONST32, dl, PtrVT, TGA);
  SDValue Chain = DAG.getNode(ISD::ADD, dl, PtrVT, GOT, Sym);

  // Copy over the argument to R0
  SDValue InFlag;
  Chain = DAG.getCopyToReg(DAG.getEntryNode(), dl, Hexagon::R0, Chain, InFlag);
  InFlag = Chain.getValue(1);

  unsigned Flags =
      static_cast<const HexagonSubtarget &>(DAG.getSubtarget()).useLongCalls()
          ? HexagonII::MO_GDPLT | HexagonII::HMOTF_ConstExtended
          : HexagonII::MO_GDPLT;

  return GetDynamicTLSAddr(DAG, Chain, GA, InFlag, PtrVT,
                           Hexagon::R0, Flags);
}

//
// Lower TLS addresses.
//
// For now for dynamic models, we only support the general dynamic model.
//
SDValue
HexagonTargetLowering::LowerGlobalTLSAddress(SDValue Op,
      SelectionDAG &DAG) const {
  GlobalAddressSDNode *GA = cast<GlobalAddressSDNode>(Op);

  switch (HTM.getTLSModel(GA->getGlobal())) {
    case TLSModel::GeneralDynamic:
    case TLSModel::LocalDynamic:
      return LowerToTLSGeneralDynamicModel(GA, DAG);
    case TLSModel::InitialExec:
      return LowerToTLSInitialExecModel(GA, DAG);
    case TLSModel::LocalExec:
      return LowerToTLSLocalExecModel(GA, DAG);
  }
  llvm_unreachable("Bogus TLS model");
}

//===----------------------------------------------------------------------===//
// TargetLowering Implementation
//===----------------------------------------------------------------------===//

HexagonTargetLowering::HexagonTargetLowering(const TargetMachine &TM,
                                             const HexagonSubtarget &ST)
    : TargetLowering(TM), HTM(static_cast<const HexagonTargetMachine&>(TM)),
      Subtarget(ST) {
  bool IsV4 = !Subtarget.hasV5TOps();
  auto &HRI = *Subtarget.getRegisterInfo();
  bool UseHVX = Subtarget.useHVXOps();
  bool UseHVXSgl = Subtarget.useHVXSglOps();
  bool UseHVXDbl = Subtarget.useHVXDblOps();

  setPrefLoopAlignment(4);
  setPrefFunctionAlignment(4);
  setMinFunctionAlignment(2);
  setStackPointerRegisterToSaveRestore(HRI.getStackRegister());

  setMaxAtomicSizeInBitsSupported(64);
  setMinCmpXchgSizeInBits(32);

  if (EnableHexSDNodeSched)
    setSchedulingPreference(Sched::VLIW);
  else
    setSchedulingPreference(Sched::Source);

  // Limits for inline expansion of memcpy/memmove
  MaxStoresPerMemcpy = MaxStoresPerMemcpyCL;
  MaxStoresPerMemcpyOptSize = MaxStoresPerMemcpyOptSizeCL;
  MaxStoresPerMemmove = MaxStoresPerMemmoveCL;
  MaxStoresPerMemmoveOptSize = MaxStoresPerMemmoveOptSizeCL;
  MaxStoresPerMemset = MaxStoresPerMemsetCL;
  MaxStoresPerMemsetOptSize = MaxStoresPerMemsetOptSizeCL;

  //
  // Set up register classes.
  //

  addRegisterClass(MVT::i1,    &Hexagon::PredRegsRegClass);
  addRegisterClass(MVT::v2i1,  &Hexagon::PredRegsRegClass);  // bbbbaaaa
  addRegisterClass(MVT::v4i1,  &Hexagon::PredRegsRegClass);  // ddccbbaa
  addRegisterClass(MVT::v8i1,  &Hexagon::PredRegsRegClass);  // hgfedcba
  addRegisterClass(MVT::i32,   &Hexagon::IntRegsRegClass);
  addRegisterClass(MVT::v4i8,  &Hexagon::IntRegsRegClass);
  addRegisterClass(MVT::v2i16, &Hexagon::IntRegsRegClass);
  addRegisterClass(MVT::i64,   &Hexagon::DoubleRegsRegClass);
  addRegisterClass(MVT::v8i8,  &Hexagon::DoubleRegsRegClass);
  addRegisterClass(MVT::v4i16, &Hexagon::DoubleRegsRegClass);
  addRegisterClass(MVT::v2i32, &Hexagon::DoubleRegsRegClass);

  if (Subtarget.hasV5TOps()) {
    addRegisterClass(MVT::f32, &Hexagon::IntRegsRegClass);
    addRegisterClass(MVT::f64, &Hexagon::DoubleRegsRegClass);
  }

  if (Subtarget.hasV60TOps()) {
    if (Subtarget.useHVXSglOps()) {
      addRegisterClass(MVT::v64i8,  &Hexagon::VectorRegsRegClass);
      addRegisterClass(MVT::v32i16, &Hexagon::VectorRegsRegClass);
      addRegisterClass(MVT::v16i32, &Hexagon::VectorRegsRegClass);
      addRegisterClass(MVT::v8i64,  &Hexagon::VectorRegsRegClass);
      addRegisterClass(MVT::v128i8, &Hexagon::VecDblRegsRegClass);
      addRegisterClass(MVT::v64i16, &Hexagon::VecDblRegsRegClass);
      addRegisterClass(MVT::v32i32, &Hexagon::VecDblRegsRegClass);
      addRegisterClass(MVT::v16i64, &Hexagon::VecDblRegsRegClass);
      addRegisterClass(MVT::v512i1, &Hexagon::VecPredRegsRegClass);
    } else if (Subtarget.useHVXDblOps()) {
      addRegisterClass(MVT::v128i8,  &Hexagon::VectorRegs128BRegClass);
      addRegisterClass(MVT::v64i16,  &Hexagon::VectorRegs128BRegClass);
      addRegisterClass(MVT::v32i32,  &Hexagon::VectorRegs128BRegClass);
      addRegisterClass(MVT::v16i64,  &Hexagon::VectorRegs128BRegClass);
      addRegisterClass(MVT::v256i8,  &Hexagon::VecDblRegs128BRegClass);
      addRegisterClass(MVT::v128i16, &Hexagon::VecDblRegs128BRegClass);
      addRegisterClass(MVT::v64i32,  &Hexagon::VecDblRegs128BRegClass);
      addRegisterClass(MVT::v32i64,  &Hexagon::VecDblRegs128BRegClass);
      addRegisterClass(MVT::v1024i1, &Hexagon::VecPredRegs128BRegClass);
    }
  }

  //
  // Handling of scalar operations.
  //
  // All operations default to "legal", except:
  // - indexed loads and stores (pre-/post-incremented),
  // - ANY_EXTEND_VECTOR_INREG, ATOMIC_CMP_SWAP_WITH_SUCCESS, CONCAT_VECTORS,
  //   ConstantFP, DEBUGTRAP, FCEIL, FCOPYSIGN, FEXP, FEXP2, FFLOOR, FGETSIGN,
  //   FLOG, FLOG2, FLOG10, FMAXNUM, FMINNUM, FNEARBYINT, FRINT, FROUND, TRAP,
  //   FTRUNC, PREFETCH, SIGN_EXTEND_VECTOR_INREG, ZERO_EXTEND_VECTOR_INREG,
  // which default to "expand" for at least one type.

  // Misc operations.
  setOperationAction(ISD::ConstantFP, MVT::f32, Legal); // Default: expand
  setOperationAction(ISD::ConstantFP, MVT::f64, Legal); // Default: expand

  setOperationAction(ISD::ConstantPool, MVT::i32, Custom);
  setOperationAction(ISD::JumpTable, MVT::i32, Custom);
  setOperationAction(ISD::BUILD_PAIR, MVT::i64, Expand);
  setOperationAction(ISD::SIGN_EXTEND_INREG, MVT::i1, Expand);
  setOperationAction(ISD::INLINEASM, MVT::Other, Custom);
  setOperationAction(ISD::PREFETCH, MVT::Other, Custom);
  setOperationAction(ISD::READCYCLECOUNTER, MVT::i64, Custom);
  setOperationAction(ISD::INTRINSIC_VOID, MVT::Other, Custom);
  setOperationAction(ISD::EH_RETURN, MVT::Other, Custom);
  setOperationAction(ISD::GLOBAL_OFFSET_TABLE, MVT::i32, Custom);
  setOperationAction(ISD::GlobalTLSAddress, MVT::i32, Custom);
  setOperationAction(ISD::ATOMIC_FENCE, MVT::Other, Custom);

  // Custom legalize GlobalAddress nodes into CONST32.
  setOperationAction(ISD::GlobalAddress, MVT::i32, Custom);
  setOperationAction(ISD::GlobalAddress, MVT::i8,  Custom);
  setOperationAction(ISD::BlockAddress,  MVT::i32, Custom);

  // Hexagon needs to optimize cases with negative constants.
  setOperationAction(ISD::SETCC, MVT::i8,  Custom);
  setOperationAction(ISD::SETCC, MVT::i16, Custom);

  // VASTART needs to be custom lowered to use the VarArgsFrameIndex.
  setOperationAction(ISD::VASTART, MVT::Other, Custom);
  setOperationAction(ISD::VAEND,   MVT::Other, Expand);
  setOperationAction(ISD::VAARG,   MVT::Other, Expand);

  setOperationAction(ISD::STACKSAVE, MVT::Other, Expand);
  setOperationAction(ISD::STACKRESTORE, MVT::Other, Expand);
  setOperationAction(ISD::DYNAMIC_STACKALLOC, MVT::i32, Custom);

  if (EmitJumpTables)
    setMinimumJumpTableEntries(MinimumJumpTables);
  else
    setMinimumJumpTableEntries(std::numeric_limits<int>::max());
  setOperationAction(ISD::BR_JT, MVT::Other, Expand);

  // Hexagon has instructions for add/sub with carry. The problem with
  // modeling these instructions is that they produce 2 results: Rdd and Px.
  // To model the update of Px, we will have to use Defs[p0..p3] which will
  // cause any predicate live range to spill. So, we pretend we dont't have
  // these instructions.
  setOperationAction(ISD::ADDE, MVT::i8,  Expand);
  setOperationAction(ISD::ADDE, MVT::i16, Expand);
  setOperationAction(ISD::ADDE, MVT::i32, Expand);
  setOperationAction(ISD::ADDE, MVT::i64, Expand);
  setOperationAction(ISD::SUBE, MVT::i8,  Expand);
  setOperationAction(ISD::SUBE, MVT::i16, Expand);
  setOperationAction(ISD::SUBE, MVT::i32, Expand);
  setOperationAction(ISD::SUBE, MVT::i64, Expand);
  setOperationAction(ISD::ADDC, MVT::i8,  Expand);
  setOperationAction(ISD::ADDC, MVT::i16, Expand);
  setOperationAction(ISD::ADDC, MVT::i32, Expand);
  setOperationAction(ISD::ADDC, MVT::i64, Expand);
  setOperationAction(ISD::SUBC, MVT::i8,  Expand);
  setOperationAction(ISD::SUBC, MVT::i16, Expand);
  setOperationAction(ISD::SUBC, MVT::i32, Expand);
  setOperationAction(ISD::SUBC, MVT::i64, Expand);

  // Only add and sub that detect overflow are the saturating ones.
  for (MVT VT : MVT::integer_valuetypes()) {
    setOperationAction(ISD::UADDO, VT, Expand);
    setOperationAction(ISD::SADDO, VT, Expand);
    setOperationAction(ISD::USUBO, VT, Expand);
    setOperationAction(ISD::SSUBO, VT, Expand);
  }

  setOperationAction(ISD::CTLZ, MVT::i8,  Promote);
  setOperationAction(ISD::CTLZ, MVT::i16, Promote);
  setOperationAction(ISD::CTTZ, MVT::i8,  Promote);
  setOperationAction(ISD::CTTZ, MVT::i16, Promote);

  // In V5, popcount can count # of 1s in i64 but returns i32.
  // On V4 it will be expanded (set later).
  setOperationAction(ISD::CTPOP, MVT::i8,  Promote);
  setOperationAction(ISD::CTPOP, MVT::i16, Promote);
  setOperationAction(ISD::CTPOP, MVT::i32, Promote);
  setOperationAction(ISD::CTPOP, MVT::i64, Legal);

  setOperationAction(ISD::BITREVERSE, MVT::i32, Legal);
  setOperationAction(ISD::BITREVERSE, MVT::i64, Legal);
  setOperationAction(ISD::BSWAP, MVT::i32, Legal);
  setOperationAction(ISD::BSWAP, MVT::i64, Legal);
  setOperationAction(ISD::MUL,   MVT::i64, Legal);

  for (unsigned IntExpOp :
       { ISD::SDIV,      ISD::UDIV,      ISD::SREM,      ISD::UREM,
         ISD::SDIVREM,   ISD::UDIVREM,   ISD::ROTL,      ISD::ROTR,
         ISD::SHL_PARTS, ISD::SRA_PARTS, ISD::SRL_PARTS,
         ISD::SMUL_LOHI, ISD::UMUL_LOHI }) {
    setOperationAction(IntExpOp, MVT::i32, Expand);
    setOperationAction(IntExpOp, MVT::i64, Expand);
  }

  for (unsigned FPExpOp :
       {ISD::FDIV, ISD::FREM, ISD::FSQRT, ISD::FSIN, ISD::FCOS, ISD::FSINCOS,
        ISD::FPOW, ISD::FCOPYSIGN}) {
    setOperationAction(FPExpOp, MVT::f32, Expand);
    setOperationAction(FPExpOp, MVT::f64, Expand);
  }

  // No extending loads from i32.
  for (MVT VT : MVT::integer_valuetypes()) {
    setLoadExtAction(ISD::ZEXTLOAD, VT, MVT::i32, Expand);
    setLoadExtAction(ISD::SEXTLOAD, VT, MVT::i32, Expand);
    setLoadExtAction(ISD::EXTLOAD,  VT, MVT::i32, Expand);
  }
  // Turn FP truncstore into trunc + store.
  setTruncStoreAction(MVT::f64, MVT::f32, Expand);
  // Turn FP extload into load/fpextend.
  for (MVT VT : MVT::fp_valuetypes())
    setLoadExtAction(ISD::EXTLOAD, VT, MVT::f32, Expand);

  // Expand BR_CC and SELECT_CC for all integer and fp types.
  for (MVT VT : MVT::integer_valuetypes()) {
    setOperationAction(ISD::BR_CC,     VT, Expand);
    setOperationAction(ISD::SELECT_CC, VT, Expand);
  }
  for (MVT VT : MVT::fp_valuetypes()) {
    setOperationAction(ISD::BR_CC,     VT, Expand);
    setOperationAction(ISD::SELECT_CC, VT, Expand);
  }
  setOperationAction(ISD::BR_CC, MVT::Other, Expand);

  //
  // Handling of vector operations.
  //

  promoteLdStType(MVT::v4i8,  MVT::i32);
  promoteLdStType(MVT::v2i16, MVT::i32);
  promoteLdStType(MVT::v8i8,  MVT::i64);
  promoteLdStType(MVT::v4i16, MVT::i64);
  promoteLdStType(MVT::v2i32, MVT::i64);

  // Set the action for vector operations to "expand", then override it with
  // either "custom" or "legal" for specific cases.
  static const unsigned VectExpOps[] = {
    // Integer arithmetic:
    ISD::ADD,     ISD::SUB,     ISD::MUL,     ISD::SDIV,    ISD::UDIV,
    ISD::SREM,    ISD::UREM,    ISD::SDIVREM, ISD::UDIVREM, ISD::ADDC,
    ISD::SUBC,    ISD::SADDO,   ISD::UADDO,   ISD::SSUBO,   ISD::USUBO,
    ISD::SMUL_LOHI,             ISD::UMUL_LOHI,
    // Logical/bit:
    ISD::AND,     ISD::OR,      ISD::XOR,     ISD::ROTL,    ISD::ROTR,
    ISD::CTPOP,   ISD::CTLZ,    ISD::CTTZ,
    // Floating point arithmetic/math functions:
    ISD::FADD,    ISD::FSUB,    ISD::FMUL,    ISD::FMA,     ISD::FDIV,
    ISD::FREM,    ISD::FNEG,    ISD::FABS,    ISD::FSQRT,   ISD::FSIN,
    ISD::FCOS,    ISD::FPOW,    ISD::FLOG,    ISD::FLOG2,
    ISD::FLOG10,  ISD::FEXP,    ISD::FEXP2,   ISD::FCEIL,   ISD::FTRUNC,
    ISD::FRINT,   ISD::FNEARBYINT,            ISD::FROUND,  ISD::FFLOOR,
    ISD::FMINNUM, ISD::FMAXNUM, ISD::FSINCOS,
    // Misc:
    ISD::BR_CC,   ISD::SELECT_CC,             ISD::ConstantPool,
    // Vector:
    ISD::BUILD_VECTOR,          ISD::SCALAR_TO_VECTOR,
    ISD::EXTRACT_VECTOR_ELT,    ISD::INSERT_VECTOR_ELT,
    ISD::EXTRACT_SUBVECTOR,     ISD::INSERT_SUBVECTOR,
    ISD::CONCAT_VECTORS,        ISD::VECTOR_SHUFFLE
  };

  for (MVT VT : MVT::vector_valuetypes()) {
    for (unsigned VectExpOp : VectExpOps)
      setOperationAction(VectExpOp, VT, Expand);

    // Expand all extending loads and truncating stores:
    for (MVT TargetVT : MVT::vector_valuetypes()) {
      if (TargetVT == VT)
        continue;
      setLoadExtAction(ISD::EXTLOAD, TargetVT, VT, Expand);
      setLoadExtAction(ISD::ZEXTLOAD, TargetVT, VT, Expand);
      setLoadExtAction(ISD::SEXTLOAD, TargetVT, VT, Expand);
      setTruncStoreAction(VT, TargetVT, Expand);
    }

    // Normalize all inputs to SELECT to be vectors of i32.
    if (VT.getVectorElementType() != MVT::i32) {
      MVT VT32 = MVT::getVectorVT(MVT::i32, VT.getSizeInBits()/32);
      setOperationAction(ISD::SELECT, VT, Promote);
      AddPromotedToType(ISD::SELECT, VT, VT32);
    }
    setOperationAction(ISD::SRA, VT, Custom);
    setOperationAction(ISD::SHL, VT, Custom);
    setOperationAction(ISD::SRL, VT, Custom);
  }

  // Types natively supported:
  for (MVT NativeVT : {MVT::v2i1, MVT::v4i1, MVT::v8i1, MVT::v32i1, MVT::v64i1,
                       MVT::v4i8, MVT::v8i8, MVT::v2i16, MVT::v4i16, MVT::v1i32,
                       MVT::v2i32, MVT::v1i64}) {
    setOperationAction(ISD::BUILD_VECTOR,       NativeVT, Custom);
    setOperationAction(ISD::EXTRACT_VECTOR_ELT, NativeVT, Custom);
    setOperationAction(ISD::INSERT_VECTOR_ELT,  NativeVT, Custom);
    setOperationAction(ISD::EXTRACT_SUBVECTOR,  NativeVT, Custom);
    setOperationAction(ISD::INSERT_SUBVECTOR,   NativeVT, Custom);
    setOperationAction(ISD::CONCAT_VECTORS,     NativeVT, Custom);

    setOperationAction(ISD::ADD, NativeVT, Legal);
    setOperationAction(ISD::SUB, NativeVT, Legal);
    setOperationAction(ISD::MUL, NativeVT, Legal);
    setOperationAction(ISD::AND, NativeVT, Legal);
    setOperationAction(ISD::OR,  NativeVT, Legal);
    setOperationAction(ISD::XOR, NativeVT, Legal);
  }

  setOperationAction(ISD::SETCC,          MVT::v2i16, Custom);
  setOperationAction(ISD::VSELECT,        MVT::v2i16, Custom);
  setOperationAction(ISD::VECTOR_SHUFFLE, MVT::v4i16, Custom);
  setOperationAction(ISD::VECTOR_SHUFFLE, MVT::v8i8,  Custom);

  if (UseHVX) {
    if (UseHVXSgl) {
      setOperationAction(ISD::CONCAT_VECTORS, MVT::v128i8,  Custom);
      setOperationAction(ISD::CONCAT_VECTORS, MVT::v64i16,  Custom);
      setOperationAction(ISD::CONCAT_VECTORS, MVT::v32i32,  Custom);
      setOperationAction(ISD::CONCAT_VECTORS, MVT::v16i64,  Custom);
      // We try to generate the vpack{e/o} instructions. If we fail
      // we fall back upon ExpandOp.
      setOperationAction(ISD::VECTOR_SHUFFLE, MVT::v64i8,  Custom);
      setOperationAction(ISD::VECTOR_SHUFFLE, MVT::v32i16, Custom);
      setOperationAction(ISD::EXTRACT_SUBVECTOR, MVT::v64i8, Custom);
      setOperationAction(ISD::EXTRACT_SUBVECTOR, MVT::v32i16, Custom);
      setOperationAction(ISD::EXTRACT_SUBVECTOR, MVT::v16i32, Custom);
    } else if (UseHVXDbl) {
      setOperationAction(ISD::CONCAT_VECTORS, MVT::v256i8,  Custom);
      setOperationAction(ISD::CONCAT_VECTORS, MVT::v128i16, Custom);
      setOperationAction(ISD::CONCAT_VECTORS, MVT::v64i32,  Custom);
      setOperationAction(ISD::CONCAT_VECTORS, MVT::v32i64,  Custom);
      // We try to generate the vpack{e/o} instructions. If we fail
      // we fall back upon ExpandOp.
      setOperationAction(ISD::VECTOR_SHUFFLE, MVT::v128i8,  Custom);
      setOperationAction(ISD::VECTOR_SHUFFLE, MVT::v64i16,  Custom);
      setOperationAction(ISD::EXTRACT_SUBVECTOR, MVT::v4i32, Custom);
      setOperationAction(ISD::EXTRACT_SUBVECTOR, MVT::v128i8, Custom);
      setOperationAction(ISD::EXTRACT_SUBVECTOR, MVT::v64i16, Custom);
      setOperationAction(ISD::EXTRACT_SUBVECTOR, MVT::v32i32, Custom);
    } else {
      llvm_unreachable("Unrecognized HVX mode");
    }
  }
  // Subtarget-specific operation actions.
  //
  if (Subtarget.hasV5TOps()) {
    setOperationAction(ISD::FMA,  MVT::f64, Expand);
    setOperationAction(ISD::FADD, MVT::f64, Expand);
    setOperationAction(ISD::FSUB, MVT::f64, Expand);
    setOperationAction(ISD::FMUL, MVT::f64, Expand);

    setOperationAction(ISD::FMINNUM, MVT::f32, Legal);
    setOperationAction(ISD::FMAXNUM, MVT::f32, Legal);

    setOperationAction(ISD::FP_TO_UINT, MVT::i1,  Promote);
    setOperationAction(ISD::FP_TO_UINT, MVT::i8,  Promote);
    setOperationAction(ISD::FP_TO_UINT, MVT::i16, Promote);
    setOperationAction(ISD::FP_TO_SINT, MVT::i1,  Promote);
    setOperationAction(ISD::FP_TO_SINT, MVT::i8,  Promote);
    setOperationAction(ISD::FP_TO_SINT, MVT::i16, Promote);
    setOperationAction(ISD::UINT_TO_FP, MVT::i1,  Promote);
    setOperationAction(ISD::UINT_TO_FP, MVT::i8,  Promote);
    setOperationAction(ISD::UINT_TO_FP, MVT::i16, Promote);
    setOperationAction(ISD::SINT_TO_FP, MVT::i1,  Promote);
    setOperationAction(ISD::SINT_TO_FP, MVT::i8,  Promote);
    setOperationAction(ISD::SINT_TO_FP, MVT::i16, Promote);
  } else { // V4
    setOperationAction(ISD::SINT_TO_FP, MVT::i32, Expand);
    setOperationAction(ISD::SINT_TO_FP, MVT::i64, Expand);
    setOperationAction(ISD::UINT_TO_FP, MVT::i32, Expand);
    setOperationAction(ISD::UINT_TO_FP, MVT::i64, Expand);
    setOperationAction(ISD::FP_TO_SINT, MVT::f64, Expand);
    setOperationAction(ISD::FP_TO_SINT, MVT::f32, Expand);
    setOperationAction(ISD::FP_EXTEND,  MVT::f32, Expand);
    setOperationAction(ISD::FP_ROUND,   MVT::f64, Expand);
    setCondCodeAction(ISD::SETUNE, MVT::f64, Expand);

    setOperationAction(ISD::CTPOP, MVT::i8,  Expand);
    setOperationAction(ISD::CTPOP, MVT::i16, Expand);
    setOperationAction(ISD::CTPOP, MVT::i32, Expand);
    setOperationAction(ISD::CTPOP, MVT::i64, Expand);

    // Expand these operations for both f32 and f64:
    for (unsigned FPExpOpV4 :
         {ISD::FADD, ISD::FSUB, ISD::FMUL, ISD::FABS, ISD::FNEG, ISD::FMA}) {
      setOperationAction(FPExpOpV4, MVT::f32, Expand);
      setOperationAction(FPExpOpV4, MVT::f64, Expand);
    }

    for (ISD::CondCode FPExpCCV4 :
         {ISD::SETOEQ, ISD::SETOGT, ISD::SETOLT, ISD::SETOGE, ISD::SETOLE,
          ISD::SETUO,  ISD::SETO}) {
      setCondCodeAction(FPExpCCV4, MVT::f32, Expand);
      setCondCodeAction(FPExpCCV4, MVT::f64, Expand);
    }
  }

  // Handling of indexed loads/stores: default is "expand".
  //
  for (MVT VT : {MVT::i8, MVT::i16, MVT::i32, MVT::i64}) {
    setIndexedLoadAction(ISD::POST_INC, VT, Legal);
    setIndexedStoreAction(ISD::POST_INC, VT, Legal);
  }

  if (UseHVXSgl) {
    for (MVT VT : {MVT::v64i8,  MVT::v32i16, MVT::v16i32, MVT::v8i64,
                   MVT::v128i8, MVT::v64i16, MVT::v32i32, MVT::v16i64}) {
      setIndexedLoadAction(ISD::POST_INC, VT, Legal);
      setIndexedStoreAction(ISD::POST_INC, VT, Legal);
    }
  } else if (UseHVXDbl) {
    for (MVT VT : {MVT::v128i8, MVT::v64i16,  MVT::v32i32, MVT::v16i64,
                   MVT::v256i8, MVT::v128i16, MVT::v64i32, MVT::v32i64}) {
      setIndexedLoadAction(ISD::POST_INC, VT, Legal);
      setIndexedStoreAction(ISD::POST_INC, VT, Legal);
    }
  }

  computeRegisterProperties(&HRI);

  //
  // Library calls for unsupported operations
  //
  bool FastMath  = EnableFastMath;

  setLibcallName(RTLIB::SDIV_I32, "__hexagon_divsi3");
  setLibcallName(RTLIB::SDIV_I64, "__hexagon_divdi3");
  setLibcallName(RTLIB::UDIV_I32, "__hexagon_udivsi3");
  setLibcallName(RTLIB::UDIV_I64, "__hexagon_udivdi3");
  setLibcallName(RTLIB::SREM_I32, "__hexagon_modsi3");
  setLibcallName(RTLIB::SREM_I64, "__hexagon_moddi3");
  setLibcallName(RTLIB::UREM_I32, "__hexagon_umodsi3");
  setLibcallName(RTLIB::UREM_I64, "__hexagon_umoddi3");

  setLibcallName(RTLIB::SINTTOFP_I128_F64, "__hexagon_floattidf");
  setLibcallName(RTLIB::SINTTOFP_I128_F32, "__hexagon_floattisf");
  setLibcallName(RTLIB::FPTOUINT_F32_I128, "__hexagon_fixunssfti");
  setLibcallName(RTLIB::FPTOUINT_F64_I128, "__hexagon_fixunsdfti");
  setLibcallName(RTLIB::FPTOSINT_F32_I128, "__hexagon_fixsfti");
  setLibcallName(RTLIB::FPTOSINT_F64_I128, "__hexagon_fixdfti");

  if (IsV4) {
    // Handle single-precision floating point operations on V4.
    if (FastMath) {
      setLibcallName(RTLIB::ADD_F32, "__hexagon_fast_addsf3");
      setLibcallName(RTLIB::SUB_F32, "__hexagon_fast_subsf3");
      setLibcallName(RTLIB::MUL_F32, "__hexagon_fast_mulsf3");
      setLibcallName(RTLIB::OGT_F32, "__hexagon_fast_gtsf2");
      setLibcallName(RTLIB::OLT_F32, "__hexagon_fast_ltsf2");
      // Double-precision compares.
      setLibcallName(RTLIB::OGT_F64, "__hexagon_fast_gtdf2");
      setLibcallName(RTLIB::OLT_F64, "__hexagon_fast_ltdf2");
    } else {
      setLibcallName(RTLIB::ADD_F32, "__hexagon_addsf3");
      setLibcallName(RTLIB::SUB_F32, "__hexagon_subsf3");
      setLibcallName(RTLIB::MUL_F32, "__hexagon_mulsf3");
      setLibcallName(RTLIB::OGT_F32, "__hexagon_gtsf2");
      setLibcallName(RTLIB::OLT_F32, "__hexagon_ltsf2");
      // Double-precision compares.
      setLibcallName(RTLIB::OGT_F64, "__hexagon_gtdf2");
      setLibcallName(RTLIB::OLT_F64, "__hexagon_ltdf2");
    }
  }

  // This is the only fast library function for sqrtd.
  if (FastMath)
    setLibcallName(RTLIB::SQRT_F64, "__hexagon_fast2_sqrtdf2");

  // Prefix is: nothing  for "slow-math",
  //            "fast2_" for V4 fast-math and V5+ fast-math double-precision
  // (actually, keep fast-math and fast-math2 separate for now)
  if (FastMath) {
    setLibcallName(RTLIB::ADD_F64, "__hexagon_fast_adddf3");
    setLibcallName(RTLIB::SUB_F64, "__hexagon_fast_subdf3");
    setLibcallName(RTLIB::MUL_F64, "__hexagon_fast_muldf3");
    setLibcallName(RTLIB::DIV_F64, "__hexagon_fast_divdf3");
    // Calling __hexagon_fast2_divsf3 with fast-math on V5 (ok).
    setLibcallName(RTLIB::DIV_F32, "__hexagon_fast_divsf3");
  } else {
    setLibcallName(RTLIB::ADD_F64, "__hexagon_adddf3");
    setLibcallName(RTLIB::SUB_F64, "__hexagon_subdf3");
    setLibcallName(RTLIB::MUL_F64, "__hexagon_muldf3");
    setLibcallName(RTLIB::DIV_F64, "__hexagon_divdf3");
    setLibcallName(RTLIB::DIV_F32, "__hexagon_divsf3");
  }

  if (Subtarget.hasV5TOps()) {
    if (FastMath)
      setLibcallName(RTLIB::SQRT_F32, "__hexagon_fast2_sqrtf");
    else
      setLibcallName(RTLIB::SQRT_F32, "__hexagon_sqrtf");
  } else {
    // V4
    setLibcallName(RTLIB::SINTTOFP_I32_F32, "__hexagon_floatsisf");
    setLibcallName(RTLIB::SINTTOFP_I32_F64, "__hexagon_floatsidf");
    setLibcallName(RTLIB::SINTTOFP_I64_F32, "__hexagon_floatdisf");
    setLibcallName(RTLIB::SINTTOFP_I64_F64, "__hexagon_floatdidf");
    setLibcallName(RTLIB::UINTTOFP_I32_F32, "__hexagon_floatunsisf");
    setLibcallName(RTLIB::UINTTOFP_I32_F64, "__hexagon_floatunsidf");
    setLibcallName(RTLIB::UINTTOFP_I64_F32, "__hexagon_floatundisf");
    setLibcallName(RTLIB::UINTTOFP_I64_F64, "__hexagon_floatundidf");
    setLibcallName(RTLIB::FPTOUINT_F32_I32, "__hexagon_fixunssfsi");
    setLibcallName(RTLIB::FPTOUINT_F32_I64, "__hexagon_fixunssfdi");
    setLibcallName(RTLIB::FPTOUINT_F64_I32, "__hexagon_fixunsdfsi");
    setLibcallName(RTLIB::FPTOUINT_F64_I64, "__hexagon_fixunsdfdi");
    setLibcallName(RTLIB::FPTOSINT_F32_I32, "__hexagon_fixsfsi");
    setLibcallName(RTLIB::FPTOSINT_F32_I64, "__hexagon_fixsfdi");
    setLibcallName(RTLIB::FPTOSINT_F64_I32, "__hexagon_fixdfsi");
    setLibcallName(RTLIB::FPTOSINT_F64_I64, "__hexagon_fixdfdi");
    setLibcallName(RTLIB::FPEXT_F32_F64,    "__hexagon_extendsfdf2");
    setLibcallName(RTLIB::FPROUND_F64_F32,  "__hexagon_truncdfsf2");
    setLibcallName(RTLIB::OEQ_F32, "__hexagon_eqsf2");
    setLibcallName(RTLIB::OEQ_F64, "__hexagon_eqdf2");
    setLibcallName(RTLIB::OGE_F32, "__hexagon_gesf2");
    setLibcallName(RTLIB::OGE_F64, "__hexagon_gedf2");
    setLibcallName(RTLIB::OLE_F32, "__hexagon_lesf2");
    setLibcallName(RTLIB::OLE_F64, "__hexagon_ledf2");
    setLibcallName(RTLIB::UNE_F32, "__hexagon_nesf2");
    setLibcallName(RTLIB::UNE_F64, "__hexagon_nedf2");
    setLibcallName(RTLIB::UO_F32,  "__hexagon_unordsf2");
    setLibcallName(RTLIB::UO_F64,  "__hexagon_unorddf2");
    setLibcallName(RTLIB::O_F32,   "__hexagon_unordsf2");
    setLibcallName(RTLIB::O_F64,   "__hexagon_unorddf2");
  }

  // These cause problems when the shift amount is non-constant.
  setLibcallName(RTLIB::SHL_I128, nullptr);
  setLibcallName(RTLIB::SRL_I128, nullptr);
  setLibcallName(RTLIB::SRA_I128, nullptr);
}

const char* HexagonTargetLowering::getTargetNodeName(unsigned Opcode) const {
  switch ((HexagonISD::NodeType)Opcode) {
  case HexagonISD::ALLOCA:        return "HexagonISD::ALLOCA";
  case HexagonISD::AT_GOT:        return "HexagonISD::AT_GOT";
  case HexagonISD::AT_PCREL:      return "HexagonISD::AT_PCREL";
  case HexagonISD::BARRIER:       return "HexagonISD::BARRIER";
  case HexagonISD::CALL:          return "HexagonISD::CALL";
  case HexagonISD::CALLnr:        return "HexagonISD::CALLnr";
  case HexagonISD::CALLR:         return "HexagonISD::CALLR";
  case HexagonISD::COMBINE:       return "HexagonISD::COMBINE";
  case HexagonISD::CONST32_GP:    return "HexagonISD::CONST32_GP";
  case HexagonISD::CONST32:       return "HexagonISD::CONST32";
  case HexagonISD::CP:            return "HexagonISD::CP";
  case HexagonISD::DCFETCH:       return "HexagonISD::DCFETCH";
  case HexagonISD::EH_RETURN:     return "HexagonISD::EH_RETURN";
  case HexagonISD::EXTRACTU:      return "HexagonISD::EXTRACTU";
  case HexagonISD::EXTRACTURP:    return "HexagonISD::EXTRACTURP";
  case HexagonISD::INSERT:        return "HexagonISD::INSERT";
  case HexagonISD::INSERTRP:      return "HexagonISD::INSERTRP";
  case HexagonISD::JT:            return "HexagonISD::JT";
  case HexagonISD::PACKHL:        return "HexagonISD::PACKHL";
  case HexagonISD::RET_FLAG:      return "HexagonISD::RET_FLAG";
  case HexagonISD::TC_RETURN:     return "HexagonISD::TC_RETURN";
  case HexagonISD::VCOMBINE:      return "HexagonISD::VCOMBINE";
  case HexagonISD::VPACKE:        return "HexagonISD::VPACKE";
  case HexagonISD::VPACKO:        return "HexagonISD::VPACKO";
  case HexagonISD::VASL:          return "HexagonISD::VASL";
  case HexagonISD::VASR:          return "HexagonISD::VASR";
  case HexagonISD::VLSR:          return "HexagonISD::VLSR";
  case HexagonISD::VSPLAT:        return "HexagonISD::VSPLAT";
  case HexagonISD::READCYCLE:     return "HexagonISD::READCYCLE";
  case HexagonISD::OP_END:        break;
  }
  return nullptr;
}

bool HexagonTargetLowering::isTruncateFree(Type *Ty1, Type *Ty2) const {
  EVT MTy1 = EVT::getEVT(Ty1);
  EVT MTy2 = EVT::getEVT(Ty2);
  if (!MTy1.isSimple() || !MTy2.isSimple())
    return false;
  return (MTy1.getSimpleVT() == MVT::i64) && (MTy2.getSimpleVT() == MVT::i32);
}

bool HexagonTargetLowering::isTruncateFree(EVT VT1, EVT VT2) const {
  if (!VT1.isSimple() || !VT2.isSimple())
    return false;
  return (VT1.getSimpleVT() == MVT::i64) && (VT2.getSimpleVT() == MVT::i32);
}

bool HexagonTargetLowering::isFMAFasterThanFMulAndFAdd(EVT VT) const {
  return isOperationLegalOrCustom(ISD::FMA, VT);
}

// Should we expand the build vector with shuffles?
bool HexagonTargetLowering::shouldExpandBuildVectorWithShuffles(EVT VT,
      unsigned DefinedValues) const {
  // Hexagon vector shuffle operates on element sizes of bytes or halfwords
  EVT EltVT = VT.getVectorElementType();
  int EltBits = EltVT.getSizeInBits();
  if ((EltBits != 8) && (EltBits != 16))
    return false;

  return TargetLowering::shouldExpandBuildVectorWithShuffles(VT, DefinedValues);
}

static StridedLoadKind isStridedLoad(const ArrayRef<int> &Mask) {
  int even_start = -2;
  int odd_start = -1;
  size_t mask_len = Mask.size();
  for (auto idx : Mask) {
    if ((idx - even_start) == 2)
      even_start = idx;
    else
      break;
  }
  if (even_start == (int)(mask_len * 2) - 2)
    return StridedLoadKind::Even;
  for (auto idx : Mask) {
    if ((idx - odd_start) == 2)
      odd_start = idx;
    else
      break;
  }
  if (odd_start == (int)(mask_len * 2) - 1)
    return StridedLoadKind::Odd;

  return StridedLoadKind::NoPattern;
}

bool HexagonTargetLowering::isShuffleMaskLegal(const SmallVectorImpl<int> &Mask,
      EVT VT) const {
  if (Subtarget.useHVXOps())
    return isStridedLoad(Mask) != StridedLoadKind::NoPattern;
  return true;
}

// Lower a vector shuffle (V1, V2, V3).  V1 and V2 are the two vectors
// to select data from, V3 is the permutation.
SDValue
HexagonTargetLowering::LowerVECTOR_SHUFFLE(SDValue Op, SelectionDAG &DAG)
      const {
  const ShuffleVectorSDNode *SVN = cast<ShuffleVectorSDNode>(Op);
  SDValue V1 = Op.getOperand(0);
  SDValue V2 = Op.getOperand(1);
  SDLoc dl(Op);
  EVT VT = Op.getValueType();
  bool UseHVX = Subtarget.useHVXOps();

  if (V2.isUndef())
    V2 = V1;

  if (SVN->isSplat()) {
    int Lane = SVN->getSplatIndex();
    if (Lane == -1) Lane = 0;

    // Test if V1 is a SCALAR_TO_VECTOR.
    if (Lane == 0 && V1.getOpcode() == ISD::SCALAR_TO_VECTOR)
      return DAG.getNode(HexagonISD::VSPLAT, dl, VT, V1.getOperand(0));

    // Test if V1 is a BUILD_VECTOR which is equivalent to a SCALAR_TO_VECTOR
    // (and probably will turn into a SCALAR_TO_VECTOR once legalization
    // reaches it).
    if (Lane == 0 && V1.getOpcode() == ISD::BUILD_VECTOR &&
        !isa<ConstantSDNode>(V1.getOperand(0))) {
      bool IsScalarToVector = true;
      for (unsigned i = 1, e = V1.getNumOperands(); i != e; ++i) {
        if (!V1.getOperand(i).isUndef()) {
          IsScalarToVector = false;
          break;
        }
      }
      if (IsScalarToVector)
        return DAG.getNode(HexagonISD::VSPLAT, dl, VT, V1.getOperand(0));
    }
    return DAG.getNode(HexagonISD::VSPLAT, dl, VT,
                       DAG.getConstant(Lane, dl, MVT::i32));
  }

  if (UseHVX) {
    ArrayRef<int> Mask = SVN->getMask();
    size_t MaskLen = Mask.size();
    unsigned SizeInBits = VT.getScalarSizeInBits() * MaskLen;

    if ((Subtarget.useHVXSglOps() && SizeInBits == 64 * 8) ||
        (Subtarget.useHVXDblOps() && SizeInBits == 128 * 8)) {
      StridedLoadKind Pattern = isStridedLoad(Mask);
      if (Pattern == StridedLoadKind::NoPattern)
        return SDValue();

      unsigned Opc = Pattern == StridedLoadKind::Even ? HexagonISD::VPACKE
                                                      : HexagonISD::VPACKO;
      return DAG.getNode(Opc, dl, VT, {Op.getOperand(1), Op.getOperand(0)});
    }
    // We used to assert in the "else" part here, but that is bad for Halide
    // Halide creates intermediate double registers by interleaving two
    // concatenated vector registers. The interleaving requires vector_shuffle
    // nodes and we shouldn't barf on a double register result of a
    // vector_shuffle because it is most likely an intermediate result.
  }
  // FIXME: We need to support more general vector shuffles.  See
  // below the comment from the ARM backend that deals in the general
  // case with the vector shuffles.  For now, let expand handle these.
  return SDValue();

  // If the shuffle is not directly supported and it has 4 elements, use
  // the PerfectShuffle-generated table to synthesize it from other shuffles.
}

// If BUILD_VECTOR has same base element repeated several times,
// report true.
static bool isCommonSplatElement(BuildVectorSDNode *BVN) {
  unsigned NElts = BVN->getNumOperands();
  SDValue V0 = BVN->getOperand(0);

  for (unsigned i = 1, e = NElts; i != e; ++i) {
    if (BVN->getOperand(i) != V0)
      return false;
  }
  return true;
}

// Lower a vector shift. Try to convert
// <VT> = SHL/SRA/SRL <VT> by <VT> to Hexagon specific
// <VT> = SHL/SRA/SRL <VT> by <IT/i32>.
SDValue
HexagonTargetLowering::LowerVECTOR_SHIFT(SDValue Op, SelectionDAG &DAG) const {
  BuildVectorSDNode *BVN = nullptr;
  SDValue V1 = Op.getOperand(0);
  SDValue V2 = Op.getOperand(1);
  SDValue V3;
  SDLoc dl(Op);
  EVT VT = Op.getValueType();

  if ((BVN = dyn_cast<BuildVectorSDNode>(V1.getNode())) &&
      isCommonSplatElement(BVN))
    V3 = V2;
  else if ((BVN = dyn_cast<BuildVectorSDNode>(V2.getNode())) &&
           isCommonSplatElement(BVN))
    V3 = V1;
  else
    return SDValue();

  SDValue CommonSplat = BVN->getOperand(0);
  SDValue Result;

  if (VT.getSimpleVT() == MVT::v4i16) {
    switch (Op.getOpcode()) {
    case ISD::SRA:
      Result = DAG.getNode(HexagonISD::VASR, dl, VT, V3, CommonSplat);
      break;
    case ISD::SHL:
      Result = DAG.getNode(HexagonISD::VASL, dl, VT, V3, CommonSplat);
      break;
    case ISD::SRL:
      Result = DAG.getNode(HexagonISD::VLSR, dl, VT, V3, CommonSplat);
      break;
    default:
      return SDValue();
    }
  } else if (VT.getSimpleVT() == MVT::v2i32) {
    switch (Op.getOpcode()) {
    case ISD::SRA:
      Result = DAG.getNode(HexagonISD::VASR, dl, VT, V3, CommonSplat);
      break;
    case ISD::SHL:
      Result = DAG.getNode(HexagonISD::VASL, dl, VT, V3, CommonSplat);
      break;
    case ISD::SRL:
      Result = DAG.getNode(HexagonISD::VLSR, dl, VT, V3, CommonSplat);
      break;
    default:
      return SDValue();
    }
  } else {
    return SDValue();
  }

  return DAG.getNode(ISD::BITCAST, dl, VT, Result);
}

SDValue
HexagonTargetLowering::LowerBUILD_VECTOR(SDValue Op, SelectionDAG &DAG) const {
  BuildVectorSDNode *BVN = cast<BuildVectorSDNode>(Op.getNode());
  SDLoc dl(Op);
  EVT VT = Op.getValueType();

  unsigned Size = VT.getSizeInBits();

  // Only handle vectors of 64 bits or shorter.
  if (Size > 64)
    return SDValue();

  unsigned NElts = BVN->getNumOperands();

  // Try to generate a SPLAT instruction.
  if (VT == MVT::v4i8 || VT == MVT::v4i16 || VT == MVT::v2i32) {
    APInt APSplatBits, APSplatUndef;
    unsigned SplatBitSize;
    bool HasAnyUndefs;
    if (BVN->isConstantSplat(APSplatBits, APSplatUndef, SplatBitSize,
                             HasAnyUndefs, 0, false)) {
      if (SplatBitSize == VT.getVectorElementType().getSizeInBits()) {
        unsigned ZV = APSplatBits.getZExtValue();
        assert(SplatBitSize <= 32 && "Can only handle up to i32");
        // Sign-extend the splat value from SplatBitSize to 32.
        int32_t SV = SplatBitSize < 32
                        ? int32_t(ZV << (32-SplatBitSize)) >> (32-SplatBitSize)
                        : int32_t(ZV);
        return DAG.getNode(HexagonISD::VSPLAT, dl, VT,
                           DAG.getConstant(SV, dl, MVT::i32));
      }
    }
  }

  // Try to generate COMBINE to build v2i32 vectors.
  if (VT.getSimpleVT() == MVT::v2i32) {
    SDValue V0 = BVN->getOperand(0);
    SDValue V1 = BVN->getOperand(1);

    if (V0.isUndef())
      V0 = DAG.getConstant(0, dl, MVT::i32);
    if (V1.isUndef())
      V1 = DAG.getConstant(0, dl, MVT::i32);

    ConstantSDNode *C0 = dyn_cast<ConstantSDNode>(V0);
    ConstantSDNode *C1 = dyn_cast<ConstantSDNode>(V1);
    // If the element isn't a constant, it is in a register:
    // generate a COMBINE Register Register instruction.
    if (!C0 || !C1)
      return DAG.getNode(HexagonISD::COMBINE, dl, VT, V1, V0);

    // If one of the operands is an 8 bit integer constant, generate
    // a COMBINE Immediate Immediate instruction.
    if (isInt<8>(C0->getSExtValue()) ||
        isInt<8>(C1->getSExtValue()))
      return DAG.getNode(HexagonISD::COMBINE, dl, VT, V1, V0);
  }

  // Try to generate a S2_packhl to build v2i16 vectors.
  if (VT.getSimpleVT() == MVT::v2i16) {
    for (unsigned i = 0, e = NElts; i != e; ++i) {
      if (BVN->getOperand(i).isUndef())
        continue;
      ConstantSDNode *Cst = dyn_cast<ConstantSDNode>(BVN->getOperand(i));
      // If the element isn't a constant, it is in a register:
      // generate a S2_packhl instruction.
      if (!Cst) {
        SDValue pack = DAG.getNode(HexagonISD::PACKHL, dl, MVT::v4i16,
                                   BVN->getOperand(1), BVN->getOperand(0));

        return DAG.getTargetExtractSubreg(Hexagon::isub_lo, dl, MVT::v2i16,
                                          pack);
      }
    }
  }

  // In the general case, generate a CONST32 or a CONST64 for constant vectors,
  // and insert_vector_elt for all the other cases.
  uint64_t Res = 0;
  unsigned EltSize = Size / NElts;
  SDValue ConstVal;
  uint64_t Mask = ~uint64_t(0ULL) >> (64 - EltSize);
  bool HasNonConstantElements = false;

  for (unsigned i = 0, e = NElts; i != e; ++i) {
    // LLVM's BUILD_VECTOR operands are in Little Endian mode, whereas Hexagon's
    // combine, const64, etc. are Big Endian.
    unsigned OpIdx = NElts - i - 1;
    SDValue Operand = BVN->getOperand(OpIdx);
    if (Operand.isUndef())
      continue;

    int64_t Val = 0;
    if (ConstantSDNode *Cst = dyn_cast<ConstantSDNode>(Operand))
      Val = Cst->getSExtValue();
    else
      HasNonConstantElements = true;

    Val &= Mask;
    Res = (Res << EltSize) | Val;
  }

  if (Size > 64)
    return SDValue();

  if (Size == 64)
    ConstVal = DAG.getConstant(Res, dl, MVT::i64);
  else
    ConstVal = DAG.getConstant(Res, dl, MVT::i32);

  // When there are non constant operands, add them with INSERT_VECTOR_ELT to
  // ConstVal, the constant part of the vector.
  if (HasNonConstantElements) {
    EVT EltVT = VT.getVectorElementType();
    SDValue Width = DAG.getConstant(EltVT.getSizeInBits(), dl, MVT::i64);
    SDValue Shifted = DAG.getNode(ISD::SHL, dl, MVT::i64, Width,
                                  DAG.getConstant(32, dl, MVT::i64));

    for (unsigned i = 0, e = NElts; i != e; ++i) {
      // LLVM's BUILD_VECTOR operands are in Little Endian mode, whereas Hexagon
      // is Big Endian.
      unsigned OpIdx = NElts - i - 1;
      SDValue Operand = BVN->getOperand(OpIdx);
      if (isa<ConstantSDNode>(Operand))
        // This operand is already in ConstVal.
        continue;

      if (VT.getSizeInBits() == 64 &&
          Operand.getValueSizeInBits() == 32) {
        SDValue C = DAG.getConstant(0, dl, MVT::i32);
        Operand = DAG.getNode(HexagonISD::COMBINE, dl, VT, C, Operand);
      }

      SDValue Idx = DAG.getConstant(OpIdx, dl, MVT::i64);
      SDValue Offset = DAG.getNode(ISD::MUL, dl, MVT::i64, Idx, Width);
      SDValue Combined = DAG.getNode(ISD::OR, dl, MVT::i64, Shifted, Offset);
      const SDValue Ops[] = {ConstVal, Operand, Combined};

      if (VT.getSizeInBits() == 32)
        ConstVal = DAG.getNode(HexagonISD::INSERTRP, dl, MVT::i32, Ops);
      else
        ConstVal = DAG.getNode(HexagonISD::INSERTRP, dl, MVT::i64, Ops);
    }
  }

  return DAG.getNode(ISD::BITCAST, dl, VT, ConstVal);
}

SDValue
HexagonTargetLowering::LowerCONCAT_VECTORS(SDValue Op,
                                           SelectionDAG &DAG) const {
  SDLoc dl(Op);
  bool UseHVX = Subtarget.useHVXOps();
  EVT VT = Op.getValueType();
  unsigned NElts = Op.getNumOperands();
  SDValue Vec0 = Op.getOperand(0);
  EVT VecVT = Vec0.getValueType();
  unsigned Width = VecVT.getSizeInBits();

  if (NElts == 2) {
    MVT ST = VecVT.getSimpleVT();
    // We are trying to concat two v2i16 to a single v4i16, or two v4i8
    // into a single v8i8.
    if (ST == MVT::v2i16 || ST == MVT::v4i8)
      return DAG.getNode(HexagonISD::COMBINE, dl, VT, Op.getOperand(1), Vec0);

    if (UseHVX) {
      assert((Width ==  64*8 && Subtarget.useHVXSglOps()) ||
             (Width == 128*8 && Subtarget.useHVXDblOps()));
      SDValue Vec1 = Op.getOperand(1);
      MVT OpTy = Subtarget.useHVXSglOps() ? MVT::v16i32 : MVT::v32i32;
      MVT ReTy = Subtarget.useHVXSglOps() ? MVT::v32i32 : MVT::v64i32;
      SDValue B0 = DAG.getNode(ISD::BITCAST, dl, OpTy, Vec0);
      SDValue B1 = DAG.getNode(ISD::BITCAST, dl, OpTy, Vec1);
      SDValue VC = DAG.getNode(HexagonISD::VCOMBINE, dl, ReTy, B1, B0);
      return DAG.getNode(ISD::BITCAST, dl, VT, VC);
    }
  }

  if (VT.getSizeInBits() != 32 && VT.getSizeInBits() != 64)
    return SDValue();

  SDValue C0 = DAG.getConstant(0, dl, MVT::i64);
  SDValue C32 = DAG.getConstant(32, dl, MVT::i64);
  SDValue W = DAG.getConstant(Width, dl, MVT::i64);
  // Create the "width" part of the argument to insert_rp/insertp_rp.
  SDValue S = DAG.getNode(ISD::SHL, dl, MVT::i64, W, C32);
  SDValue V = C0;

  for (unsigned i = 0, e = NElts; i != e; ++i) {
    unsigned N = NElts-i-1;
    SDValue OpN = Op.getOperand(N);

    if (VT.getSizeInBits() == 64 && OpN.getValueSizeInBits() == 32) {
      SDValue C = DAG.getConstant(0, dl, MVT::i32);
      OpN = DAG.getNode(HexagonISD::COMBINE, dl, VT, C, OpN);
    }
    SDValue Idx = DAG.getConstant(N, dl, MVT::i64);
    SDValue Offset = DAG.getNode(ISD::MUL, dl, MVT::i64, Idx, W);
    SDValue Or = DAG.getNode(ISD::OR, dl, MVT::i64, S, Offset);
    if (VT.getSizeInBits() == 32)
      V = DAG.getNode(HexagonISD::INSERTRP, dl, MVT::i32, {V, OpN, Or});
    else if (VT.getSizeInBits() == 64)
      V = DAG.getNode(HexagonISD::INSERTRP, dl, MVT::i64, {V, OpN, Or});
    else
      return SDValue();
  }

  return DAG.getNode(ISD::BITCAST, dl, VT, V);
}

SDValue
HexagonTargetLowering::LowerEXTRACT_SUBVECTOR_HVX(SDValue Op,
                                                  SelectionDAG &DAG) const {
  EVT VT = Op.getOperand(0).getValueType();
  SDLoc dl(Op);
  bool UseHVX = Subtarget.useHVXOps();
  bool UseHVXSgl = Subtarget.useHVXSglOps();
  // Just in case...

  if (!VT.isVector() || !UseHVX)
    return SDValue();

  EVT ResVT = Op.getValueType();
  unsigned ResSize = ResVT.getSizeInBits();
  unsigned VectorSizeInBits = UseHVXSgl ? (64 * 8) : (128 * 8);
  unsigned OpSize = VT.getSizeInBits();

  // We deal only with cases where the result is the vector size
  // and the vector operand is a double register.
  if (!(ResVT.isByteSized() && ResSize == VectorSizeInBits) ||
      !(VT.isByteSized() && OpSize == 2 * VectorSizeInBits))
    return SDValue();

  ConstantSDNode *Cst = dyn_cast<ConstantSDNode>(Op.getOperand(1));
  if (!Cst)
    return SDValue();
  unsigned Val = Cst->getZExtValue();

  // These two will get lowered to an appropriate EXTRACT_SUBREG in ISel.
  if (Val == 0) {
    SDValue Vec = Op.getOperand(0);
    return DAG.getTargetExtractSubreg(Hexagon::vsub_lo, dl, ResVT, Vec);
  }

  if (ResVT.getVectorNumElements() == Val) {
    SDValue Vec = Op.getOperand(0);
    return DAG.getTargetExtractSubreg(Hexagon::vsub_hi, dl, ResVT, Vec);
  }

  return SDValue();
}

SDValue
HexagonTargetLowering::LowerEXTRACT_VECTOR(SDValue Op,
                                           SelectionDAG &DAG) const {
  // If we are dealing with EXTRACT_SUBVECTOR on a HVX type, we may
  // be able to simplify it to an EXTRACT_SUBREG.
  if (Op.getOpcode() == ISD::EXTRACT_SUBVECTOR && Subtarget.useHVXOps() &&
      isHvxVectorType(Op.getValueType().getSimpleVT()))
    return LowerEXTRACT_SUBVECTOR_HVX(Op, DAG);

  EVT VT = Op.getValueType();
  int VTN = VT.isVector() ? VT.getVectorNumElements() : 1;
  SDLoc dl(Op);
  SDValue Idx = Op.getOperand(1);
  SDValue Vec = Op.getOperand(0);
  EVT VecVT = Vec.getValueType();
  EVT EltVT = VecVT.getVectorElementType();
  int EltSize = EltVT.getSizeInBits();
  SDValue Width = DAG.getConstant(Op.getOpcode() == ISD::EXTRACT_VECTOR_ELT ?
                                  EltSize : VTN * EltSize, dl, MVT::i64);

  // Constant element number.
  if (ConstantSDNode *CI = dyn_cast<ConstantSDNode>(Idx)) {
    uint64_t X = CI->getZExtValue();
    SDValue Offset = DAG.getConstant(X * EltSize, dl, MVT::i32);
    const SDValue Ops[] = {Vec, Width, Offset};

    ConstantSDNode *CW = dyn_cast<ConstantSDNode>(Width);
    assert(CW && "Non constant width in LowerEXTRACT_VECTOR");

    SDValue N;
    MVT SVT = VecVT.getSimpleVT();
    uint64_t W = CW->getZExtValue();

    if (W == 32) {
      // Translate this node into EXTRACT_SUBREG.
      unsigned Subreg = (X == 0) ? Hexagon::isub_lo : 0;

      if (X == 0)
        Subreg = Hexagon::isub_lo;
      else if (SVT == MVT::v2i32 && X == 1)
        Subreg = Hexagon::isub_hi;
      else if (SVT == MVT::v4i16 && X == 2)
        Subreg = Hexagon::isub_hi;
      else if (SVT == MVT::v8i8 && X == 4)
        Subreg = Hexagon::isub_hi;
      else
        llvm_unreachable("Bad offset");
      N = DAG.getTargetExtractSubreg(Subreg, dl, MVT::i32, Vec);

    } else if (SVT.getSizeInBits() == 32) {
      N = DAG.getNode(HexagonISD::EXTRACTU, dl, MVT::i32, Ops);
    } else if (SVT.getSizeInBits() == 64) {
      N = DAG.getNode(HexagonISD::EXTRACTU, dl, MVT::i64, Ops);
      if (VT.getSizeInBits() == 32)
        N = DAG.getTargetExtractSubreg(Hexagon::isub_lo, dl, MVT::i32, N);
    } else
      return SDValue();

    return DAG.getNode(ISD::BITCAST, dl, VT, N);
  }

  // Variable element number.
  SDValue Offset = DAG.getNode(ISD::MUL, dl, MVT::i32, Idx,
                               DAG.getConstant(EltSize, dl, MVT::i32));
  SDValue Shifted = DAG.getNode(ISD::SHL, dl, MVT::i64, Width,
                                DAG.getConstant(32, dl, MVT::i64));
  SDValue Combined = DAG.getNode(ISD::OR, dl, MVT::i64, Shifted, Offset);

  const SDValue Ops[] = {Vec, Combined};

  SDValue N;
  if (VecVT.getSizeInBits() == 32) {
    N = DAG.getNode(HexagonISD::EXTRACTURP, dl, MVT::i32, Ops);
  } else {
    N = DAG.getNode(HexagonISD::EXTRACTURP, dl, MVT::i64, Ops);
    if (VT.getSizeInBits() == 32)
      N = DAG.getTargetExtractSubreg(Hexagon::isub_lo, dl, MVT::i32, N);
  }
  return DAG.getNode(ISD::BITCAST, dl, VT, N);
}

SDValue
HexagonTargetLowering::LowerINSERT_VECTOR(SDValue Op,
                                          SelectionDAG &DAG) const {
  EVT VT = Op.getValueType();
  int VTN = VT.isVector() ? VT.getVectorNumElements() : 1;
  SDLoc dl(Op);
  SDValue Vec = Op.getOperand(0);
  SDValue Val = Op.getOperand(1);
  SDValue Idx = Op.getOperand(2);
  EVT VecVT = Vec.getValueType();
  EVT EltVT = VecVT.getVectorElementType();
  int EltSize = EltVT.getSizeInBits();
  SDValue Width = DAG.getConstant(Op.getOpcode() == ISD::INSERT_VECTOR_ELT ?
                                  EltSize : VTN * EltSize, dl, MVT::i64);

  if (ConstantSDNode *C = cast<ConstantSDNode>(Idx)) {
    SDValue Offset = DAG.getConstant(C->getSExtValue() * EltSize, dl, MVT::i32);
    const SDValue Ops[] = {Vec, Val, Width, Offset};

    SDValue N;
    if (VT.getSizeInBits() == 32)
      N = DAG.getNode(HexagonISD::INSERT, dl, MVT::i32, Ops);
    else if (VT.getSizeInBits() == 64)
      N = DAG.getNode(HexagonISD::INSERT, dl, MVT::i64, Ops);
    else
      return SDValue();

    return DAG.getNode(ISD::BITCAST, dl, VT, N);
  }

  // Variable element number.
  SDValue Offset = DAG.getNode(ISD::MUL, dl, MVT::i32, Idx,
                               DAG.getConstant(EltSize, dl, MVT::i32));
  SDValue Shifted = DAG.getNode(ISD::SHL, dl, MVT::i64, Width,
                                DAG.getConstant(32, dl, MVT::i64));
  SDValue Combined = DAG.getNode(ISD::OR, dl, MVT::i64, Shifted, Offset);

  if (VT.getSizeInBits() == 64 && Val.getValueSizeInBits() == 32) {
    SDValue C = DAG.getConstant(0, dl, MVT::i32);
    Val = DAG.getNode(HexagonISD::COMBINE, dl, VT, C, Val);
  }

  const SDValue Ops[] = {Vec, Val, Combined};

  SDValue N;
  if (VT.getSizeInBits() == 32)
    N = DAG.getNode(HexagonISD::INSERTRP, dl, MVT::i32, Ops);
  else if (VT.getSizeInBits() == 64)
    N = DAG.getNode(HexagonISD::INSERTRP, dl, MVT::i64, Ops);
  else
    return SDValue();

  return DAG.getNode(ISD::BITCAST, dl, VT, N);
}

bool
HexagonTargetLowering::allowTruncateForTailCall(Type *Ty1, Type *Ty2) const {
  // Assuming the caller does not have either a signext or zeroext modifier, and
  // only one value is accepted, any reasonable truncation is allowed.
  if (!Ty1->isIntegerTy() || !Ty2->isIntegerTy())
    return false;

  // FIXME: in principle up to 64-bit could be made safe, but it would be very
  // fragile at the moment: any support for multiple value returns would be
  // liable to disallow tail calls involving i64 -> iN truncation in many cases.
  return Ty1->getPrimitiveSizeInBits() <= 32;
}

SDValue
HexagonTargetLowering::LowerEH_RETURN(SDValue Op, SelectionDAG &DAG) const {
  SDValue Chain     = Op.getOperand(0);
  SDValue Offset    = Op.getOperand(1);
  SDValue Handler   = Op.getOperand(2);
  SDLoc dl(Op);
  auto PtrVT = getPointerTy(DAG.getDataLayout());

  // Mark function as containing a call to EH_RETURN.
  HexagonMachineFunctionInfo *FuncInfo =
    DAG.getMachineFunction().getInfo<HexagonMachineFunctionInfo>();
  FuncInfo->setHasEHReturn();

  unsigned OffsetReg = Hexagon::R28;

  SDValue StoreAddr =
      DAG.getNode(ISD::ADD, dl, PtrVT, DAG.getRegister(Hexagon::R30, PtrVT),
                  DAG.getIntPtrConstant(4, dl));
  Chain = DAG.getStore(Chain, dl, Handler, StoreAddr, MachinePointerInfo());
  Chain = DAG.getCopyToReg(Chain, dl, OffsetReg, Offset);

  // Not needed we already use it as explict input to EH_RETURN.
  // MF.getRegInfo().addLiveOut(OffsetReg);

  return DAG.getNode(HexagonISD::EH_RETURN, dl, MVT::Other, Chain);
}

SDValue
HexagonTargetLowering::LowerOperation(SDValue Op, SelectionDAG &DAG) const {
  unsigned Opc = Op.getOpcode();
  switch (Opc) {
    default:
#ifndef NDEBUG
      Op.getNode()->dumpr(&DAG);
      if (Opc > HexagonISD::OP_BEGIN && Opc < HexagonISD::OP_END)
        errs() << "Check for a non-legal type in this operation\n";
#endif
      llvm_unreachable("Should not custom lower this!");
    case ISD::CONCAT_VECTORS:       return LowerCONCAT_VECTORS(Op, DAG);
    case ISD::INSERT_SUBVECTOR:     return LowerINSERT_VECTOR(Op, DAG);
    case ISD::INSERT_VECTOR_ELT:    return LowerINSERT_VECTOR(Op, DAG);
    case ISD::EXTRACT_SUBVECTOR:    return LowerEXTRACT_VECTOR(Op, DAG);
    case ISD::EXTRACT_VECTOR_ELT:   return LowerEXTRACT_VECTOR(Op, DAG);
    case ISD::BUILD_VECTOR:         return LowerBUILD_VECTOR(Op, DAG);
    case ISD::VECTOR_SHUFFLE:       return LowerVECTOR_SHUFFLE(Op, DAG);
    case ISD::SRA:
    case ISD::SHL:
    case ISD::SRL:                  return LowerVECTOR_SHIFT(Op, DAG);
    case ISD::ConstantPool:         return LowerConstantPool(Op, DAG);
    case ISD::JumpTable:            return LowerJumpTable(Op, DAG);
    case ISD::EH_RETURN:            return LowerEH_RETURN(Op, DAG);
      // Frame & Return address. Currently unimplemented.
    case ISD::RETURNADDR:           return LowerRETURNADDR(Op, DAG);
    case ISD::FRAMEADDR:            return LowerFRAMEADDR(Op, DAG);
    case ISD::GlobalTLSAddress:     return LowerGlobalTLSAddress(Op, DAG);
    case ISD::ATOMIC_FENCE:         return LowerATOMIC_FENCE(Op, DAG);
    case ISD::GlobalAddress:        return LowerGLOBALADDRESS(Op, DAG);
    case ISD::BlockAddress:         return LowerBlockAddress(Op, DAG);
    case ISD::GLOBAL_OFFSET_TABLE:  return LowerGLOBAL_OFFSET_TABLE(Op, DAG);
    case ISD::VASTART:              return LowerVASTART(Op, DAG);
    case ISD::DYNAMIC_STACKALLOC:   return LowerDYNAMIC_STACKALLOC(Op, DAG);
    case ISD::SETCC:                return LowerSETCC(Op, DAG);
    case ISD::VSELECT:              return LowerVSELECT(Op, DAG);
    case ISD::INTRINSIC_WO_CHAIN:   return LowerINTRINSIC_WO_CHAIN(Op, DAG);
    case ISD::INTRINSIC_VOID:       return LowerINTRINSIC_VOID(Op, DAG);
    case ISD::INLINEASM:            return LowerINLINEASM(Op, DAG);
    case ISD::PREFETCH:             return LowerPREFETCH(Op, DAG);
    case ISD::READCYCLECOUNTER:     return LowerREADCYCLECOUNTER(Op, DAG);
  }
}

/// Returns relocation base for the given PIC jumptable.
SDValue
HexagonTargetLowering::getPICJumpTableRelocBase(SDValue Table,
                                                SelectionDAG &DAG) const {
  int Idx = cast<JumpTableSDNode>(Table)->getIndex();
  EVT VT = Table.getValueType();
  SDValue T = DAG.getTargetJumpTable(Idx, VT, HexagonII::MO_PCREL);
  return DAG.getNode(HexagonISD::AT_PCREL, SDLoc(Table), VT, T);
}

//===----------------------------------------------------------------------===//
// Inline Assembly Support
//===----------------------------------------------------------------------===//

TargetLowering::ConstraintType
HexagonTargetLowering::getConstraintType(StringRef Constraint) const {
  if (Constraint.size() == 1) {
    switch (Constraint[0]) {
      case 'q':
      case 'v':
        if (Subtarget.useHVXOps())
          return C_Register;
        break;
    }
  }
  return TargetLowering::getConstraintType(Constraint);
}

std::pair<unsigned, const TargetRegisterClass*>
HexagonTargetLowering::getRegForInlineAsmConstraint(
    const TargetRegisterInfo *TRI, StringRef Constraint, MVT VT) const {
  bool UseHVX = Subtarget.useHVXOps(), UseHVXDbl = Subtarget.useHVXDblOps();

  if (Constraint.size() == 1) {
    switch (Constraint[0]) {
    case 'r':   // R0-R31
      switch (VT.SimpleTy) {
      default:
        llvm_unreachable("getRegForInlineAsmConstraint Unhandled data type");
      case MVT::i1:
      case MVT::i8:
      case MVT::i16:
      case MVT::i32:
      case MVT::f32:
        return std::make_pair(0U, &Hexagon::IntRegsRegClass);
      case MVT::i64:
      case MVT::f64:
        return std::make_pair(0U, &Hexagon::DoubleRegsRegClass);
      }
    case 'q': // q0-q3
      switch (VT.getSizeInBits()) {
      default:
        llvm_unreachable("getRegForInlineAsmConstraint Unhandled vector size");
      case 512:
        return std::make_pair(0U, &Hexagon::VecPredRegsRegClass);
      case 1024:
        return std::make_pair(0U, &Hexagon::VecPredRegs128BRegClass);
      }
    case 'v': // V0-V31
      switch (VT.getSizeInBits()) {
      default:
        llvm_unreachable("getRegForInlineAsmConstraint Unhandled vector size");
      case 512:
        return std::make_pair(0U, &Hexagon::VectorRegsRegClass);
      case 1024:
        if (Subtarget.hasV60TOps() && UseHVX && UseHVXDbl)
          return std::make_pair(0U, &Hexagon::VectorRegs128BRegClass);
        return std::make_pair(0U, &Hexagon::VecDblRegsRegClass);
      case 2048:
        return std::make_pair(0U, &Hexagon::VecDblRegs128BRegClass);
      }

    default:
      llvm_unreachable("Unknown asm register class");
    }
  }

  return TargetLowering::getRegForInlineAsmConstraint(TRI, Constraint, VT);
}

/// isFPImmLegal - Returns true if the target can instruction select the
/// specified FP immediate natively. If false, the legalizer will
/// materialize the FP immediate as a load from a constant pool.
bool HexagonTargetLowering::isFPImmLegal(const APFloat &Imm, EVT VT) const {
  return Subtarget.hasV5TOps();
}

/// isLegalAddressingMode - Return true if the addressing mode represented by
/// AM is legal for this target, for a load/store of the specified type.
bool HexagonTargetLowering::isLegalAddressingMode(const DataLayout &DL,
                                                  const AddrMode &AM, Type *Ty,
                                                  unsigned AS) const {
  if (Ty->isSized()) {
    // When LSR detects uses of the same base address to access different
    // types (e.g. unions), it will assume a conservative type for these
    // uses:
    //   LSR Use: Kind=Address of void in addrspace(4294967295), ...
    // The type Ty passed here would then be "void". Skip the alignment
    // checks, but do not return false right away, since that confuses
    // LSR into crashing.
    unsigned A = DL.getABITypeAlignment(Ty);
    // The base offset must be a multiple of the alignment.
    if ((AM.BaseOffs % A) != 0)
      return false;
    // The shifted offset must fit in 11 bits.
    if (!isInt<11>(AM.BaseOffs >> Log2_32(A)))
      return false;
  }

  // No global is ever allowed as a base.
  if (AM.BaseGV)
    return false;

  int Scale = AM.Scale;
  if (Scale < 0)
    Scale = -Scale;
  switch (Scale) {
  case 0:  // No scale reg, "r+i", "r", or just "i".
    break;
  default: // No scaled addressing mode.
    return false;
  }
  return true;
}

/// Return true if folding a constant offset with the given GlobalAddress is
/// legal.  It is frequently not legal in PIC relocation models.
bool HexagonTargetLowering::isOffsetFoldingLegal(const GlobalAddressSDNode *GA)
      const {
  return HTM.getRelocationModel() == Reloc::Static;
}

/// isLegalICmpImmediate - Return true if the specified immediate is legal
/// icmp immediate, that is the target has icmp instructions which can compare
/// a register against the immediate without having to materialize the
/// immediate into a register.
bool HexagonTargetLowering::isLegalICmpImmediate(int64_t Imm) const {
  return Imm >= -512 && Imm <= 511;
}

/// IsEligibleForTailCallOptimization - Check whether the call is eligible
/// for tail call optimization. Targets which want to do tail call
/// optimization should implement this function.
bool HexagonTargetLowering::IsEligibleForTailCallOptimization(
                                 SDValue Callee,
                                 CallingConv::ID CalleeCC,
                                 bool isVarArg,
                                 bool isCalleeStructRet,
                                 bool isCallerStructRet,
                                 const SmallVectorImpl<ISD::OutputArg> &Outs,
                                 const SmallVectorImpl<SDValue> &OutVals,
                                 const SmallVectorImpl<ISD::InputArg> &Ins,
                                 SelectionDAG& DAG) const {
  const Function *CallerF = DAG.getMachineFunction().getFunction();
  CallingConv::ID CallerCC = CallerF->getCallingConv();
  bool CCMatch = CallerCC == CalleeCC;

  // ***************************************************************************
  //  Look for obvious safe cases to perform tail call optimization that do not
  //  require ABI changes.
  // ***************************************************************************

  // If this is a tail call via a function pointer, then don't do it!
  if (!isa<GlobalAddressSDNode>(Callee) &&
      !isa<ExternalSymbolSDNode>(Callee)) {
    return false;
  }

  // Do not optimize if the calling conventions do not match and the conventions
  // used are not C or Fast.
  if (!CCMatch) {
    bool R = (CallerCC == CallingConv::C || CallerCC == CallingConv::Fast);
    bool E = (CalleeCC == CallingConv::C || CalleeCC == CallingConv::Fast);
    // If R & E, then ok.
    if (!R || !E)
      return false;
  }

  // Do not tail call optimize vararg calls.
  if (isVarArg)
    return false;

  // Also avoid tail call optimization if either caller or callee uses struct
  // return semantics.
  if (isCalleeStructRet || isCallerStructRet)
    return false;

  // In addition to the cases above, we also disable Tail Call Optimization if
  // the calling convention code that at least one outgoing argument needs to
  // go on the stack. We cannot check that here because at this point that
  // information is not available.
  return true;
}

/// Returns the target specific optimal type for load and store operations as
/// a result of memset, memcpy, and memmove lowering.
///
/// If DstAlign is zero that means it's safe to destination alignment can
/// satisfy any constraint. Similarly if SrcAlign is zero it means there isn't
/// a need to check it against alignment requirement, probably because the
/// source does not need to be loaded. If 'IsMemset' is true, that means it's
/// expanding a memset. If 'ZeroMemset' is true, that means it's a memset of
/// zero. 'MemcpyStrSrc' indicates whether the memcpy source is constant so it
/// does not need to be loaded.  It returns EVT::Other if the type should be
/// determined using generic target-independent logic.
EVT HexagonTargetLowering::getOptimalMemOpType(uint64_t Size,
      unsigned DstAlign, unsigned SrcAlign, bool IsMemset, bool ZeroMemset,
      bool MemcpyStrSrc, MachineFunction &MF) const {

  auto Aligned = [](unsigned GivenA, unsigned MinA) -> bool {
    return (GivenA % MinA) == 0;
  };

  if (Size >= 8 && Aligned(DstAlign, 8) && (IsMemset || Aligned(SrcAlign, 8)))
    return MVT::i64;
  if (Size >= 4 && Aligned(DstAlign, 4) && (IsMemset || Aligned(SrcAlign, 4)))
    return MVT::i32;
  if (Size >= 2 && Aligned(DstAlign, 2) && (IsMemset || Aligned(SrcAlign, 2)))
    return MVT::i16;

  return MVT::Other;
}

bool HexagonTargetLowering::allowsMisalignedMemoryAccesses(EVT VT,
      unsigned AS, unsigned Align, bool *Fast) const {
  if (Fast)
    *Fast = false;

  switch (VT.getSimpleVT().SimpleTy) {
  default:
    return false;
  case MVT::v64i8:
  case MVT::v128i8:
  case MVT::v256i8:
  case MVT::v32i16:
  case MVT::v64i16:
  case MVT::v128i16:
  case MVT::v16i32:
  case MVT::v32i32:
  case MVT::v64i32:
  case MVT::v8i64:
  case MVT::v16i64:
  case MVT::v32i64:
    return true;
  }
  return false;
}

std::pair<const TargetRegisterClass*, uint8_t>
HexagonTargetLowering::findRepresentativeClass(const TargetRegisterInfo *TRI,
      MVT VT) const {
  const TargetRegisterClass *RRC = nullptr;

  uint8_t Cost = 1;
  switch (VT.SimpleTy) {
  default:
    return TargetLowering::findRepresentativeClass(TRI, VT);
  case MVT::v64i8:
  case MVT::v32i16:
  case MVT::v16i32:
  case MVT::v8i64:
    RRC = &Hexagon::VectorRegsRegClass;
    break;
  case MVT::v128i8:
  case MVT::v64i16:
  case MVT::v32i32:
  case MVT::v16i64:
    if (Subtarget.hasV60TOps() && Subtarget.useHVXOps() &&
        Subtarget.useHVXDblOps())
      RRC = &Hexagon::VectorRegs128BRegClass;
    else
      RRC = &Hexagon::VecDblRegsRegClass;
    break;
  case MVT::v256i8:
  case MVT::v128i16:
  case MVT::v64i32:
  case MVT::v32i64:
    RRC = &Hexagon::VecDblRegs128BRegClass;
    break;
  }
  return std::make_pair(RRC, Cost);
}

Value *HexagonTargetLowering::emitLoadLinked(IRBuilder<> &Builder, Value *Addr,
      AtomicOrdering Ord) const {
  BasicBlock *BB = Builder.GetInsertBlock();
  Module *M = BB->getParent()->getParent();
  Type *Ty = cast<PointerType>(Addr->getType())->getElementType();
  unsigned SZ = Ty->getPrimitiveSizeInBits();
  assert((SZ == 32 || SZ == 64) && "Only 32/64-bit atomic loads supported");
  Intrinsic::ID IntID = (SZ == 32) ? Intrinsic::hexagon_L2_loadw_locked
                                   : Intrinsic::hexagon_L4_loadd_locked;
  Value *Fn = Intrinsic::getDeclaration(M, IntID);
  return Builder.CreateCall(Fn, Addr, "larx");
}

/// Perform a store-conditional operation to Addr. Return the status of the
/// store. This should be 0 if the store succeeded, non-zero otherwise.
Value *HexagonTargetLowering::emitStoreConditional(IRBuilder<> &Builder,
      Value *Val, Value *Addr, AtomicOrdering Ord) const {
  BasicBlock *BB = Builder.GetInsertBlock();
  Module *M = BB->getParent()->getParent();
  Type *Ty = Val->getType();
  unsigned SZ = Ty->getPrimitiveSizeInBits();
  assert((SZ == 32 || SZ == 64) && "Only 32/64-bit atomic stores supported");
  Intrinsic::ID IntID = (SZ == 32) ? Intrinsic::hexagon_S2_storew_locked
                                   : Intrinsic::hexagon_S4_stored_locked;
  Value *Fn = Intrinsic::getDeclaration(M, IntID);
  Value *Call = Builder.CreateCall(Fn, {Addr, Val}, "stcx");
  Value *Cmp = Builder.CreateICmpEQ(Call, Builder.getInt32(0), "");
  Value *Ext = Builder.CreateZExt(Cmp, Type::getInt32Ty(M->getContext()));
  return Ext;
}

TargetLowering::AtomicExpansionKind
HexagonTargetLowering::shouldExpandAtomicLoadInIR(LoadInst *LI) const {
  // Do not expand loads and stores that don't exceed 64 bits.
  return LI->getType()->getPrimitiveSizeInBits() > 64
             ? AtomicExpansionKind::LLOnly
             : AtomicExpansionKind::None;
}

bool HexagonTargetLowering::shouldExpandAtomicStoreInIR(StoreInst *SI) const {
  // Do not expand loads and stores that don't exceed 64 bits.
  return SI->getValueOperand()->getType()->getPrimitiveSizeInBits() > 64;
}

bool HexagonTargetLowering::shouldExpandAtomicCmpXchgInIR(
      AtomicCmpXchgInst *AI) const {
  const DataLayout &DL = AI->getModule()->getDataLayout();
  unsigned Size = DL.getTypeStoreSize(AI->getCompareOperand()->getType());
  return Size >= 4 && Size <= 8;
}
