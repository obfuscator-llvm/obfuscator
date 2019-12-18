//===- LegalizerHelperTest.cpp
//-----------------------------------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "GISelMITest.h"

namespace {

class DummyGISelObserver : public GISelChangeObserver {
public:
  void changingInstr(MachineInstr &MI) override {}
  void changedInstr(MachineInstr &MI) override {}
  void createdInstr(MachineInstr &MI) override {}
  void erasingInstr(MachineInstr &MI) override {}
};

// Test CTTZ expansion when CTTZ_ZERO_UNDEF is legal or custom,
// in which case it becomes CTTZ_ZERO_UNDEF with select.
TEST_F(GISelMITest, LowerBitCountingCTTZ0) {
  if (!TM)
    return;

  // Declare your legalization info
  DefineLegalizerInfo(
      A, { getActionDefinitionsBuilder(G_CTTZ_ZERO_UNDEF).legalFor({s64}); });
  // Build Instr
  auto MIBCTTZ =
      B.buildInstr(TargetOpcode::G_CTTZ, {LLT::scalar(64)}, {Copies[0]});
  AInfo Info(MF->getSubtarget());
  DummyGISelObserver Observer;
  LegalizerHelper Helper(*MF, Info, Observer, B);
  // Perform Legalization
  ASSERT_TRUE(Helper.lower(*MIBCTTZ, 0, LLT::scalar(64)) ==
              LegalizerHelper::LegalizeResult::Legalized);

  auto CheckStr = R"(
  CHECK: [[CZU:%[0-9]+]]:_(s64) = G_CTTZ_ZERO_UNDEF %0
  CHECK: [[ZERO:%[0-9]+]]:_(s64) = G_CONSTANT i64 0
  CHECK: [[SIXTY4:%[0-9]+]]:_(s64) = G_CONSTANT i64 64
  CHECK: [[CMP:%[0-9]+]]:_(s1) = G_ICMP intpred(eq), %0:_(s64), [[ZERO]]
  CHECK: [[SEL:%[0-9]+]]:_(s64) = G_SELECT [[CMP]]:_(s1), [[SIXTY4]]:_, [[CZU]]
  )";

  // Check
  ASSERT_TRUE(CheckMachineFunction(*MF, CheckStr));
}

// CTTZ expansion in terms of CTLZ
TEST_F(GISelMITest, LowerBitCountingCTTZ1) {
  if (!TM)
    return;

  // Declare your legalization info
  DefineLegalizerInfo(A,
                      { getActionDefinitionsBuilder(G_CTLZ).legalFor({s64}); });
  // Build Instr
  auto MIBCTTZ =
      B.buildInstr(TargetOpcode::G_CTTZ, {LLT::scalar(64)}, {Copies[0]});
  AInfo Info(MF->getSubtarget());
  DummyGISelObserver Observer;
  LegalizerHelper Helper(*MF, Info, Observer, B);
  // Perform Legalization
  ASSERT_TRUE(Helper.lower(*MIBCTTZ, 0, LLT::scalar(64)) ==
              LegalizerHelper::LegalizeResult::Legalized);

  auto CheckStr = R"(
  CHECK: [[NEG1:%[0-9]+]]:_(s64) = G_CONSTANT i64 -1
  CHECK: [[NOT:%[0-9]+]]:_(s64) = G_XOR %0:_, [[NEG1]]
  CHECK: [[SUB1:%[0-9]+]]:_(s64) = G_ADD %0:_, [[NEG1]]
  CHECK: [[AND1:%[0-9]+]]:_(s64) = G_AND [[NOT]]:_, [[SUB1]]:_
  CHECK: [[CST64:%[0-9]+]]:_(s64) = G_CONSTANT i64 64
  CHECK: [[CTLZ:%[0-9]+]]:_(s64) = G_CTLZ [[AND1]]:_
  CHECK: G_SUB [[CST64]]:_, [[CTLZ]]:_
  )";

  // Check
  ASSERT_TRUE(CheckMachineFunction(*MF, CheckStr));
}

