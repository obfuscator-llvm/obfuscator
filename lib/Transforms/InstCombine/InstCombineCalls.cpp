//===- InstCombineCalls.cpp -----------------------------------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implements the visitCall and visitInvoke functions.
//
//===----------------------------------------------------------------------===//

#include "InstCombineInternal.h"
#include "llvm/ADT/APFloat.h"
#include "llvm/ADT/APInt.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/None.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/ADT/Twine.h"
#include "llvm/Analysis/InstructionSimplify.h"
#include "llvm/Analysis/MemoryBuiltins.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/CallSite.h"
#include "llvm/IR/Constant.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/PatternMatch.h"
#include "llvm/IR/Statepoint.h"
#include "llvm/IR/Type.h"
#include "llvm/IR/Value.h"
#include "llvm/IR/ValueHandle.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/KnownBits.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/Transforms/Utils/Local.h"
#include "llvm/Transforms/Utils/SimplifyLibCalls.h"
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <vector>

using namespace llvm;
using namespace PatternMatch;

#define DEBUG_TYPE "instcombine"

STATISTIC(NumSimplified, "Number of library calls simplified");

static cl::opt<unsigned> UnfoldElementAtomicMemcpyMaxElements(
    "unfold-element-atomic-memcpy-max-elements",
    cl::init(16),
    cl::desc("Maximum number of elements in atomic memcpy the optimizer is "
             "allowed to unfold"));

/// Return the specified type promoted as it would be to pass though a va_arg
/// area.
static Type *getPromotedType(Type *Ty) {
  if (IntegerType* ITy = dyn_cast<IntegerType>(Ty)) {
    if (ITy->getBitWidth() < 32)
      return Type::getInt32Ty(Ty->getContext());
  }
  return Ty;
}

/// Return a constant boolean vector that has true elements in all positions
/// where the input constant data vector has an element with the sign bit set.
static Constant *getNegativeIsTrueBoolVec(ConstantDataVector *V) {
  SmallVector<Constant *, 32> BoolVec;
  IntegerType *BoolTy = Type::getInt1Ty(V->getContext());
  for (unsigned I = 0, E = V->getNumElements(); I != E; ++I) {
    Constant *Elt = V->getElementAsConstant(I);
    assert((isa<ConstantInt>(Elt) || isa<ConstantFP>(Elt)) &&
           "Unexpected constant data vector element type");
    bool Sign = V->getElementType()->isIntegerTy()
                    ? cast<ConstantInt>(Elt)->isNegative()
                    : cast<ConstantFP>(Elt)->isNegative();
    BoolVec.push_back(ConstantInt::get(BoolTy, Sign));
  }
  return ConstantVector::get(BoolVec);
}

Instruction *InstCombiner::SimplifyElementUnorderedAtomicMemCpy(
    ElementUnorderedAtomicMemCpyInst *AMI) {
  // Try to unfold this intrinsic into sequence of explicit atomic loads and
  // stores.
  // First check that number of elements is compile time constant.
  auto *LengthCI = dyn_cast<ConstantInt>(AMI->getLength());
  if (!LengthCI)
    return nullptr;

  // Check that there are not too many elements.
  uint64_t LengthInBytes = LengthCI->getZExtValue();
  uint32_t ElementSizeInBytes = AMI->getElementSizeInBytes();
  uint64_t NumElements = LengthInBytes / ElementSizeInBytes;
  if (NumElements >= UnfoldElementAtomicMemcpyMaxElements)
    return nullptr;

  // Only expand if there are elements to copy.
  if (NumElements > 0) {
    // Don't unfold into illegal integers
    uint64_t ElementSizeInBits = ElementSizeInBytes * 8;
    if (!getDataLayout().isLegalInteger(ElementSizeInBits))
      return nullptr;

    // Cast source and destination to the correct type. Intrinsic input
    // arguments are usually represented as i8*. Often operands will be
    // explicitly casted to i8* and we can just strip those casts instead of
    // inserting new ones. However it's easier to rely on other InstCombine
    // rules which will cover trivial cases anyway.
    Value *Src = AMI->getRawSource();
    Value *Dst = AMI->getRawDest();
    Type *ElementPointerType =
        Type::getIntNPtrTy(AMI->getContext(), ElementSizeInBits,
                           Src->getType()->getPointerAddressSpace());

    Value *SrcCasted = Builder.CreatePointerCast(Src, ElementPointerType,
                                                 "memcpy_unfold.src_casted");
    Value *DstCasted = Builder.CreatePointerCast(Dst, ElementPointerType,
                                                 "memcpy_unfold.dst_casted");

    for (uint64_t i = 0; i < NumElements; ++i) {
      // Get current element addresses
      ConstantInt *ElementIdxCI =
          ConstantInt::get(AMI->getContext(), APInt(64, i));
      Value *SrcElementAddr =
          Builder.CreateGEP(SrcCasted, ElementIdxCI, "memcpy_unfold.src_addr");
      Value *DstElementAddr =
          Builder.CreateGEP(DstCasted, ElementIdxCI, "memcpy_unfold.dst_addr");

      // Load from the source. Transfer alignment information and mark load as
      // unordered atomic.
      LoadInst *Load = Builder.CreateLoad(SrcElementAddr, "memcpy_unfold.val");
      Load->setOrdering(AtomicOrdering::Unordered);
      // We know alignment of the first element. It is also guaranteed by the
      // verifier that element size is less or equal than first element
      // alignment and both of this values are powers of two. This means that
      // all subsequent accesses are at least element size aligned.
      // TODO: We can infer better alignment but there is no evidence that this
      // will matter.
      Load->setAlignment(i == 0 ? AMI->getParamAlignment(1)
                                : ElementSizeInBytes);
      Load->setDebugLoc(AMI->getDebugLoc());

      // Store loaded value via unordered atomic store.
      StoreInst *Store = Builder.CreateStore(Load, DstElementAddr);
      Store->setOrdering(AtomicOrdering::Unordered);
      Store->setAlignment(i == 0 ? AMI->getParamAlignment(0)
                                 : ElementSizeInBytes);
      Store->setDebugLoc(AMI->getDebugLoc());
    }
  }

  // Set the number of elements of the copy to 0, it will be deleted on the
  // next iteration.
  AMI->setLength(Constant::getNullValue(LengthCI->getType()));
  return AMI;
}

Instruction *InstCombiner::SimplifyMemTransfer(MemIntrinsic *MI) {
  unsigned DstAlign = getKnownAlignment(MI->getArgOperand(0), DL, MI, &AC, &DT);
  unsigned SrcAlign = getKnownAlignment(MI->getArgOperand(1), DL, MI, &AC, &DT);
  unsigned MinAlign = std::min(DstAlign, SrcAlign);
  unsigned CopyAlign = MI->getAlignment();

  if (CopyAlign < MinAlign) {
    MI->setAlignment(ConstantInt::get(MI->getAlignmentType(), MinAlign, false));
    return MI;
  }

  // If MemCpyInst length is 1/2/4/8 bytes then replace memcpy with
  // load/store.
  ConstantInt *MemOpLength = dyn_cast<ConstantInt>(MI->getArgOperand(2));
  if (!MemOpLength) return nullptr;

  // Source and destination pointer types are always "i8*" for intrinsic.  See
  // if the size is something we can handle with a single primitive load/store.
  // A single load+store correctly handles overlapping memory in the memmove
  // case.
  uint64_t Size = MemOpLength->getLimitedValue();
  assert(Size && "0-sized memory transferring should be removed already.");

  if (Size > 8 || (Size&(Size-1)))
    return nullptr;  // If not 1/2/4/8 bytes, exit.

  // Use an integer load+store unless we can find something better.
  unsigned SrcAddrSp =
    cast<PointerType>(MI->getArgOperand(1)->getType())->getAddressSpace();
  unsigned DstAddrSp =
    cast<PointerType>(MI->getArgOperand(0)->getType())->getAddressSpace();

  IntegerType* IntType = IntegerType::get(MI->getContext(), Size<<3);
  Type *NewSrcPtrTy = PointerType::get(IntType, SrcAddrSp);
  Type *NewDstPtrTy = PointerType::get(IntType, DstAddrSp);

  // If the memcpy has metadata describing the members, see if we can get the
  // TBAA tag describing our copy.
  MDNode *CopyMD = nullptr;
  if (MDNode *M = MI->getMetadata(LLVMContext::MD_tbaa_struct)) {
    if (M->getNumOperands() == 3 && M->getOperand(0) &&
        mdconst::hasa<ConstantInt>(M->getOperand(0)) &&
        mdconst::extract<ConstantInt>(M->getOperand(0))->isZero() &&
        M->getOperand(1) &&
        mdconst::hasa<ConstantInt>(M->getOperand(1)) &&
        mdconst::extract<ConstantInt>(M->getOperand(1))->getValue() ==
        Size &&
        M->getOperand(2) && isa<MDNode>(M->getOperand(2)))
      CopyMD = cast<MDNode>(M->getOperand(2));
  }

  // If the memcpy/memmove provides better alignment info than we can
  // infer, use it.
  SrcAlign = std::max(SrcAlign, CopyAlign);
  DstAlign = std::max(DstAlign, CopyAlign);

  Value *Src = Builder.CreateBitCast(MI->getArgOperand(1), NewSrcPtrTy);
  Value *Dest = Builder.CreateBitCast(MI->getArgOperand(0), NewDstPtrTy);
  LoadInst *L = Builder.CreateLoad(Src, MI->isVolatile());
  L->setAlignment(SrcAlign);
  if (CopyMD)
    L->setMetadata(LLVMContext::MD_tbaa, CopyMD);
  MDNode *LoopMemParallelMD =
    MI->getMetadata(LLVMContext::MD_mem_parallel_loop_access);
  if (LoopMemParallelMD)
    L->setMetadata(LLVMContext::MD_mem_parallel_loop_access, LoopMemParallelMD);

  StoreInst *S = Builder.CreateStore(L, Dest, MI->isVolatile());
  S->setAlignment(DstAlign);
  if (CopyMD)
    S->setMetadata(LLVMContext::MD_tbaa, CopyMD);
  if (LoopMemParallelMD)
    S->setMetadata(LLVMContext::MD_mem_parallel_loop_access, LoopMemParallelMD);

  // Set the size of the copy to 0, it will be deleted on the next iteration.
  MI->setArgOperand(2, Constant::getNullValue(MemOpLength->getType()));
  return MI;
}

Instruction *InstCombiner::SimplifyMemSet(MemSetInst *MI) {
  unsigned Alignment = getKnownAlignment(MI->getDest(), DL, MI, &AC, &DT);
  if (MI->getAlignment() < Alignment) {
    MI->setAlignment(ConstantInt::get(MI->getAlignmentType(),
                                             Alignment, false));
    return MI;
  }

  // Extract the length and alignment and fill if they are constant.
  ConstantInt *LenC = dyn_cast<ConstantInt>(MI->getLength());
  ConstantInt *FillC = dyn_cast<ConstantInt>(MI->getValue());
  if (!LenC || !FillC || !FillC->getType()->isIntegerTy(8))
    return nullptr;
  uint64_t Len = LenC->getLimitedValue();
  Alignment = MI->getAlignment();
  assert(Len && "0-sized memory setting should be removed already.");

  // memset(s,c,n) -> store s, c (for n=1,2,4,8)
  if (Len <= 8 && isPowerOf2_32((uint32_t)Len)) {
    Type *ITy = IntegerType::get(MI->getContext(), Len*8);  // n=1 -> i8.

    Value *Dest = MI->getDest();
    unsigned DstAddrSp = cast<PointerType>(Dest->getType())->getAddressSpace();
    Type *NewDstPtrTy = PointerType::get(ITy, DstAddrSp);
    Dest = Builder.CreateBitCast(Dest, NewDstPtrTy);

    // Alignment 0 is identity for alignment 1 for memset, but not store.
    if (Alignment == 0) Alignment = 1;

    // Extract the fill value and store.
    uint64_t Fill = FillC->getZExtValue()*0x0101010101010101ULL;
    StoreInst *S = Builder.CreateStore(ConstantInt::get(ITy, Fill), Dest,
                                       MI->isVolatile());
    S->setAlignment(Alignment);

    // Set the size of the copy to 0, it will be deleted on the next iteration.
    MI->setLength(Constant::getNullValue(LenC->getType()));
    return MI;
  }

  return nullptr;
}

static Value *simplifyX86immShift(const IntrinsicInst &II,
                                  InstCombiner::BuilderTy &Builder) {
  bool LogicalShift = false;
  bool ShiftLeft = false;

  switch (II.getIntrinsicID()) {
  default: llvm_unreachable("Unexpected intrinsic!");
  case Intrinsic::x86_sse2_psra_d:
  case Intrinsic::x86_sse2_psra_w:
  case Intrinsic::x86_sse2_psrai_d:
  case Intrinsic::x86_sse2_psrai_w:
  case Intrinsic::x86_avx2_psra_d:
  case Intrinsic::x86_avx2_psra_w:
  case Intrinsic::x86_avx2_psrai_d:
  case Intrinsic::x86_avx2_psrai_w:
  case Intrinsic::x86_avx512_psra_q_128:
  case Intrinsic::x86_avx512_psrai_q_128:
  case Intrinsic::x86_avx512_psra_q_256:
  case Intrinsic::x86_avx512_psrai_q_256:
  case Intrinsic::x86_avx512_psra_d_512:
  case Intrinsic::x86_avx512_psra_q_512:
  case Intrinsic::x86_avx512_psra_w_512:
  case Intrinsic::x86_avx512_psrai_d_512:
  case Intrinsic::x86_avx512_psrai_q_512:
  case Intrinsic::x86_avx512_psrai_w_512:
    LogicalShift = false; ShiftLeft = false;
    break;
  case Intrinsic::x86_sse2_psrl_d:
  case Intrinsic::x86_sse2_psrl_q:
  case Intrinsic::x86_sse2_psrl_w:
  case Intrinsic::x86_sse2_psrli_d:
  case Intrinsic::x86_sse2_psrli_q:
  case Intrinsic::x86_sse2_psrli_w:
  case Intrinsic::x86_avx2_psrl_d:
  case Intrinsic::x86_avx2_psrl_q:
  case Intrinsic::x86_avx2_psrl_w:
  case Intrinsic::x86_avx2_psrli_d:
  case Intrinsic::x86_avx2_psrli_q:
  case Intrinsic::x86_avx2_psrli_w:
  case Intrinsic::x86_avx512_psrl_d_512:
  case Intrinsic::x86_avx512_psrl_q_512:
  case Intrinsic::x86_avx512_psrl_w_512:
  case Intrinsic::x86_avx512_psrli_d_512:
  case Intrinsic::x86_avx512_psrli_q_512:
  case Intrinsic::x86_avx512_psrli_w_512:
    LogicalShift = true; ShiftLeft = false;
    break;
  case Intrinsic::x86_sse2_psll_d:
  case Intrinsic::x86_sse2_psll_q:
  case Intrinsic::x86_sse2_psll_w:
  case Intrinsic::x86_sse2_pslli_d:
  case Intrinsic::x86_sse2_pslli_q:
  case Intrinsic::x86_sse2_pslli_w:
  case Intrinsic::x86_avx2_psll_d:
  case Intrinsic::x86_avx2_psll_q:
  case Intrinsic::x86_avx2_psll_w:
  case Intrinsic::x86_avx2_pslli_d:
  case Intrinsic::x86_avx2_pslli_q:
  case Intrinsic::x86_avx2_pslli_w:
  case Intrinsic::x86_avx512_psll_d_512:
  case Intrinsic::x86_avx512_psll_q_512:
  case Intrinsic::x86_avx512_psll_w_512:
  case Intrinsic::x86_avx512_pslli_d_512:
  case Intrinsic::x86_avx512_pslli_q_512:
  case Intrinsic::x86_avx512_pslli_w_512:
    LogicalShift = true; ShiftLeft = true;
    break;
  }
  assert((LogicalShift || !ShiftLeft) && "Only logical shifts can shift left");

  // Simplify if count is constant.
  auto Arg1 = II.getArgOperand(1);
  auto CAZ = dyn_cast<ConstantAggregateZero>(Arg1);
  auto CDV = dyn_cast<ConstantDataVector>(Arg1);
  auto CInt = dyn_cast<ConstantInt>(Arg1);
  if (!CAZ && !CDV && !CInt)
    return nullptr;

  APInt Count(64, 0);
  if (CDV) {
    // SSE2/AVX2 uses all the first 64-bits of the 128-bit vector
    // operand to compute the shift amount.
    auto VT = cast<VectorType>(CDV->getType());
    unsigned BitWidth = VT->getElementType()->getPrimitiveSizeInBits();
    assert((64 % BitWidth) == 0 && "Unexpected packed shift size");
    unsigned NumSubElts = 64 / BitWidth;

    // Concatenate the sub-elements to create the 64-bit value.
    for (unsigned i = 0; i != NumSubElts; ++i) {
      unsigned SubEltIdx = (NumSubElts - 1) - i;
      auto SubElt = cast<ConstantInt>(CDV->getElementAsConstant(SubEltIdx));
      Count <<= BitWidth;
      Count |= SubElt->getValue().zextOrTrunc(64);
    }
  }
  else if (CInt)
    Count = CInt->getValue();

  auto Vec = II.getArgOperand(0);
  auto VT = cast<VectorType>(Vec->getType());
  auto SVT = VT->getElementType();
  unsigned VWidth = VT->getNumElements();
  unsigned BitWidth = SVT->getPrimitiveSizeInBits();

  // If shift-by-zero then just return the original value.
  if (Count.isNullValue())
    return Vec;

  // Handle cases when Shift >= BitWidth.
  if (Count.uge(BitWidth)) {
    // If LogicalShift - just return zero.
    if (LogicalShift)
      return ConstantAggregateZero::get(VT);

    // If ArithmeticShift - clamp Shift to (BitWidth - 1).
    Count = APInt(64, BitWidth - 1);
  }

  // Get a constant vector of the same type as the first operand.
  auto ShiftAmt = ConstantInt::get(SVT, Count.zextOrTrunc(BitWidth));
  auto ShiftVec = Builder.CreateVectorSplat(VWidth, ShiftAmt);

  if (ShiftLeft)
    return Builder.CreateShl(Vec, ShiftVec);

  if (LogicalShift)
    return Builder.CreateLShr(Vec, ShiftVec);

  return Builder.CreateAShr(Vec, ShiftVec);
}

// Attempt to simplify AVX2 per-element shift intrinsics to a generic IR shift.
// Unlike the generic IR shifts, the intrinsics have defined behaviour for out
// of range shift amounts (logical - set to zero, arithmetic - splat sign bit).
static Value *simplifyX86varShift(const IntrinsicInst &II,
                                  InstCombiner::BuilderTy &Builder) {
  bool LogicalShift = false;
  bool ShiftLeft = false;

  switch (II.getIntrinsicID()) {
  default: llvm_unreachable("Unexpected intrinsic!");
  case Intrinsic::x86_avx2_psrav_d:
  case Intrinsic::x86_avx2_psrav_d_256:
  case Intrinsic::x86_avx512_psrav_q_128:
  case Intrinsic::x86_avx512_psrav_q_256:
  case Intrinsic::x86_avx512_psrav_d_512:
  case Intrinsic::x86_avx512_psrav_q_512:
  case Intrinsic::x86_avx512_psrav_w_128:
  case Intrinsic::x86_avx512_psrav_w_256:
  case Intrinsic::x86_avx512_psrav_w_512:
    LogicalShift = false;
    ShiftLeft = false;
    break;
  case Intrinsic::x86_avx2_psrlv_d:
  case Intrinsic::x86_avx2_psrlv_d_256:
  case Intrinsic::x86_avx2_psrlv_q:
  case Intrinsic::x86_avx2_psrlv_q_256:
  case Intrinsic::x86_avx512_psrlv_d_512:
  case Intrinsic::x86_avx512_psrlv_q_512:
  case Intrinsic::x86_avx512_psrlv_w_128:
  case Intrinsic::x86_avx512_psrlv_w_256:
  case Intrinsic::x86_avx512_psrlv_w_512:
    LogicalShift = true;
    ShiftLeft = false;
    break;
  case Intrinsic::x86_avx2_psllv_d:
  case Intrinsic::x86_avx2_psllv_d_256:
  case Intrinsic::x86_avx2_psllv_q:
  case Intrinsic::x86_avx2_psllv_q_256:
  case Intrinsic::x86_avx512_psllv_d_512:
  case Intrinsic::x86_avx512_psllv_q_512:
  case Intrinsic::x86_avx512_psllv_w_128:
  case Intrinsic::x86_avx512_psllv_w_256:
  case Intrinsic::x86_avx512_psllv_w_512:
    LogicalShift = true;
    ShiftLeft = true;
    break;
  }
  assert((LogicalShift || !ShiftLeft) && "Only logical shifts can shift left");

  // Simplify if all shift amounts are constant/undef.
  auto *CShift = dyn_cast<Constant>(II.getArgOperand(1));
  if (!CShift)
    return nullptr;

  auto Vec = II.getArgOperand(0);
  auto VT = cast<VectorType>(II.getType());
  auto SVT = VT->getVectorElementType();
  int NumElts = VT->getNumElements();
  int BitWidth = SVT->getIntegerBitWidth();

  // Collect each element's shift amount.
  // We also collect special cases: UNDEF = -1, OUT-OF-RANGE = BitWidth.
  bool AnyOutOfRange = false;
  SmallVector<int, 8> ShiftAmts;
  for (int I = 0; I < NumElts; ++I) {
    auto *CElt = CShift->getAggregateElement(I);
    if (CElt && isa<UndefValue>(CElt)) {
      ShiftAmts.push_back(-1);
      continue;
    }

    auto *COp = dyn_cast_or_null<ConstantInt>(CElt);
    if (!COp)
      return nullptr;

    // Handle out of range shifts.
    // If LogicalShift - set to BitWidth (special case).
    // If ArithmeticShift - set to (BitWidth - 1) (sign splat).
    APInt ShiftVal = COp->getValue();
    if (ShiftVal.uge(BitWidth)) {
      AnyOutOfRange = LogicalShift;
      ShiftAmts.push_back(LogicalShift ? BitWidth : BitWidth - 1);
      continue;
    }

    ShiftAmts.push_back((int)ShiftVal.getZExtValue());
  }

  // If all elements out of range or UNDEF, return vector of zeros/undefs.
  // ArithmeticShift should only hit this if they are all UNDEF.
  auto OutOfRange = [&](int Idx) { return (Idx < 0) || (BitWidth <= Idx); };
  if (all_of(ShiftAmts, OutOfRange)) {
    SmallVector<Constant *, 8> ConstantVec;
    for (int Idx : ShiftAmts) {
      if (Idx < 0) {
        ConstantVec.push_back(UndefValue::get(SVT));
      } else {
        assert(LogicalShift && "Logical shift expected");
        ConstantVec.push_back(ConstantInt::getNullValue(SVT));
      }
    }
    return ConstantVector::get(ConstantVec);
  }

  // We can't handle only some out of range values with generic logical shifts.
  if (AnyOutOfRange)
    return nullptr;

  // Build the shift amount constant vector.
  SmallVector<Constant *, 8> ShiftVecAmts;
  for (int Idx : ShiftAmts) {
    if (Idx < 0)
      ShiftVecAmts.push_back(UndefValue::get(SVT));
    else
      ShiftVecAmts.push_back(ConstantInt::get(SVT, Idx));
  }
  auto ShiftVec = ConstantVector::get(ShiftVecAmts);

  if (ShiftLeft)
    return Builder.CreateShl(Vec, ShiftVec);

  if (LogicalShift)
    return Builder.CreateLShr(Vec, ShiftVec);

  return Builder.CreateAShr(Vec, ShiftVec);
}

