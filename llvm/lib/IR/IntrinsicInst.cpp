//===-- InstrinsicInst.cpp - Intrinsic Instruction Wrappers ---------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file implements methods that make it really easy to deal with intrinsic
// functions.
//
// All intrinsic function calls are instances of the call instruction, so these
// are all subclasses of the CallInst class.  Note that none of these classes
// has state or virtual methods, which is an important part of this gross/neat
// hack working.
//
// In some cases, arguments to intrinsics need to be generic and are defined as
// type pointer to empty struct { }*.  To access the real item of interest the
// cast instruction needs to be stripped away.
//
//===----------------------------------------------------------------------===//

#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Operator.h"
#include "llvm/ADT/StringSwitch.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/raw_ostream.h"
using namespace llvm;

//===----------------------------------------------------------------------===//
/// DbgVariableIntrinsic - This is the common base class for debug info
/// intrinsics for variables.
///

Value *DbgVariableIntrinsic::getVariableLocation(bool AllowNullOp) const {
  Value *Op = getArgOperand(0);
  if (AllowNullOp && !Op)
    return nullptr;

  auto *MD = cast<MetadataAsValue>(Op)->getMetadata();
  if (auto *V = dyn_cast<ValueAsMetadata>(MD))
    return V->getValue();

  // When the value goes to null, it gets replaced by an empty MDNode.
  assert(!cast<MDNode>(MD)->getNumOperands() && "Expected an empty MDNode");
  return nullptr;
}

Optional<uint64_t> DbgVariableIntrinsic::getFragmentSizeInBits() const {
  if (auto Fragment = getExpression()->getFragmentInfo())
    return Fragment->SizeInBits;
  return getVariable()->getSizeInBits();
}

int llvm::Intrinsic::lookupLLVMIntrinsicByName(ArrayRef<const char *> NameTable,
                                               StringRef Name) {
  assert(Name.startswith("llvm."));

  // Do successive binary searches of the dotted name components. For
  // "llvm.gc.experimental.statepoint.p1i8.p1i32", we will find the range of
  // intrinsics starting with "llvm.gc", then "llvm.gc.experimental", then
  // "llvm.gc.experimental.statepoint", and then we will stop as the range is
  // size 1. During the search, we can skip the prefix that we already know is
  // identical. By using strncmp we consider names with differing suffixes to
  // be part of the equal range.
  size_t CmpStart = 0;
  size_t CmpEnd = 4; // Skip the "llvm" component.
  const char *const *Low = NameTable.begin();
  const char *const *High = NameTable.end();
  const char *const *LastLow = Low;
  while (CmpEnd < Name.size() && High - Low > 0) {
    CmpStart = CmpEnd;
    CmpEnd = Name.find('.', CmpStart + 1);
    CmpEnd = CmpEnd == StringRef::npos ? Name.size() : CmpEnd;
    auto Cmp = [CmpStart, CmpEnd](const char *LHS, const char *RHS) {
      return strncmp(LHS + CmpStart, RHS + CmpStart, CmpEnd - CmpStart) < 0;
    };
    LastLow = Low;
    std::tie(Low, High) = std::equal_range(Low, High, Name.data(), Cmp);
  }
  if (High - Low > 0)
    LastLow = Low;

  if (LastLow == NameTable.end())
    return -1;
  StringRef NameFound = *LastLow;
  if (Name == NameFound ||
      (Name.startswith(NameFound) && Name[NameFound.size()] == '.'))
    return LastLow - NameTable.begin();
  return -1;
}

Value *InstrProfIncrementInst::getStep() const {
  if (InstrProfIncrementInstStep::classof(this)) {
    return const_cast<Value *>(getArgOperand(4));
  }
  const Module *M = getModule();
  LLVMContext &Context = M->getContext();
  return ConstantInt::get(Type::getInt64Ty(Context), 1);
}

Optional<ConstrainedFPIntrinsic::RoundingMode>
ConstrainedFPIntrinsic::getRoundingMode() const {
  unsigned NumOperands = getNumArgOperands();
  Metadata *MD =
      dyn_cast<MetadataAsValue>(getArgOperand(NumOperands - 2))->getMetadata();
  if (!MD || !isa<MDString>(MD))
    return None;
  return StrToRoundingMode(cast<MDString>(MD)->getString());
}

Optional<ConstrainedFPIntrinsic::RoundingMode>
ConstrainedFPIntrinsic::StrToRoundingMode(StringRef RoundingArg) {
  // For dynamic rounding mode, we use round to nearest but we will set the
  // 'exact' SDNodeFlag so that the value will not be rounded.
  return StringSwitch<Optional<RoundingMode>>(RoundingArg)
    .Case("round.dynamic",    rmDynamic)
    .Case("round.tonearest",  rmToNearest)
    .Case("round.downward",   rmDownward)
    .Case("round.upward",     rmUpward)
    .Case("round.towardzero", rmTowardZero)
    .Default(None);
}

