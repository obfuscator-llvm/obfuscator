//===- ParallelDSP.cpp - Parallel DSP Pass --------------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
/// \file
/// Armv6 introduced instructions to perform 32-bit SIMD operations. The
/// purpose of this pass is do some IR pattern matching to create ACLE
/// DSP intrinsics, which map on these 32-bit SIMD operations.
/// This pass runs only when unaligned accesses is supported/enabled.
//
//===----------------------------------------------------------------------===//

#include "llvm/ADT/Statistic.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/Analysis/LoopAccessAnalysis.h"
#include "llvm/Analysis/LoopPass.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/NoFolder.h"
#include "llvm/Transforms/Scalar.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include "llvm/Transforms/Utils/LoopUtils.h"
#include "llvm/Pass.h"
#include "llvm/PassRegistry.h"
#include "llvm/PassSupport.h"
#include "llvm/Support/Debug.h"
#include "llvm/IR/PatternMatch.h"
#include "llvm/CodeGen/TargetPassConfig.h"
#include "ARM.h"
#include "ARMSubtarget.h"

using namespace llvm;
using namespace PatternMatch;

#define DEBUG_TYPE "arm-parallel-dsp"

STATISTIC(NumSMLAD , "Number of smlad instructions generated");

static cl::opt<bool>
DisableParallelDSP("disable-arm-parallel-dsp", cl::Hidden, cl::init(false),
                   cl::desc("Disable the ARM Parallel DSP pass"));

namespace {
  struct OpChain;
  struct BinOpChain;
  class Reduction;

  using OpChainList     = SmallVector<std::unique_ptr<OpChain>, 8>;
  using ReductionList   = SmallVector<Reduction, 8>;
  using ValueList       = SmallVector<Value*, 8>;
  using MemInstList     = SmallVector<LoadInst*, 8>;
  using PMACPair        = std::pair<BinOpChain*,BinOpChain*>;
  using PMACPairList    = SmallVector<PMACPair, 8>;
  using Instructions    = SmallVector<Instruction*,16>;
  using MemLocList      = SmallVector<MemoryLocation, 4>;

  struct OpChain {
    Instruction   *Root;
    ValueList     AllValues;
    MemInstList   VecLd;    // List of all load instructions.
    MemInstList   Loads;
    bool          ReadOnly = true;

    OpChain(Instruction *I, ValueList &vl) : Root(I), AllValues(vl) { }
    virtual ~OpChain() = default;

    void PopulateLoads() {
      for (auto *V : AllValues) {
        if (auto *Ld = dyn_cast<LoadInst>(V))
          Loads.push_back(Ld);
      }
    }

    unsigned size() const { return AllValues.size(); }
  };

  // 'BinOpChain' holds the multiplication instructions that are candidates
  // for parallel execution.
  struct BinOpChain : public OpChain {
    ValueList     LHS;      // List of all (narrow) left hand operands.
    ValueList     RHS;      // List of all (narrow) right hand operands.
    bool Exchange = false;

    BinOpChain(Instruction *I, ValueList &lhs, ValueList &rhs) :
      OpChain(I, lhs), LHS(lhs), RHS(rhs) {
        for (auto *V : RHS)
          AllValues.push_back(V);
      }

    bool AreSymmetrical(BinOpChain *Other);
  };

  /// Represent a sequence of multiply-accumulate operations with the aim to
  /// perform the multiplications in parallel.
  class Reduction {
    Instruction     *Root = nullptr;
    Value           *Acc = nullptr;
    OpChainList     Muls;
    PMACPairList        MulPairs;
    SmallPtrSet<Instruction*, 4> Adds;

  public:
    Reduction() = delete;

    Reduction (Instruction *Add) : Root(Add) { }

    /// Record an Add instruction that is a part of the this reduction.
    void InsertAdd(Instruction *I) { Adds.insert(I); }

    /// Record a BinOpChain, rooted at a Mul instruction, that is a part of
    /// this reduction.
    void InsertMul(Instruction *I, ValueList &LHS, ValueList &RHS) {
      Muls.push_back(make_unique<BinOpChain>(I, LHS, RHS));
    }

