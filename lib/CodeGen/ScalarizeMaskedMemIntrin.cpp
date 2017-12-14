//=== ScalarizeMaskedMemIntrin.cpp - Scalarize unsupported masked mem      ===//
//===                                instrinsics                           ===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This pass replaces masked memory intrinsics - when unsupported by the target
// - with a chain of basic blocks, that deal with the elements one-by-one if the
// appropriate mask bit is set.
//
//===----------------------------------------------------------------------===//

#include "llvm/Analysis/TargetTransformInfo.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/Target/TargetSubtargetInfo.h"

using namespace llvm;

#define DEBUG_TYPE "scalarize-masked-mem-intrin"

namespace {

class ScalarizeMaskedMemIntrin : public FunctionPass {
  const TargetTransformInfo *TTI;

public:
  static char ID; // Pass identification, replacement for typeid
  explicit ScalarizeMaskedMemIntrin() : FunctionPass(ID), TTI(nullptr) {
    initializeScalarizeMaskedMemIntrinPass(*PassRegistry::getPassRegistry());
  }
  bool runOnFunction(Function &F) override;

  StringRef getPassName() const override {
    return "Scalarize Masked Memory Intrinsics";
  }

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.addRequired<TargetTransformInfoWrapperPass>();
  }

private:
  bool optimizeBlock(BasicBlock &BB, bool &ModifiedDT);
  bool optimizeCallInst(CallInst *CI, bool &ModifiedDT);
};
} // namespace

char ScalarizeMaskedMemIntrin::ID = 0;
INITIALIZE_PASS(ScalarizeMaskedMemIntrin, DEBUG_TYPE,
                "Scalarize unsupported masked memory intrinsics", false, false)

FunctionPass *llvm::createScalarizeMaskedMemIntrinPass() {
  return new ScalarizeMaskedMemIntrin();
}

