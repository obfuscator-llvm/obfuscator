//===- CodeGenPrepare.cpp - Prepare a function for code generation --------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This pass munges the code in the input function to better prepare it for
// SelectionDAG-based code generation. This works around limitations in it's
// basic-block-at-a-time approach. It should eventually be removed.
//
//===----------------------------------------------------------------------===//

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/ADT/SmallSet.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/Analysis/BlockFrequencyInfo.h"
#include "llvm/Analysis/BranchProbabilityInfo.h"
#include "llvm/Analysis/CFG.h"
#include "llvm/Analysis/InstructionSimplify.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/MemoryBuiltins.h"
#include "llvm/Analysis/ProfileSummaryInfo.h"
#include "llvm/Analysis/TargetLibraryInfo.h"
#include "llvm/Analysis/TargetTransformInfo.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/CodeGen/Analysis.h"
#include "llvm/CodeGen/Passes.h"
#include "llvm/CodeGen/TargetPassConfig.h"
#include "llvm/IR/CallSite.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GetElementPtrTypeIterator.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/MDBuilder.h"
#include "llvm/IR/PatternMatch.h"
#include "llvm/IR/Statepoint.h"
#include "llvm/IR/ValueHandle.h"
#include "llvm/IR/ValueMap.h"
#include "llvm/Pass.h"
#include "llvm/Support/BranchProbability.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Target/TargetLowering.h"
#include "llvm/Target/TargetSubtargetInfo.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include "llvm/Transforms/Utils/BuildLibCalls.h"
#include "llvm/Transforms/Utils/BypassSlowDivision.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include "llvm/Transforms/Utils/Local.h"
#include "llvm/Transforms/Utils/SimplifyLibCalls.h"
#include "llvm/Transforms/Utils/ValueMapper.h"

using namespace llvm;
using namespace llvm::PatternMatch;

#define DEBUG_TYPE "codegenprepare"

STATISTIC(NumBlocksElim, "Number of blocks eliminated");
STATISTIC(NumPHIsElim,   "Number of trivial PHIs eliminated");
STATISTIC(NumGEPsElim,   "Number of GEPs converted to casts");
STATISTIC(NumCmpUses, "Number of uses of Cmp expressions replaced with uses of "
                      "sunken Cmps");
STATISTIC(NumCastUses, "Number of uses of Cast expressions replaced with uses "
                       "of sunken Casts");
STATISTIC(NumMemoryInsts, "Number of memory instructions whose address "
                          "computations were sunk");
STATISTIC(NumExtsMoved,  "Number of [s|z]ext instructions combined with loads");
STATISTIC(NumExtUses,    "Number of uses of [s|z]ext instructions optimized");
STATISTIC(NumAndsAdded,
          "Number of and mask instructions added to form ext loads");
STATISTIC(NumAndUses, "Number of uses of and mask instructions optimized");
STATISTIC(NumRetsDup,    "Number of return instructions duplicated");
STATISTIC(NumDbgValueMoved, "Number of debug value instructions moved");
STATISTIC(NumSelectsExpanded, "Number of selects turned into branches");
STATISTIC(NumStoreExtractExposed, "Number of store(extractelement) exposed");

STATISTIC(NumMemCmpCalls, "Number of memcmp calls");
STATISTIC(NumMemCmpNotConstant, "Number of memcmp calls without constant size");
STATISTIC(NumMemCmpGreaterThanMax,
          "Number of memcmp calls with size greater than max size");
STATISTIC(NumMemCmpInlined, "Number of inlined memcmp calls");

static cl::opt<bool> DisableBranchOpts(
  "disable-cgp-branch-opts", cl::Hidden, cl::init(false),
  cl::desc("Disable branch optimizations in CodeGenPrepare"));

static cl::opt<bool>
    DisableGCOpts("disable-cgp-gc-opts", cl::Hidden, cl::init(false),
                  cl::desc("Disable GC optimizations in CodeGenPrepare"));

static cl::opt<bool> DisableSelectToBranch(
  "disable-cgp-select2branch", cl::Hidden, cl::init(false),
  cl::desc("Disable select to branch conversion."));

static cl::opt<bool> AddrSinkUsingGEPs(
  "addr-sink-using-gep", cl::Hidden, cl::init(true),
  cl::desc("Address sinking in CGP using GEPs."));

static cl::opt<bool> EnableAndCmpSinking(
   "enable-andcmp-sinking", cl::Hidden, cl::init(true),
   cl::desc("Enable sinkinig and/cmp into branches."));

static cl::opt<bool> DisableStoreExtract(
    "disable-cgp-store-extract", cl::Hidden, cl::init(false),
    cl::desc("Disable store(extract) optimizations in CodeGenPrepare"));

static cl::opt<bool> StressStoreExtract(
    "stress-cgp-store-extract", cl::Hidden, cl::init(false),
    cl::desc("Stress test store(extract) optimizations in CodeGenPrepare"));

static cl::opt<bool> DisableExtLdPromotion(
    "disable-cgp-ext-ld-promotion", cl::Hidden, cl::init(false),
    cl::desc("Disable ext(promotable(ld)) -> promoted(ext(ld)) optimization in "
             "CodeGenPrepare"));

static cl::opt<bool> StressExtLdPromotion(
    "stress-cgp-ext-ld-promotion", cl::Hidden, cl::init(false),
    cl::desc("Stress test ext(promotable(ld)) -> promoted(ext(ld)) "
             "optimization in CodeGenPrepare"));

static cl::opt<bool> DisablePreheaderProtect(
    "disable-preheader-prot", cl::Hidden, cl::init(false),
    cl::desc("Disable protection against removing loop preheaders"));

static cl::opt<bool> ProfileGuidedSectionPrefix(
    "profile-guided-section-prefix", cl::Hidden, cl::init(true), cl::ZeroOrMore,
    cl::desc("Use profile info to add section prefix for hot/cold functions"));

static cl::opt<unsigned> FreqRatioToSkipMerge(
    "cgp-freq-ratio-to-skip-merge", cl::Hidden, cl::init(2),
    cl::desc("Skip merging empty blocks if (frequency of empty block) / "
             "(frequency of destination block) is greater than this ratio"));

static cl::opt<bool> ForceSplitStore(
    "force-split-store", cl::Hidden, cl::init(false),
    cl::desc("Force store splitting no matter what the target query says."));

static cl::opt<bool>
EnableTypePromotionMerge("cgp-type-promotion-merge", cl::Hidden,
    cl::desc("Enable merging of redundant sexts when one is dominating"
    " the other."), cl::init(true));

static cl::opt<unsigned> MemCmpNumLoadsPerBlock(
    "memcmp-num-loads-per-block", cl::Hidden, cl::init(1),
    cl::desc("The number of loads per basic block for inline expansion of "
             "memcmp that is only being compared against zero."));

namespace {
typedef SmallPtrSet<Instruction *, 16> SetOfInstrs;
typedef PointerIntPair<Type *, 1, bool> TypeIsSExt;
typedef DenseMap<Instruction *, TypeIsSExt> InstrToOrigTy;
typedef SmallVector<Instruction *, 16> SExts;
typedef DenseMap<Value *, SExts> ValueToSExts;
class TypePromotionTransaction;

  class CodeGenPrepare : public FunctionPass {
    const TargetMachine *TM;
    const TargetSubtargetInfo *SubtargetInfo;
    const TargetLowering *TLI;
    const TargetRegisterInfo *TRI;
    const TargetTransformInfo *TTI;
    const TargetLibraryInfo *TLInfo;
    const LoopInfo *LI;
    std::unique_ptr<BlockFrequencyInfo> BFI;
    std::unique_ptr<BranchProbabilityInfo> BPI;

    /// As we scan instructions optimizing them, this is the next instruction
    /// to optimize. Transforms that can invalidate this should update it.
    BasicBlock::iterator CurInstIterator;

    /// Keeps track of non-local addresses that have been sunk into a block.
    /// This allows us to avoid inserting duplicate code for blocks with
    /// multiple load/stores of the same address.
    ValueMap<Value*, Value*> SunkAddrs;

    /// Keeps track of all instructions inserted for the current function.
    SetOfInstrs InsertedInsts;
    /// Keeps track of the type of the related instruction before their
    /// promotion for the current function.
    InstrToOrigTy PromotedInsts;

    /// Keep track of instructions removed during promotion.
    SetOfInstrs RemovedInsts;

    /// Keep track of sext chains based on their initial value.
    DenseMap<Value *, Instruction *> SeenChainsForSExt;

    /// Keep track of SExt promoted.
    ValueToSExts ValToSExtendedUses;

    /// True if CFG is modified in any way.
    bool ModifiedDT;

    /// True if optimizing for size.
    bool OptSize;

    /// DataLayout for the Function being processed.
    const DataLayout *DL;

  public:
    static char ID; // Pass identification, replacement for typeid
    CodeGenPrepare()
        : FunctionPass(ID), TM(nullptr), TLI(nullptr), TTI(nullptr),
          DL(nullptr) {
      initializeCodeGenPreparePass(*PassRegistry::getPassRegistry());
    }
    bool runOnFunction(Function &F) override;

    StringRef getPassName() const override { return "CodeGen Prepare"; }

    void getAnalysisUsage(AnalysisUsage &AU) const override {
      // FIXME: When we can selectively preserve passes, preserve the domtree.
      AU.addRequired<ProfileSummaryInfoWrapperPass>();
      AU.addRequired<TargetLibraryInfoWrapperPass>();
      AU.addRequired<TargetTransformInfoWrapperPass>();
      AU.addRequired<LoopInfoWrapperPass>();
    }

  private:
    bool eliminateFallThrough(Function &F);
    bool eliminateMostlyEmptyBlocks(Function &F);
    BasicBlock *findDestBlockOfMergeableEmptyBlock(BasicBlock *BB);
    bool canMergeBlocks(const BasicBlock *BB, const BasicBlock *DestBB) const;
    void eliminateMostlyEmptyBlock(BasicBlock *BB);
    bool isMergingEmptyBlockProfitable(BasicBlock *BB, BasicBlock *DestBB,
                                       bool isPreheader);
    bool optimizeBlock(BasicBlock &BB, bool &ModifiedDT);
    bool optimizeInst(Instruction *I, bool &ModifiedDT);
    bool optimizeMemoryInst(Instruction *I, Value *Addr,
                            Type *AccessTy, unsigned AS);
    bool optimizeInlineAsmInst(CallInst *CS);
    bool optimizeCallInst(CallInst *CI, bool &ModifiedDT);
    bool optimizeExt(Instruction *&I);
    bool optimizeExtUses(Instruction *I);
    bool optimizeLoadExt(LoadInst *I);
    bool optimizeSelectInst(SelectInst *SI);
    bool optimizeShuffleVectorInst(ShuffleVectorInst *SI);
    bool optimizeSwitchInst(SwitchInst *CI);
    bool optimizeExtractElementInst(Instruction *Inst);
    bool dupRetToEnableTailCallOpts(BasicBlock *BB);
    bool placeDbgValues(Function &F);
    bool canFormExtLd(const SmallVectorImpl<Instruction *> &MovedExts,
                      LoadInst *&LI, Instruction *&Inst, bool HasPromoted);
    bool tryToPromoteExts(TypePromotionTransaction &TPT,
                          const SmallVectorImpl<Instruction *> &Exts,
                          SmallVectorImpl<Instruction *> &ProfitablyMovedExts,
                          unsigned CreatedInstsCost = 0);
    bool mergeSExts(Function &F);
    bool performAddressTypePromotion(
        Instruction *&Inst,
        bool AllowPromotionWithoutCommonHeader,
        bool HasPromoted, TypePromotionTransaction &TPT,
        SmallVectorImpl<Instruction *> &SpeculativelyMovedExts);
    bool splitBranchCondition(Function &F);
    bool simplifyOffsetableRelocate(Instruction &I);
    bool splitIndirectCriticalEdges(Function &F);
  };
}

char CodeGenPrepare::ID = 0;
INITIALIZE_PASS_BEGIN(CodeGenPrepare, DEBUG_TYPE,
                      "Optimize for code generation", false, false)
INITIALIZE_PASS_DEPENDENCY(ProfileSummaryInfoWrapperPass)
INITIALIZE_PASS_END(CodeGenPrepare, DEBUG_TYPE,
                    "Optimize for code generation", false, false)

FunctionPass *llvm::createCodeGenPreparePass() { return new CodeGenPrepare(); }

bool CodeGenPrepare::runOnFunction(Function &F) {
  if (skipFunction(F))
    return false;

  DL = &F.getParent()->getDataLayout();

  bool EverMadeChange = false;
  // Clear per function information.
  InsertedInsts.clear();
  PromotedInsts.clear();
  BFI.reset();
  BPI.reset();

  ModifiedDT = false;
  if (auto *TPC = getAnalysisIfAvailable<TargetPassConfig>()) {
    TM = &TPC->getTM<TargetMachine>();
    SubtargetInfo = TM->getSubtargetImpl(F);
    TLI = SubtargetInfo->getTargetLowering();
    TRI = SubtargetInfo->getRegisterInfo();
  }
  TLInfo = &getAnalysis<TargetLibraryInfoWrapperPass>().getTLI();
  TTI = &getAnalysis<TargetTransformInfoWrapperPass>().getTTI(F);
  LI = &getAnalysis<LoopInfoWrapperPass>().getLoopInfo();
  OptSize = F.optForSize();

  if (ProfileGuidedSectionPrefix) {
    ProfileSummaryInfo *PSI =
        getAnalysis<ProfileSummaryInfoWrapperPass>().getPSI();
    if (PSI->isFunctionHotInCallGraph(&F))
      F.setSectionPrefix(".hot");
    else if (PSI->isFunctionColdInCallGraph(&F))
      F.setSectionPrefix(".unlikely");
  }

  /// This optimization identifies DIV instructions that can be
  /// profitably bypassed and carried out with a shorter, faster divide.
  if (!OptSize && TLI && TLI->isSlowDivBypassed()) {
    const DenseMap<unsigned int, unsigned int> &BypassWidths =
       TLI->getBypassSlowDivWidths();
    BasicBlock* BB = &*F.begin();
    while (BB != nullptr) {
      // bypassSlowDivision may create new BBs, but we don't want to reapply the
      // optimization to those blocks.
      BasicBlock* Next = BB->getNextNode();
      EverMadeChange |= bypassSlowDivision(BB, BypassWidths);
      BB = Next;
    }
  }

  // Eliminate blocks that contain only PHI nodes and an
  // unconditional branch.
  EverMadeChange |= eliminateMostlyEmptyBlocks(F);

  // llvm.dbg.value is far away from the value then iSel may not be able
  // handle it properly. iSel will drop llvm.dbg.value if it can not
  // find a node corresponding to the value.
  EverMadeChange |= placeDbgValues(F);

  if (!DisableBranchOpts)
    EverMadeChange |= splitBranchCondition(F);

  // Split some critical edges where one of the sources is an indirect branch,
  // to help generate sane code for PHIs involving such edges.
  EverMadeChange |= splitIndirectCriticalEdges(F);

  bool MadeChange = true;
  while (MadeChange) {
    MadeChange = false;
    SeenChainsForSExt.clear();
    ValToSExtendedUses.clear();
    RemovedInsts.clear();
    for (Function::iterator I = F.begin(); I != F.end(); ) {
      BasicBlock *BB = &*I++;
      bool ModifiedDTOnIteration = false;
      MadeChange |= optimizeBlock(*BB, ModifiedDTOnIteration);

      // Restart BB iteration if the dominator tree of the Function was changed
      if (ModifiedDTOnIteration)
        break;
    }
    if (EnableTypePromotionMerge && !ValToSExtendedUses.empty())
      MadeChange |= mergeSExts(F);

    // Really free removed instructions during promotion.
    for (Instruction *I : RemovedInsts)
      I->deleteValue();

    EverMadeChange |= MadeChange;
  }

  SunkAddrs.clear();

  if (!DisableBranchOpts) {
    MadeChange = false;
    SmallPtrSet<BasicBlock*, 8> WorkList;
    for (BasicBlock &BB : F) {
      SmallVector<BasicBlock *, 2> Successors(succ_begin(&BB), succ_end(&BB));
      MadeChange |= ConstantFoldTerminator(&BB, true);
      if (!MadeChange) continue;

      for (SmallVectorImpl<BasicBlock*>::iterator
             II = Successors.begin(), IE = Successors.end(); II != IE; ++II)
        if (pred_begin(*II) == pred_end(*II))
          WorkList.insert(*II);
    }

    // Delete the dead blocks and any of their dead successors.
    MadeChange |= !WorkList.empty();
    while (!WorkList.empty()) {
      BasicBlock *BB = *WorkList.begin();
      WorkList.erase(BB);
      SmallVector<BasicBlock*, 2> Successors(succ_begin(BB), succ_end(BB));

      DeleteDeadBlock(BB);

      for (SmallVectorImpl<BasicBlock*>::iterator
             II = Successors.begin(), IE = Successors.end(); II != IE; ++II)
        if (pred_begin(*II) == pred_end(*II))
          WorkList.insert(*II);
    }

    // Merge pairs of basic blocks with unconditional branches, connected by
    // a single edge.
    if (EverMadeChange || MadeChange)
      MadeChange |= eliminateFallThrough(F);

    EverMadeChange |= MadeChange;
  }

  if (!DisableGCOpts) {
    SmallVector<Instruction *, 2> Statepoints;
    for (BasicBlock &BB : F)
      for (Instruction &I : BB)
        if (isStatepoint(I))
          Statepoints.push_back(&I);
    for (auto &I : Statepoints)
      EverMadeChange |= simplifyOffsetableRelocate(*I);
  }

  return EverMadeChange;
}

/// Merge basic blocks which are connected by a single edge, where one of the
/// basic blocks has a single successor pointing to the other basic block,
/// which has a single predecessor.
bool CodeGenPrepare::eliminateFallThrough(Function &F) {
  bool Changed = false;
  // Scan all of the blocks in the function, except for the entry block.
  for (Function::iterator I = std::next(F.begin()), E = F.end(); I != E;) {
    BasicBlock *BB = &*I++;
    // If the destination block has a single pred, then this is a trivial
    // edge, just collapse it.
    BasicBlock *SinglePred = BB->getSinglePredecessor();

    // Don't merge if BB's address is taken.
    if (!SinglePred || SinglePred == BB || BB->hasAddressTaken()) continue;

    BranchInst *Term = dyn_cast<BranchInst>(SinglePred->getTerminator());
    if (Term && !Term->isConditional()) {
      Changed = true;
      DEBUG(dbgs() << "To merge:\n"<< *SinglePred << "\n\n\n");
      // Remember if SinglePred was the entry block of the function.
      // If so, we will need to move BB back to the entry position.
      bool isEntry = SinglePred == &SinglePred->getParent()->getEntryBlock();
      MergeBasicBlockIntoOnlyPred(BB, nullptr);

      if (isEntry && BB != &BB->getParent()->getEntryBlock())
        BB->moveBefore(&BB->getParent()->getEntryBlock());

      // We have erased a block. Update the iterator.
      I = BB->getIterator();
    }
  }
  return Changed;
}

/// Find a destination block from BB if BB is mergeable empty block.
BasicBlock *CodeGenPrepare::findDestBlockOfMergeableEmptyBlock(BasicBlock *BB) {
  // If this block doesn't end with an uncond branch, ignore it.
  BranchInst *BI = dyn_cast<BranchInst>(BB->getTerminator());
  if (!BI || !BI->isUnconditional())
    return nullptr;

  // If the instruction before the branch (skipping debug info) isn't a phi
  // node, then other stuff is happening here.
  BasicBlock::iterator BBI = BI->getIterator();
  if (BBI != BB->begin()) {
    --BBI;
    while (isa<DbgInfoIntrinsic>(BBI)) {
      if (BBI == BB->begin())
        break;
      --BBI;
    }
    if (!isa<DbgInfoIntrinsic>(BBI) && !isa<PHINode>(BBI))
      return nullptr;
  }

  // Do not break infinite loops.
  BasicBlock *DestBB = BI->getSuccessor(0);
  if (DestBB == BB)
    return nullptr;

  if (!canMergeBlocks(BB, DestBB))
    DestBB = nullptr;

  return DestBB;
}

// Return the unique indirectbr predecessor of a block. This may return null
// even if such a predecessor exists, if it's not useful for splitting.
// If a predecessor is found, OtherPreds will contain all other (non-indirectbr)
// predecessors of BB.
static BasicBlock *
findIBRPredecessor(BasicBlock *BB, SmallVectorImpl<BasicBlock *> &OtherPreds) {
  // If the block doesn't have any PHIs, we don't care about it, since there's
  // no point in splitting it.
  PHINode *PN = dyn_cast<PHINode>(BB->begin());
  if (!PN)
    return nullptr;

  // Verify we have exactly one IBR predecessor.
  // Conservatively bail out if one of the other predecessors is not a "regular"
  // terminator (that is, not a switch or a br).
  BasicBlock *IBB = nullptr;
  for (unsigned Pred = 0, E = PN->getNumIncomingValues(); Pred != E; ++Pred) {
    BasicBlock *PredBB = PN->getIncomingBlock(Pred);
    TerminatorInst *PredTerm = PredBB->getTerminator();
    switch (PredTerm->getOpcode()) {
    case Instruction::IndirectBr:
      if (IBB)
        return nullptr;
      IBB = PredBB;
      break;
    case Instruction::Br:
    case Instruction::Switch:
      OtherPreds.push_back(PredBB);
      continue;
    default:
      return nullptr;
    }
  }

  return IBB;
}

// Split critical edges where the source of the edge is an indirectbr
// instruction. This isn't always possible, but we can handle some easy cases.
// This is useful because MI is unable to split such critical edges,
// which means it will not be able to sink instructions along those edges.
// This is especially painful for indirect branches with many successors, where
// we end up having to prepare all outgoing values in the origin block.
//
// Our normal algorithm for splitting critical edges requires us to update
// the outgoing edges of the edge origin block, but for an indirectbr this
// is hard, since it would require finding and updating the block addresses
// the indirect branch uses. But if a block only has a single indirectbr
// predecessor, with the others being regular branches, we can do it in a
// different way.
// Say we have A -> D, B -> D, I -> D where only I -> D is an indirectbr.
// We can split D into D0 and D1, where D0 contains only the PHIs from D,
// and D1 is the D block body. We can then duplicate D0 as D0A and D0B, and
// create the following structure:
// A -> D0A, B -> D0A, I -> D0B, D0A -> D1, D0B -> D1
bool CodeGenPrepare::splitIndirectCriticalEdges(Function &F) {
  // Check whether the function has any indirectbrs, and collect which blocks
  // they may jump to. Since most functions don't have indirect branches,
  // this lowers the common case's overhead to O(Blocks) instead of O(Edges).
  SmallSetVector<BasicBlock *, 16> Targets;
  for (auto &BB : F) {
    auto *IBI = dyn_cast<IndirectBrInst>(BB.getTerminator());
    if (!IBI)
      continue;

    for (unsigned Succ = 0, E = IBI->getNumSuccessors(); Succ != E; ++Succ)
      Targets.insert(IBI->getSuccessor(Succ));
  }

  if (Targets.empty())
    return false;

  bool Changed = false;
  for (BasicBlock *Target : Targets) {
    SmallVector<BasicBlock *, 16> OtherPreds;
    BasicBlock *IBRPred = findIBRPredecessor(Target, OtherPreds);
    // If we did not found an indirectbr, or the indirectbr is the only
    // incoming edge, this isn't the kind of edge we're looking for.
    if (!IBRPred || OtherPreds.empty())
      continue;

    // Don't even think about ehpads/landingpads.
    Instruction *FirstNonPHI = Target->getFirstNonPHI();
    if (FirstNonPHI->isEHPad() || Target->isLandingPad())
      continue;

    BasicBlock *BodyBlock = Target->splitBasicBlock(FirstNonPHI, ".split");
    // It's possible Target was its own successor through an indirectbr.
    // In this case, the indirectbr now comes from BodyBlock.
    if (IBRPred == Target)
      IBRPred = BodyBlock;

    // At this point Target only has PHIs, and BodyBlock has the rest of the
    // block's body. Create a copy of Target that will be used by the "direct"
    // preds.
    ValueToValueMapTy VMap;
    BasicBlock *DirectSucc = CloneBasicBlock(Target, VMap, ".clone", &F);

    for (BasicBlock *Pred : OtherPreds) {
      // If the target is a loop to itself, then the terminator of the split
      // block needs to be updated.
      if (Pred == Target)
        BodyBlock->getTerminator()->replaceUsesOfWith(Target, DirectSucc);
      else
        Pred->getTerminator()->replaceUsesOfWith(Target, DirectSucc);
    }

    // Ok, now fix up the PHIs. We know the two blocks only have PHIs, and that
    // they are clones, so the number of PHIs are the same.
    // (a) Remove the edge coming from IBRPred from the "Direct" PHI
    // (b) Leave that as the only edge in the "Indirect" PHI.
    // (c) Merge the two in the body block.
    BasicBlock::iterator Indirect = Target->begin(),
                         End = Target->getFirstNonPHI()->getIterator();
    BasicBlock::iterator Direct = DirectSucc->begin();
    BasicBlock::iterator MergeInsert = BodyBlock->getFirstInsertionPt();

    assert(&*End == Target->getTerminator() &&
           "Block was expected to only contain PHIs");

    while (Indirect != End) {
      PHINode *DirPHI = cast<PHINode>(Direct);
      PHINode *IndPHI = cast<PHINode>(Indirect);

      // Now, clean up - the direct block shouldn't get the indirect value,
      // and vice versa.
      DirPHI->removeIncomingValue(IBRPred);
      Direct++;

      // Advance the pointer here, to avoid invalidation issues when the old
      // PHI is erased.
      Indirect++;

      PHINode *NewIndPHI = PHINode::Create(IndPHI->getType(), 1, "ind", IndPHI);
      NewIndPHI->addIncoming(IndPHI->getIncomingValueForBlock(IBRPred),
                             IBRPred);

      // Create a PHI in the body block, to merge the direct and indirect
      // predecessors.
      PHINode *MergePHI =
          PHINode::Create(IndPHI->getType(), 2, "merge", &*MergeInsert);
      MergePHI->addIncoming(NewIndPHI, Target);
      MergePHI->addIncoming(DirPHI, DirectSucc);

      IndPHI->replaceAllUsesWith(MergePHI);
      IndPHI->eraseFromParent();
    }

    Changed = true;
  }

  return Changed;
}

/// Eliminate blocks that contain only PHI nodes, debug info directives, and an
/// unconditional branch. Passes before isel (e.g. LSR/loopsimplify) often split
/// edges in ways that are non-optimal for isel. Start by eliminating these
/// blocks so we can split them the way we want them.
bool CodeGenPrepare::eliminateMostlyEmptyBlocks(Function &F) {
  SmallPtrSet<BasicBlock *, 16> Preheaders;
  SmallVector<Loop *, 16> LoopList(LI->begin(), LI->end());
  while (!LoopList.empty()) {
    Loop *L = LoopList.pop_back_val();
    LoopList.insert(LoopList.end(), L->begin(), L->end());
    if (BasicBlock *Preheader = L->getLoopPreheader())
      Preheaders.insert(Preheader);
  }

  bool MadeChange = false;
  // Note that this intentionally skips the entry block.
  for (Function::iterator I = std::next(F.begin()), E = F.end(); I != E;) {
    BasicBlock *BB = &*I++;
    BasicBlock *DestBB = findDestBlockOfMergeableEmptyBlock(BB);
    if (!DestBB ||
        !isMergingEmptyBlockProfitable(BB, DestBB, Preheaders.count(BB)))
      continue;

    eliminateMostlyEmptyBlock(BB);
    MadeChange = true;
  }
  return MadeChange;
}

bool CodeGenPrepare::isMergingEmptyBlockProfitable(BasicBlock *BB,
                                                   BasicBlock *DestBB,
                                                   bool isPreheader) {
  // Do not delete loop preheaders if doing so would create a critical edge.
  // Loop preheaders can be good locations to spill registers. If the
  // preheader is deleted and we create a critical edge, registers may be
  // spilled in the loop body instead.
  if (!DisablePreheaderProtect && isPreheader &&
      !(BB->getSinglePredecessor() &&
        BB->getSinglePredecessor()->getSingleSuccessor()))
    return false;

  // Try to skip merging if the unique predecessor of BB is terminated by a
  // switch or indirect branch instruction, and BB is used as an incoming block
  // of PHIs in DestBB. In such case, merging BB and DestBB would cause ISel to
  // add COPY instructions in the predecessor of BB instead of BB (if it is not
  // merged). Note that the critical edge created by merging such blocks wont be
  // split in MachineSink because the jump table is not analyzable. By keeping
  // such empty block (BB), ISel will place COPY instructions in BB, not in the
  // predecessor of BB.
  BasicBlock *Pred = BB->getUniquePredecessor();
  if (!Pred ||
      !(isa<SwitchInst>(Pred->getTerminator()) ||
        isa<IndirectBrInst>(Pred->getTerminator())))
    return true;

  if (BB->getTerminator() != BB->getFirstNonPHI())
    return true;

  // We use a simple cost heuristic which determine skipping merging is
  // profitable if the cost of skipping merging is less than the cost of
  // merging : Cost(skipping merging) < Cost(merging BB), where the
  // Cost(skipping merging) is Freq(BB) * (Cost(Copy) + Cost(Branch)), and
  // the Cost(merging BB) is Freq(Pred) * Cost(Copy).
  // Assuming Cost(Copy) == Cost(Branch), we could simplify it to :
  //   Freq(Pred) / Freq(BB) > 2.
  // Note that if there are multiple empty blocks sharing the same incoming
  // value for the PHIs in the DestBB, we consider them together. In such
  // case, Cost(merging BB) will be the sum of their frequencies.

  if (!isa<PHINode>(DestBB->begin()))
    return true;

  SmallPtrSet<BasicBlock *, 16> SameIncomingValueBBs;

  // Find all other incoming blocks from which incoming values of all PHIs in
  // DestBB are the same as the ones from BB.
  for (pred_iterator PI = pred_begin(DestBB), E = pred_end(DestBB); PI != E;
       ++PI) {
    BasicBlock *DestBBPred = *PI;
    if (DestBBPred == BB)
      continue;

    bool HasAllSameValue = true;
    BasicBlock::const_iterator DestBBI = DestBB->begin();
    while (const PHINode *DestPN = dyn_cast<PHINode>(DestBBI++)) {
      if (DestPN->getIncomingValueForBlock(BB) !=
          DestPN->getIncomingValueForBlock(DestBBPred)) {
        HasAllSameValue = false;
        break;
      }
    }
    if (HasAllSameValue)
      SameIncomingValueBBs.insert(DestBBPred);
  }

  // See if all BB's incoming values are same as the value from Pred. In this
  // case, no reason to skip merging because COPYs are expected to be place in
  // Pred already.
  if (SameIncomingValueBBs.count(Pred))
    return true;

  if (!BFI) {
    Function &F = *BB->getParent();
    LoopInfo LI{DominatorTree(F)};
    BPI.reset(new BranchProbabilityInfo(F, LI));
    BFI.reset(new BlockFrequencyInfo(F, *BPI, LI));
  }

  BlockFrequency PredFreq = BFI->getBlockFreq(Pred);
  BlockFrequency BBFreq = BFI->getBlockFreq(BB);

  for (auto SameValueBB : SameIncomingValueBBs)
    if (SameValueBB->getUniquePredecessor() == Pred &&
        DestBB == findDestBlockOfMergeableEmptyBlock(SameValueBB))
      BBFreq += BFI->getBlockFreq(SameValueBB);

  return PredFreq.getFrequency() <=
         BBFreq.getFrequency() * FreqRatioToSkipMerge;
}

/// Return true if we can merge BB into DestBB if there is a single
/// unconditional branch between them, and BB contains no other non-phi
/// instructions.
bool CodeGenPrepare::canMergeBlocks(const BasicBlock *BB,
                                    const BasicBlock *DestBB) const {
  // We only want to eliminate blocks whose phi nodes are used by phi nodes in
  // the successor.  If there are more complex condition (e.g. preheaders),
  // don't mess around with them.
  BasicBlock::const_iterator BBI = BB->begin();
  while (const PHINode *PN = dyn_cast<PHINode>(BBI++)) {
    for (const User *U : PN->users()) {
      const Instruction *UI = cast<Instruction>(U);
      if (UI->getParent() != DestBB || !isa<PHINode>(UI))
        return false;
      // If User is inside DestBB block and it is a PHINode then check
      // incoming value. If incoming value is not from BB then this is
      // a complex condition (e.g. preheaders) we want to avoid here.
      if (UI->getParent() == DestBB) {
        if (const PHINode *UPN = dyn_cast<PHINode>(UI))
          for (unsigned I = 0, E = UPN->getNumIncomingValues(); I != E; ++I) {
            Instruction *Insn = dyn_cast<Instruction>(UPN->getIncomingValue(I));
            if (Insn && Insn->getParent() == BB &&
                Insn->getParent() != UPN->getIncomingBlock(I))
              return false;
          }
      }
    }
  }

  // If BB and DestBB contain any common predecessors, then the phi nodes in BB
  // and DestBB may have conflicting incoming values for the block.  If so, we
  // can't merge the block.
  const PHINode *DestBBPN = dyn_cast<PHINode>(DestBB->begin());
  if (!DestBBPN) return true;  // no conflict.

  // Collect the preds of BB.
  SmallPtrSet<const BasicBlock*, 16> BBPreds;
  if (const PHINode *BBPN = dyn_cast<PHINode>(BB->begin())) {
    // It is faster to get preds from a PHI than with pred_iterator.
    for (unsigned i = 0, e = BBPN->getNumIncomingValues(); i != e; ++i)
      BBPreds.insert(BBPN->getIncomingBlock(i));
  } else {
    BBPreds.insert(pred_begin(BB), pred_end(BB));
  }

  // Walk the preds of DestBB.
  for (unsigned i = 0, e = DestBBPN->getNumIncomingValues(); i != e; ++i) {
    BasicBlock *Pred = DestBBPN->getIncomingBlock(i);
    if (BBPreds.count(Pred)) {   // Common predecessor?
      BBI = DestBB->begin();
      while (const PHINode *PN = dyn_cast<PHINode>(BBI++)) {
        const Value *V1 = PN->getIncomingValueForBlock(Pred);
        const Value *V2 = PN->getIncomingValueForBlock(BB);

        // If V2 is a phi node in BB, look up what the mapped value will be.
        if (const PHINode *V2PN = dyn_cast<PHINode>(V2))
          if (V2PN->getParent() == BB)
            V2 = V2PN->getIncomingValueForBlock(Pred);

        // If there is a conflict, bail out.
        if (V1 != V2) return false;
      }
    }
  }

  return true;
}