    /// Add the incoming accumulator value, returns true if a value had not
    /// already been added. Returning false signals to the user that this
    /// reduction already has a value to initialise the accumulator.
    bool InsertAcc(Value *V) {
      if (Acc)
        return false;
      Acc = V;
      return true;
    }

    /// Set two BinOpChains, rooted at muls, that can be executed as a single
    /// parallel operation.
    void AddMulPair(BinOpChain *Mul0, BinOpChain *Mul1) {
      MulPairs.push_back(std::make_pair(Mul0, Mul1));
    }

    /// Return true if enough mul operations are found that can be executed in
    /// parallel.
    bool CreateParallelPairs();

    /// Return the add instruction which is the root of the reduction.
    Instruction *getRoot() { return Root; }

    /// Return the incoming value to be accumulated. This maybe null.
    Value *getAccumulator() { return Acc; }

    /// Return the set of adds that comprise the reduction.
    SmallPtrSetImpl<Instruction*> &getAdds() { return Adds; }

    /// Return the BinOpChain, rooted at mul instruction, that comprise the
    /// the reduction.
    OpChainList &getMuls() { return Muls; }

    /// Return the BinOpChain, rooted at mul instructions, that have been
    /// paired for parallel execution.
    PMACPairList &getMulPairs() { return MulPairs; }

    /// To finalise, replace the uses of the root with the intrinsic call.
    void UpdateRoot(Instruction *SMLAD) {
      Root->replaceAllUsesWith(SMLAD);
    }
  };

  class WidenedLoad {
    LoadInst *NewLd = nullptr;
    SmallVector<LoadInst*, 4> Loads;

  public:
    WidenedLoad(SmallVectorImpl<LoadInst*> &Lds, LoadInst *Wide)
      : NewLd(Wide) {
      for (auto *I : Lds)
        Loads.push_back(I);
    }
    LoadInst *getLoad() {
      return NewLd;
    }
  };

  class ARMParallelDSP : public LoopPass {
    ScalarEvolution   *SE;
    AliasAnalysis     *AA;
    TargetLibraryInfo *TLI;
    DominatorTree     *DT;
    LoopInfo          *LI;
    Loop              *L;
    const DataLayout  *DL;
    Module            *M;
    std::map<LoadInst*, LoadInst*> LoadPairs;
    SmallPtrSet<LoadInst*, 4> OffsetLoads;
    std::map<LoadInst*, std::unique_ptr<WidenedLoad>> WideLoads;

    template<unsigned>
    bool IsNarrowSequence(Value *V, ValueList &VL);

    bool RecordMemoryOps(BasicBlock *BB);
    void InsertParallelMACs(Reduction &Reduction);
    bool AreSequentialLoads(LoadInst *Ld0, LoadInst *Ld1, MemInstList &VecMem);
    LoadInst* CreateWideLoad(SmallVectorImpl<LoadInst*> &Loads,
                             IntegerType *LoadTy);
    bool CreateParallelPairs(Reduction &R);

    /// Try to match and generate: SMLAD, SMLADX - Signed Multiply Accumulate
    /// Dual performs two signed 16x16-bit multiplications. It adds the
    /// products to a 32-bit accumulate operand. Optionally, the instruction can
    /// exchange the halfwords of the second operand before performing the
    /// arithmetic.
    bool MatchSMLAD(Loop *L);

  public:
    static char ID;

    ARMParallelDSP() : LoopPass(ID) { }

    bool doInitialization(Loop *L, LPPassManager &LPM) override {
      LoadPairs.clear();
      WideLoads.clear();
      return true;
    }

    void getAnalysisUsage(AnalysisUsage &AU) const override {
      LoopPass::getAnalysisUsage(AU);
      AU.addRequired<AssumptionCacheTracker>();
      AU.addRequired<ScalarEvolutionWrapperPass>();
      AU.addRequired<AAResultsWrapperPass>();
      AU.addRequired<TargetLibraryInfoWrapperPass>();
      AU.addRequired<LoopInfoWrapperPass>();
      AU.addRequired<DominatorTreeWrapperPass>();
      AU.addRequired<TargetPassConfig>();
      AU.addPreserved<LoopInfoWrapperPass>();
      AU.setPreservesCFG();
    }