// Translate a masked load intrinsic like
// <16 x i32 > @llvm.masked.load( <16 x i32>* %addr, i32 align,
//                               <16 x i1> %mask, <16 x i32> %passthru)
// to a chain of basic blocks, with loading element one-by-one if
// the appropriate mask bit is set
//
//  %1 = bitcast i8* %addr to i32*
//  %2 = extractelement <16 x i1> %mask, i32 0
//  %3 = icmp eq i1 %2, true
//  br i1 %3, label %cond.load, label %else
//
// cond.load:                                        ; preds = %0
//  %4 = getelementptr i32* %1, i32 0
//  %5 = load i32* %4
//  %6 = insertelement <16 x i32> undef, i32 %5, i32 0
//  br label %else
//
// else:                                             ; preds = %0, %cond.load
//  %res.phi.else = phi <16 x i32> [ %6, %cond.load ], [ undef, %0 ]
//  %7 = extractelement <16 x i1> %mask, i32 1
//  %8 = icmp eq i1 %7, true
//  br i1 %8, label %cond.load1, label %else2
//
// cond.load1:                                       ; preds = %else
//  %9 = getelementptr i32* %1, i32 1
//  %10 = load i32* %9
//  %11 = insertelement <16 x i32> %res.phi.else, i32 %10, i32 1
//  br label %else2
//
// else2:                                          ; preds = %else, %cond.load1
//  %res.phi.else3 = phi <16 x i32> [ %11, %cond.load1 ], [ %res.phi.else, %else ]
//  %12 = extractelement <16 x i1> %mask, i32 2
//  %13 = icmp eq i1 %12, true
//  br i1 %13, label %cond.load4, label %else5
//
static void scalarizeMaskedLoad(CallInst *CI) {
  Value *Ptr = CI->getArgOperand(0);
  Value *Alignment = CI->getArgOperand(1);
  Value *Mask = CI->getArgOperand(2);
  Value *Src0 = CI->getArgOperand(3);

  unsigned AlignVal = cast<ConstantInt>(Alignment)->getZExtValue();
  VectorType *VecType = dyn_cast<VectorType>(CI->getType());
  assert(VecType && "Unexpected return type of masked load intrinsic");

  Type *EltTy = CI->getType()->getVectorElementType();

  IRBuilder<> Builder(CI->getContext());
  Instruction *InsertPt = CI;
  BasicBlock *IfBlock = CI->getParent();
  BasicBlock *CondBlock = nullptr;
  BasicBlock *PrevIfBlock = CI->getParent();

  Builder.SetInsertPoint(InsertPt);
  Builder.SetCurrentDebugLocation(CI->getDebugLoc());

  // Short-cut if the mask is all-true.
  bool IsAllOnesMask =
      isa<Constant>(Mask) && cast<Constant>(Mask)->isAllOnesValue();

  if (IsAllOnesMask) {
    Value *NewI = Builder.CreateAlignedLoad(Ptr, AlignVal);
    CI->replaceAllUsesWith(NewI);
    CI->eraseFromParent();
    return;
  }

  // Adjust alignment for the scalar instruction.
  AlignVal = std::min(AlignVal, VecType->getScalarSizeInBits() / 8);
  // Bitcast %addr fron i8* to EltTy*
  Type *NewPtrType =
      EltTy->getPointerTo(cast<PointerType>(Ptr->getType())->getAddressSpace());
  Value *FirstEltPtr = Builder.CreateBitCast(Ptr, NewPtrType);
  unsigned VectorWidth = VecType->getNumElements();

  Value *UndefVal = UndefValue::get(VecType);

  // The result vector
  Value *VResult = UndefVal;

  if (isa<ConstantVector>(Mask)) {
    for (unsigned Idx = 0; Idx < VectorWidth; ++Idx) {
      if (cast<ConstantVector>(Mask)->getOperand(Idx)->isNullValue())
        continue;
      Value *Gep =
          Builder.CreateInBoundsGEP(EltTy, FirstEltPtr, Builder.getInt32(Idx));
      LoadInst *Load = Builder.CreateAlignedLoad(Gep, AlignVal);
      VResult =
          Builder.CreateInsertElement(VResult, Load, Builder.getInt32(Idx));
    }
    Value *NewI = Builder.CreateSelect(Mask, VResult, Src0);
    CI->replaceAllUsesWith(NewI);
    CI->eraseFromParent();
    return;
  }

  PHINode *Phi = nullptr;
  Value *PrevPhi = UndefVal;

  for (unsigned Idx = 0; Idx < VectorWidth; ++Idx) {

    // Fill the "else" block, created in the previous iteration
    //
    //  %res.phi.else3 = phi <16 x i32> [ %11, %cond.load1 ], [ %res.phi.else, %else ]
    //  %mask_1 = extractelement <16 x i1> %mask, i32 Idx
    //  %to_load = icmp eq i1 %mask_1, true
    //  br i1 %to_load, label %cond.load, label %else
    //
    if (Idx > 0) {
      Phi = Builder.CreatePHI(VecType, 2, "res.phi.else");
      Phi->addIncoming(VResult, CondBlock);
      Phi->addIncoming(PrevPhi, PrevIfBlock);
      PrevPhi = Phi;
      VResult = Phi;
    }

    Value *Predicate =
        Builder.CreateExtractElement(Mask, Builder.getInt32(Idx));
    Value *Cmp = Builder.CreateICmp(ICmpInst::ICMP_EQ, Predicate,
                                    ConstantInt::get(Predicate->getType(), 1));

    // Create "cond" block
    //
    //  %EltAddr = getelementptr i32* %1, i32 0
    //  %Elt = load i32* %EltAddr
    //  VResult = insertelement <16 x i32> VResult, i32 %Elt, i32 Idx
    //
    CondBlock = IfBlock->splitBasicBlock(InsertPt->getIterator(), "cond.load");
    Builder.SetInsertPoint(InsertPt);

    Value *Gep =
        Builder.CreateInBoundsGEP(EltTy, FirstEltPtr, Builder.getInt32(Idx));
    LoadInst *Load = Builder.CreateAlignedLoad(Gep, AlignVal);
    VResult = Builder.CreateInsertElement(VResult, Load, Builder.getInt32(Idx));

    // Create "else" block, fill it in the next iteration
    BasicBlock *NewIfBlock =
        CondBlock->splitBasicBlock(InsertPt->getIterator(), "else");
    Builder.SetInsertPoint(InsertPt);
    Instruction *OldBr = IfBlock->getTerminator();
    BranchInst::Create(CondBlock, NewIfBlock, Cmp, OldBr);
    OldBr->eraseFromParent();
    PrevIfBlock = IfBlock;
    IfBlock = NewIfBlock;
  }

  Phi = Builder.CreatePHI(VecType, 2, "res.phi.select");
  Phi->addIncoming(VResult, CondBlock);
  Phi->addIncoming(PrevPhi, PrevIfBlock);
  Value *NewI = Builder.CreateSelect(Mask, Phi, Src0);
  CI->replaceAllUsesWith(NewI);
  CI->eraseFromParent();
}