/// Eliminate a basic block that has only phi's and an unconditional branch in
/// it.
void CodeGenPrepare::eliminateMostlyEmptyBlock(BasicBlock *BB) {
  BranchInst *BI = cast<BranchInst>(BB->getTerminator());
  BasicBlock *DestBB = BI->getSuccessor(0);

  DEBUG(dbgs() << "MERGING MOSTLY EMPTY BLOCKS - BEFORE:\n" << *BB << *DestBB);

  // If the destination block has a single pred, then this is a trivial edge,
  // just collapse it.
  if (BasicBlock *SinglePred = DestBB->getSinglePredecessor()) {
    if (SinglePred != DestBB) {
      // Remember if SinglePred was the entry block of the function.  If so, we
      // will need to move BB back to the entry position.
      bool isEntry = SinglePred == &SinglePred->getParent()->getEntryBlock();
      MergeBasicBlockIntoOnlyPred(DestBB, nullptr);

      if (isEntry && BB != &BB->getParent()->getEntryBlock())
        BB->moveBefore(&BB->getParent()->getEntryBlock());

      DEBUG(dbgs() << "AFTER:\n" << *DestBB << "\n\n\n");
      return;
    }
  }

  // Otherwise, we have multiple predecessors of BB.  Update the PHIs in DestBB
  // to handle the new incoming edges it is about to have.
  PHINode *PN;
  for (BasicBlock::iterator BBI = DestBB->begin();
       (PN = dyn_cast<PHINode>(BBI)); ++BBI) {
    // Remove the incoming value for BB, and remember it.
    Value *InVal = PN->removeIncomingValue(BB, false);

    // Two options: either the InVal is a phi node defined in BB or it is some
    // value that dominates BB.
    PHINode *InValPhi = dyn_cast<PHINode>(InVal);
    if (InValPhi && InValPhi->getParent() == BB) {
      // Add all of the input values of the input PHI as inputs of this phi.
      for (unsigned i = 0, e = InValPhi->getNumIncomingValues(); i != e; ++i)
        PN->addIncoming(InValPhi->getIncomingValue(i),
                        InValPhi->getIncomingBlock(i));
    } else {
      // Otherwise, add one instance of the dominating value for each edge that
      // we will be adding.
      if (PHINode *BBPN = dyn_cast<PHINode>(BB->begin())) {
        for (unsigned i = 0, e = BBPN->getNumIncomingValues(); i != e; ++i)
          PN->addIncoming(InVal, BBPN->getIncomingBlock(i));
      } else {
        for (pred_iterator PI = pred_begin(BB), E = pred_end(BB); PI != E; ++PI)
          PN->addIncoming(InVal, *PI);
      }
    }
  }

  // The PHIs are now updated, change everything that refers to BB to use
  // DestBB and remove BB.
  BB->replaceAllUsesWith(DestBB);
  BB->eraseFromParent();
  ++NumBlocksElim;

  DEBUG(dbgs() << "AFTER:\n" << *DestBB << "\n\n\n");
}

// Computes a map of base pointer relocation instructions to corresponding
// derived pointer relocation instructions given a vector of all relocate calls
static void computeBaseDerivedRelocateMap(
    const SmallVectorImpl<GCRelocateInst *> &AllRelocateCalls,
    DenseMap<GCRelocateInst *, SmallVector<GCRelocateInst *, 2>>
        &RelocateInstMap) {
  // Collect information in two maps: one primarily for locating the base object
  // while filling the second map; the second map is the final structure holding
  // a mapping between Base and corresponding Derived relocate calls
  DenseMap<std::pair<unsigned, unsigned>, GCRelocateInst *> RelocateIdxMap;
  for (auto *ThisRelocate : AllRelocateCalls) {
    auto K = std::make_pair(ThisRelocate->getBasePtrIndex(),
                            ThisRelocate->getDerivedPtrIndex());
    RelocateIdxMap.insert(std::make_pair(K, ThisRelocate));
  }
  for (auto &Item : RelocateIdxMap) {
    std::pair<unsigned, unsigned> Key = Item.first;
    if (Key.first == Key.second)
      // Base relocation: nothing to insert
      continue;

    GCRelocateInst *I = Item.second;
    auto BaseKey = std::make_pair(Key.first, Key.first);

    // We're iterating over RelocateIdxMap so we cannot modify it.
    auto MaybeBase = RelocateIdxMap.find(BaseKey);
    if (MaybeBase == RelocateIdxMap.end())
      // TODO: We might want to insert a new base object relocate and gep off
      // that, if there are enough derived object relocates.
      continue;

    RelocateInstMap[MaybeBase->second].push_back(I);
  }
}

// Accepts a GEP and extracts the operands into a vector provided they're all
// small integer constants
static bool getGEPSmallConstantIntOffsetV(GetElementPtrInst *GEP,
                                          SmallVectorImpl<Value *> &OffsetV) {
  for (unsigned i = 1; i < GEP->getNumOperands(); i++) {
    // Only accept small constant integer operands
    auto Op = dyn_cast<ConstantInt>(GEP->getOperand(i));
    if (!Op || Op->getZExtValue() > 20)
      return false;
  }

  for (unsigned i = 1; i < GEP->getNumOperands(); i++)
    OffsetV.push_back(GEP->getOperand(i));
  return true;
}

// Takes a RelocatedBase (base pointer relocation instruction) and Targets to
// replace, computes a replacement, and affects it.
static bool
simplifyRelocatesOffABase(GCRelocateInst *RelocatedBase,
                          const SmallVectorImpl<GCRelocateInst *> &Targets) {
  bool MadeChange = false;
  for (GCRelocateInst *ToReplace : Targets) {
    assert(ToReplace->getBasePtrIndex() == RelocatedBase->getBasePtrIndex() &&
           "Not relocating a derived object of the original base object");
    if (ToReplace->getBasePtrIndex() == ToReplace->getDerivedPtrIndex()) {
      // A duplicate relocate call. TODO: coalesce duplicates.
      continue;
    }

    if (RelocatedBase->getParent() != ToReplace->getParent()) {
      // Base and derived relocates are in different basic blocks.
      // In this case transform is only valid when base dominates derived
      // relocate. However it would be too expensive to check dominance
      // for each such relocate, so we skip the whole transformation.
      continue;
    }

    Value *Base = ToReplace->getBasePtr();
    auto Derived = dyn_cast<GetElementPtrInst>(ToReplace->getDerivedPtr());
    if (!Derived || Derived->getPointerOperand() != Base)
      continue;

    SmallVector<Value *, 2> OffsetV;
    if (!getGEPSmallConstantIntOffsetV(Derived, OffsetV))
      continue;

    // Create a Builder and replace the target callsite with a gep
    assert(RelocatedBase->getNextNode() &&
           "Should always have one since it's not a terminator");

    // Insert after RelocatedBase
    IRBuilder<> Builder(RelocatedBase->getNextNode());
    Builder.SetCurrentDebugLocation(ToReplace->getDebugLoc());

    // If gc_relocate does not match the actual type, cast it to the right type.
    // In theory, there must be a bitcast after gc_relocate if the type does not
    // match, and we should reuse it to get the derived pointer. But it could be
    // cases like this:
    // bb1:
    //  ...
    //  %g1 = call coldcc i8 addrspace(1)* @llvm.experimental.gc.relocate.p1i8(...)
    //  br label %merge
    //
    // bb2:
    //  ...
    //  %g2 = call coldcc i8 addrspace(1)* @llvm.experimental.gc.relocate.p1i8(...)
    //  br label %merge
    //
    // merge:
    //  %p1 = phi i8 addrspace(1)* [ %g1, %bb1 ], [ %g2, %bb2 ]
    //  %cast = bitcast i8 addrspace(1)* %p1 in to i32 addrspace(1)*
    //
    // In this case, we can not find the bitcast any more. So we insert a new bitcast
    // no matter there is already one or not. In this way, we can handle all cases, and
    // the extra bitcast should be optimized away in later passes.
    Value *ActualRelocatedBase = RelocatedBase;
    if (RelocatedBase->getType() != Base->getType()) {
      ActualRelocatedBase =
          Builder.CreateBitCast(RelocatedBase, Base->getType());
    }
    Value *Replacement = Builder.CreateGEP(
        Derived->getSourceElementType(), ActualRelocatedBase, makeArrayRef(OffsetV));
    Replacement->takeName(ToReplace);
    // If the newly generated derived pointer's type does not match the original derived
    // pointer's type, cast the new derived pointer to match it. Same reasoning as above.
    Value *ActualReplacement = Replacement;
    if (Replacement->getType() != ToReplace->getType()) {
      ActualReplacement =
          Builder.CreateBitCast(Replacement, ToReplace->getType());
    }
    ToReplace->replaceAllUsesWith(ActualReplacement);
    ToReplace->eraseFromParent();

    MadeChange = true;
  }
  return MadeChange;
}

// Turns this:
//
// %base = ...
// %ptr = gep %base + 15
// %tok = statepoint (%fun, i32 0, i32 0, i32 0, %base, %ptr)
// %base' = relocate(%tok, i32 4, i32 4)
// %ptr' = relocate(%tok, i32 4, i32 5)
// %val = load %ptr'
//
// into this:
//
// %base = ...
// %ptr = gep %base + 15
// %tok = statepoint (%fun, i32 0, i32 0, i32 0, %base, %ptr)
// %base' = gc.relocate(%tok, i32 4, i32 4)
// %ptr' = gep %base' + 15
// %val = load %ptr'
bool CodeGenPrepare::simplifyOffsetableRelocate(Instruction &I) {
  bool MadeChange = false;
  SmallVector<GCRelocateInst *, 2> AllRelocateCalls;

  for (auto *U : I.users())
    if (GCRelocateInst *Relocate = dyn_cast<GCRelocateInst>(U))
      // Collect all the relocate calls associated with a statepoint
      AllRelocateCalls.push_back(Relocate);

  // We need atleast one base pointer relocation + one derived pointer
  // relocation to mangle
  if (AllRelocateCalls.size() < 2)
    return false;

  // RelocateInstMap is a mapping from the base relocate instruction to the
  // corresponding derived relocate instructions
  DenseMap<GCRelocateInst *, SmallVector<GCRelocateInst *, 2>> RelocateInstMap;
  computeBaseDerivedRelocateMap(AllRelocateCalls, RelocateInstMap);
  if (RelocateInstMap.empty())
    return false;

  for (auto &Item : RelocateInstMap)
    // Item.first is the RelocatedBase to offset against
    // Item.second is the vector of Targets to replace
    MadeChange = simplifyRelocatesOffABase(Item.first, Item.second);
  return MadeChange;
}

/// SinkCast - Sink the specified cast instruction into its user blocks
static bool SinkCast(CastInst *CI) {
  BasicBlock *DefBB = CI->getParent();

  /// InsertedCasts - Only insert a cast in each block once.
  DenseMap<BasicBlock*, CastInst*> InsertedCasts;

  bool MadeChange = false;
  for (Value::user_iterator UI = CI->user_begin(), E = CI->user_end();
       UI != E; ) {
    Use &TheUse = UI.getUse();
    Instruction *User = cast<Instruction>(*UI);

    // Figure out which BB this cast is used in.  For PHI's this is the
    // appropriate predecessor block.
    BasicBlock *UserBB = User->getParent();
    if (PHINode *PN = dyn_cast<PHINode>(User)) {
      UserBB = PN->getIncomingBlock(TheUse);
    }

    // Preincrement use iterator so we don't invalidate it.
    ++UI;

    // The first insertion point of a block containing an EH pad is after the
    // pad.  If the pad is the user, we cannot sink the cast past the pad.
    if (User->isEHPad())
      continue;

    // If the block selected to receive the cast is an EH pad that does not
    // allow non-PHI instructions before the terminator, we can't sink the
    // cast.
    if (UserBB->getTerminator()->isEHPad())
      continue;

    // If this user is in the same block as the cast, don't change the cast.
    if (UserBB == DefBB) continue;

    // If we have already inserted a cast into this block, use it.
    CastInst *&InsertedCast = InsertedCasts[UserBB];

    if (!InsertedCast) {
      BasicBlock::iterator InsertPt = UserBB->getFirstInsertionPt();
      assert(InsertPt != UserBB->end());
      InsertedCast = CastInst::Create(CI->getOpcode(), CI->getOperand(0),
                                      CI->getType(), "", &*InsertPt);
    }

    // Replace a use of the cast with a use of the new cast.
    TheUse = InsertedCast;
    MadeChange = true;
    ++NumCastUses;
  }

  // If we removed all uses, nuke the cast.
  if (CI->use_empty()) {
    CI->eraseFromParent();
    MadeChange = true;
  }

  return MadeChange;
}

/// If the specified cast instruction is a noop copy (e.g. it's casting from
/// one pointer type to another, i32->i8 on PPC), sink it into user blocks to
/// reduce the number of virtual registers that must be created and coalesced.
///
/// Return true if any changes are made.
///
static bool OptimizeNoopCopyExpression(CastInst *CI, const TargetLowering &TLI,
                                       const DataLayout &DL) {
  // Sink only "cheap" (or nop) address-space casts.  This is a weaker condition
  // than sinking only nop casts, but is helpful on some platforms.
  if (auto *ASC = dyn_cast<AddrSpaceCastInst>(CI)) {
    if (!TLI.isCheapAddrSpaceCast(ASC->getSrcAddressSpace(),
                                  ASC->getDestAddressSpace()))
      return false;
  }

  // If this is a noop copy,
  EVT SrcVT = TLI.getValueType(DL, CI->getOperand(0)->getType());
  EVT DstVT = TLI.getValueType(DL, CI->getType());

  // This is an fp<->int conversion?
  if (SrcVT.isInteger() != DstVT.isInteger())
    return false;

  // If this is an extension, it will be a zero or sign extension, which
  // isn't a noop.
  if (SrcVT.bitsLT(DstVT)) return false;

  // If these values will be promoted, find out what they will be promoted
  // to.  This helps us consider truncates on PPC as noop copies when they
  // are.
  if (TLI.getTypeAction(CI->getContext(), SrcVT) ==
      TargetLowering::TypePromoteInteger)
    SrcVT = TLI.getTypeToTransformTo(CI->getContext(), SrcVT);
  if (TLI.getTypeAction(CI->getContext(), DstVT) ==
      TargetLowering::TypePromoteInteger)
    DstVT = TLI.getTypeToTransformTo(CI->getContext(), DstVT);

  // If, after promotion, these are the same types, this is a noop copy.
  if (SrcVT != DstVT)
    return false;

  return SinkCast(CI);
}

/// Try to combine CI into a call to the llvm.uadd.with.overflow intrinsic if
/// possible.
///
/// Return true if any changes were made.
static bool CombineUAddWithOverflow(CmpInst *CI) {
  Value *A, *B;
  Instruction *AddI;
  if (!match(CI,
             m_UAddWithOverflow(m_Value(A), m_Value(B), m_Instruction(AddI))))
    return false;

  Type *Ty = AddI->getType();
  if (!isa<IntegerType>(Ty))
    return false;

  // We don't want to move around uses of condition values this late, so we we
  // check if it is legal to create the call to the intrinsic in the basic
  // block containing the icmp:

  if (AddI->getParent() != CI->getParent() && !AddI->hasOneUse())
    return false;

#ifndef NDEBUG
  // Someday m_UAddWithOverflow may get smarter, but this is a safe assumption
  // for now:
  if (AddI->hasOneUse())
    assert(*AddI->user_begin() == CI && "expected!");
#endif

  Module *M = CI->getModule();
  Value *F = Intrinsic::getDeclaration(M, Intrinsic::uadd_with_overflow, Ty);

  auto *InsertPt = AddI->hasOneUse() ? CI : AddI;

  auto *UAddWithOverflow =
      CallInst::Create(F, {A, B}, "uadd.overflow", InsertPt);
  auto *UAdd = ExtractValueInst::Create(UAddWithOverflow, 0, "uadd", InsertPt);
  auto *Overflow =
      ExtractValueInst::Create(UAddWithOverflow, 1, "overflow", InsertPt);

  CI->replaceAllUsesWith(Overflow);
  AddI->replaceAllUsesWith(UAdd);
  CI->eraseFromParent();
  AddI->eraseFromParent();
  return true;
}

/// Sink the given CmpInst into user blocks to reduce the number of virtual
/// registers that must be created and coalesced. This is a clear win except on
/// targets with multiple condition code registers (PowerPC), where it might
/// lose; some adjustment may be wanted there.
///
/// Return true if any changes are made.
static bool SinkCmpExpression(CmpInst *CI, const TargetLowering *TLI) {
  BasicBlock *DefBB = CI->getParent();

  // Avoid sinking soft-FP comparisons, since this can move them into a loop.
  if (TLI && TLI->useSoftFloat() && isa<FCmpInst>(CI))
    return false;

  // Only insert a cmp in each block once.
  DenseMap<BasicBlock*, CmpInst*> InsertedCmps;

  bool MadeChange = false;
  for (Value::user_iterator UI = CI->user_begin(), E = CI->user_end();
       UI != E; ) {
    Use &TheUse = UI.getUse();
    Instruction *User = cast<Instruction>(*UI);

    // Preincrement use iterator so we don't invalidate it.
    ++UI;

    // Don't bother for PHI nodes.
    if (isa<PHINode>(User))
      continue;

    // Figure out which BB this cmp is used in.
    BasicBlock *UserBB = User->getParent();

    // If this user is in the same block as the cmp, don't change the cmp.
    if (UserBB == DefBB) continue;

    // If we have already inserted a cmp into this block, use it.
    CmpInst *&InsertedCmp = InsertedCmps[UserBB];

    if (!InsertedCmp) {
      BasicBlock::iterator InsertPt = UserBB->getFirstInsertionPt();
      assert(InsertPt != UserBB->end());
      InsertedCmp =
          CmpInst::Create(CI->getOpcode(), CI->getPredicate(),
                          CI->getOperand(0), CI->getOperand(1), "", &*InsertPt);
      // Propagate the debug info.
      InsertedCmp->setDebugLoc(CI->getDebugLoc());
    }

    // Replace a use of the cmp with a use of the new cmp.
    TheUse = InsertedCmp;
    MadeChange = true;
    ++NumCmpUses;
  }

  // If we removed all uses, nuke the cmp.
  if (CI->use_empty()) {
    CI->eraseFromParent();
    MadeChange = true;
  }

  return MadeChange;
}

static bool OptimizeCmpExpression(CmpInst *CI, const TargetLowering *TLI) {
  if (SinkCmpExpression(CI, TLI))
    return true;

  if (CombineUAddWithOverflow(CI))
    return true;

  return false;
}

/// Duplicate and sink the given 'and' instruction into user blocks where it is
/// used in a compare to allow isel to generate better code for targets where
/// this operation can be combined.
///
/// Return true if any changes are made.
static bool sinkAndCmp0Expression(Instruction *AndI,
                                  const TargetLowering &TLI,
                                  SetOfInstrs &InsertedInsts) {
  // Double-check that we're not trying to optimize an instruction that was
  // already optimized by some other part of this pass.
  assert(!InsertedInsts.count(AndI) &&
         "Attempting to optimize already optimized and instruction");
  (void) InsertedInsts;

  // Nothing to do for single use in same basic block.
  if (AndI->hasOneUse() &&
      AndI->getParent() == cast<Instruction>(*AndI->user_begin())->getParent())
    return false;

  // Try to avoid cases where sinking/duplicating is likely to increase register
  // pressure.
  if (!isa<ConstantInt>(AndI->getOperand(0)) &&
      !isa<ConstantInt>(AndI->getOperand(1)) &&
      AndI->getOperand(0)->hasOneUse() && AndI->getOperand(1)->hasOneUse())
    return false;

  for (auto *U : AndI->users()) {
    Instruction *User = cast<Instruction>(U);

    // Only sink for and mask feeding icmp with 0.
    if (!isa<ICmpInst>(User))
      return false;

    auto *CmpC = dyn_cast<ConstantInt>(User->getOperand(1));
    if (!CmpC || !CmpC->isZero())
      return false;
  }

  if (!TLI.isMaskAndCmp0FoldingBeneficial(*AndI))
    return false;

  DEBUG(dbgs() << "found 'and' feeding only icmp 0;\n");
  DEBUG(AndI->getParent()->dump());

  // Push the 'and' into the same block as the icmp 0.  There should only be
  // one (icmp (and, 0)) in each block, since CSE/GVN should have removed any
  // others, so we don't need to keep track of which BBs we insert into.
  for (Value::user_iterator UI = AndI->user_begin(), E = AndI->user_end();
       UI != E; ) {
    Use &TheUse = UI.getUse();
    Instruction *User = cast<Instruction>(*UI);

    // Preincrement use iterator so we don't invalidate it.
    ++UI;

    DEBUG(dbgs() << "sinking 'and' use: " << *User << "\n");

    // Keep the 'and' in the same place if the use is already in the same block.
    Instruction *InsertPt =
        User->getParent() == AndI->getParent() ? AndI : User;
    Instruction *InsertedAnd =
        BinaryOperator::Create(Instruction::And, AndI->getOperand(0),
                               AndI->getOperand(1), "", InsertPt);
    // Propagate the debug info.
    InsertedAnd->setDebugLoc(AndI->getDebugLoc());

    // Replace a use of the 'and' with a use of the new 'and'.
    TheUse = InsertedAnd;
    ++NumAndUses;
    DEBUG(User->getParent()->dump());
  }

  // We removed all uses, nuke the and.
  AndI->eraseFromParent();
  return true;
}

/// Check if the candidates could be combined with a shift instruction, which
/// includes:
/// 1. Truncate instruction
/// 2. And instruction and the imm is a mask of the low bits:
/// imm & (imm+1) == 0
static bool isExtractBitsCandidateUse(Instruction *User) {
  if (!isa<TruncInst>(User)) {
    if (User->getOpcode() != Instruction::And ||
        !isa<ConstantInt>(User->getOperand(1)))
      return false;

    const APInt &Cimm = cast<ConstantInt>(User->getOperand(1))->getValue();

    if ((Cimm & (Cimm + 1)).getBoolValue())
      return false;
  }
  return true;
}

/// Sink both shift and truncate instruction to the use of truncate's BB.
static bool
SinkShiftAndTruncate(BinaryOperator *ShiftI, Instruction *User, ConstantInt *CI,
                     DenseMap<BasicBlock *, BinaryOperator *> &InsertedShifts,
                     const TargetLowering &TLI, const DataLayout &DL) {
  BasicBlock *UserBB = User->getParent();
  DenseMap<BasicBlock *, CastInst *> InsertedTruncs;
  TruncInst *TruncI = dyn_cast<TruncInst>(User);
  bool MadeChange = false;

  for (Value::user_iterator TruncUI = TruncI->user_begin(),
                            TruncE = TruncI->user_end();
       TruncUI != TruncE;) {

    Use &TruncTheUse = TruncUI.getUse();
    Instruction *TruncUser = cast<Instruction>(*TruncUI);
    // Preincrement use iterator so we don't invalidate it.

    ++TruncUI;

    int ISDOpcode = TLI.InstructionOpcodeToISD(TruncUser->getOpcode());
    if (!ISDOpcode)
      continue;

    // If the use is actually a legal node, there will not be an
    // implicit truncate.
    // FIXME: always querying the result type is just an
    // approximation; some nodes' legality is determined by the
    // operand or other means. There's no good way to find out though.
    if (TLI.isOperationLegalOrCustom(
            ISDOpcode, TLI.getValueType(DL, TruncUser->getType(), true)))
      continue;

    // Don't bother for PHI nodes.
    if (isa<PHINode>(TruncUser))
      continue;

    BasicBlock *TruncUserBB = TruncUser->getParent();

    if (UserBB == TruncUserBB)
      continue;

    BinaryOperator *&InsertedShift = InsertedShifts[TruncUserBB];
    CastInst *&InsertedTrunc = InsertedTruncs[TruncUserBB];

    if (!InsertedShift && !InsertedTrunc) {
      BasicBlock::iterator InsertPt = TruncUserBB->getFirstInsertionPt();
      assert(InsertPt != TruncUserBB->end());
      // Sink the shift
      if (ShiftI->getOpcode() == Instruction::AShr)
        InsertedShift = BinaryOperator::CreateAShr(ShiftI->getOperand(0), CI,
                                                   "", &*InsertPt);
      else
        InsertedShift = BinaryOperator::CreateLShr(ShiftI->getOperand(0), CI,
                                                   "", &*InsertPt);

      // Sink the trunc
      BasicBlock::iterator TruncInsertPt = TruncUserBB->getFirstInsertionPt();
      TruncInsertPt++;
      assert(TruncInsertPt != TruncUserBB->end());

      InsertedTrunc = CastInst::Create(TruncI->getOpcode(), InsertedShift,
                                       TruncI->getType(), "", &*TruncInsertPt);

      MadeChange = true;

      TruncTheUse = InsertedTrunc;
    }
  }
  return MadeChange;
}

/// Sink the shift *right* instruction into user blocks if the uses could
/// potentially be combined with this shift instruction and generate BitExtract
/// instruction. It will only be applied if the architecture supports BitExtract
/// instruction. Here is an example:
/// BB1:
///   %x.extract.shift = lshr i64 %arg1, 32
/// BB2:
///   %x.extract.trunc = trunc i64 %x.extract.shift to i16
/// ==>
///
/// BB2:
///   %x.extract.shift.1 = lshr i64 %arg1, 32
///   %x.extract.trunc = trunc i64 %x.extract.shift.1 to i16
///
/// CodeGen will recoginze the pattern in BB2 and generate BitExtract
/// instruction.
/// Return true if any changes are made.
static bool OptimizeExtractBits(BinaryOperator *ShiftI, ConstantInt *CI,
                                const TargetLowering &TLI,
                                const DataLayout &DL) {
  BasicBlock *DefBB = ShiftI->getParent();

  /// Only insert instructions in each block once.
  DenseMap<BasicBlock *, BinaryOperator *> InsertedShifts;

  bool shiftIsLegal = TLI.isTypeLegal(TLI.getValueType(DL, ShiftI->getType()));

  bool MadeChange = false;
  for (Value::user_iterator UI = ShiftI->user_begin(), E = ShiftI->user_end();
       UI != E;) {
    Use &TheUse = UI.getUse();
    Instruction *User = cast<Instruction>(*UI);
    // Preincrement use iterator so we don't invalidate it.
    ++UI;

    // Don't bother for PHI nodes.
    if (isa<PHINode>(User))
      continue;

    if (!isExtractBitsCandidateUse(User))
      continue;

    BasicBlock *UserBB = User->getParent();

    if (UserBB == DefBB) {
      // If the shift and truncate instruction are in the same BB. The use of
      // the truncate(TruncUse) may still introduce another truncate if not
      // legal. In this case, we would like to sink both shift and truncate
      // instruction to the BB of TruncUse.
      // for example:
      // BB1:
      // i64 shift.result = lshr i64 opnd, imm
      // trunc.result = trunc shift.result to i16
      //
      // BB2:
      //   ----> We will have an implicit truncate here if the architecture does
      //   not have i16 compare.
      // cmp i16 trunc.result, opnd2
      //
      if (isa<TruncInst>(User) && shiftIsLegal
          // If the type of the truncate is legal, no trucate will be
          // introduced in other basic blocks.
          &&
          (!TLI.isTypeLegal(TLI.getValueType(DL, User->getType()))))
        MadeChange =
            SinkShiftAndTruncate(ShiftI, User, CI, InsertedShifts, TLI, DL);

      continue;
    }
    // If we have already inserted a shift into this block, use it.
    BinaryOperator *&InsertedShift = InsertedShifts[UserBB];

    if (!InsertedShift) {
      BasicBlock::iterator InsertPt = UserBB->getFirstInsertionPt();
      assert(InsertPt != UserBB->end());

      if (ShiftI->getOpcode() == Instruction::AShr)
        InsertedShift = BinaryOperator::CreateAShr(ShiftI->getOperand(0), CI,
                                                   "", &*InsertPt);
      else
        InsertedShift = BinaryOperator::CreateLShr(ShiftI->getOperand(0), CI,
                                                   "", &*InsertPt);

      MadeChange = true;
    }

    // Replace a use of the shift with a use of the new shift.
    TheUse = InsertedShift;
  }

  // If we removed all uses, nuke the shift.
  if (ShiftI->use_empty())
    ShiftI->eraseFromParent();

  return MadeChange;
}

/// If counting leading or trailing zeros is an expensive operation and a zero
/// input is defined, add a check for zero to avoid calling the intrinsic.
///
/// We want to transform:
///     %z = call i64 @llvm.cttz.i64(i64 %A, i1 false)
///
/// into:
///   entry:
///     %cmpz = icmp eq i64 %A, 0
///     br i1 %cmpz, label %cond.end, label %cond.false
///   cond.false:
///     %z = call i64 @llvm.cttz.i64(i64 %A, i1 true)
///     br label %cond.end
///   cond.end:
///     %ctz = phi i64 [ 64, %entry ], [ %z, %cond.false ]
///
/// If the transform is performed, return true and set ModifiedDT to true.
static bool despeculateCountZeros(IntrinsicInst *CountZeros,
                                  const TargetLowering *TLI,
                                  const DataLayout *DL,
                                  bool &ModifiedDT) {
  if (!TLI || !DL)
    return false;

  // If a zero input is undefined, it doesn't make sense to despeculate that.
  if (match(CountZeros->getOperand(1), m_One()))
    return false;

  // If it's cheap to speculate, there's nothing to do.
  auto IntrinsicID = CountZeros->getIntrinsicID();
  if ((IntrinsicID == Intrinsic::cttz && TLI->isCheapToSpeculateCttz()) ||
      (IntrinsicID == Intrinsic::ctlz && TLI->isCheapToSpeculateCtlz()))
    return false;

  // Only handle legal scalar cases. Anything else requires too much work.
  Type *Ty = CountZeros->getType();
  unsigned SizeInBits = Ty->getPrimitiveSizeInBits();
  if (Ty->isVectorTy() || SizeInBits > DL->getLargestLegalIntTypeSizeInBits())
    return false;

  // The intrinsic will be sunk behind a compare against zero and branch.
  BasicBlock *StartBlock = CountZeros->getParent();
  BasicBlock *CallBlock = StartBlock->splitBasicBlock(CountZeros, "cond.false");

  // Create another block after the count zero intrinsic. A PHI will be added
  // in this block to select the result of the intrinsic or the bit-width
  // constant if the input to the intrinsic is zero.
  BasicBlock::iterator SplitPt = ++(BasicBlock::iterator(CountZeros));
  BasicBlock *EndBlock = CallBlock->splitBasicBlock(SplitPt, "cond.end");

  // Set up a builder to create a compare, conditional branch, and PHI.
  IRBuilder<> Builder(CountZeros->getContext());
  Builder.SetInsertPoint(StartBlock->getTerminator());
  Builder.SetCurrentDebugLocation(CountZeros->getDebugLoc());

  // Replace the unconditional branch that was created by the first split with
  // a compare against zero and a conditional branch.
  Value *Zero = Constant::getNullValue(Ty);
  Value *Cmp = Builder.CreateICmpEQ(CountZeros->getOperand(0), Zero, "cmpz");
  Builder.CreateCondBr(Cmp, EndBlock, CallBlock);
  StartBlock->getTerminator()->eraseFromParent();

  // Create a PHI in the end block to select either the output of the intrinsic
  // or the bit width of the operand.
  Builder.SetInsertPoint(&EndBlock->front());
  PHINode *PN = Builder.CreatePHI(Ty, 2, "ctz");
  CountZeros->replaceAllUsesWith(PN);
  Value *BitWidth = Builder.getInt(APInt(SizeInBits, SizeInBits));
  PN->addIncoming(BitWidth, StartBlock);
  PN->addIncoming(CountZeros, CallBlock);

  // We are explicitly handling the zero case, so we can set the intrinsic's
  // undefined zero argument to 'true'. This will also prevent reprocessing the
  // intrinsic; we only despeculate when a zero input is defined.
  CountZeros->setArgOperand(1, Builder.getTrue());
  ModifiedDT = true;
  return true;
}