static Value *simplifyX86muldq(const IntrinsicInst &II,
                               InstCombiner::BuilderTy &Builder) {
  Value *Arg0 = II.getArgOperand(0);
  Value *Arg1 = II.getArgOperand(1);
  Type *ResTy = II.getType();
  assert(Arg0->getType()->getScalarSizeInBits() == 32 &&
         Arg1->getType()->getScalarSizeInBits() == 32 &&
         ResTy->getScalarSizeInBits() == 64 && "Unexpected muldq/muludq types");

  // muldq/muludq(undef, undef) -> zero (matches generic mul behavior)
  if (isa<UndefValue>(Arg0) || isa<UndefValue>(Arg1))
    return ConstantAggregateZero::get(ResTy);

  // Constant folding.
  // PMULDQ  = (mul(vXi64 sext(shuffle<0,2,..>(Arg0)),
  //                vXi64 sext(shuffle<0,2,..>(Arg1))))
  // PMULUDQ = (mul(vXi64 zext(shuffle<0,2,..>(Arg0)),
  //                vXi64 zext(shuffle<0,2,..>(Arg1))))
  if (!isa<Constant>(Arg0) || !isa<Constant>(Arg1))
    return nullptr;

  unsigned NumElts = ResTy->getVectorNumElements();
  assert(Arg0->getType()->getVectorNumElements() == (2 * NumElts) &&
         Arg1->getType()->getVectorNumElements() == (2 * NumElts) &&
         "Unexpected muldq/muludq types");

  unsigned IntrinsicID = II.getIntrinsicID();
  bool IsSigned = (Intrinsic::x86_sse41_pmuldq == IntrinsicID ||
                   Intrinsic::x86_avx2_pmul_dq == IntrinsicID ||
                   Intrinsic::x86_avx512_pmul_dq_512 == IntrinsicID);

  SmallVector<unsigned, 16> ShuffleMask;
  for (unsigned i = 0; i != NumElts; ++i)
    ShuffleMask.push_back(i * 2);

  auto *LHS = Builder.CreateShuffleVector(Arg0, Arg0, ShuffleMask);
  auto *RHS = Builder.CreateShuffleVector(Arg1, Arg1, ShuffleMask);

  if (IsSigned) {
    LHS = Builder.CreateSExt(LHS, ResTy);
    RHS = Builder.CreateSExt(RHS, ResTy);
  } else {
    LHS = Builder.CreateZExt(LHS, ResTy);
    RHS = Builder.CreateZExt(RHS, ResTy);
  }

  return Builder.CreateMul(LHS, RHS);
}

static Value *simplifyX86pack(IntrinsicInst &II, bool IsSigned) {
  Value *Arg0 = II.getArgOperand(0);
  Value *Arg1 = II.getArgOperand(1);
  Type *ResTy = II.getType();

  // Fast all undef handling.
  if (isa<UndefValue>(Arg0) && isa<UndefValue>(Arg1))
    return UndefValue::get(ResTy);

  Type *ArgTy = Arg0->getType();
  unsigned NumLanes = ResTy->getPrimitiveSizeInBits() / 128;
  unsigned NumDstElts = ResTy->getVectorNumElements();
  unsigned NumSrcElts = ArgTy->getVectorNumElements();
  assert(NumDstElts == (2 * NumSrcElts) && "Unexpected packing types");

  unsigned NumDstEltsPerLane = NumDstElts / NumLanes;
  unsigned NumSrcEltsPerLane = NumSrcElts / NumLanes;
  unsigned DstScalarSizeInBits = ResTy->getScalarSizeInBits();
  assert(ArgTy->getScalarSizeInBits() == (2 * DstScalarSizeInBits) &&
         "Unexpected packing types");

  // Constant folding.
  auto *Cst0 = dyn_cast<Constant>(Arg0);
  auto *Cst1 = dyn_cast<Constant>(Arg1);
  if (!Cst0 || !Cst1)
    return nullptr;

  SmallVector<Constant *, 32> Vals;
  for (unsigned Lane = 0; Lane != NumLanes; ++Lane) {
    for (unsigned Elt = 0; Elt != NumDstEltsPerLane; ++Elt) {
      unsigned SrcIdx = Lane * NumSrcEltsPerLane + Elt % NumSrcEltsPerLane;
      auto *Cst = (Elt >= NumSrcEltsPerLane) ? Cst1 : Cst0;
      auto *COp = Cst->getAggregateElement(SrcIdx);
      if (COp && isa<UndefValue>(COp)) {
        Vals.push_back(UndefValue::get(ResTy->getScalarType()));
        continue;
      }

      auto *CInt = dyn_cast_or_null<ConstantInt>(COp);
      if (!CInt)
        return nullptr;

      APInt Val = CInt->getValue();
      assert(Val.getBitWidth() == ArgTy->getScalarSizeInBits() &&
             "Unexpected constant bitwidth");

      if (IsSigned) {
        // PACKSS: Truncate signed value with signed saturation.
        // Source values less than dst minint are saturated to minint.
        // Source values greater than dst maxint are saturated to maxint.
        if (Val.isSignedIntN(DstScalarSizeInBits))
          Val = Val.trunc(DstScalarSizeInBits);
        else if (Val.isNegative())
          Val = APInt::getSignedMinValue(DstScalarSizeInBits);
        else
          Val = APInt::getSignedMaxValue(DstScalarSizeInBits);
      } else {
        // PACKUS: Truncate signed value with unsigned saturation.
        // Source values less than zero are saturated to zero.
        // Source values greater than dst maxuint are saturated to maxuint.
        if (Val.isIntN(DstScalarSizeInBits))
          Val = Val.trunc(DstScalarSizeInBits);
        else if (Val.isNegative())
          Val = APInt::getNullValue(DstScalarSizeInBits);
        else
          Val = APInt::getAllOnesValue(DstScalarSizeInBits);
      }

      Vals.push_back(ConstantInt::get(ResTy->getScalarType(), Val));
    }
  }

  return ConstantVector::get(Vals);
}

static Value *simplifyX86movmsk(const IntrinsicInst &II) {
  Value *Arg = II.getArgOperand(0);
  Type *ResTy = II.getType();
  Type *ArgTy = Arg->getType();

  // movmsk(undef) -> zero as we must ensure the upper bits are zero.
  if (isa<UndefValue>(Arg))
    return Constant::getNullValue(ResTy);

  // We can't easily peek through x86_mmx types.
  if (!ArgTy->isVectorTy())
    return nullptr;

  auto *C = dyn_cast<Constant>(Arg);
  if (!C)
    return nullptr;

  // Extract signbits of the vector input and pack into integer result.
  APInt Result(ResTy->getPrimitiveSizeInBits(), 0);
  for (unsigned I = 0, E = ArgTy->getVectorNumElements(); I != E; ++I) {
    auto *COp = C->getAggregateElement(I);
    if (!COp)
      return nullptr;
    if (isa<UndefValue>(COp))
      continue;

    auto *CInt = dyn_cast<ConstantInt>(COp);
    auto *CFp = dyn_cast<ConstantFP>(COp);
    if (!CInt && !CFp)
      return nullptr;

    if ((CInt && CInt->isNegative()) || (CFp && CFp->isNegative()))
      Result.setBit(I);
  }

  return Constant::getIntegerValue(ResTy, Result);
}

static Value *simplifyX86insertps(const IntrinsicInst &II,
                                  InstCombiner::BuilderTy &Builder) {
  auto *CInt = dyn_cast<ConstantInt>(II.getArgOperand(2));
  if (!CInt)
    return nullptr;

  VectorType *VecTy = cast<VectorType>(II.getType());
  assert(VecTy->getNumElements() == 4 && "insertps with wrong vector type");

  // The immediate permute control byte looks like this:
  //    [3:0] - zero mask for each 32-bit lane
  //    [5:4] - select one 32-bit destination lane
  //    [7:6] - select one 32-bit source lane

  uint8_t Imm = CInt->getZExtValue();
  uint8_t ZMask = Imm & 0xf;
  uint8_t DestLane = (Imm >> 4) & 0x3;
  uint8_t SourceLane = (Imm >> 6) & 0x3;

  ConstantAggregateZero *ZeroVector = ConstantAggregateZero::get(VecTy);

  // If all zero mask bits are set, this was just a weird way to
  // generate a zero vector.
  if (ZMask == 0xf)
    return ZeroVector;

  // Initialize by passing all of the first source bits through.
  uint32_t ShuffleMask[4] = { 0, 1, 2, 3 };

  // We may replace the second operand with the zero vector.
  Value *V1 = II.getArgOperand(1);

  if (ZMask) {
    // If the zero mask is being used with a single input or the zero mask
    // overrides the destination lane, this is a shuffle with the zero vector.
    if ((II.getArgOperand(0) == II.getArgOperand(1)) ||
        (ZMask & (1 << DestLane))) {
      V1 = ZeroVector;
      // We may still move 32-bits of the first source vector from one lane
      // to another.
      ShuffleMask[DestLane] = SourceLane;
      // The zero mask may override the previous insert operation.
      for (unsigned i = 0; i < 4; ++i)
        if ((ZMask >> i) & 0x1)
          ShuffleMask[i] = i + 4;
    } else {
      // TODO: Model this case as 2 shuffles or a 'logical and' plus shuffle?
      return nullptr;
    }
  } else {
    // Replace the selected destination lane with the selected source lane.
    ShuffleMask[DestLane] = SourceLane + 4;
  }

  return Builder.CreateShuffleVector(II.getArgOperand(0), V1, ShuffleMask);
}

/// Attempt to simplify SSE4A EXTRQ/EXTRQI instructions using constant folding
/// or conversion to a shuffle vector.
static Value *simplifyX86extrq(IntrinsicInst &II, Value *Op0,
                               ConstantInt *CILength, ConstantInt *CIIndex,
                               InstCombiner::BuilderTy &Builder) {
  auto LowConstantHighUndef = [&](uint64_t Val) {
    Type *IntTy64 = Type::getInt64Ty(II.getContext());
    Constant *Args[] = {ConstantInt::get(IntTy64, Val),
                        UndefValue::get(IntTy64)};
    return ConstantVector::get(Args);
  };

  // See if we're dealing with constant values.
  Constant *C0 = dyn_cast<Constant>(Op0);
  ConstantInt *CI0 =
      C0 ? dyn_cast_or_null<ConstantInt>(C0->getAggregateElement((unsigned)0))
         : nullptr;

  // Attempt to constant fold.
  if (CILength && CIIndex) {
    // From AMD documentation: "The bit index and field length are each six
    // bits in length other bits of the field are ignored."
    APInt APIndex = CIIndex->getValue().zextOrTrunc(6);
    APInt APLength = CILength->getValue().zextOrTrunc(6);

    unsigned Index = APIndex.getZExtValue();

    // From AMD documentation: "a value of zero in the field length is
    // defined as length of 64".
    unsigned Length = APLength == 0 ? 64 : APLength.getZExtValue();

    // From AMD documentation: "If the sum of the bit index + length field
    // is greater than 64, the results are undefined".
    unsigned End = Index + Length;

    // Note that both field index and field length are 8-bit quantities.
    // Since variables 'Index' and 'Length' are unsigned values
    // obtained from zero-extending field index and field length
    // respectively, their sum should never wrap around.
    if (End > 64)
      return UndefValue::get(II.getType());

    // If we are inserting whole bytes, we can convert this to a shuffle.
    // Lowering can recognize EXTRQI shuffle masks.
    if ((Length % 8) == 0 && (Index % 8) == 0) {
      // Convert bit indices to byte indices.
      Length /= 8;
      Index /= 8;

      Type *IntTy8 = Type::getInt8Ty(II.getContext());
      Type *IntTy32 = Type::getInt32Ty(II.getContext());
      VectorType *ShufTy = VectorType::get(IntTy8, 16);

      SmallVector<Constant *, 16> ShuffleMask;
      for (int i = 0; i != (int)Length; ++i)
        ShuffleMask.push_back(
            Constant::getIntegerValue(IntTy32, APInt(32, i + Index)));
      for (int i = Length; i != 8; ++i)
        ShuffleMask.push_back(
            Constant::getIntegerValue(IntTy32, APInt(32, i + 16)));
      for (int i = 8; i != 16; ++i)
        ShuffleMask.push_back(UndefValue::get(IntTy32));

      Value *SV = Builder.CreateShuffleVector(
          Builder.CreateBitCast(Op0, ShufTy),
          ConstantAggregateZero::get(ShufTy), ConstantVector::get(ShuffleMask));
      return Builder.CreateBitCast(SV, II.getType());
    }

    // Constant Fold - shift Index'th bit to lowest position and mask off
    // Length bits.
    if (CI0) {
      APInt Elt = CI0->getValue();
      Elt.lshrInPlace(Index);
      Elt = Elt.zextOrTrunc(Length);
      return LowConstantHighUndef(Elt.getZExtValue());
    }

    // If we were an EXTRQ call, we'll save registers if we convert to EXTRQI.
    if (II.getIntrinsicID() == Intrinsic::x86_sse4a_extrq) {
      Value *Args[] = {Op0, CILength, CIIndex};
      Module *M = II.getModule();
      Value *F = Intrinsic::getDeclaration(M, Intrinsic::x86_sse4a_extrqi);
      return Builder.CreateCall(F, Args);
    }
  }

  // Constant Fold - extraction from zero is always {zero, undef}.
  if (CI0 && CI0->isZero())
    return LowConstantHighUndef(0);

  return nullptr;
}

/// Attempt to simplify SSE4A INSERTQ/INSERTQI instructions using constant
/// folding or conversion to a shuffle vector.
static Value *simplifyX86insertq(IntrinsicInst &II, Value *Op0, Value *Op1,
                                 APInt APLength, APInt APIndex,
                                 InstCombiner::BuilderTy &Builder) {
  // From AMD documentation: "The bit index and field length are each six bits
  // in length other bits of the field are ignored."
  APIndex = APIndex.zextOrTrunc(6);
  APLength = APLength.zextOrTrunc(6);

  // Attempt to constant fold.
  unsigned Index = APIndex.getZExtValue();

  // From AMD documentation: "a value of zero in the field length is
  // defined as length of 64".
  unsigned Length = APLength == 0 ? 64 : APLength.getZExtValue();

  // From AMD documentation: "If the sum of the bit index + length field
  // is greater than 64, the results are undefined".
  unsigned End = Index + Length;

  // Note that both field index and field length are 8-bit quantities.
  // Since variables 'Index' and 'Length' are unsigned values
  // obtained from zero-extending field index and field length
  // respectively, their sum should never wrap around.
  if (End > 64)
    return UndefValue::get(II.getType());

  // If we are inserting whole bytes, we can convert this to a shuffle.
  // Lowering can recognize INSERTQI shuffle masks.
  if ((Length % 8) == 0 && (Index % 8) == 0) {
    // Convert bit indices to byte indices.
    Length /= 8;
    Index /= 8;

    Type *IntTy8 = Type::getInt8Ty(II.getContext());
    Type *IntTy32 = Type::getInt32Ty(II.getContext());
    VectorType *ShufTy = VectorType::get(IntTy8, 16);

    SmallVector<Constant *, 16> ShuffleMask;
    for (int i = 0; i != (int)Index; ++i)
      ShuffleMask.push_back(Constant::getIntegerValue(IntTy32, APInt(32, i)));
    for (int i = 0; i != (int)Length; ++i)
      ShuffleMask.push_back(
          Constant::getIntegerValue(IntTy32, APInt(32, i + 16)));
    for (int i = Index + Length; i != 8; ++i)
      ShuffleMask.push_back(Constant::getIntegerValue(IntTy32, APInt(32, i)));
    for (int i = 8; i != 16; ++i)
      ShuffleMask.push_back(UndefValue::get(IntTy32));

    Value *SV = Builder.CreateShuffleVector(Builder.CreateBitCast(Op0, ShufTy),
                                            Builder.CreateBitCast(Op1, ShufTy),
                                            ConstantVector::get(ShuffleMask));
    return Builder.CreateBitCast(SV, II.getType());
  }

  // See if we're dealing with constant values.
  Constant *C0 = dyn_cast<Constant>(Op0);
  Constant *C1 = dyn_cast<Constant>(Op1);
  ConstantInt *CI00 =
      C0 ? dyn_cast_or_null<ConstantInt>(C0->getAggregateElement((unsigned)0))
         : nullptr;
  ConstantInt *CI10 =
      C1 ? dyn_cast_or_null<ConstantInt>(C1->getAggregateElement((unsigned)0))
         : nullptr;

  // Constant Fold - insert bottom Length bits starting at the Index'th bit.
  if (CI00 && CI10) {
    APInt V00 = CI00->getValue();
    APInt V10 = CI10->getValue();
    APInt Mask = APInt::getLowBitsSet(64, Length).shl(Index);
    V00 = V00 & ~Mask;
    V10 = V10.zextOrTrunc(Length).zextOrTrunc(64).shl(Index);
    APInt Val = V00 | V10;
    Type *IntTy64 = Type::getInt64Ty(II.getContext());
    Constant *Args[] = {ConstantInt::get(IntTy64, Val.getZExtValue()),
                        UndefValue::get(IntTy64)};
    return ConstantVector::get(Args);
  }

  // If we were an INSERTQ call, we'll save demanded elements if we convert to
  // INSERTQI.
  if (II.getIntrinsicID() == Intrinsic::x86_sse4a_insertq) {
    Type *IntTy8 = Type::getInt8Ty(II.getContext());
    Constant *CILength = ConstantInt::get(IntTy8, Length, false);
    Constant *CIIndex = ConstantInt::get(IntTy8, Index, false);

    Value *Args[] = {Op0, Op1, CILength, CIIndex};
    Module *M = II.getModule();
    Value *F = Intrinsic::getDeclaration(M, Intrinsic::x86_sse4a_insertqi);
    return Builder.CreateCall(F, Args);
  }

  return nullptr;
}

/// Attempt to convert pshufb* to shufflevector if the mask is constant.
static Value *simplifyX86pshufb(const IntrinsicInst &II,
                                InstCombiner::BuilderTy &Builder) {
  Constant *V = dyn_cast<Constant>(II.getArgOperand(1));
  if (!V)
    return nullptr;

  auto *VecTy = cast<VectorType>(II.getType());
  auto *MaskEltTy = Type::getInt32Ty(II.getContext());
  unsigned NumElts = VecTy->getNumElements();
  assert((NumElts == 16 || NumElts == 32 || NumElts == 64) &&
         "Unexpected number of elements in shuffle mask!");

  // Construct a shuffle mask from constant integers or UNDEFs.
  Constant *Indexes[64] = {nullptr};

  // Each byte in the shuffle control mask forms an index to permute the
  // corresponding byte in the destination operand.
  for (unsigned I = 0; I < NumElts; ++I) {
    Constant *COp = V->getAggregateElement(I);
    if (!COp || (!isa<UndefValue>(COp) && !isa<ConstantInt>(COp)))
      return nullptr;

    if (isa<UndefValue>(COp)) {
      Indexes[I] = UndefValue::get(MaskEltTy);
      continue;
    }

    int8_t Index = cast<ConstantInt>(COp)->getValue().getZExtValue();

    // If the most significant bit (bit[7]) of each byte of the shuffle
    // control mask is set, then zero is written in the result byte.
    // The zero vector is in the right-hand side of the resulting
    // shufflevector.

    // The value of each index for the high 128-bit lane is the least
    // significant 4 bits of the respective shuffle control byte.
    Index = ((Index < 0) ? NumElts : Index & 0x0F) + (I & 0xF0);
    Indexes[I] = ConstantInt::get(MaskEltTy, Index);
  }

  auto ShuffleMask = ConstantVector::get(makeArrayRef(Indexes, NumElts));
  auto V1 = II.getArgOperand(0);
  auto V2 = Constant::getNullValue(VecTy);
  return Builder.CreateShuffleVector(V1, V2, ShuffleMask);
}

/// Attempt to convert vpermilvar* to shufflevector if the mask is constant.
static Value *simplifyX86vpermilvar(const IntrinsicInst &II,
                                    InstCombiner::BuilderTy &Builder) {
  Constant *V = dyn_cast<Constant>(II.getArgOperand(1));
  if (!V)
    return nullptr;

  auto *VecTy = cast<VectorType>(II.getType());
  auto *MaskEltTy = Type::getInt32Ty(II.getContext());
  unsigned NumElts = VecTy->getVectorNumElements();
  bool IsPD = VecTy->getScalarType()->isDoubleTy();
  unsigned NumLaneElts = IsPD ? 2 : 4;
  assert(NumElts == 16 || NumElts == 8 || NumElts == 4 || NumElts == 2);

  // Construct a shuffle mask from constant integers or UNDEFs.
  Constant *Indexes[16] = {nullptr};

  // The intrinsics only read one or two bits, clear the rest.
  for (unsigned I = 0; I < NumElts; ++I) {
    Constant *COp = V->getAggregateElement(I);
    if (!COp || (!isa<UndefValue>(COp) && !isa<ConstantInt>(COp)))
      return nullptr;

    if (isa<UndefValue>(COp)) {
      Indexes[I] = UndefValue::get(MaskEltTy);
      continue;
    }

    APInt Index = cast<ConstantInt>(COp)->getValue();
    Index = Index.zextOrTrunc(32).getLoBits(2);

    // The PD variants uses bit 1 to select per-lane element index, so
    // shift down to convert to generic shuffle mask index.
    if (IsPD)
      Index.lshrInPlace(1);

    // The _256 variants are a bit trickier since the mask bits always index
    // into the corresponding 128 half. In order to convert to a generic
    // shuffle, we have to make that explicit.
    Index += APInt(32, (I / NumLaneElts) * NumLaneElts);

    Indexes[I] = ConstantInt::get(MaskEltTy, Index);
  }

  auto ShuffleMask = ConstantVector::get(makeArrayRef(Indexes, NumElts));
  auto V1 = II.getArgOperand(0);
  auto V2 = UndefValue::get(V1->getType());
  return Builder.CreateShuffleVector(V1, V2, ShuffleMask);
}

/// Attempt to convert vpermd/vpermps to shufflevector if the mask is constant.
static Value *simplifyX86vpermv(const IntrinsicInst &II,
                                InstCombiner::BuilderTy &Builder) {
  auto *V = dyn_cast<Constant>(II.getArgOperand(1));
  if (!V)
    return nullptr;

  auto *VecTy = cast<VectorType>(II.getType());
  auto *MaskEltTy = Type::getInt32Ty(II.getContext());
  unsigned Size = VecTy->getNumElements();
  assert((Size == 4 || Size == 8 || Size == 16 || Size == 32 || Size == 64) &&
         "Unexpected shuffle mask size");

  // Construct a shuffle mask from constant integers or UNDEFs.
  Constant *Indexes[64] = {nullptr};

  for (unsigned I = 0; I < Size; ++I) {
    Constant *COp = V->getAggregateElement(I);
    if (!COp || (!isa<UndefValue>(COp) && !isa<ConstantInt>(COp)))
      return nullptr;

    if (isa<UndefValue>(COp)) {
      Indexes[I] = UndefValue::get(MaskEltTy);
      continue;
    }

    uint32_t Index = cast<ConstantInt>(COp)->getZExtValue();
    Index &= Size - 1;
    Indexes[I] = ConstantInt::get(MaskEltTy, Index);
  }

  auto ShuffleMask = ConstantVector::get(makeArrayRef(Indexes, Size));
  auto V1 = II.getArgOperand(0);
  auto V2 = UndefValue::get(VecTy);
  return Builder.CreateShuffleVector(V1, V2, ShuffleMask);
}