// Translate a masked store intrinsic, like
// void @llvm.masked.store(<16 x i32> %src, <16 x i32>* %addr, i32 align,
//                               <16 x i1> %mask)
// to a chain of basic blocks, that stores element one-by-one if
// the appropriate mask bit is set
//
//   %1 = bitcast i8* %addr to i32*
//   %2 = extractelement <16 x i1> %mask, i32 0
//   %3 = icmp eq i1 %2, true
//   br i1 %3, label %cond.store, label %else
//
// cond.store:                                       ; preds = %0
//   %4 = extractelement <16 x i32> %val, i32 0
//   %5 = getelementptr i32* %1, i32 0
//   store i32 %4, i32* %5
//   br label %else
//
// else:                                             ; preds = %0, %cond.store
//   %6 = extractelement <16 x i1> %mask, i32 1
//   %7 = icmp eq i1 %6, true
//   br i1 %7, label %cond.store1, label %else2
//
// cond.store1:                                      ; preds = %else
//   %8 = extractelement <16 x i32> %val, i32 1
//   %9 = getelementptr i32* %1, i32 1
//   store i32 %8, i32* %9
//   br label %else2
//   . . .
static void scalarizeMaskedStore(CallInst *CI) {
  Value *Src = CI->getArgOperand(0);
  Value *Ptr = CI->getArgOperand(1);
  Value *Alignment = CI->getArgOperand(2);
  Value *Mask = CI->getArgOperand(3);

  unsigned AlignVal = cast<ConstantInt>(Alignment)->getZExtValue();
  VectorType *VecType = dyn_cast<VectorType>(Src->getType());
  assert(VecType && "Unexpected data type in masked store intrinsic");

  Type *EltTy = VecType->getElementType();

  IRBuilder<> Builder(CI->getContext());
  Instruction *InsertPt = CI;
  BasicBlock *IfBlock = CI->getParent();
  Builder.SetInsertPoint(InsertPt);
  Builder.SetCurrentDebugLocation(CI->getDebugLoc());

  // Short-cut if the mask is all-true.
  bool IsAllOnesMask =
      isa<Constant>(Mask) && cast<Constant>(Mask)->isAllOnesValue();

  if (IsAllOnesMask) {
    Builder.CreateAlignedStore(Src, Ptr, AlignVal);
    CI->eraseFromParent();
    return;
  }

  // Adjust alignment for the scalar instruction.
  AlignVal = std::max(AlignVal, VecType->getScalarSizeInBits() / 8);
  // Bitcast %addr fron i8* to EltTy*
  Type *NewPtrType =
      EltTy->getPointerTo(cast<PointerType>(Ptr->getType())->getAddressSpace());
  Value *FirstEltPtr = Builder.CreateBitCast(Ptr, NewPtrType);
  unsigned VectorWidth = VecType->getNumElements();

  if (isa<ConstantVector>(Mask)) {
    for (unsigned Idx = 0; Idx < VectorWidth; ++Idx) {
      if (cast<ConstantVector>(Mask)->getOperand(Idx)->isNullValue())
        continue;
      Value *OneElt = Builder.CreateExtractElement(Src, Builder.getInt32(Idx));
      Value *Gep =
          Builder.CreateInBoundsGEP(EltTy, FirstEltPtr, Builder.getInt32(Idx));
      Builder.CreateAlignedStore(OneElt, Gep, AlignVal);
    }
    CI->eraseFromParent();
    return;
  }

  for (unsigned Idx = 0; Idx < VectorWidth; ++Idx) {

    // Fill the "else" block, created in the previous iteration
    //
    //  %mask_1 = extractelement <16 x i1> %mask, i32 Idx
    //  %to_store = icmp eq i1 %mask_1, true
    //  br i1 %to_store, label %cond.store, label %else
    //
    Value *Predicate =
        Builder.CreateExtractElement(Mask, Builder.getInt32(Idx));
    Value *Cmp = Builder.CreateICmp(ICmpInst::ICMP_EQ, Predicate,
                                    ConstantInt::get(Predicate->getType(), 1));

    // Create "cond" block
    //
    //  %OneElt = extractelement <16 x i32> %Src, i32 Idx
    //  %EltAddr = getelementptr i32* %1, i32 0
    //  %store i32 %OneElt, i32* %EltAddr
    //
    BasicBlock *CondBlock =
        IfBlock->splitBasicBlock(InsertPt->getIterator(), "cond.store");
    Builder.SetInsertPoint(InsertPt);

    Value *OneElt = Builder.CreateExtractElement(Src, Builder.getInt32(Idx));
    Value *Gep =
        Builder.CreateInBoundsGEP(EltTy, FirstEltPtr, Builder.getInt32(Idx));
    Builder.CreateAlignedStore(OneElt, Gep, AlignVal);

    // Create "else" block, fill it in the next iteration
    BasicBlock *NewIfBlock =
        CondBlock->splitBasicBlock(InsertPt->getIterator(), "else");
    Builder.SetInsertPoint(InsertPt);
    Instruction *OldBr = IfBlock->getTerminator();
    BranchInst::Create(CondBlock, NewIfBlock, Cmp, OldBr);
    OldBr->eraseFromParent();
    IfBlock = NewIfBlock;
  }
  CI->eraseFromParent();
}