// This class provides helper functions to expand a memcmp library call into an
// inline expansion.
class MemCmpExpansion {
  struct ResultBlock {
    BasicBlock *BB;
    PHINode *PhiSrc1;
    PHINode *PhiSrc2;
    ResultBlock();
  };

  CallInst *CI;
  ResultBlock ResBlock;
  unsigned MaxLoadSize;
  unsigned NumBlocks;
  unsigned NumBlocksNonOneByte;
  unsigned NumLoadsPerBlock;
  std::vector<BasicBlock *> LoadCmpBlocks;
  BasicBlock *EndBlock;
  PHINode *PhiRes;
  bool IsUsedForZeroCmp;
  const DataLayout &DL;
  IRBuilder<> Builder;

  unsigned calculateNumBlocks(unsigned Size);
  void createLoadCmpBlocks();
  void createResultBlock();
  void setupResultBlockPHINodes();
  void setupEndBlockPHINodes();
  void emitLoadCompareBlock(unsigned Index, unsigned LoadSize,
                            unsigned GEPIndex);
  Value *getCompareLoadPairs(unsigned Index, unsigned Size,
                             unsigned &NumBytesProcessed);
  void emitLoadCompareBlockMultipleLoads(unsigned Index, unsigned Size,
                                         unsigned &NumBytesProcessed);
  void emitLoadCompareByteBlock(unsigned Index, unsigned GEPIndex);
  void emitMemCmpResultBlock();
  Value *getMemCmpExpansionZeroCase(unsigned Size);
  Value *getMemCmpEqZeroOneBlock(unsigned Size);
  Value *getMemCmpOneBlock(unsigned Size);
  unsigned getLoadSize(unsigned Size);
  unsigned getNumLoads(unsigned Size);

public:
  MemCmpExpansion(CallInst *CI, uint64_t Size, unsigned MaxLoadSize,
                  unsigned NumLoadsPerBlock, const DataLayout &DL);
  Value *getMemCmpExpansion(uint64_t Size);
};

MemCmpExpansion::ResultBlock::ResultBlock()
    : BB(nullptr), PhiSrc1(nullptr), PhiSrc2(nullptr) {}

// Initialize the basic block structure required for expansion of memcmp call
// with given maximum load size and memcmp size parameter.
// This structure includes:
// 1. A list of load compare blocks - LoadCmpBlocks.
// 2. An EndBlock, split from original instruction point, which is the block to
// return from.
// 3. ResultBlock, block to branch to for early exit when a
// LoadCmpBlock finds a difference.
MemCmpExpansion::MemCmpExpansion(CallInst *CI, uint64_t Size,
                                 unsigned MaxLoadSize, unsigned LoadsPerBlock,
                                 const DataLayout &TheDataLayout)
    : CI(CI), MaxLoadSize(MaxLoadSize), NumLoadsPerBlock(LoadsPerBlock),
      DL(TheDataLayout), Builder(CI) {

  // A memcmp with zero-comparison with only one block of load and compare does
  // not need to set up any extra blocks. This case could be handled in the DAG,
  // but since we have all of the machinery to flexibly expand any memcpy here,
  // we choose to handle this case too to avoid fragmented lowering.
  IsUsedForZeroCmp = isOnlyUsedInZeroEqualityComparison(CI);
  NumBlocks = calculateNumBlocks(Size);
  if ((!IsUsedForZeroCmp && NumLoadsPerBlock != 1) || NumBlocks != 1) {
    BasicBlock *StartBlock = CI->getParent();
    EndBlock = StartBlock->splitBasicBlock(CI, "endblock");
    setupEndBlockPHINodes();
    createResultBlock();

    // If return value of memcmp is not used in a zero equality, we need to
    // calculate which source was larger. The calculation requires the
    // two loaded source values of each load compare block.
    // These will be saved in the phi nodes created by setupResultBlockPHINodes.
    if (!IsUsedForZeroCmp)
      setupResultBlockPHINodes();

    // Create the number of required load compare basic blocks.
    createLoadCmpBlocks();

    // Update the terminator added by splitBasicBlock to branch to the first
    // LoadCmpBlock.
    StartBlock->getTerminator()->setSuccessor(0, LoadCmpBlocks[0]);
  }

  Builder.SetCurrentDebugLocation(CI->getDebugLoc());
}

void MemCmpExpansion::createLoadCmpBlocks() {
  for (unsigned i = 0; i < NumBlocks; i++) {
    BasicBlock *BB = BasicBlock::Create(CI->getContext(), "loadbb",
                                        EndBlock->getParent(), EndBlock);
    LoadCmpBlocks.push_back(BB);
  }
}

void MemCmpExpansion::createResultBlock() {
  ResBlock.BB = BasicBlock::Create(CI->getContext(), "res_block",
                                   EndBlock->getParent(), EndBlock);
}

// This function creates the IR instructions for loading and comparing 1 byte.
// It loads 1 byte from each source of the memcmp parameters with the given
// GEPIndex. It then subtracts the two loaded values and adds this result to the
// final phi node for selecting the memcmp result.
void MemCmpExpansion::emitLoadCompareByteBlock(unsigned Index,
                                               unsigned GEPIndex) {
  Value *Source1 = CI->getArgOperand(0);
  Value *Source2 = CI->getArgOperand(1);

  Builder.SetInsertPoint(LoadCmpBlocks[Index]);
  Type *LoadSizeType = Type::getInt8Ty(CI->getContext());
  // Cast source to LoadSizeType*.
  if (Source1->getType() != LoadSizeType)
    Source1 = Builder.CreateBitCast(Source1, LoadSizeType->getPointerTo());
  if (Source2->getType() != LoadSizeType)
    Source2 = Builder.CreateBitCast(Source2, LoadSizeType->getPointerTo());

  // Get the base address using the GEPIndex.
  if (GEPIndex != 0) {
    Source1 = Builder.CreateGEP(LoadSizeType, Source1,
                                ConstantInt::get(LoadSizeType, GEPIndex));
    Source2 = Builder.CreateGEP(LoadSizeType, Source2,
                                ConstantInt::get(LoadSizeType, GEPIndex));
  }

  Value *LoadSrc1 = Builder.CreateLoad(LoadSizeType, Source1);
  Value *LoadSrc2 = Builder.CreateLoad(LoadSizeType, Source2);

  LoadSrc1 = Builder.CreateZExt(LoadSrc1, Type::getInt32Ty(CI->getContext()));
  LoadSrc2 = Builder.CreateZExt(LoadSrc2, Type::getInt32Ty(CI->getContext()));
  Value *Diff = Builder.CreateSub(LoadSrc1, LoadSrc2);

  PhiRes->addIncoming(Diff, LoadCmpBlocks[Index]);

  if (Index < (LoadCmpBlocks.size() - 1)) {
    // Early exit branch if difference found to EndBlock. Otherwise, continue to
    // next LoadCmpBlock,
    Value *Cmp = Builder.CreateICmp(ICmpInst::ICMP_NE, Diff,
                                    ConstantInt::get(Diff->getType(), 0));
    BranchInst *CmpBr =
        BranchInst::Create(EndBlock, LoadCmpBlocks[Index + 1], Cmp);
    Builder.Insert(CmpBr);
  } else {
    // The last block has an unconditional branch to EndBlock.
    BranchInst *CmpBr = BranchInst::Create(EndBlock);
    Builder.Insert(CmpBr);
  }
}

unsigned MemCmpExpansion::getNumLoads(unsigned Size) {
  return (Size / MaxLoadSize) + countPopulation(Size % MaxLoadSize);
}

unsigned MemCmpExpansion::getLoadSize(unsigned Size) {
  return MinAlign(PowerOf2Floor(Size), MaxLoadSize);
}

/// Generate an equality comparison for one or more pairs of loaded values.
/// This is used in the case where the memcmp() call is compared equal or not
/// equal to zero.
Value *MemCmpExpansion::getCompareLoadPairs(unsigned Index, unsigned Size,
                                            unsigned &NumBytesProcessed) {
  std::vector<Value *> XorList, OrList;
  Value *Diff;

  unsigned RemainingBytes = Size - NumBytesProcessed;
  unsigned NumLoadsRemaining = getNumLoads(RemainingBytes);
  unsigned NumLoads = std::min(NumLoadsRemaining, NumLoadsPerBlock);

  // For a single-block expansion, start inserting before the memcmp call.
  if (LoadCmpBlocks.empty())
    Builder.SetInsertPoint(CI);
  else
    Builder.SetInsertPoint(LoadCmpBlocks[Index]);

  Value *Cmp = nullptr;
  for (unsigned i = 0; i < NumLoads; ++i) {
    unsigned LoadSize = getLoadSize(RemainingBytes);
    unsigned GEPIndex = NumBytesProcessed / LoadSize;
    NumBytesProcessed += LoadSize;
    RemainingBytes -= LoadSize;

    Type *LoadSizeType = IntegerType::get(CI->getContext(), LoadSize * 8);
    Type *MaxLoadType = IntegerType::get(CI->getContext(), MaxLoadSize * 8);
    assert(LoadSize <= MaxLoadSize && "Unexpected load type");

    Value *Source1 = CI->getArgOperand(0);
    Value *Source2 = CI->getArgOperand(1);

    // Cast source to LoadSizeType*.
    if (Source1->getType() != LoadSizeType)
      Source1 = Builder.CreateBitCast(Source1, LoadSizeType->getPointerTo());
    if (Source2->getType() != LoadSizeType)
      Source2 = Builder.CreateBitCast(Source2, LoadSizeType->getPointerTo());

    // Get the base address using the GEPIndex.
    if (GEPIndex != 0) {
      Source1 = Builder.CreateGEP(LoadSizeType, Source1,
                                  ConstantInt::get(LoadSizeType, GEPIndex));
      Source2 = Builder.CreateGEP(LoadSizeType, Source2,
                                  ConstantInt::get(LoadSizeType, GEPIndex));
    }

    // Get a constant or load a value for each source address.
    Value *LoadSrc1 = nullptr;
    if (auto *Source1C = dyn_cast<Constant>(Source1))
      LoadSrc1 = ConstantFoldLoadFromConstPtr(Source1C, LoadSizeType, DL);
    if (!LoadSrc1)
      LoadSrc1 = Builder.CreateLoad(LoadSizeType, Source1);

    Value *LoadSrc2 = nullptr;
    if (auto *Source2C = dyn_cast<Constant>(Source2))
      LoadSrc2 = ConstantFoldLoadFromConstPtr(Source2C, LoadSizeType, DL);
    if (!LoadSrc2)
      LoadSrc2 = Builder.CreateLoad(LoadSizeType, Source2);

    if (NumLoads != 1) {
      if (LoadSizeType != MaxLoadType) {
        LoadSrc1 = Builder.CreateZExt(LoadSrc1, MaxLoadType);
        LoadSrc2 = Builder.CreateZExt(LoadSrc2, MaxLoadType);
      }
      // If we have multiple loads per block, we need to generate a composite
      // comparison using xor+or.
      Diff = Builder.CreateXor(LoadSrc1, LoadSrc2);
      Diff = Builder.CreateZExt(Diff, MaxLoadType);
      XorList.push_back(Diff);
    } else {
      // If there's only one load per block, we just compare the loaded values.
      Cmp = Builder.CreateICmpNE(LoadSrc1, LoadSrc2);
    }
  }

  auto pairWiseOr = [&](std::vector<Value *> &InList) -> std::vector<Value *> {
    std::vector<Value *> OutList;
    for (unsigned i = 0; i < InList.size() - 1; i = i + 2) {
      Value *Or = Builder.CreateOr(InList[i], InList[i + 1]);
      OutList.push_back(Or);
    }
    if (InList.size() % 2 != 0)
      OutList.push_back(InList.back());
    return OutList;
  };

  if (!Cmp) {
    // Pairwise OR the XOR results.
    OrList = pairWiseOr(XorList);

    // Pairwise OR the OR results until one result left.
    while (OrList.size() != 1) {
      OrList = pairWiseOr(OrList);
    }
    Cmp = Builder.CreateICmpNE(OrList[0], ConstantInt::get(Diff->getType(), 0));
  }

  return Cmp;
}

void MemCmpExpansion::emitLoadCompareBlockMultipleLoads(
    unsigned Index, unsigned Size, unsigned &NumBytesProcessed) {
  Value *Cmp = getCompareLoadPairs(Index, Size, NumBytesProcessed);

  BasicBlock *NextBB = (Index == (LoadCmpBlocks.size() - 1))
                           ? EndBlock
                           : LoadCmpBlocks[Index + 1];
  // Early exit branch if difference found to ResultBlock. Otherwise,
  // continue to next LoadCmpBlock or EndBlock.
  BranchInst *CmpBr = BranchInst::Create(ResBlock.BB, NextBB, Cmp);
  Builder.Insert(CmpBr);

  // Add a phi edge for the last LoadCmpBlock to Endblock with a value of 0
  // since early exit to ResultBlock was not taken (no difference was found in
  // any of the bytes).
  if (Index == LoadCmpBlocks.size() - 1) {
    Value *Zero = ConstantInt::get(Type::getInt32Ty(CI->getContext()), 0);
    PhiRes->addIncoming(Zero, LoadCmpBlocks[Index]);
  }
}

// This function creates the IR intructions for loading and comparing using the
// given LoadSize. It loads the number of bytes specified by LoadSize from each
// source of the memcmp parameters. It then does a subtract to see if there was
// a difference in the loaded values. If a difference is found, it branches
// with an early exit to the ResultBlock for calculating which source was
// larger. Otherwise, it falls through to the either the next LoadCmpBlock or
// the EndBlock if this is the last LoadCmpBlock. Loading 1 byte is handled with
// a special case through emitLoadCompareByteBlock. The special handling can
// simply subtract the loaded values and add it to the result phi node.
void MemCmpExpansion::emitLoadCompareBlock(unsigned Index, unsigned LoadSize,
                                           unsigned GEPIndex) {
  if (LoadSize == 1) {
    MemCmpExpansion::emitLoadCompareByteBlock(Index, GEPIndex);
    return;
  }

  Type *LoadSizeType = IntegerType::get(CI->getContext(), LoadSize * 8);
  Type *MaxLoadType = IntegerType::get(CI->getContext(), MaxLoadSize * 8);
  assert(LoadSize <= MaxLoadSize && "Unexpected load type");

  Value *Source1 = CI->getArgOperand(0);
  Value *Source2 = CI->getArgOperand(1);

  Builder.SetInsertPoint(LoadCmpBlocks[Index]);
  // Cast source to LoadSizeType*.
  if (Source1->getType() != LoadSizeType)
    Source1 = Builder.CreateBitCast(Source1, LoadSizeType->getPointerTo());
  if (Source2->getType() != LoadSizeType)
    Source2 = Builder.CreateBitCast(Source2, LoadSizeType->getPointerTo());

  // Get the base address using the GEPIndex.
  if (GEPIndex != 0) {
    Source1 = Builder.CreateGEP(LoadSizeType, Source1,
                                ConstantInt::get(LoadSizeType, GEPIndex));
    Source2 = Builder.CreateGEP(LoadSizeType, Source2,
                                ConstantInt::get(LoadSizeType, GEPIndex));
  }

  // Load LoadSizeType from the base address.
  Value *LoadSrc1 = Builder.CreateLoad(LoadSizeType, Source1);
  Value *LoadSrc2 = Builder.CreateLoad(LoadSizeType, Source2);

  if (DL.isLittleEndian()) {
    Function *Bswap = Intrinsic::getDeclaration(CI->getModule(),
                                                Intrinsic::bswap, LoadSizeType);
    LoadSrc1 = Builder.CreateCall(Bswap, LoadSrc1);
    LoadSrc2 = Builder.CreateCall(Bswap, LoadSrc2);
  }

  if (LoadSizeType != MaxLoadType) {
    LoadSrc1 = Builder.CreateZExt(LoadSrc1, MaxLoadType);
    LoadSrc2 = Builder.CreateZExt(LoadSrc2, MaxLoadType);
  }

  // Add the loaded values to the phi nodes for calculating memcmp result only
  // if result is not used in a zero equality.
  if (!IsUsedForZeroCmp) {
    ResBlock.PhiSrc1->addIncoming(LoadSrc1, LoadCmpBlocks[Index]);
    ResBlock.PhiSrc2->addIncoming(LoadSrc2, LoadCmpBlocks[Index]);
  }

  Value *Cmp = Builder.CreateICmp(ICmpInst::ICMP_EQ, LoadSrc1, LoadSrc2);
  BasicBlock *NextBB = (Index == (LoadCmpBlocks.size() - 1))
                           ? EndBlock
                           : LoadCmpBlocks[Index + 1];
  // Early exit branch if difference found to ResultBlock. Otherwise, continue
  // to next LoadCmpBlock or EndBlock.
  BranchInst *CmpBr = BranchInst::Create(NextBB, ResBlock.BB, Cmp);
  Builder.Insert(CmpBr);

  // Add a phi edge for the last LoadCmpBlock to Endblock with a value of 0
  // since early exit to ResultBlock was not taken (no difference was found in
  // any of the bytes).
  if (Index == LoadCmpBlocks.size() - 1) {
    Value *Zero = ConstantInt::get(Type::getInt32Ty(CI->getContext()), 0);
    PhiRes->addIncoming(Zero, LoadCmpBlocks[Index]);
  }
}

// This function populates the ResultBlock with a sequence to calculate the
// memcmp result. It compares the two loaded source values and returns -1 if
// src1 < src2 and 1 if src1 > src2.
void MemCmpExpansion::emitMemCmpResultBlock() {
  // Special case: if memcmp result is used in a zero equality, result does not
  // need to be calculated and can simply return 1.
  if (IsUsedForZeroCmp) {
    BasicBlock::iterator InsertPt = ResBlock.BB->getFirstInsertionPt();
    Builder.SetInsertPoint(ResBlock.BB, InsertPt);
    Value *Res = ConstantInt::get(Type::getInt32Ty(CI->getContext()), 1);
    PhiRes->addIncoming(Res, ResBlock.BB);
    BranchInst *NewBr = BranchInst::Create(EndBlock);
    Builder.Insert(NewBr);
    return;
  }
  BasicBlock::iterator InsertPt = ResBlock.BB->getFirstInsertionPt();
  Builder.SetInsertPoint(ResBlock.BB, InsertPt);

  Value *Cmp = Builder.CreateICmp(ICmpInst::ICMP_ULT, ResBlock.PhiSrc1,
                                  ResBlock.PhiSrc2);

  Value *Res =
      Builder.CreateSelect(Cmp, ConstantInt::get(Builder.getInt32Ty(), -1),
                           ConstantInt::get(Builder.getInt32Ty(), 1));

  BranchInst *NewBr = BranchInst::Create(EndBlock);
  Builder.Insert(NewBr);
  PhiRes->addIncoming(Res, ResBlock.BB);
}

unsigned MemCmpExpansion::calculateNumBlocks(unsigned Size) {
  unsigned NumBlocks = 0;
  bool HaveOneByteLoad = false;
  unsigned RemainingSize = Size;
  unsigned LoadSize = MaxLoadSize;
  while (RemainingSize) {
    if (LoadSize == 1)
      HaveOneByteLoad = true;
    NumBlocks += RemainingSize / LoadSize;
    RemainingSize = RemainingSize % LoadSize;
    LoadSize = LoadSize / 2;
  }
  NumBlocksNonOneByte = HaveOneByteLoad ? (NumBlocks - 1) : NumBlocks;

  if (IsUsedForZeroCmp)
    NumBlocks = NumBlocks / NumLoadsPerBlock +
                (NumBlocks % NumLoadsPerBlock != 0 ? 1 : 0);

  return NumBlocks;
}

void MemCmpExpansion::setupResultBlockPHINodes() {
  Type *MaxLoadType = IntegerType::get(CI->getContext(), MaxLoadSize * 8);
  Builder.SetInsertPoint(ResBlock.BB);
  ResBlock.PhiSrc1 =
      Builder.CreatePHI(MaxLoadType, NumBlocksNonOneByte, "phi.src1");
  ResBlock.PhiSrc2 =
      Builder.CreatePHI(MaxLoadType, NumBlocksNonOneByte, "phi.src2");
}

void MemCmpExpansion::setupEndBlockPHINodes() {
  Builder.SetInsertPoint(&EndBlock->front());
  PhiRes = Builder.CreatePHI(Type::getInt32Ty(CI->getContext()), 2, "phi.res");
}

Value *MemCmpExpansion::getMemCmpExpansionZeroCase(unsigned Size) {
  unsigned NumBytesProcessed = 0;
  // This loop populates each of the LoadCmpBlocks with the IR sequence to
  // handle multiple loads per block.
  for (unsigned i = 0; i < NumBlocks; ++i)
    emitLoadCompareBlockMultipleLoads(i, Size, NumBytesProcessed);

  emitMemCmpResultBlock();
  return PhiRes;
}

/// A memcmp expansion that compares equality with 0 and only has one block of
/// load and compare can bypass the compare, branch, and phi IR that is required
/// in the general case.
Value *MemCmpExpansion::getMemCmpEqZeroOneBlock(unsigned Size) {
  unsigned NumBytesProcessed = 0;
  Value *Cmp = getCompareLoadPairs(0, Size, NumBytesProcessed);
  return Builder.CreateZExt(Cmp, Type::getInt32Ty(CI->getContext()));
}

/// A memcmp expansion that only has one block of load and compare can bypass
/// the compare, branch, and phi IR that is required in the general case.
Value *MemCmpExpansion::getMemCmpOneBlock(unsigned Size) {
  assert(NumLoadsPerBlock == 1 && "Only handles one load pair per block");

  Type *LoadSizeType = IntegerType::get(CI->getContext(), Size * 8);
  Value *Source1 = CI->getArgOperand(0);
  Value *Source2 = CI->getArgOperand(1);

  // Cast source to LoadSizeType*.
  if (Source1->getType() != LoadSizeType)
    Source1 = Builder.CreateBitCast(Source1, LoadSizeType->getPointerTo());
  if (Source2->getType() != LoadSizeType)
    Source2 = Builder.CreateBitCast(Source2, LoadSizeType->getPointerTo());

  // Load LoadSizeType from the base address.
  Value *LoadSrc1 = Builder.CreateLoad(LoadSizeType, Source1);
  Value *LoadSrc2 = Builder.CreateLoad(LoadSizeType, Source2);

  if (DL.isLittleEndian() && Size != 1) {
    Function *Bswap = Intrinsic::getDeclaration(CI->getModule(),
                                                Intrinsic::bswap, LoadSizeType);
    LoadSrc1 = Builder.CreateCall(Bswap, LoadSrc1);
    LoadSrc2 = Builder.CreateCall(Bswap, LoadSrc2);
  }

  // TODO: Instead of comparing ULT, just subtract and return the difference?
  Value *CmpNE = Builder.CreateICmpNE(LoadSrc1, LoadSrc2);
  Value *CmpULT = Builder.CreateICmpULT(LoadSrc1, LoadSrc2);
  Type *I32 = Builder.getInt32Ty();
  Value *Sel1 = Builder.CreateSelect(CmpULT, ConstantInt::get(I32, -1),
                                             ConstantInt::get(I32, 1));
  return Builder.CreateSelect(CmpNE, Sel1, ConstantInt::get(I32, 0));
}

// This function expands the memcmp call into an inline expansion and returns
// the memcmp result.
Value *MemCmpExpansion::getMemCmpExpansion(uint64_t Size) {
  if (IsUsedForZeroCmp)
    return NumBlocks == 1 ? getMemCmpEqZeroOneBlock(Size) :
                            getMemCmpExpansionZeroCase(Size);

  // TODO: Handle more than one load pair per block in getMemCmpOneBlock().
  if (NumBlocks == 1 && NumLoadsPerBlock == 1)
    return getMemCmpOneBlock(Size);

  // This loop calls emitLoadCompareBlock for comparing Size bytes of the two
  // memcmp sources. It starts with loading using the maximum load size set by
  // the target. It processes any remaining bytes using a load size which is the
  // next smallest power of 2.
  unsigned LoadSize = MaxLoadSize;
  unsigned NumBytesToBeProcessed = Size;
  unsigned Index = 0;
  while (NumBytesToBeProcessed) {
    // Calculate how many blocks we can create with the current load size.
    unsigned NumBlocks = NumBytesToBeProcessed / LoadSize;
    unsigned GEPIndex = (Size - NumBytesToBeProcessed) / LoadSize;
    NumBytesToBeProcessed = NumBytesToBeProcessed % LoadSize;

    // For each NumBlocks, populate the instruction sequence for loading and
    // comparing LoadSize bytes.
    while (NumBlocks--) {
      emitLoadCompareBlock(Index, LoadSize, GEPIndex);
      Index++;
      GEPIndex++;
    }
    // Get the next LoadSize to use.
    LoadSize = LoadSize / 2;
  }

  emitMemCmpResultBlock();
  return PhiRes;
}

// This function checks to see if an expansion of memcmp can be generated.
// It checks for constant compare size that is less than the max inline size.
// If an expansion cannot occur, returns false to leave as a library call.
// Otherwise, the library call is replaced with a new IR instruction sequence.
/// We want to transform:
/// %call = call signext i32 @memcmp(i8* %0, i8* %1, i64 15)
/// To:
/// loadbb:
///  %0 = bitcast i32* %buffer2 to i8*
///  %1 = bitcast i32* %buffer1 to i8*
///  %2 = bitcast i8* %1 to i64*
///  %3 = bitcast i8* %0 to i64*
///  %4 = load i64, i64* %2
///  %5 = load i64, i64* %3
///  %6 = call i64 @llvm.bswap.i64(i64 %4)
///  %7 = call i64 @llvm.bswap.i64(i64 %5)
///  %8 = sub i64 %6, %7
///  %9 = icmp ne i64 %8, 0
///  br i1 %9, label %res_block, label %loadbb1
/// res_block:                                        ; preds = %loadbb2,
/// %loadbb1, %loadbb
///  %phi.src1 = phi i64 [ %6, %loadbb ], [ %22, %loadbb1 ], [ %36, %loadbb2 ]
///  %phi.src2 = phi i64 [ %7, %loadbb ], [ %23, %loadbb1 ], [ %37, %loadbb2 ]
///  %10 = icmp ult i64 %phi.src1, %phi.src2
///  %11 = select i1 %10, i32 -1, i32 1
///  br label %endblock
/// loadbb1:                                          ; preds = %loadbb
///  %12 = bitcast i32* %buffer2 to i8*
///  %13 = bitcast i32* %buffer1 to i8*
///  %14 = bitcast i8* %13 to i32*
///  %15 = bitcast i8* %12 to i32*
///  %16 = getelementptr i32, i32* %14, i32 2
///  %17 = getelementptr i32, i32* %15, i32 2
///  %18 = load i32, i32* %16
///  %19 = load i32, i32* %17
///  %20 = call i32 @llvm.bswap.i32(i32 %18)
///  %21 = call i32 @llvm.bswap.i32(i32 %19)
///  %22 = zext i32 %20 to i64
///  %23 = zext i32 %21 to i64
///  %24 = sub i64 %22, %23
///  %25 = icmp ne i64 %24, 0
///  br i1 %25, label %res_block, label %loadbb2
/// loadbb2:                                          ; preds = %loadbb1
///  %26 = bitcast i32* %buffer2 to i8*
///  %27 = bitcast i32* %buffer1 to i8*
///  %28 = bitcast i8* %27 to i16*
///  %29 = bitcast i8* %26 to i16*
///  %30 = getelementptr i16, i16* %28, i16 6
///  %31 = getelementptr i16, i16* %29, i16 6
///  %32 = load i16, i16* %30
///  %33 = load i16, i16* %31
///  %34 = call i16 @llvm.bswap.i16(i16 %32)
///  %35 = call i16 @llvm.bswap.i16(i16 %33)
///  %36 = zext i16 %34 to i64
///  %37 = zext i16 %35 to i64
///  %38 = sub i64 %36, %37
///  %39 = icmp ne i64 %38, 0
///  br i1 %39, label %res_block, label %loadbb3
/// loadbb3:                                          ; preds = %loadbb2
///  %40 = bitcast i32* %buffer2 to i8*
///  %41 = bitcast i32* %buffer1 to i8*
///  %42 = getelementptr i8, i8* %41, i8 14
///  %43 = getelementptr i8, i8* %40, i8 14
///  %44 = load i8, i8* %42
///  %45 = load i8, i8* %43
///  %46 = zext i8 %44 to i32
///  %47 = zext i8 %45 to i32
///  %48 = sub i32 %46, %47
///  br label %endblock
/// endblock:                                         ; preds = %res_block,
/// %loadbb3
///  %phi.res = phi i32 [ %48, %loadbb3 ], [ %11, %res_block ]
///  ret i32 %phi.res
static bool expandMemCmp(CallInst *CI, const TargetTransformInfo *TTI,
                         const TargetLowering *TLI, const DataLayout *DL) {
  NumMemCmpCalls++;

  // TTI call to check if target would like to expand memcmp. Also, get the
  // MaxLoadSize.
  unsigned MaxLoadSize;
  if (!TTI->expandMemCmp(CI, MaxLoadSize))
    return false;

  // Early exit from expansion if -Oz.
  if (CI->getFunction()->optForMinSize())
    return false;

  // Early exit from expansion if size is not a constant.
  ConstantInt *SizeCast = dyn_cast<ConstantInt>(CI->getArgOperand(2));
  if (!SizeCast) {
    NumMemCmpNotConstant++;
    return false;
  }

  // Early exit from expansion if size greater than max bytes to load.
  uint64_t SizeVal = SizeCast->getZExtValue();
  unsigned NumLoads = 0;
  unsigned RemainingSize = SizeVal;
  unsigned LoadSize = MaxLoadSize;
  while (RemainingSize) {
    NumLoads += RemainingSize / LoadSize;
    RemainingSize = RemainingSize % LoadSize;
    LoadSize = LoadSize / 2;
  }

  if (NumLoads > TLI->getMaxExpandSizeMemcmp(CI->getFunction()->optForSize())) {
    NumMemCmpGreaterThanMax++;
    return false;
  }

  NumMemCmpInlined++;

  // MemCmpHelper object creates and sets up basic blocks required for
  // expanding memcmp with size SizeVal.
  unsigned NumLoadsPerBlock = MemCmpNumLoadsPerBlock;
  MemCmpExpansion MemCmpHelper(CI, SizeVal, MaxLoadSize, NumLoadsPerBlock, *DL);

  Value *Res = MemCmpHelper.getMemCmpExpansion(SizeVal);

  // Replace call with result of expansion and erase call.
  CI->replaceAllUsesWith(Res);
  CI->eraseFromParent();

  return true;
}