/// The shuffle mask for a perm2*128 selects any two halves of two 256-bit
/// source vectors, unless a zero bit is set. If a zero bit is set,
/// then ignore that half of the mask and clear that half of the vector.
static Value *simplifyX86vperm2(const IntrinsicInst &II,
                                InstCombiner::BuilderTy &Builder) {
  auto *CInt = dyn_cast<ConstantInt>(II.getArgOperand(2));
  if (!CInt)
    return nullptr;

  VectorType *VecTy = cast<VectorType>(II.getType());
  ConstantAggregateZero *ZeroVector = ConstantAggregateZero::get(VecTy);

  // The immediate permute control byte looks like this:
  //    [1:0] - select 128 bits from sources for low half of destination
  //    [2]   - ignore
  //    [3]   - zero low half of destination
  //    [5:4] - select 128 bits from sources for high half of destination
  //    [6]   - ignore
  //    [7]   - zero high half of destination

  uint8_t Imm = CInt->getZExtValue();

  bool LowHalfZero = Imm & 0x08;
  bool HighHalfZero = Imm & 0x80;

  // If both zero mask bits are set, this was just a weird way to
  // generate a zero vector.
  if (LowHalfZero && HighHalfZero)
    return ZeroVector;

  // If 0 or 1 zero mask bits are set, this is a simple shuffle.
  unsigned NumElts = VecTy->getNumElements();
  unsigned HalfSize = NumElts / 2;
  SmallVector<uint32_t, 8> ShuffleMask(NumElts);

  // The high bit of the selection field chooses the 1st or 2nd operand.
  bool LowInputSelect = Imm & 0x02;
  bool HighInputSelect = Imm & 0x20;

  // The low bit of the selection field chooses the low or high half
  // of the selected operand.
  bool LowHalfSelect = Imm & 0x01;
  bool HighHalfSelect = Imm & 0x10;

  // Determine which operand(s) are actually in use for this instruction.
  Value *V0 = LowInputSelect ? II.getArgOperand(1) : II.getArgOperand(0);
  Value *V1 = HighInputSelect ? II.getArgOperand(1) : II.getArgOperand(0);

  // If needed, replace operands based on zero mask.
  V0 = LowHalfZero ? ZeroVector : V0;
  V1 = HighHalfZero ? ZeroVector : V1;

  // Permute low half of result.
  unsigned StartIndex = LowHalfSelect ? HalfSize : 0;
  for (unsigned i = 0; i < HalfSize; ++i)
    ShuffleMask[i] = StartIndex + i;

  // Permute high half of result.
  StartIndex = HighHalfSelect ? HalfSize : 0;
  StartIndex += NumElts;
  for (unsigned i = 0; i < HalfSize; ++i)
    ShuffleMask[i + HalfSize] = StartIndex + i;

  return Builder.CreateShuffleVector(V0, V1, ShuffleMask);
}

/// Decode XOP integer vector comparison intrinsics.
static Value *simplifyX86vpcom(const IntrinsicInst &II,
                               InstCombiner::BuilderTy &Builder,
                               bool IsSigned) {
  if (auto *CInt = dyn_cast<ConstantInt>(II.getArgOperand(2))) {
    uint64_t Imm = CInt->getZExtValue() & 0x7;
    VectorType *VecTy = cast<VectorType>(II.getType());
    CmpInst::Predicate Pred = ICmpInst::BAD_ICMP_PREDICATE;

    switch (Imm) {
    case 0x0:
      Pred = IsSigned ? ICmpInst::ICMP_SLT : ICmpInst::ICMP_ULT;
      break;
    case 0x1:
      Pred = IsSigned ? ICmpInst::ICMP_SLE : ICmpInst::ICMP_ULE;
      break;
    case 0x2:
      Pred = IsSigned ? ICmpInst::ICMP_SGT : ICmpInst::ICMP_UGT;
      break;
    case 0x3:
      Pred = IsSigned ? ICmpInst::ICMP_SGE : ICmpInst::ICMP_UGE;
      break;
    case 0x4:
      Pred = ICmpInst::ICMP_EQ; break;
    case 0x5:
      Pred = ICmpInst::ICMP_NE; break;
    case 0x6:
      return ConstantInt::getSigned(VecTy, 0); // FALSE
    case 0x7:
      return ConstantInt::getSigned(VecTy, -1); // TRUE
    }

    if (Value *Cmp = Builder.CreateICmp(Pred, II.getArgOperand(0),
                                        II.getArgOperand(1)))
      return Builder.CreateSExtOrTrunc(Cmp, VecTy);
  }
  return nullptr;
}

// Emit a select instruction and appropriate bitcasts to help simplify
// masked intrinsics.
static Value *emitX86MaskSelect(Value *Mask, Value *Op0, Value *Op1,
                                InstCombiner::BuilderTy &Builder) {
  unsigned VWidth = Op0->getType()->getVectorNumElements();

  // If the mask is all ones we don't need the select. But we need to check
  // only the bit thats will be used in case VWidth is less than 8.
  if (auto *C = dyn_cast<ConstantInt>(Mask))
    if (C->getValue().zextOrTrunc(VWidth).isAllOnesValue())
      return Op0;

  auto *MaskTy = VectorType::get(Builder.getInt1Ty(),
                         cast<IntegerType>(Mask->getType())->getBitWidth());
  Mask = Builder.CreateBitCast(Mask, MaskTy);

  // If we have less than 8 elements, then the starting mask was an i8 and
  // we need to extract down to the right number of elements.
  if (VWidth < 8) {
    uint32_t Indices[4];
    for (unsigned i = 0; i != VWidth; ++i)
      Indices[i] = i;
    Mask = Builder.CreateShuffleVector(Mask, Mask,
                                       makeArrayRef(Indices, VWidth),
                                       "extract");
  }

  return Builder.CreateSelect(Mask, Op0, Op1);
}

static Value *simplifyMinnumMaxnum(const IntrinsicInst &II) {
  Value *Arg0 = II.getArgOperand(0);
  Value *Arg1 = II.getArgOperand(1);

  // fmin(x, x) -> x
  if (Arg0 == Arg1)
    return Arg0;

  const auto *C1 = dyn_cast<ConstantFP>(Arg1);

  // fmin(x, nan) -> x
  if (C1 && C1->isNaN())
    return Arg0;

  // This is the value because if undef were NaN, we would return the other
  // value and cannot return a NaN unless both operands are.
  //
  // fmin(undef, x) -> x
  if (isa<UndefValue>(Arg0))
    return Arg1;

  // fmin(x, undef) -> x
  if (isa<UndefValue>(Arg1))
    return Arg0;

  Value *X = nullptr;
  Value *Y = nullptr;
  if (II.getIntrinsicID() == Intrinsic::minnum) {
    // fmin(x, fmin(x, y)) -> fmin(x, y)
    // fmin(y, fmin(x, y)) -> fmin(x, y)
    if (match(Arg1, m_FMin(m_Value(X), m_Value(Y)))) {
      if (Arg0 == X || Arg0 == Y)
        return Arg1;
    }

    // fmin(fmin(x, y), x) -> fmin(x, y)
    // fmin(fmin(x, y), y) -> fmin(x, y)
    if (match(Arg0, m_FMin(m_Value(X), m_Value(Y)))) {
      if (Arg1 == X || Arg1 == Y)
        return Arg0;
    }

    // TODO: fmin(nnan x, inf) -> x
    // TODO: fmin(nnan ninf x, flt_max) -> x
    if (C1 && C1->isInfinity()) {
      // fmin(x, -inf) -> -inf
      if (C1->isNegative())
        return Arg1;
    }
  } else {
    assert(II.getIntrinsicID() == Intrinsic::maxnum);
    // fmax(x, fmax(x, y)) -> fmax(x, y)
    // fmax(y, fmax(x, y)) -> fmax(x, y)
    if (match(Arg1, m_FMax(m_Value(X), m_Value(Y)))) {
      if (Arg0 == X || Arg0 == Y)
        return Arg1;
    }

    // fmax(fmax(x, y), x) -> fmax(x, y)
    // fmax(fmax(x, y), y) -> fmax(x, y)
    if (match(Arg0, m_FMax(m_Value(X), m_Value(Y)))) {
      if (Arg1 == X || Arg1 == Y)
        return Arg0;
    }

    // TODO: fmax(nnan x, -inf) -> x
    // TODO: fmax(nnan ninf x, -flt_max) -> x
    if (C1 && C1->isInfinity()) {
      // fmax(x, inf) -> inf
      if (!C1->isNegative())
        return Arg1;
    }
  }
  return nullptr;
}

static bool maskIsAllOneOrUndef(Value *Mask) {
  auto *ConstMask = dyn_cast<Constant>(Mask);
  if (!ConstMask)
    return false;
  if (ConstMask->isAllOnesValue() || isa<UndefValue>(ConstMask))
    return true;
  for (unsigned I = 0, E = ConstMask->getType()->getVectorNumElements(); I != E;
       ++I) {
    if (auto *MaskElt = ConstMask->getAggregateElement(I))
      if (MaskElt->isAllOnesValue() || isa<UndefValue>(MaskElt))
        continue;
    return false;
  }
  return true;
}

static Value *simplifyMaskedLoad(const IntrinsicInst &II,
                                 InstCombiner::BuilderTy &Builder) {
  // If the mask is all ones or undefs, this is a plain vector load of the 1st
  // argument.
  if (maskIsAllOneOrUndef(II.getArgOperand(2))) {
    Value *LoadPtr = II.getArgOperand(0);
    unsigned Alignment = cast<ConstantInt>(II.getArgOperand(1))->getZExtValue();
    return Builder.CreateAlignedLoad(LoadPtr, Alignment, "unmaskedload");
  }

  return nullptr;
}

static Instruction *simplifyMaskedStore(IntrinsicInst &II, InstCombiner &IC) {
  auto *ConstMask = dyn_cast<Constant>(II.getArgOperand(3));
  if (!ConstMask)
    return nullptr;

  // If the mask is all zeros, this instruction does nothing.
  if (ConstMask->isNullValue())
    return IC.eraseInstFromFunction(II);

  // If the mask is all ones, this is a plain vector store of the 1st argument.
  if (ConstMask->isAllOnesValue()) {
    Value *StorePtr = II.getArgOperand(1);
    unsigned Alignment = cast<ConstantInt>(II.getArgOperand(2))->getZExtValue();
    return new StoreInst(II.getArgOperand(0), StorePtr, false, Alignment);
  }

  return nullptr;
}

static Instruction *simplifyMaskedGather(IntrinsicInst &II, InstCombiner &IC) {
  // If the mask is all zeros, return the "passthru" argument of the gather.
  auto *ConstMask = dyn_cast<Constant>(II.getArgOperand(2));
  if (ConstMask && ConstMask->isNullValue())
    return IC.replaceInstUsesWith(II, II.getArgOperand(3));

  return nullptr;
}

static Instruction *simplifyMaskedScatter(IntrinsicInst &II, InstCombiner &IC) {
  // If the mask is all zeros, a scatter does nothing.
  auto *ConstMask = dyn_cast<Constant>(II.getArgOperand(3));
  if (ConstMask && ConstMask->isNullValue())
    return IC.eraseInstFromFunction(II);

  return nullptr;
}

static Instruction *foldCttzCtlz(IntrinsicInst &II, InstCombiner &IC) {
  assert((II.getIntrinsicID() == Intrinsic::cttz ||
          II.getIntrinsicID() == Intrinsic::ctlz) &&
         "Expected cttz or ctlz intrinsic");
  Value *Op0 = II.getArgOperand(0);

  KnownBits Known = IC.computeKnownBits(Op0, 0, &II);

  // Create a mask for bits above (ctlz) or below (cttz) the first known one.
  bool IsTZ = II.getIntrinsicID() == Intrinsic::cttz;
  unsigned PossibleZeros = IsTZ ? Known.countMaxTrailingZeros()
                                : Known.countMaxLeadingZeros();
  unsigned DefiniteZeros = IsTZ ? Known.countMinTrailingZeros()
                                : Known.countMinLeadingZeros();

  // If all bits above (ctlz) or below (cttz) the first known one are known
  // zero, this value is constant.
  // FIXME: This should be in InstSimplify because we're replacing an
  // instruction with a constant.
  if (PossibleZeros == DefiniteZeros) {
    auto *C = ConstantInt::get(Op0->getType(), DefiniteZeros);
    return IC.replaceInstUsesWith(II, C);
  }

  // If the input to cttz/ctlz is known to be non-zero,
  // then change the 'ZeroIsUndef' parameter to 'true'
  // because we know the zero behavior can't affect the result.
  if (!Known.One.isNullValue() ||
      isKnownNonZero(Op0, IC.getDataLayout(), 0, &IC.getAssumptionCache(), &II,
                     &IC.getDominatorTree())) {
    if (!match(II.getArgOperand(1), m_One())) {
      II.setOperand(1, IC.Builder.getTrue());
      return &II;
    }
  }

  // Add range metadata since known bits can't completely reflect what we know.
  // TODO: Handle splat vectors.
  auto *IT = dyn_cast<IntegerType>(Op0->getType());
  if (IT && IT->getBitWidth() != 1 && !II.getMetadata(LLVMContext::MD_range)) {
    Metadata *LowAndHigh[] = {
        ConstantAsMetadata::get(ConstantInt::get(IT, DefiniteZeros)),
        ConstantAsMetadata::get(ConstantInt::get(IT, PossibleZeros + 1))};
    II.setMetadata(LLVMContext::MD_range,
                   MDNode::get(II.getContext(), LowAndHigh));
    return &II;
  }

  return nullptr;
}

static Instruction *foldCtpop(IntrinsicInst &II, InstCombiner &IC) {
  assert(II.getIntrinsicID() == Intrinsic::ctpop &&
         "Expected ctpop intrinsic");
  Value *Op0 = II.getArgOperand(0);
  // FIXME: Try to simplify vectors of integers.
  auto *IT = dyn_cast<IntegerType>(Op0->getType());
  if (!IT)
    return nullptr;

  unsigned BitWidth = IT->getBitWidth();
  KnownBits Known(BitWidth);
  IC.computeKnownBits(Op0, Known, 0, &II);

  unsigned MinCount = Known.countMinPopulation();
  unsigned MaxCount = Known.countMaxPopulation();

  // Add range metadata since known bits can't completely reflect what we know.
  if (IT->getBitWidth() != 1 && !II.getMetadata(LLVMContext::MD_range)) {
    Metadata *LowAndHigh[] = {
        ConstantAsMetadata::get(ConstantInt::get(IT, MinCount)),
        ConstantAsMetadata::get(ConstantInt::get(IT, MaxCount + 1))};
    II.setMetadata(LLVMContext::MD_range,
                   MDNode::get(II.getContext(), LowAndHigh));
    return &II;
  }

  return nullptr;
}

// TODO: If the x86 backend knew how to convert a bool vector mask back to an
// XMM register mask efficiently, we could transform all x86 masked intrinsics
// to LLVM masked intrinsics and remove the x86 masked intrinsic defs.
static Instruction *simplifyX86MaskedLoad(IntrinsicInst &II, InstCombiner &IC) {
  Value *Ptr = II.getOperand(0);
  Value *Mask = II.getOperand(1);
  Constant *ZeroVec = Constant::getNullValue(II.getType());

  // Special case a zero mask since that's not a ConstantDataVector.
  // This masked load instruction creates a zero vector.
  if (isa<ConstantAggregateZero>(Mask))
    return IC.replaceInstUsesWith(II, ZeroVec);

  auto *ConstMask = dyn_cast<ConstantDataVector>(Mask);
  if (!ConstMask)
    return nullptr;

  // The mask is constant. Convert this x86 intrinsic to the LLVM instrinsic
  // to allow target-independent optimizations.

  // First, cast the x86 intrinsic scalar pointer to a vector pointer to match
  // the LLVM intrinsic definition for the pointer argument.
  unsigned AddrSpace = cast<PointerType>(Ptr->getType())->getAddressSpace();
  PointerType *VecPtrTy = PointerType::get(II.getType(), AddrSpace);
  Value *PtrCast = IC.Builder.CreateBitCast(Ptr, VecPtrTy, "castvec");

  // Second, convert the x86 XMM integer vector mask to a vector of bools based
  // on each element's most significant bit (the sign bit).
  Constant *BoolMask = getNegativeIsTrueBoolVec(ConstMask);

  // The pass-through vector for an x86 masked load is a zero vector.
  CallInst *NewMaskedLoad =
      IC.Builder.CreateMaskedLoad(PtrCast, 1, BoolMask, ZeroVec);
  return IC.replaceInstUsesWith(II, NewMaskedLoad);
}

// TODO: If the x86 backend knew how to convert a bool vector mask back to an
// XMM register mask efficiently, we could transform all x86 masked intrinsics
// to LLVM masked intrinsics and remove the x86 masked intrinsic defs.
static bool simplifyX86MaskedStore(IntrinsicInst &II, InstCombiner &IC) {
  Value *Ptr = II.getOperand(0);
  Value *Mask = II.getOperand(1);
  Value *Vec = II.getOperand(2);

  // Special case a zero mask since that's not a ConstantDataVector:
  // this masked store instruction does nothing.
  if (isa<ConstantAggregateZero>(Mask)) {
    IC.eraseInstFromFunction(II);
    return true;
  }

  // The SSE2 version is too weird (eg, unaligned but non-temporal) to do
  // anything else at this level.
  if (II.getIntrinsicID() == Intrinsic::x86_sse2_maskmov_dqu)
    return false;

  auto *ConstMask = dyn_cast<ConstantDataVector>(Mask);
  if (!ConstMask)
    return false;

  // The mask is constant. Convert this x86 intrinsic to the LLVM instrinsic
  // to allow target-independent optimizations.

  // First, cast the x86 intrinsic scalar pointer to a vector pointer to match
  // the LLVM intrinsic definition for the pointer argument.
  unsigned AddrSpace = cast<PointerType>(Ptr->getType())->getAddressSpace();
  PointerType *VecPtrTy = PointerType::get(Vec->getType(), AddrSpace);
  Value *PtrCast = IC.Builder.CreateBitCast(Ptr, VecPtrTy, "castvec");

  // Second, convert the x86 XMM integer vector mask to a vector of bools based
  // on each element's most significant bit (the sign bit).
  Constant *BoolMask = getNegativeIsTrueBoolVec(ConstMask);

  IC.Builder.CreateMaskedStore(Vec, PtrCast, 1, BoolMask);

  // 'Replace uses' doesn't work for stores. Erase the original masked store.
  IC.eraseInstFromFunction(II);
  return true;
}

// Constant fold llvm.amdgcn.fmed3 intrinsics for standard inputs.
//
// A single NaN input is folded to minnum, so we rely on that folding for
// handling NaNs.
static APFloat fmed3AMDGCN(const APFloat &Src0, const APFloat &Src1,
                           const APFloat &Src2) {
  APFloat Max3 = maxnum(maxnum(Src0, Src1), Src2);

  APFloat::cmpResult Cmp0 = Max3.compare(Src0);
  assert(Cmp0 != APFloat::cmpUnordered && "nans handled separately");
  if (Cmp0 == APFloat::cmpEqual)
    return maxnum(Src1, Src2);

  APFloat::cmpResult Cmp1 = Max3.compare(Src1);
  assert(Cmp1 != APFloat::cmpUnordered && "nans handled separately");
  if (Cmp1 == APFloat::cmpEqual)
    return maxnum(Src0, Src2);

  return maxnum(Src0, Src1);
}

// Returns true iff the 2 intrinsics have the same operands, limiting the
// comparison to the first NumOperands.
static bool haveSameOperands(const IntrinsicInst &I, const IntrinsicInst &E,
                             unsigned NumOperands) {
  assert(I.getNumArgOperands() >= NumOperands && "Not enough operands");
  assert(E.getNumArgOperands() >= NumOperands && "Not enough operands");
  for (unsigned i = 0; i < NumOperands; i++)
    if (I.getArgOperand(i) != E.getArgOperand(i))
      return false;
  return true;
}

// Remove trivially empty start/end intrinsic ranges, i.e. a start
// immediately followed by an end (ignoring debuginfo or other
// start/end intrinsics in between). As this handles only the most trivial
// cases, tracking the nesting level is not needed:
//
//   call @llvm.foo.start(i1 0) ; &I
//   call @llvm.foo.start(i1 0)
//   call @llvm.foo.end(i1 0) ; This one will not be skipped: it will be removed
//   call @llvm.foo.end(i1 0)
static bool removeTriviallyEmptyRange(IntrinsicInst &I, unsigned StartID,
                                      unsigned EndID, InstCombiner &IC) {
  assert(I.getIntrinsicID() == StartID &&
         "Start intrinsic does not have expected ID");
  BasicBlock::iterator BI(I), BE(I.getParent()->end());
  for (++BI; BI != BE; ++BI) {
    if (auto *E = dyn_cast<IntrinsicInst>(BI)) {
      if (isa<DbgInfoIntrinsic>(E) || E->getIntrinsicID() == StartID)
        continue;
      if (E->getIntrinsicID() == EndID &&
          haveSameOperands(I, *E, E->getNumArgOperands())) {
        IC.eraseInstFromFunction(*E);
        IC.eraseInstFromFunction(I);
        return true;
      }
    }
    break;
  }

  return false;
}