    bool runOnLoop(Loop *TheLoop, LPPassManager &) override {
      if (DisableParallelDSP)
        return false;
      L = TheLoop;
      SE = &getAnalysis<ScalarEvolutionWrapperPass>().getSE();
      AA = &getAnalysis<AAResultsWrapperPass>().getAAResults();
      TLI = &getAnalysis<TargetLibraryInfoWrapperPass>().getTLI();
      DT = &getAnalysis<DominatorTreeWrapperPass>().getDomTree();
      LI = &getAnalysis<LoopInfoWrapperPass>().getLoopInfo();
      auto &TPC = getAnalysis<TargetPassConfig>();

      BasicBlock *Header = TheLoop->getHeader();
      if (!Header)
        return false;

      // TODO: We assume the loop header and latch to be the same block.
      // This is not a fundamental restriction, but lifting this would just
      // require more work to do the transformation and then patch up the CFG.
      if (Header != TheLoop->getLoopLatch()) {
        LLVM_DEBUG(dbgs() << "The loop header is not the loop latch: not "
                             "running pass ARMParallelDSP\n");
        return false;
      }

      if (!TheLoop->getLoopPreheader())
        InsertPreheaderForLoop(L, DT, LI, nullptr, true);

      Function &F = *Header->getParent();
      M = F.getParent();
      DL = &M->getDataLayout();

      auto &TM = TPC.getTM<TargetMachine>();
      auto *ST = &TM.getSubtarget<ARMSubtarget>(F);

      if (!ST->allowsUnalignedMem()) {
        LLVM_DEBUG(dbgs() << "Unaligned memory access not supported: not "
                             "running pass ARMParallelDSP\n");
        return false;
      }

      if (!ST->hasDSP()) {
        LLVM_DEBUG(dbgs() << "DSP extension not enabled: not running pass "
                             "ARMParallelDSP\n");
        return false;
      }

      if (!ST->isLittle()) {
        LLVM_DEBUG(dbgs() << "Only supporting little endian: not running pass "
                          << "ARMParallelDSP\n");
        return false;
      }

      LoopAccessInfo LAI(L, SE, TLI, AA, DT, LI);

      LLVM_DEBUG(dbgs() << "\n== Parallel DSP pass ==\n");
      LLVM_DEBUG(dbgs() << " - " << F.getName() << "\n\n");

      if (!RecordMemoryOps(Header)) {
        LLVM_DEBUG(dbgs() << " - No sequential loads found.\n");
        return false;
      }

      bool Changes = MatchSMLAD(L);
      return Changes;
    }
  };
}

template<typename MemInst>
static bool AreSequentialAccesses(MemInst *MemOp0, MemInst *MemOp1,
                                  const DataLayout &DL, ScalarEvolution &SE) {
  if (isConsecutiveAccess(MemOp0, MemOp1, DL, SE))
    return true;
  return false;
}

bool ARMParallelDSP::AreSequentialLoads(LoadInst *Ld0, LoadInst *Ld1,
                                        MemInstList &VecMem) {
  if (!Ld0 || !Ld1)
    return false;

  if (!LoadPairs.count(Ld0) || LoadPairs[Ld0] != Ld1)
    return false;

  LLVM_DEBUG(dbgs() << "Loads are sequential and valid:\n";
    dbgs() << "Ld0:"; Ld0->dump();
    dbgs() << "Ld1:"; Ld1->dump();
  );

  VecMem.clear();
  VecMem.push_back(Ld0);
  VecMem.push_back(Ld1);
  return true;
}