bool CodeGenPrepare::optimizeCallInst(CallInst *CI, bool &ModifiedDT) {
  BasicBlock *BB = CI->getParent();

  // Lower inline assembly if we can.
  // If we found an inline asm expession, and if the target knows how to
  // lower it to normal LLVM code, do so now.
  if (TLI && isa<InlineAsm>(CI->getCalledValue())) {
    if (TLI->ExpandInlineAsm(CI)) {
      // Avoid invalidating the iterator.
      CurInstIterator = BB->begin();
      // Avoid processing instructions out of order, which could cause
      // reuse before a value is defined.
      SunkAddrs.clear();
      return true;
    }
    // Sink address computing for memory operands into the block.
    if (optimizeInlineAsmInst(CI))
      return true;
  }

  // Align the pointer arguments to this call if the target thinks it's a good
  // idea
  unsigned MinSize, PrefAlign;
  if (TLI && TLI->shouldAlignPointerArgs(CI, MinSize, PrefAlign)) {
    for (auto &Arg : CI->arg_operands()) {
      // We want to align both objects whose address is used directly and
      // objects whose address is used in casts and GEPs, though it only makes
      // sense for GEPs if the offset is a multiple of the desired alignment and
      // if size - offset meets the size threshold.
      if (!Arg->getType()->isPointerTy())
        continue;
      APInt Offset(DL->getPointerSizeInBits(
                       cast<PointerType>(Arg->getType())->getAddressSpace()),
                   0);
      Value *Val = Arg->stripAndAccumulateInBoundsConstantOffsets(*DL, Offset);
      uint64_t Offset2 = Offset.getLimitedValue();
      if ((Offset2 & (PrefAlign-1)) != 0)
        continue;
      AllocaInst *AI;
      if ((AI = dyn_cast<AllocaInst>(Val)) && AI->getAlignment() < PrefAlign &&
          DL->getTypeAllocSize(AI->getAllocatedType()) >= MinSize + Offset2)
        AI->setAlignment(PrefAlign);
      // Global variables can only be aligned if they are defined in this
      // object (i.e. they are uniquely initialized in this object), and
      // over-aligning global variables that have an explicit section is
      // forbidden.
      GlobalVariable *GV;
      if ((GV = dyn_cast<GlobalVariable>(Val)) && GV->canIncreaseAlignment() &&
          GV->getPointerAlignment(*DL) < PrefAlign &&
          DL->getTypeAllocSize(GV->getValueType()) >=
              MinSize + Offset2)
        GV->setAlignment(PrefAlign);
    }
    // If this is a memcpy (or similar) then we may be able to improve the
    // alignment
    if (MemIntrinsic *MI = dyn_cast<MemIntrinsic>(CI)) {
      unsigned Align = getKnownAlignment(MI->getDest(), *DL);
      if (MemTransferInst *MTI = dyn_cast<MemTransferInst>(MI))
        Align = std::min(Align, getKnownAlignment(MTI->getSource(), *DL));
      if (Align > MI->getAlignment())
        MI->setAlignment(ConstantInt::get(MI->getAlignmentType(), Align));
    }
  }

  // If we have a cold call site, try to sink addressing computation into the
  // cold block.  This interacts with our handling for loads and stores to
  // ensure that we can fold all uses of a potential addressing computation
  // into their uses.  TODO: generalize this to work over profiling data
  if (!OptSize && CI->hasFnAttr(Attribute::Cold))
    for (auto &Arg : CI->arg_operands()) {
      if (!Arg->getType()->isPointerTy())
        continue;
      unsigned AS = Arg->getType()->getPointerAddressSpace();
      return optimizeMemoryInst(CI, Arg, Arg->getType(), AS);
    }

  IntrinsicInst *II = dyn_cast<IntrinsicInst>(CI);
  if (II) {
    switch (II->getIntrinsicID()) {
    default: break;
    case Intrinsic::objectsize: {
      // Lower all uses of llvm.objectsize.*
      ConstantInt *RetVal =
          lowerObjectSizeCall(II, *DL, TLInfo, /*MustSucceed=*/true);
      // Substituting this can cause recursive simplifications, which can
      // invalidate our iterator.  Use a WeakTrackingVH to hold onto it in case
      // this
      // happens.
      Value *CurValue = &*CurInstIterator;
      WeakTrackingVH IterHandle(CurValue);

      replaceAndRecursivelySimplify(CI, RetVal, TLInfo, nullptr);

      // If the iterator instruction was recursively deleted, start over at the
      // start of the block.
      if (IterHandle != CurValue) {
        CurInstIterator = BB->begin();
        SunkAddrs.clear();
      }
      return true;
    }
    case Intrinsic::aarch64_stlxr:
    case Intrinsic::aarch64_stxr: {
      ZExtInst *ExtVal = dyn_cast<ZExtInst>(CI->getArgOperand(0));
      if (!ExtVal || !ExtVal->hasOneUse() ||
          ExtVal->getParent() == CI->getParent())
        return false;
      // Sink a zext feeding stlxr/stxr before it, so it can be folded into it.
      ExtVal->moveBefore(CI);
      // Mark this instruction as "inserted by CGP", so that other
      // optimizations don't touch it.
      InsertedInsts.insert(ExtVal);
      return true;
    }
    case Intrinsic::invariant_group_barrier:
      II->replaceAllUsesWith(II->getArgOperand(0));
      II->eraseFromParent();
      return true;

    case Intrinsic::cttz:
    case Intrinsic::ctlz:
      // If counting zeros is expensive, try to avoid it.
      return despeculateCountZeros(II, TLI, DL, ModifiedDT);
    }

    if (TLI) {
      SmallVector<Value*, 2> PtrOps;
      Type *AccessTy;
      if (TLI->getAddrModeArguments(II, PtrOps, AccessTy))
        while (!PtrOps.empty()) {
          Value *PtrVal = PtrOps.pop_back_val();
          unsigned AS = PtrVal->getType()->getPointerAddressSpace();
          if (optimizeMemoryInst(II, PtrVal, AccessTy, AS))
            return true;
        }
    }
  }

  // From here on out we're working with named functions.
  if (!CI->getCalledFunction()) return false;

  // Lower all default uses of _chk calls.  This is very similar
  // to what InstCombineCalls does, but here we are only lowering calls
  // to fortified library functions (e.g. __memcpy_chk) that have the default
  // "don't know" as the objectsize.  Anything else should be left alone.
  FortifiedLibCallSimplifier Simplifier(TLInfo, true);
  if (Value *V = Simplifier.optimizeCall(CI)) {
    CI->replaceAllUsesWith(V);
    CI->eraseFromParent();
    return true;
  }

  LibFunc Func;
  if (TLInfo->getLibFunc(ImmutableCallSite(CI), Func) &&
      Func == LibFunc_memcmp && expandMemCmp(CI, TTI, TLI, DL)) {
    ModifiedDT = true;
    return true;
  }
  return false;
}

/// Look for opportunities to duplicate return instructions to the predecessor
/// to enable tail call optimizations. The case it is currently looking for is:
/// @code
/// bb0:
///   %tmp0 = tail call i32 @f0()
///   br label %return
/// bb1:
///   %tmp1 = tail call i32 @f1()
///   br label %return
/// bb2:
///   %tmp2 = tail call i32 @f2()
///   br label %return
/// return:
///   %retval = phi i32 [ %tmp0, %bb0 ], [ %tmp1, %bb1 ], [ %tmp2, %bb2 ]
///   ret i32 %retval
/// @endcode
///
/// =>
///
/// @code
/// bb0:
///   %tmp0 = tail call i32 @f0()
///   ret i32 %tmp0
/// bb1:
///   %tmp1 = tail call i32 @f1()
///   ret i32 %tmp1
/// bb2:
///   %tmp2 = tail call i32 @f2()
///   ret i32 %tmp2
/// @endcode
bool CodeGenPrepare::dupRetToEnableTailCallOpts(BasicBlock *BB) {
  if (!TLI)
    return false;

  ReturnInst *RetI = dyn_cast<ReturnInst>(BB->getTerminator());
  if (!RetI)
    return false;

  PHINode *PN = nullptr;
  BitCastInst *BCI = nullptr;
  Value *V = RetI->getReturnValue();
  if (V) {
    BCI = dyn_cast<BitCastInst>(V);
    if (BCI)
      V = BCI->getOperand(0);

    PN = dyn_cast<PHINode>(V);
    if (!PN)
      return false;
  }

  if (PN && PN->getParent() != BB)
    return false;

  // Make sure there are no instructions between the PHI and return, or that the
  // return is the first instruction in the block.
  if (PN) {
    BasicBlock::iterator BI = BB->begin();
    do { ++BI; } while (isa<DbgInfoIntrinsic>(BI));
    if (&*BI == BCI)
      // Also skip over the bitcast.
      ++BI;
    if (&*BI != RetI)
      return false;
  } else {
    BasicBlock::iterator BI = BB->begin();
    while (isa<DbgInfoIntrinsic>(BI)) ++BI;
    if (&*BI != RetI)
      return false;
  }

  /// Only dup the ReturnInst if the CallInst is likely to be emitted as a tail
  /// call.
  const Function *F = BB->getParent();
  SmallVector<CallInst*, 4> TailCalls;
  if (PN) {
    for (unsigned I = 0, E = PN->getNumIncomingValues(); I != E; ++I) {
      CallInst *CI = dyn_cast<CallInst>(PN->getIncomingValue(I));
      // Make sure the phi value is indeed produced by the tail call.
      if (CI && CI->hasOneUse() && CI->getParent() == PN->getIncomingBlock(I) &&
          TLI->mayBeEmittedAsTailCall(CI) &&
          attributesPermitTailCall(F, CI, RetI, *TLI))
        TailCalls.push_back(CI);
    }
  } else {
    SmallPtrSet<BasicBlock*, 4> VisitedBBs;
    for (pred_iterator PI = pred_begin(BB), PE = pred_end(BB); PI != PE; ++PI) {
      if (!VisitedBBs.insert(*PI).second)
        continue;

      BasicBlock::InstListType &InstList = (*PI)->getInstList();
      BasicBlock::InstListType::reverse_iterator RI = InstList.rbegin();
      BasicBlock::InstListType::reverse_iterator RE = InstList.rend();
      do { ++RI; } while (RI != RE && isa<DbgInfoIntrinsic>(&*RI));
      if (RI == RE)
        continue;

      CallInst *CI = dyn_cast<CallInst>(&*RI);
      if (CI && CI->use_empty() && TLI->mayBeEmittedAsTailCall(CI) &&
          attributesPermitTailCall(F, CI, RetI, *TLI))
        TailCalls.push_back(CI);
    }
  }

  bool Changed = false;
  for (unsigned i = 0, e = TailCalls.size(); i != e; ++i) {
    CallInst *CI = TailCalls[i];
    CallSite CS(CI);

    // Conservatively require the attributes of the call to match those of the
    // return. Ignore noalias because it doesn't affect the call sequence.
    AttributeList CalleeAttrs = CS.getAttributes();
    if (AttrBuilder(CalleeAttrs, AttributeList::ReturnIndex)
            .removeAttribute(Attribute::NoAlias) !=
        AttrBuilder(CalleeAttrs, AttributeList::ReturnIndex)
            .removeAttribute(Attribute::NoAlias))
      continue;

    // Make sure the call instruction is followed by an unconditional branch to
    // the return block.
    BasicBlock *CallBB = CI->getParent();
    BranchInst *BI = dyn_cast<BranchInst>(CallBB->getTerminator());
    if (!BI || !BI->isUnconditional() || BI->getSuccessor(0) != BB)
      continue;

    // Duplicate the return into CallBB.
    (void)FoldReturnIntoUncondBranch(RetI, BB, CallBB);
    ModifiedDT = Changed = true;
    ++NumRetsDup;
  }

  // If we eliminated all predecessors of the block, delete the block now.
  if (Changed && !BB->hasAddressTaken() && pred_begin(BB) == pred_end(BB))
    BB->eraseFromParent();

  return Changed;
}

//===----------------------------------------------------------------------===//
// Memory Optimization
//===----------------------------------------------------------------------===//

namespace {

/// This is an extended version of TargetLowering::AddrMode
/// which holds actual Value*'s for register values.
struct ExtAddrMode : public TargetLowering::AddrMode {
  Value *BaseReg;
  Value *ScaledReg;
  ExtAddrMode() : BaseReg(nullptr), ScaledReg(nullptr) {}
  void print(raw_ostream &OS) const;
  void dump() const;

  bool operator==(const ExtAddrMode& O) const {
    return (BaseReg == O.BaseReg) && (ScaledReg == O.ScaledReg) &&
           (BaseGV == O.BaseGV) && (BaseOffs == O.BaseOffs) &&
           (HasBaseReg == O.HasBaseReg) && (Scale == O.Scale);
  }
};

#ifndef NDEBUG
static inline raw_ostream &operator<<(raw_ostream &OS, const ExtAddrMode &AM) {
  AM.print(OS);
  return OS;
}
#endif

void ExtAddrMode::print(raw_ostream &OS) const {
  bool NeedPlus = false;
  OS << "[";
  if (BaseGV) {
    OS << (NeedPlus ? " + " : "")
       << "GV:";
    BaseGV->printAsOperand(OS, /*PrintType=*/false);
    NeedPlus = true;
  }

  if (BaseOffs) {
    OS << (NeedPlus ? " + " : "")
       << BaseOffs;
    NeedPlus = true;
  }

  if (BaseReg) {
    OS << (NeedPlus ? " + " : "")
       << "Base:";
    BaseReg->printAsOperand(OS, /*PrintType=*/false);
    NeedPlus = true;
  }
  if (Scale) {
    OS << (NeedPlus ? " + " : "")
       << Scale << "*";
    ScaledReg->printAsOperand(OS, /*PrintType=*/false);
  }

  OS << ']';
}

#if !defined(NDEBUG) || defined(LLVM_ENABLE_DUMP)
LLVM_DUMP_METHOD void ExtAddrMode::dump() const {
  print(dbgs());
  dbgs() << '\n';
}
#endif

/// \brief This class provides transaction based operation on the IR.
/// Every change made through this class is recorded in the internal state and
/// can be undone (rollback) until commit is called.
class TypePromotionTransaction {

  /// \brief This represents the common interface of the individual transaction.
  /// Each class implements the logic for doing one specific modification on
  /// the IR via the TypePromotionTransaction.
  class TypePromotionAction {
  protected:
    /// The Instruction modified.
    Instruction *Inst;

  public:
    /// \brief Constructor of the action.
    /// The constructor performs the related action on the IR.
    TypePromotionAction(Instruction *Inst) : Inst(Inst) {}

    virtual ~TypePromotionAction() {}

    /// \brief Undo the modification done by this action.
    /// When this method is called, the IR must be in the same state as it was
    /// before this action was applied.
    /// \pre Undoing the action works if and only if the IR is in the exact same
    /// state as it was directly after this action was applied.
    virtual void undo() = 0;

    /// \brief Advocate every change made by this action.
    /// When the results on the IR of the action are to be kept, it is important
    /// to call this function, otherwise hidden information may be kept forever.
    virtual void commit() {
      // Nothing to be done, this action is not doing anything.
    }
  };

  /// \brief Utility to remember the position of an instruction.
  class InsertionHandler {
    /// Position of an instruction.
    /// Either an instruction:
    /// - Is the first in a basic block: BB is used.
    /// - Has a previous instructon: PrevInst is used.
    union {
      Instruction *PrevInst;
      BasicBlock *BB;
    } Point;
    /// Remember whether or not the instruction had a previous instruction.
    bool HasPrevInstruction;

  public:
    /// \brief Record the position of \p Inst.
    InsertionHandler(Instruction *Inst) {
      BasicBlock::iterator It = Inst->getIterator();
      HasPrevInstruction = (It != (Inst->getParent()->begin()));
      if (HasPrevInstruction)
        Point.PrevInst = &*--It;
      else
        Point.BB = Inst->getParent();
    }

    /// \brief Insert \p Inst at the recorded position.
    void insert(Instruction *Inst) {
      if (HasPrevInstruction) {
        if (Inst->getParent())
          Inst->removeFromParent();
        Inst->insertAfter(Point.PrevInst);
      } else {
        Instruction *Position = &*Point.BB->getFirstInsertionPt();
        if (Inst->getParent())
          Inst->moveBefore(Position);
        else
          Inst->insertBefore(Position);
      }
    }
  };

  /// \brief Move an instruction before another.
  class InstructionMoveBefore : public TypePromotionAction {
    /// Original position of the instruction.
    InsertionHandler Position;

  public:
    /// \brief Move \p Inst before \p Before.
    InstructionMoveBefore(Instruction *Inst, Instruction *Before)
        : TypePromotionAction(Inst), Position(Inst) {
      DEBUG(dbgs() << "Do: move: " << *Inst << "\nbefore: " << *Before << "\n");
      Inst->moveBefore(Before);
    }

    /// \brief Move the instruction back to its original position.
    void undo() override {
      DEBUG(dbgs() << "Undo: moveBefore: " << *Inst << "\n");
      Position.insert(Inst);
    }
  };

  /// \brief Set the operand of an instruction with a new value.
  class OperandSetter : public TypePromotionAction {
    /// Original operand of the instruction.
    Value *Origin;
    /// Index of the modified instruction.
    unsigned Idx;

  public:
    /// \brief Set \p Idx operand of \p Inst with \p NewVal.
    OperandSetter(Instruction *Inst, unsigned Idx, Value *NewVal)
        : TypePromotionAction(Inst), Idx(Idx) {
      DEBUG(dbgs() << "Do: setOperand: " << Idx << "\n"
                   << "for:" << *Inst << "\n"
                   << "with:" << *NewVal << "\n");
      Origin = Inst->getOperand(Idx);
      Inst->setOperand(Idx, NewVal);
    }

    /// \brief Restore the original value of the instruction.
    void undo() override {
      DEBUG(dbgs() << "Undo: setOperand:" << Idx << "\n"
                   << "for: " << *Inst << "\n"
                   << "with: " << *Origin << "\n");
      Inst->setOperand(Idx, Origin);
    }
  };

  /// \brief Hide the operands of an instruction.
  /// Do as if this instruction was not using any of its operands.
  class OperandsHider : public TypePromotionAction {
    /// The list of original operands.
    SmallVector<Value *, 4> OriginalValues;

  public:
    /// \brief Remove \p Inst from the uses of the operands of \p Inst.
    OperandsHider(Instruction *Inst) : TypePromotionAction(Inst) {
      DEBUG(dbgs() << "Do: OperandsHider: " << *Inst << "\n");
      unsigned NumOpnds = Inst->getNumOperands();
      OriginalValues.reserve(NumOpnds);
      for (unsigned It = 0; It < NumOpnds; ++It) {
        // Save the current operand.
        Value *Val = Inst->getOperand(It);
        OriginalValues.push_back(Val);
        // Set a dummy one.
        // We could use OperandSetter here, but that would imply an overhead
        // that we are not willing to pay.
        Inst->setOperand(It, UndefValue::get(Val->getType()));
      }
    }

    /// \brief Restore the original list of uses.
    void undo() override {
      DEBUG(dbgs() << "Undo: OperandsHider: " << *Inst << "\n");
      for (unsigned It = 0, EndIt = OriginalValues.size(); It != EndIt; ++It)
        Inst->setOperand(It, OriginalValues[It]);
    }
  };

  /// \brief Build a truncate instruction.
  class TruncBuilder : public TypePromotionAction {
    Value *Val;
  public:
    /// \brief Build a truncate instruction of \p Opnd producing a \p Ty
    /// result.
    /// trunc Opnd to Ty.
    TruncBuilder(Instruction *Opnd, Type *Ty) : TypePromotionAction(Opnd) {
      IRBuilder<> Builder(Opnd);
      Val = Builder.CreateTrunc(Opnd, Ty, "promoted");
      DEBUG(dbgs() << "Do: TruncBuilder: " << *Val << "\n");
    }

    /// \brief Get the built value.
    Value *getBuiltValue() { return Val; }

    /// \brief Remove the built instruction.
    void undo() override {
      DEBUG(dbgs() << "Undo: TruncBuilder: " << *Val << "\n");
      if (Instruction *IVal = dyn_cast<Instruction>(Val))
        IVal->eraseFromParent();
    }
  };

  /// \brief Build a sign extension instruction.
  class SExtBuilder : public TypePromotionAction {
    Value *Val;
  public:
    /// \brief Build a sign extension instruction of \p Opnd producing a \p Ty
    /// result.
    /// sext Opnd to Ty.
    SExtBuilder(Instruction *InsertPt, Value *Opnd, Type *Ty)
        : TypePromotionAction(InsertPt) {
      IRBuilder<> Builder(InsertPt);
      Val = Builder.CreateSExt(Opnd, Ty, "promoted");
      DEBUG(dbgs() << "Do: SExtBuilder: " << *Val << "\n");
    }

    /// \brief Get the built value.
    Value *getBuiltValue() { return Val; }

    /// \brief Remove the built instruction.
    void undo() override {
      DEBUG(dbgs() << "Undo: SExtBuilder: " << *Val << "\n");
      if (Instruction *IVal = dyn_cast<Instruction>(Val))
        IVal->eraseFromParent();
    }
  };

  /// \brief Build a zero extension instruction.
  class ZExtBuilder : public TypePromotionAction {
    Value *Val;
  public:
    /// \brief Build a zero extension instruction of \p Opnd producing a \p Ty
    /// result.
    /// zext Opnd to Ty.
    ZExtBuilder(Instruction *InsertPt, Value *Opnd, Type *Ty)
        : TypePromotionAction(InsertPt) {
      IRBuilder<> Builder(InsertPt);
      Val = Builder.CreateZExt(Opnd, Ty, "promoted");
      DEBUG(dbgs() << "Do: ZExtBuilder: " << *Val << "\n");
    }

    /// \brief Get the built value.
    Value *getBuiltValue() { return Val; }

    /// \brief Remove the built instruction.
    void undo() override {
      DEBUG(dbgs() << "Undo: ZExtBuilder: " << *Val << "\n");
      if (Instruction *IVal = dyn_cast<Instruction>(Val))
        IVal->eraseFromParent();
    }
  };

  /// \brief Mutate an instruction to another type.
  class TypeMutator : public TypePromotionAction {
    /// Record the original type.
    Type *OrigTy;

  public:
    /// \brief Mutate the type of \p Inst into \p NewTy.
    TypeMutator(Instruction *Inst, Type *NewTy)
        : TypePromotionAction(Inst), OrigTy(Inst->getType()) {
      DEBUG(dbgs() << "Do: MutateType: " << *Inst << " with " << *NewTy
                   << "\n");
      Inst->mutateType(NewTy);
    }

    /// \brief Mutate the instruction back to its original type.
    void undo() override {
      DEBUG(dbgs() << "Undo: MutateType: " << *Inst << " with " << *OrigTy
                   << "\n");
      Inst->mutateType(OrigTy);
    }
  };

  /// \brief Replace the uses of an instruction by another instruction.
  class UsesReplacer : public TypePromotionAction {
    /// Helper structure to keep track of the replaced uses.
    struct InstructionAndIdx {
      /// The instruction using the instruction.
      Instruction *Inst;
      /// The index where this instruction is used for Inst.
      unsigned Idx;
      InstructionAndIdx(Instruction *Inst, unsigned Idx)
          : Inst(Inst), Idx(Idx) {}
    };

    /// Keep track of the original uses (pair Instruction, Index).
    SmallVector<InstructionAndIdx, 4> OriginalUses;
    typedef SmallVectorImpl<InstructionAndIdx>::iterator use_iterator;

  public:
    /// \brief Replace all the use of \p Inst by \p New.
    UsesReplacer(Instruction *Inst, Value *New) : TypePromotionAction(Inst) {
      DEBUG(dbgs() << "Do: UsersReplacer: " << *Inst << " with " << *New
                   << "\n");
      // Record the original uses.
      for (Use &U : Inst->uses()) {
        Instruction *UserI = cast<Instruction>(U.getUser());
        OriginalUses.push_back(InstructionAndIdx(UserI, U.getOperandNo()));
      }
      // Now, we can replace the uses.
      Inst->replaceAllUsesWith(New);
    }

    /// \brief Reassign the original uses of Inst to Inst.
    void undo() override {
      DEBUG(dbgs() << "Undo: UsersReplacer: " << *Inst << "\n");
      for (use_iterator UseIt = OriginalUses.begin(),
                        EndIt = OriginalUses.end();
           UseIt != EndIt; ++UseIt) {
        UseIt->Inst->setOperand(UseIt->Idx, Inst);
      }
    }
  };

  /// \brief Remove an instruction from the IR.
  class InstructionRemover : public TypePromotionAction {
    /// Original position of the instruction.
    InsertionHandler Inserter;
    /// Helper structure to hide all the link to the instruction. In other
    /// words, this helps to do as if the instruction was removed.
    OperandsHider Hider;
    /// Keep track of the uses replaced, if any.
    UsesReplacer *Replacer;
    /// Keep track of instructions removed.
    SetOfInstrs &RemovedInsts;

  public:
    /// \brief Remove all reference of \p Inst and optinally replace all its
    /// uses with New.
    /// \p RemovedInsts Keep track of the instructions removed by this Action.
    /// \pre If !Inst->use_empty(), then New != nullptr
    InstructionRemover(Instruction *Inst, SetOfInstrs &RemovedInsts,
                       Value *New = nullptr)
        : TypePromotionAction(Inst), Inserter(Inst), Hider(Inst),
          Replacer(nullptr), RemovedInsts(RemovedInsts) {
      if (New)
        Replacer = new UsesReplacer(Inst, New);
      DEBUG(dbgs() << "Do: InstructionRemover: " << *Inst << "\n");
      RemovedInsts.insert(Inst);
      /// The instructions removed here will be freed after completing
      /// optimizeBlock() for all blocks as we need to keep track of the
      /// removed instructions during promotion.
      Inst->removeFromParent();
    }

    ~InstructionRemover() override { delete Replacer; }

    /// \brief Resurrect the instruction and reassign it to the proper uses if
    /// new value was provided when build this action.
    void undo() override {
      DEBUG(dbgs() << "Undo: InstructionRemover: " << *Inst << "\n");
      Inserter.insert(Inst);
      if (Replacer)
        Replacer->undo();
      Hider.undo();
      RemovedInsts.erase(Inst);
    }
  };

public:
  /// Restoration point.
  /// The restoration point is a pointer to an action instead of an iterator
  /// because the iterator may be invalidated but not the pointer.
  typedef const TypePromotionAction *ConstRestorationPt;

  TypePromotionTransaction(SetOfInstrs &RemovedInsts)
      : RemovedInsts(RemovedInsts) {}

  /// Advocate every changes made in that transaction.
  void commit();
  /// Undo all the changes made after the given point.
  void rollback(ConstRestorationPt Point);
  /// Get the current restoration point.
  ConstRestorationPt getRestorationPoint() const;

  /// \name API for IR modification with state keeping to support rollback.
  /// @{
  /// Same as Instruction::setOperand.
  void setOperand(Instruction *Inst, unsigned Idx, Value *NewVal);
  /// Same as Instruction::eraseFromParent.
  void eraseInstruction(Instruction *Inst, Value *NewVal = nullptr);
  /// Same as Value::replaceAllUsesWith.
  void replaceAllUsesWith(Instruction *Inst, Value *New);
  /// Same as Value::mutateType.
  void mutateType(Instruction *Inst, Type *NewTy);
  /// Same as IRBuilder::createTrunc.
  Value *createTrunc(Instruction *Opnd, Type *Ty);
  /// Same as IRBuilder::createSExt.
  Value *createSExt(Instruction *Inst, Value *Opnd, Type *Ty);
  /// Same as IRBuilder::createZExt.
  Value *createZExt(Instruction *Inst, Value *Opnd, Type *Ty);
  /// Same as Instruction::moveBefore.
  void moveBefore(Instruction *Inst, Instruction *Before);
  /// @}

private:
  /// The ordered list of actions made so far.
  SmallVector<std::unique_ptr<TypePromotionAction>, 16> Actions;
  typedef SmallVectorImpl<std::unique_ptr<TypePromotionAction>>::iterator CommitPt;
  SetOfInstrs &RemovedInsts;
};

void TypePromotionTransaction::setOperand(Instruction *Inst, unsigned Idx,
                                          Value *NewVal) {
  Actions.push_back(
      make_unique<TypePromotionTransaction::OperandSetter>(Inst, Idx, NewVal));
}

void TypePromotionTransaction::eraseInstruction(Instruction *Inst,
                                                Value *NewVal) {
  Actions.push_back(
      make_unique<TypePromotionTransaction::InstructionRemover>(Inst,
                                                         RemovedInsts, NewVal));
}

void TypePromotionTransaction::replaceAllUsesWith(Instruction *Inst,
                                                  Value *New) {
  Actions.push_back(make_unique<TypePromotionTransaction::UsesReplacer>(Inst, New));
}

void TypePromotionTransaction::mutateType(Instruction *Inst, Type *NewTy) {
  Actions.push_back(make_unique<TypePromotionTransaction::TypeMutator>(Inst, NewTy));
}

Value *TypePromotionTransaction::createTrunc(Instruction *Opnd,
                                             Type *Ty) {
  std::unique_ptr<TruncBuilder> Ptr(new TruncBuilder(Opnd, Ty));
  Value *Val = Ptr->getBuiltValue();
  Actions.push_back(std::move(Ptr));
  return Val;
}

Value *TypePromotionTransaction::createSExt(Instruction *Inst,
                                            Value *Opnd, Type *Ty) {
  std::unique_ptr<SExtBuilder> Ptr(new SExtBuilder(Inst, Opnd, Ty));
  Value *Val = Ptr->getBuiltValue();
  Actions.push_back(std::move(Ptr));
  return Val;
}

Value *TypePromotionTransaction::createZExt(Instruction *Inst,
                                            Value *Opnd, Type *Ty) {
  std::unique_ptr<ZExtBuilder> Ptr(new ZExtBuilder(Inst, Opnd, Ty));
  Value *Val = Ptr->getBuiltValue();
  Actions.push_back(std::move(Ptr));
  return Val;
}

void TypePromotionTransaction::moveBefore(Instruction *Inst,
                                          Instruction *Before) {
  Actions.push_back(
      make_unique<TypePromotionTransaction::InstructionMoveBefore>(Inst, Before));
}

TypePromotionTransaction::ConstRestorationPt
TypePromotionTransaction::getRestorationPoint() const {
  return !Actions.empty() ? Actions.back().get() : nullptr;
}

void TypePromotionTransaction::commit() {
  for (CommitPt It = Actions.begin(), EndIt = Actions.end(); It != EndIt;
       ++It)
    (*It)->commit();
  Actions.clear();
}

void TypePromotionTransaction::rollback(
    TypePromotionTransaction::ConstRestorationPt Point) {
  while (!Actions.empty() && Point != Actions.back().get()) {
    std::unique_ptr<TypePromotionAction> Curr = Actions.pop_back_val();
    Curr->undo();
  }
}

/// \brief A helper class for matching addressing modes.
///
/// This encapsulates the logic for matching the target-legal addressing modes.
class AddressingModeMatcher {
  SmallVectorImpl<Instruction*> &AddrModeInsts;
  const TargetLowering &TLI;
  const TargetRegisterInfo &TRI;
  const DataLayout &DL;

  /// AccessTy/MemoryInst - This is the type for the access (e.g. double) and
  /// the memory instruction that we're computing this address for.
  Type *AccessTy;
  unsigned AddrSpace;
  Instruction *MemoryInst;

  /// This is the addressing mode that we're building up. This is
  /// part of the return value of this addressing mode matching stuff.
  ExtAddrMode &AddrMode;

  /// The instructions inserted by other CodeGenPrepare optimizations.
  const SetOfInstrs &InsertedInsts;
  /// A map from the instructions to their type before promotion.
  InstrToOrigTy &PromotedInsts;
  /// The ongoing transaction where every action should be registered.
  TypePromotionTransaction &TPT;

  /// This is set to true when we should not do profitability checks.
  /// When true, IsProfitableToFoldIntoAddressingMode always returns true.
  bool IgnoreProfitability;

  AddressingModeMatcher(SmallVectorImpl<Instruction *> &AMI,
                        const TargetLowering &TLI,
                        const TargetRegisterInfo &TRI,
                        Type *AT, unsigned AS,
                        Instruction *MI, ExtAddrMode &AM,
                        const SetOfInstrs &InsertedInsts,
                        InstrToOrigTy &PromotedInsts,
                        TypePromotionTransaction &TPT)
      : AddrModeInsts(AMI), TLI(TLI), TRI(TRI),
        DL(MI->getModule()->getDataLayout()), AccessTy(AT), AddrSpace(AS),
        MemoryInst(MI), AddrMode(AM), InsertedInsts(InsertedInsts),
        PromotedInsts(PromotedInsts), TPT(TPT) {
    IgnoreProfitability = false;
  }
public:

  /// Find the maximal addressing mode that a load/store of V can fold,
  /// give an access type of AccessTy.  This returns a list of involved
  /// instructions in AddrModeInsts.
  /// \p InsertedInsts The instructions inserted by other CodeGenPrepare
  /// optimizations.
  /// \p PromotedInsts maps the instructions to their type before promotion.
  /// \p The ongoing transaction where every action should be registered.
  static ExtAddrMode Match(Value *V, Type *AccessTy, unsigned AS,
                           Instruction *MemoryInst,
                           SmallVectorImpl<Instruction*> &AddrModeInsts,
                           const TargetLowering &TLI,
                           const TargetRegisterInfo &TRI,
                           const SetOfInstrs &InsertedInsts,
                           InstrToOrigTy &PromotedInsts,
                           TypePromotionTransaction &TPT) {
    ExtAddrMode Result;

    bool Success = AddressingModeMatcher(AddrModeInsts, TLI, TRI,
                                         AccessTy, AS,
                                         MemoryInst, Result, InsertedInsts,
                                         PromotedInsts, TPT).matchAddr(V, 0);
    (void)Success; assert(Success && "Couldn't select *anything*?");
    return Result;
  }
private:
  bool matchScaledValue(Value *ScaleReg, int64_t Scale, unsigned Depth);
  bool matchAddr(Value *V, unsigned Depth);
  bool matchOperationAddr(User *Operation, unsigned Opcode, unsigned Depth,
                          bool *MovedAway = nullptr);
  bool isProfitableToFoldIntoAddressingMode(Instruction *I,
                                            ExtAddrMode &AMBefore,
                                            ExtAddrMode &AMAfter);
  bool valueAlreadyLiveAtInst(Value *Val, Value *KnownLive1, Value *KnownLive2);
  bool isPromotionProfitable(unsigned NewCost, unsigned OldCost,
                             Value *PromotedOperand) const;
};

/// Try adding ScaleReg*Scale to the current addressing mode.
/// Return true and update AddrMode if this addr mode is legal for the target,
/// false if not.
bool AddressingModeMatcher::matchScaledValue(Value *ScaleReg, int64_t Scale,
                                             unsigned Depth) {
  // If Scale is 1, then this is the same as adding ScaleReg to the addressing
  // mode.  Just process that directly.
  if (Scale == 1)
    return matchAddr(ScaleReg, Depth);

  // If the scale is 0, it takes nothing to add this.
  if (Scale == 0)
    return true;

  // If we already have a scale of this value, we can add to it, otherwise, we
  // need an available scale field.
  if (AddrMode.Scale != 0 && AddrMode.ScaledReg != ScaleReg)
    return false;

  ExtAddrMode TestAddrMode = AddrMode;

  // Add scale to turn X*4+X*3 -> X*7.  This could also do things like
  // [A+B + A*7] -> [B+A*8].
  TestAddrMode.Scale += Scale;
  TestAddrMode.ScaledReg = ScaleReg;

  // If the new address isn't legal, bail out.
  if (!TLI.isLegalAddressingMode(DL, TestAddrMode, AccessTy, AddrSpace))
    return false;

  // It was legal, so commit it.
  AddrMode = TestAddrMode;

  // Okay, we decided that we can add ScaleReg+Scale to AddrMode.  Check now
  // to see if ScaleReg is actually X+C.  If so, we can turn this into adding
  // X*Scale + C*Scale to addr mode.
  ConstantInt *CI = nullptr; Value *AddLHS = nullptr;
  if (isa<Instruction>(ScaleReg) &&  // not a constant expr.
      match(ScaleReg, m_Add(m_Value(AddLHS), m_ConstantInt(CI)))) {
    TestAddrMode.ScaledReg = AddLHS;
    TestAddrMode.BaseOffs += CI->getSExtValue()*TestAddrMode.Scale;

    // If this addressing mode is legal, commit it and remember that we folded
    // this instruction.
    if (TLI.isLegalAddressingMode(DL, TestAddrMode, AccessTy, AddrSpace)) {
      AddrModeInsts.push_back(cast<Instruction>(ScaleReg));
      AddrMode = TestAddrMode;
      return true;
    }
  }

  // Otherwise, not (x+c)*scale, just return what we have.
  return true;
}

/// This is a little filter, which returns true if an addressing computation
/// involving I might be folded into a load/store accessing it.
/// This doesn't need to be perfect, but needs to accept at least
/// the set of instructions that MatchOperationAddr can.
static bool MightBeFoldableInst(Instruction *I) {
  switch (I->getOpcode()) {
  case Instruction::BitCast:
  case Instruction::AddrSpaceCast:
    // Don't touch identity bitcasts.
    if (I->getType() == I->getOperand(0)->getType())
      return false;
    return I->getType()->isPointerTy() || I->getType()->isIntegerTy();
  case Instruction::PtrToInt:
    // PtrToInt is always a noop, as we know that the int type is pointer sized.
    return true;
  case Instruction::IntToPtr:
    // We know the input is intptr_t, so this is foldable.
    return true;
  case Instruction::Add:
    return true;
  case Instruction::Mul:
  case Instruction::Shl:
    // Can only handle X*C and X << C.
    return isa<ConstantInt>(I->getOperand(1));
  case Instruction::GetElementPtr:
    return true;
  default:
    return false;
  }
}

/// \brief Check whether or not \p Val is a legal instruction for \p TLI.
/// \note \p Val is assumed to be the product of some type promotion.
/// Therefore if \p Val has an undefined state in \p TLI, this is assumed
/// to be legal, as the non-promoted value would have had the same state.
static bool isPromotedInstructionLegal(const TargetLowering &TLI,
                                       const DataLayout &DL, Value *Val) {
  Instruction *PromotedInst = dyn_cast<Instruction>(Val);
  if (!PromotedInst)
    return false;
  int ISDOpcode = TLI.InstructionOpcodeToISD(PromotedInst->getOpcode());
  // If the ISDOpcode is undefined, it was undefined before the promotion.
  if (!ISDOpcode)
    return true;
  // Otherwise, check if the promoted instruction is legal or not.
  return TLI.isOperationLegalOrCustom(
      ISDOpcode, TLI.getValueType(DL, PromotedInst->getType()));
}

/// \brief Hepler class to perform type promotion.
class TypePromotionHelper {
  /// \brief Utility function to check whether or not a sign or zero extension
  /// of \p Inst with \p ConsideredExtType can be moved through \p Inst by
  /// either using the operands of \p Inst or promoting \p Inst.
  /// The type of the extension is defined by \p IsSExt.
  /// In other words, check if:
  /// ext (Ty Inst opnd1 opnd2 ... opndN) to ConsideredExtType.
  /// #1 Promotion applies:
  /// ConsideredExtType Inst (ext opnd1 to ConsideredExtType, ...).
  /// #2 Operand reuses:
  /// ext opnd1 to ConsideredExtType.
  /// \p PromotedInsts maps the instructions to their type before promotion.
  static bool canGetThrough(const Instruction *Inst, Type *ConsideredExtType,
                            const InstrToOrigTy &PromotedInsts, bool IsSExt);

  /// \brief Utility function to determine if \p OpIdx should be promoted when
  /// promoting \p Inst.
  static bool shouldExtOperand(const Instruction *Inst, int OpIdx) {
    return !(isa<SelectInst>(Inst) && OpIdx == 0);
  }

  /// \brief Utility function to promote the operand of \p Ext when this
  /// operand is a promotable trunc or sext or zext.
  /// \p PromotedInsts maps the instructions to their type before promotion.
  /// \p CreatedInstsCost[out] contains the cost of all instructions
  /// created to promote the operand of Ext.
  /// Newly added extensions are inserted in \p Exts.
  /// Newly added truncates are inserted in \p Truncs.
  /// Should never be called directly.
  /// \return The promoted value which is used instead of Ext.
  static Value *promoteOperandForTruncAndAnyExt(
      Instruction *Ext, TypePromotionTransaction &TPT,
      InstrToOrigTy &PromotedInsts, unsigned &CreatedInstsCost,
      SmallVectorImpl<Instruction *> *Exts,
      SmallVectorImpl<Instruction *> *Truncs, const TargetLowering &TLI);

  /// \brief Utility function to promote the operand of \p Ext when this
  /// operand is promotable and is not a supported trunc or sext.
  /// \p PromotedInsts maps the instructions to their type before promotion.
  /// \p CreatedInstsCost[out] contains the cost of all the instructions
  /// created to promote the operand of Ext.
  /// Newly added extensions are inserted in \p Exts.
  /// Newly added truncates are inserted in \p Truncs.
  /// Should never be called directly.
  /// \return The promoted value which is used instead of Ext.
  static Value *promoteOperandForOther(Instruction *Ext,
                                       TypePromotionTransaction &TPT,
                                       InstrToOrigTy &PromotedInsts,
                                       unsigned &CreatedInstsCost,
                                       SmallVectorImpl<Instruction *> *Exts,
                                       SmallVectorImpl<Instruction *> *Truncs,
                                       const TargetLowering &TLI, bool IsSExt);

  /// \see promoteOperandForOther.
  static Value *signExtendOperandForOther(
      Instruction *Ext, TypePromotionTransaction &TPT,
      InstrToOrigTy &PromotedInsts, unsigned &CreatedInstsCost,
      SmallVectorImpl<Instruction *> *Exts,
      SmallVectorImpl<Instruction *> *Truncs, const TargetLowering &TLI) {
    return promoteOperandForOther(Ext, TPT, PromotedInsts, CreatedInstsCost,
                                  Exts, Truncs, TLI, true);
  }

  /// \see promoteOperandForOther.
  static Value *zeroExtendOperandForOther(
      Instruction *Ext, TypePromotionTransaction &TPT,
      InstrToOrigTy &PromotedInsts, unsigned &CreatedInstsCost,
      SmallVectorImpl<Instruction *> *Exts,
      SmallVectorImpl<Instruction *> *Truncs, const TargetLowering &TLI) {
    return promoteOperandForOther(Ext, TPT, PromotedInsts, CreatedInstsCost,
                                  Exts, Truncs, TLI, false);
  }

public:
  /// Type for the utility function that promotes the operand of Ext.
  typedef Value *(*Action)(Instruction *Ext, TypePromotionTransaction &TPT,
                           InstrToOrigTy &PromotedInsts,
                           unsigned &CreatedInstsCost,
                           SmallVectorImpl<Instruction *> *Exts,
                           SmallVectorImpl<Instruction *> *Truncs,
                           const TargetLowering &TLI);
  /// \brief Given a sign/zero extend instruction \p Ext, return the approriate
  /// action to promote the operand of \p Ext instead of using Ext.
  /// \return NULL if no promotable action is possible with the current
  /// sign extension.
  /// \p InsertedInsts keeps track of all the instructions inserted by the
  /// other CodeGenPrepare optimizations. This information is important
  /// because we do not want to promote these instructions as CodeGenPrepare
  /// will reinsert them later. Thus creating an infinite loop: create/remove.
  /// \p PromotedInsts maps the instructions to their type before promotion.
  static Action getAction(Instruction *Ext, const SetOfInstrs &InsertedInsts,
                          const TargetLowering &TLI,
                          const InstrToOrigTy &PromotedInsts);
};

bool TypePromotionHelper::canGetThrough(const Instruction *Inst,
                                        Type *ConsideredExtType,
                                        const InstrToOrigTy &PromotedInsts,
                                        bool IsSExt) {
  // The promotion helper does not know how to deal with vector types yet.
  // To be able to fix that, we would need to fix the places where we
  // statically extend, e.g., constants and such.
  if (Inst->getType()->isVectorTy())
    return false;

  // We can always get through zext.
  if (isa<ZExtInst>(Inst))
    return true;

  // sext(sext) is ok too.
  if (IsSExt && isa<SExtInst>(Inst))
    return true;

  // We can get through binary operator, if it is legal. In other words, the
  // binary operator must have a nuw or nsw flag.
  const BinaryOperator *BinOp = dyn_cast<BinaryOperator>(Inst);
  if (BinOp && isa<OverflowingBinaryOperator>(BinOp) &&
      ((!IsSExt && BinOp->hasNoUnsignedWrap()) ||
       (IsSExt && BinOp->hasNoSignedWrap())))
    return true;

  // Check if we can do the following simplification.
  // ext(trunc(opnd)) --> ext(opnd)
  if (!isa<TruncInst>(Inst))
    return false;

  Value *OpndVal = Inst->getOperand(0);
  // Check if we can use this operand in the extension.
  // If the type is larger than the result type of the extension, we cannot.
  if (!OpndVal->getType()->isIntegerTy() ||
      OpndVal->getType()->getIntegerBitWidth() >
          ConsideredExtType->getIntegerBitWidth())
    return false;

  // If the operand of the truncate is not an instruction, we will not have
  // any information on the dropped bits.
  // (Actually we could for constant but it is not worth the extra logic).
  Instruction *Opnd = dyn_cast<Instruction>(OpndVal);
  if (!Opnd)
    return false;

  // Check if the source of the type is narrow enough.
  // I.e., check that trunc just drops extended bits of the same kind of
  // the extension.
  // #1 get the type of the operand and check the kind of the extended bits.
  const Type *OpndType;
  InstrToOrigTy::const_iterator It = PromotedInsts.find(Opnd);
  if (It != PromotedInsts.end() && It->second.getInt() == IsSExt)
    OpndType = It->second.getPointer();
  else if ((IsSExt && isa<SExtInst>(Opnd)) || (!IsSExt && isa<ZExtInst>(Opnd)))
    OpndType = Opnd->getOperand(0)->getType();
  else
    return false;

  // #2 check that the truncate just drops extended bits.
  return Inst->getType()->getIntegerBitWidth() >=
         OpndType->getIntegerBitWidth();
}

TypePromotionHelper::Action TypePromotionHelper::getAction(
    Instruction *Ext, const SetOfInstrs &InsertedInsts,
    const TargetLowering &TLI, const InstrToOrigTy &PromotedInsts) {
  assert((isa<SExtInst>(Ext) || isa<ZExtInst>(Ext)) &&
         "Unexpected instruction type");
  Instruction *ExtOpnd = dyn_cast<Instruction>(Ext->getOperand(0));
  Type *ExtTy = Ext->getType();
  bool IsSExt = isa<SExtInst>(Ext);
  // If the operand of the extension is not an instruction, we cannot
  // get through.
  // If it, check we can get through.
  if (!ExtOpnd || !canGetThrough(ExtOpnd, ExtTy, PromotedInsts, IsSExt))
    return nullptr;

  // Do not promote if the operand has been added by codegenprepare.
  // Otherwise, it means we are undoing an optimization that is likely to be
  // redone, thus causing potential infinite loop.
  if (isa<TruncInst>(ExtOpnd) && InsertedInsts.count(ExtOpnd))
    return nullptr;

  // SExt or Trunc instructions.
  // Return the related handler.
  if (isa<SExtInst>(ExtOpnd) || isa<TruncInst>(ExtOpnd) ||
      isa<ZExtInst>(ExtOpnd))
    return promoteOperandForTruncAndAnyExt;

  // Regular instruction.
  // Abort early if we will have to insert non-free instructions.
  if (!ExtOpnd->hasOneUse() && !TLI.isTruncateFree(ExtTy, ExtOpnd->getType()))
    return nullptr;
  return IsSExt ? signExtendOperandForOther : zeroExtendOperandForOther;
}

Value *TypePromotionHelper::promoteOperandForTruncAndAnyExt(
    llvm::Instruction *SExt, TypePromotionTransaction &TPT,
    InstrToOrigTy &PromotedInsts, unsigned &CreatedInstsCost,
    SmallVectorImpl<Instruction *> *Exts,
    SmallVectorImpl<Instruction *> *Truncs, const TargetLowering &TLI) {
  // By construction, the operand of SExt is an instruction. Otherwise we cannot
  // get through it and this method should not be called.
  Instruction *SExtOpnd = cast<Instruction>(SExt->getOperand(0));
  Value *ExtVal = SExt;
  bool HasMergedNonFreeExt = false;
  if (isa<ZExtInst>(SExtOpnd)) {
    // Replace s|zext(zext(opnd))
    // => zext(opnd).
    HasMergedNonFreeExt = !TLI.isExtFree(SExtOpnd);
    Value *ZExt =
        TPT.createZExt(SExt, SExtOpnd->getOperand(0), SExt->getType());
    TPT.replaceAllUsesWith(SExt, ZExt);
    TPT.eraseInstruction(SExt);
    ExtVal = ZExt;
  } else {
    // Replace z|sext(trunc(opnd)) or sext(sext(opnd))
    // => z|sext(opnd).
    TPT.setOperand(SExt, 0, SExtOpnd->getOperand(0));
  }
  CreatedInstsCost = 0;

  // Remove dead code.
  if (SExtOpnd->use_empty())
    TPT.eraseInstruction(SExtOpnd);

  // Check if the extension is still needed.
  Instruction *ExtInst = dyn_cast<Instruction>(ExtVal);
  if (!ExtInst || ExtInst->getType() != ExtInst->getOperand(0)->getType()) {
    if (ExtInst) {
      if (Exts)
        Exts->push_back(ExtInst);
      CreatedInstsCost = !TLI.isExtFree(ExtInst) && !HasMergedNonFreeExt;
    }
    return ExtVal;
  }

  // At this point we have: ext ty opnd to ty.
  // Reassign the uses of ExtInst to the opnd and remove ExtInst.
  Value *NextVal = ExtInst->getOperand(0);
  TPT.eraseInstruction(ExtInst, NextVal);
  return NextVal;
}

Value *TypePromotionHelper::promoteOperandForOther(
    Instruction *Ext, TypePromotionTransaction &TPT,
    InstrToOrigTy &PromotedInsts, unsigned &CreatedInstsCost,
    SmallVectorImpl<Instruction *> *Exts,
    SmallVectorImpl<Instruction *> *Truncs, const TargetLowering &TLI,
    bool IsSExt) {
  // By construction, the operand of Ext is an instruction. Otherwise we cannot
  // get through it and this method should not be called.
  Instruction *ExtOpnd = cast<Instruction>(Ext->getOperand(0));
  CreatedInstsCost = 0;
  if (!ExtOpnd->hasOneUse()) {
    // ExtOpnd will be promoted.
    // All its uses, but Ext, will need to use a truncated value of the
    // promoted version.
    // Create the truncate now.
    Value *Trunc = TPT.createTrunc(Ext, ExtOpnd->getType());
    if (Instruction *ITrunc = dyn_cast<Instruction>(Trunc)) {
      ITrunc->removeFromParent();
      // Insert it just after the definition.
      ITrunc->insertAfter(ExtOpnd);
      if (Truncs)
        Truncs->push_back(ITrunc);
    }

    TPT.replaceAllUsesWith(ExtOpnd, Trunc);
    // Restore the operand of Ext (which has been replaced by the previous call
    // to replaceAllUsesWith) to avoid creating a cycle trunc <-> sext.
    TPT.setOperand(Ext, 0, ExtOpnd);
  }

  // Get through the Instruction:
  // 1. Update its type.
  // 2. Replace the uses of Ext by Inst.
  // 3. Extend each operand that needs to be extended.

  // Remember the original type of the instruction before promotion.
  // This is useful to know that the high bits are sign extended bits.
  PromotedInsts.insert(std::pair<Instruction *, TypeIsSExt>(
      ExtOpnd, TypeIsSExt(ExtOpnd->getType(), IsSExt)));
  // Step #1.
  TPT.mutateType(ExtOpnd, Ext->getType());
  // Step #2.
  TPT.replaceAllUsesWith(Ext, ExtOpnd);
  // Step #3.
  Instruction *ExtForOpnd = Ext;

  DEBUG(dbgs() << "Propagate Ext to operands\n");
  for (int OpIdx = 0, EndOpIdx = ExtOpnd->getNumOperands(); OpIdx != EndOpIdx;
       ++OpIdx) {
    DEBUG(dbgs() << "Operand:\n" << *(ExtOpnd->getOperand(OpIdx)) << '\n');
    if (ExtOpnd->getOperand(OpIdx)->getType() == Ext->getType() ||
        !shouldExtOperand(ExtOpnd, OpIdx)) {
      DEBUG(dbgs() << "No need to propagate\n");
      continue;
    }
    // Check if we can statically extend the operand.
    Value *Opnd = ExtOpnd->getOperand(OpIdx);
    if (const ConstantInt *Cst = dyn_cast<ConstantInt>(Opnd)) {
      DEBUG(dbgs() << "Statically extend\n");
      unsigned BitWidth = Ext->getType()->getIntegerBitWidth();
      APInt CstVal = IsSExt ? Cst->getValue().sext(BitWidth)
                            : Cst->getValue().zext(BitWidth);
      TPT.setOperand(ExtOpnd, OpIdx, ConstantInt::get(Ext->getType(), CstVal));
      continue;
    }
    // UndefValue are typed, so we have to statically sign extend them.
    if (isa<UndefValue>(Opnd)) {
      DEBUG(dbgs() << "Statically extend\n");
      TPT.setOperand(ExtOpnd, OpIdx, UndefValue::get(Ext->getType()));
      continue;
    }

    // Otherwise we have to explicity sign extend the operand.
    // Check if Ext was reused to extend an operand.
    if (!ExtForOpnd) {
      // If yes, create a new one.
      DEBUG(dbgs() << "More operands to ext\n");
      Value *ValForExtOpnd = IsSExt ? TPT.createSExt(Ext, Opnd, Ext->getType())
        : TPT.createZExt(Ext, Opnd, Ext->getType());
      if (!isa<Instruction>(ValForExtOpnd)) {
        TPT.setOperand(ExtOpnd, OpIdx, ValForExtOpnd);
        continue;
      }
      ExtForOpnd = cast<Instruction>(ValForExtOpnd);
    }
    if (Exts)
      Exts->push_back(ExtForOpnd);
    TPT.setOperand(ExtForOpnd, 0, Opnd);

    // Move the sign extension before the insertion point.
    TPT.moveBefore(ExtForOpnd, ExtOpnd);
    TPT.setOperand(ExtOpnd, OpIdx, ExtForOpnd);
    CreatedInstsCost += !TLI.isExtFree(ExtForOpnd);
    // If more sext are required, new instructions will have to be created.
    ExtForOpnd = nullptr;
  }
  if (ExtForOpnd == Ext) {
    DEBUG(dbgs() << "Extension is useless now\n");
    TPT.eraseInstruction(Ext);
  }
  return ExtOpnd;
}

/// Check whether or not promoting an instruction to a wider type is profitable.
/// \p NewCost gives the cost of extension instructions created by the
/// promotion.
/// \p OldCost gives the cost of extension instructions before the promotion
/// plus the number of instructions that have been
/// matched in the addressing mode the promotion.
/// \p PromotedOperand is the value that has been promoted.
/// \return True if the promotion is profitable, false otherwise.
bool AddressingModeMatcher::isPromotionProfitable(
    unsigned NewCost, unsigned OldCost, Value *PromotedOperand) const {
  DEBUG(dbgs() << "OldCost: " << OldCost << "\tNewCost: " << NewCost << '\n');
  // The cost of the new extensions is greater than the cost of the
  // old extension plus what we folded.
  // This is not profitable.
  if (NewCost > OldCost)
    return false;
  if (NewCost < OldCost)
    return true;
  // The promotion is neutral but it may help folding the sign extension in
  // loads for instance.
  // Check that we did not create an illegal instruction.
  return isPromotedInstructionLegal(TLI, DL, PromotedOperand);
}

/// Given an instruction or constant expr, see if we can fold the operation
/// into the addressing mode. If so, update the addressing mode and return
/// true, otherwise return false without modifying AddrMode.
/// If \p MovedAway is not NULL, it contains the information of whether or
/// not AddrInst has to be folded into the addressing mode on success.
/// If \p MovedAway == true, \p AddrInst will not be part of the addressing
/// because it has been moved away.
/// Thus AddrInst must not be added in the matched instructions.
/// This state can happen when AddrInst is a sext, since it may be moved away.
/// Therefore, AddrInst may not be valid when MovedAway is true and it must
/// not be referenced anymore.
bool AddressingModeMatcher::matchOperationAddr(User *AddrInst, unsigned Opcode,
                                               unsigned Depth,
                                               bool *MovedAway) {
  // Avoid exponential behavior on extremely deep expression trees.
  if (Depth >= 5) return false;

  // By default, all matched instructions stay in place.
  if (MovedAway)
    *MovedAway = false;

  switch (Opcode) {
  case Instruction::PtrToInt:
    // PtrToInt is always a noop, as we know that the int type is pointer sized.
    return matchAddr(AddrInst->getOperand(0), Depth);
  case Instruction::IntToPtr: {
    auto AS = AddrInst->getType()->getPointerAddressSpace();
    auto PtrTy = MVT::getIntegerVT(DL.getPointerSizeInBits(AS));
    // This inttoptr is a no-op if the integer type is pointer sized.
    if (TLI.getValueType(DL, AddrInst->getOperand(0)->getType()) == PtrTy)
      return matchAddr(AddrInst->getOperand(0), Depth);
    return false;
  }
  case Instruction::BitCast:
    // BitCast is always a noop, and we can handle it as long as it is
    // int->int or pointer->pointer (we don't want int<->fp or something).
    if ((AddrInst->getOperand(0)->getType()->isPointerTy() ||
         AddrInst->getOperand(0)->getType()->isIntegerTy()) &&
        // Don't touch identity bitcasts.  These were probably put here by LSR,
        // and we don't want to mess around with them.  Assume it knows what it
        // is doing.
        AddrInst->getOperand(0)->getType() != AddrInst->getType())
      return matchAddr(AddrInst->getOperand(0), Depth);
    return false;
  case Instruction::AddrSpaceCast: {
    unsigned SrcAS
      = AddrInst->getOperand(0)->getType()->getPointerAddressSpace();
    unsigned DestAS = AddrInst->getType()->getPointerAddressSpace();
    if (TLI.isNoopAddrSpaceCast(SrcAS, DestAS))
      return matchAddr(AddrInst->getOperand(0), Depth);
    return false;
  }
  case Instruction::Add: {
    // Check to see if we can merge in the RHS then the LHS.  If so, we win.
    ExtAddrMode BackupAddrMode = AddrMode;
    unsigned OldSize = AddrModeInsts.size();
    // Start a transaction at this point.
    // The LHS may match but not the RHS.
    // Therefore, we need a higher level restoration point to undo partially
    // matched operation.
    TypePromotionTransaction::ConstRestorationPt LastKnownGood =
        TPT.getRestorationPoint();

    if (matchAddr(AddrInst->getOperand(1), Depth+1) &&
        matchAddr(AddrInst->getOperand(0), Depth+1))
      return true;

    // Restore the old addr mode info.
    AddrMode = BackupAddrMode;
    AddrModeInsts.resize(OldSize);
    TPT.rollback(LastKnownGood);

    // Otherwise this was over-aggressive.  Try merging in the LHS then the RHS.
    if (matchAddr(AddrInst->getOperand(0), Depth+1) &&
        matchAddr(AddrInst->getOperand(1), Depth+1))
      return true;

    // Otherwise we definitely can't merge the ADD in.
    AddrMode = BackupAddrMode;
    AddrModeInsts.resize(OldSize);
    TPT.rollback(LastKnownGood);
    break;
  }
  //case Instruction::Or:
  // TODO: We can handle "Or Val, Imm" iff this OR is equivalent to an ADD.
  //break;
  case Instruction::Mul:
  case Instruction::Shl: {
    // Can only handle X*C and X << C.
    ConstantInt *RHS = dyn_cast<ConstantInt>(AddrInst->getOperand(1));
    if (!RHS)
      return false;
    int64_t Scale = RHS->getSExtValue();
    if (Opcode == Instruction::Shl)
      Scale = 1LL << Scale;

    return matchScaledValue(AddrInst->getOperand(0), Scale, Depth);
  }
  case Instruction::GetElementPtr: {
    // Scan the GEP.  We check it if it contains constant offsets and at most
    // one variable offset.
    int VariableOperand = -1;
    unsigned VariableScale = 0;

    int64_t ConstantOffset = 0;
    gep_type_iterator GTI = gep_type_begin(AddrInst);
    for (unsigned i = 1, e = AddrInst->getNumOperands(); i != e; ++i, ++GTI) {
      if (StructType *STy = GTI.getStructTypeOrNull()) {
        const StructLayout *SL = DL.getStructLayout(STy);
        unsigned Idx =
          cast<ConstantInt>(AddrInst->getOperand(i))->getZExtValue();
        ConstantOffset += SL->getElementOffset(Idx);
      } else {
        uint64_t TypeSize = DL.getTypeAllocSize(GTI.getIndexedType());
        if (ConstantInt *CI = dyn_cast<ConstantInt>(AddrInst->getOperand(i))) {
          ConstantOffset += CI->getSExtValue()*TypeSize;
        } else if (TypeSize) {  // Scales of zero don't do anything.
          // We only allow one variable index at the moment.
          if (VariableOperand != -1)
            return false;

          // Remember the variable index.
          VariableOperand = i;
          VariableScale = TypeSize;
        }
      }
    }

    // A common case is for the GEP to only do a constant offset.  In this case,
    // just add it to the disp field and check validity.
    if (VariableOperand == -1) {
      AddrMode.BaseOffs += ConstantOffset;
      if (ConstantOffset == 0 ||
          TLI.isLegalAddressingMode(DL, AddrMode, AccessTy, AddrSpace)) {
        // Check to see if we can fold the base pointer in too.
        if (matchAddr(AddrInst->getOperand(0), Depth+1))
          return true;
      }
      AddrMode.BaseOffs -= ConstantOffset;
      return false;
    }

    // Save the valid addressing mode in case we can't match.
    ExtAddrMode BackupAddrMode = AddrMode;
    unsigned OldSize = AddrModeInsts.size();

    // See if the scale and offset amount is valid for this target.
    AddrMode.BaseOffs += ConstantOffset;

    // Match the base operand of the GEP.
    if (!matchAddr(AddrInst->getOperand(0), Depth+1)) {
      // If it couldn't be matched, just stuff the value in a register.
      if (AddrMode.HasBaseReg) {
        AddrMode = BackupAddrMode;
        AddrModeInsts.resize(OldSize);
        return false;
      }
      AddrMode.HasBaseReg = true;
      AddrMode.BaseReg = AddrInst->getOperand(0);
    }

    // Match the remaining variable portion of the GEP.
    if (!matchScaledValue(AddrInst->getOperand(VariableOperand), VariableScale,
                          Depth)) {
      // If it couldn't be matched, try stuffing the base into a register
      // instead of matching it, and retrying the match of the scale.
      AddrMode = BackupAddrMode;
      AddrModeInsts.resize(OldSize);
      if (AddrMode.HasBaseReg)
        return false;
      AddrMode.HasBaseReg = true;
      AddrMode.BaseReg = AddrInst->getOperand(0);
      AddrMode.BaseOffs += ConstantOffset;
      if (!matchScaledValue(AddrInst->getOperand(VariableOperand),
                            VariableScale, Depth)) {
        // If even that didn't work, bail.
        AddrMode = BackupAddrMode;
        AddrModeInsts.resize(OldSize);
        return false;
      }
    }

    return true;
  }
  case Instruction::SExt:
  case Instruction::ZExt: {
    Instruction *Ext = dyn_cast<Instruction>(AddrInst);
    if (!Ext)
      return false;

    // Try to move this ext out of the way of the addressing mode.
    // Ask for a method for doing so.
    TypePromotionHelper::Action TPH =
        TypePromotionHelper::getAction(Ext, InsertedInsts, TLI, PromotedInsts);
    if (!TPH)
      return false;

    TypePromotionTransaction::ConstRestorationPt LastKnownGood =
        TPT.getRestorationPoint();
    unsigned CreatedInstsCost = 0;
    unsigned ExtCost = !TLI.isExtFree(Ext);
    Value *PromotedOperand =
        TPH(Ext, TPT, PromotedInsts, CreatedInstsCost, nullptr, nullptr, TLI);
    // SExt has been moved away.
    // Thus either it will be rematched later in the recursive calls or it is
    // gone. Anyway, we must not fold it into the addressing mode at this point.
    // E.g.,
    // op = add opnd, 1
    // idx = ext op
    // addr = gep base, idx
    // is now:
    // promotedOpnd = ext opnd            <- no match here
    // op = promoted_add promotedOpnd, 1  <- match (later in recursive calls)
    // addr = gep base, op                <- match
    if (MovedAway)
      *MovedAway = true;

    assert(PromotedOperand &&
           "TypePromotionHelper should have filtered out those cases");

    ExtAddrMode BackupAddrMode = AddrMode;
    unsigned OldSize = AddrModeInsts.size();

    if (!matchAddr(PromotedOperand, Depth) ||
        // The total of the new cost is equal to the cost of the created
        // instructions.
        // The total of the old cost is equal to the cost of the extension plus
        // what we have saved in the addressing mode.
        !isPromotionProfitable(CreatedInstsCost,
                               ExtCost + (AddrModeInsts.size() - OldSize),
                               PromotedOperand)) {
      AddrMode = BackupAddrMode;
      AddrModeInsts.resize(OldSize);
      DEBUG(dbgs() << "Sign extension does not pay off: rollback\n");
      TPT.rollback(LastKnownGood);
      return false;
    }
    return true;
  }
  }
  return false;
}

/// If we can, try to add the value of 'Addr' into the current addressing mode.
/// If Addr can't be added to AddrMode this returns false and leaves AddrMode
/// unmodified. This assumes that Addr is either a pointer type or intptr_t
/// for the target.
///
bool AddressingModeMatcher::matchAddr(Value *Addr, unsigned Depth) {
  // Start a transaction at this point that we will rollback if the matching
  // fails.
  TypePromotionTransaction::ConstRestorationPt LastKnownGood =
      TPT.getRestorationPoint();
  if (ConstantInt *CI = dyn_cast<ConstantInt>(Addr)) {
    // Fold in immediates if legal for the target.
    AddrMode.BaseOffs += CI->getSExtValue();
    if (TLI.isLegalAddressingMode(DL, AddrMode, AccessTy, AddrSpace))
      return true;
    AddrMode.BaseOffs -= CI->getSExtValue();
  } else if (GlobalValue *GV = dyn_cast<GlobalValue>(Addr)) {
    // If this is a global variable, try to fold it into the addressing mode.
    if (!AddrMode.BaseGV) {
      AddrMode.BaseGV = GV;
      if (TLI.isLegalAddressingMode(DL, AddrMode, AccessTy, AddrSpace))
        return true;
      AddrMode.BaseGV = nullptr;
    }
  } else if (Instruction *I = dyn_cast<Instruction>(Addr)) {
    ExtAddrMode BackupAddrMode = AddrMode;
    unsigned OldSize = AddrModeInsts.size();

    // Check to see if it is possible to fold this operation.
    bool MovedAway = false;
    if (matchOperationAddr(I, I->getOpcode(), Depth, &MovedAway)) {
      // This instruction may have been moved away. If so, there is nothing
      // to check here.
      if (MovedAway)
        return true;
      // Okay, it's possible to fold this.  Check to see if it is actually
      // *profitable* to do so.  We use a simple cost model to avoid increasing
      // register pressure too much.
      if (I->hasOneUse() ||
          isProfitableToFoldIntoAddressingMode(I, BackupAddrMode, AddrMode)) {
        AddrModeInsts.push_back(I);
        return true;
      }

      // It isn't profitable to do this, roll back.
      //cerr << "NOT FOLDING: " << *I;
      AddrMode = BackupAddrMode;
      AddrModeInsts.resize(OldSize);
      TPT.rollback(LastKnownGood);
    }
  } else if (ConstantExpr *CE = dyn_cast<ConstantExpr>(Addr)) {
    if (matchOperationAddr(CE, CE->getOpcode(), Depth))
      return true;
    TPT.rollback(LastKnownGood);
  } else if (isa<ConstantPointerNull>(Addr)) {
    // Null pointer gets folded without affecting the addressing mode.
    return true;
  }

  // Worse case, the target should support [reg] addressing modes. :)
  if (!AddrMode.HasBaseReg) {
    AddrMode.HasBaseReg = true;
    AddrMode.BaseReg = Addr;
    // Still check for legality in case the target supports [imm] but not [i+r].
    if (TLI.isLegalAddressingMode(DL, AddrMode, AccessTy, AddrSpace))
      return true;
    AddrMode.HasBaseReg = false;
    AddrMode.BaseReg = nullptr;
  }

  // If the base register is already taken, see if we can do [r+r].
  if (AddrMode.Scale == 0) {
    AddrMode.Scale = 1;
    AddrMode.ScaledReg = Addr;
    if (TLI.isLegalAddressingMode(DL, AddrMode, AccessTy, AddrSpace))
      return true;
    AddrMode.Scale = 0;
    AddrMode.ScaledReg = nullptr;
  }
  // Couldn't match.
  TPT.rollback(LastKnownGood);
  return false;
}

/// Check to see if all uses of OpVal by the specified inline asm call are due
/// to memory operands. If so, return true, otherwise return false.
static bool IsOperandAMemoryOperand(CallInst *CI, InlineAsm *IA, Value *OpVal,
                                    const TargetLowering &TLI,
                                    const TargetRegisterInfo &TRI) {
  const Function *F = CI->getFunction();
  TargetLowering::AsmOperandInfoVector TargetConstraints =
      TLI.ParseConstraints(F->getParent()->getDataLayout(), &TRI,
                            ImmutableCallSite(CI));

  for (unsigned i = 0, e = TargetConstraints.size(); i != e; ++i) {
    TargetLowering::AsmOperandInfo &OpInfo = TargetConstraints[i];

    // Compute the constraint code and ConstraintType to use.
    TLI.ComputeConstraintToUse(OpInfo, SDValue());

    // If this asm operand is our Value*, and if it isn't an indirect memory
    // operand, we can't fold it!
    if (OpInfo.CallOperandVal == OpVal &&
        (OpInfo.ConstraintType != TargetLowering::C_Memory ||
         !OpInfo.isIndirect))
      return false;
  }

  return true;
}

// Max number of memory uses to look at before aborting the search to conserve
// compile time.
static constexpr int MaxMemoryUsesToScan = 20;

/// Recursively walk all the uses of I until we find a memory use.
/// If we find an obviously non-foldable instruction, return true.
/// Add the ultimately found memory instructions to MemoryUses.
static bool FindAllMemoryUses(
    Instruction *I,
    SmallVectorImpl<std::pair<Instruction *, unsigned>> &MemoryUses,
    SmallPtrSetImpl<Instruction *> &ConsideredInsts, const TargetLowering &TLI,
    const TargetRegisterInfo &TRI, int SeenInsts = 0) {
  // If we already considered this instruction, we're done.
  if (!ConsideredInsts.insert(I).second)
    return false;

  // If this is an obviously unfoldable instruction, bail out.
  if (!MightBeFoldableInst(I))
    return true;

  const bool OptSize = I->getFunction()->optForSize();

  // Loop over all the uses, recursively processing them.
  for (Use &U : I->uses()) {
    // Conservatively return true if we're seeing a large number or a deep chain
    // of users. This avoids excessive compilation times in pathological cases.
    if (SeenInsts++ >= MaxMemoryUsesToScan)
      return true;

    Instruction *UserI = cast<Instruction>(U.getUser());
    if (LoadInst *LI = dyn_cast<LoadInst>(UserI)) {
      MemoryUses.push_back(std::make_pair(LI, U.getOperandNo()));
      continue;
    }

    if (StoreInst *SI = dyn_cast<StoreInst>(UserI)) {
      unsigned opNo = U.getOperandNo();
      if (opNo != StoreInst::getPointerOperandIndex())
        return true; // Storing addr, not into addr.
      MemoryUses.push_back(std::make_pair(SI, opNo));
      continue;
    }

    if (AtomicRMWInst *RMW = dyn_cast<AtomicRMWInst>(UserI)) {
      unsigned opNo = U.getOperandNo();
      if (opNo != AtomicRMWInst::getPointerOperandIndex())
        return true; // Storing addr, not into addr.
      MemoryUses.push_back(std::make_pair(RMW, opNo));
      continue;
    }

    if (AtomicCmpXchgInst *CmpX = dyn_cast<AtomicCmpXchgInst>(UserI)) {
      unsigned opNo = U.getOperandNo();
      if (opNo != AtomicCmpXchgInst::getPointerOperandIndex())
        return true; // Storing addr, not into addr.
      MemoryUses.push_back(std::make_pair(CmpX, opNo));
      continue;
    }

    if (CallInst *CI = dyn_cast<CallInst>(UserI)) {
      // If this is a cold call, we can sink the addressing calculation into
      // the cold path.  See optimizeCallInst
      if (!OptSize && CI->hasFnAttr(Attribute::Cold))
        continue;

      InlineAsm *IA = dyn_cast<InlineAsm>(CI->getCalledValue());
      if (!IA) return true;

      // If this is a memory operand, we're cool, otherwise bail out.
      if (!IsOperandAMemoryOperand(CI, IA, I, TLI, TRI))
        return true;
      continue;
    }

    if (FindAllMemoryUses(UserI, MemoryUses, ConsideredInsts, TLI, TRI,
                          SeenInsts))
      return true;
  }

  return false;
}

/// Return true if Val is already known to be live at the use site that we're
/// folding it into. If so, there is no cost to include it in the addressing
/// mode. KnownLive1 and KnownLive2 are two values that we know are live at the
/// instruction already.
bool AddressingModeMatcher::valueAlreadyLiveAtInst(Value *Val,Value *KnownLive1,
                                                   Value *KnownLive2) {
  // If Val is either of the known-live values, we know it is live!
  if (Val == nullptr || Val == KnownLive1 || Val == KnownLive2)
    return true;

  // All values other than instructions and arguments (e.g. constants) are live.
  if (!isa<Instruction>(Val) && !isa<Argument>(Val)) return true;

  // If Val is a constant sized alloca in the entry block, it is live, this is
  // true because it is just a reference to the stack/frame pointer, which is
  // live for the whole function.
  if (AllocaInst *AI = dyn_cast<AllocaInst>(Val))
    if (AI->isStaticAlloca())
      return true;

  // Check to see if this value is already used in the memory instruction's
  // block.  If so, it's already live into the block at the very least, so we
  // can reasonably fold it.
  return Val->isUsedInBasicBlock(MemoryInst->getParent());
}

/// It is possible for the addressing mode of the machine to fold the specified
/// instruction into a load or store that ultimately uses it.
/// However, the specified instruction has multiple uses.
/// Given this, it may actually increase register pressure to fold it
/// into the load. For example, consider this code:
///
///     X = ...
///     Y = X+1
///     use(Y)   -> nonload/store
///     Z = Y+1
///     load Z
///
/// In this case, Y has multiple uses, and can be folded into the load of Z
/// (yielding load [X+2]).  However, doing this will cause both "X" and "X+1" to
/// be live at the use(Y) line.  If we don't fold Y into load Z, we use one
/// fewer register.  Since Y can't be folded into "use(Y)" we don't increase the
/// number of computations either.
///
/// Note that this (like most of CodeGenPrepare) is just a rough heuristic.  If
/// X was live across 'load Z' for other reasons, we actually *would* want to
/// fold the addressing mode in the Z case.  This would make Y die earlier.
bool AddressingModeMatcher::
isProfitableToFoldIntoAddressingMode(Instruction *I, ExtAddrMode &AMBefore,
                                     ExtAddrMode &AMAfter) {
  if (IgnoreProfitability) return true;

  // AMBefore is the addressing mode before this instruction was folded into it,
  // and AMAfter is the addressing mode after the instruction was folded.  Get
  // the set of registers referenced by AMAfter and subtract out those
  // referenced by AMBefore: this is the set of values which folding in this
  // address extends the lifetime of.
  //
  // Note that there are only two potential values being referenced here,
  // BaseReg and ScaleReg (global addresses are always available, as are any
  // folded immediates).
  Value *BaseReg = AMAfter.BaseReg, *ScaledReg = AMAfter.ScaledReg;

  // If the BaseReg or ScaledReg was referenced by the previous addrmode, their
  // lifetime wasn't extended by adding this instruction.
  if (valueAlreadyLiveAtInst(BaseReg, AMBefore.BaseReg, AMBefore.ScaledReg))
    BaseReg = nullptr;
  if (valueAlreadyLiveAtInst(ScaledReg, AMBefore.BaseReg, AMBefore.ScaledReg))
    ScaledReg = nullptr;

  // If folding this instruction (and it's subexprs) didn't extend any live
  // ranges, we're ok with it.
  if (!BaseReg && !ScaledReg)
    return true;

  // If all uses of this instruction can have the address mode sunk into them,
  // we can remove the addressing mode and effectively trade one live register
  // for another (at worst.)  In this context, folding an addressing mode into
  // the use is just a particularly nice way of sinking it.
  SmallVector<std::pair<Instruction*,unsigned>, 16> MemoryUses;
  SmallPtrSet<Instruction*, 16> ConsideredInsts;
  if (FindAllMemoryUses(I, MemoryUses, ConsideredInsts, TLI, TRI))
    return false;  // Has a non-memory, non-foldable use!

  // Now that we know that all uses of this instruction are part of a chain of
  // computation involving only operations that could theoretically be folded
  // into a memory use, loop over each of these memory operation uses and see
  // if they could  *actually* fold the instruction.  The assumption is that
  // addressing modes are cheap and that duplicating the computation involved
  // many times is worthwhile, even on a fastpath. For sinking candidates
  // (i.e. cold call sites), this serves as a way to prevent excessive code
  // growth since most architectures have some reasonable small and fast way to
  // compute an effective address.  (i.e LEA on x86)
  SmallVector<Instruction*, 32> MatchedAddrModeInsts;
  for (unsigned i = 0, e = MemoryUses.size(); i != e; ++i) {
    Instruction *User = MemoryUses[i].first;
    unsigned OpNo = MemoryUses[i].second;

    // Get the access type of this use.  If the use isn't a pointer, we don't
    // know what it accesses.
    Value *Address = User->getOperand(OpNo);
    PointerType *AddrTy = dyn_cast<PointerType>(Address->getType());
    if (!AddrTy)
      return false;
    Type *AddressAccessTy = AddrTy->getElementType();
    unsigned AS = AddrTy->getAddressSpace();

    // Do a match against the root of this address, ignoring profitability. This
    // will tell us if the addressing mode for the memory operation will
    // *actually* cover the shared instruction.
    ExtAddrMode Result;
    TypePromotionTransaction::ConstRestorationPt LastKnownGood =
        TPT.getRestorationPoint();
    AddressingModeMatcher Matcher(MatchedAddrModeInsts, TLI, TRI,
                                  AddressAccessTy, AS,
                                  MemoryInst, Result, InsertedInsts,
                                  PromotedInsts, TPT);
    Matcher.IgnoreProfitability = true;
    bool Success = Matcher.matchAddr(Address, 0);
    (void)Success; assert(Success && "Couldn't select *anything*?");

    // The match was to check the profitability, the changes made are not
    // part of the original matcher. Therefore, they should be dropped
    // otherwise the original matcher will not present the right state.
    TPT.rollback(LastKnownGood);

    // If the match didn't cover I, then it won't be shared by it.
    if (!is_contained(MatchedAddrModeInsts, I))
      return false;

    MatchedAddrModeInsts.clear();
  }

  return true;
}

} // end anonymous namespace