Optional<StringRef>
ConstrainedFPIntrinsic::RoundingModeToStr(RoundingMode UseRounding) {
  Optional<StringRef> RoundingStr = None;
  switch (UseRounding) {
  case ConstrainedFPIntrinsic::rmDynamic:
    RoundingStr = "round.dynamic";
    break;
  case ConstrainedFPIntrinsic::rmToNearest:
    RoundingStr = "round.tonearest";
    break;
  case ConstrainedFPIntrinsic::rmDownward:
    RoundingStr = "round.downward";
    break;
  case ConstrainedFPIntrinsic::rmUpward:
    RoundingStr = "round.upward";
    break;
  case ConstrainedFPIntrinsic::rmTowardZero:
    RoundingStr = "round.tozero";
    break;
  }
  return RoundingStr;
}

Optional<ConstrainedFPIntrinsic::ExceptionBehavior>
ConstrainedFPIntrinsic::getExceptionBehavior() const {
  unsigned NumOperands = getNumArgOperands();
  Metadata *MD =
      dyn_cast<MetadataAsValue>(getArgOperand(NumOperands - 1))->getMetadata();
  if (!MD || !isa<MDString>(MD))
    return None;
  return StrToExceptionBehavior(cast<MDString>(MD)->getString());
}

Optional<ConstrainedFPIntrinsic::ExceptionBehavior>
ConstrainedFPIntrinsic::StrToExceptionBehavior(StringRef ExceptionArg) {
  return StringSwitch<Optional<ExceptionBehavior>>(ExceptionArg)
    .Case("fpexcept.ignore",  ebIgnore)
    .Case("fpexcept.maytrap", ebMayTrap)
    .Case("fpexcept.strict",  ebStrict)
    .Default(None);
}

Optional<StringRef>
ConstrainedFPIntrinsic::ExceptionBehaviorToStr(ExceptionBehavior UseExcept) {
  Optional<StringRef> ExceptStr = None;
  switch (UseExcept) {
  case ConstrainedFPIntrinsic::ebStrict:
    ExceptStr = "fpexcept.strict";
    break;
  case ConstrainedFPIntrinsic::ebIgnore:
    ExceptStr = "fpexcept.ignore";
    break;
  case ConstrainedFPIntrinsic::ebMayTrap:
    ExceptStr = "fpexcept.maytrap";
    break;
  }
  return ExceptStr;
}

bool ConstrainedFPIntrinsic::isUnaryOp() const {
  switch (getIntrinsicID()) {
    default:
      return false;
    case Intrinsic::experimental_constrained_fptrunc:
    case Intrinsic::experimental_constrained_fpext:
    case Intrinsic::experimental_constrained_sqrt:
    case Intrinsic::experimental_constrained_sin:
    case Intrinsic::experimental_constrained_cos:
    case Intrinsic::experimental_constrained_exp:
    case Intrinsic::experimental_constrained_exp2:
    case Intrinsic::experimental_constrained_log:
    case Intrinsic::experimental_constrained_log10:
    case Intrinsic::experimental_constrained_log2:
    case Intrinsic::experimental_constrained_rint:
    case Intrinsic::experimental_constrained_nearbyint:
    case Intrinsic::experimental_constrained_ceil:
    case Intrinsic::experimental_constrained_floor:
    case Intrinsic::experimental_constrained_round:
    case Intrinsic::experimental_constrained_trunc:
      return true;
  }
}

bool ConstrainedFPIntrinsic::isTernaryOp() const {
  switch (getIntrinsicID()) {
    default:
      return false;
    case Intrinsic::experimental_constrained_fma:
      return true;
  }
}

Instruction::BinaryOps BinaryOpIntrinsic::getBinaryOp() const {
  switch (getIntrinsicID()) {
    case Intrinsic::uadd_with_overflow:
    case Intrinsic::sadd_with_overflow:
    case Intrinsic::uadd_sat:
    case Intrinsic::sadd_sat:
      return Instruction::Add;
    case Intrinsic::usub_with_overflow:
    case Intrinsic::ssub_with_overflow:
    case Intrinsic::usub_sat:
    case Intrinsic::ssub_sat:
      return Instruction::Sub;
    case Intrinsic::umul_with_overflow:
    case Intrinsic::smul_with_overflow:
      return Instruction::Mul;
    default:
      llvm_unreachable("Invalid intrinsic");
  }
}

bool BinaryOpIntrinsic::isSigned() const {
  switch (getIntrinsicID()) {
    case Intrinsic::sadd_with_overflow:
    case Intrinsic::ssub_with_overflow:
    case Intrinsic::smul_with_overflow:
    case Intrinsic::sadd_sat:
    case Intrinsic::ssub_sat:
      return true;
    default:
      return false;
  }
}

unsigned BinaryOpIntrinsic::getNoWrapKind() const {
  if (isSigned())
    return OverflowingBinaryOperator::NoSignedWrap;
  else
    return OverflowingBinaryOperator::NoUnsignedWrap;
}