// MaxBitwidth: the maximum supported bitwidth of the elements in the DSP
// instructions, which is set to 16. So here we should collect all i8 and i16
// narrow operations.
// TODO: we currently only collect i16, and will support i8 later, so that's
// why we check that types are equal to MaxBitWidth, and not <= MaxBitWidth.
template<unsigned MaxBitWidth>
bool ARMParallelDSP::IsNarrowSequence(Value *V, ValueList &VL) {
  ConstantInt *CInt;

  if (match(V, m_ConstantInt(CInt))) {
    // TODO: if a constant is used, it needs to fit within the bit width.
    return false;
  }

  auto *I = dyn_cast<Instruction>(V);
  if (!I)
    return false;

  Value *Val, *LHS, *RHS;
  if (match(V, m_Trunc(m_Value(Val)))) {
    if (cast<TruncInst>(I)->getDestTy()->getIntegerBitWidth() == MaxBitWidth)
      return IsNarrowSequence<MaxBitWidth>(Val, VL);
  } else if (match(V, m_Add(m_Value(LHS), m_Value(RHS)))) {
    // TODO: we need to implement sadd16/sadd8 for this, which enables to
    // also do the rewrite for smlad8.ll, but it is unsupported for now.
    return false;
  } else if (match(V, m_ZExtOrSExt(m_Value(Val)))) {
    if (cast<CastInst>(I)->getSrcTy()->getIntegerBitWidth() != MaxBitWidth)
      return false;

    if (match(Val, m_Load(m_Value()))) {
      auto *Ld = cast<LoadInst>(Val);

      // Check that these load could be paired.
      if (!LoadPairs.count(Ld) && !OffsetLoads.count(Ld))
        return false;

      VL.push_back(Val);
      VL.push_back(I);
      return true;
    }
  }
  return false;
}

/// Iterate through the block and record base, offset pairs of loads which can
/// be widened into a single load.
bool ARMParallelDSP::RecordMemoryOps(BasicBlock *BB) {
  SmallVector<LoadInst*, 8> Loads;
  SmallVector<Instruction*, 8> Writes;

  // Collect loads and instruction that may write to memory. For now we only
  // record loads which are simple, sign-extended and have a single user.
  // TODO: Allow zero-extended loads.
  for (auto &I : *BB) {
    if (I.mayWriteToMemory())
      Writes.push_back(&I);
    auto *Ld = dyn_cast<LoadInst>(&I);
    if (!Ld || !Ld->isSimple() ||
        !Ld->hasOneUse() || !isa<SExtInst>(Ld->user_back()))
      continue;
    Loads.push_back(Ld);
  }

  using InstSet = std::set<Instruction*>;
  using DepMap = std::map<Instruction*, InstSet>;
  DepMap RAWDeps;

  // Record any writes that may alias a load.
  const auto Size = LocationSize::unknown();
  for (auto Read : Loads) {
    for (auto Write : Writes) {
      MemoryLocation ReadLoc =
        MemoryLocation(Read->getPointerOperand(), Size);

      if (!isModOrRefSet(intersectModRef(AA->getModRefInfo(Write, ReadLoc),
          ModRefInfo::ModRef)))
        continue;
      if (DT->dominates(Write, Read))
        RAWDeps[Read].insert(Write);
    }
  }

  // Check whether there's not a write between the two loads which would
  // prevent them from being safely merged.
  auto SafeToPair = [&](LoadInst *Base, LoadInst *Offset) {
    LoadInst *Dominator = DT->dominates(Base, Offset) ? Base : Offset;
    LoadInst *Dominated = DT->dominates(Base, Offset) ? Offset : Base;

    if (RAWDeps.count(Dominated)) {
      InstSet &WritesBefore = RAWDeps[Dominated];

      for (auto Before : WritesBefore) {

        // We can't move the second load backward, past a write, to merge
        // with the first load.
        if (DT->dominates(Dominator, Before))
          return false;
      }
    }
    return true;
  };

  // Record base, offset load pairs.
  for (auto *Base : Loads) {
    for (auto *Offset : Loads) {
      if (Base == Offset)
        continue;

      if (AreSequentialAccesses<LoadInst>(Base, Offset, *DL, *SE) &&
          SafeToPair(Base, Offset)) {
        LoadPairs[Base] = Offset;
        OffsetLoads.insert(Offset);
        break;
      }
    }
  }

  LLVM_DEBUG(if (!LoadPairs.empty()) {
               dbgs() << "Consecutive load pairs:\n";
               for (auto &MapIt : LoadPairs) {
                 LLVM_DEBUG(dbgs() << *MapIt.first << ", "
                            << *MapIt.second << "\n");
               }
             });
  return LoadPairs.size() > 1;
}