// Convert NVVM intrinsics to target-generic LLVM code where possible.
static Instruction *SimplifyNVVMIntrinsic(IntrinsicInst *II, InstCombiner &IC) {
  // Each NVVM intrinsic we can simplify can be replaced with one of:
  //
  //  * an LLVM intrinsic,
  //  * an LLVM cast operation,
  //  * an LLVM binary operation, or
  //  * ad-hoc LLVM IR for the particular operation.

  // Some transformations are only valid when the module's
  // flush-denormals-to-zero (ftz) setting is true/false, whereas other
  // transformations are valid regardless of the module's ftz setting.
  enum FtzRequirementTy {
    FTZ_Any,       // Any ftz setting is ok.
    FTZ_MustBeOn,  // Transformation is valid only if ftz is on.
    FTZ_MustBeOff, // Transformation is valid only if ftz is off.
  };
  // Classes of NVVM intrinsics that can't be replaced one-to-one with a
  // target-generic intrinsic, cast op, or binary op but that we can nonetheless
  // simplify.
  enum SpecialCase {
    SPC_Reciprocal,
  };

  // SimplifyAction is a poor-man's variant (plus an additional flag) that
  // represents how to replace an NVVM intrinsic with target-generic LLVM IR.
  struct SimplifyAction {
    // Invariant: At most one of these Optionals has a value.
    Optional<Intrinsic::ID> IID;
    Optional<Instruction::CastOps> CastOp;
    Optional<Instruction::BinaryOps> BinaryOp;
    Optional<SpecialCase> Special;

    FtzRequirementTy FtzRequirement = FTZ_Any;

    SimplifyAction() = default;

    SimplifyAction(Intrinsic::ID IID, FtzRequirementTy FtzReq)
        : IID(IID), FtzRequirement(FtzReq) {}

    // Cast operations don't have anything to do with FTZ, so we skip that
    // argument.
    SimplifyAction(Instruction::CastOps CastOp) : CastOp(CastOp) {}

    SimplifyAction(Instruction::BinaryOps BinaryOp, FtzRequirementTy FtzReq)
        : BinaryOp(BinaryOp), FtzRequirement(FtzReq) {}

    SimplifyAction(SpecialCase Special, FtzRequirementTy FtzReq)
        : Special(Special), FtzRequirement(FtzReq) {}
  };

  // Try to generate a SimplifyAction describing how to replace our
  // IntrinsicInstr with target-generic LLVM IR.
  const SimplifyAction Action = [II]() -> SimplifyAction {
    switch (II->getIntrinsicID()) {

    // NVVM intrinsics that map directly to LLVM intrinsics.
    case Intrinsic::nvvm_ceil_d:
      return {Intrinsic::ceil, FTZ_Any};
    case Intrinsic::nvvm_ceil_f:
      return {Intrinsic::ceil, FTZ_MustBeOff};
    case Intrinsic::nvvm_ceil_ftz_f:
      return {Intrinsic::ceil, FTZ_MustBeOn};
    case Intrinsic::nvvm_fabs_d:
      return {Intrinsic::fabs, FTZ_Any};
    case Intrinsic::nvvm_fabs_f:
      return {Intrinsic::fabs, FTZ_MustBeOff};
    case Intrinsic::nvvm_fabs_ftz_f:
      return {Intrinsic::fabs, FTZ_MustBeOn};
    case Intrinsic::nvvm_floor_d:
      return {Intrinsic::floor, FTZ_Any};
    case Intrinsic::nvvm_floor_f:
      return {Intrinsic::floor, FTZ_MustBeOff};
    case Intrinsic::nvvm_floor_ftz_f:
      return {Intrinsic::floor, FTZ_MustBeOn};
    case Intrinsic::nvvm_fma_rn_d:
      return {Intrinsic::fma, FTZ_Any};
    case Intrinsic::nvvm_fma_rn_f:
      return {Intrinsic::fma, FTZ_MustBeOff};
    case Intrinsic::nvvm_fma_rn_ftz_f:
      return {Intrinsic::fma, FTZ_MustBeOn};
    case Intrinsic::nvvm_fmax_d:
      return {Intrinsic::maxnum, FTZ_Any};
    case Intrinsic::nvvm_fmax_f:
      return {Intrinsic::maxnum, FTZ_MustBeOff};
    case Intrinsic::nvvm_fmax_ftz_f:
      return {Intrinsic::maxnum, FTZ_MustBeOn};
    case Intrinsic::nvvm_fmin_d:
      return {Intrinsic::minnum, FTZ_Any};
    case Intrinsic::nvvm_fmin_f:
      return {Intrinsic::minnum, FTZ_MustBeOff};
    case Intrinsic::nvvm_fmin_ftz_f:
      return {Intrinsic::minnum, FTZ_MustBeOn};
    case Intrinsic::nvvm_round_d:
      return {Intrinsic::round, FTZ_Any};
    case Intrinsic::nvvm_round_f:
      return {Intrinsic::round, FTZ_MustBeOff};
    case Intrinsic::nvvm_round_ftz_f:
      return {Intrinsic::round, FTZ_MustBeOn};
    case Intrinsic::nvvm_sqrt_rn_d:
      return {Intrinsic::sqrt, FTZ_Any};
    case Intrinsic::nvvm_sqrt_f:
      // nvvm_sqrt_f is a special case.  For  most intrinsics, foo_ftz_f is the
      // ftz version, and foo_f is the non-ftz version.  But nvvm_sqrt_f adopts
      // the ftz-ness of the surrounding code.  sqrt_rn_f and sqrt_rn_ftz_f are
      // the versions with explicit ftz-ness.
      return {Intrinsic::sqrt, FTZ_Any};
    case Intrinsic::nvvm_sqrt_rn_f:
      return {Intrinsic::sqrt, FTZ_MustBeOff};
    case Intrinsic::nvvm_sqrt_rn_ftz_f:
      return {Intrinsic::sqrt, FTZ_MustBeOn};
    case Intrinsic::nvvm_trunc_d:
      return {Intrinsic::trunc, FTZ_Any};
    case Intrinsic::nvvm_trunc_f:
      return {Intrinsic::trunc, FTZ_MustBeOff};
    case Intrinsic::nvvm_trunc_ftz_f:
      return {Intrinsic::trunc, FTZ_MustBeOn};

    // NVVM intrinsics that map to LLVM cast operations.
    //
    // Note that llvm's target-generic conversion operators correspond to the rz
    // (round to zero) versions of the nvvm conversion intrinsics, even though
    // most everything else here uses the rn (round to nearest even) nvvm ops.
    case Intrinsic::nvvm_d2i_rz:
    case Intrinsic::nvvm_f2i_rz:
    case Intrinsic::nvvm_d2ll_rz:
    case Intrinsic::nvvm_f2ll_rz:
      return {Instruction::FPToSI};
    case Intrinsic::nvvm_d2ui_rz:
    case Intrinsic::nvvm_f2ui_rz:
    case Intrinsic::nvvm_d2ull_rz:
    case Intrinsic::nvvm_f2ull_rz:
      return {Instruction::FPToUI};
    case Intrinsic::nvvm_i2d_rz:
    case Intrinsic::nvvm_i2f_rz:
    case Intrinsic::nvvm_ll2d_rz:
    case Intrinsic::nvvm_ll2f_rz:
      return {Instruction::SIToFP};
    case Intrinsic::nvvm_ui2d_rz:
    case Intrinsic::nvvm_ui2f_rz:
    case Intrinsic::nvvm_ull2d_rz:
    case Intrinsic::nvvm_ull2f_rz:
      return {Instruction::UIToFP};

    // NVVM intrinsics that map to LLVM binary ops.
    case Intrinsic::nvvm_add_rn_d:
      return {Instruction::FAdd, FTZ_Any};
    case Intrinsic::nvvm_add_rn_f:
      return {Instruction::FAdd, FTZ_MustBeOff};
    case Intrinsic::nvvm_add_rn_ftz_f:
      return {Instruction::FAdd, FTZ_MustBeOn};
    case Intrinsic::nvvm_mul_rn_d:
      return {Instruction::FMul, FTZ_Any};
    case Intrinsic::nvvm_mul_rn_f:
      return {Instruction::FMul, FTZ_MustBeOff};
    case Intrinsic::nvvm_mul_rn_ftz_f:
      return {Instruction::FMul, FTZ_MustBeOn};
    case Intrinsic::nvvm_div_rn_d:
      return {Instruction::FDiv, FTZ_Any};
    case Intrinsic::nvvm_div_rn_f:
      return {Instruction::FDiv, FTZ_MustBeOff};
    case Intrinsic::nvvm_div_rn_ftz_f:
      return {Instruction::FDiv, FTZ_MustBeOn};

    // The remainder of cases are NVVM intrinsics that map to LLVM idioms, but
    // need special handling.
    //
    // We seem to be missing intrinsics for rcp.approx.{ftz.}f32, which is just
    // as well.
    case Intrinsic::nvvm_rcp_rn_d:
      return {SPC_Reciprocal, FTZ_Any};
    case Intrinsic::nvvm_rcp_rn_f:
      return {SPC_Reciprocal, FTZ_MustBeOff};
    case Intrinsic::nvvm_rcp_rn_ftz_f:
      return {SPC_Reciprocal, FTZ_MustBeOn};

    // We do not currently simplify intrinsics that give an approximate answer.
    // These include:
    //
    //   - nvvm_cos_approx_{f,ftz_f}
    //   - nvvm_ex2_approx_{d,f,ftz_f}
    //   - nvvm_lg2_approx_{d,f,ftz_f}
    //   - nvvm_sin_approx_{f,ftz_f}
    //   - nvvm_sqrt_approx_{f,ftz_f}
    //   - nvvm_rsqrt_approx_{d,f,ftz_f}
    //   - nvvm_div_approx_{ftz_d,ftz_f,f}
    //   - nvvm_rcp_approx_ftz_d
    //
    // Ideally we'd encode them as e.g. "fast call @llvm.cos", where "fast"
    // means that fastmath is enabled in the intrinsic.  Unfortunately only
    // binary operators (currently) have a fastmath bit in SelectionDAG, so this
    // information gets lost and we can't select on it.
    //
    // TODO: div and rcp are lowered to a binary op, so these we could in theory
    // lower them to "fast fdiv".

    default:
      return {};
    }
  }();

  // If Action.FtzRequirementTy is not satisfied by the module's ftz state, we
  // can bail out now.  (Notice that in the case that IID is not an NVVM
  // intrinsic, we don't have to look up any module metadata, as
  // FtzRequirementTy will be FTZ_Any.)
  if (Action.FtzRequirement != FTZ_Any) {
    bool FtzEnabled =
        II->getFunction()->getFnAttribute("nvptx-f32ftz").getValueAsString() ==
        "true";

    if (FtzEnabled != (Action.FtzRequirement == FTZ_MustBeOn))
      return nullptr;
  }

  // Simplify to target-generic intrinsic.
  if (Action.IID) {
    SmallVector<Value *, 4> Args(II->arg_operands());
    // All the target-generic intrinsics currently of interest to us have one
    // type argument, equal to that of the nvvm intrinsic's argument.
    Type *Tys[] = {II->getArgOperand(0)->getType()};
    return CallInst::Create(
        Intrinsic::getDeclaration(II->getModule(), *Action.IID, Tys), Args);
  }

  // Simplify to target-generic binary op.
  if (Action.BinaryOp)
    return BinaryOperator::Create(*Action.BinaryOp, II->getArgOperand(0),
                                  II->getArgOperand(1), II->getName());

  // Simplify to target-generic cast op.
  if (Action.CastOp)
    return CastInst::Create(*Action.CastOp, II->getArgOperand(0), II->getType(),
                            II->getName());

  // All that's left are the special cases.
  if (!Action.Special)
    return nullptr;

  switch (*Action.Special) {
  case SPC_Reciprocal:
    // Simplify reciprocal.
    return BinaryOperator::Create(
        Instruction::FDiv, ConstantFP::get(II->getArgOperand(0)->getType(), 1),
        II->getArgOperand(0), II->getName());
  }
  llvm_unreachable("All SpecialCase enumerators should be handled in switch.");
}

Instruction *InstCombiner::visitVAStartInst(VAStartInst &I) {
  removeTriviallyEmptyRange(I, Intrinsic::vastart, Intrinsic::vaend, *this);
  return nullptr;
}

Instruction *InstCombiner::visitVACopyInst(VACopyInst &I) {
  removeTriviallyEmptyRange(I, Intrinsic::vacopy, Intrinsic::vaend, *this);
  return nullptr;
}