// CTTZ expansion in terms of CTPOP
TEST_F(GISelMITest, LowerBitCountingCTTZ2) {
  if (!TM)
    return;

  // Declare your legalization info
  DefineLegalizerInfo(
      A, { getActionDefinitionsBuilder(G_CTPOP).legalFor({s64}); });
  // Build
  auto MIBCTTZ =
      B.buildInstr(TargetOpcode::G_CTTZ, {LLT::scalar(64)}, {Copies[0]});
  AInfo Info(MF->getSubtarget());
  DummyGISelObserver Observer;
  LegalizerHelper Helper(*MF, Info, Observer, B);
  ASSERT_TRUE(Helper.lower(*MIBCTTZ, 0, LLT::scalar(64)) ==
              LegalizerHelper::LegalizeResult::Legalized);

  auto CheckStr = R"(
  CHECK: [[NEG1:%[0-9]+]]:_(s64) = G_CONSTANT i64 -1
  CHECK: [[NOT:%[0-9]+]]:_(s64) = G_XOR %0:_, [[NEG1]]
  CHECK: [[SUB1:%[0-9]+]]:_(s64) = G_ADD %0:_, [[NEG1]]
  CHECK: [[AND1:%[0-9]+]]:_(s64) = G_AND [[NOT]]:_, [[SUB1]]:_
  CHECK: [[POP:%[0-9]+]]:_(s64) = G_CTPOP [[AND1]]
  )";

  // Check
  ASSERT_TRUE(CheckMachineFunction(*MF, CheckStr));
}

// CTTZ_ZERO_UNDEF expansion in terms of CTTZ
TEST_F(GISelMITest, LowerBitCountingCTTZ3) {
  if (!TM)
    return;

  // Declare your legalization info
  DefineLegalizerInfo(A,
                      { getActionDefinitionsBuilder(G_CTTZ).legalFor({s64}); });
  // Build
  auto MIBCTTZ = B.buildInstr(TargetOpcode::G_CTTZ_ZERO_UNDEF,
                              {LLT::scalar(64)}, {Copies[0]});
  AInfo Info(MF->getSubtarget());
  DummyGISelObserver Observer;
  LegalizerHelper Helper(*MF, Info, Observer, B);
  ASSERT_TRUE(Helper.lower(*MIBCTTZ, 0, LLT::scalar(64)) ==
              LegalizerHelper::LegalizeResult::Legalized);

  auto CheckStr = R"(
  CHECK: CTTZ
  )";

  // Check
  ASSERT_TRUE(CheckMachineFunction(*MF, CheckStr));
}

// CTLZ expansion in terms of CTLZ_ZERO_UNDEF
TEST_F(GISelMITest, LowerBitCountingCTLZ0) {
  if (!TM)
    return;

  // Declare your legalization info
  DefineLegalizerInfo(
      A, { getActionDefinitionsBuilder(G_CTLZ_ZERO_UNDEF).legalFor({s64}); });
  // Build
  auto MIBCTLZ =
      B.buildInstr(TargetOpcode::G_CTLZ, {LLT::scalar(64)}, {Copies[0]});
  AInfo Info(MF->getSubtarget());
  DummyGISelObserver Observer;
  LegalizerHelper Helper(*MF, Info, Observer, B);
  ASSERT_TRUE(Helper.lower(*MIBCTLZ, 0, LLT::scalar(64)) ==
              LegalizerHelper::LegalizeResult::Legalized);

  auto CheckStr = R"(
  CHECK: [[CZU:%[0-9]+]]:_(s64) = G_CTLZ_ZERO_UNDEF %0
  CHECK: [[ZERO:%[0-9]+]]:_(s64) = G_CONSTANT i64 0
  CHECK: [[SIXTY4:%[0-9]+]]:_(s64) = G_CONSTANT i64 64
  CHECK: [[CMP:%[0-9]+]]:_(s1) = G_ICMP intpred(eq), %0:_(s64), [[ZERO]]
  CHECK: [[SEL:%[0-9]+]]:_(s64) = G_SELECT [[CMP]]:_(s1), [[SIXTY4]]:_, [[CZU]]
  )";

  // Check
  ASSERT_TRUE(CheckMachineFunction(*MF, CheckStr));
}