// Loop Pass that needs to identify integer add/sub reductions of 16-bit vector
// multiplications.
// To use SMLAD:
// 1) we first need to find integer add then look for this pattern:
//
// acc0 = ...
// ld0 = load i16
// sext0 = sext i16 %ld0 to i32
// ld1 = load i16
// sext1 = sext i16 %ld1 to i32
// mul0 = mul %sext0, %sext1
// ld2 = load i16
// sext2 = sext i16 %ld2 to i32
// ld3 = load i16
// sext3 = sext i16 %ld3 to i32
// mul1 = mul i32 %sext2, %sext3
// add0 = add i32 %mul0, %acc0
// acc1 = add i32 %add0, %mul1
//
// Which can be selected to:
//
// ldr r0
// ldr r1
// smlad r2, r0, r1, r2
//
// If constants are used instead of loads, these will need to be hoisted
// out and into a register.
//
// If loop invariants are used instead of loads, these need to be packed
// before the loop begins.
//
bool ARMParallelDSP::MatchSMLAD(Loop *L) {
  // Search recursively back through the operands to find a tree of values that
  // form a multiply-accumulate chain. The search records the Add and Mul
  // instructions that form the reduction and allows us to find a single value
  // to be used as the initial input to the accumlator.
  std::function<bool(Value*, Reduction&)> Search = [&]
    (Value *V, Reduction &R) -> bool {

    // If we find a non-instruction, try to use it as the initial accumulator
    // value. This may have already been found during the search in which case
    // this function will return false, signaling a search fail.
    auto *I = dyn_cast<Instruction>(V);
    if (!I)
      return R.InsertAcc(V);

    switch (I->getOpcode()) {
    default:
      break;
    case Instruction::PHI:
      // Could be the accumulator value.
      return R.InsertAcc(V);
    case Instruction::Add: {
      // Adds should be adding together two muls, or another add and a mul to
      // be within the mac chain. One of the operands may also be the
      // accumulator value at which point we should stop searching.
      bool ValidLHS = Search(I->getOperand(0), R);
      bool ValidRHS = Search(I->getOperand(1), R);
      if (!ValidLHS && !ValidLHS)
        return false;
      else if (ValidLHS && ValidRHS) {
        R.InsertAdd(I);
        return true;
      } else {
        R.InsertAdd(I);
        return R.InsertAcc(I);
      }
    }
    case Instruction::Mul: {
      Value *MulOp0 = I->getOperand(0);
      Value *MulOp1 = I->getOperand(1);
      if (isa<SExtInst>(MulOp0) && isa<SExtInst>(MulOp1)) {
        ValueList LHS;
        ValueList RHS;
        if (IsNarrowSequence<16>(MulOp0, LHS) &&
            IsNarrowSequence<16>(MulOp1, RHS)) {
          R.InsertMul(I, LHS, RHS);
          return true;
        }
      }
      return false;
    }
    case Instruction::SExt:
      return Search(I->getOperand(0), R);
    }
    return false;
  };

  bool Changed = false;
  SmallPtrSet<Instruction*, 4> AllAdds;
  BasicBlock *Latch = L->getLoopLatch();

  for (Instruction &I : reverse(*Latch)) {
    if (I.getOpcode() != Instruction::Add)
      continue;

    if (AllAdds.count(&I))
      continue;

    const auto *Ty = I.getType();
    if (!Ty->isIntegerTy(32) && !Ty->isIntegerTy(64))
      continue;

    Reduction R(&I);
    if (!Search(&I, R))
      continue;

    if (!CreateParallelPairs(R))
      continue;

    InsertParallelMACs(R);
    Changed = true;
    AllAdds.insert(R.getAdds().begin(), R.getAdds().end());
  }

  return Changed;
}