/// Return true if the specified values are defined in a
/// different basic block than BB.
static bool IsNonLocalValue(Value *V, BasicBlock *BB) {
  if (Instruction *I = dyn_cast<Instruction>(V))
    return I->getParent() != BB;
  return false;
}

/// Sink addressing mode computation immediate before MemoryInst if doing so
/// can be done without increasing register pressure.  The need for the
/// register pressure constraint means this can end up being an all or nothing
/// decision for all uses of the same addressing computation.
///
/// Load and Store Instructions often have addressing modes that can do
/// significant amounts of computation. As such, instruction selection will try
/// to get the load or store to do as much computation as possible for the
/// program. The problem is that isel can only see within a single block. As
/// such, we sink as much legal addressing mode work into the block as possible.
///
/// This method is used to optimize both load/store and inline asms with memory
/// operands.  It's also used to sink addressing computations feeding into cold
/// call sites into their (cold) basic block.
///
/// The motivation for handling sinking into cold blocks is that doing so can
/// both enable other address mode sinking (by satisfying the register pressure
/// constraint above), and reduce register pressure globally (by removing the
/// addressing mode computation from the fast path entirely.).
bool CodeGenPrepare::optimizeMemoryInst(Instruction *MemoryInst, Value *Addr,
                                        Type *AccessTy, unsigned AddrSpace) {
  Value *Repl = Addr;

  // Try to collapse single-value PHI nodes.  This is necessary to undo
  // unprofitable PRE transformations.
  SmallVector<Value*, 8> worklist;
  SmallPtrSet<Value*, 16> Visited;
  worklist.push_back(Addr);

  // Use a worklist to iteratively look through PHI nodes, and ensure that
  // the addressing mode obtained from the non-PHI roots of the graph
  // are equivalent.
  bool AddrModeFound = false;
  bool PhiSeen = false;
  SmallVector<Instruction*, 16> AddrModeInsts;
  ExtAddrMode AddrMode;
  TypePromotionTransaction TPT(RemovedInsts);
  TypePromotionTransaction::ConstRestorationPt LastKnownGood =
      TPT.getRestorationPoint();
  while (!worklist.empty()) {
    Value *V = worklist.back();
    worklist.pop_back();

    // We allow traversing cyclic Phi nodes.
    // In case of success after this loop we ensure that traversing through
    // Phi nodes ends up with all cases to compute address of the form
    //    BaseGV + Base + Scale * Index + Offset
    // where Scale and Offset are constans and BaseGV, Base and Index
    // are exactly the same Values in all cases.
    // It means that BaseGV, Scale and Offset dominate our memory instruction
    // and have the same value as they had in address computation represented
    // as Phi. So we can safely sink address computation to memory instruction.
    if (!Visited.insert(V).second)
      continue;

    // For a PHI node, push all of its incoming values.
    if (PHINode *P = dyn_cast<PHINode>(V)) {
      for (Value *IncValue : P->incoming_values())
        worklist.push_back(IncValue);
      PhiSeen = true;
      continue;
    }

    // For non-PHIs, determine the addressing mode being computed.  Note that
    // the result may differ depending on what other uses our candidate
    // addressing instructions might have.
    AddrModeInsts.clear();
    ExtAddrMode NewAddrMode = AddressingModeMatcher::Match(
        V, AccessTy, AddrSpace, MemoryInst, AddrModeInsts, *TLI, *TRI,
        InsertedInsts, PromotedInsts, TPT);

    if (!AddrModeFound) {
      AddrModeFound = true;
      AddrMode = NewAddrMode;
      continue;
    }
    if (NewAddrMode == AddrMode)
      continue;

    AddrModeFound = false;
    break;
  }

  // If the addressing mode couldn't be determined, or if multiple different
  // ones were determined, bail out now.
  if (!AddrModeFound) {
    TPT.rollback(LastKnownGood);
    return false;
  }
  TPT.commit();

  // If all the instructions matched are already in this BB, don't do anything.
  // If we saw Phi node then it is not local definitely.
  if (!PhiSeen && none_of(AddrModeInsts, [&](Value *V) {
        return IsNonLocalValue(V, MemoryInst->getParent());
                  })) {
    DEBUG(dbgs() << "CGP: Found      local addrmode: " << AddrMode << "\n");
    return false;
  }

  // Insert this computation right after this user.  Since our caller is
  // scanning from the top of the BB to the bottom, reuse of the expr are
  // guaranteed to happen later.
  IRBuilder<> Builder(MemoryInst);

  // Now that we determined the addressing expression we want to use and know
  // that we have to sink it into this block.  Check to see if we have already
  // done this for some other load/store instr in this block.  If so, reuse the
  // computation.
  Value *&SunkAddr = SunkAddrs[Addr];
  if (SunkAddr) {
    DEBUG(dbgs() << "CGP: Reusing nonlocal addrmode: " << AddrMode << " for "
                 << *MemoryInst << "\n");
    if (SunkAddr->getType() != Addr->getType())
      SunkAddr = Builder.CreatePointerCast(SunkAddr, Addr->getType());
  } else if (AddrSinkUsingGEPs ||
             (!AddrSinkUsingGEPs.getNumOccurrences() && TM &&
              SubtargetInfo->useAA())) {
    // By default, we use the GEP-based method when AA is used later. This
    // prevents new inttoptr/ptrtoint pairs from degrading AA capabilities.
    DEBUG(dbgs() << "CGP: SINKING nonlocal addrmode: " << AddrMode << " for "
                 << *MemoryInst << "\n");
    Type *IntPtrTy = DL->getIntPtrType(Addr->getType());
    Value *ResultPtr = nullptr, *ResultIndex = nullptr;

    // First, find the pointer.
    if (AddrMode.BaseReg && AddrMode.BaseReg->getType()->isPointerTy()) {
      ResultPtr = AddrMode.BaseReg;
      AddrMode.BaseReg = nullptr;
    }

    if (AddrMode.Scale && AddrMode.ScaledReg->getType()->isPointerTy()) {
      // We can't add more than one pointer together, nor can we scale a
      // pointer (both of which seem meaningless).
      if (ResultPtr || AddrMode.Scale != 1)
        return false;

      ResultPtr = AddrMode.ScaledReg;
      AddrMode.Scale = 0;
    }

    // It is only safe to sign extend the BaseReg if we know that the math
    // required to create it did not overflow before we extend it. Since
    // the original IR value was tossed in favor of a constant back when
    // the AddrMode was created we need to bail out gracefully if widths
    // do not match instead of extending it.
    //
    // (See below for code to add the scale.)
    if (AddrMode.Scale) {
      Type *ScaledRegTy = AddrMode.ScaledReg->getType();
      if (cast<IntegerType>(IntPtrTy)->getBitWidth() >
          cast<IntegerType>(ScaledRegTy)->getBitWidth())
        return false;
    }

    if (AddrMode.BaseGV) {
      if (ResultPtr)
        return false;

      ResultPtr = AddrMode.BaseGV;
    }

    // If the real base value actually came from an inttoptr, then the matcher
    // will look through it and provide only the integer value. In that case,
    // use it here.
    if (!DL->isNonIntegralPointerType(Addr->getType())) {
      if (!ResultPtr && AddrMode.BaseReg) {
        ResultPtr = Builder.CreateIntToPtr(AddrMode.BaseReg, Addr->getType(),
                                           "sunkaddr");
        AddrMode.BaseReg = nullptr;
      } else if (!ResultPtr && AddrMode.Scale == 1) {
        ResultPtr = Builder.CreateIntToPtr(AddrMode.ScaledReg, Addr->getType(),
                                           "sunkaddr");
        AddrMode.Scale = 0;
      }
    }

    if (!ResultPtr &&
        !AddrMode.BaseReg && !AddrMode.Scale && !AddrMode.BaseOffs) {
      SunkAddr = Constant::getNullValue(Addr->getType());
    } else if (!ResultPtr) {
      return false;
    } else {
      Type *I8PtrTy =
          Builder.getInt8PtrTy(Addr->getType()->getPointerAddressSpace());
      Type *I8Ty = Builder.getInt8Ty();

      // Start with the base register. Do this first so that subsequent address
      // matching finds it last, which will prevent it from trying to match it
      // as the scaled value in case it happens to be a mul. That would be
      // problematic if we've sunk a different mul for the scale, because then
      // we'd end up sinking both muls.
      if (AddrMode.BaseReg) {
        Value *V = AddrMode.BaseReg;
        if (V->getType() != IntPtrTy)
          V = Builder.CreateIntCast(V, IntPtrTy, /*isSigned=*/true, "sunkaddr");

        ResultIndex = V;
      }

      // Add the scale value.
      if (AddrMode.Scale) {
        Value *V = AddrMode.ScaledReg;
        if (V->getType() == IntPtrTy) {
          // done.
        } else {
          assert(cast<IntegerType>(IntPtrTy)->getBitWidth() <
                 cast<IntegerType>(V->getType())->getBitWidth() &&
                 "We can't transform if ScaledReg is too narrow");
          V = Builder.CreateTrunc(V, IntPtrTy, "sunkaddr");
        }

        if (AddrMode.Scale != 1)
          V = Builder.CreateMul(V, ConstantInt::get(IntPtrTy, AddrMode.Scale),
                                "sunkaddr");
        if (ResultIndex)
          ResultIndex = Builder.CreateAdd(ResultIndex, V, "sunkaddr");
        else
          ResultIndex = V;
      }

      // Add in the Base Offset if present.
      if (AddrMode.BaseOffs) {
        Value *V = ConstantInt::get(IntPtrTy, AddrMode.BaseOffs);
        if (ResultIndex) {
          // We need to add this separately from the scale above to help with
          // SDAG consecutive load/store merging.
          if (ResultPtr->getType() != I8PtrTy)
            ResultPtr = Builder.CreatePointerCast(ResultPtr, I8PtrTy);
          ResultPtr = Builder.CreateGEP(I8Ty, ResultPtr, ResultIndex, "sunkaddr");
        }

        ResultIndex = V;
      }

      if (!ResultIndex) {
        SunkAddr = ResultPtr;
      } else {
        if (ResultPtr->getType() != I8PtrTy)
          ResultPtr = Builder.CreatePointerCast(ResultPtr, I8PtrTy);
        SunkAddr = Builder.CreateGEP(I8Ty, ResultPtr, ResultIndex, "sunkaddr");
      }

      if (SunkAddr->getType() != Addr->getType())
        SunkAddr = Builder.CreatePointerCast(SunkAddr, Addr->getType());
    }
  } else {
    // We'd require a ptrtoint/inttoptr down the line, which we can't do for
    // non-integral pointers, so in that case bail out now.
    Type *BaseTy = AddrMode.BaseReg ? AddrMode.BaseReg->getType() : nullptr;
    Type *ScaleTy = AddrMode.Scale ? AddrMode.ScaledReg->getType() : nullptr;
    PointerType *BasePtrTy = dyn_cast_or_null<PointerType>(BaseTy);
    PointerType *ScalePtrTy = dyn_cast_or_null<PointerType>(ScaleTy);
    if (DL->isNonIntegralPointerType(Addr->getType()) ||
        (BasePtrTy && DL->isNonIntegralPointerType(BasePtrTy)) ||
        (ScalePtrTy && DL->isNonIntegralPointerType(ScalePtrTy)) ||
        (AddrMode.BaseGV &&
         DL->isNonIntegralPointerType(AddrMode.BaseGV->getType())))
      return false;

    DEBUG(dbgs() << "CGP: SINKING nonlocal addrmode: " << AddrMode << " for "
                 << *MemoryInst << "\n");
    Type *IntPtrTy = DL->getIntPtrType(Addr->getType());
    Value *Result = nullptr;

    // Start with the base register. Do this first so that subsequent address
    // matching finds it last, which will prevent it from trying to match it
    // as the scaled value in case it happens to be a mul. That would be
    // problematic if we've sunk a different mul for the scale, because then
    // we'd end up sinking both muls.
    if (AddrMode.BaseReg) {
      Value *V = AddrMode.BaseReg;
      if (V->getType()->isPointerTy())
        V = Builder.CreatePtrToInt(V, IntPtrTy, "sunkaddr");
      if (V->getType() != IntPtrTy)
        V = Builder.CreateIntCast(V, IntPtrTy, /*isSigned=*/true, "sunkaddr");
      Result = V;
    }

    // Add the scale value.
    if (AddrMode.Scale) {
      Value *V = AddrMode.ScaledReg;
      if (V->getType() == IntPtrTy) {
        // done.
      } else if (V->getType()->isPointerTy()) {
        V = Builder.CreatePtrToInt(V, IntPtrTy, "sunkaddr");
      } else if (cast<IntegerType>(IntPtrTy)->getBitWidth() <
                 cast<IntegerType>(V->getType())->getBitWidth()) {
        V = Builder.CreateTrunc(V, IntPtrTy, "sunkaddr");
      } else {
        // It is only safe to sign extend the BaseReg if we know that the math
        // required to create it did not overflow before we extend it. Since
        // the original IR value was tossed in favor of a constant back when
        // the AddrMode was created we need to bail out gracefully if widths
        // do not match instead of extending it.
        Instruction *I = dyn_cast_or_null<Instruction>(Result);
        if (I && (Result != AddrMode.BaseReg))
          I->eraseFromParent();
        return false;
      }
      if (AddrMode.Scale != 1)
        V = Builder.CreateMul(V, ConstantInt::get(IntPtrTy, AddrMode.Scale),
                              "sunkaddr");
      if (Result)
        Result = Builder.CreateAdd(Result, V, "sunkaddr");
      else
        Result = V;
    }

    // Add in the BaseGV if present.
    if (AddrMode.BaseGV) {
      Value *V = Builder.CreatePtrToInt(AddrMode.BaseGV, IntPtrTy, "sunkaddr");
      if (Result)
        Result = Builder.CreateAdd(Result, V, "sunkaddr");
      else
        Result = V;
    }

    // Add in the Base Offset if present.
    if (AddrMode.BaseOffs) {
      Value *V = ConstantInt::get(IntPtrTy, AddrMode.BaseOffs);
      if (Result)
        Result = Builder.CreateAdd(Result, V, "sunkaddr");
      else
        Result = V;
    }

    if (!Result)
      SunkAddr = Constant::getNullValue(Addr->getType());
    else
      SunkAddr = Builder.CreateIntToPtr(Result, Addr->getType(), "sunkaddr");
  }

  MemoryInst->replaceUsesOfWith(Repl, SunkAddr);

  // If we have no uses, recursively delete the value and all dead instructions
  // using it.
  if (Repl->use_empty()) {
    // This can cause recursive deletion, which can invalidate our iterator.
    // Use a WeakTrackingVH to hold onto it in case this happens.
    Value *CurValue = &*CurInstIterator;
    WeakTrackingVH IterHandle(CurValue);
    BasicBlock *BB = CurInstIterator->getParent();

    RecursivelyDeleteTriviallyDeadInstructions(Repl, TLInfo);

    if (IterHandle != CurValue) {
      // If the iterator instruction was recursively deleted, start over at the
      // start of the block.
      CurInstIterator = BB->begin();
      SunkAddrs.clear();
    }
  }
  ++NumMemoryInsts;
  return true;
}

/// If there are any memory operands, use OptimizeMemoryInst to sink their
/// address computing into the block when possible / profitable.
bool CodeGenPrepare::optimizeInlineAsmInst(CallInst *CS) {
  bool MadeChange = false;

  const TargetRegisterInfo *TRI =
      TM->getSubtargetImpl(*CS->getFunction())->getRegisterInfo();
  TargetLowering::AsmOperandInfoVector TargetConstraints =
      TLI->ParseConstraints(*DL, TRI, CS);
  unsigned ArgNo = 0;
  for (unsigned i = 0, e = TargetConstraints.size(); i != e; ++i) {
    TargetLowering::AsmOperandInfo &OpInfo = TargetConstraints[i];

    // Compute the constraint code and ConstraintType to use.
    TLI->ComputeConstraintToUse(OpInfo, SDValue());

    if (OpInfo.ConstraintType == TargetLowering::C_Memory &&
        OpInfo.isIndirect) {
      Value *OpVal = CS->getArgOperand(ArgNo++);
      MadeChange |= optimizeMemoryInst(CS, OpVal, OpVal->getType(), ~0u);
    } else if (OpInfo.Type == InlineAsm::isInput)
      ArgNo++;
  }

  return MadeChange;
}

/// \brief Check if all the uses of \p Val are equivalent (or free) zero or
/// sign extensions.
static bool hasSameExtUse(Value *Val, const TargetLowering &TLI) {
  assert(!Val->use_empty() && "Input must have at least one use");
  const Instruction *FirstUser = cast<Instruction>(*Val->user_begin());
  bool IsSExt = isa<SExtInst>(FirstUser);
  Type *ExtTy = FirstUser->getType();
  for (const User *U : Val->users()) {
    const Instruction *UI = cast<Instruction>(U);
    if ((IsSExt && !isa<SExtInst>(UI)) || (!IsSExt && !isa<ZExtInst>(UI)))
      return false;
    Type *CurTy = UI->getType();
    // Same input and output types: Same instruction after CSE.
    if (CurTy == ExtTy)
      continue;

    // If IsSExt is true, we are in this situation:
    // a = Val
    // b = sext ty1 a to ty2
    // c = sext ty1 a to ty3
    // Assuming ty2 is shorter than ty3, this could be turned into:
    // a = Val
    // b = sext ty1 a to ty2
    // c = sext ty2 b to ty3
    // However, the last sext is not free.
    if (IsSExt)
      return false;

    // This is a ZExt, maybe this is free to extend from one type to another.
    // In that case, we would not account for a different use.
    Type *NarrowTy;
    Type *LargeTy;
    if (ExtTy->getScalarType()->getIntegerBitWidth() >
        CurTy->getScalarType()->getIntegerBitWidth()) {
      NarrowTy = CurTy;
      LargeTy = ExtTy;
    } else {
      NarrowTy = ExtTy;
      LargeTy = CurTy;
    }

    if (!TLI.isZExtFree(NarrowTy, LargeTy))
      return false;
  }
  // All uses are the same or can be derived from one another for free.
  return true;
}

/// \brief Try to speculatively promote extensions in \p Exts and continue
/// promoting through newly promoted operands recursively as far as doing so is
/// profitable. Save extensions profitably moved up, in \p ProfitablyMovedExts.
/// When some promotion happened, \p TPT contains the proper state to revert
/// them.
///
/// \return true if some promotion happened, false otherwise.
bool CodeGenPrepare::tryToPromoteExts(
    TypePromotionTransaction &TPT, const SmallVectorImpl<Instruction *> &Exts,
    SmallVectorImpl<Instruction *> &ProfitablyMovedExts,
    unsigned CreatedInstsCost) {
  bool Promoted = false;

  // Iterate over all the extensions to try to promote them.
  for (auto I : Exts) {
    // Early check if we directly have ext(load).
    if (isa<LoadInst>(I->getOperand(0))) {
      ProfitablyMovedExts.push_back(I);
      continue;
    }

    // Check whether or not we want to do any promotion.  The reason we have
    // this check inside the for loop is to catch the case where an extension
    // is directly fed by a load because in such case the extension can be moved
    // up without any promotion on its operands.
    if (!TLI || !TLI->enableExtLdPromotion() || DisableExtLdPromotion)
      return false;

    // Get the action to perform the promotion.
    TypePromotionHelper::Action TPH =
        TypePromotionHelper::getAction(I, InsertedInsts, *TLI, PromotedInsts);
    // Check if we can promote.
    if (!TPH) {
      // Save the current extension as we cannot move up through its operand.
      ProfitablyMovedExts.push_back(I);
      continue;
    }

    // Save the current state.
    TypePromotionTransaction::ConstRestorationPt LastKnownGood =
        TPT.getRestorationPoint();
    SmallVector<Instruction *, 4> NewExts;
    unsigned NewCreatedInstsCost = 0;
    unsigned ExtCost = !TLI->isExtFree(I);
    // Promote.
    Value *PromotedVal = TPH(I, TPT, PromotedInsts, NewCreatedInstsCost,
                             &NewExts, nullptr, *TLI);
    assert(PromotedVal &&
           "TypePromotionHelper should have filtered out those cases");

    // We would be able to merge only one extension in a load.
    // Therefore, if we have more than 1 new extension we heuristically
    // cut this search path, because it means we degrade the code quality.
    // With exactly 2, the transformation is neutral, because we will merge
    // one extension but leave one. However, we optimistically keep going,
    // because the new extension may be removed too.
    long long TotalCreatedInstsCost = CreatedInstsCost + NewCreatedInstsCost;
    // FIXME: It would be possible to propagate a negative value instead of
    // conservatively ceiling it to 0.
    TotalCreatedInstsCost =
        std::max((long long)0, (TotalCreatedInstsCost - ExtCost));
    if (!StressExtLdPromotion &&
        (TotalCreatedInstsCost > 1 ||
         !isPromotedInstructionLegal(*TLI, *DL, PromotedVal))) {
      // This promotion is not profitable, rollback to the previous state, and
      // save the current extension in ProfitablyMovedExts as the latest
      // speculative promotion turned out to be unprofitable.
      TPT.rollback(LastKnownGood);
      ProfitablyMovedExts.push_back(I);
      continue;
    }
    // Continue promoting NewExts as far as doing so is profitable.
    SmallVector<Instruction *, 2> NewlyMovedExts;
    (void)tryToPromoteExts(TPT, NewExts, NewlyMovedExts, TotalCreatedInstsCost);
    bool NewPromoted = false;
    for (auto ExtInst : NewlyMovedExts) {
      Instruction *MovedExt = cast<Instruction>(ExtInst);
      Value *ExtOperand = MovedExt->getOperand(0);
      // If we have reached to a load, we need this extra profitability check
      // as it could potentially be merged into an ext(load).
      if (isa<LoadInst>(ExtOperand) &&
          !(StressExtLdPromotion || NewCreatedInstsCost <= ExtCost ||
            (ExtOperand->hasOneUse() || hasSameExtUse(ExtOperand, *TLI))))
        continue;

      ProfitablyMovedExts.push_back(MovedExt);
      NewPromoted = true;
    }

    // If none of speculative promotions for NewExts is profitable, rollback
    // and save the current extension (I) as the last profitable extension.
    if (!NewPromoted) {
      TPT.rollback(LastKnownGood);
      ProfitablyMovedExts.push_back(I);
      continue;
    }
    // The promotion is profitable.
    Promoted = true;
  }
  return Promoted;
}