// CTLZ expansion in terms of CTLZ_ZERO_UNDEF if the latter is a libcall
TEST_F(GISelMITest, LowerBitCountingCTLZLibcall) {
  if (!TM)
    return;

  // Declare your legalization info
  DefineLegalizerInfo(
      A, { getActionDefinitionsBuilder(G_CTLZ_ZERO_UNDEF).libcallFor({s64}); });
  // Build
  auto MIBCTLZ =
      B.buildInstr(TargetOpcode::G_CTLZ, {LLT::scalar(64)}, {Copies[0]});
  AInfo Info(MF->getSubtarget());
  DummyGISelObserver Observer;
  LegalizerHelper Helper(*MF, Info, Observer, B);
  ASSERT_TRUE(Helper.lower(*MIBCTLZ, 0, LLT::scalar(64)) ==
              LegalizerHelper::LegalizeResult::Legalized);

  auto CheckStr = R"(
  CHECK: [[CZU:%[0-9]+]]:_(s64) = G_CTLZ_ZERO_UNDEF %0
  CHECK: [[ZERO:%[0-9]+]]:_(s64) = G_CONSTANT i64 0
  CHECK: [[THIRTY2:%[0-9]+]]:_(s64) = G_CONSTANT i64 64
  CHECK: [[CMP:%[0-9]+]]:_(s1) = G_ICMP intpred(eq), %0:_(s64), [[ZERO]]
  CHECK: [[SEL:%[0-9]+]]:_(s64) = G_SELECT [[CMP]]:_(s1), [[THIRTY2]]:_, [[CZU]]
  )";

  // Check
  ASSERT_TRUE(CheckMachineFunction(*MF, CheckStr));
}

// CTLZ expansion
TEST_F(GISelMITest, LowerBitCountingCTLZ1) {
  if (!TM)
    return;

  // Declare your legalization info
  DefineLegalizerInfo(A,
                      { getActionDefinitionsBuilder(G_CTPOP).legalFor({s8}); });
  // Build
  // Trunc it to s8.
  LLT s8{LLT::scalar(8)};
  auto MIBTrunc = B.buildTrunc(s8, Copies[0]);
  auto MIBCTLZ = B.buildInstr(TargetOpcode::G_CTLZ, {s8}, {MIBTrunc});
  AInfo Info(MF->getSubtarget());
  DummyGISelObserver Observer;
  LegalizerHelper Helper(*MF, Info, Observer, B);
  ASSERT_TRUE(Helper.lower(*MIBCTLZ, 0, s8) ==
              LegalizerHelper::LegalizeResult::Legalized);

  auto CheckStr = R"(
  CHECK: [[Trunc:%[0-9]+]]:_(s8) = G_TRUNC
  CHECK: [[Cst1:%[0-9]+]]:_(s8) = G_CONSTANT i8 1
  CHECK: [[Sh1:%[0-9]+]]:_(s8) = G_LSHR [[Trunc]]:_, [[Cst1]]:_
  CHECK: [[Or1:%[0-9]+]]:_(s8) = G_OR [[Trunc]]:_, [[Sh1]]:_
  CHECK: [[Cst2:%[0-9]+]]:_(s8) = G_CONSTANT i8 2
  CHECK: [[Sh2:%[0-9]+]]:_(s8) = G_LSHR [[Or1]]:_, [[Cst2]]:_
  CHECK: [[Or2:%[0-9]+]]:_(s8) = G_OR [[Or1]]:_, [[Sh2]]:_
  CHECK: [[Cst4:%[0-9]+]]:_(s8) = G_CONSTANT i8 4
  CHECK: [[Sh4:%[0-9]+]]:_(s8) = G_LSHR [[Or2]]:_, [[Cst4]]:_
  CHECK: [[Or4:%[0-9]+]]:_(s8) = G_OR [[Or2]]:_, [[Sh4]]:_
  CHECK: [[CTPOP:%[0-9]+]]:_(s8) = G_CTPOP [[Or4]]:_
  CHECK: [[Len:%[0-9]+]]:_(s8) = G_CONSTANT i8 8
  CHECK: [[Sub:%[0-9]+]]:_(s8) = G_SUB [[Len]]:_, [[CTPOP]]:_
  )";

  // Check
  ASSERT_TRUE(CheckMachineFunction(*MF, CheckStr));
}