// Translate a masked gather intrinsic like
// <16 x i32 > @llvm.masked.gather.v16i32( <16 x i32*> %Ptrs, i32 4,
//                               <16 x i1> %Mask, <16 x i32> %Src)
// to a chain of basic blocks, with loading element one-by-one if
// the appropriate mask bit is set
//
// % Ptrs = getelementptr i32, i32* %base, <16 x i64> %ind
// % Mask0 = extractelement <16 x i1> %Mask, i32 0
// % ToLoad0 = icmp eq i1 % Mask0, true
// br i1 % ToLoad0, label %cond.load, label %else
//
// cond.load:
// % Ptr0 = extractelement <16 x i32*> %Ptrs, i32 0
// % Load0 = load i32, i32* % Ptr0, align 4
// % Res0 = insertelement <16 x i32> undef, i32 % Load0, i32 0
// br label %else
//
// else:
// %res.phi.else = phi <16 x i32>[% Res0, %cond.load], [undef, % 0]
// % Mask1 = extractelement <16 x i1> %Mask, i32 1
// % ToLoad1 = icmp eq i1 % Mask1, true
// br i1 % ToLoad1, label %cond.load1, label %else2
//
// cond.load1:
// % Ptr1 = extractelement <16 x i32*> %Ptrs, i32 1
// % Load1 = load i32, i32* % Ptr1, align 4
// % Res1 = insertelement <16 x i32> %res.phi.else, i32 % Load1, i32 1
// br label %else2
// . . .
// % Result = select <16 x i1> %Mask, <16 x i32> %res.phi.select, <16 x i32> %Src
// ret <16 x i32> %Result
static void scalarizeMaskedGather(CallInst *CI) {
  Value *Ptrs = CI->getArgOperand(0);
  Value *Alignment = CI->getArgOperand(1);
  Value *Mask = CI->getArgOperand(2);
  Value *Src0 = CI->getArgOperand(3);

  VectorType *VecType = dyn_cast<VectorType>(CI->getType());

  assert(VecType && "Unexpected return type of masked load intrinsic");

  IRBuilder<> Builder(CI->getContext());
  Instruction *InsertPt = CI;
  BasicBlock *IfBlock = CI->getParent();
  BasicBlock *CondBlock = nullptr;
  BasicBlock *PrevIfBlock = CI->getParent();
  Builder.SetInsertPoint(InsertPt);
  unsigned AlignVal = cast<ConstantInt>(Alignment)->getZExtValue();

  Builder.SetCurrentDebugLocation(CI->getDebugLoc());

  Value *UndefVal = UndefValue::get(VecType);

  // The result vector
  Value *VResult = UndefVal;
  unsigned VectorWidth = VecType->getNumElements();

  // Shorten the way if the mask is a vector of constants.
  bool IsConstMask = isa<ConstantVector>(Mask);

  if (IsConstMask) {
    for (unsigned Idx = 0; Idx < VectorWidth; ++Idx) {
      if (cast<ConstantVector>(Mask)->getOperand(Idx)->isNullValue())
        continue;
      Value *Ptr = Builder.CreateExtractElement(Ptrs, Builder.getInt32(Idx),
                                                "Ptr" + Twine(Idx));
      LoadInst *Load =
          Builder.CreateAlignedLoad(Ptr, AlignVal, "Load" + Twine(Idx));
      VResult = Builder.CreateInsertElement(
          VResult, Load, Builder.getInt32(Idx), "Res" + Twine(Idx));
    }
    Value *NewI = Builder.CreateSelect(Mask, VResult, Src0);
    CI->replaceAllUsesWith(NewI);
    CI->eraseFromParent();
    return;
  }

  PHINode *Phi = nullptr;
  Value *PrevPhi = UndefVal;

  for (unsigned Idx = 0; Idx < VectorWidth; ++Idx) {

    // Fill the "else" block, created in the previous iteration
    //
    //  %Mask1 = extractelement <16 x i1> %Mask, i32 1
    //  %ToLoad1 = icmp eq i1 %Mask1, true
    //  br i1 %ToLoad1, label %cond.load, label %else
    //
    if (Idx > 0) {
      Phi = Builder.CreatePHI(VecType, 2, "res.phi.else");
      Phi->addIncoming(VResult, CondBlock);
      Phi->addIncoming(PrevPhi, PrevIfBlock);
      PrevPhi = Phi;
      VResult = Phi;
    }

    Value *Predicate = Builder.CreateExtractElement(Mask, Builder.getInt32(Idx),
                                                    "Mask" + Twine(Idx));
    Value *Cmp = Builder.CreateICmp(ICmpInst::ICMP_EQ, Predicate,
                                    ConstantInt::get(Predicate->getType(), 1),
                                    "ToLoad" + Twine(Idx));

    // Create "cond" block
    //
    //  %EltAddr = getelementptr i32* %1, i32 0
    //  %Elt = load i32* %EltAddr
    //  VResult = insertelement <16 x i32> VResult, i32 %Elt, i32 Idx
    //
    CondBlock = IfBlock->splitBasicBlock(InsertPt, "cond.load");
    Builder.SetInsertPoint(InsertPt);

    Value *Ptr = Builder.CreateExtractElement(Ptrs, Builder.getInt32(Idx),
                                              "Ptr" + Twine(Idx));
    LoadInst *Load =
        Builder.CreateAlignedLoad(Ptr, AlignVal, "Load" + Twine(Idx));
    VResult = Builder.CreateInsertElement(VResult, Load, Builder.getInt32(Idx),
                                          "Res" + Twine(Idx));

    // Create "else" block, fill it in the next iteration
    BasicBlock *NewIfBlock = CondBlock->splitBasicBlock(InsertPt, "else");
    Builder.SetInsertPoint(InsertPt);
    Instruction *OldBr = IfBlock->getTerminator();
    BranchInst::Create(CondBlock, NewIfBlock, Cmp, OldBr);
    OldBr->eraseFromParent();
    PrevIfBlock = IfBlock;
    IfBlock = NewIfBlock;
  }

  Phi = Builder.CreatePHI(VecType, 2, "res.phi.select");
  Phi->addIncoming(VResult, CondBlock);
  Phi->addIncoming(PrevPhi, PrevIfBlock);
  Value *NewI = Builder.CreateSelect(Mask, Phi, Src0);
  CI->replaceAllUsesWith(NewI);
  CI->eraseFromParent();
}

