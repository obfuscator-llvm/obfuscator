//===-- HexagonTargetTransformInfo.cpp - Hexagon specific TTI pass --------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
/// \file
/// This file implements a TargetTransformInfo analysis pass specific to the
/// Hexagon target machine. It uses the target's detailed information to provide
/// more precise answers to certain TTI queries, while letting the target
/// independent and default TTI implementations handle the rest.
///
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_HEXAGON_HEXAGONTARGETTRANSFORMINFO_H
#define LLVM_LIB_TARGET_HEXAGON_HEXAGONTARGETTRANSFORMINFO_H

#include "Hexagon.h"
#include "HexagonTargetMachine.h"
#include "llvm/Analysis/TargetTransformInfo.h"
#include "llvm/CodeGen/BasicTTIImpl.h"
#include "llvm/Target/TargetLowering.h"

namespace llvm {

class HexagonTTIImpl : public BasicTTIImplBase<HexagonTTIImpl> {
  typedef BasicTTIImplBase<HexagonTTIImpl> BaseT;
  typedef TargetTransformInfo TTI;
  friend BaseT;

  const HexagonSubtarget *ST;
  const HexagonTargetLowering *TLI;

  const HexagonSubtarget *getST() const { return ST; }
  const HexagonTargetLowering *getTLI() const { return TLI; }

public:
  explicit HexagonTTIImpl(const HexagonTargetMachine *TM, const Function &F)
      : BaseT(TM, F.getParent()->getDataLayout()), ST(TM->getSubtargetImpl(F)),
        TLI(ST->getTargetLowering()) {}

  /// \name Scalar TTI Implementations
  /// @{

  TTI::PopcntSupportKind getPopcntSupport(unsigned IntTyWidthInBit) const;

  // The Hexagon target can unroll loops with run-time trip counts.
  void getUnrollingPreferences(Loop *L, ScalarEvolution &SE,
                               TTI::UnrollingPreferences &UP);

  // L1 cache prefetch.
  unsigned getPrefetchDistance() const;
  unsigned getCacheLineSize() const;

  /// @}

  /// \name Vector TTI Implementations
  /// @{

  unsigned getNumberOfRegisters(bool vector) const;

  /// @}

  int getUserCost(const User *U, ArrayRef<const Value *> Operands);

  // Hexagon specific decision to generate a lookup table.
  bool shouldBuildLookupTables() const;
};

} // end namespace llvm

#endif