// CTLZ widening.
TEST_F(GISelMITest, WidenBitCountingCTLZ) {
  if (!TM)
    return;

  // Declare your legalization info
  DefineLegalizerInfo(A,
                      { getActionDefinitionsBuilder(G_CTLZ).legalFor({s16}); });
  // Build
  // Trunc it to s8.
  LLT s8{LLT::scalar(8)};
  LLT s16{LLT::scalar(16)};
  auto MIBTrunc = B.buildTrunc(s8, Copies[0]);
  auto MIBCTLZ = B.buildInstr(TargetOpcode::G_CTLZ, {s8}, {MIBTrunc});
  AInfo Info(MF->getSubtarget());
  DummyGISelObserver Observer;
  LegalizerHelper Helper(*MF, Info, Observer, B);
  ASSERT_TRUE(Helper.widenScalar(*MIBCTLZ, 0, s16) ==
              LegalizerHelper::LegalizeResult::Legalized);

  auto CheckStr = R"(
  CHECK: [[Trunc:%[0-9]+]]:_(s8) = G_TRUNC
  CHECK: [[Zext:%[0-9]+]]:_(s16) = G_ZEXT [[Trunc]]
  CHECK: [[Ctlz:%[0-9]+]]:_(s16) = G_CTLZ [[Zext]]
  CHECK: [[Cst8:%[0-9]+]]:_(s16) = G_CONSTANT i16 8
  CHECK: [[Sub:%[0-9]+]]:_(s16) = G_SUB [[Ctlz]]:_, [[Cst8]]:_
  CHECK: [[Trunc:%[0-9]+]]:_(s8) = G_TRUNC [[Sub]]
  )";

  // Check
  ASSERT_TRUE(CheckMachineFunction(*MF, CheckStr));
}

// CTLZ_ZERO_UNDEF widening.
TEST_F(GISelMITest, WidenBitCountingCTLZZeroUndef) {
  if (!TM)
    return;

  // Declare your legalization info
  DefineLegalizerInfo(
      A, { getActionDefinitionsBuilder(G_CTLZ_ZERO_UNDEF).legalFor({s16}); });
  // Build
  // Trunc it to s8.
  LLT s8{LLT::scalar(8)};
  LLT s16{LLT::scalar(16)};
  auto MIBTrunc = B.buildTrunc(s8, Copies[0]);
  auto MIBCTLZ_ZU =
      B.buildInstr(TargetOpcode::G_CTLZ_ZERO_UNDEF, {s8}, {MIBTrunc});
  AInfo Info(MF->getSubtarget());
  DummyGISelObserver Observer;
  LegalizerHelper Helper(*MF, Info, Observer, B);
  ASSERT_TRUE(Helper.widenScalar(*MIBCTLZ_ZU, 0, s16) ==
              LegalizerHelper::LegalizeResult::Legalized);

  auto CheckStr = R"(
  CHECK: [[Trunc:%[0-9]+]]:_(s8) = G_TRUNC
  CHECK: [[Zext:%[0-9]+]]:_(s16) = G_ZEXT [[Trunc]]
  CHECK: [[CtlzZu:%[0-9]+]]:_(s16) = G_CTLZ_ZERO_UNDEF [[Zext]]
  CHECK: [[Cst8:%[0-9]+]]:_(s16) = G_CONSTANT i16 8
  CHECK: [[Sub:%[0-9]+]]:_(s16) = G_SUB [[CtlzZu]]:_, [[Cst8]]:_
  CHECK: [[Trunc:%[0-9]+]]:_(s8) = G_TRUNC [[Sub]]
  )";

  // Check
  ASSERT_TRUE(CheckMachineFunction(*MF, CheckStr));
}