// Translate a masked scatter intrinsic, like
// void @llvm.masked.scatter.v16i32(<16 x i32> %Src, <16 x i32*>* %Ptrs, i32 4,
//                                  <16 x i1> %Mask)
// to a chain of basic blocks, that stores element one-by-one if
// the appropriate mask bit is set.
//
// % Ptrs = getelementptr i32, i32* %ptr, <16 x i64> %ind
// % Mask0 = extractelement <16 x i1> % Mask, i32 0
// % ToStore0 = icmp eq i1 % Mask0, true
// br i1 %ToStore0, label %cond.store, label %else
//
// cond.store:
// % Elt0 = extractelement <16 x i32> %Src, i32 0
// % Ptr0 = extractelement <16 x i32*> %Ptrs, i32 0
// store i32 %Elt0, i32* % Ptr0, align 4
// br label %else
//
// else:
// % Mask1 = extractelement <16 x i1> % Mask, i32 1
// % ToStore1 = icmp eq i1 % Mask1, true
// br i1 % ToStore1, label %cond.store1, label %else2
//
// cond.store1:
// % Elt1 = extractelement <16 x i32> %Src, i32 1
// % Ptr1 = extractelement <16 x i32*> %Ptrs, i32 1
// store i32 % Elt1, i32* % Ptr1, align 4
// br label %else2
//   . . .
static void scalarizeMaskedScatter(CallInst *CI) {
  Value *Src = CI->getArgOperand(0);
  Value *Ptrs = CI->getArgOperand(1);
  Value *Alignment = CI->getArgOperand(2);
  Value *Mask = CI->getArgOperand(3);

  assert(isa<VectorType>(Src->getType()) &&
         "Unexpected data type in masked scatter intrinsic");
  assert(isa<VectorType>(Ptrs->getType()) &&
         isa<PointerType>(Ptrs->getType()->getVectorElementType()) &&
         "Vector of pointers is expected in masked scatter intrinsic");

  IRBuilder<> Builder(CI->getContext());
  Instruction *InsertPt = CI;
  BasicBlock *IfBlock = CI->getParent();
  Builder.SetInsertPoint(InsertPt);
  Builder.SetCurrentDebugLocation(CI->getDebugLoc());

  unsigned AlignVal = cast<ConstantInt>(Alignment)->getZExtValue();
  unsigned VectorWidth = Src->getType()->getVectorNumElements();

  // Shorten the way if the mask is a vector of constants.
  bool IsConstMask = isa<ConstantVector>(Mask);

  if (IsConstMask) {
    for (unsigned Idx = 0; Idx < VectorWidth; ++Idx) {
      if (cast<ConstantVector>(Mask)->getOperand(Idx)->isNullValue())
        continue;
      Value *OneElt = Builder.CreateExtractElement(Src, Builder.getInt32(Idx),
                                                   "Elt" + Twine(Idx));
      Value *Ptr = Builder.CreateExtractElement(Ptrs, Builder.getInt32(Idx),
                                                "Ptr" + Twine(Idx));
      Builder.CreateAlignedStore(OneElt, Ptr, AlignVal);
    }
    CI->eraseFromParent();
    return;
  }
  for (unsigned Idx = 0; Idx < VectorWidth; ++Idx) {
    // Fill the "else" block, created in the previous iteration
    //
    //  % Mask1 = extractelement <16 x i1> % Mask, i32 Idx
    //  % ToStore = icmp eq i1 % Mask1, true
    //  br i1 % ToStore, label %cond.store, label %else
    //
    Value *Predicate = Builder.CreateExtractElement(Mask, Builder.getInt32(Idx),
                                                    "Mask" + Twine(Idx));
    Value *Cmp = Builder.CreateICmp(ICmpInst::ICMP_EQ, Predicate,
                                    ConstantInt::get(Predicate->getType(), 1),
                                    "ToStore" + Twine(Idx));

    // Create "cond" block
    //
    //  % Elt1 = extractelement <16 x i32> %Src, i32 1
    //  % Ptr1 = extractelement <16 x i32*> %Ptrs, i32 1
    //  %store i32 % Elt1, i32* % Ptr1
    //
    BasicBlock *CondBlock = IfBlock->splitBasicBlock(InsertPt, "cond.store");
    Builder.SetInsertPoint(InsertPt);

    Value *OneElt = Builder.CreateExtractElement(Src, Builder.getInt32(Idx),
                                                 "Elt" + Twine(Idx));
    Value *Ptr = Builder.CreateExtractElement(Ptrs, Builder.getInt32(Idx),
                                              "Ptr" + Twine(Idx));
    Builder.CreateAlignedStore(OneElt, Ptr, AlignVal);

    // Create "else" block, fill it in the next iteration
    BasicBlock *NewIfBlock = CondBlock->splitBasicBlock(InsertPt, "else");
    Builder.SetInsertPoint(InsertPt);
    Instruction *OldBr = IfBlock->getTerminator();
    BranchInst::Create(CondBlock, NewIfBlock, Cmp, OldBr);
    OldBr->eraseFromParent();
    IfBlock = NewIfBlock;
  }
  CI->eraseFromParent();
}