/// Merging redundant sexts when one is dominating the other.
bool CodeGenPrepare::mergeSExts(Function &F) {
  DominatorTree DT(F);
  bool Changed = false;
  for (auto &Entry : ValToSExtendedUses) {
    SExts &Insts = Entry.second;
    SExts CurPts;
    for (Instruction *Inst : Insts) {
      if (RemovedInsts.count(Inst) || !isa<SExtInst>(Inst) ||
          Inst->getOperand(0) != Entry.first)
        continue;
      bool inserted = false;
      for (auto &Pt : CurPts) {
        if (DT.dominates(Inst, Pt)) {
          Pt->replaceAllUsesWith(Inst);
          RemovedInsts.insert(Pt);
          Pt->removeFromParent();
          Pt = Inst;
          inserted = true;
          Changed = true;
          break;
        }
        if (!DT.dominates(Pt, Inst))
          // Give up if we need to merge in a common dominator as the
          // expermients show it is not profitable.
          continue;
        Inst->replaceAllUsesWith(Pt);
        RemovedInsts.insert(Inst);
        Inst->removeFromParent();
        inserted = true;
        Changed = true;
        break;
      }
      if (!inserted)
        CurPts.push_back(Inst);
    }
  }
  return Changed;
}

/// Return true, if an ext(load) can be formed from an extension in
/// \p MovedExts.
bool CodeGenPrepare::canFormExtLd(
    const SmallVectorImpl<Instruction *> &MovedExts, LoadInst *&LI,
    Instruction *&Inst, bool HasPromoted) {
  for (auto *MovedExtInst : MovedExts) {
    if (isa<LoadInst>(MovedExtInst->getOperand(0))) {
      LI = cast<LoadInst>(MovedExtInst->getOperand(0));
      Inst = MovedExtInst;
      break;
    }
  }
  if (!LI)
    return false;

  // If they're already in the same block, there's nothing to do.
  // Make the cheap checks first if we did not promote.
  // If we promoted, we need to check if it is indeed profitable.
  if (!HasPromoted && LI->getParent() == Inst->getParent())
    return false;

  return TLI->isExtLoad(LI, Inst, *DL);
}

/// Move a zext or sext fed by a load into the same basic block as the load,
/// unless conditions are unfavorable. This allows SelectionDAG to fold the
/// extend into the load.
///
/// E.g.,
/// \code
/// %ld = load i32* %addr
/// %add = add nuw i32 %ld, 4
/// %zext = zext i32 %add to i64
// \endcode
/// =>
/// \code
/// %ld = load i32* %addr
/// %zext = zext i32 %ld to i64
/// %add = add nuw i64 %zext, 4
/// \encode
/// Note that the promotion in %add to i64 is done in tryToPromoteExts(), which
/// allow us to match zext(load i32*) to i64.
///
/// Also, try to promote the computations used to obtain a sign extended
/// value used into memory accesses.
/// E.g.,
/// \code
/// a = add nsw i32 b, 3
/// d = sext i32 a to i64
/// e = getelementptr ..., i64 d
/// \endcode
/// =>
/// \code
/// f = sext i32 b to i64
/// a = add nsw i64 f, 3
/// e = getelementptr ..., i64 a
/// \endcode
///
/// \p Inst[in/out] the extension may be modified during the process if some
/// promotions apply.
bool CodeGenPrepare::optimizeExt(Instruction *&Inst) {
  // ExtLoad formation and address type promotion infrastructure requires TLI to
  // be effective.
  if (!TLI)
    return false;

  bool AllowPromotionWithoutCommonHeader = false;
  /// See if it is an interesting sext operations for the address type
  /// promotion before trying to promote it, e.g., the ones with the right
  /// type and used in memory accesses.
  bool ATPConsiderable = TTI->shouldConsiderAddressTypePromotion(
      *Inst, AllowPromotionWithoutCommonHeader);
  TypePromotionTransaction TPT(RemovedInsts);
  TypePromotionTransaction::ConstRestorationPt LastKnownGood =
      TPT.getRestorationPoint();
  SmallVector<Instruction *, 1> Exts;
  SmallVector<Instruction *, 2> SpeculativelyMovedExts;
  Exts.push_back(Inst);

  bool HasPromoted = tryToPromoteExts(TPT, Exts, SpeculativelyMovedExts);

  // Look for a load being extended.
  LoadInst *LI = nullptr;
  Instruction *ExtFedByLoad;

  // Try to promote a chain of computation if it allows to form an extended
  // load.
  if (canFormExtLd(SpeculativelyMovedExts, LI, ExtFedByLoad, HasPromoted)) {
    assert(LI && ExtFedByLoad && "Expect a valid load and extension");
    TPT.commit();
    // Move the extend into the same block as the load
    ExtFedByLoad->removeFromParent();
    ExtFedByLoad->insertAfter(LI);
    // CGP does not check if the zext would be speculatively executed when moved
    // to the same basic block as the load. Preserving its original location
    // would pessimize the debugging experience, as well as negatively impact
    // the quality of sample pgo. We don't want to use "line 0" as that has a
    // size cost in the line-table section and logically the zext can be seen as
    // part of the load. Therefore we conservatively reuse the same debug
    // location for the load and the zext.
    ExtFedByLoad->setDebugLoc(LI->getDebugLoc());
    ++NumExtsMoved;
    Inst = ExtFedByLoad;
    return true;
  }

  // Continue promoting SExts if known as considerable depending on targets.
  if (ATPConsiderable &&
      performAddressTypePromotion(Inst, AllowPromotionWithoutCommonHeader,
                                  HasPromoted, TPT, SpeculativelyMovedExts))
    return true;

  TPT.rollback(LastKnownGood);
  return false;
}

// Perform address type promotion if doing so is profitable.
// If AllowPromotionWithoutCommonHeader == false, we should find other sext
// instructions that sign extended the same initial value. However, if
// AllowPromotionWithoutCommonHeader == true, we expect promoting the
// extension is just profitable.
bool CodeGenPrepare::performAddressTypePromotion(
    Instruction *&Inst, bool AllowPromotionWithoutCommonHeader,
    bool HasPromoted, TypePromotionTransaction &TPT,
    SmallVectorImpl<Instruction *> &SpeculativelyMovedExts) {
  bool Promoted = false;
  SmallPtrSet<Instruction *, 1> UnhandledExts;
  bool AllSeenFirst = true;
  for (auto I : SpeculativelyMovedExts) {
    Value *HeadOfChain = I->getOperand(0);
    DenseMap<Value *, Instruction *>::iterator AlreadySeen =
        SeenChainsForSExt.find(HeadOfChain);
    // If there is an unhandled SExt which has the same header, try to promote
    // it as well.
    if (AlreadySeen != SeenChainsForSExt.end()) {
      if (AlreadySeen->second != nullptr)
        UnhandledExts.insert(AlreadySeen->second);
      AllSeenFirst = false;
    }
  }

  if (!AllSeenFirst || (AllowPromotionWithoutCommonHeader &&
                        SpeculativelyMovedExts.size() == 1)) {
    TPT.commit();
    if (HasPromoted)
      Promoted = true;
    for (auto I : SpeculativelyMovedExts) {
      Value *HeadOfChain = I->getOperand(0);
      SeenChainsForSExt[HeadOfChain] = nullptr;
      ValToSExtendedUses[HeadOfChain].push_back(I);
    }
    // Update Inst as promotion happen.
    Inst = SpeculativelyMovedExts.pop_back_val();
  } else {
    // This is the first chain visited from the header, keep the current chain
    // as unhandled. Defer to promote this until we encounter another SExt
    // chain derived from the same header.
    for (auto I : SpeculativelyMovedExts) {
      Value *HeadOfChain = I->getOperand(0);
      SeenChainsForSExt[HeadOfChain] = Inst;
    }
    return false;
  }

  if (!AllSeenFirst && !UnhandledExts.empty())
    for (auto VisitedSExt : UnhandledExts) {
      if (RemovedInsts.count(VisitedSExt))
        continue;
      TypePromotionTransaction TPT(RemovedInsts);
      SmallVector<Instruction *, 1> Exts;
      SmallVector<Instruction *, 2> Chains;
      Exts.push_back(VisitedSExt);
      bool HasPromoted = tryToPromoteExts(TPT, Exts, Chains);
      TPT.commit();
      if (HasPromoted)
        Promoted = true;
      for (auto I : Chains) {
        Value *HeadOfChain = I->getOperand(0);
        // Mark this as handled.
        SeenChainsForSExt[HeadOfChain] = nullptr;
        ValToSExtendedUses[HeadOfChain].push_back(I);
      }
    }
  return Promoted;
}

bool CodeGenPrepare::optimizeExtUses(Instruction *I) {
  BasicBlock *DefBB = I->getParent();

  // If the result of a {s|z}ext and its source are both live out, rewrite all
  // other uses of the source with result of extension.
  Value *Src = I->getOperand(0);
  if (Src->hasOneUse())
    return false;

  // Only do this xform if truncating is free.
  if (TLI && !TLI->isTruncateFree(I->getType(), Src->getType()))
    return false;

  // Only safe to perform the optimization if the source is also defined in
  // this block.
  if (!isa<Instruction>(Src) || DefBB != cast<Instruction>(Src)->getParent())
    return false;

  bool DefIsLiveOut = false;
  for (User *U : I->users()) {
    Instruction *UI = cast<Instruction>(U);

    // Figure out which BB this ext is used in.
    BasicBlock *UserBB = UI->getParent();
    if (UserBB == DefBB) continue;
    DefIsLiveOut = true;
    break;
  }
  if (!DefIsLiveOut)
    return false;

  // Make sure none of the uses are PHI nodes.
  for (User *U : Src->users()) {
    Instruction *UI = cast<Instruction>(U);
    BasicBlock *UserBB = UI->getParent();
    if (UserBB == DefBB) continue;
    // Be conservative. We don't want this xform to end up introducing
    // reloads just before load / store instructions.
    if (isa<PHINode>(UI) || isa<LoadInst>(UI) || isa<StoreInst>(UI))
      return false;
  }

  // InsertedTruncs - Only insert one trunc in each block once.
  DenseMap<BasicBlock*, Instruction*> InsertedTruncs;

  bool MadeChange = false;
  for (Use &U : Src->uses()) {
    Instruction *User = cast<Instruction>(U.getUser());

    // Figure out which BB this ext is used in.
    BasicBlock *UserBB = User->getParent();
    if (UserBB == DefBB) continue;

    // Both src and def are live in this block. Rewrite the use.
    Instruction *&InsertedTrunc = InsertedTruncs[UserBB];

    if (!InsertedTrunc) {
      BasicBlock::iterator InsertPt = UserBB->getFirstInsertionPt();
      assert(InsertPt != UserBB->end());
      InsertedTrunc = new TruncInst(I, Src->getType(), "", &*InsertPt);
      InsertedInsts.insert(InsertedTrunc);
    }

    // Replace a use of the {s|z}ext source with a use of the result.
    U = InsertedTrunc;
    ++NumExtUses;
    MadeChange = true;
  }

  return MadeChange;
}

// Find loads whose uses only use some of the loaded value's bits.  Add an "and"
// just after the load if the target can fold this into one extload instruction,
// with the hope of eliminating some of the other later "and" instructions using
// the loaded value.  "and"s that are made trivially redundant by the insertion
// of the new "and" are removed by this function, while others (e.g. those whose
// path from the load goes through a phi) are left for isel to potentially
// remove.
//
// For example:
//
// b0:
//   x = load i32
//   ...
// b1:
//   y = and x, 0xff
//   z = use y
//
// becomes:
//
// b0:
//   x = load i32
//   x' = and x, 0xff
//   ...
// b1:
//   z = use x'
//
// whereas:
//
// b0:
//   x1 = load i32
//   ...
// b1:
//   x2 = load i32
//   ...
// b2:
//   x = phi x1, x2
//   y = and x, 0xff
//
// becomes (after a call to optimizeLoadExt for each load):
//
// b0:
//   x1 = load i32
//   x1' = and x1, 0xff
//   ...
// b1:
//   x2 = load i32
//   x2' = and x2, 0xff
//   ...
// b2:
//   x = phi x1', x2'
//   y = and x, 0xff
//

bool CodeGenPrepare::optimizeLoadExt(LoadInst *Load) {

  if (!Load->isSimple() ||
      !(Load->getType()->isIntegerTy() || Load->getType()->isPointerTy()))
    return false;

  // Skip loads we've already transformed.
  if (Load->hasOneUse() &&
      InsertedInsts.count(cast<Instruction>(*Load->user_begin())))
    return false;

  // Look at all uses of Load, looking through phis, to determine how many bits
  // of the loaded value are needed.
  SmallVector<Instruction *, 8> WorkList;
  SmallPtrSet<Instruction *, 16> Visited;
  SmallVector<Instruction *, 8> AndsToMaybeRemove;
  for (auto *U : Load->users())
    WorkList.push_back(cast<Instruction>(U));

  EVT LoadResultVT = TLI->getValueType(*DL, Load->getType());
  unsigned BitWidth = LoadResultVT.getSizeInBits();
  APInt DemandBits(BitWidth, 0);
  APInt WidestAndBits(BitWidth, 0);

  while (!WorkList.empty()) {
    Instruction *I = WorkList.back();
    WorkList.pop_back();

    // Break use-def graph loops.
    if (!Visited.insert(I).second)
      continue;

    // For a PHI node, push all of its users.
    if (auto *Phi = dyn_cast<PHINode>(I)) {
      for (auto *U : Phi->users())
        WorkList.push_back(cast<Instruction>(U));
      continue;
    }

    switch (I->getOpcode()) {
    case llvm::Instruction::And: {
      auto *AndC = dyn_cast<ConstantInt>(I->getOperand(1));
      if (!AndC)
        return false;
      APInt AndBits = AndC->getValue();
      DemandBits |= AndBits;
      // Keep track of the widest and mask we see.
      if (AndBits.ugt(WidestAndBits))
        WidestAndBits = AndBits;
      if (AndBits == WidestAndBits && I->getOperand(0) == Load)
        AndsToMaybeRemove.push_back(I);
      break;
    }

    case llvm::Instruction::Shl: {
      auto *ShlC = dyn_cast<ConstantInt>(I->getOperand(1));
      if (!ShlC)
        return false;
      uint64_t ShiftAmt = ShlC->getLimitedValue(BitWidth - 1);
      DemandBits.setLowBits(BitWidth - ShiftAmt);
      break;
    }

    case llvm::Instruction::Trunc: {
      EVT TruncVT = TLI->getValueType(*DL, I->getType());
      unsigned TruncBitWidth = TruncVT.getSizeInBits();
      DemandBits.setLowBits(TruncBitWidth);
      break;
    }

    default:
      return false;
    }
  }

  uint32_t ActiveBits = DemandBits.getActiveBits();
  // Avoid hoisting (and (load x) 1) since it is unlikely to be folded by the
  // target even if isLoadExtLegal says an i1 EXTLOAD is valid.  For example,
  // for the AArch64 target isLoadExtLegal(ZEXTLOAD, i32, i1) returns true, but
  // (and (load x) 1) is not matched as a single instruction, rather as a LDR
  // followed by an AND.
  // TODO: Look into removing this restriction by fixing backends to either
  // return false for isLoadExtLegal for i1 or have them select this pattern to
  // a single instruction.
  //
  // Also avoid hoisting if we didn't see any ands with the exact DemandBits
  // mask, since these are the only ands that will be removed by isel.
  if (ActiveBits <= 1 || !DemandBits.isMask(ActiveBits) ||
      WidestAndBits != DemandBits)
    return false;

  LLVMContext &Ctx = Load->getType()->getContext();
  Type *TruncTy = Type::getIntNTy(Ctx, ActiveBits);
  EVT TruncVT = TLI->getValueType(*DL, TruncTy);

  // Reject cases that won't be matched as extloads.
  if (!LoadResultVT.bitsGT(TruncVT) || !TruncVT.isRound() ||
      !TLI->isLoadExtLegal(ISD::ZEXTLOAD, LoadResultVT, TruncVT))
    return false;

  IRBuilder<> Builder(Load->getNextNode());
  auto *NewAnd = dyn_cast<Instruction>(
      Builder.CreateAnd(Load, ConstantInt::get(Ctx, DemandBits)));
  // Mark this instruction as "inserted by CGP", so that other
  // optimizations don't touch it.
  InsertedInsts.insert(NewAnd);

  // Replace all uses of load with new and (except for the use of load in the
  // new and itself).
  Load->replaceAllUsesWith(NewAnd);
  NewAnd->setOperand(0, Load);

  // Remove any and instructions that are now redundant.
  for (auto *And : AndsToMaybeRemove)
    // Check that the and mask is the same as the one we decided to put on the
    // new and.
    if (cast<ConstantInt>(And->getOperand(1))->getValue() == DemandBits) {
      And->replaceAllUsesWith(NewAnd);
      if (&*CurInstIterator == And)
        CurInstIterator = std::next(And->getIterator());
      And->eraseFromParent();
      ++NumAndUses;
    }

  ++NumAndsAdded;
  return true;
}

/// Check if V (an operand of a select instruction) is an expensive instruction
/// that is only used once.
static bool sinkSelectOperand(const TargetTransformInfo *TTI, Value *V) {
  auto *I = dyn_cast<Instruction>(V);
  // If it's safe to speculatively execute, then it should not have side
  // effects; therefore, it's safe to sink and possibly *not* execute.
  return I && I->hasOneUse() && isSafeToSpeculativelyExecute(I) &&
         TTI->getUserCost(I) >= TargetTransformInfo::TCC_Expensive;
}

/// Returns true if a SelectInst should be turned into an explicit branch.
static bool isFormingBranchFromSelectProfitable(const TargetTransformInfo *TTI,
                                                const TargetLowering *TLI,
                                                SelectInst *SI) {
  // If even a predictable select is cheap, then a branch can't be cheaper.
  if (!TLI->isPredictableSelectExpensive())
    return false;

  // FIXME: This should use the same heuristics as IfConversion to determine
  // whether a select is better represented as a branch.

  // If metadata tells us that the select condition is obviously predictable,
  // then we want to replace the select with a branch.
  uint64_t TrueWeight, FalseWeight;
  if (SI->extractProfMetadata(TrueWeight, FalseWeight)) {
    uint64_t Max = std::max(TrueWeight, FalseWeight);
    uint64_t Sum = TrueWeight + FalseWeight;
    if (Sum != 0) {
      auto Probability = BranchProbability::getBranchProbability(Max, Sum);
      if (Probability > TLI->getPredictableBranchThreshold())
        return true;
    }
  }

  CmpInst *Cmp = dyn_cast<CmpInst>(SI->getCondition());

  // If a branch is predictable, an out-of-order CPU can avoid blocking on its
  // comparison condition. If the compare has more than one use, there's
  // probably another cmov or setcc around, so it's not worth emitting a branch.
  if (!Cmp || !Cmp->hasOneUse())
    return false;

  // If either operand of the select is expensive and only needed on one side
  // of the select, we should form a branch.
  if (sinkSelectOperand(TTI, SI->getTrueValue()) ||
      sinkSelectOperand(TTI, SI->getFalseValue()))
    return true;

  return false;
}

/// If \p isTrue is true, return the true value of \p SI, otherwise return
/// false value of \p SI. If the true/false value of \p SI is defined by any
/// select instructions in \p Selects, look through the defining select
/// instruction until the true/false value is not defined in \p Selects.
static Value *getTrueOrFalseValue(
    SelectInst *SI, bool isTrue,
    const SmallPtrSet<const Instruction *, 2> &Selects) {
  Value *V;

  for (SelectInst *DefSI = SI; DefSI != nullptr && Selects.count(DefSI);
       DefSI = dyn_cast<SelectInst>(V)) {
    assert(DefSI->getCondition() == SI->getCondition() &&
           "The condition of DefSI does not match with SI");
    V = (isTrue ? DefSI->getTrueValue() : DefSI->getFalseValue());
  }
  return V;
}

/// If we have a SelectInst that will likely profit from branch prediction,
/// turn it into a branch.
bool CodeGenPrepare::optimizeSelectInst(SelectInst *SI) {
  // Find all consecutive select instructions that share the same condition.
  SmallVector<SelectInst *, 2> ASI;
  ASI.push_back(SI);
  for (BasicBlock::iterator It = ++BasicBlock::iterator(SI);
       It != SI->getParent()->end(); ++It) {
    SelectInst *I = dyn_cast<SelectInst>(&*It);
    if (I && SI->getCondition() == I->getCondition()) {
      ASI.push_back(I);
    } else {
      break;
    }
  }

  SelectInst *LastSI = ASI.back();
  // Increment the current iterator to skip all the rest of select instructions
  // because they will be either "not lowered" or "all lowered" to branch.
  CurInstIterator = std::next(LastSI->getIterator());

  bool VectorCond = !SI->getCondition()->getType()->isIntegerTy(1);

  // Can we convert the 'select' to CF ?
  if (DisableSelectToBranch || OptSize || !TLI || VectorCond ||
      SI->getMetadata(LLVMContext::MD_unpredictable))
    return false;

  TargetLowering::SelectSupportKind SelectKind;
  if (VectorCond)
    SelectKind = TargetLowering::VectorMaskSelect;
  else if (SI->getType()->isVectorTy())
    SelectKind = TargetLowering::ScalarCondVectorVal;
  else
    SelectKind = TargetLowering::ScalarValSelect;

  if (TLI->isSelectSupported(SelectKind) &&
      !isFormingBranchFromSelectProfitable(TTI, TLI, SI))
    return false;

  ModifiedDT = true;

  // Transform a sequence like this:
  //    start:
  //       %cmp = cmp uge i32 %a, %b
  //       %sel = select i1 %cmp, i32 %c, i32 %d
  //
  // Into:
  //    start:
  //       %cmp = cmp uge i32 %a, %b
  //       br i1 %cmp, label %select.true, label %select.false
  //    select.true:
  //       br label %select.end
  //    select.false:
  //       br label %select.end
  //    select.end:
  //       %sel = phi i32 [ %c, %select.true ], [ %d, %select.false ]
  //
  // In addition, we may sink instructions that produce %c or %d from
  // the entry block into the destination(s) of the new branch.
  // If the true or false blocks do not contain a sunken instruction, that
  // block and its branch may be optimized away. In that case, one side of the
  // first branch will point directly to select.end, and the corresponding PHI
  // predecessor block will be the start block.

  // First, we split the block containing the select into 2 blocks.
  BasicBlock *StartBlock = SI->getParent();
  BasicBlock::iterator SplitPt = ++(BasicBlock::iterator(LastSI));
  BasicBlock *EndBlock = StartBlock->splitBasicBlock(SplitPt, "select.end");

  // Delete the unconditional branch that was just created by the split.
  StartBlock->getTerminator()->eraseFromParent();

  // These are the new basic blocks for the conditional branch.
  // At least one will become an actual new basic block.
  BasicBlock *TrueBlock = nullptr;
  BasicBlock *FalseBlock = nullptr;
  BranchInst *TrueBranch = nullptr;
  BranchInst *FalseBranch = nullptr;

  // Sink expensive instructions into the conditional blocks to avoid executing
  // them speculatively.
  for (SelectInst *SI : ASI) {
    if (sinkSelectOperand(TTI, SI->getTrueValue())) {
      if (TrueBlock == nullptr) {
        TrueBlock = BasicBlock::Create(SI->getContext(), "select.true.sink",
                                       EndBlock->getParent(), EndBlock);
        TrueBranch = BranchInst::Create(EndBlock, TrueBlock);
      }
      auto *TrueInst = cast<Instruction>(SI->getTrueValue());
      TrueInst->moveBefore(TrueBranch);
    }
    if (sinkSelectOperand(TTI, SI->getFalseValue())) {
      if (FalseBlock == nullptr) {
        FalseBlock = BasicBlock::Create(SI->getContext(), "select.false.sink",
                                        EndBlock->getParent(), EndBlock);
        FalseBranch = BranchInst::Create(EndBlock, FalseBlock);
      }
      auto *FalseInst = cast<Instruction>(SI->getFalseValue());
      FalseInst->moveBefore(FalseBranch);
    }
  }

  // If there was nothing to sink, then arbitrarily choose the 'false' side
  // for a new input value to the PHI.
  if (TrueBlock == FalseBlock) {
    assert(TrueBlock == nullptr &&
           "Unexpected basic block transform while optimizing select");

    FalseBlock = BasicBlock::Create(SI->getContext(), "select.false",
                                    EndBlock->getParent(), EndBlock);
    BranchInst::Create(EndBlock, FalseBlock);
  }

  // Insert the real conditional branch based on the original condition.
  // If we did not create a new block for one of the 'true' or 'false' paths
  // of the condition, it means that side of the branch goes to the end block
  // directly and the path originates from the start block from the point of
  // view of the new PHI.
  BasicBlock *TT, *FT;
  if (TrueBlock == nullptr) {
    TT = EndBlock;
    FT = FalseBlock;
    TrueBlock = StartBlock;
  } else if (FalseBlock == nullptr) {
    TT = TrueBlock;
    FT = EndBlock;
    FalseBlock = StartBlock;
  } else {
    TT = TrueBlock;
    FT = FalseBlock;
  }
  IRBuilder<>(SI).CreateCondBr(SI->getCondition(), TT, FT, SI);

  SmallPtrSet<const Instruction *, 2> INS;
  INS.insert(ASI.begin(), ASI.end());
  // Use reverse iterator because later select may use the value of the
  // earlier select, and we need to propagate value through earlier select
  // to get the PHI operand.
  for (auto It = ASI.rbegin(); It != ASI.rend(); ++It) {
    SelectInst *SI = *It;
    // The select itself is replaced with a PHI Node.
    PHINode *PN = PHINode::Create(SI->getType(), 2, "", &EndBlock->front());
    PN->takeName(SI);
    PN->addIncoming(getTrueOrFalseValue(SI, true, INS), TrueBlock);
    PN->addIncoming(getTrueOrFalseValue(SI, false, INS), FalseBlock);

    SI->replaceAllUsesWith(PN);
    SI->eraseFromParent();
    INS.erase(SI);
    ++NumSelectsExpanded;
  }

  // Instruct OptimizeBlock to skip to the next block.
  CurInstIterator = StartBlock->end();
  return true;
}

static bool isBroadcastShuffle(ShuffleVectorInst *SVI) {
  SmallVector<int, 16> Mask(SVI->getShuffleMask());
  int SplatElem = -1;
  for (unsigned i = 0; i < Mask.size(); ++i) {
    if (SplatElem != -1 && Mask[i] != -1 && Mask[i] != SplatElem)
      return false;
    SplatElem = Mask[i];
  }

  return true;
}

/// Some targets have expensive vector shifts if the lanes aren't all the same
/// (e.g. x86 only introduced "vpsllvd" and friends with AVX2). In these cases
/// it's often worth sinking a shufflevector splat down to its use so that
/// codegen can spot all lanes are identical.
bool CodeGenPrepare::optimizeShuffleVectorInst(ShuffleVectorInst *SVI) {
  BasicBlock *DefBB = SVI->getParent();

  // Only do this xform if variable vector shifts are particularly expensive.
  if (!TLI || !TLI->isVectorShiftByScalarCheap(SVI->getType()))
    return false;

  // We only expect better codegen by sinking a shuffle if we can recognise a
  // constant splat.
  if (!isBroadcastShuffle(SVI))
    return false;

  // InsertedShuffles - Only insert a shuffle in each block once.
  DenseMap<BasicBlock*, Instruction*> InsertedShuffles;

  bool MadeChange = false;
  for (User *U : SVI->users()) {
    Instruction *UI = cast<Instruction>(U);

    // Figure out which BB this ext is used in.
    BasicBlock *UserBB = UI->getParent();
    if (UserBB == DefBB) continue;

    // For now only apply this when the splat is used by a shift instruction.
    if (!UI->isShift()) continue;

    // Everything checks out, sink the shuffle if the user's block doesn't
    // already have a copy.
    Instruction *&InsertedShuffle = InsertedShuffles[UserBB];

    if (!InsertedShuffle) {
      BasicBlock::iterator InsertPt = UserBB->getFirstInsertionPt();
      assert(InsertPt != UserBB->end());
      InsertedShuffle =
          new ShuffleVectorInst(SVI->getOperand(0), SVI->getOperand(1),
                                SVI->getOperand(2), "", &*InsertPt);
    }

    UI->replaceUsesOfWith(SVI, InsertedShuffle);
    MadeChange = true;
  }

  // If we removed all uses, nuke the shuffle.
  if (SVI->use_empty()) {
    SVI->eraseFromParent();
    MadeChange = true;
  }

  return MadeChange;
}

bool CodeGenPrepare::optimizeSwitchInst(SwitchInst *SI) {
  if (!TLI || !DL)
    return false;

  Value *Cond = SI->getCondition();
  Type *OldType = Cond->getType();
  LLVMContext &Context = Cond->getContext();
  MVT RegType = TLI->getRegisterType(Context, TLI->getValueType(*DL, OldType));
  unsigned RegWidth = RegType.getSizeInBits();

  if (RegWidth <= cast<IntegerType>(OldType)->getBitWidth())
    return false;

  // If the register width is greater than the type width, expand the condition
  // of the switch instruction and each case constant to the width of the
  // register. By widening the type of the switch condition, subsequent
  // comparisons (for case comparisons) will not need to be extended to the
  // preferred register width, so we will potentially eliminate N-1 extends,
  // where N is the number of cases in the switch.
  auto *NewType = Type::getIntNTy(Context, RegWidth);

  // Zero-extend the switch condition and case constants unless the switch
  // condition is a function argument that is already being sign-extended.
  // In that case, we can avoid an unnecessary mask/extension by sign-extending
  // everything instead.
  Instruction::CastOps ExtType = Instruction::ZExt;
  if (auto *Arg = dyn_cast<Argument>(Cond))
    if (Arg->hasSExtAttr())
      ExtType = Instruction::SExt;

  auto *ExtInst = CastInst::Create(ExtType, Cond, NewType);
  ExtInst->insertBefore(SI);
  SI->setCondition(ExtInst);
  for (auto Case : SI->cases()) {
    APInt NarrowConst = Case.getCaseValue()->getValue();
    APInt WideConst = (ExtType == Instruction::ZExt) ?
                      NarrowConst.zext(RegWidth) : NarrowConst.sext(RegWidth);
    Case.setValue(ConstantInt::get(Context, WideConst));
  }

  return true;
}


namespace {
/// \brief Helper class to promote a scalar operation to a vector one.
/// This class is used to move downward extractelement transition.
/// E.g.,
/// a = vector_op <2 x i32>
/// b = extractelement <2 x i32> a, i32 0
/// c = scalar_op b
/// store c
///
/// =>
/// a = vector_op <2 x i32>
/// c = vector_op a (equivalent to scalar_op on the related lane)
/// * d = extractelement <2 x i32> c, i32 0
/// * store d
/// Assuming both extractelement and store can be combine, we get rid of the
/// transition.
class VectorPromoteHelper {
  /// DataLayout associated with the current module.
  const DataLayout &DL;

  /// Used to perform some checks on the legality of vector operations.
  const TargetLowering &TLI;

  /// Used to estimated the cost of the promoted chain.
  const TargetTransformInfo &TTI;

  /// The transition being moved downwards.
  Instruction *Transition;
  /// The sequence of instructions to be promoted.
  SmallVector<Instruction *, 4> InstsToBePromoted;
  /// Cost of combining a store and an extract.
  unsigned StoreExtractCombineCost;
  /// Instruction that will be combined with the transition.
  Instruction *CombineInst;

  /// \brief The instruction that represents the current end of the transition.
  /// Since we are faking the promotion until we reach the end of the chain
  /// of computation, we need a way to get the current end of the transition.
  Instruction *getEndOfTransition() const {
    if (InstsToBePromoted.empty())
      return Transition;
    return InstsToBePromoted.back();
  }

  /// \brief Return the index of the original value in the transition.
  /// E.g., for "extractelement <2 x i32> c, i32 1" the original value,
  /// c, is at index 0.
  unsigned getTransitionOriginalValueIdx() const {
    assert(isa<ExtractElementInst>(Transition) &&
           "Other kind of transitions are not supported yet");
    return 0;
  }

  /// \brief Return the index of the index in the transition.
  /// E.g., for "extractelement <2 x i32> c, i32 0" the index
  /// is at index 1.
  unsigned getTransitionIdx() const {
    assert(isa<ExtractElementInst>(Transition) &&
           "Other kind of transitions are not supported yet");
    return 1;
  }

  /// \brief Get the type of the transition.
  /// This is the type of the original value.
  /// E.g., for "extractelement <2 x i32> c, i32 1" the type of the
  /// transition is <2 x i32>.
  Type *getTransitionType() const {
    return Transition->getOperand(getTransitionOriginalValueIdx())->getType();
  }

  /// \brief Promote \p ToBePromoted by moving \p Def downward through.
  /// I.e., we have the following sequence:
  /// Def = Transition <ty1> a to <ty2>
  /// b = ToBePromoted <ty2> Def, ...
  /// =>
  /// b = ToBePromoted <ty1> a, ...
  /// Def = Transition <ty1> ToBePromoted to <ty2>
  void promoteImpl(Instruction *ToBePromoted);