/// CallInst simplification. This mostly only handles folding of intrinsic
/// instructions. For normal calls, it allows visitCallSite to do the heavy
/// lifting.
Instruction *InstCombiner::visitCallInst(CallInst &CI) {
  auto Args = CI.arg_operands();
  if (Value *V = SimplifyCall(&CI, CI.getCalledValue(), Args.begin(),
                              Args.end(), SQ.getWithInstruction(&CI)))
    return replaceInstUsesWith(CI, V);

  if (isFreeCall(&CI, &TLI))
    return visitFree(CI);

  // If the caller function is nounwind, mark the call as nounwind, even if the
  // callee isn't.
  if (CI.getFunction()->doesNotThrow() && !CI.doesNotThrow()) {
    CI.setDoesNotThrow();
    return &CI;
  }

  IntrinsicInst *II = dyn_cast<IntrinsicInst>(&CI);
  if (!II) return visitCallSite(&CI);

  // Intrinsics cannot occur in an invoke, so handle them here instead of in
  // visitCallSite.
  if (MemIntrinsic *MI = dyn_cast<MemIntrinsic>(II)) {
    bool Changed = false;

    // memmove/cpy/set of zero bytes is a noop.
    if (Constant *NumBytes = dyn_cast<Constant>(MI->getLength())) {
      if (NumBytes->isNullValue())
        return eraseInstFromFunction(CI);

      if (ConstantInt *CI = dyn_cast<ConstantInt>(NumBytes))
        if (CI->getZExtValue() == 1) {
          // Replace the instruction with just byte operations.  We would
          // transform other cases to loads/stores, but we don't know if
          // alignment is sufficient.
        }
    }

    // No other transformations apply to volatile transfers.
    if (MI->isVolatile())
      return nullptr;

    // If we have a memmove and the source operation is a constant global,
    // then the source and dest pointers can't alias, so we can change this
    // into a call to memcpy.
    if (MemMoveInst *MMI = dyn_cast<MemMoveInst>(MI)) {
      if (GlobalVariable *GVSrc = dyn_cast<GlobalVariable>(MMI->getSource()))
        if (GVSrc->isConstant()) {
          Module *M = CI.getModule();
          Intrinsic::ID MemCpyID = Intrinsic::memcpy;
          Type *Tys[3] = { CI.getArgOperand(0)->getType(),
                           CI.getArgOperand(1)->getType(),
                           CI.getArgOperand(2)->getType() };
          CI.setCalledFunction(Intrinsic::getDeclaration(M, MemCpyID, Tys));
          Changed = true;
        }
    }

    if (MemTransferInst *MTI = dyn_cast<MemTransferInst>(MI)) {
      // memmove(x,x,size) -> noop.
      if (MTI->getSource() == MTI->getDest())
        return eraseInstFromFunction(CI);
    }

    // If we can determine a pointer alignment that is bigger than currently
    // set, update the alignment.
    if (isa<MemTransferInst>(MI)) {
      if (Instruction *I = SimplifyMemTransfer(MI))
        return I;
    } else if (MemSetInst *MSI = dyn_cast<MemSetInst>(MI)) {
      if (Instruction *I = SimplifyMemSet(MSI))
        return I;
    }

    if (Changed) return II;
  }

  if (auto *AMI = dyn_cast<ElementUnorderedAtomicMemCpyInst>(II)) {
    if (Constant *C = dyn_cast<Constant>(AMI->getLength()))
      if (C->isNullValue())
        return eraseInstFromFunction(*AMI);

    if (Instruction *I = SimplifyElementUnorderedAtomicMemCpy(AMI))
      return I;
  }

  if (Instruction *I = SimplifyNVVMIntrinsic(II, *this))
    return I;

  auto SimplifyDemandedVectorEltsLow = [this](Value *Op, unsigned Width,
                                              unsigned DemandedWidth) {
    APInt UndefElts(Width, 0);
    APInt DemandedElts = APInt::getLowBitsSet(Width, DemandedWidth);
    return SimplifyDemandedVectorElts(Op, DemandedElts, UndefElts);
  };

  switch (II->getIntrinsicID()) {
  default: break;
  case Intrinsic::objectsize:
    if (ConstantInt *N =
            lowerObjectSizeCall(II, DL, &TLI, /*MustSucceed=*/false))
      return replaceInstUsesWith(CI, N);
    return nullptr;

  case Intrinsic::bswap: {
    Value *IIOperand = II->getArgOperand(0);
    Value *X = nullptr;

    // TODO should this be in InstSimplify?
    // bswap(bswap(x)) -> x
    if (match(IIOperand, m_BSwap(m_Value(X))))
      return replaceInstUsesWith(CI, X);

    // bswap(trunc(bswap(x))) -> trunc(lshr(x, c))
    if (match(IIOperand, m_Trunc(m_BSwap(m_Value(X))))) {
      unsigned C = X->getType()->getPrimitiveSizeInBits() -
        IIOperand->getType()->getPrimitiveSizeInBits();
      Value *CV = ConstantInt::get(X->getType(), C);
      Value *V = Builder.CreateLShr(X, CV);
      return new TruncInst(V, IIOperand->getType());
    }
    break;
  }

  case Intrinsic::bitreverse: {
    Value *IIOperand = II->getArgOperand(0);
    Value *X = nullptr;

    // TODO should this be in InstSimplify?
    // bitreverse(bitreverse(x)) -> x
    if (match(IIOperand, m_BitReverse(m_Value(X))))
      return replaceInstUsesWith(CI, X);
    break;
  }

  case Intrinsic::masked_load:
    if (Value *SimplifiedMaskedOp = simplifyMaskedLoad(*II, Builder))
      return replaceInstUsesWith(CI, SimplifiedMaskedOp);
    break;
  case Intrinsic::masked_store:
    return simplifyMaskedStore(*II, *this);
  case Intrinsic::masked_gather:
    return simplifyMaskedGather(*II, *this);
  case Intrinsic::masked_scatter:
    return simplifyMaskedScatter(*II, *this);

  case Intrinsic::powi:
    if (ConstantInt *Power = dyn_cast<ConstantInt>(II->getArgOperand(1))) {
      // powi(x, 0) -> 1.0
      if (Power->isZero())
        return replaceInstUsesWith(CI, ConstantFP::get(CI.getType(), 1.0));
      // powi(x, 1) -> x
      if (Power->isOne())
        return replaceInstUsesWith(CI, II->getArgOperand(0));
      // powi(x, -1) -> 1/x
      if (Power->isMinusOne())
        return BinaryOperator::CreateFDiv(ConstantFP::get(CI.getType(), 1.0),
                                          II->getArgOperand(0));
    }
    break;

  case Intrinsic::cttz:
  case Intrinsic::ctlz:
    if (auto *I = foldCttzCtlz(*II, *this))
      return I;
    break;

  case Intrinsic::ctpop:
    if (auto *I = foldCtpop(*II, *this))
      return I;
    break;

  case Intrinsic::uadd_with_overflow:
  case Intrinsic::sadd_with_overflow:
  case Intrinsic::umul_with_overflow:
  case Intrinsic::smul_with_overflow:
    if (isa<Constant>(II->getArgOperand(0)) &&
        !isa<Constant>(II->getArgOperand(1))) {
      // Canonicalize constants into the RHS.
      Value *LHS = II->getArgOperand(0);
      II->setArgOperand(0, II->getArgOperand(1));
      II->setArgOperand(1, LHS);
      return II;
    }
    LLVM_FALLTHROUGH;

  case Intrinsic::usub_with_overflow:
  case Intrinsic::ssub_with_overflow: {
    OverflowCheckFlavor OCF =
        IntrinsicIDToOverflowCheckFlavor(II->getIntrinsicID());
    assert(OCF != OCF_INVALID && "unexpected!");

    Value *OperationResult = nullptr;
    Constant *OverflowResult = nullptr;
    if (OptimizeOverflowCheck(OCF, II->getArgOperand(0), II->getArgOperand(1),
                              *II, OperationResult, OverflowResult))
      return CreateOverflowTuple(II, OperationResult, OverflowResult);

    break;
  }

  case Intrinsic::minnum:
  case Intrinsic::maxnum: {
    Value *Arg0 = II->getArgOperand(0);
    Value *Arg1 = II->getArgOperand(1);
    // Canonicalize constants to the RHS.
    if (isa<ConstantFP>(Arg0) && !isa<ConstantFP>(Arg1)) {
      II->setArgOperand(0, Arg1);
      II->setArgOperand(1, Arg0);
      return II;
    }
    if (Value *V = simplifyMinnumMaxnum(*II))
      return replaceInstUsesWith(*II, V);
    break;
  }
  case Intrinsic::fmuladd: {
    // Canonicalize fast fmuladd to the separate fmul + fadd.
    if (II->hasUnsafeAlgebra()) {
      BuilderTy::FastMathFlagGuard Guard(Builder);
      Builder.setFastMathFlags(II->getFastMathFlags());
      Value *Mul = Builder.CreateFMul(II->getArgOperand(0),
                                      II->getArgOperand(1));
      Value *Add = Builder.CreateFAdd(Mul, II->getArgOperand(2));
      Add->takeName(II);
      return replaceInstUsesWith(*II, Add);
    }

    LLVM_FALLTHROUGH;
  }
  case Intrinsic::fma: {
    Value *Src0 = II->getArgOperand(0);
    Value *Src1 = II->getArgOperand(1);

    // Canonicalize constants into the RHS.
    if (isa<Constant>(Src0) && !isa<Constant>(Src1)) {
      II->setArgOperand(0, Src1);
      II->setArgOperand(1, Src0);
      std::swap(Src0, Src1);
    }

    Value *LHS = nullptr;
    Value *RHS = nullptr;

    // fma fneg(x), fneg(y), z -> fma x, y, z
    if (match(Src0, m_FNeg(m_Value(LHS))) &&
        match(Src1, m_FNeg(m_Value(RHS)))) {
      II->setArgOperand(0, LHS);
      II->setArgOperand(1, RHS);
      return II;
    }

    // fma fabs(x), fabs(x), z -> fma x, x, z
    if (match(Src0, m_Intrinsic<Intrinsic::fabs>(m_Value(LHS))) &&
        match(Src1, m_Intrinsic<Intrinsic::fabs>(m_Value(RHS))) && LHS == RHS) {
      II->setArgOperand(0, LHS);
      II->setArgOperand(1, RHS);
      return II;
    }

    // fma x, 1, z -> fadd x, z
    if (match(Src1, m_FPOne())) {
      Instruction *RI = BinaryOperator::CreateFAdd(Src0, II->getArgOperand(2));
      RI->copyFastMathFlags(II);
      return RI;
    }

    break;
  }
  case Intrinsic::fabs: {
    Value *Cond;
    Constant *LHS, *RHS;
    if (match(II->getArgOperand(0),
              m_Select(m_Value(Cond), m_Constant(LHS), m_Constant(RHS)))) {
      CallInst *Call0 = Builder.CreateCall(II->getCalledFunction(), {LHS});
      CallInst *Call1 = Builder.CreateCall(II->getCalledFunction(), {RHS});
      return SelectInst::Create(Cond, Call0, Call1);
    }

    LLVM_FALLTHROUGH;
  }
  case Intrinsic::ceil:
  case Intrinsic::floor:
  case Intrinsic::round:
  case Intrinsic::nearbyint:
  case Intrinsic::rint:
  case Intrinsic::trunc: {
    Value *ExtSrc;
    if (match(II->getArgOperand(0), m_FPExt(m_Value(ExtSrc))) &&
        II->getArgOperand(0)->hasOneUse()) {
      // fabs (fpext x) -> fpext (fabs x)
      Value *F = Intrinsic::getDeclaration(II->getModule(), II->getIntrinsicID(),
                                           { ExtSrc->getType() });
      CallInst *NewFabs = Builder.CreateCall(F, ExtSrc);
      NewFabs->copyFastMathFlags(II);
      NewFabs->takeName(II);
      return new FPExtInst(NewFabs, II->getType());
    }

    break;
  }
  case Intrinsic::cos:
  case Intrinsic::amdgcn_cos: {
    Value *SrcSrc;
    Value *Src = II->getArgOperand(0);
    if (match(Src, m_FNeg(m_Value(SrcSrc))) ||
        match(Src, m_Intrinsic<Intrinsic::fabs>(m_Value(SrcSrc)))) {
      // cos(-x) -> cos(x)
      // cos(fabs(x)) -> cos(x)
      II->setArgOperand(0, SrcSrc);
      return II;
    }

    break;
  }
  case Intrinsic::ppc_altivec_lvx:
  case Intrinsic::ppc_altivec_lvxl:
    // Turn PPC lvx -> load if the pointer is known aligned.
    if (getOrEnforceKnownAlignment(II->getArgOperand(0), 16, DL, II, &AC,
                                   &DT) >= 16) {
      Value *Ptr = Builder.CreateBitCast(II->getArgOperand(0),
                                         PointerType::getUnqual(II->getType()));
      return new LoadInst(Ptr);
    }
    break;
  case Intrinsic::ppc_vsx_lxvw4x:
  case Intrinsic::ppc_vsx_lxvd2x: {
    // Turn PPC VSX loads into normal loads.
    Value *Ptr = Builder.CreateBitCast(II->getArgOperand(0),
                                       PointerType::getUnqual(II->getType()));
    return new LoadInst(Ptr, Twine(""), false, 1);
  }
  case Intrinsic::ppc_altivec_stvx:
  case Intrinsic::ppc_altivec_stvxl:
    // Turn stvx -> store if the pointer is known aligned.
    if (getOrEnforceKnownAlignment(II->getArgOperand(1), 16, DL, II, &AC,
                                   &DT) >= 16) {
      Type *OpPtrTy =
        PointerType::getUnqual(II->getArgOperand(0)->getType());
      Value *Ptr = Builder.CreateBitCast(II->getArgOperand(1), OpPtrTy);
      return new StoreInst(II->getArgOperand(0), Ptr);
    }
    break;
  case Intrinsic::ppc_vsx_stxvw4x:
  case Intrinsic::ppc_vsx_stxvd2x: {
    // Turn PPC VSX stores into normal stores.
    Type *OpPtrTy = PointerType::getUnqual(II->getArgOperand(0)->getType());
    Value *Ptr = Builder.CreateBitCast(II->getArgOperand(1), OpPtrTy);
    return new StoreInst(II->getArgOperand(0), Ptr, false, 1);
  }
  case Intrinsic::ppc_qpx_qvlfs:
    // Turn PPC QPX qvlfs -> load if the pointer is known aligned.
    if (getOrEnforceKnownAlignment(II->getArgOperand(0), 16, DL, II, &AC,
                                   &DT) >= 16) {
      Type *VTy = VectorType::get(Builder.getFloatTy(),
                                  II->getType()->getVectorNumElements());
      Value *Ptr = Builder.CreateBitCast(II->getArgOperand(0),
                                         PointerType::getUnqual(VTy));
      Value *Load = Builder.CreateLoad(Ptr);
      return new FPExtInst(Load, II->getType());
    }
    break;
  case Intrinsic::ppc_qpx_qvlfd:
    // Turn PPC QPX qvlfd -> load if the pointer is known aligned.
    if (getOrEnforceKnownAlignment(II->getArgOperand(0), 32, DL, II, &AC,
                                   &DT) >= 32) {
      Value *Ptr = Builder.CreateBitCast(II->getArgOperand(0),
                                         PointerType::getUnqual(II->getType()));
      return new LoadInst(Ptr);
    }
    break;
  case Intrinsic::ppc_qpx_qvstfs:
    // Turn PPC QPX qvstfs -> store if the pointer is known aligned.
    if (getOrEnforceKnownAlignment(II->getArgOperand(1), 16, DL, II, &AC,
                                   &DT) >= 16) {
      Type *VTy = VectorType::get(Builder.getFloatTy(),
          II->getArgOperand(0)->getType()->getVectorNumElements());
      Value *TOp = Builder.CreateFPTrunc(II->getArgOperand(0), VTy);
      Type *OpPtrTy = PointerType::getUnqual(VTy);
      Value *Ptr = Builder.CreateBitCast(II->getArgOperand(1), OpPtrTy);
      return new StoreInst(TOp, Ptr);
    }
    break;
  case Intrinsic::ppc_qpx_qvstfd:
    // Turn PPC QPX qvstfd -> store if the pointer is known aligned.
    if (getOrEnforceKnownAlignment(II->getArgOperand(1), 32, DL, II, &AC,
                                   &DT) >= 32) {
      Type *OpPtrTy =
        PointerType::getUnqual(II->getArgOperand(0)->getType());
      Value *Ptr = Builder.CreateBitCast(II->getArgOperand(1), OpPtrTy);
      return new StoreInst(II->getArgOperand(0), Ptr);
    }
    break;

  case Intrinsic::x86_vcvtph2ps_128:
  case Intrinsic::x86_vcvtph2ps_256: {
    auto Arg = II->getArgOperand(0);
    auto ArgType = cast<VectorType>(Arg->getType());
    auto RetType = cast<VectorType>(II->getType());
    unsigned ArgWidth = ArgType->getNumElements();
    unsigned RetWidth = RetType->getNumElements();
    assert(RetWidth <= ArgWidth && "Unexpected input/return vector widths");
    assert(ArgType->isIntOrIntVectorTy() &&
           ArgType->getScalarSizeInBits() == 16 &&
           "CVTPH2PS input type should be 16-bit integer vector");
    assert(RetType->getScalarType()->isFloatTy() &&
           "CVTPH2PS output type should be 32-bit float vector");

    // Constant folding: Convert to generic half to single conversion.
    if (isa<ConstantAggregateZero>(Arg))
      return replaceInstUsesWith(*II, ConstantAggregateZero::get(RetType));

    if (isa<ConstantDataVector>(Arg)) {
      auto VectorHalfAsShorts = Arg;
      if (RetWidth < ArgWidth) {
        SmallVector<uint32_t, 8> SubVecMask;
        for (unsigned i = 0; i != RetWidth; ++i)
          SubVecMask.push_back((int)i);
        VectorHalfAsShorts = Builder.CreateShuffleVector(
            Arg, UndefValue::get(ArgType), SubVecMask);
      }

      auto VectorHalfType =
          VectorType::get(Type::getHalfTy(II->getContext()), RetWidth);
      auto VectorHalfs =
          Builder.CreateBitCast(VectorHalfAsShorts, VectorHalfType);
      auto VectorFloats = Builder.CreateFPExt(VectorHalfs, RetType);
      return replaceInstUsesWith(*II, VectorFloats);
    }

    // We only use the lowest lanes of the argument.
    if (Value *V = SimplifyDemandedVectorEltsLow(Arg, ArgWidth, RetWidth)) {
      II->setArgOperand(0, V);
      return II;
    }
    break;
  }

  case Intrinsic::x86_sse_cvtss2si:
  case Intrinsic::x86_sse_cvtss2si64:
  case Intrinsic::x86_sse_cvttss2si:
  case Intrinsic::x86_sse_cvttss2si64:
  case Intrinsic::x86_sse2_cvtsd2si:
  case Intrinsic::x86_sse2_cvtsd2si64:
  case Intrinsic::x86_sse2_cvttsd2si:
  case Intrinsic::x86_sse2_cvttsd2si64:
  case Intrinsic::x86_avx512_vcvtss2si32:
  case Intrinsic::x86_avx512_vcvtss2si64:
  case Intrinsic::x86_avx512_vcvtss2usi32:
  case Intrinsic::x86_avx512_vcvtss2usi64:
  case Intrinsic::x86_avx512_vcvtsd2si32:
  case Intrinsic::x86_avx512_vcvtsd2si64:
  case Intrinsic::x86_avx512_vcvtsd2usi32:
  case Intrinsic::x86_avx512_vcvtsd2usi64:
  case Intrinsic::x86_avx512_cvttss2si:
  case Intrinsic::x86_avx512_cvttss2si64:
  case Intrinsic::x86_avx512_cvttss2usi:
  case Intrinsic::x86_avx512_cvttss2usi64:
  case Intrinsic::x86_avx512_cvttsd2si:
  case Intrinsic::x86_avx512_cvttsd2si64:
  case Intrinsic::x86_avx512_cvttsd2usi:
  case Intrinsic::x86_avx512_cvttsd2usi64: {
    // These intrinsics only demand the 0th element of their input vectors. If
    // we can simplify the input based on that, do so now.
    Value *Arg = II->getArgOperand(0);
    unsigned VWidth = Arg->getType()->getVectorNumElements();
    if (Value *V = SimplifyDemandedVectorEltsLow(Arg, VWidth, 1)) {
      II->setArgOperand(0, V);
      return II;
    }
    break;
  }

  case Intrinsic::x86_mmx_pmovmskb:
  case Intrinsic::x86_sse_movmsk_ps:
  case Intrinsic::x86_sse2_movmsk_pd:
  case Intrinsic::x86_sse2_pmovmskb_128:
  case Intrinsic::x86_avx_movmsk_pd_256:
  case Intrinsic::x86_avx_movmsk_ps_256:
  case Intrinsic::x86_avx2_pmovmskb: {
    if (Value *V = simplifyX86movmsk(*II))
      return replaceInstUsesWith(*II, V);
    break;
  }

  case Intrinsic::x86_sse_comieq_ss:
  case Intrinsic::x86_sse_comige_ss:
  case Intrinsic::x86_sse_comigt_ss:
  case Intrinsic::x86_sse_comile_ss:
  case Intrinsic::x86_sse_comilt_ss:
  case Intrinsic::x86_sse_comineq_ss:
  case Intrinsic::x86_sse_ucomieq_ss:
  case Intrinsic::x86_sse_ucomige_ss:
  case Intrinsic::x86_sse_ucomigt_ss:
  case Intrinsic::x86_sse_ucomile_ss:
  case Intrinsic::x86_sse_ucomilt_ss:
  case Intrinsic::x86_sse_ucomineq_ss:
  case Intrinsic::x86_sse2_comieq_sd:
  case Intrinsic::x86_sse2_comige_sd:
  case Intrinsic::x86_sse2_comigt_sd:
  case Intrinsic::x86_sse2_comile_sd:
  case Intrinsic::x86_sse2_comilt_sd:
  case Intrinsic::x86_sse2_comineq_sd:
  case Intrinsic::x86_sse2_ucomieq_sd:
  case Intrinsic::x86_sse2_ucomige_sd:
  case Intrinsic::x86_sse2_ucomigt_sd:
  case Intrinsic::x86_sse2_ucomile_sd:
  case Intrinsic::x86_sse2_ucomilt_sd:
  case Intrinsic::x86_sse2_ucomineq_sd:
  case Intrinsic::x86_avx512_vcomi_ss:
  case Intrinsic::x86_avx512_vcomi_sd:
  case Intrinsic::x86_avx512_mask_cmp_ss:
  case Intrinsic::x86_avx512_mask_cmp_sd: {
    // These intrinsics only demand the 0th element of their input vectors. If
    // we can simplify the input based on that, do so now.
    bool MadeChange = false;
    Value *Arg0 = II->getArgOperand(0);
    Value *Arg1 = II->getArgOperand(1);
    unsigned VWidth = Arg0->getType()->getVectorNumElements();
    if (Value *V = SimplifyDemandedVectorEltsLow(Arg0, VWidth, 1)) {
      II->setArgOperand(0, V);
      MadeChange = true;
    }
    if (Value *V = SimplifyDemandedVectorEltsLow(Arg1, VWidth, 1)) {
      II->setArgOperand(1, V);
      MadeChange = true;
    }
    if (MadeChange)
      return II;
    break;
  }
  case Intrinsic::x86_avx512_mask_cmp_pd_128:
  case Intrinsic::x86_avx512_mask_cmp_pd_256:
  case Intrinsic::x86_avx512_mask_cmp_pd_512:
  case Intrinsic::x86_avx512_mask_cmp_ps_128:
  case Intrinsic::x86_avx512_mask_cmp_ps_256:
  case Intrinsic::x86_avx512_mask_cmp_ps_512: {
    // Folding cmp(sub(a,b),0) -> cmp(a,b) and cmp(0,sub(a,b)) -> cmp(b,a)
    Value *Arg0 = II->getArgOperand(0);
    Value *Arg1 = II->getArgOperand(1);
    bool Arg0IsZero = match(Arg0, m_Zero());
    if (Arg0IsZero)
      std::swap(Arg0, Arg1);
    Value *A, *B;
    // This fold requires only the NINF(not +/- inf) since inf minus
    // inf is nan.
    // NSZ(No Signed Zeros) is not needed because zeros of any sign are
    // equal for both compares.
    // NNAN is not needed because nans compare the same for both compares.
    // The compare intrinsic uses the above assumptions and therefore
    // doesn't require additional flags.
    if ((match(Arg0, m_OneUse(m_FSub(m_Value(A), m_Value(B)))) &&
         match(Arg1, m_Zero()) &&
         cast<Instruction>(Arg0)->getFastMathFlags().noInfs())) {
      if (Arg0IsZero)
        std::swap(A, B);
      II->setArgOperand(0, A);
      II->setArgOperand(1, B);
      return II;
    }
    break;
  }

  case Intrinsic::x86_avx512_mask_add_ps_512:
  case Intrinsic::x86_avx512_mask_div_ps_512:
  case Intrinsic::x86_avx512_mask_mul_ps_512:
  case Intrinsic::x86_avx512_mask_sub_ps_512:
  case Intrinsic::x86_avx512_mask_add_pd_512:
  case Intrinsic::x86_avx512_mask_div_pd_512:
  case Intrinsic::x86_avx512_mask_mul_pd_512:
  case Intrinsic::x86_avx512_mask_sub_pd_512:
    // If the rounding mode is CUR_DIRECTION(4) we can turn these into regular
    // IR operations.
    if (auto *R = dyn_cast<ConstantInt>(II->getArgOperand(4))) {
      if (R->getValue() == 4) {
        Value *Arg0 = II->getArgOperand(0);
        Value *Arg1 = II->getArgOperand(1);

        Value *V;
        switch (II->getIntrinsicID()) {
        default: llvm_unreachable("Case stmts out of sync!");
        case Intrinsic::x86_avx512_mask_add_ps_512:
        case Intrinsic::x86_avx512_mask_add_pd_512:
          V = Builder.CreateFAdd(Arg0, Arg1);
          break;
        case Intrinsic::x86_avx512_mask_sub_ps_512:
        case Intrinsic::x86_avx512_mask_sub_pd_512:
          V = Builder.CreateFSub(Arg0, Arg1);
          break;
        case Intrinsic::x86_avx512_mask_mul_ps_512:
        case Intrinsic::x86_avx512_mask_mul_pd_512:
          V = Builder.CreateFMul(Arg0, Arg1);
          break;
        case Intrinsic::x86_avx512_mask_div_ps_512:
        case Intrinsic::x86_avx512_mask_div_pd_512:
          V = Builder.CreateFDiv(Arg0, Arg1);
          break;
        }

        // Create a select for the masking.
        V = emitX86MaskSelect(II->getArgOperand(3), V, II->getArgOperand(2),
                              Builder);
        return replaceInstUsesWith(*II, V);
      }
    }
    break;

  case Intrinsic::x86_avx512_mask_add_ss_round:
  case Intrinsic::x86_avx512_mask_div_ss_round:
  case Intrinsic::x86_avx512_mask_mul_ss_round:
  case Intrinsic::x86_avx512_mask_sub_ss_round:
  case Intrinsic::x86_avx512_mask_add_sd_round:
  case Intrinsic::x86_avx512_mask_div_sd_round:
  case Intrinsic::x86_avx512_mask_mul_sd_round:
  case Intrinsic::x86_avx512_mask_sub_sd_round:
    // If the rounding mode is CUR_DIRECTION(4) we can turn these into regular
    // IR operations.
    if (auto *R = dyn_cast<ConstantInt>(II->getArgOperand(4))) {
      if (R->getValue() == 4) {
        // Extract the element as scalars.
        Value *Arg0 = II->getArgOperand(0);
        Value *Arg1 = II->getArgOperand(1);
        Value *LHS = Builder.CreateExtractElement(Arg0, (uint64_t)0);
        Value *RHS = Builder.CreateExtractElement(Arg1, (uint64_t)0);

        Value *V;
        switch (II->getIntrinsicID()) {
        default: llvm_unreachable("Case stmts out of sync!");
        case Intrinsic::x86_avx512_mask_add_ss_round:
        case Intrinsic::x86_avx512_mask_add_sd_round:
          V = Builder.CreateFAdd(LHS, RHS);
          break;
        case Intrinsic::x86_avx512_mask_sub_ss_round:
        case Intrinsic::x86_avx512_mask_sub_sd_round:
          V = Builder.CreateFSub(LHS, RHS);
          break;
        case Intrinsic::x86_avx512_mask_mul_ss_round:
        case Intrinsic::x86_avx512_mask_mul_sd_round:
          V = Builder.CreateFMul(LHS, RHS);
          break;
        case Intrinsic::x86_avx512_mask_div_ss_round:
        case Intrinsic::x86_avx512_mask_div_sd_round:
          V = Builder.CreateFDiv(LHS, RHS);
          break;
        }

        // Handle the masking aspect of the intrinsic.
        Value *Mask = II->getArgOperand(3);
        auto *C = dyn_cast<ConstantInt>(Mask);
        // We don't need a select if we know the mask bit is a 1.
        if (!C || !C->getValue()[0]) {
          // Cast the mask to an i1 vector and then extract the lowest element.
          auto *MaskTy = VectorType::get(Builder.getInt1Ty(),
                             cast<IntegerType>(Mask->getType())->getBitWidth());
          Mask = Builder.CreateBitCast(Mask, MaskTy);
          Mask = Builder.CreateExtractElement(Mask, (uint64_t)0);
          // Extract the lowest element from the passthru operand.
          Value *Passthru = Builder.CreateExtractElement(II->getArgOperand(2),
                                                          (uint64_t)0);
          V = Builder.CreateSelect(Mask, V, Passthru);
        }

        // Insert the result back into the original argument 0.
        V = Builder.CreateInsertElement(Arg0, V, (uint64_t)0);

        return replaceInstUsesWith(*II, V);
      }
    }
    LLVM_FALLTHROUGH;

  // X86 scalar intrinsics simplified with SimplifyDemandedVectorElts.
  case Intrinsic::x86_avx512_mask_max_ss_round:
  case Intrinsic::x86_avx512_mask_min_ss_round:
  case Intrinsic::x86_avx512_mask_max_sd_round:
  case Intrinsic::x86_avx512_mask_min_sd_round:
  case Intrinsic::x86_avx512_mask_vfmadd_ss:
  case Intrinsic::x86_avx512_mask_vfmadd_sd:
  case Intrinsic::x86_avx512_maskz_vfmadd_ss:
  case Intrinsic::x86_avx512_maskz_vfmadd_sd:
  case Intrinsic::x86_avx512_mask3_vfmadd_ss:
  case Intrinsic::x86_avx512_mask3_vfmadd_sd:
  case Intrinsic::x86_avx512_mask3_vfmsub_ss:
  case Intrinsic::x86_avx512_mask3_vfmsub_sd:
  case Intrinsic::x86_avx512_mask3_vfnmsub_ss:
  case Intrinsic::x86_avx512_mask3_vfnmsub_sd:
  case Intrinsic::x86_fma_vfmadd_ss:
  case Intrinsic::x86_fma_vfmsub_ss:
  case Intrinsic::x86_fma_vfnmadd_ss:
  case Intrinsic::x86_fma_vfnmsub_ss:
  case Intrinsic::x86_fma_vfmadd_sd:
  case Intrinsic::x86_fma_vfmsub_sd:
  case Intrinsic::x86_fma_vfnmadd_sd:
  case Intrinsic::x86_fma_vfnmsub_sd:
  case Intrinsic::x86_sse_cmp_ss:
  case Intrinsic::x86_sse_min_ss:
  case Intrinsic::x86_sse_max_ss:
  case Intrinsic::x86_sse2_cmp_sd:
  case Intrinsic::x86_sse2_min_sd:
  case Intrinsic::x86_sse2_max_sd:
  case Intrinsic::x86_sse41_round_ss:
  case Intrinsic::x86_sse41_round_sd:
  case Intrinsic::x86_xop_vfrcz_ss:
  case Intrinsic::x86_xop_vfrcz_sd: {
   unsigned VWidth = II->getType()->getVectorNumElements();
   APInt UndefElts(VWidth, 0);
   APInt AllOnesEltMask(APInt::getAllOnesValue(VWidth));
   if (Value *V = SimplifyDemandedVectorElts(II, AllOnesEltMask, UndefElts)) {
     if (V != II)
       return replaceInstUsesWith(*II, V);
     return II;
   }
   break;
  }

  // Constant fold ashr( <A x Bi>, Ci ).
  // Constant fold lshr( <A x Bi>, Ci ).
  // Constant fold shl( <A x Bi>, Ci ).
  case Intrinsic::x86_sse2_psrai_d:
  case Intrinsic::x86_sse2_psrai_w:
  case Intrinsic::x86_avx2_psrai_d:
  case Intrinsic::x86_avx2_psrai_w:
  case Intrinsic::x86_avx512_psrai_q_128:
  case Intrinsic::x86_avx512_psrai_q_256:
  case Intrinsic::x86_avx512_psrai_d_512:
  case Intrinsic::x86_avx512_psrai_q_512:
  case Intrinsic::x86_avx512_psrai_w_512:
  case Intrinsic::x86_sse2_psrli_d:
  case Intrinsic::x86_sse2_psrli_q:
  case Intrinsic::x86_sse2_psrli_w:
  case Intrinsic::x86_avx2_psrli_d:
  case Intrinsic::x86_avx2_psrli_q:
  case Intrinsic::x86_avx2_psrli_w:
  case Intrinsic::x86_avx512_psrli_d_512:
  case Intrinsic::x86_avx512_psrli_q_512:
  case Intrinsic::x86_avx512_psrli_w_512:
  case Intrinsic::x86_sse2_pslli_d:
  case Intrinsic::x86_sse2_pslli_q:
  case Intrinsic::x86_sse2_pslli_w:
  case Intrinsic::x86_avx2_pslli_d:
  case Intrinsic::x86_avx2_pslli_q:
  case Intrinsic::x86_avx2_pslli_w:
  case Intrinsic::x86_avx512_pslli_d_512:
  case Intrinsic::x86_avx512_pslli_q_512:
  case Intrinsic::x86_avx512_pslli_w_512:
    if (Value *V = simplifyX86immShift(*II, Builder))
      return replaceInstUsesWith(*II, V);
    break;

  case Intrinsic::x86_sse2_psra_d:
  case Intrinsic::x86_sse2_psra_w:
  case Intrinsic::x86_avx2_psra_d:
  case Intrinsic::x86_avx2_psra_w:
  case Intrinsic::x86_avx512_psra_q_128:
  case Intrinsic::x86_avx512_psra_q_256:
  case Intrinsic::x86_avx512_psra_d_512:
  case Intrinsic::x86_avx512_psra_q_512:
  case Intrinsic::x86_avx512_psra_w_512:
  case Intrinsic::x86_sse2_psrl_d:
  case Intrinsic::x86_sse2_psrl_q:
  case Intrinsic::x86_sse2_psrl_w:
  case Intrinsic::x86_avx2_psrl_d:
  case Intrinsic::x86_avx2_psrl_q:
  case Intrinsic::x86_avx2_psrl_w:
  case Intrinsic::x86_avx512_psrl_d_512:
  case Intrinsic::x86_avx512_psrl_q_512:
  case Intrinsic::x86_avx512_psrl_w_512:
  case Intrinsic::x86_sse2_psll_d:
  case Intrinsic::x86_sse2_psll_q:
  case Intrinsic::x86_sse2_psll_w:
  case Intrinsic::x86_avx2_psll_d:
  case Intrinsic::x86_avx2_psll_q:
  case Intrinsic::x86_avx2_psll_w:
  case Intrinsic::x86_avx512_psll_d_512:
  case Intrinsic::x86_avx512_psll_q_512:
  case Intrinsic::x86_avx512_psll_w_512: {
    if (Value *V = simplifyX86immShift(*II, Builder))
      return replaceInstUsesWith(*II, V);

    // SSE2/AVX2 uses only the first 64-bits of the 128-bit vector
    // operand to compute the shift amount.
    Value *Arg1 = II->getArgOperand(1);
    assert(Arg1->getType()->getPrimitiveSizeInBits() == 128 &&
           "Unexpected packed shift size");
    unsigned VWidth = Arg1->getType()->getVectorNumElements();

    if (Value *V = SimplifyDemandedVectorEltsLow(Arg1, VWidth, VWidth / 2)) {
      II->setArgOperand(1, V);
      return II;
    }
    break;
  }

  case Intrinsic::x86_avx2_psllv_d:
  case Intrinsic::x86_avx2_psllv_d_256:
  case Intrinsic::x86_avx2_psllv_q:
  case Intrinsic::x86_avx2_psllv_q_256:
  case Intrinsic::x86_avx512_psllv_d_512:
  case Intrinsic::x86_avx512_psllv_q_512:
  case Intrinsic::x86_avx512_psllv_w_128:
  case Intrinsic::x86_avx512_psllv_w_256:
  case Intrinsic::x86_avx512_psllv_w_512:
  case Intrinsic::x86_avx2_psrav_d:
  case Intrinsic::x86_avx2_psrav_d_256:
  case Intrinsic::x86_avx512_psrav_q_128:
  case Intrinsic::x86_avx512_psrav_q_256:
  case Intrinsic::x86_avx512_psrav_d_512:
  case Intrinsic::x86_avx512_psrav_q_512:
  case Intrinsic::x86_avx512_psrav_w_128:
  case Intrinsic::x86_avx512_psrav_w_256:
  case Intrinsic::x86_avx512_psrav_w_512:
  case Intrinsic::x86_avx2_psrlv_d:
  case Intrinsic::x86_avx2_psrlv_d_256:
  case Intrinsic::x86_avx2_psrlv_q:
  case Intrinsic::x86_avx2_psrlv_q_256:
  case Intrinsic::x86_avx512_psrlv_d_512:
  case Intrinsic::x86_avx512_psrlv_q_512:
  case Intrinsic::x86_avx512_psrlv_w_128:
  case Intrinsic::x86_avx512_psrlv_w_256:
  case Intrinsic::x86_avx512_psrlv_w_512:
    if (Value *V = simplifyX86varShift(*II, Builder))
      return replaceInstUsesWith(*II, V);
    break;

  case Intrinsic::x86_sse2_pmulu_dq:
  case Intrinsic::x86_sse41_pmuldq:
  case Intrinsic::x86_avx2_pmul_dq:
  case Intrinsic::x86_avx2_pmulu_dq:
  case Intrinsic::x86_avx512_pmul_dq_512:
  case Intrinsic::x86_avx512_pmulu_dq_512: {
    if (Value *V = simplifyX86muldq(*II, Builder))
      return replaceInstUsesWith(*II, V);

    unsigned VWidth = II->getType()->getVectorNumElements();
    APInt UndefElts(VWidth, 0);
    APInt DemandedElts = APInt::getAllOnesValue(VWidth);
    if (Value *V = SimplifyDemandedVectorElts(II, DemandedElts, UndefElts)) {
      if (V != II)
        return replaceInstUsesWith(*II, V);
      return II;
    }
    break;
  }

  case Intrinsic::x86_sse2_packssdw_128:
  case Intrinsic::x86_sse2_packsswb_128:
  case Intrinsic::x86_avx2_packssdw:
  case Intrinsic::x86_avx2_packsswb:
  case Intrinsic::x86_avx512_packssdw_512:
  case Intrinsic::x86_avx512_packsswb_512:
    if (Value *V = simplifyX86pack(*II, true))
      return replaceInstUsesWith(*II, V);
    break;

  case Intrinsic::x86_sse2_packuswb_128:
  case Intrinsic::x86_sse41_packusdw:
  case Intrinsic::x86_avx2_packusdw:
  case Intrinsic::x86_avx2_packuswb:
  case Intrinsic::x86_avx512_packusdw_512:
  case Intrinsic::x86_avx512_packuswb_512:
    if (Value *V = simplifyX86pack(*II, false))
      return replaceInstUsesWith(*II, V);
    break;

  case Intrinsic::x86_pclmulqdq: {
    if (auto *C = dyn_cast<ConstantInt>(II->getArgOperand(2))) {
      unsigned Imm = C->getZExtValue();

      bool MadeChange = false;
      Value *Arg0 = II->getArgOperand(0);
      Value *Arg1 = II->getArgOperand(1);
      unsigned VWidth = Arg0->getType()->getVectorNumElements();
      APInt DemandedElts(VWidth, 0);

      APInt UndefElts1(VWidth, 0);
      DemandedElts = (Imm & 0x01) ? 2 : 1;
      if (Value *V = SimplifyDemandedVectorElts(Arg0, DemandedElts,
                                                UndefElts1)) {
        II->setArgOperand(0, V);
        MadeChange = true;
      }

      APInt UndefElts2(VWidth, 0);
      DemandedElts = (Imm & 0x10) ? 2 : 1;
      if (Value *V = SimplifyDemandedVectorElts(Arg1, DemandedElts,
                                                UndefElts2)) {
        II->setArgOperand(1, V);
        MadeChange = true;
      }

      // If both input elements are undef, the result is undef.
      if (UndefElts1[(Imm & 0x01) ? 1 : 0] ||
          UndefElts2[(Imm & 0x10) ? 1 : 0])
        return replaceInstUsesWith(*II,
                                   ConstantAggregateZero::get(II->getType()));

      if (MadeChange)
        return II;
    }
    break;
  }

  case Intrinsic::x86_sse41_insertps:
    if (Value *V = simplifyX86insertps(*II, Builder))
      return replaceInstUsesWith(*II, V);
    break;

  case Intrinsic::x86_sse4a_extrq: {
    Value *Op0 = II->getArgOperand(0);
    Value *Op1 = II->getArgOperand(1);
    unsigned VWidth0 = Op0->getType()->getVectorNumElements();
    unsigned VWidth1 = Op1->getType()->getVectorNumElements();
    assert(Op0->getType()->getPrimitiveSizeInBits() == 128 &&
           Op1->getType()->getPrimitiveSizeInBits() == 128 && VWidth0 == 2 &&
           VWidth1 == 16 && "Unexpected operand sizes");

    // See if we're dealing with constant values.
    Constant *C1 = dyn_cast<Constant>(Op1);
    ConstantInt *CILength =
        C1 ? dyn_cast_or_null<ConstantInt>(C1->getAggregateElement((unsigned)0))
           : nullptr;
    ConstantInt *CIIndex =
        C1 ? dyn_cast_or_null<ConstantInt>(C1->getAggregateElement((unsigned)1))
           : nullptr;

    // Attempt to simplify to a constant, shuffle vector or EXTRQI call.
    if (Value *V = simplifyX86extrq(*II, Op0, CILength, CIIndex, Builder))
      return replaceInstUsesWith(*II, V);

    // EXTRQ only uses the lowest 64-bits of the first 128-bit vector
    // operands and the lowest 16-bits of the second.
    bool MadeChange = false;
    if (Value *V = SimplifyDemandedVectorEltsLow(Op0, VWidth0, 1)) {
      II->setArgOperand(0, V);
      MadeChange = true;
    }
    if (Value *V = SimplifyDemandedVectorEltsLow(Op1, VWidth1, 2)) {
      II->setArgOperand(1, V);
      MadeChange = true;
    }
    if (MadeChange)
      return II;
    break;
  }

  case Intrinsic::x86_sse4a_extrqi: {
    // EXTRQI: Extract Length bits starting from Index. Zero pad the remaining
    // bits of the lower 64-bits. The upper 64-bits are undefined.
    Value *Op0 = II->getArgOperand(0);
    unsigned VWidth = Op0->getType()->getVectorNumElements();
    assert(Op0->getType()->getPrimitiveSizeInBits() == 128 && VWidth == 2 &&
           "Unexpected operand size");

    // See if we're dealing with constant values.
    ConstantInt *CILength = dyn_cast<ConstantInt>(II->getArgOperand(1));
    ConstantInt *CIIndex = dyn_cast<ConstantInt>(II->getArgOperand(2));

    // Attempt to simplify to a constant or shuffle vector.
    if (Value *V = simplifyX86extrq(*II, Op0, CILength, CIIndex, Builder))
      return replaceInstUsesWith(*II, V);

    // EXTRQI only uses the lowest 64-bits of the first 128-bit vector
    // operand.
    if (Value *V = SimplifyDemandedVectorEltsLow(Op0, VWidth, 1)) {
      II->setArgOperand(0, V);
      return II;
    }
    break;
  }

  case Intrinsic::x86_sse4a_insertq: {
    Value *Op0 = II->getArgOperand(0);
    Value *Op1 = II->getArgOperand(1);
    unsigned VWidth = Op0->getType()->getVectorNumElements();
    assert(Op0->getType()->getPrimitiveSizeInBits() == 128 &&
           Op1->getType()->getPrimitiveSizeInBits() == 128 && VWidth == 2 &&
           Op1->getType()->getVectorNumElements() == 2 &&
           "Unexpected operand size");

    // See if we're dealing with constant values.
    Constant *C1 = dyn_cast<Constant>(Op1);
    ConstantInt *CI11 =
        C1 ? dyn_cast_or_null<ConstantInt>(C1->getAggregateElement((unsigned)1))
           : nullptr;

    // Attempt to simplify to a constant, shuffle vector or INSERTQI call.
    if (CI11) {
      const APInt &V11 = CI11->getValue();
      APInt Len = V11.zextOrTrunc(6);
      APInt Idx = V11.lshr(8).zextOrTrunc(6);
      if (Value *V = simplifyX86insertq(*II, Op0, Op1, Len, Idx, Builder))
        return replaceInstUsesWith(*II, V);
    }

    // INSERTQ only uses the lowest 64-bits of the first 128-bit vector
    // operand.
    if (Value *V = SimplifyDemandedVectorEltsLow(Op0, VWidth, 1)) {
      II->setArgOperand(0, V);
      return II;
    }
    break;
  }

  case Intrinsic::x86_sse4a_insertqi: {
    // INSERTQI: Extract lowest Length bits from lower half of second source and
    // insert over first source starting at Index bit. The upper 64-bits are
    // undefined.
    Value *Op0 = II->getArgOperand(0);
    Value *Op1 = II->getArgOperand(1);
    unsigned VWidth0 = Op0->getType()->getVectorNumElements();
    unsigned VWidth1 = Op1->getType()->getVectorNumElements();
    assert(Op0->getType()->getPrimitiveSizeInBits() == 128 &&
           Op1->getType()->getPrimitiveSizeInBits() == 128 && VWidth0 == 2 &&
           VWidth1 == 2 && "Unexpected operand sizes");

    // See if we're dealing with constant values.
    ConstantInt *CILength = dyn_cast<ConstantInt>(II->getArgOperand(2));
    ConstantInt *CIIndex = dyn_cast<ConstantInt>(II->getArgOperand(3));

    // Attempt to simplify to a constant or shuffle vector.
    if (CILength && CIIndex) {
      APInt Len = CILength->getValue().zextOrTrunc(6);
      APInt Idx = CIIndex->getValue().zextOrTrunc(6);
      if (Value *V = simplifyX86insertq(*II, Op0, Op1, Len, Idx, Builder))
        return replaceInstUsesWith(*II, V);
    }

    // INSERTQI only uses the lowest 64-bits of the first two 128-bit vector
    // operands.
    bool MadeChange = false;
    if (Value *V = SimplifyDemandedVectorEltsLow(Op0, VWidth0, 1)) {
      II->setArgOperand(0, V);
      MadeChange = true;
    }
    if (Value *V = SimplifyDemandedVectorEltsLow(Op1, VWidth1, 1)) {
      II->setArgOperand(1, V);
      MadeChange = true;
    }
    if (MadeChange)
      return II;
    break;
  }

  case Intrinsic::x86_sse41_pblendvb:
  case Intrinsic::x86_sse41_blendvps:
  case Intrinsic::x86_sse41_blendvpd:
  case Intrinsic::x86_avx_blendv_ps_256:
  case Intrinsic::x86_avx_blendv_pd_256:
  case Intrinsic::x86_avx2_pblendvb: {
    // Convert blendv* to vector selects if the mask is constant.
    // This optimization is convoluted because the intrinsic is defined as
    // getting a vector of floats or doubles for the ps and pd versions.
    // FIXME: That should be changed.

    Value *Op0 = II->getArgOperand(0);
    Value *Op1 = II->getArgOperand(1);
    Value *Mask = II->getArgOperand(2);

    // fold (blend A, A, Mask) -> A
    if (Op0 == Op1)
      return replaceInstUsesWith(CI, Op0);

    // Zero Mask - select 1st argument.
    if (isa<ConstantAggregateZero>(Mask))
      return replaceInstUsesWith(CI, Op0);

    // Constant Mask - select 1st/2nd argument lane based on top bit of mask.
    if (auto *ConstantMask = dyn_cast<ConstantDataVector>(Mask)) {
      Constant *NewSelector = getNegativeIsTrueBoolVec(ConstantMask);
      return SelectInst::Create(NewSelector, Op1, Op0, "blendv");
    }
    break;
  }

  case Intrinsic::x86_ssse3_pshuf_b_128:
  case Intrinsic::x86_avx2_pshuf_b:
  case Intrinsic::x86_avx512_pshuf_b_512:
    if (Value *V = simplifyX86pshufb(*II, Builder))
      return replaceInstUsesWith(*II, V);
    break;

  case Intrinsic::x86_avx_vpermilvar_ps:
  case Intrinsic::x86_avx_vpermilvar_ps_256:
  case Intrinsic::x86_avx512_vpermilvar_ps_512:
  case Intrinsic::x86_avx_vpermilvar_pd:
  case Intrinsic::x86_avx_vpermilvar_pd_256:
  case Intrinsic::x86_avx512_vpermilvar_pd_512:
    if (Value *V = simplifyX86vpermilvar(*II, Builder))
      return replaceInstUsesWith(*II, V);
    break;

  case Intrinsic::x86_avx2_permd:
  case Intrinsic::x86_avx2_permps:
    if (Value *V = simplifyX86vpermv(*II, Builder))
      return replaceInstUsesWith(*II, V);
    break;

  case Intrinsic::x86_avx512_mask_permvar_df_256:
  case Intrinsic::x86_avx512_mask_permvar_df_512:
  case Intrinsic::x86_avx512_mask_permvar_di_256:
  case Intrinsic::x86_avx512_mask_permvar_di_512:
  case Intrinsic::x86_avx512_mask_permvar_hi_128:
  case Intrinsic::x86_avx512_mask_permvar_hi_256:
  case Intrinsic::x86_avx512_mask_permvar_hi_512:
  case Intrinsic::x86_avx512_mask_permvar_qi_128:
  case Intrinsic::x86_avx512_mask_permvar_qi_256:
  case Intrinsic::x86_avx512_mask_permvar_qi_512:
  case Intrinsic::x86_avx512_mask_permvar_sf_256:
  case Intrinsic::x86_avx512_mask_permvar_sf_512:
  case Intrinsic::x86_avx512_mask_permvar_si_256:
  case Intrinsic::x86_avx512_mask_permvar_si_512:
    if (Value *V = simplifyX86vpermv(*II, Builder)) {
      // We simplified the permuting, now create a select for the masking.
      V = emitX86MaskSelect(II->getArgOperand(3), V, II->getArgOperand(2),
                            Builder);
      return replaceInstUsesWith(*II, V);
    }
    break;

  case Intrinsic::x86_avx_vperm2f128_pd_256:
  case Intrinsic::x86_avx_vperm2f128_ps_256:
  case Intrinsic::x86_avx_vperm2f128_si_256:
  case Intrinsic::x86_avx2_vperm2i128:
    if (Value *V = simplifyX86vperm2(*II, Builder))
      return replaceInstUsesWith(*II, V);
    break;

  case Intrinsic::x86_avx_maskload_ps:
  case Intrinsic::x86_avx_maskload_pd:
  case Intrinsic::x86_avx_maskload_ps_256:
  case Intrinsic::x86_avx_maskload_pd_256:
  case Intrinsic::x86_avx2_maskload_d:
  case Intrinsic::x86_avx2_maskload_q:
  case Intrinsic::x86_avx2_maskload_d_256:
  case Intrinsic::x86_avx2_maskload_q_256:
    if (Instruction *I = simplifyX86MaskedLoad(*II, *this))
      return I;
    break;

  case Intrinsic::x86_sse2_maskmov_dqu:
  case Intrinsic::x86_avx_maskstore_ps:
  case Intrinsic::x86_avx_maskstore_pd:
  case Intrinsic::x86_avx_maskstore_ps_256:
  case Intrinsic::x86_avx_maskstore_pd_256:
  case Intrinsic::x86_avx2_maskstore_d:
  case Intrinsic::x86_avx2_maskstore_q:
  case Intrinsic::x86_avx2_maskstore_d_256:
  case Intrinsic::x86_avx2_maskstore_q_256:
    if (simplifyX86MaskedStore(*II, *this))
      return nullptr;
    break;

  case Intrinsic::x86_xop_vpcomb:
  case Intrinsic::x86_xop_vpcomd:
  case Intrinsic::x86_xop_vpcomq:
  case Intrinsic::x86_xop_vpcomw:
    if (Value *V = simplifyX86vpcom(*II, Builder, true))
      return replaceInstUsesWith(*II, V);
    break;

  case Intrinsic::x86_xop_vpcomub:
  case Intrinsic::x86_xop_vpcomud:
  case Intrinsic::x86_xop_vpcomuq:
  case Intrinsic::x86_xop_vpcomuw:
    if (Value *V = simplifyX86vpcom(*II, Builder, false))
      return replaceInstUsesWith(*II, V);
    break;

  case Intrinsic::ppc_altivec_vperm:
    // Turn vperm(V1,V2,mask) -> shuffle(V1,V2,mask) if mask is a constant.
    // Note that ppc_altivec_vperm has a big-endian bias, so when creating
    // a vectorshuffle for little endian, we must undo the transformation
    // performed on vec_perm in altivec.h.  That is, we must complement
    // the permutation mask with respect to 31 and reverse the order of
    // V1 and V2.
    if (Constant *Mask = dyn_cast<Constant>(II->getArgOperand(2))) {
      assert(Mask->getType()->getVectorNumElements() == 16 &&
             "Bad type for intrinsic!");

      // Check that all of the elements are integer constants or undefs.
      bool AllEltsOk = true;
      for (unsigned i = 0; i != 16; ++i) {
        Constant *Elt = Mask->getAggregateElement(i);
        if (!Elt || !(isa<ConstantInt>(Elt) || isa<UndefValue>(Elt))) {
          AllEltsOk = false;
          break;
        }
      }

      if (AllEltsOk) {
        // Cast the input vectors to byte vectors.
        Value *Op0 = Builder.CreateBitCast(II->getArgOperand(0),
                                           Mask->getType());
        Value *Op1 = Builder.CreateBitCast(II->getArgOperand(1),
                                           Mask->getType());
        Value *Result = UndefValue::get(Op0->getType());

        // Only extract each element once.
        Value *ExtractedElts[32];
        memset(ExtractedElts, 0, sizeof(ExtractedElts));

        for (unsigned i = 0; i != 16; ++i) {
          if (isa<UndefValue>(Mask->getAggregateElement(i)))
            continue;
          unsigned Idx =
            cast<ConstantInt>(Mask->getAggregateElement(i))->getZExtValue();
          Idx &= 31;  // Match the hardware behavior.
          if (DL.isLittleEndian())
            Idx = 31 - Idx;

          if (!ExtractedElts[Idx]) {
            Value *Op0ToUse = (DL.isLittleEndian()) ? Op1 : Op0;
            Value *Op1ToUse = (DL.isLittleEndian()) ? Op0 : Op1;
            ExtractedElts[Idx] =
              Builder.CreateExtractElement(Idx < 16 ? Op0ToUse : Op1ToUse,
                                           Builder.getInt32(Idx&15));
          }

          // Insert this value into the result vector.
          Result = Builder.CreateInsertElement(Result, ExtractedElts[Idx],
                                               Builder.getInt32(i));
        }
        return CastInst::Create(Instruction::BitCast, Result, CI.getType());
      }
    }
    break;

  case Intrinsic::arm_neon_vld1:
  case Intrinsic::arm_neon_vld2:
  case Intrinsic::arm_neon_vld3:
  case Intrinsic::arm_neon_vld4:
  case Intrinsic::arm_neon_vld2lane:
  case Intrinsic::arm_neon_vld3lane:
  case Intrinsic::arm_neon_vld4lane:
  case Intrinsic::arm_neon_vst1:
  case Intrinsic::arm_neon_vst2:
  case Intrinsic::arm_neon_vst3:
  case Intrinsic::arm_neon_vst4:
  case Intrinsic::arm_neon_vst2lane:
  case Intrinsic::arm_neon_vst3lane:
  case Intrinsic::arm_neon_vst4lane: {
    unsigned MemAlign =
        getKnownAlignment(II->getArgOperand(0), DL, II, &AC, &DT);
    unsigned AlignArg = II->getNumArgOperands() - 1;
    ConstantInt *IntrAlign = dyn_cast<ConstantInt>(II->getArgOperand(AlignArg));
    if (IntrAlign && IntrAlign->getZExtValue() < MemAlign) {
      II->setArgOperand(AlignArg,
                        ConstantInt::get(Type::getInt32Ty(II->getContext()),
                                         MemAlign, false));
      return II;
    }
    break;
  }

  case Intrinsic::arm_neon_vmulls:
  case Intrinsic::arm_neon_vmullu:
  case Intrinsic::aarch64_neon_smull:
  case Intrinsic::aarch64_neon_umull: {
    Value *Arg0 = II->getArgOperand(0);
    Value *Arg1 = II->getArgOperand(1);

    // Handle mul by zero first:
    if (isa<ConstantAggregateZero>(Arg0) || isa<ConstantAggregateZero>(Arg1)) {
      return replaceInstUsesWith(CI, ConstantAggregateZero::get(II->getType()));
    }

    // Check for constant LHS & RHS - in this case we just simplify.
    bool Zext = (II->getIntrinsicID() == Intrinsic::arm_neon_vmullu ||
                 II->getIntrinsicID() == Intrinsic::aarch64_neon_umull);
    VectorType *NewVT = cast<VectorType>(II->getType());
    if (Constant *CV0 = dyn_cast<Constant>(Arg0)) {
      if (Constant *CV1 = dyn_cast<Constant>(Arg1)) {
        CV0 = ConstantExpr::getIntegerCast(CV0, NewVT, /*isSigned=*/!Zext);
        CV1 = ConstantExpr::getIntegerCast(CV1, NewVT, /*isSigned=*/!Zext);

        return replaceInstUsesWith(CI, ConstantExpr::getMul(CV0, CV1));
      }

      // Couldn't simplify - canonicalize constant to the RHS.
      std::swap(Arg0, Arg1);
    }

    // Handle mul by one:
    if (Constant *CV1 = dyn_cast<Constant>(Arg1))
      if (ConstantInt *Splat =
              dyn_cast_or_null<ConstantInt>(CV1->getSplatValue()))
        if (Splat->isOne())
          return CastInst::CreateIntegerCast(Arg0, II->getType(),
                                             /*isSigned=*/!Zext);

    break;
  }
  case Intrinsic::amdgcn_rcp: {
    Value *Src = II->getArgOperand(0);

    // TODO: Move to ConstantFolding/InstSimplify?
    if (isa<UndefValue>(Src))
      return replaceInstUsesWith(CI, Src);

    if (const ConstantFP *C = dyn_cast<ConstantFP>(Src)) {
      const APFloat &ArgVal = C->getValueAPF();
      APFloat Val(ArgVal.getSemantics(), 1.0);
      APFloat::opStatus Status = Val.divide(ArgVal,
                                            APFloat::rmNearestTiesToEven);
      // Only do this if it was exact and therefore not dependent on the
      // rounding mode.
      if (Status == APFloat::opOK)
        return replaceInstUsesWith(CI, ConstantFP::get(II->getContext(), Val));
    }

    break;
  }
  case Intrinsic::amdgcn_rsq: {
    Value *Src = II->getArgOperand(0);

    // TODO: Move to ConstantFolding/InstSimplify?
    if (isa<UndefValue>(Src))
      return replaceInstUsesWith(CI, Src);
    break;
  }
  case Intrinsic::amdgcn_frexp_mant:
  case Intrinsic::amdgcn_frexp_exp: {
    Value *Src = II->getArgOperand(0);
    if (const ConstantFP *C = dyn_cast<ConstantFP>(Src)) {
      int Exp;
      APFloat Significand = frexp(C->getValueAPF(), Exp,
                                  APFloat::rmNearestTiesToEven);

      if (II->getIntrinsicID() == Intrinsic::amdgcn_frexp_mant) {
        return replaceInstUsesWith(CI, ConstantFP::get(II->getContext(),
                                                       Significand));
      }

      // Match instruction special case behavior.
      if (Exp == APFloat::IEK_NaN || Exp == APFloat::IEK_Inf)
        Exp = 0;

      return replaceInstUsesWith(CI, ConstantInt::get(II->getType(), Exp));
    }

    if (isa<UndefValue>(Src))
      return replaceInstUsesWith(CI, UndefValue::get(II->getType()));

    break;
  }
  case Intrinsic::amdgcn_class: {
    enum  {
      S_NAN = 1 << 0,        // Signaling NaN
      Q_NAN = 1 << 1,        // Quiet NaN
      N_INFINITY = 1 << 2,   // Negative infinity
      N_NORMAL = 1 << 3,     // Negative normal
      N_SUBNORMAL = 1 << 4,  // Negative subnormal
      N_ZERO = 1 << 5,       // Negative zero
      P_ZERO = 1 << 6,       // Positive zero
      P_SUBNORMAL = 1 << 7,  // Positive subnormal
      P_NORMAL = 1 << 8,     // Positive normal
      P_INFINITY = 1 << 9    // Positive infinity
    };

    const uint32_t FullMask = S_NAN | Q_NAN | N_INFINITY | N_NORMAL |
      N_SUBNORMAL | N_ZERO | P_ZERO | P_SUBNORMAL | P_NORMAL | P_INFINITY;

    Value *Src0 = II->getArgOperand(0);
    Value *Src1 = II->getArgOperand(1);
    const ConstantInt *CMask = dyn_cast<ConstantInt>(Src1);
    if (!CMask) {
      if (isa<UndefValue>(Src0))
        return replaceInstUsesWith(*II, UndefValue::get(II->getType()));

      if (isa<UndefValue>(Src1))
        return replaceInstUsesWith(*II, ConstantInt::get(II->getType(), false));
      break;
    }

    uint32_t Mask = CMask->getZExtValue();

    // If all tests are made, it doesn't matter what the value is.
    if ((Mask & FullMask) == FullMask)
      return replaceInstUsesWith(*II, ConstantInt::get(II->getType(), true));

    if ((Mask & FullMask) == 0)
      return replaceInstUsesWith(*II, ConstantInt::get(II->getType(), false));

    if (Mask == (S_NAN | Q_NAN)) {
      // Equivalent of isnan. Replace with standard fcmp.
      Value *FCmp = Builder.CreateFCmpUNO(Src0, Src0);
      FCmp->takeName(II);
      return replaceInstUsesWith(*II, FCmp);
    }

    const ConstantFP *CVal = dyn_cast<ConstantFP>(Src0);
    if (!CVal) {
      if (isa<UndefValue>(Src0))
        return replaceInstUsesWith(*II, UndefValue::get(II->getType()));

      // Clamp mask to used bits
      if ((Mask & FullMask) != Mask) {
        CallInst *NewCall = Builder.CreateCall(II->getCalledFunction(),
          { Src0, ConstantInt::get(Src1->getType(), Mask & FullMask) }
        );

        NewCall->takeName(II);
        return replaceInstUsesWith(*II, NewCall);
      }

      break;
    }

    const APFloat &Val = CVal->getValueAPF();

    bool Result =
      ((Mask & S_NAN) && Val.isNaN() && Val.isSignaling()) ||
      ((Mask & Q_NAN) && Val.isNaN() && !Val.isSignaling()) ||
      ((Mask & N_INFINITY) && Val.isInfinity() && Val.isNegative()) ||
      ((Mask & N_NORMAL) && Val.isNormal() && Val.isNegative()) ||
      ((Mask & N_SUBNORMAL) && Val.isDenormal() && Val.isNegative()) ||
      ((Mask & N_ZERO) && Val.isZero() && Val.isNegative()) ||
      ((Mask & P_ZERO) && Val.isZero() && !Val.isNegative()) ||
      ((Mask & P_SUBNORMAL) && Val.isDenormal() && !Val.isNegative()) ||
      ((Mask & P_NORMAL) && Val.isNormal() && !Val.isNegative()) ||
      ((Mask & P_INFINITY) && Val.isInfinity() && !Val.isNegative());

    return replaceInstUsesWith(*II, ConstantInt::get(II->getType(), Result));
  }
  case Intrinsic::amdgcn_cvt_pkrtz: {
    Value *Src0 = II->getArgOperand(0);
    Value *Src1 = II->getArgOperand(1);
    if (const ConstantFP *C0 = dyn_cast<ConstantFP>(Src0)) {
      if (const ConstantFP *C1 = dyn_cast<ConstantFP>(Src1)) {
        const fltSemantics &HalfSem
          = II->getType()->getScalarType()->getFltSemantics();
        bool LosesInfo;
        APFloat Val0 = C0->getValueAPF();
        APFloat Val1 = C1->getValueAPF();
        Val0.convert(HalfSem, APFloat::rmTowardZero, &LosesInfo);
        Val1.convert(HalfSem, APFloat::rmTowardZero, &LosesInfo);

        Constant *Folded = ConstantVector::get({
            ConstantFP::get(II->getContext(), Val0),
            ConstantFP::get(II->getContext(), Val1) });
        return replaceInstUsesWith(*II, Folded);
      }
    }

    if (isa<UndefValue>(Src0) && isa<UndefValue>(Src1))
      return replaceInstUsesWith(*II, UndefValue::get(II->getType()));

    break;
  }
  case Intrinsic::amdgcn_ubfe:
  case Intrinsic::amdgcn_sbfe: {
    // Decompose simple cases into standard shifts.
    Value *Src = II->getArgOperand(0);
    if (isa<UndefValue>(Src))
      return replaceInstUsesWith(*II, Src);

    unsigned Width;
    Type *Ty = II->getType();
    unsigned IntSize = Ty->getIntegerBitWidth();

    ConstantInt *CWidth = dyn_cast<ConstantInt>(II->getArgOperand(2));
    if (CWidth) {
      Width = CWidth->getZExtValue();
      if ((Width & (IntSize - 1)) == 0)
        return replaceInstUsesWith(*II, ConstantInt::getNullValue(Ty));

      if (Width >= IntSize) {
        // Hardware ignores high bits, so remove those.
        II->setArgOperand(2, ConstantInt::get(CWidth->getType(),
                                              Width & (IntSize - 1)));
        return II;
      }
    }

    unsigned Offset;
    ConstantInt *COffset = dyn_cast<ConstantInt>(II->getArgOperand(1));
    if (COffset) {
      Offset = COffset->getZExtValue();
      if (Offset >= IntSize) {
        II->setArgOperand(1, ConstantInt::get(COffset->getType(),
                                              Offset & (IntSize - 1)));
        return II;
      }
    }

    bool Signed = II->getIntrinsicID() == Intrinsic::amdgcn_sbfe;

    // TODO: Also emit sub if only width is constant.
    if (!CWidth && COffset && Offset == 0) {
      Constant *KSize = ConstantInt::get(COffset->getType(), IntSize);
      Value *ShiftVal = Builder.CreateSub(KSize, II->getArgOperand(2));
      ShiftVal = Builder.CreateZExt(ShiftVal, II->getType());

      Value *Shl = Builder.CreateShl(Src, ShiftVal);
      Value *RightShift = Signed ? Builder.CreateAShr(Shl, ShiftVal)
                                 : Builder.CreateLShr(Shl, ShiftVal);
      RightShift->takeName(II);
      return replaceInstUsesWith(*II, RightShift);
    }

    if (!CWidth || !COffset)
      break;

    // TODO: This allows folding to undef when the hardware has specific
    // behavior?
    if (Offset + Width < IntSize) {
      Value *Shl = Builder.CreateShl(Src, IntSize - Offset - Width);
      Value *RightShift = Signed ? Builder.CreateAShr(Shl, IntSize - Width)
                                 : Builder.CreateLShr(Shl, IntSize - Width);
      RightShift->takeName(II);
      return replaceInstUsesWith(*II, RightShift);
    }

    Value *RightShift = Signed ? Builder.CreateAShr(Src, Offset)
                               : Builder.CreateLShr(Src, Offset);

    RightShift->takeName(II);
    return replaceInstUsesWith(*II, RightShift);
  }
  case Intrinsic::amdgcn_exp:
  case Intrinsic::amdgcn_exp_compr: {
    ConstantInt *En = dyn_cast<ConstantInt>(II->getArgOperand(1));
    if (!En) // Illegal.
      break;

    unsigned EnBits = En->getZExtValue();
    if (EnBits == 0xf)
      break; // All inputs enabled.

    bool IsCompr = II->getIntrinsicID() == Intrinsic::amdgcn_exp_compr;
    bool Changed = false;
    for (int I = 0; I < (IsCompr ? 2 : 4); ++I) {
      if ((!IsCompr && (EnBits & (1 << I)) == 0) ||
          (IsCompr && ((EnBits & (0x3 << (2 * I))) == 0))) {
        Value *Src = II->getArgOperand(I + 2);
        if (!isa<UndefValue>(Src)) {
          II->setArgOperand(I + 2, UndefValue::get(Src->getType()));
          Changed = true;
        }
      }
    }

    if (Changed)
      return II;

    break;

  }
  case Intrinsic::amdgcn_fmed3: {
    // Note this does not preserve proper sNaN behavior if IEEE-mode is enabled
    // for the shader.

    Value *Src0 = II->getArgOperand(0);
    Value *Src1 = II->getArgOperand(1);
    Value *Src2 = II->getArgOperand(2);

    bool Swap = false;
    // Canonicalize constants to RHS operands.
    //
    // fmed3(c0, x, c1) -> fmed3(x, c0, c1)
    if (isa<Constant>(Src0) && !isa<Constant>(Src1)) {
      std::swap(Src0, Src1);
      Swap = true;
    }

    if (isa<Constant>(Src1) && !isa<Constant>(Src2)) {
      std::swap(Src1, Src2);
      Swap = true;
    }

    if (isa<Constant>(Src0) && !isa<Constant>(Src1)) {
      std::swap(Src0, Src1);
      Swap = true;
    }

    if (Swap) {
      II->setArgOperand(0, Src0);
      II->setArgOperand(1, Src1);
      II->setArgOperand(2, Src2);
      return II;
    }

    if (match(Src2, m_NaN()) || isa<UndefValue>(Src2)) {
      CallInst *NewCall = Builder.CreateMinNum(Src0, Src1);
      NewCall->copyFastMathFlags(II);
      NewCall->takeName(II);
      return replaceInstUsesWith(*II, NewCall);
    }

    if (const ConstantFP *C0 = dyn_cast<ConstantFP>(Src0)) {
      if (const ConstantFP *C1 = dyn_cast<ConstantFP>(Src1)) {
        if (const ConstantFP *C2 = dyn_cast<ConstantFP>(Src2)) {
          APFloat Result = fmed3AMDGCN(C0->getValueAPF(), C1->getValueAPF(),
                                       C2->getValueAPF());
          return replaceInstUsesWith(*II,
            ConstantFP::get(Builder.getContext(), Result));
        }
      }
    }

    break;
  }
  case Intrinsic::amdgcn_icmp:
  case Intrinsic::amdgcn_fcmp: {
    const ConstantInt *CC = dyn_cast<ConstantInt>(II->getArgOperand(2));
    if (!CC)
      break;

    // Guard against invalid arguments.
    int64_t CCVal = CC->getZExtValue();
    bool IsInteger = II->getIntrinsicID() == Intrinsic::amdgcn_icmp;
    if ((IsInteger && (CCVal < CmpInst::FIRST_ICMP_PREDICATE ||
                       CCVal > CmpInst::LAST_ICMP_PREDICATE)) ||
        (!IsInteger && (CCVal < CmpInst::FIRST_FCMP_PREDICATE ||
                        CCVal > CmpInst::LAST_FCMP_PREDICATE)))
      break;

    Value *Src0 = II->getArgOperand(0);
    Value *Src1 = II->getArgOperand(1);

    if (auto *CSrc0 = dyn_cast<Constant>(Src0)) {
      if (auto *CSrc1 = dyn_cast<Constant>(Src1)) {
        Constant *CCmp = ConstantExpr::getCompare(CCVal, CSrc0, CSrc1);
        if (CCmp->isNullValue()) {
          return replaceInstUsesWith(
              *II, ConstantExpr::getSExt(CCmp, II->getType()));
        }

        // The result of V_ICMP/V_FCMP assembly instructions (which this
        // intrinsic exposes) is one bit per thread, masked with the EXEC
        // register (which contains the bitmask of live threads). So a
        // comparison that always returns true is the same as a read of the
        // EXEC register.
        Value *NewF = Intrinsic::getDeclaration(
            II->getModule(), Intrinsic::read_register, II->getType());
        Metadata *MDArgs[] = {MDString::get(II->getContext(), "exec")};
        MDNode *MD = MDNode::get(II->getContext(), MDArgs);
        Value *Args[] = {MetadataAsValue::get(II->getContext(), MD)};
        CallInst *NewCall = Builder.CreateCall(NewF, Args);
        NewCall->addAttribute(AttributeList::FunctionIndex,
                              Attribute::Convergent);
        NewCall->takeName(II);
        return replaceInstUsesWith(*II, NewCall);
      }

      // Canonicalize constants to RHS.
      CmpInst::Predicate SwapPred
        = CmpInst::getSwappedPredicate(static_cast<CmpInst::Predicate>(CCVal));
      II->setArgOperand(0, Src1);
      II->setArgOperand(1, Src0);
      II->setArgOperand(2, ConstantInt::get(CC->getType(),
                                            static_cast<int>(SwapPred)));
      return II;
    }

    if (CCVal != CmpInst::ICMP_EQ && CCVal != CmpInst::ICMP_NE)
      break;

    // Canonicalize compare eq with true value to compare != 0
    // llvm.amdgcn.icmp(zext (i1 x), 1, eq)
    //   -> llvm.amdgcn.icmp(zext (i1 x), 0, ne)
    // llvm.amdgcn.icmp(sext (i1 x), -1, eq)
    //   -> llvm.amdgcn.icmp(sext (i1 x), 0, ne)
    Value *ExtSrc;
    if (CCVal == CmpInst::ICMP_EQ &&
        ((match(Src1, m_One()) && match(Src0, m_ZExt(m_Value(ExtSrc)))) ||
         (match(Src1, m_AllOnes()) && match(Src0, m_SExt(m_Value(ExtSrc))))) &&
        ExtSrc->getType()->isIntegerTy(1)) {
      II->setArgOperand(1, ConstantInt::getNullValue(Src1->getType()));
      II->setArgOperand(2, ConstantInt::get(CC->getType(), CmpInst::ICMP_NE));
      return II;
    }

    CmpInst::Predicate SrcPred;
    Value *SrcLHS;
    Value *SrcRHS;

    // Fold compare eq/ne with 0 from a compare result as the predicate to the
    // intrinsic. The typical use is a wave vote function in the library, which
    // will be fed from a user code condition compared with 0. Fold in the
    // redundant compare.

    // llvm.amdgcn.icmp([sz]ext ([if]cmp pred a, b), 0, ne)
    //   -> llvm.amdgcn.[if]cmp(a, b, pred)
    //
    // llvm.amdgcn.icmp([sz]ext ([if]cmp pred a, b), 0, eq)
    //   -> llvm.amdgcn.[if]cmp(a, b, inv pred)
    if (match(Src1, m_Zero()) &&
        match(Src0,
              m_ZExtOrSExt(m_Cmp(SrcPred, m_Value(SrcLHS), m_Value(SrcRHS))))) {
      if (CCVal == CmpInst::ICMP_EQ)
        SrcPred = CmpInst::getInversePredicate(SrcPred);

      Intrinsic::ID NewIID = CmpInst::isFPPredicate(SrcPred) ?
        Intrinsic::amdgcn_fcmp : Intrinsic::amdgcn_icmp;

      Value *NewF = Intrinsic::getDeclaration(II->getModule(), NewIID,
                                              SrcLHS->getType());
      Value *Args[] = { SrcLHS, SrcRHS,
                        ConstantInt::get(CC->getType(), SrcPred) };
      CallInst *NewCall = Builder.CreateCall(NewF, Args);
      NewCall->takeName(II);
      return replaceInstUsesWith(*II, NewCall);
    }

    break;
  }
  case Intrinsic::stackrestore: {
    // If the save is right next to the restore, remove the restore.  This can
    // happen when variable allocas are DCE'd.
    if (IntrinsicInst *SS = dyn_cast<IntrinsicInst>(II->getArgOperand(0))) {
      if (SS->getIntrinsicID() == Intrinsic::stacksave) {
        if (&*++SS->getIterator() == II)
          return eraseInstFromFunction(CI);
      }
    }

    // Scan down this block to see if there is another stack restore in the
    // same block without an intervening call/alloca.
    BasicBlock::iterator BI(II);
    TerminatorInst *TI = II->getParent()->getTerminator();
    bool CannotRemove = false;
    for (++BI; &*BI != TI; ++BI) {
      if (isa<AllocaInst>(BI)) {
        CannotRemove = true;
        break;
      }
      if (CallInst *BCI = dyn_cast<CallInst>(BI)) {
        if (IntrinsicInst *II = dyn_cast<IntrinsicInst>(BCI)) {
          // If there is a stackrestore below this one, remove this one.
          if (II->getIntrinsicID() == Intrinsic::stackrestore)
            return eraseInstFromFunction(CI);

          // Bail if we cross over an intrinsic with side effects, such as
          // llvm.stacksave, llvm.read_register, or llvm.setjmp.
          if (II->mayHaveSideEffects()) {
            CannotRemove = true;
            break;
          }
        } else {
          // If we found a non-intrinsic call, we can't remove the stack
          // restore.
          CannotRemove = true;
          break;
        }
      }
    }

    // If the stack restore is in a return, resume, or unwind block and if there
    // are no allocas or calls between the restore and the return, nuke the
    // restore.
    if (!CannotRemove && (isa<ReturnInst>(TI) || isa<ResumeInst>(TI)))
      return eraseInstFromFunction(CI);
    break;
  }
  case Intrinsic::lifetime_start:
    // Asan needs to poison memory to detect invalid access which is possible
    // even for empty lifetime range.
    if (II->getFunction()->hasFnAttribute(Attribute::SanitizeAddress))
      break;

    if (removeTriviallyEmptyRange(*II, Intrinsic::lifetime_start,
                                  Intrinsic::lifetime_end, *this))
      return nullptr;
    break;
  case Intrinsic::assume: {
    Value *IIOperand = II->getArgOperand(0);
    // Remove an assume if it is immediately followed by an identical assume.
    if (match(II->getNextNode(),
              m_Intrinsic<Intrinsic::assume>(m_Specific(IIOperand))))
      return eraseInstFromFunction(CI);

    // Canonicalize assume(a && b) -> assume(a); assume(b);
    // Note: New assumption intrinsics created here are registered by
    // the InstCombineIRInserter object.
    Value *AssumeIntrinsic = II->getCalledValue(), *A, *B;
    if (match(IIOperand, m_And(m_Value(A), m_Value(B)))) {
      Builder.CreateCall(AssumeIntrinsic, A, II->getName());
      Builder.CreateCall(AssumeIntrinsic, B, II->getName());
      return eraseInstFromFunction(*II);
    }
    // assume(!(a || b)) -> assume(!a); assume(!b);
    if (match(IIOperand, m_Not(m_Or(m_Value(A), m_Value(B))))) {
      Builder.CreateCall(AssumeIntrinsic, Builder.CreateNot(A), II->getName());
      Builder.CreateCall(AssumeIntrinsic, Builder.CreateNot(B), II->getName());
      return eraseInstFromFunction(*II);
    }

    // assume( (load addr) != null ) -> add 'nonnull' metadata to load
    // (if assume is valid at the load)
    CmpInst::Predicate Pred;
    Instruction *LHS;
    if (match(IIOperand, m_ICmp(Pred, m_Instruction(LHS), m_Zero())) &&
        Pred == ICmpInst::ICMP_NE && LHS->getOpcode() == Instruction::Load &&
        LHS->getType()->isPointerTy() &&
        isValidAssumeForContext(II, LHS, &DT)) {
      MDNode *MD = MDNode::get(II->getContext(), None);
      LHS->setMetadata(LLVMContext::MD_nonnull, MD);
      return eraseInstFromFunction(*II);

      // TODO: apply nonnull return attributes to calls and invokes
      // TODO: apply range metadata for range check patterns?
    }

    // If there is a dominating assume with the same condition as this one,
    // then this one is redundant, and should be removed.
    KnownBits Known(1);
    computeKnownBits(IIOperand, Known, 0, II);
    if (Known.isAllOnes())
      return eraseInstFromFunction(*II);

    // Update the cache of affected values for this assumption (we might be
    // here because we just simplified the condition).
    AC.updateAffectedValues(II);
    break;
  }
  case Intrinsic::experimental_gc_relocate: {
    // Translate facts known about a pointer before relocating into
    // facts about the relocate value, while being careful to
    // preserve relocation semantics.
    Value *DerivedPtr = cast<GCRelocateInst>(II)->getDerivedPtr();

    // Remove the relocation if unused, note that this check is required
    // to prevent the cases below from looping forever.
    if (II->use_empty())
      return eraseInstFromFunction(*II);

    // Undef is undef, even after relocation.
    // TODO: provide a hook for this in GCStrategy.  This is clearly legal for
    // most practical collectors, but there was discussion in the review thread
    // about whether it was legal for all possible collectors.
    if (isa<UndefValue>(DerivedPtr))
      // Use undef of gc_relocate's type to replace it.
      return replaceInstUsesWith(*II, UndefValue::get(II->getType()));

    if (auto *PT = dyn_cast<PointerType>(II->getType())) {
      // The relocation of null will be null for most any collector.
      // TODO: provide a hook for this in GCStrategy.  There might be some
      // weird collector this property does not hold for.
      if (isa<ConstantPointerNull>(DerivedPtr))
        // Use null-pointer of gc_relocate's type to replace it.
        return replaceInstUsesWith(*II, ConstantPointerNull::get(PT));

      // isKnownNonNull -> nonnull attribute
      if (isKnownNonNullAt(DerivedPtr, II, &DT))
        II->addAttribute(AttributeList::ReturnIndex, Attribute::NonNull);
    }

    // TODO: bitcast(relocate(p)) -> relocate(bitcast(p))
    // Canonicalize on the type from the uses to the defs

    // TODO: relocate((gep p, C, C2, ...)) -> gep(relocate(p), C, C2, ...)
    break;
  }

  case Intrinsic::experimental_guard: {
    // Is this guard followed by another guard?
    Instruction *NextInst = II->getNextNode();
    Value *NextCond = nullptr;
    if (match(NextInst,
              m_Intrinsic<Intrinsic::experimental_guard>(m_Value(NextCond)))) {
      Value *CurrCond = II->getArgOperand(0);

      // Remove a guard that it is immediately preceded by an identical guard.
      if (CurrCond == NextCond)
        return eraseInstFromFunction(*NextInst);

      // Otherwise canonicalize guard(a); guard(b) -> guard(a & b).
      II->setArgOperand(0, Builder.CreateAnd(CurrCond, NextCond));
      return eraseInstFromFunction(*NextInst);
    }
    break;
  }
  }
  return visitCallSite(II);
}