bool ScalarizeMaskedMemIntrin::runOnFunction(Function &F) {
  if (skipFunction(F))
    return false;

  bool EverMadeChange = false;

  TTI = &getAnalysis<TargetTransformInfoWrapperPass>().getTTI(F);

  bool MadeChange = true;
  while (MadeChange) {
    MadeChange = false;
    for (Function::iterator I = F.begin(); I != F.end();) {
      BasicBlock *BB = &*I++;
      bool ModifiedDTOnIteration = false;
      MadeChange |= optimizeBlock(*BB, ModifiedDTOnIteration);

      // Restart BB iteration if the dominator tree of the Function was changed
      if (ModifiedDTOnIteration)
        break;
    }

    EverMadeChange |= MadeChange;
  }

  return EverMadeChange;
}

bool ScalarizeMaskedMemIntrin::optimizeBlock(BasicBlock &BB, bool &ModifiedDT) {
  bool MadeChange = false;

  BasicBlock::iterator CurInstIterator = BB.begin();
  while (CurInstIterator != BB.end()) {
    if (CallInst *CI = dyn_cast<CallInst>(&*CurInstIterator++))
      MadeChange |= optimizeCallInst(CI, ModifiedDT);
    if (ModifiedDT)
      return true;
  }

  return MadeChange;
}