bool ARMParallelDSP::CreateParallelPairs(Reduction &R) {

  // Not enough mul operations to make a pair.
  if (R.getMuls().size() < 2)
    return false;

  // Check that the muls operate directly upon sign extended loads.
  for (auto &MulChain : R.getMuls()) {
    // A mul has 2 operands, and a narrow op consist of sext and a load; thus
    // we expect at least 4 items in this operand value list.
    if (MulChain->size() < 4) {
      LLVM_DEBUG(dbgs() << "Operand list too short.\n");
      return false;
    }
    MulChain->PopulateLoads();
    ValueList &LHS = static_cast<BinOpChain*>(MulChain.get())->LHS;
    ValueList &RHS = static_cast<BinOpChain*>(MulChain.get())->RHS;

    // Use +=2 to skip over the expected extend instructions.
    for (unsigned i = 0, e = LHS.size(); i < e; i += 2) {
      if (!isa<LoadInst>(LHS[i]) || !isa<LoadInst>(RHS[i]))
        return false;
    }
  }

  auto CanPair = [&](Reduction &R, BinOpChain *PMul0, BinOpChain *PMul1) {
    if (!PMul0->AreSymmetrical(PMul1))
      return false;

    // The first elements of each vector should be loads with sexts. If we
    // find that its two pairs of consecutive loads, then these can be
    // transformed into two wider loads and the users can be replaced with
    // DSP intrinsics.
    for (unsigned x = 0; x < PMul0->LHS.size(); x += 2) {
      auto *Ld0 = dyn_cast<LoadInst>(PMul0->LHS[x]);
      auto *Ld1 = dyn_cast<LoadInst>(PMul1->LHS[x]);
      auto *Ld2 = dyn_cast<LoadInst>(PMul0->RHS[x]);
      auto *Ld3 = dyn_cast<LoadInst>(PMul1->RHS[x]);

      if (!Ld0 || !Ld1 || !Ld2 || !Ld3)
        return false;

      LLVM_DEBUG(dbgs() << "Loads:\n"
                 << " - " << *Ld0 << "\n"
                 << " - " << *Ld1 << "\n"
                 << " - " << *Ld2 << "\n"
                 << " - " << *Ld3 << "\n");

      if (AreSequentialLoads(Ld0, Ld1, PMul0->VecLd)) {
        if (AreSequentialLoads(Ld2, Ld3, PMul1->VecLd)) {
          LLVM_DEBUG(dbgs() << "OK: found two pairs of parallel loads!\n");
          R.AddMulPair(PMul0, PMul1);
          return true;
        } else if (AreSequentialLoads(Ld3, Ld2, PMul1->VecLd)) {
          LLVM_DEBUG(dbgs() << "OK: found two pairs of parallel loads!\n");
          LLVM_DEBUG(dbgs() << "    exchanging Ld2 and Ld3\n");
          PMul1->Exchange = true;
          R.AddMulPair(PMul0, PMul1);
          return true;
        }
      } else if (AreSequentialLoads(Ld1, Ld0, PMul0->VecLd) &&
                 AreSequentialLoads(Ld2, Ld3, PMul1->VecLd)) {
        LLVM_DEBUG(dbgs() << "OK: found two pairs of parallel loads!\n");
        LLVM_DEBUG(dbgs() << "    exchanging Ld0 and Ld1\n");
        LLVM_DEBUG(dbgs() << "    and swapping muls\n");
        PMul0->Exchange = true;
        // Only the second operand can be exchanged, so swap the muls.
        R.AddMulPair(PMul1, PMul0);
        return true;
      }
    }
    return false;
  };

  OpChainList &Muls = R.getMuls();
  const unsigned Elems = Muls.size();
  SmallPtrSet<const Instruction*, 4> Paired;
  for (unsigned i = 0; i < Elems; ++i) {
    BinOpChain *PMul0 = static_cast<BinOpChain*>(Muls[i].get());
    if (Paired.count(PMul0->Root))
      continue;

    for (unsigned j = 0; j < Elems; ++j) {
      if (i == j)
        continue;

      BinOpChain *PMul1 = static_cast<BinOpChain*>(Muls[j].get());
      if (Paired.count(PMul1->Root))
        continue;

      const Instruction *Mul0 = PMul0->Root;
      const Instruction *Mul1 = PMul1->Root;
      if (Mul0 == Mul1)
        continue;

      assert(PMul0 != PMul1 && "expected different chains");

      if (CanPair(R, PMul0, PMul1)) {
        Paired.insert(Mul0);
        Paired.insert(Mul1);
        break;
      }
    }
  }
  return !R.getMulPairs().empty();
}


