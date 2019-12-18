//===- llvm/unittest/CodeGen/AArch64SelectionDAGTest.cpp -------------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "llvm/CodeGen/SelectionDAG.h"
#include "llvm/Analysis/OptimizationRemarkEmitter.h"
#include "llvm/AsmParser/Parser.h"
#include "llvm/CodeGen/MachineModuleInfo.h"
#include "llvm/CodeGen/TargetLowering.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/TargetRegistry.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Target/TargetMachine.h"
#include "gtest/gtest.h"

using namespace llvm;

namespace {

class AArch64SelectionDAGTest : public testing::Test {
protected:
  static void SetUpTestCase() {
    InitializeAllTargets();
    InitializeAllTargetMCs();
  }

  void SetUp() override {
    StringRef Assembly = "define void @f() { ret void }";

    Triple TargetTriple("aarch64--");
    std::string Error;
    const Target *T = TargetRegistry::lookupTarget("", TargetTriple, Error);
    // FIXME: These tests do not depend on AArch64 specifically, but we have to
    // initialize a target. A skeleton Target for unittests would allow us to
    // always run these tests.
    if (!T)
      return;

    TargetOptions Options;
    TM = std::unique_ptr<LLVMTargetMachine>(static_cast<LLVMTargetMachine*>(
        T->createTargetMachine("AArch64", "", "", Options, None, None,
                               CodeGenOpt::Aggressive)));
    if (!TM)
      return;

    SMDiagnostic SMError;
    M = parseAssemblyString(Assembly, SMError, Context);
    if (!M)
      report_fatal_error(SMError.getMessage());
    M->setDataLayout(TM->createDataLayout());

    F = M->getFunction("f");
    if (!F)
      report_fatal_error("F?");

    MachineModuleInfo MMI(TM.get());

    MF = make_unique<MachineFunction>(*F, *TM, *TM->getSubtargetImpl(*F), 0,
                                      MMI);

    DAG = make_unique<SelectionDAG>(*TM, CodeGenOpt::None);
    if (!DAG)
      report_fatal_error("DAG?");
    OptimizationRemarkEmitter ORE(F);
    DAG->init(*MF, ORE, nullptr, nullptr, nullptr);
  }

  LLVMContext Context;
  std::unique_ptr<LLVMTargetMachine> TM;
  std::unique_ptr<Module> M;
  Function *F;
  std::unique_ptr<MachineFunction> MF;
  std::unique_ptr<SelectionDAG> DAG;
};

TEST_F(AArch64SelectionDAGTest, computeKnownBits_ZERO_EXTEND_VECTOR_INREG) {
  if (!TM)
    return;
  SDLoc Loc;
  auto Int8VT = EVT::getIntegerVT(Context, 8);
  auto Int16VT = EVT::getIntegerVT(Context, 16);
  auto InVecVT = EVT::getVectorVT(Context, Int8VT, 4);
  auto OutVecVT = EVT::getVectorVT(Context, Int16VT, 2);
  auto InVec = DAG->getConstant(0, Loc, InVecVT);
  auto Op = DAG->getNode(ISD::ZERO_EXTEND_VECTOR_INREG, Loc, OutVecVT, InVec);
  auto DemandedElts = APInt(2, 3);
  KnownBits Known = DAG->computeKnownBits(Op, DemandedElts);
  EXPECT_TRUE(Known.isZero());
}

TEST_F(AArch64SelectionDAGTest, computeKnownBits_EXTRACT_SUBVECTOR) {
  if (!TM)
    return;
  SDLoc Loc;
  auto IntVT = EVT::getIntegerVT(Context, 8);
  auto VecVT = EVT::getVectorVT(Context, IntVT, 3);
  auto IdxVT = EVT::getIntegerVT(Context, 64);
  auto Vec = DAG->getConstant(0, Loc, VecVT);
  auto ZeroIdx = DAG->getConstant(0, Loc, IdxVT);
  auto Op = DAG->getNode(ISD::EXTRACT_SUBVECTOR, Loc, VecVT, Vec, ZeroIdx);
  auto DemandedElts = APInt(3, 7);
  KnownBits Known = DAG->computeKnownBits(Op, DemandedElts);
  EXPECT_TRUE(Known.isZero());
}

TEST_F(AArch64SelectionDAGTest, ComputeNumSignBits_SIGN_EXTEND_VECTOR_INREG) {
  if (!TM)
    return;
  SDLoc Loc;
  auto Int8VT = EVT::getIntegerVT(Context, 8);
  auto Int16VT = EVT::getIntegerVT(Context, 16);
  auto InVecVT = EVT::getVectorVT(Context, Int8VT, 4);
  auto OutVecVT = EVT::getVectorVT(Context, Int16VT, 2);
  auto InVec = DAG->getConstant(1, Loc, InVecVT);
  auto Op = DAG->getNode(ISD::SIGN_EXTEND_VECTOR_INREG, Loc, OutVecVT, InVec);
  auto DemandedElts = APInt(2, 3);
  EXPECT_EQ(DAG->ComputeNumSignBits(Op, DemandedElts), 15u);
}

TEST_F(AArch64SelectionDAGTest, ComputeNumSignBits_EXTRACT_SUBVECTOR) {
  if (!TM)
    return;
  SDLoc Loc;
  auto IntVT = EVT::getIntegerVT(Context, 8);
  auto VecVT = EVT::getVectorVT(Context, IntVT, 3);
  auto IdxVT = EVT::getIntegerVT(Context, 64);
  auto Vec = DAG->getConstant(1, Loc, VecVT);
  auto ZeroIdx = DAG->getConstant(0, Loc, IdxVT);
  auto Op = DAG->getNode(ISD::EXTRACT_SUBVECTOR, Loc, VecVT, Vec, ZeroIdx);
  auto DemandedElts = APInt(3, 7);
  EXPECT_EQ(DAG->ComputeNumSignBits(Op, DemandedElts), 7u);
}

TEST_F(AArch64SelectionDAGTest, SimplifyDemandedVectorElts_EXTRACT_SUBVECTOR) {
  if (!TM)
    return;

  TargetLowering TL(*TM);

  SDLoc Loc;
  auto IntVT = EVT::getIntegerVT(Context, 8);
  auto VecVT = EVT::getVectorVT(Context, IntVT, 3);
  auto IdxVT = EVT::getIntegerVT(Context, 64);
  auto Vec = DAG->getConstant(1, Loc, VecVT);
  auto ZeroIdx = DAG->getConstant(0, Loc, IdxVT);
  auto Op = DAG->getNode(ISD::EXTRACT_SUBVECTOR, Loc, VecVT, Vec, ZeroIdx);
  auto DemandedElts = APInt(3, 7);
  auto KnownUndef = APInt(3, 0);
  auto KnownZero = APInt(3, 0);
  TargetLowering::TargetLoweringOpt TLO(*DAG, false, false);
  EXPECT_EQ(TL.SimplifyDemandedVectorElts(Op, DemandedElts, KnownUndef,
                                          KnownZero, TLO),
            false);
}

} // end anonymous namespace