// CTPOP widening.
TEST_F(GISelMITest, WidenBitCountingCTPOP) {
  if (!TM)
    return;

  // Declare your legalization info
  DefineLegalizerInfo(
      A, { getActionDefinitionsBuilder(G_CTPOP).legalFor({s16}); });
  // Build
  // Trunc it to s8.
  LLT s8{LLT::scalar(8)};
  LLT s16{LLT::scalar(16)};
  auto MIBTrunc = B.buildTrunc(s8, Copies[0]);
  auto MIBCTPOP = B.buildInstr(TargetOpcode::G_CTPOP, {s8}, {MIBTrunc});
  AInfo Info(MF->getSubtarget());
  DummyGISelObserver Observer;
  LegalizerHelper Helper(*MF, Info, Observer, B);
  ASSERT_TRUE(Helper.widenScalar(*MIBCTPOP, 0, s16) ==
              LegalizerHelper::LegalizeResult::Legalized);

  auto CheckStr = R"(
  CHECK: [[Trunc:%[0-9]+]]:_(s8) = G_TRUNC
  CHECK: [[Zext:%[0-9]+]]:_(s16) = G_ZEXT [[Trunc]]
  CHECK: [[Ctpop:%[0-9]+]]:_(s16) = G_CTPOP [[Zext]]
  CHECK: [[Trunc:%[0-9]+]]:_(s8) = G_TRUNC [[Ctpop]]
  )";

  // Check
  ASSERT_TRUE(CheckMachineFunction(*MF, CheckStr));
}

// CTTZ_ZERO_UNDEF widening.
TEST_F(GISelMITest, WidenBitCountingCTTZ_ZERO_UNDEF) {
  if (!TM)
    return;

  // Declare your legalization info
  DefineLegalizerInfo(
      A, { getActionDefinitionsBuilder(G_CTTZ_ZERO_UNDEF).legalFor({s16}); });
  // Build
  // Trunc it to s8.
  LLT s8{LLT::scalar(8)};
  LLT s16{LLT::scalar(16)};
  auto MIBTrunc = B.buildTrunc(s8, Copies[0]);
  auto MIBCTTZ_ZERO_UNDEF =
      B.buildInstr(TargetOpcode::G_CTTZ_ZERO_UNDEF, {s8}, {MIBTrunc});
  AInfo Info(MF->getSubtarget());
  DummyGISelObserver Observer;
  LegalizerHelper Helper(*MF, Info, Observer, B);
  ASSERT_TRUE(Helper.widenScalar(*MIBCTTZ_ZERO_UNDEF, 0, s16) ==
              LegalizerHelper::LegalizeResult::Legalized);

  auto CheckStr = R"(
  CHECK: [[Trunc:%[0-9]+]]:_(s8) = G_TRUNC
  CHECK: [[Zext:%[0-9]+]]:_(s16) = G_ZEXT [[Trunc]]
  CHECK: [[CttzZu:%[0-9]+]]:_(s16) = G_CTTZ_ZERO_UNDEF [[Zext]]
  CHECK: [[Trunc:%[0-9]+]]:_(s8) = G_TRUNC [[CttzZu]]
  )";

  // Check
  ASSERT_TRUE(CheckMachineFunction(*MF, CheckStr));
}