// Fence instruction simplification
Instruction *InstCombiner::visitFenceInst(FenceInst &FI) {
  // Remove identical consecutive fences.
  if (auto *NFI = dyn_cast<FenceInst>(FI.getNextNode()))
    if (FI.isIdenticalTo(NFI))
      return eraseInstFromFunction(FI);
  return nullptr;
}

// InvokeInst simplification
//
Instruction *InstCombiner::visitInvokeInst(InvokeInst &II) {
  return visitCallSite(&II);
}

/// If this cast does not affect the value passed through the varargs area, we
/// can eliminate the use of the cast.
static bool isSafeToEliminateVarargsCast(const CallSite CS,
                                         const DataLayout &DL,
                                         const CastInst *const CI,
                                         const int ix) {
  if (!CI->isLosslessCast())
    return false;

  // If this is a GC intrinsic, avoid munging types.  We need types for
  // statepoint reconstruction in SelectionDAG.
  // TODO: This is probably something which should be expanded to all
  // intrinsics since the entire point of intrinsics is that
  // they are understandable by the optimizer.
  if (isStatepoint(CS) || isGCRelocate(CS) || isGCResult(CS))
    return false;

  // The size of ByVal or InAlloca arguments is derived from the type, so we
  // can't change to a type with a different size.  If the size were
  // passed explicitly we could avoid this check.
  if (!CS.isByValOrInAllocaArgument(ix))
    return true;

  Type* SrcTy =
            cast<PointerType>(CI->getOperand(0)->getType())->getElementType();
  Type* DstTy = cast<PointerType>(CI->getType())->getElementType();
  if (!SrcTy->isSized() || !DstTy->isSized())
    return false;
  if (DL.getTypeAllocSize(SrcTy) != DL.getTypeAllocSize(DstTy))
    return false;
  return true;
}