  /// \brief Check whether or not it is profitable to promote all the
  /// instructions enqueued to be promoted.
  bool isProfitableToPromote() {
    Value *ValIdx = Transition->getOperand(getTransitionOriginalValueIdx());
    unsigned Index = isa<ConstantInt>(ValIdx)
                         ? cast<ConstantInt>(ValIdx)->getZExtValue()
                         : -1;
    Type *PromotedType = getTransitionType();

    StoreInst *ST = cast<StoreInst>(CombineInst);
    unsigned AS = ST->getPointerAddressSpace();
    unsigned Align = ST->getAlignment();
    // Check if this store is supported.
    if (!TLI.allowsMisalignedMemoryAccesses(
            TLI.getValueType(DL, ST->getValueOperand()->getType()), AS,
            Align)) {
      // If this is not supported, there is no way we can combine
      // the extract with the store.
      return false;
    }

    // The scalar chain of computation has to pay for the transition
    // scalar to vector.
    // The vector chain has to account for the combining cost.
    uint64_t ScalarCost =
        TTI.getVectorInstrCost(Transition->getOpcode(), PromotedType, Index);
    uint64_t VectorCost = StoreExtractCombineCost;
    for (const auto &Inst : InstsToBePromoted) {
      // Compute the cost.
      // By construction, all instructions being promoted are arithmetic ones.
      // Moreover, one argument is a constant that can be viewed as a splat
      // constant.
      Value *Arg0 = Inst->getOperand(0);
      bool IsArg0Constant = isa<UndefValue>(Arg0) || isa<ConstantInt>(Arg0) ||
                            isa<ConstantFP>(Arg0);
      TargetTransformInfo::OperandValueKind Arg0OVK =
          IsArg0Constant ? TargetTransformInfo::OK_UniformConstantValue
                         : TargetTransformInfo::OK_AnyValue;
      TargetTransformInfo::OperandValueKind Arg1OVK =
          !IsArg0Constant ? TargetTransformInfo::OK_UniformConstantValue
                          : TargetTransformInfo::OK_AnyValue;
      ScalarCost += TTI.getArithmeticInstrCost(
          Inst->getOpcode(), Inst->getType(), Arg0OVK, Arg1OVK);
      VectorCost += TTI.getArithmeticInstrCost(Inst->getOpcode(), PromotedType,
                                               Arg0OVK, Arg1OVK);
    }
    DEBUG(dbgs() << "Estimated cost of computation to be promoted:\nScalar: "
                 << ScalarCost << "\nVector: " << VectorCost << '\n');
    return ScalarCost > VectorCost;
  }

  /// \brief Generate a constant vector with \p Val with the same
  /// number of elements as the transition.
  /// \p UseSplat defines whether or not \p Val should be replicated
  /// across the whole vector.
  /// In other words, if UseSplat == true, we generate <Val, Val, ..., Val>,
  /// otherwise we generate a vector with as many undef as possible:
  /// <undef, ..., undef, Val, undef, ..., undef> where \p Val is only
  /// used at the index of the extract.
  Value *getConstantVector(Constant *Val, bool UseSplat) const {
    unsigned ExtractIdx = UINT_MAX;
    if (!UseSplat) {
      // If we cannot determine where the constant must be, we have to
      // use a splat constant.
      Value *ValExtractIdx = Transition->getOperand(getTransitionIdx());
      if (ConstantInt *CstVal = dyn_cast<ConstantInt>(ValExtractIdx))
        ExtractIdx = CstVal->getSExtValue();
      else
        UseSplat = true;
    }

    unsigned End = getTransitionType()->getVectorNumElements();
    if (UseSplat)
      return ConstantVector::getSplat(End, Val);

    SmallVector<Constant *, 4> ConstVec;
    UndefValue *UndefVal = UndefValue::get(Val->getType());
    for (unsigned Idx = 0; Idx != End; ++Idx) {
      if (Idx == ExtractIdx)
        ConstVec.push_back(Val);
      else
        ConstVec.push_back(UndefVal);
    }
    return ConstantVector::get(ConstVec);
  }

  /// \brief Check if promoting to a vector type an operand at \p OperandIdx
  /// in \p Use can trigger undefined behavior.
  static bool canCauseUndefinedBehavior(const Instruction *Use,
                                        unsigned OperandIdx) {
    // This is not safe to introduce undef when the operand is on
    // the right hand side of a division-like instruction.
    if (OperandIdx != 1)
      return false;
    switch (Use->getOpcode()) {
    default:
      return false;
    case Instruction::SDiv:
    case Instruction::UDiv:
    case Instruction::SRem:
    case Instruction::URem:
      return true;
    case Instruction::FDiv:
    case Instruction::FRem:
      return !Use->hasNoNaNs();
    }
    llvm_unreachable(nullptr);
  }

public:
  VectorPromoteHelper(const DataLayout &DL, const TargetLowering &TLI,
                      const TargetTransformInfo &TTI, Instruction *Transition,
                      unsigned CombineCost)
      : DL(DL), TLI(TLI), TTI(TTI), Transition(Transition),
        StoreExtractCombineCost(CombineCost), CombineInst(nullptr) {
    assert(Transition && "Do not know how to promote null");
  }

  /// \brief Check if we can promote \p ToBePromoted to \p Type.
  bool canPromote(const Instruction *ToBePromoted) const {
    // We could support CastInst too.
    return isa<BinaryOperator>(ToBePromoted);
  }

  /// \brief Check if it is profitable to promote \p ToBePromoted
  /// by moving downward the transition through.
  bool shouldPromote(const Instruction *ToBePromoted) const {
    // Promote only if all the operands can be statically expanded.
    // Indeed, we do not want to introduce any new kind of transitions.
    for (const Use &U : ToBePromoted->operands()) {
      const Value *Val = U.get();
      if (Val == getEndOfTransition()) {
        // If the use is a division and the transition is on the rhs,
        // we cannot promote the operation, otherwise we may create a
        // division by zero.
        if (canCauseUndefinedBehavior(ToBePromoted, U.getOperandNo()))
          return false;
        continue;
      }
      if (!isa<ConstantInt>(Val) && !isa<UndefValue>(Val) &&
          !isa<ConstantFP>(Val))
        return false;
    }
    // Check that the resulting operation is legal.
    int ISDOpcode = TLI.InstructionOpcodeToISD(ToBePromoted->getOpcode());
    if (!ISDOpcode)
      return false;
    return StressStoreExtract ||
           TLI.isOperationLegalOrCustom(
               ISDOpcode, TLI.getValueType(DL, getTransitionType(), true));
  }

  /// \brief Check whether or not \p Use can be combined
  /// with the transition.
  /// I.e., is it possible to do Use(Transition) => AnotherUse?
  bool canCombine(const Instruction *Use) { return isa<StoreInst>(Use); }

  /// \brief Record \p ToBePromoted as part of the chain to be promoted.
  void enqueueForPromotion(Instruction *ToBePromoted) {
    InstsToBePromoted.push_back(ToBePromoted);
  }

  /// \brief Set the instruction that will be combined with the transition.
  void recordCombineInstruction(Instruction *ToBeCombined) {
    assert(canCombine(ToBeCombined) && "Unsupported instruction to combine");
    CombineInst = ToBeCombined;
  }

  /// \brief Promote all the instructions enqueued for promotion if it is
  /// is profitable.
  /// \return True if the promotion happened, false otherwise.
  bool promote() {
    // Check if there is something to promote.
    // Right now, if we do not have anything to combine with,
    // we assume the promotion is not profitable.
    if (InstsToBePromoted.empty() || !CombineInst)
      return false;

    // Check cost.
    if (!StressStoreExtract && !isProfitableToPromote())
      return false;

    // Promote.
    for (auto &ToBePromoted : InstsToBePromoted)
      promoteImpl(ToBePromoted);
    InstsToBePromoted.clear();
    return true;
  }
};
} // End of anonymous namespace.

void VectorPromoteHelper::promoteImpl(Instruction *ToBePromoted) {
  // At this point, we know that all the operands of ToBePromoted but Def
  // can be statically promoted.
  // For Def, we need to use its parameter in ToBePromoted:
  // b = ToBePromoted ty1 a
  // Def = Transition ty1 b to ty2
  // Move the transition down.
  // 1. Replace all uses of the promoted operation by the transition.
  // = ... b => = ... Def.
  assert(ToBePromoted->getType() == Transition->getType() &&
         "The type of the result of the transition does not match "
         "the final type");
  ToBePromoted->replaceAllUsesWith(Transition);
  // 2. Update the type of the uses.
  // b = ToBePromoted ty2 Def => b = ToBePromoted ty1 Def.
  Type *TransitionTy = getTransitionType();
  ToBePromoted->mutateType(TransitionTy);
  // 3. Update all the operands of the promoted operation with promoted
  // operands.
  // b = ToBePromoted ty1 Def => b = ToBePromoted ty1 a.
  for (Use &U : ToBePromoted->operands()) {
    Value *Val = U.get();
    Value *NewVal = nullptr;
    if (Val == Transition)
      NewVal = Transition->getOperand(getTransitionOriginalValueIdx());
    else if (isa<UndefValue>(Val) || isa<ConstantInt>(Val) ||
             isa<ConstantFP>(Val)) {
      // Use a splat constant if it is not safe to use undef.
      NewVal = getConstantVector(
          cast<Constant>(Val),
          isa<UndefValue>(Val) ||
              canCauseUndefinedBehavior(ToBePromoted, U.getOperandNo()));
    } else
      llvm_unreachable("Did you modified shouldPromote and forgot to update "
                       "this?");
    ToBePromoted->setOperand(U.getOperandNo(), NewVal);
  }
  Transition->removeFromParent();
  Transition->insertAfter(ToBePromoted);
  Transition->setOperand(getTransitionOriginalValueIdx(), ToBePromoted);
}

/// Some targets can do store(extractelement) with one instruction.
/// Try to push the extractelement towards the stores when the target
/// has this feature and this is profitable.
bool CodeGenPrepare::optimizeExtractElementInst(Instruction *Inst) {
  unsigned CombineCost = UINT_MAX;
  if (DisableStoreExtract || !TLI ||
      (!StressStoreExtract &&
       !TLI->canCombineStoreAndExtract(Inst->getOperand(0)->getType(),
                                       Inst->getOperand(1), CombineCost)))
    return false;

  // At this point we know that Inst is a vector to scalar transition.
  // Try to move it down the def-use chain, until:
  // - We can combine the transition with its single use
  //   => we got rid of the transition.
  // - We escape the current basic block
  //   => we would need to check that we are moving it at a cheaper place and
  //      we do not do that for now.
  BasicBlock *Parent = Inst->getParent();
  DEBUG(dbgs() << "Found an interesting transition: " << *Inst << '\n');
  VectorPromoteHelper VPH(*DL, *TLI, *TTI, Inst, CombineCost);
  // If the transition has more than one use, assume this is not going to be
  // beneficial.
  while (Inst->hasOneUse()) {
    Instruction *ToBePromoted = cast<Instruction>(*Inst->user_begin());
    DEBUG(dbgs() << "Use: " << *ToBePromoted << '\n');

    if (ToBePromoted->getParent() != Parent) {
      DEBUG(dbgs() << "Instruction to promote is in a different block ("
                   << ToBePromoted->getParent()->getName()
                   << ") than the transition (" << Parent->getName() << ").\n");
      return false;
    }

    if (VPH.canCombine(ToBePromoted)) {
      DEBUG(dbgs() << "Assume " << *Inst << '\n'
                   << "will be combined with: " << *ToBePromoted << '\n');
      VPH.recordCombineInstruction(ToBePromoted);
      bool Changed = VPH.promote();
      NumStoreExtractExposed += Changed;
      return Changed;
    }

    DEBUG(dbgs() << "Try promoting.\n");
    if (!VPH.canPromote(ToBePromoted) || !VPH.shouldPromote(ToBePromoted))
      return false;

    DEBUG(dbgs() << "Promoting is possible... Enqueue for promotion!\n");

    VPH.enqueueForPromotion(ToBePromoted);
    Inst = ToBePromoted;
  }
  return false;
}

/// For the instruction sequence of store below, F and I values
/// are bundled together as an i64 value before being stored into memory.
/// Sometimes it is more efficent to generate separate stores for F and I,
/// which can remove the bitwise instructions or sink them to colder places.
///
///   (store (or (zext (bitcast F to i32) to i64),
///              (shl (zext I to i64), 32)), addr)  -->
///   (store F, addr) and (store I, addr+4)
///
/// Similarly, splitting for other merged store can also be beneficial, like:
/// For pair of {i32, i32}, i64 store --> two i32 stores.
/// For pair of {i32, i16}, i64 store --> two i32 stores.
/// For pair of {i16, i16}, i32 store --> two i16 stores.
/// For pair of {i16, i8},  i32 store --> two i16 stores.
/// For pair of {i8, i8},   i16 store --> two i8 stores.
///
/// We allow each target to determine specifically which kind of splitting is
/// supported.
///
/// The store patterns are commonly seen from the simple code snippet below
/// if only std::make_pair(...) is sroa transformed before inlined into hoo.
///   void goo(const std::pair<int, float> &);
///   hoo() {
///     ...
///     goo(std::make_pair(tmp, ftmp));
///     ...
///   }
///
/// Although we already have similar splitting in DAG Combine, we duplicate
/// it in CodeGenPrepare to catch the case in which pattern is across
/// multiple BBs. The logic in DAG Combine is kept to catch case generated
/// during code expansion.
static bool splitMergedValStore(StoreInst &SI, const DataLayout &DL,
                                const TargetLowering &TLI) {
  // Handle simple but common cases only.
  Type *StoreType = SI.getValueOperand()->getType();
  if (DL.getTypeStoreSizeInBits(StoreType) != DL.getTypeSizeInBits(StoreType) ||
      DL.getTypeSizeInBits(StoreType) == 0)
    return false;

  unsigned HalfValBitSize = DL.getTypeSizeInBits(StoreType) / 2;
  Type *SplitStoreType = Type::getIntNTy(SI.getContext(), HalfValBitSize);
  if (DL.getTypeStoreSizeInBits(SplitStoreType) !=
      DL.getTypeSizeInBits(SplitStoreType))
    return false;

  // Match the following patterns:
  // (store (or (zext LValue to i64),
  //            (shl (zext HValue to i64), 32)), HalfValBitSize)
  //  or
  // (store (or (shl (zext HValue to i64), 32)), HalfValBitSize)
  //            (zext LValue to i64),
  // Expect both operands of OR and the first operand of SHL have only
  // one use.
  Value *LValue, *HValue;
  if (!match(SI.getValueOperand(),
             m_c_Or(m_OneUse(m_ZExt(m_Value(LValue))),
                    m_OneUse(m_Shl(m_OneUse(m_ZExt(m_Value(HValue))),
                                   m_SpecificInt(HalfValBitSize))))))
    return false;

  // Check LValue and HValue are int with size less or equal than 32.
  if (!LValue->getType()->isIntegerTy() ||
      DL.getTypeSizeInBits(LValue->getType()) > HalfValBitSize ||
      !HValue->getType()->isIntegerTy() ||
      DL.getTypeSizeInBits(HValue->getType()) > HalfValBitSize)
    return false;

  // If LValue/HValue is a bitcast instruction, use the EVT before bitcast
  // as the input of target query.
  auto *LBC = dyn_cast<BitCastInst>(LValue);
  auto *HBC = dyn_cast<BitCastInst>(HValue);
  EVT LowTy = LBC ? EVT::getEVT(LBC->getOperand(0)->getType())
                  : EVT::getEVT(LValue->getType());
  EVT HighTy = HBC ? EVT::getEVT(HBC->getOperand(0)->getType())
                   : EVT::getEVT(HValue->getType());
  if (!ForceSplitStore && !TLI.isMultiStoresCheaperThanBitsMerge(LowTy, HighTy))
    return false;

  // Start to split store.
  IRBuilder<> Builder(SI.getContext());
  Builder.SetInsertPoint(&SI);

  // If LValue/HValue is a bitcast in another BB, create a new one in current
  // BB so it may be merged with the splitted stores by dag combiner.
  if (LBC && LBC->getParent() != SI.getParent())
    LValue = Builder.CreateBitCast(LBC->getOperand(0), LBC->getType());
  if (HBC && HBC->getParent() != SI.getParent())
    HValue = Builder.CreateBitCast(HBC->getOperand(0), HBC->getType());

  auto CreateSplitStore = [&](Value *V, bool Upper) {
    V = Builder.CreateZExtOrBitCast(V, SplitStoreType);
    Value *Addr = Builder.CreateBitCast(
        SI.getOperand(1),
        SplitStoreType->getPointerTo(SI.getPointerAddressSpace()));
    if (Upper)
      Addr = Builder.CreateGEP(
          SplitStoreType, Addr,
          ConstantInt::get(Type::getInt32Ty(SI.getContext()), 1));
    Builder.CreateAlignedStore(
        V, Addr, Upper ? SI.getAlignment() / 2 : SI.getAlignment());
  };

  CreateSplitStore(LValue, false);
  CreateSplitStore(HValue, true);

  // Delete the old store.
  SI.eraseFromParent();
  return true;
}

bool CodeGenPrepare::optimizeInst(Instruction *I, bool &ModifiedDT) {
  // Bail out if we inserted the instruction to prevent optimizations from
  // stepping on each other's toes.
  if (InsertedInsts.count(I))
    return false;

  if (PHINode *P = dyn_cast<PHINode>(I)) {
    // It is possible for very late stage optimizations (such as SimplifyCFG)
    // to introduce PHI nodes too late to be cleaned up.  If we detect such a
    // trivial PHI, go ahead and zap it here.
    if (Value *V = SimplifyInstruction(P, {*DL, TLInfo})) {
      P->replaceAllUsesWith(V);
      P->eraseFromParent();
      ++NumPHIsElim;
      return true;
    }
    return false;
  }

  if (CastInst *CI = dyn_cast<CastInst>(I)) {
    // If the source of the cast is a constant, then this should have
    // already been constant folded.  The only reason NOT to constant fold
    // it is if something (e.g. LSR) was careful to place the constant
    // evaluation in a block other than then one that uses it (e.g. to hoist
    // the address of globals out of a loop).  If this is the case, we don't
    // want to forward-subst the cast.
    if (isa<Constant>(CI->getOperand(0)))
      return false;

    if (TLI && OptimizeNoopCopyExpression(CI, *TLI, *DL))
      return true;

    if (isa<ZExtInst>(I) || isa<SExtInst>(I)) {
      /// Sink a zext or sext into its user blocks if the target type doesn't
      /// fit in one register
      if (TLI &&
          TLI->getTypeAction(CI->getContext(),
                             TLI->getValueType(*DL, CI->getType())) ==
              TargetLowering::TypeExpandInteger) {
        return SinkCast(CI);
      } else {
        bool MadeChange = optimizeExt(I);
        return MadeChange | optimizeExtUses(I);
      }
    }
    return false;
  }

  if (CmpInst *CI = dyn_cast<CmpInst>(I))
    if (!TLI || !TLI->hasMultipleConditionRegisters())
      return OptimizeCmpExpression(CI, TLI);

  if (LoadInst *LI = dyn_cast<LoadInst>(I)) {
    LI->setMetadata(LLVMContext::MD_invariant_group, nullptr);
    if (TLI) {
      bool Modified = optimizeLoadExt(LI);
      unsigned AS = LI->getPointerAddressSpace();
      Modified |= optimizeMemoryInst(I, I->getOperand(0), LI->getType(), AS);
      return Modified;
    }
    return false;
  }

  if (StoreInst *SI = dyn_cast<StoreInst>(I)) {
    if (TLI && splitMergedValStore(*SI, *DL, *TLI))
      return true;
    SI->setMetadata(LLVMContext::MD_invariant_group, nullptr);
    if (TLI) {
      unsigned AS = SI->getPointerAddressSpace();
      return optimizeMemoryInst(I, SI->getOperand(1),
                                SI->getOperand(0)->getType(), AS);
    }
    return false;
  }

  if (AtomicRMWInst *RMW = dyn_cast<AtomicRMWInst>(I)) {
      unsigned AS = RMW->getPointerAddressSpace();
      return optimizeMemoryInst(I, RMW->getPointerOperand(),
                                RMW->getType(), AS);
  }

  if (AtomicCmpXchgInst *CmpX = dyn_cast<AtomicCmpXchgInst>(I)) {
      unsigned AS = CmpX->getPointerAddressSpace();
      return optimizeMemoryInst(I, CmpX->getPointerOperand(),
                                CmpX->getCompareOperand()->getType(), AS);
  }

  BinaryOperator *BinOp = dyn_cast<BinaryOperator>(I);

  if (BinOp && (BinOp->getOpcode() == Instruction::And) &&
      EnableAndCmpSinking && TLI)
    return sinkAndCmp0Expression(BinOp, *TLI, InsertedInsts);

  if (BinOp && (BinOp->getOpcode() == Instruction::AShr ||
                BinOp->getOpcode() == Instruction::LShr)) {
    ConstantInt *CI = dyn_cast<ConstantInt>(BinOp->getOperand(1));
    if (TLI && CI && TLI->hasExtractBitsInsn())
      return OptimizeExtractBits(BinOp, CI, *TLI, *DL);

    return false;
  }

  if (GetElementPtrInst *GEPI = dyn_cast<GetElementPtrInst>(I)) {
    if (GEPI->hasAllZeroIndices()) {
      /// The GEP operand must be a pointer, so must its result -> BitCast
      Instruction *NC = new BitCastInst(GEPI->getOperand(0), GEPI->getType(),
                                        GEPI->getName(), GEPI);
      GEPI->replaceAllUsesWith(NC);
      GEPI->eraseFromParent();
      ++NumGEPsElim;
      optimizeInst(NC, ModifiedDT);
      return true;
    }
    return false;
  }

  if (CallInst *CI = dyn_cast<CallInst>(I))
    return optimizeCallInst(CI, ModifiedDT);

  if (SelectInst *SI = dyn_cast<SelectInst>(I))
    return optimizeSelectInst(SI);

  if (ShuffleVectorInst *SVI = dyn_cast<ShuffleVectorInst>(I))
    return optimizeShuffleVectorInst(SVI);

  if (auto *Switch = dyn_cast<SwitchInst>(I))
    return optimizeSwitchInst(Switch);

  if (isa<ExtractElementInst>(I))
    return optimizeExtractElementInst(I);

  return false;
}

/// Given an OR instruction, check to see if this is a bitreverse
/// idiom. If so, insert the new intrinsic and return true.
static bool makeBitReverse(Instruction &I, const DataLayout &DL,
                           const TargetLowering &TLI) {
  if (!I.getType()->isIntegerTy() ||
      !TLI.isOperationLegalOrCustom(ISD::BITREVERSE,
                                    TLI.getValueType(DL, I.getType(), true)))
    return false;

  SmallVector<Instruction*, 4> Insts;
  if (!recognizeBSwapOrBitReverseIdiom(&I, false, true, Insts))
    return false;
  Instruction *LastInst = Insts.back();
  I.replaceAllUsesWith(LastInst);
  RecursivelyDeleteTriviallyDeadInstructions(&I);
  return true;
}

// In this pass we look for GEP and cast instructions that are used
// across basic blocks and rewrite them to improve basic-block-at-a-time
// selection.
bool CodeGenPrepare::optimizeBlock(BasicBlock &BB, bool &ModifiedDT) {
  SunkAddrs.clear();
  bool MadeChange = false;

  CurInstIterator = BB.begin();
  while (CurInstIterator != BB.end()) {
    MadeChange |= optimizeInst(&*CurInstIterator++, ModifiedDT);
    if (ModifiedDT)
      return true;
  }

  bool MadeBitReverse = true;
  while (TLI && MadeBitReverse) {
    MadeBitReverse = false;
    for (auto &I : reverse(BB)) {
      if (makeBitReverse(I, *DL, *TLI)) {
        MadeBitReverse = MadeChange = true;
        ModifiedDT = true;
        break;
      }
    }
  }
  MadeChange |= dupRetToEnableTailCallOpts(&BB);

  return MadeChange;
}

// llvm.dbg.value is far away from the value then iSel may not be able
// handle it properly. iSel will drop llvm.dbg.value if it can not
// find a node corresponding to the value.
bool CodeGenPrepare::placeDbgValues(Function &F) {
  bool MadeChange = false;
  for (BasicBlock &BB : F) {
    Instruction *PrevNonDbgInst = nullptr;
    for (BasicBlock::iterator BI = BB.begin(), BE = BB.end(); BI != BE;) {
      Instruction *Insn = &*BI++;
      DbgValueInst *DVI = dyn_cast<DbgValueInst>(Insn);
      // Leave dbg.values that refer to an alloca alone. These
      // instrinsics describe the address of a variable (= the alloca)
      // being taken.  They should not be moved next to the alloca
      // (and to the beginning of the scope), but rather stay close to
      // where said address is used.
      if (!DVI || (DVI->getValue() && isa<AllocaInst>(DVI->getValue()))) {
        PrevNonDbgInst = Insn;
        continue;
      }

      Instruction *VI = dyn_cast_or_null<Instruction>(DVI->getValue());
      if (VI && VI != PrevNonDbgInst && !VI->isTerminator()) {
        // If VI is a phi in a block with an EHPad terminator, we can't insert
        // after it.
        if (isa<PHINode>(VI) && VI->getParent()->getTerminator()->isEHPad())
          continue;
        DEBUG(dbgs() << "Moving Debug Value before :\n" << *DVI << ' ' << *VI);
        DVI->removeFromParent();
        if (isa<PHINode>(VI))
          DVI->insertBefore(&*VI->getParent()->getFirstInsertionPt());
        else
          DVI->insertAfter(VI);
        MadeChange = true;
        ++NumDbgValueMoved;
      }
    }
  }
  return MadeChange;
}

/// \brief Scale down both weights to fit into uint32_t.
static void scaleWeights(uint64_t &NewTrue, uint64_t &NewFalse) {
  uint64_t NewMax = (NewTrue > NewFalse) ? NewTrue : NewFalse;
  uint32_t Scale = (NewMax / UINT32_MAX) + 1;
  NewTrue = NewTrue / Scale;
  NewFalse = NewFalse / Scale;
}

/// \brief Some targets prefer to split a conditional branch like:
/// \code
///   %0 = icmp ne i32 %a, 0
///   %1 = icmp ne i32 %b, 0
///   %or.cond = or i1 %0, %1
///   br i1 %or.cond, label %TrueBB, label %FalseBB
/// \endcode
/// into multiple branch instructions like:
/// \code
///   bb1:
///     %0 = icmp ne i32 %a, 0
///     br i1 %0, label %TrueBB, label %bb2
///   bb2:
///     %1 = icmp ne i32 %b, 0
///     br i1 %1, label %TrueBB, label %FalseBB
/// \endcode
/// This usually allows instruction selection to do even further optimizations
/// and combine the compare with the branch instruction. Currently this is
/// applied for targets which have "cheap" jump instructions.
///
/// FIXME: Remove the (equivalent?) implementation in SelectionDAG.
///
bool CodeGenPrepare::splitBranchCondition(Function &F) {
  if (!TM || !TM->Options.EnableFastISel || !TLI || TLI->isJumpExpensive())
    return false;

  bool MadeChange = false;
  for (auto &BB : F) {
    // Does this BB end with the following?
    //   %cond1 = icmp|fcmp|binary instruction ...
    //   %cond2 = icmp|fcmp|binary instruction ...
    //   %cond.or = or|and i1 %cond1, cond2
    //   br i1 %cond.or label %dest1, label %dest2"
    BinaryOperator *LogicOp;
    BasicBlock *TBB, *FBB;
    if (!match(BB.getTerminator(), m_Br(m_OneUse(m_BinOp(LogicOp)), TBB, FBB)))
      continue;

    auto *Br1 = cast<BranchInst>(BB.getTerminator());
    if (Br1->getMetadata(LLVMContext::MD_unpredictable))
      continue;

    unsigned Opc;
    Value *Cond1, *Cond2;
    if (match(LogicOp, m_And(m_OneUse(m_Value(Cond1)),
                             m_OneUse(m_Value(Cond2)))))
      Opc = Instruction::And;
    else if (match(LogicOp, m_Or(m_OneUse(m_Value(Cond1)),
                                 m_OneUse(m_Value(Cond2)))))
      Opc = Instruction::Or;
    else
      continue;

    if (!match(Cond1, m_CombineOr(m_Cmp(), m_BinOp())) ||
        !match(Cond2, m_CombineOr(m_Cmp(), m_BinOp()))   )
      continue;

    DEBUG(dbgs() << "Before branch condition splitting\n"; BB.dump());

    // Create a new BB.
    auto TmpBB =
        BasicBlock::Create(BB.getContext(), BB.getName() + ".cond.split",
                           BB.getParent(), BB.getNextNode());

    // Update original basic block by using the first condition directly by the
    // branch instruction and removing the no longer needed and/or instruction.
    Br1->setCondition(Cond1);
    LogicOp->eraseFromParent();

    // Depending on the conditon we have to either replace the true or the false
    // successor of the original branch instruction.
    if (Opc == Instruction::And)
      Br1->setSuccessor(0, TmpBB);
    else
      Br1->setSuccessor(1, TmpBB);

    // Fill in the new basic block.
    auto *Br2 = IRBuilder<>(TmpBB).CreateCondBr(Cond2, TBB, FBB);
    if (auto *I = dyn_cast<Instruction>(Cond2)) {
      I->removeFromParent();
      I->insertBefore(Br2);
    }

    // Update PHI nodes in both successors. The original BB needs to be
    // replaced in one successor's PHI nodes, because the branch comes now from
    // the newly generated BB (NewBB). In the other successor we need to add one
    // incoming edge to the PHI nodes, because both branch instructions target
    // now the same successor. Depending on the original branch condition
    // (and/or) we have to swap the successors (TrueDest, FalseDest), so that
    // we perform the correct update for the PHI nodes.
    // This doesn't change the successor order of the just created branch
    // instruction (or any other instruction).
    if (Opc == Instruction::Or)
      std::swap(TBB, FBB);

    // Replace the old BB with the new BB.
    for (auto &I : *TBB) {
      PHINode *PN = dyn_cast<PHINode>(&I);
      if (!PN)
        break;
      int i;
      while ((i = PN->getBasicBlockIndex(&BB)) >= 0)
        PN->setIncomingBlock(i, TmpBB);
    }

    // Add another incoming edge form the new BB.
    for (auto &I : *FBB) {
      PHINode *PN = dyn_cast<PHINode>(&I);
      if (!PN)
        break;
      auto *Val = PN->getIncomingValueForBlock(&BB);
      PN->addIncoming(Val, TmpBB);
    }

    // Update the branch weights (from SelectionDAGBuilder::
    // FindMergedConditions).
    if (Opc == Instruction::Or) {
      // Codegen X | Y as:
      // BB1:
      //   jmp_if_X TBB
      //   jmp TmpBB
      // TmpBB:
      //   jmp_if_Y TBB
      //   jmp FBB
      //

      // We have flexibility in setting Prob for BB1 and Prob for NewBB.
      // The requirement is that
      //   TrueProb for BB1 + (FalseProb for BB1 * TrueProb for TmpBB)
      //     = TrueProb for orignal BB.
      // Assuming the orignal weights are A and B, one choice is to set BB1's
      // weights to A and A+2B, and set TmpBB's weights to A and 2B. This choice
      // assumes that
      //   TrueProb for BB1 == FalseProb for BB1 * TrueProb for TmpBB.
      // Another choice is to assume TrueProb for BB1 equals to TrueProb for
      // TmpBB, but the math is more complicated.
      uint64_t TrueWeight, FalseWeight;
      if (Br1->extractProfMetadata(TrueWeight, FalseWeight)) {
        uint64_t NewTrueWeight = TrueWeight;
        uint64_t NewFalseWeight = TrueWeight + 2 * FalseWeight;
        scaleWeights(NewTrueWeight, NewFalseWeight);
        Br1->setMetadata(LLVMContext::MD_prof, MDBuilder(Br1->getContext())
                         .createBranchWeights(TrueWeight, FalseWeight));

        NewTrueWeight = TrueWeight;
        NewFalseWeight = 2 * FalseWeight;
        scaleWeights(NewTrueWeight, NewFalseWeight);
        Br2->setMetadata(LLVMContext::MD_prof, MDBuilder(Br2->getContext())
                         .createBranchWeights(TrueWeight, FalseWeight));
      }
    } else {
      // Codegen X & Y as:
      // BB1:
      //   jmp_if_X TmpBB
      //   jmp FBB
      // TmpBB:
      //   jmp_if_Y TBB
      //   jmp FBB
      //
      //  This requires creation of TmpBB after CurBB.

      // We have flexibility in setting Prob for BB1 and Prob for TmpBB.
      // The requirement is that
      //   FalseProb for BB1 + (TrueProb for BB1 * FalseProb for TmpBB)
      //     = FalseProb for orignal BB.
      // Assuming the orignal weights are A and B, one choice is to set BB1's
      // weights to 2A+B and B, and set TmpBB's weights to 2A and B. This choice
      // assumes that
      //   FalseProb for BB1 == TrueProb for BB1 * FalseProb for TmpBB.
      uint64_t TrueWeight, FalseWeight;
      if (Br1->extractProfMetadata(TrueWeight, FalseWeight)) {
        uint64_t NewTrueWeight = 2 * TrueWeight + FalseWeight;
        uint64_t NewFalseWeight = FalseWeight;
        scaleWeights(NewTrueWeight, NewFalseWeight);
        Br1->setMetadata(LLVMContext::MD_prof, MDBuilder(Br1->getContext())
                         .createBranchWeights(TrueWeight, FalseWeight));

        NewTrueWeight = 2 * TrueWeight;
        NewFalseWeight = FalseWeight;
        scaleWeights(NewTrueWeight, NewFalseWeight);
        Br2->setMetadata(LLVMContext::MD_prof, MDBuilder(Br2->getContext())
                         .createBranchWeights(TrueWeight, FalseWeight));
      }
    }

    // Note: No point in getting fancy here, since the DT info is never
    // available to CodeGenPrepare.
    ModifiedDT = true;

    MadeChange = true;

    DEBUG(dbgs() << "After branch condition splitting\n"; BB.dump();
          TmpBB->dump());
  }
  return MadeChange;
}