// CTTZ widening.
TEST_F(GISelMITest, WidenBitCountingCTTZ) {
  if (!TM)
    return;

  // Declare your legalization info
  DefineLegalizerInfo(A,
                      { getActionDefinitionsBuilder(G_CTTZ).legalFor({s16}); });
  // Build
  // Trunc it to s8.
  LLT s8{LLT::scalar(8)};
  LLT s16{LLT::scalar(16)};
  auto MIBTrunc = B.buildTrunc(s8, Copies[0]);
  auto MIBCTTZ = B.buildInstr(TargetOpcode::G_CTTZ, {s8}, {MIBTrunc});
  AInfo Info(MF->getSubtarget());
  DummyGISelObserver Observer;
  LegalizerHelper Helper(*MF, Info, Observer, B);
  ASSERT_TRUE(Helper.widenScalar(*MIBCTTZ, 0, s16) ==
              LegalizerHelper::LegalizeResult::Legalized);

  auto CheckStr = R"(
  CHECK: [[Trunc:%[0-9]+]]:_(s8) = G_TRUNC
  CHECK: [[Zext:%[0-9]+]]:_(s16) = G_ZEXT [[Trunc]]
  CHECK: [[Cst:%[0-9]+]]:_(s16) = G_CONSTANT i16 256
  CHECK: [[Or:%[0-9]+]]:_(s16) = G_OR [[Zext]]:_, [[Cst]]
  CHECK: [[Cttz:%[0-9]+]]:_(s16) = G_CTTZ [[Or]]
  CHECK: [[Trunc:%[0-9]+]]:_(s8) = G_TRUNC [[Cttz]]
  )";

  // Check
  ASSERT_TRUE(CheckMachineFunction(*MF, CheckStr));
}
// UADDO widening.
TEST_F(GISelMITest, WidenUADDO) {
  if (!TM)
    return;

  // Declare your legalization info
  DefineLegalizerInfo(A,
                      { getActionDefinitionsBuilder(G_ADD).legalFor({s16}); });
  // Build
  // Trunc it to s8.
  LLT s8{LLT::scalar(8)};
  LLT s16{LLT::scalar(16)};
  auto MIBTrunc = B.buildTrunc(s8, Copies[0]);
  unsigned CarryReg = MRI->createGenericVirtualRegister(LLT::scalar(1));
  auto MIBUAddO =
      B.buildInstr(TargetOpcode::G_UADDO, {s8, CarryReg}, {MIBTrunc, MIBTrunc});
  AInfo Info(MF->getSubtarget());
  DummyGISelObserver Observer;
  LegalizerHelper Helper(*MF, Info, Observer, B);
  ASSERT_TRUE(Helper.widenScalar(*MIBUAddO, 0, s16) ==
              LegalizerHelper::LegalizeResult::Legalized);

  auto CheckStr = R"(
  CHECK: [[Trunc:%[0-9]+]]:_(s8) = G_TRUNC
  CHECK: [[LHS:%[0-9]+]]:_(s16) = G_ZEXT [[Trunc]]
  CHECK: [[RHS:%[0-9]+]]:_(s16) = G_ZEXT [[Trunc]]
  CHECK: [[ADD:%[0-9]+]]:_(s16) = G_ADD [[LHS]]:_, [[RHS]]:_
  CHECK: [[CST:%[0-9]+]]:_(s16) = G_CONSTANT i16 255
  CHECK: [[AND:%[0-9]+]]:_(s16) = G_AND [[ADD]]:_, [[CST]]:_
  CHECK: G_ICMP intpred(ne), [[ADD]]:_(s16), [[AND]]:_
  CHECK: G_TRUNC [[ADD]]
  )";

  // Check
  ASSERT_TRUE(CheckMachineFunction(*MF, CheckStr));
}

// USUBO widening.
TEST_F(GISelMITest, WidenUSUBO) {
  if (!TM)
    return;

  // Declare your legalization info
  DefineLegalizerInfo(A,
                      { getActionDefinitionsBuilder(G_SUB).legalFor({s16}); });
  // Build
  // Trunc it to s8.
  LLT s8{LLT::scalar(8)};
  LLT s16{LLT::scalar(16)};
  auto MIBTrunc = B.buildTrunc(s8, Copies[0]);
  unsigned CarryReg = MRI->createGenericVirtualRegister(LLT::scalar(1));
  auto MIBUSUBO =
      B.buildInstr(TargetOpcode::G_USUBO, {s8, CarryReg}, {MIBTrunc, MIBTrunc});
  AInfo Info(MF->getSubtarget());
  DummyGISelObserver Observer;
  LegalizerHelper Helper(*MF, Info, Observer, B);
  ASSERT_TRUE(Helper.widenScalar(*MIBUSUBO, 0, s16) ==
              LegalizerHelper::LegalizeResult::Legalized);

  auto CheckStr = R"(
  CHECK: [[Trunc:%[0-9]+]]:_(s8) = G_TRUNC
  CHECK: [[LHS:%[0-9]+]]:_(s16) = G_ZEXT [[Trunc]]
  CHECK: [[RHS:%[0-9]+]]:_(s16) = G_ZEXT [[Trunc]]
  CHECK: [[SUB:%[0-9]+]]:_(s16) = G_SUB [[LHS]]:_, [[RHS]]:_
  CHECK: [[CST:%[0-9]+]]:_(s16) = G_CONSTANT i16 255
  CHECK: [[AND:%[0-9]+]]:_(s16) = G_AND [[SUB]]:_, [[CST]]:_
  CHECK: G_ICMP intpred(ne), [[SUB]]:_(s16), [[AND]]:_
  CHECK: G_TRUNC [[SUB]]
  )";

  // Check
  ASSERT_TRUE(CheckMachineFunction(*MF, CheckStr));
}
} // namespace