Instruction *InstCombiner::tryOptimizeCall(CallInst *CI) {
  if (!CI->getCalledFunction()) return nullptr;

  auto InstCombineRAUW = [this](Instruction *From, Value *With) {
    replaceInstUsesWith(*From, With);
  };
  LibCallSimplifier Simplifier(DL, &TLI, InstCombineRAUW);
  if (Value *With = Simplifier.optimizeCall(CI)) {
    ++NumSimplified;
    return CI->use_empty() ? CI : replaceInstUsesWith(*CI, With);
  }

  return nullptr;
}

static IntrinsicInst *findInitTrampolineFromAlloca(Value *TrampMem) {
  // Strip off at most one level of pointer casts, looking for an alloca.  This
  // is good enough in practice and simpler than handling any number of casts.
  Value *Underlying = TrampMem->stripPointerCasts();
  if (Underlying != TrampMem &&
      (!Underlying->hasOneUse() || Underlying->user_back() != TrampMem))
    return nullptr;
  if (!isa<AllocaInst>(Underlying))
    return nullptr;

  IntrinsicInst *InitTrampoline = nullptr;
  for (User *U : TrampMem->users()) {
    IntrinsicInst *II = dyn_cast<IntrinsicInst>(U);
    if (!II)
      return nullptr;
    if (II->getIntrinsicID() == Intrinsic::init_trampoline) {
      if (InitTrampoline)
        // More than one init_trampoline writes to this value.  Give up.
        return nullptr;
      InitTrampoline = II;
      continue;
    }
    if (II->getIntrinsicID() == Intrinsic::adjust_trampoline)
      // Allow any number of calls to adjust.trampoline.
      continue;
    return nullptr;
  }

  // No call to init.trampoline found.
  if (!InitTrampoline)
    return nullptr;

  // Check that the alloca is being used in the expected way.
  if (InitTrampoline->getOperand(0) != TrampMem)
    return nullptr;

  return InitTrampoline;
}