void ARMParallelDSP::InsertParallelMACs(Reduction &R) {

  auto CreateSMLADCall = [&](SmallVectorImpl<LoadInst*> &VecLd0,
                             SmallVectorImpl<LoadInst*> &VecLd1,
                             Value *Acc, bool Exchange,
                             Instruction *InsertAfter) {
    // Replace the reduction chain with an intrinsic call
    IntegerType *Ty = IntegerType::get(M->getContext(), 32);
    LoadInst *WideLd0 = WideLoads.count(VecLd0[0]) ?
      WideLoads[VecLd0[0]]->getLoad() : CreateWideLoad(VecLd0, Ty);
    LoadInst *WideLd1 = WideLoads.count(VecLd1[0]) ?
      WideLoads[VecLd1[0]]->getLoad() : CreateWideLoad(VecLd1, Ty);

    Value* Args[] = { WideLd0, WideLd1, Acc };
    Function *SMLAD = nullptr;
    if (Exchange)
      SMLAD = Acc->getType()->isIntegerTy(32) ?
        Intrinsic::getDeclaration(M, Intrinsic::arm_smladx) :
        Intrinsic::getDeclaration(M, Intrinsic::arm_smlaldx);
    else
      SMLAD = Acc->getType()->isIntegerTy(32) ?
        Intrinsic::getDeclaration(M, Intrinsic::arm_smlad) :
        Intrinsic::getDeclaration(M, Intrinsic::arm_smlald);

    IRBuilder<NoFolder> Builder(InsertAfter->getParent(),
                                ++BasicBlock::iterator(InsertAfter));
    Instruction *Call = Builder.CreateCall(SMLAD, Args);
    NumSMLAD++;
    return Call;
  };

  Instruction *InsertAfter = R.getRoot();
  Value *Acc = R.getAccumulator();
  if (!Acc)
    Acc = ConstantInt::get(IntegerType::get(M->getContext(), 32), 0);

  LLVM_DEBUG(dbgs() << "Root: " << *InsertAfter << "\n"
             << "Acc: " << *Acc << "\n");
  for (auto &Pair : R.getMulPairs()) {
    BinOpChain *PMul0 = Pair.first;
    BinOpChain *PMul1 = Pair.second;
    LLVM_DEBUG(dbgs() << "Muls:\n"
               << "- " << *PMul0->Root << "\n"
               << "- " << *PMul1->Root << "\n");

    Acc = CreateSMLADCall(PMul0->VecLd, PMul1->VecLd, Acc, PMul1->Exchange,
                          InsertAfter);
    InsertAfter = cast<Instruction>(Acc);
  }
  R.UpdateRoot(cast<Instruction>(Acc));
}