bool ScalarizeMaskedMemIntrin::optimizeCallInst(CallInst *CI,
                                                bool &ModifiedDT) {

  IntrinsicInst *II = dyn_cast<IntrinsicInst>(CI);
  if (II) {
    switch (II->getIntrinsicID()) {
    default:
      break;
    case Intrinsic::masked_load: {
      // Scalarize unsupported vector masked load
      if (!TTI->isLegalMaskedLoad(CI->getType())) {
        scalarizeMaskedLoad(CI);
        ModifiedDT = true;
        return true;
      }
      return false;
    }
    case Intrinsic::masked_store: {
      if (!TTI->isLegalMaskedStore(CI->getArgOperand(0)->getType())) {
        scalarizeMaskedStore(CI);
        ModifiedDT = true;
        return true;
      }
      return false;
    }
    case Intrinsic::masked_gather: {
      if (!TTI->isLegalMaskedGather(CI->getType())) {
        scalarizeMaskedGather(CI);
        ModifiedDT = true;
        return true;
      }
      return false;
    }
    case Intrinsic::masked_scatter: {
      if (!TTI->isLegalMaskedScatter(CI->getArgOperand(0)->getType())) {
        scalarizeMaskedScatter(CI);
        ModifiedDT = true;
        return true;
      }
      return false;
    }
    }
  }

  return false;
}