static IntrinsicInst *findInitTrampolineFromBB(IntrinsicInst *AdjustTramp,
                                               Value *TrampMem) {
  // Visit all the previous instructions in the basic block, and try to find a
  // init.trampoline which has a direct path to the adjust.trampoline.
  for (BasicBlock::iterator I = AdjustTramp->getIterator(),
                            E = AdjustTramp->getParent()->begin();
       I != E;) {
    Instruction *Inst = &*--I;
    if (IntrinsicInst *II = dyn_cast<IntrinsicInst>(I))
      if (II->getIntrinsicID() == Intrinsic::init_trampoline &&
          II->getOperand(0) == TrampMem)
        return II;
    if (Inst->mayWriteToMemory())
      return nullptr;
  }
  return nullptr;
}

// Given a call to llvm.adjust.trampoline, find and return the corresponding
// call to llvm.init.trampoline if the call to the trampoline can be optimized
// to a direct call to a function.  Otherwise return NULL.
//
static IntrinsicInst *findInitTrampoline(Value *Callee) {
  Callee = Callee->stripPointerCasts();
  IntrinsicInst *AdjustTramp = dyn_cast<IntrinsicInst>(Callee);
  if (!AdjustTramp ||
      AdjustTramp->getIntrinsicID() != Intrinsic::adjust_trampoline)
    return nullptr;

  Value *TrampMem = AdjustTramp->getOperand(0);

  if (IntrinsicInst *IT = findInitTrampolineFromAlloca(TrampMem))
    return IT;
  if (IntrinsicInst *IT = findInitTrampolineFromBB(AdjustTramp, TrampMem))
    return IT;
  return nullptr;
}

/// Improvements for call and invoke instructions.
Instruction *InstCombiner::visitCallSite(CallSite CS) {
  if (isAllocLikeFn(CS.getInstruction(), &TLI))
    return visitAllocSite(*CS.getInstruction());

  bool Changed = false;

  // Mark any parameters that are known to be non-null with the nonnull
  // attribute.  This is helpful for inlining calls to functions with null
  // checks on their arguments.
  SmallVector<unsigned, 4> ArgNos;
  unsigned ArgNo = 0;

  for (Value *V : CS.args()) {
    if (V->getType()->isPointerTy() &&
        !CS.paramHasAttr(ArgNo, Attribute::NonNull) &&
        isKnownNonNullAt(V, CS.getInstruction(), &DT))
      ArgNos.push_back(ArgNo);
    ArgNo++;
  }

  assert(ArgNo == CS.arg_size() && "sanity check");

  if (!ArgNos.empty()) {
    AttributeList AS = CS.getAttributes();
    LLVMContext &Ctx = CS.getInstruction()->getContext();
    AS = AS.addParamAttribute(Ctx, ArgNos,
                              Attribute::get(Ctx, Attribute::NonNull));
    CS.setAttributes(AS);
    Changed = true;
  }

  // If the callee is a pointer to a function, attempt to move any casts to the
  // arguments of the call/invoke.
  Value *Callee = CS.getCalledValue();
  if (!isa<Function>(Callee) && transformConstExprCastCall(CS))
    return nullptr;

  if (Function *CalleeF = dyn_cast<Function>(Callee)) {
    // Remove the convergent attr on calls when the callee is not convergent.
    if (CS.isConvergent() && !CalleeF->isConvergent() &&
        !CalleeF->isIntrinsic()) {
      DEBUG(dbgs() << "Removing convergent attr from instr "
                   << CS.getInstruction() << "\n");
      CS.setNotConvergent();
      return CS.getInstruction();
    }

    // If the call and callee calling conventions don't match, this call must
    // be unreachable, as the call is undefined.
    if (CalleeF->getCallingConv() != CS.getCallingConv() &&
        // Only do this for calls to a function with a body.  A prototype may
        // not actually end up matching the implementation's calling conv for a
        // variety of reasons (e.g. it may be written in assembly).
        !CalleeF->isDeclaration()) {
      Instruction *OldCall = CS.getInstruction();
      new StoreInst(ConstantInt::getTrue(Callee->getContext()),
                UndefValue::get(Type::getInt1PtrTy(Callee->getContext())),
                                  OldCall);
      // If OldCall does not return void then replaceAllUsesWith undef.
      // This allows ValueHandlers and custom metadata to adjust itself.
      if (!OldCall->getType()->isVoidTy())
        replaceInstUsesWith(*OldCall, UndefValue::get(OldCall->getType()));
      if (isa<CallInst>(OldCall))
        return eraseInstFromFunction(*OldCall);

      // We cannot remove an invoke, because it would change the CFG, just
      // change the callee to a null pointer.
      cast<InvokeInst>(OldCall)->setCalledFunction(
                                    Constant::getNullValue(CalleeF->getType()));
      return nullptr;
    }
  }

  if (isa<ConstantPointerNull>(Callee) || isa<UndefValue>(Callee)) {
    // If CS does not return void then replaceAllUsesWith undef.
    // This allows ValueHandlers and custom metadata to adjust itself.
    if (!CS.getInstruction()->getType()->isVoidTy())
      replaceInstUsesWith(*CS.getInstruction(),
                          UndefValue::get(CS.getInstruction()->getType()));

    if (isa<InvokeInst>(CS.getInstruction())) {
      // Can't remove an invoke because we cannot change the CFG.
      return nullptr;
    }

    // This instruction is not reachable, just remove it.  We insert a store to
    // undef so that we know that this code is not reachable, despite the fact
    // that we can't modify the CFG here.
    new StoreInst(ConstantInt::getTrue(Callee->getContext()),
                  UndefValue::get(Type::getInt1PtrTy(Callee->getContext())),
                  CS.getInstruction());

    return eraseInstFromFunction(*CS.getInstruction());
  }

  if (IntrinsicInst *II = findInitTrampoline(Callee))
    return transformCallThroughTrampoline(CS, II);

  PointerType *PTy = cast<PointerType>(Callee->getType());
  FunctionType *FTy = cast<FunctionType>(PTy->getElementType());
  if (FTy->isVarArg()) {
    int ix = FTy->getNumParams();
    // See if we can optimize any arguments passed through the varargs area of
    // the call.
    for (CallSite::arg_iterator I = CS.arg_begin() + FTy->getNumParams(),
           E = CS.arg_end(); I != E; ++I, ++ix) {
      CastInst *CI = dyn_cast<CastInst>(*I);
      if (CI && isSafeToEliminateVarargsCast(CS, DL, CI, ix)) {
        *I = CI->getOperand(0);
        Changed = true;
      }
    }
  }

  if (isa<InlineAsm>(Callee) && !CS.doesNotThrow()) {
    // Inline asm calls cannot throw - mark them 'nounwind'.
    CS.setDoesNotThrow();
    Changed = true;
  }

  // Try to optimize the call if possible, we require DataLayout for most of
  // this.  None of these calls are seen as possibly dead so go ahead and
  // delete the instruction now.
  if (CallInst *CI = dyn_cast<CallInst>(CS.getInstruction())) {
    Instruction *I = tryOptimizeCall(CI);
    // If we changed something return the result, etc. Otherwise let
    // the fallthrough check.
    if (I) return eraseInstFromFunction(*I);
  }

  return Changed ? CS.getInstruction() : nullptr;
}

/// If the callee is a constexpr cast of a function, attempt to move the cast to
/// the arguments of the call/invoke.
bool InstCombiner::transformConstExprCastCall(CallSite CS) {
  auto *Callee = dyn_cast<Function>(CS.getCalledValue()->stripPointerCasts());
  if (!Callee)
    return false;

  // The prototype of a thunk is a lie. Don't directly call such a function.
  if (Callee->hasFnAttribute("thunk"))
    return false;

  Instruction *Caller = CS.getInstruction();
  const AttributeList &CallerPAL = CS.getAttributes();

  // Okay, this is a cast from a function to a different type.  Unless doing so
  // would cause a type conversion of one of our arguments, change this call to
  // be a direct call with arguments casted to the appropriate types.
  //
  FunctionType *FT = Callee->getFunctionType();
  Type *OldRetTy = Caller->getType();
  Type *NewRetTy = FT->getReturnType();

  // Check to see if we are changing the return type...
  if (OldRetTy != NewRetTy) {

    if (NewRetTy->isStructTy())
      return false; // TODO: Handle multiple return values.

    if (!CastInst::isBitOrNoopPointerCastable(NewRetTy, OldRetTy, DL)) {
      if (Callee->isDeclaration())
        return false;   // Cannot transform this return value.

      if (!Caller->use_empty() &&
          // void -> non-void is handled specially
          !NewRetTy->isVoidTy())
        return false;   // Cannot transform this return value.
    }

    if (!CallerPAL.isEmpty() && !Caller->use_empty()) {
      AttrBuilder RAttrs(CallerPAL, AttributeList::ReturnIndex);
      if (RAttrs.overlaps(AttributeFuncs::typeIncompatible(NewRetTy)))
        return false;   // Attribute not compatible with transformed value.
    }

    // If the callsite is an invoke instruction, and the return value is used by
    // a PHI node in a successor, we cannot change the return type of the call
    // because there is no place to put the cast instruction (without breaking
    // the critical edge).  Bail out in this case.
    if (!Caller->use_empty())
      if (InvokeInst *II = dyn_cast<InvokeInst>(Caller))
        for (User *U : II->users())
          if (PHINode *PN = dyn_cast<PHINode>(U))
            if (PN->getParent() == II->getNormalDest() ||
                PN->getParent() == II->getUnwindDest())
              return false;
  }

  unsigned NumActualArgs = CS.arg_size();
  unsigned NumCommonArgs = std::min(FT->getNumParams(), NumActualArgs);

  // Prevent us turning:
  // declare void @takes_i32_inalloca(i32* inalloca)
  //  call void bitcast (void (i32*)* @takes_i32_inalloca to void (i32)*)(i32 0)
  //
  // into:
  //  call void @takes_i32_inalloca(i32* null)
  //
  //  Similarly, avoid folding away bitcasts of byval calls.
  if (Callee->getAttributes().hasAttrSomewhere(Attribute::InAlloca) ||
      Callee->getAttributes().hasAttrSomewhere(Attribute::ByVal))
    return false;

  CallSite::arg_iterator AI = CS.arg_begin();
  for (unsigned i = 0, e = NumCommonArgs; i != e; ++i, ++AI) {
    Type *ParamTy = FT->getParamType(i);
    Type *ActTy = (*AI)->getType();

    if (!CastInst::isBitOrNoopPointerCastable(ActTy, ParamTy, DL))
      return false;   // Cannot transform this parameter value.

    if (AttrBuilder(CallerPAL.getParamAttributes(i))
            .overlaps(AttributeFuncs::typeIncompatible(ParamTy)))
      return false;   // Attribute not compatible with transformed value.

    if (CS.isInAllocaArgument(i))
      return false;   // Cannot transform to and from inalloca.

    // If the parameter is passed as a byval argument, then we have to have a
    // sized type and the sized type has to have the same size as the old type.
    if (ParamTy != ActTy && CallerPAL.hasParamAttribute(i, Attribute::ByVal)) {
      PointerType *ParamPTy = dyn_cast<PointerType>(ParamTy);
      if (!ParamPTy || !ParamPTy->getElementType()->isSized())
        return false;

      Type *CurElTy = ActTy->getPointerElementType();
      if (DL.getTypeAllocSize(CurElTy) !=
          DL.getTypeAllocSize(ParamPTy->getElementType()))
        return false;
    }
  }

  if (Callee->isDeclaration()) {
    // Do not delete arguments unless we have a function body.
    if (FT->getNumParams() < NumActualArgs && !FT->isVarArg())
      return false;

    // If the callee is just a declaration, don't change the varargsness of the
    // call.  We don't want to introduce a varargs call where one doesn't
    // already exist.
    PointerType *APTy = cast<PointerType>(CS.getCalledValue()->getType());
    if (FT->isVarArg()!=cast<FunctionType>(APTy->getElementType())->isVarArg())
      return false;

    // If both the callee and the cast type are varargs, we still have to make
    // sure the number of fixed parameters are the same or we have the same
    // ABI issues as if we introduce a varargs call.
    if (FT->isVarArg() &&
        cast<FunctionType>(APTy->getElementType())->isVarArg() &&
        FT->getNumParams() !=
        cast<FunctionType>(APTy->getElementType())->getNumParams())
      return false;
  }

  if (FT->getNumParams() < NumActualArgs && FT->isVarArg() &&
      !CallerPAL.isEmpty()) {
    // In this case we have more arguments than the new function type, but we
    // won't be dropping them.  Check that these extra arguments have attributes
    // that are compatible with being a vararg call argument.
    unsigned SRetIdx;
    if (CallerPAL.hasAttrSomewhere(Attribute::StructRet, &SRetIdx) &&
        SRetIdx > FT->getNumParams())
      return false;
  }

  // Okay, we decided that this is a safe thing to do: go ahead and start
  // inserting cast instructions as necessary.
  SmallVector<Value *, 8> Args;
  SmallVector<AttributeSet, 8> ArgAttrs;
  Args.reserve(NumActualArgs);
  ArgAttrs.reserve(NumActualArgs);

  // Get any return attributes.
  AttrBuilder RAttrs(CallerPAL, AttributeList::ReturnIndex);

  // If the return value is not being used, the type may not be compatible
  // with the existing attributes.  Wipe out any problematic attributes.
  RAttrs.remove(AttributeFuncs::typeIncompatible(NewRetTy));

  AI = CS.arg_begin();
  for (unsigned i = 0; i != NumCommonArgs; ++i, ++AI) {
    Type *ParamTy = FT->getParamType(i);

    Value *NewArg = *AI;
    if ((*AI)->getType() != ParamTy)
      NewArg = Builder.CreateBitOrPointerCast(*AI, ParamTy);
    Args.push_back(NewArg);

    // Add any parameter attributes.
    ArgAttrs.push_back(CallerPAL.getParamAttributes(i));
  }

  // If the function takes more arguments than the call was taking, add them
  // now.
  for (unsigned i = NumCommonArgs; i != FT->getNumParams(); ++i) {
    Args.push_back(Constant::getNullValue(FT->getParamType(i)));
    ArgAttrs.push_back(AttributeSet());
  }

  // If we are removing arguments to the function, emit an obnoxious warning.
  if (FT->getNumParams() < NumActualArgs) {
    // TODO: if (!FT->isVarArg()) this call may be unreachable. PR14722
    if (FT->isVarArg()) {
      // Add all of the arguments in their promoted form to the arg list.
      for (unsigned i = FT->getNumParams(); i != NumActualArgs; ++i, ++AI) {
        Type *PTy = getPromotedType((*AI)->getType());
        Value *NewArg = *AI;
        if (PTy != (*AI)->getType()) {
          // Must promote to pass through va_arg area!
          Instruction::CastOps opcode =
            CastInst::getCastOpcode(*AI, false, PTy, false);
          NewArg = Builder.CreateCast(opcode, *AI, PTy);
        }
        Args.push_back(NewArg);

        // Add any parameter attributes.
        ArgAttrs.push_back(CallerPAL.getParamAttributes(i));
      }
    }
  }

  AttributeSet FnAttrs = CallerPAL.getFnAttributes();

  if (NewRetTy->isVoidTy())
    Caller->setName("");   // Void type should not have a name.

  assert((ArgAttrs.size() == FT->getNumParams() || FT->isVarArg()) &&
         "missing argument attributes");
  LLVMContext &Ctx = Callee->getContext();
  AttributeList NewCallerPAL = AttributeList::get(
      Ctx, FnAttrs, AttributeSet::get(Ctx, RAttrs), ArgAttrs);

  SmallVector<OperandBundleDef, 1> OpBundles;
  CS.getOperandBundlesAsDefs(OpBundles);

  CallSite NewCS;
  if (InvokeInst *II = dyn_cast<InvokeInst>(Caller)) {
    NewCS = Builder.CreateInvoke(Callee, II->getNormalDest(),
                                 II->getUnwindDest(), Args, OpBundles);
  } else {
    NewCS = Builder.CreateCall(Callee, Args, OpBundles);
    cast<CallInst>(NewCS.getInstruction())
        ->setTailCallKind(cast<CallInst>(Caller)->getTailCallKind());
  }
  NewCS->takeName(Caller);
  NewCS.setCallingConv(CS.getCallingConv());
  NewCS.setAttributes(NewCallerPAL);

  // Preserve the weight metadata for the new call instruction. The metadata
  // is used by SamplePGO to check callsite's hotness.
  uint64_t W;
  if (Caller->extractProfTotalWeight(W))
    NewCS->setProfWeight(W);

  // Insert a cast of the return type as necessary.
  Instruction *NC = NewCS.getInstruction();
  Value *NV = NC;
  if (OldRetTy != NV->getType() && !Caller->use_empty()) {
    if (!NV->getType()->isVoidTy()) {
      NV = NC = CastInst::CreateBitOrPointerCast(NC, OldRetTy);
      NC->setDebugLoc(Caller->getDebugLoc());

      // If this is an invoke instruction, we should insert it after the first
      // non-phi, instruction in the normal successor block.
      if (InvokeInst *II = dyn_cast<InvokeInst>(Caller)) {
        BasicBlock::iterator I = II->getNormalDest()->getFirstInsertionPt();
        InsertNewInstBefore(NC, *I);
      } else {
        // Otherwise, it's a call, just insert cast right after the call.
        InsertNewInstBefore(NC, *Caller);
      }
      Worklist.AddUsersToWorkList(*Caller);
    } else {
      NV = UndefValue::get(Caller->getType());
    }
  }

  if (!Caller->use_empty())
    replaceInstUsesWith(*Caller, NV);
  else if (Caller->hasValueHandle()) {
    if (OldRetTy == NV->getType())
      ValueHandleBase::ValueIsRAUWd(Caller, NV);
    else
      // We cannot call ValueIsRAUWd with a different type, and the
      // actual tracked value will disappear.
      ValueHandleBase::ValueIsDeleted(Caller);
  }

  eraseInstFromFunction(*Caller);
  return true;
}

/// Turn a call to a function created by init_trampoline / adjust_trampoline
/// intrinsic pair into a direct call to the underlying function.
Instruction *
InstCombiner::transformCallThroughTrampoline(CallSite CS,
                                             IntrinsicInst *Tramp) {
  Value *Callee = CS.getCalledValue();
  PointerType *PTy = cast<PointerType>(Callee->getType());
  FunctionType *FTy = cast<FunctionType>(PTy->getElementType());
  AttributeList Attrs = CS.getAttributes();

  // If the call already has the 'nest' attribute somewhere then give up -
  // otherwise 'nest' would occur twice after splicing in the chain.
  if (Attrs.hasAttrSomewhere(Attribute::Nest))
    return nullptr;

  assert(Tramp &&
         "transformCallThroughTrampoline called with incorrect CallSite.");

  Function *NestF =cast<Function>(Tramp->getArgOperand(1)->stripPointerCasts());
  FunctionType *NestFTy = cast<FunctionType>(NestF->getValueType());

  AttributeList NestAttrs = NestF->getAttributes();
  if (!NestAttrs.isEmpty()) {
    unsigned NestArgNo = 0;
    Type *NestTy = nullptr;
    AttributeSet NestAttr;

    // Look for a parameter marked with the 'nest' attribute.
    for (FunctionType::param_iterator I = NestFTy->param_begin(),
                                      E = NestFTy->param_end();
         I != E; ++NestArgNo, ++I) {
      AttributeSet AS = NestAttrs.getParamAttributes(NestArgNo);
      if (AS.hasAttribute(Attribute::Nest)) {
        // Record the parameter type and any other attributes.
        NestTy = *I;
        NestAttr = AS;
        break;
      }
    }

    if (NestTy) {
      Instruction *Caller = CS.getInstruction();
      std::vector<Value*> NewArgs;
      std::vector<AttributeSet> NewArgAttrs;
      NewArgs.reserve(CS.arg_size() + 1);
      NewArgAttrs.reserve(CS.arg_size());

      // Insert the nest argument into the call argument list, which may
      // mean appending it.  Likewise for attributes.

      {
        unsigned ArgNo = 0;
        CallSite::arg_iterator I = CS.arg_begin(), E = CS.arg_end();
        do {
          if (ArgNo == NestArgNo) {
            // Add the chain argument and attributes.
            Value *NestVal = Tramp->getArgOperand(2);
            if (NestVal->getType() != NestTy)
              NestVal = Builder.CreateBitCast(NestVal, NestTy, "nest");
            NewArgs.push_back(NestVal);
            NewArgAttrs.push_back(NestAttr);
          }

          if (I == E)
            break;

          // Add the original argument and attributes.
          NewArgs.push_back(*I);
          NewArgAttrs.push_back(Attrs.getParamAttributes(ArgNo));

          ++ArgNo;
          ++I;
        } while (true);
      }

      // The trampoline may have been bitcast to a bogus type (FTy).
      // Handle this by synthesizing a new function type, equal to FTy
      // with the chain parameter inserted.

      std::vector<Type*> NewTypes;
      NewTypes.reserve(FTy->getNumParams()+1);

      // Insert the chain's type into the list of parameter types, which may
      // mean appending it.
      {
        unsigned ArgNo = 0;
        FunctionType::param_iterator I = FTy->param_begin(),
          E = FTy->param_end();

        do {
          if (ArgNo == NestArgNo)
            // Add the chain's type.
            NewTypes.push_back(NestTy);

          if (I == E)
            break;

          // Add the original type.
          NewTypes.push_back(*I);

          ++ArgNo;
          ++I;
        } while (true);
      }

      // Replace the trampoline call with a direct call.  Let the generic
      // code sort out any function type mismatches.
      FunctionType *NewFTy = FunctionType::get(FTy->getReturnType(), NewTypes,
                                                FTy->isVarArg());
      Constant *NewCallee =
        NestF->getType() == PointerType::getUnqual(NewFTy) ?
        NestF : ConstantExpr::getBitCast(NestF,
                                         PointerType::getUnqual(NewFTy));
      AttributeList NewPAL =
          AttributeList::get(FTy->getContext(), Attrs.getFnAttributes(),
                             Attrs.getRetAttributes(), NewArgAttrs);

      SmallVector<OperandBundleDef, 1> OpBundles;
      CS.getOperandBundlesAsDefs(OpBundles);

      Instruction *NewCaller;
      if (InvokeInst *II = dyn_cast<InvokeInst>(Caller)) {
        NewCaller = InvokeInst::Create(NewCallee,
                                       II->getNormalDest(), II->getUnwindDest(),
                                       NewArgs, OpBundles);
        cast<InvokeInst>(NewCaller)->setCallingConv(II->getCallingConv());
        cast<InvokeInst>(NewCaller)->setAttributes(NewPAL);
      } else {
        NewCaller = CallInst::Create(NewCallee, NewArgs, OpBundles);
        cast<CallInst>(NewCaller)->setTailCallKind(
            cast<CallInst>(Caller)->getTailCallKind());
        cast<CallInst>(NewCaller)->setCallingConv(
            cast<CallInst>(Caller)->getCallingConv());
        cast<CallInst>(NewCaller)->setAttributes(NewPAL);
      }

      return NewCaller;
    }
  }

  // Replace the trampoline call with a direct call.  Since there is no 'nest'
  // parameter, there is no need to adjust the argument list.  Let the generic
  // code sort out any function type mismatches.
  Constant *NewCallee =
    NestF->getType() == PTy ? NestF :
                              ConstantExpr::getBitCast(NestF, PTy);
  CS.setCalledFunction(NewCallee);
  return CS.getInstruction();
}