LoadInst* ARMParallelDSP::CreateWideLoad(SmallVectorImpl<LoadInst*> &Loads,
                                         IntegerType *LoadTy) {
  assert(Loads.size() == 2 && "currently only support widening two loads");

  LoadInst *Base = Loads[0];
  LoadInst *Offset = Loads[1];

  Instruction *BaseSExt = dyn_cast<SExtInst>(Base->user_back());
  Instruction *OffsetSExt = dyn_cast<SExtInst>(Offset->user_back());

  assert((BaseSExt && OffsetSExt)
         && "Loads should have a single, extending, user");

  std::function<void(Value*, Value*)> MoveBefore =
    [&](Value *A, Value *B) -> void {
      if (!isa<Instruction>(A) || !isa<Instruction>(B))
        return;

      auto *Source = cast<Instruction>(A);
      auto *Sink = cast<Instruction>(B);

      if (DT->dominates(Source, Sink) ||
          Source->getParent() != Sink->getParent() ||
          isa<PHINode>(Source) || isa<PHINode>(Sink))
        return;

      Source->moveBefore(Sink);
      for (auto &U : Source->uses())
        MoveBefore(Source, U.getUser());
    };

  // Insert the load at the point of the original dominating load.
  LoadInst *DomLoad = DT->dominates(Base, Offset) ? Base : Offset;
  IRBuilder<NoFolder> IRB(DomLoad->getParent(),
                          ++BasicBlock::iterator(DomLoad));

  // Bitcast the pointer to a wider type and create the wide load, while making
  // sure to maintain the original alignment as this prevents ldrd from being
  // generated when it could be illegal due to memory alignment.
  const unsigned AddrSpace = DomLoad->getPointerAddressSpace();
  Value *VecPtr = IRB.CreateBitCast(Base->getPointerOperand(),
                                    LoadTy->getPointerTo(AddrSpace));
  LoadInst *WideLoad = IRB.CreateAlignedLoad(LoadTy, VecPtr,
                                             Base->getAlignment());

  // Make sure everything is in the correct order in the basic block.
  MoveBefore(Base->getPointerOperand(), VecPtr);
  MoveBefore(VecPtr, WideLoad);

  // From the wide load, create two values that equal the original two loads.
  // Loads[0] needs trunc while Loads[1] needs a lshr and trunc.
  // TODO: Support big-endian as well.
  Value *Bottom = IRB.CreateTrunc(WideLoad, Base->getType());
  BaseSExt->setOperand(0, Bottom);

  IntegerType *OffsetTy = cast<IntegerType>(Offset->getType());
  Value *ShiftVal = ConstantInt::get(LoadTy, OffsetTy->getBitWidth());
  Value *Top = IRB.CreateLShr(WideLoad, ShiftVal);
  Value *Trunc = IRB.CreateTrunc(Top, OffsetTy);
  OffsetSExt->setOperand(0, Trunc);

  WideLoads.emplace(std::make_pair(Base,
                                   make_unique<WidenedLoad>(Loads, WideLoad)));
  return WideLoad;
}

// Compare the value lists in Other to this chain.
bool BinOpChain::AreSymmetrical(BinOpChain *Other) {
  // Element-by-element comparison of Value lists returning true if they are
  // instructions with the same opcode or constants with the same value.
  auto CompareValueList = [](const ValueList &VL0,
                             const ValueList &VL1) {
    if (VL0.size() != VL1.size()) {
      LLVM_DEBUG(dbgs() << "Muls are mismatching operand list lengths: "
                        << VL0.size() << " != " << VL1.size() << "\n");
      return false;
    }

    const unsigned Pairs = VL0.size();

    for (unsigned i = 0; i < Pairs; ++i) {
      const Value *V0 = VL0[i];
      const Value *V1 = VL1[i];
      const auto *Inst0 = dyn_cast<Instruction>(V0);
      const auto *Inst1 = dyn_cast<Instruction>(V1);

      if (!Inst0 || !Inst1)
        return false;

      if (Inst0->isSameOperationAs(Inst1))
        continue;

      const APInt *C0, *C1;
      if (!(match(V0, m_APInt(C0)) && match(V1, m_APInt(C1)) && C0 == C1))
        return false;
    }

    return true;
  };

  return CompareValueList(LHS, Other->LHS) &&
         CompareValueList(RHS, Other->RHS);
}

Pass *llvm::createARMParallelDSPPass() {
  return new ARMParallelDSP();
}

char ARMParallelDSP::ID = 0;

INITIALIZE_PASS_BEGIN(ARMParallelDSP, "arm-parallel-dsp",
                "Transform loops to use DSP intrinsics", false, false)
INITIALIZE_PASS_END(ARMParallelDSP, "arm-parallel-dsp",
                "Transform loops to use DSP intrinsics", false, false)
