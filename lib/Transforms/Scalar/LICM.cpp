//===-- LICM.cpp - Loop Invariant Code Motion Pass ------------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This pass performs loop invariant code motion, attempting to remove as much
// code from the body of a loop as possible.  It does this by either hoisting
// code into the preheader block, or by sinking code to the exit blocks if it is
// safe.  This pass also promotes must-aliased memory locations in the loop to
// live in registers, thus hoisting and sinking "invariant" loads and stores.
//
// This pass uses alias analysis for two purposes:
//
//  1. Moving loop invariant loads and calls out of loops.  If we can determine
//     that a load or call inside of a loop never aliases anything stored to,
//     we can hoist it or sink it like any other instruction.
//  2. Scalar Promotion of Memory - If there is a store instruction inside of
//     the loop, we try to move the store to happen AFTER the loop instead of
//     inside of the loop.  This can only happen if a few conditions are true:
//       A. The pointer stored through is loop invariant
//       B. There are no stores or loads in the loop which _may_ alias the
//          pointer.  There are no calls in the loop which mod/ref the pointer.
//     If these conditions are true, we can promote the loads and stores in the
//     loop of the pointer to use a temporary alloca'd variable.  We then use
//     the SSAUpdater to construct the appropriate SSA form for the value.
//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/Scalar/LICM.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/Analysis/AliasSetTracker.h"
#include "llvm/Analysis/BasicAliasAnalysis.h"
#include "llvm/Analysis/CaptureTracking.h"
#include "llvm/Analysis/ConstantFolding.h"
#include "llvm/Analysis/GlobalsModRef.h"
#include "llvm/Analysis/Loads.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/LoopPass.h"
#include "llvm/Analysis/MemoryBuiltins.h"
#include "llvm/Analysis/OptimizationDiagnosticInfo.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/Analysis/ScalarEvolutionAliasAnalysis.h"
#include "llvm/Analysis/TargetLibraryInfo.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/PredIteratorCache.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Scalar.h"
#include "llvm/Transforms/Scalar/LoopPassManager.h"
#include "llvm/Transforms/Utils/Local.h"
#include "llvm/Transforms/Utils/LoopUtils.h"
#include "llvm/Transforms/Utils/SSAUpdater.h"
#include <algorithm>
#include <utility>
using namespace llvm;

#define DEBUG_TYPE "licm"

STATISTIC(NumSunk, "Number of instructions sunk out of loop");
STATISTIC(NumHoisted, "Number of instructions hoisted out of loop");
STATISTIC(NumMovedLoads, "Number of load insts hoisted or sunk");
STATISTIC(NumMovedCalls, "Number of call insts hoisted or sunk");
STATISTIC(NumPromoted, "Number of memory locations promoted to registers");

/// Memory promotion is enabled by default.
static cl::opt<bool>
    DisablePromotion("disable-licm-promotion", cl::Hidden, cl::init(false),
                     cl::desc("Disable memory promotion in LICM pass"));

static cl::opt<uint32_t> MaxNumUsesTraversed(
    "licm-max-num-uses-traversed", cl::Hidden, cl::init(8),
    cl::desc("Max num uses visited for identifying load "
             "invariance in loop using invariant start (default = 8)"));

static bool inSubLoop(BasicBlock *BB, Loop *CurLoop, LoopInfo *LI);
static bool isNotUsedInLoop(const Instruction &I, const Loop *CurLoop,
                            const LoopSafetyInfo *SafetyInfo);
static bool hoist(Instruction &I, const DominatorTree *DT, const Loop *CurLoop,
                  const LoopSafetyInfo *SafetyInfo,
                  OptimizationRemarkEmitter *ORE);
static bool sink(Instruction &I, const LoopInfo *LI, const DominatorTree *DT,
                 const Loop *CurLoop, AliasSetTracker *CurAST,
                 const LoopSafetyInfo *SafetyInfo,
                 OptimizationRemarkEmitter *ORE);
static bool isSafeToExecuteUnconditionally(Instruction &Inst,
                                           const DominatorTree *DT,
                                           const Loop *CurLoop,
                                           const LoopSafetyInfo *SafetyInfo,
                                           OptimizationRemarkEmitter *ORE,
                                           const Instruction *CtxI = nullptr);
static bool pointerInvalidatedByLoop(Value *V, uint64_t Size,
                                     const AAMDNodes &AAInfo,
                                     AliasSetTracker *CurAST);
static Instruction *
CloneInstructionInExitBlock(Instruction &I, BasicBlock &ExitBlock, PHINode &PN,
                            const LoopInfo *LI,
                            const LoopSafetyInfo *SafetyInfo);

namespace {
struct LoopInvariantCodeMotion {
  bool runOnLoop(Loop *L, AliasAnalysis *AA, LoopInfo *LI, DominatorTree *DT,
                 TargetLibraryInfo *TLI, ScalarEvolution *SE,
                 OptimizationRemarkEmitter *ORE, bool DeleteAST);

  DenseMap<Loop *, AliasSetTracker *> &getLoopToAliasSetMap() {
    return LoopToAliasSetMap;
  }

private:
  DenseMap<Loop *, AliasSetTracker *> LoopToAliasSetMap;

  AliasSetTracker *collectAliasInfoForLoop(Loop *L, LoopInfo *LI,
                                           AliasAnalysis *AA);
};

struct LegacyLICMPass : public LoopPass {
  static char ID; // Pass identification, replacement for typeid
  LegacyLICMPass() : LoopPass(ID) {
    initializeLegacyLICMPassPass(*PassRegistry::getPassRegistry());
  }

  bool runOnLoop(Loop *L, LPPassManager &LPM) override {
    if (skipLoop(L)) {
      // If we have run LICM on a previous loop but now we are skipping
      // (because we've hit the opt-bisect limit), we need to clear the
      // loop alias information.
      for (auto &LTAS : LICM.getLoopToAliasSetMap())
        delete LTAS.second;
      LICM.getLoopToAliasSetMap().clear();
      return false;
    }

    auto *SE = getAnalysisIfAvailable<ScalarEvolutionWrapperPass>();
    // For the old PM, we can't use OptimizationRemarkEmitter as an analysis
    // pass.  Function analyses need to be preserved across loop transformations
    // but ORE cannot be preserved (see comment before the pass definition).
    OptimizationRemarkEmitter ORE(L->getHeader()->getParent());
    return LICM.runOnLoop(L,
                          &getAnalysis<AAResultsWrapperPass>().getAAResults(),
                          &getAnalysis<LoopInfoWrapperPass>().getLoopInfo(),
                          &getAnalysis<DominatorTreeWrapperPass>().getDomTree(),
                          &getAnalysis<TargetLibraryInfoWrapperPass>().getTLI(),
                          SE ? &SE->getSE() : nullptr, &ORE, false);
  }

  /// This transformation requires natural loop information & requires that
  /// loop preheaders be inserted into the CFG...
  ///
  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.setPreservesCFG();
    AU.addRequired<TargetLibraryInfoWrapperPass>();
    getLoopAnalysisUsage(AU);
  }

  using llvm::Pass::doFinalization;

  bool doFinalization() override {
    assert(LICM.getLoopToAliasSetMap().empty() &&
           "Didn't free loop alias sets");
    return false;
  }

private:
  LoopInvariantCodeMotion LICM;

  /// cloneBasicBlockAnalysis - Simple Analysis hook. Clone alias set info.
  void cloneBasicBlockAnalysis(BasicBlock *From, BasicBlock *To,
                               Loop *L) override;

  /// deleteAnalysisValue - Simple Analysis hook. Delete value V from alias
  /// set.
  void deleteAnalysisValue(Value *V, Loop *L) override;

  /// Simple Analysis hook. Delete loop L from alias set map.
  void deleteAnalysisLoop(Loop *L) override;
};
}

PreservedAnalyses LICMPass::run(Loop &L, LoopAnalysisManager &AM,
                                LoopStandardAnalysisResults &AR, LPMUpdater &) {
  const auto &FAM =
      AM.getResult<FunctionAnalysisManagerLoopProxy>(L, AR).getManager();
  Function *F = L.getHeader()->getParent();

  auto *ORE = FAM.getCachedResult<OptimizationRemarkEmitterAnalysis>(*F);
  // FIXME: This should probably be optional rather than required.
  if (!ORE)
    report_fatal_error("LICM: OptimizationRemarkEmitterAnalysis not "
                       "cached at a higher level");

  LoopInvariantCodeMotion LICM;
  if (!LICM.runOnLoop(&L, &AR.AA, &AR.LI, &AR.DT, &AR.TLI, &AR.SE, ORE, true))
    return PreservedAnalyses::all();

  auto PA = getLoopPassPreservedAnalyses();
  PA.preserveSet<CFGAnalyses>();
  return PA;
}

char LegacyLICMPass::ID = 0;
INITIALIZE_PASS_BEGIN(LegacyLICMPass, "licm", "Loop Invariant Code Motion",
                      false, false)
INITIALIZE_PASS_DEPENDENCY(LoopPass)
INITIALIZE_PASS_DEPENDENCY(TargetLibraryInfoWrapperPass)
INITIALIZE_PASS_END(LegacyLICMPass, "licm", "Loop Invariant Code Motion", false,
                    false)

Pass *llvm::createLICMPass() { return new LegacyLICMPass(); }

/// Hoist expressions out of the specified loop. Note, alias info for inner
/// loop is not preserved so it is not a good idea to run LICM multiple
/// times on one loop.
/// We should delete AST for inner loops in the new pass manager to avoid
/// memory leak.
///
bool LoopInvariantCodeMotion::runOnLoop(Loop *L, AliasAnalysis *AA,
                                        LoopInfo *LI, DominatorTree *DT,
                                        TargetLibraryInfo *TLI,
                                        ScalarEvolution *SE,
                                        OptimizationRemarkEmitter *ORE,
                                        bool DeleteAST) {
  bool Changed = false;

  assert(L->isLCSSAForm(*DT) && "Loop is not in LCSSA form.");

  AliasSetTracker *CurAST = collectAliasInfoForLoop(L, LI, AA);

  // Get the preheader block to move instructions into...
  BasicBlock *Preheader = L->getLoopPreheader();

  // Compute loop safety information.
  LoopSafetyInfo SafetyInfo;
  computeLoopSafetyInfo(&SafetyInfo, L);

  // We want to visit all of the instructions in this loop... that are not parts
  // of our subloops (they have already had their invariants hoisted out of
  // their loop, into this loop, so there is no need to process the BODIES of
  // the subloops).
  //
  // Traverse the body of the loop in depth first order on the dominator tree so
  // that we are guaranteed to see definitions before we see uses.  This allows
  // us to sink instructions in one pass, without iteration.  After sinking
  // instructions, we perform another pass to hoist them out of the loop.
  //
  if (L->hasDedicatedExits())
    Changed |= sinkRegion(DT->getNode(L->getHeader()), AA, LI, DT, TLI, L,
                          CurAST, &SafetyInfo, ORE);
  if (Preheader)
    Changed |= hoistRegion(DT->getNode(L->getHeader()), AA, LI, DT, TLI, L,
                           CurAST, &SafetyInfo, ORE);

  // Now that all loop invariants have been removed from the loop, promote any
  // memory references to scalars that we can.
  // Don't sink stores from loops without dedicated block exits. Exits
  // containing indirect branches are not transformed by loop simplify,
  // make sure we catch that. An additional load may be generated in the
  // preheader for SSA updater, so also avoid sinking when no preheader
  // is available.
  if (!DisablePromotion && Preheader && L->hasDedicatedExits()) {
    // Figure out the loop exits and their insertion points
    SmallVector<BasicBlock *, 8> ExitBlocks;
    L->getUniqueExitBlocks(ExitBlocks);

    // We can't insert into a catchswitch.
    bool HasCatchSwitch = llvm::any_of(ExitBlocks, [](BasicBlock *Exit) {
      return isa<CatchSwitchInst>(Exit->getTerminator());
    });

    if (!HasCatchSwitch) {
      SmallVector<Instruction *, 8> InsertPts;
      InsertPts.reserve(ExitBlocks.size());
      for (BasicBlock *ExitBlock : ExitBlocks)
        InsertPts.push_back(&*ExitBlock->getFirstInsertionPt());

      PredIteratorCache PIC;

      bool Promoted = false;

      // Loop over all of the alias sets in the tracker object.
      for (AliasSet &AS : *CurAST)
        Promoted |=
            promoteLoopAccessesToScalars(AS, ExitBlocks, InsertPts, PIC, LI, DT,
                                         TLI, L, CurAST, &SafetyInfo, ORE);

      // Once we have promoted values across the loop body we have to
      // recursively reform LCSSA as any nested loop may now have values defined
      // within the loop used in the outer loop.
      // FIXME: This is really heavy handed. It would be a bit better to use an
      // SSAUpdater strategy during promotion that was LCSSA aware and reformed
      // it as it went.
      if (Promoted)
        formLCSSARecursively(*L, *DT, LI, SE);

      Changed |= Promoted;
    }
  }

  // Check that neither this loop nor its parent have had LCSSA broken. LICM is
  // specifically moving instructions across the loop boundary and so it is
  // especially in need of sanity checking here.
  assert(L->isLCSSAForm(*DT) && "Loop not left in LCSSA form after LICM!");
  assert((!L->getParentLoop() || L->getParentLoop()->isLCSSAForm(*DT)) &&
         "Parent loop not left in LCSSA form after LICM!");

  // If this loop is nested inside of another one, save the alias information
  // for when we process the outer loop.
  if (L->getParentLoop() && !DeleteAST)
    LoopToAliasSetMap[L] = CurAST;
  else
    delete CurAST;

  if (Changed && SE)
    SE->forgetLoopDispositions(L);
  return Changed;
}

/// Walk the specified region of the CFG (defined by all blocks dominated by
/// the specified block, and that are in the current loop) in reverse depth
/// first order w.r.t the DominatorTree.  This allows us to visit uses before
/// definitions, allowing us to sink a loop body in one pass without iteration.
///
bool llvm::sinkRegion(DomTreeNode *N, AliasAnalysis *AA, LoopInfo *LI,
                      DominatorTree *DT, TargetLibraryInfo *TLI, Loop *CurLoop,
                      AliasSetTracker *CurAST, LoopSafetyInfo *SafetyInfo,
                      OptimizationRemarkEmitter *ORE) {

  // Verify inputs.
  assert(N != nullptr && AA != nullptr && LI != nullptr && DT != nullptr &&
         CurLoop != nullptr && CurAST != nullptr && SafetyInfo != nullptr &&
         "Unexpected input to sinkRegion");

  BasicBlock *BB = N->getBlock();
  // If this subregion is not in the top level loop at all, exit.
  if (!CurLoop->contains(BB))
    return false;

  // We are processing blocks in reverse dfo, so process children first.
  bool Changed = false;
  const std::vector<DomTreeNode *> &Children = N->getChildren();
  for (DomTreeNode *Child : Children)
    Changed |=
        sinkRegion(Child, AA, LI, DT, TLI, CurLoop, CurAST, SafetyInfo, ORE);

  // Only need to process the contents of this block if it is not part of a
  // subloop (which would already have been processed).
  if (inSubLoop(BB, CurLoop, LI))
    return Changed;

  for (BasicBlock::iterator II = BB->end(); II != BB->begin();) {
    Instruction &I = *--II;

    // If the instruction is dead, we would try to sink it because it isn't used
    // in the loop, instead, just delete it.
    if (isInstructionTriviallyDead(&I, TLI)) {
      DEBUG(dbgs() << "LICM deleting dead inst: " << I << '\n');
      ++II;
      CurAST->deleteValue(&I);
      I.eraseFromParent();
      Changed = true;
      continue;
    }

    // Check to see if we can sink this instruction to the exit blocks
    // of the loop.  We can do this if the all users of the instruction are
    // outside of the loop.  In this case, it doesn't even matter if the
    // operands of the instruction are loop invariant.
    //
    if (isNotUsedInLoop(I, CurLoop, SafetyInfo) &&
        canSinkOrHoistInst(I, AA, DT, CurLoop, CurAST, SafetyInfo, ORE)) {
      ++II;
      Changed |= sink(I, LI, DT, CurLoop, CurAST, SafetyInfo, ORE);
    }
  }
  return Changed;
}

/// Walk the specified region of the CFG (defined by all blocks dominated by
/// the specified block, and that are in the current loop) in depth first
/// order w.r.t the DominatorTree.  This allows us to visit definitions before
/// uses, allowing us to hoist a loop body in one pass without iteration.
///
bool llvm::hoistRegion(DomTreeNode *N, AliasAnalysis *AA, LoopInfo *LI,
                       DominatorTree *DT, TargetLibraryInfo *TLI, Loop *CurLoop,
                       AliasSetTracker *CurAST, LoopSafetyInfo *SafetyInfo,
                       OptimizationRemarkEmitter *ORE) {
  // Verify inputs.
  assert(N != nullptr && AA != nullptr && LI != nullptr && DT != nullptr &&
         CurLoop != nullptr && CurAST != nullptr && SafetyInfo != nullptr &&
         "Unexpected input to hoistRegion");

  BasicBlock *BB = N->getBlock();

  // If this subregion is not in the top level loop at all, exit.
  if (!CurLoop->contains(BB))
    return false;

  // Only need to process the contents of this block if it is not part of a
  // subloop (which would already have been processed).
  bool Changed = false;
  if (!inSubLoop(BB, CurLoop, LI))
    for (BasicBlock::iterator II = BB->begin(), E = BB->end(); II != E;) {
      Instruction &I = *II++;
      // Try constant folding this instruction.  If all the operands are
      // constants, it is technically hoistable, but it would be better to just
      // fold it.
      if (Constant *C = ConstantFoldInstruction(
              &I, I.getModule()->getDataLayout(), TLI)) {
        DEBUG(dbgs() << "LICM folding inst: " << I << "  --> " << *C << '\n');
        CurAST->copyValue(&I, C);
        I.replaceAllUsesWith(C);
        if (isInstructionTriviallyDead(&I, TLI)) {
          CurAST->deleteValue(&I);
          I.eraseFromParent();
        }
        Changed = true;
        continue;
      }

      // Attempt to remove floating point division out of the loop by converting
      // it to a reciprocal multiplication.
      if (I.getOpcode() == Instruction::FDiv &&
          CurLoop->isLoopInvariant(I.getOperand(1)) &&
          I.hasAllowReciprocal()) {
        auto Divisor = I.getOperand(1);
        auto One = llvm::ConstantFP::get(Divisor->getType(), 1.0);
        auto ReciprocalDivisor = BinaryOperator::CreateFDiv(One, Divisor);
        ReciprocalDivisor->setFastMathFlags(I.getFastMathFlags());
        ReciprocalDivisor->insertBefore(&I);

        auto Product = BinaryOperator::CreateFMul(I.getOperand(0),
                                                  ReciprocalDivisor);
        Product->setFastMathFlags(I.getFastMathFlags());
        Product->insertAfter(&I);
        I.replaceAllUsesWith(Product);
        I.eraseFromParent();

        hoist(*ReciprocalDivisor, DT, CurLoop, SafetyInfo, ORE);
        Changed = true;
        continue;
      }

      // Try hoisting the instruction out to the preheader.  We can only do this
      // if all of the operands of the instruction are loop invariant and if it
      // is safe to hoist the instruction.
      //
      if (CurLoop->hasLoopInvariantOperands(&I) &&
          canSinkOrHoistInst(I, AA, DT, CurLoop, CurAST, SafetyInfo, ORE) &&
          isSafeToExecuteUnconditionally(
              I, DT, CurLoop, SafetyInfo, ORE,
              CurLoop->getLoopPreheader()->getTerminator()))
        Changed |= hoist(I, DT, CurLoop, SafetyInfo, ORE);
    }

  const std::vector<DomTreeNode *> &Children = N->getChildren();
  for (DomTreeNode *Child : Children)
    Changed |=
        hoistRegion(Child, AA, LI, DT, TLI, CurLoop, CurAST, SafetyInfo, ORE);
  return Changed;
}

/// Computes loop safety information, checks loop body & header
/// for the possibility of may throw exception.
///
void llvm::computeLoopSafetyInfo(LoopSafetyInfo *SafetyInfo, Loop *CurLoop) {
  assert(CurLoop != nullptr && "CurLoop cant be null");
  BasicBlock *Header = CurLoop->getHeader();
  // Setting default safety values.
  SafetyInfo->MayThrow = false;
  SafetyInfo->HeaderMayThrow = false;
  // Iterate over header and compute safety info.
  for (BasicBlock::iterator I = Header->begin(), E = Header->end();
       (I != E) && !SafetyInfo->HeaderMayThrow; ++I)
    SafetyInfo->HeaderMayThrow |=
        !isGuaranteedToTransferExecutionToSuccessor(&*I);

  SafetyInfo->MayThrow = SafetyInfo->HeaderMayThrow;
  // Iterate over loop instructions and compute safety info.
  // Skip header as it has been computed and stored in HeaderMayThrow.
  // The first block in loopinfo.Blocks is guaranteed to be the header.
  assert(Header == *CurLoop->getBlocks().begin() && "First block must be header");
  for (Loop::block_iterator BB = std::next(CurLoop->block_begin()),
                            BBE = CurLoop->block_end();
       (BB != BBE) && !SafetyInfo->MayThrow; ++BB)
    for (BasicBlock::iterator I = (*BB)->begin(), E = (*BB)->end();
         (I != E) && !SafetyInfo->MayThrow; ++I)
      SafetyInfo->MayThrow |= !isGuaranteedToTransferExecutionToSuccessor(&*I);

  // Compute funclet colors if we might sink/hoist in a function with a funclet
  // personality routine.
  Function *Fn = CurLoop->getHeader()->getParent();
  if (Fn->hasPersonalityFn())
    if (Constant *PersonalityFn = Fn->getPersonalityFn())
      if (isFuncletEHPersonality(classifyEHPersonality(PersonalityFn)))
        SafetyInfo->BlockColors = colorEHFunclets(*Fn);
}

// Return true if LI is invariant within scope of the loop. LI is invariant if
// CurLoop is dominated by an invariant.start representing the same memory location
// and size as the memory location LI loads from, and also the invariant.start
// has no uses.
static bool isLoadInvariantInLoop(LoadInst *LI, DominatorTree *DT,
                                  Loop *CurLoop) {
  Value *Addr = LI->getOperand(0);
  const DataLayout &DL = LI->getModule()->getDataLayout();
  const uint32_t LocSizeInBits = DL.getTypeSizeInBits(
      cast<PointerType>(Addr->getType())->getElementType());

  // if the type is i8 addrspace(x)*, we know this is the type of
  // llvm.invariant.start operand
  auto *PtrInt8Ty = PointerType::get(Type::getInt8Ty(LI->getContext()),
                                     LI->getPointerAddressSpace());
  unsigned BitcastsVisited = 0;
  // Look through bitcasts until we reach the i8* type (this is invariant.start
  // operand type).
  while (Addr->getType() != PtrInt8Ty) {
    auto *BC = dyn_cast<BitCastInst>(Addr);
    // Avoid traversing high number of bitcast uses.
    if (++BitcastsVisited > MaxNumUsesTraversed || !BC)
      return false;
    Addr = BC->getOperand(0);
  }

  unsigned UsesVisited = 0;
  // Traverse all uses of the load operand value, to see if invariant.start is
  // one of the uses, and whether it dominates the load instruction.
  for (auto *U : Addr->users()) {
    // Avoid traversing for Load operand with high number of users.
    if (++UsesVisited > MaxNumUsesTraversed)
      return false;
    IntrinsicInst *II = dyn_cast<IntrinsicInst>(U);
    // If there are escaping uses of invariant.start instruction, the load maybe
    // non-invariant.
    if (!II || II->getIntrinsicID() != Intrinsic::invariant_start ||
        !II->use_empty())
      continue;
    unsigned InvariantSizeInBits =
        cast<ConstantInt>(II->getArgOperand(0))->getSExtValue() * 8;
    // Confirm the invariant.start location size contains the load operand size
    // in bits. Also, the invariant.start should dominate the load, and we
    // should not hoist the load out of a loop that contains this dominating
    // invariant.start.
    if (LocSizeInBits <= InvariantSizeInBits &&
        DT->properlyDominates(II->getParent(), CurLoop->getHeader()))
      return true;
  }

  return false;
}

bool llvm::canSinkOrHoistInst(Instruction &I, AAResults *AA, DominatorTree *DT,
                              Loop *CurLoop, AliasSetTracker *CurAST,
                              LoopSafetyInfo *SafetyInfo,
                              OptimizationRemarkEmitter *ORE) {
  // Loads have extra constraints we have to verify before we can hoist them.
  if (LoadInst *LI = dyn_cast<LoadInst>(&I)) {
    if (!LI->isUnordered())
      return false; // Don't hoist volatile/atomic loads!

    // Loads from constant memory are always safe to move, even if they end up
    // in the same alias set as something that ends up being modified.
    if (AA->pointsToConstantMemory(LI->getOperand(0)))
      return true;
    if (LI->getMetadata(LLVMContext::MD_invariant_load))
      return true;

    // This checks for an invariant.start dominating the load.
    if (isLoadInvariantInLoop(LI, DT, CurLoop))
      return true;

    // Don't hoist loads which have may-aliased stores in loop.
    uint64_t Size = 0;
    if (LI->getType()->isSized())
      Size = I.getModule()->getDataLayout().getTypeStoreSize(LI->getType());

    AAMDNodes AAInfo;
    LI->getAAMetadata(AAInfo);

    bool Invalidated =
        pointerInvalidatedByLoop(LI->getOperand(0), Size, AAInfo, CurAST);
    // Check loop-invariant address because this may also be a sinkable load
    // whose address is not necessarily loop-invariant.
    if (ORE && Invalidated && CurLoop->isLoopInvariant(LI->getPointerOperand()))
      ORE->emit(OptimizationRemarkMissed(
                    DEBUG_TYPE, "LoadWithLoopInvariantAddressInvalidated", LI)
                << "failed to move load with loop-invariant address "
                   "because the loop may invalidate its value");

    return !Invalidated;
  } else if (CallInst *CI = dyn_cast<CallInst>(&I)) {
    // Don't sink or hoist dbg info; it's legal, but not useful.
    if (isa<DbgInfoIntrinsic>(I))
      return false;

    // Don't sink calls which can throw.
    if (CI->mayThrow())
      return false;

    // Handle simple cases by querying alias analysis.
    FunctionModRefBehavior Behavior = AA->getModRefBehavior(CI);
    if (Behavior == FMRB_DoesNotAccessMemory)
      return true;
    if (AliasAnalysis::onlyReadsMemory(Behavior)) {
      // A readonly argmemonly function only reads from memory pointed to by
      // it's arguments with arbitrary offsets.  If we can prove there are no
      // writes to this memory in the loop, we can hoist or sink.
      if (AliasAnalysis::onlyAccessesArgPointees(Behavior)) {
        for (Value *Op : CI->arg_operands())
          if (Op->getType()->isPointerTy() &&
              pointerInvalidatedByLoop(Op, MemoryLocation::UnknownSize,
                                       AAMDNodes(), CurAST))
            return false;
        return true;
      }
      // If this call only reads from memory and there are no writes to memory
      // in the loop, we can hoist or sink the call as appropriate.
      bool FoundMod = false;
      for (AliasSet &AS : *CurAST) {
        if (!AS.isForwardingAliasSet() && AS.isMod()) {
          FoundMod = true;
          break;
        }
      }
      if (!FoundMod)
        return true;
    }

    // FIXME: This should use mod/ref information to see if we can hoist or
    // sink the call.

    return false;
  }

  // Only these instructions are hoistable/sinkable.
  if (!isa<BinaryOperator>(I) && !isa<CastInst>(I) && !isa<SelectInst>(I) &&
      !isa<GetElementPtrInst>(I) && !isa<CmpInst>(I) &&
      !isa<InsertElementInst>(I) && !isa<ExtractElementInst>(I) &&
      !isa<ShuffleVectorInst>(I) && !isa<ExtractValueInst>(I) &&
      !isa<InsertValueInst>(I))
    return false;

  // SafetyInfo is nullptr if we are checking for sinking from preheader to
  // loop body. It will be always safe as there is no speculative execution.
  if (!SafetyInfo)
    return true;

  // TODO: Plumb the context instruction through to make hoisting and sinking
  // more powerful. Hoisting of loads already works due to the special casing
  // above.
  return isSafeToExecuteUnconditionally(I, DT, CurLoop, SafetyInfo, nullptr);
}

/// Returns true if a PHINode is a trivially replaceable with an
/// Instruction.
/// This is true when all incoming values are that instruction.
/// This pattern occurs most often with LCSSA PHI nodes.
///
static bool isTriviallyReplacablePHI(const PHINode &PN, const Instruction &I) {
  for (const Value *IncValue : PN.incoming_values())
    if (IncValue != &I)
      return false;

  return true;
}

/// Return true if the only users of this instruction are outside of
/// the loop. If this is true, we can sink the instruction to the exit
/// blocks of the loop.
///
static bool isNotUsedInLoop(const Instruction &I, const Loop *CurLoop,
                            const LoopSafetyInfo *SafetyInfo) {
  const auto &BlockColors = SafetyInfo->BlockColors;
  for (const User *U : I.users()) {
    const Instruction *UI = cast<Instruction>(U);
    if (const PHINode *PN = dyn_cast<PHINode>(UI)) {
      const BasicBlock *BB = PN->getParent();
      // We cannot sink uses in catchswitches.
      if (isa<CatchSwitchInst>(BB->getTerminator()))
        return false;

      // We need to sink a callsite to a unique funclet.  Avoid sinking if the
      // phi use is too muddled.
      if (isa<CallInst>(I))
        if (!BlockColors.empty() &&
            BlockColors.find(const_cast<BasicBlock *>(BB))->second.size() != 1)
          return false;

      // A PHI node where all of the incoming values are this instruction are
      // special -- they can just be RAUW'ed with the instruction and thus
      // don't require a use in the predecessor. This is a particular important
      // special case because it is the pattern found in LCSSA form.
      if (isTriviallyReplacablePHI(*PN, I)) {
        if (CurLoop->contains(PN))
          return false;
        else
          continue;
      }

      // Otherwise, PHI node uses occur in predecessor blocks if the incoming
      // values. Check for such a use being inside the loop.
      for (unsigned i = 0, e = PN->getNumIncomingValues(); i != e; ++i)
        if (PN->getIncomingValue(i) == &I)
          if (CurLoop->contains(PN->getIncomingBlock(i)))
            return false;

      continue;
    }

    if (CurLoop->contains(UI))
      return false;
  }
  return true;
}

static Instruction *
CloneInstructionInExitBlock(Instruction &I, BasicBlock &ExitBlock, PHINode &PN,
                            const LoopInfo *LI,
                            const LoopSafetyInfo *SafetyInfo) {
  Instruction *New;
  if (auto *CI = dyn_cast<CallInst>(&I)) {
    const auto &BlockColors = SafetyInfo->BlockColors;

    // Sinking call-sites need to be handled differently from other
    // instructions.  The cloned call-site needs a funclet bundle operand
    // appropriate for it's location in the CFG.
    SmallVector<OperandBundleDef, 1> OpBundles;
    for (unsigned BundleIdx = 0, BundleEnd = CI->getNumOperandBundles();
         BundleIdx != BundleEnd; ++BundleIdx) {
      OperandBundleUse Bundle = CI->getOperandBundleAt(BundleIdx);
      if (Bundle.getTagID() == LLVMContext::OB_funclet)
        continue;

      OpBundles.emplace_back(Bundle);
    }

    if (!BlockColors.empty()) {
      const ColorVector &CV = BlockColors.find(&ExitBlock)->second;
      assert(CV.size() == 1 && "non-unique color for exit block!");
      BasicBlock *BBColor = CV.front();
      Instruction *EHPad = BBColor->getFirstNonPHI();
      if (EHPad->isEHPad())
        OpBundles.emplace_back("funclet", EHPad);
    }

    New = CallInst::Create(CI, OpBundles);
  } else {
    New = I.clone();
  }

  ExitBlock.getInstList().insert(ExitBlock.getFirstInsertionPt(), New);
  if (!I.getName().empty())
    New->setName(I.getName() + ".le");

  // Build LCSSA PHI nodes for any in-loop operands. Note that this is
  // particularly cheap because we can rip off the PHI node that we're
  // replacing for the number and blocks of the predecessors.
  // OPT: If this shows up in a profile, we can instead finish sinking all
  // invariant instructions, and then walk their operands to re-establish
  // LCSSA. That will eliminate creating PHI nodes just to nuke them when
  // sinking bottom-up.
  for (User::op_iterator OI = New->op_begin(), OE = New->op_end(); OI != OE;
       ++OI)
    if (Instruction *OInst = dyn_cast<Instruction>(*OI))
      if (Loop *OLoop = LI->getLoopFor(OInst->getParent()))
        if (!OLoop->contains(&PN)) {
          PHINode *OpPN =
              PHINode::Create(OInst->getType(), PN.getNumIncomingValues(),
                              OInst->getName() + ".lcssa", &ExitBlock.front());
          for (unsigned i = 0, e = PN.getNumIncomingValues(); i != e; ++i)
            OpPN->addIncoming(OInst, PN.getIncomingBlock(i));
          *OI = OpPN;
        }
  return New;
}

/// When an instruction is found to only be used outside of the loop, this
/// function moves it to the exit blocks and patches up SSA form as needed.
/// This method is guaranteed to remove the original instruction from its
/// position, and may either delete it or move it to outside of the loop.
///
static bool sink(Instruction &I, const LoopInfo *LI, const DominatorTree *DT,
                 const Loop *CurLoop, AliasSetTracker *CurAST,
                 const LoopSafetyInfo *SafetyInfo,
                 OptimizationRemarkEmitter *ORE) {
  DEBUG(dbgs() << "LICM sinking instruction: " << I << "\n");
  ORE->emit(OptimizationRemark(DEBUG_TYPE, "InstSunk", &I)
            << "sinking " << ore::NV("Inst", &I));
  bool Changed = false;
  if (isa<LoadInst>(I))
    ++NumMovedLoads;
  else if (isa<CallInst>(I))
    ++NumMovedCalls;
  ++NumSunk;
  Changed = true;

#ifndef NDEBUG
  SmallVector<BasicBlock *, 32> ExitBlocks;
  CurLoop->getUniqueExitBlocks(ExitBlocks);
  SmallPtrSet<BasicBlock *, 32> ExitBlockSet(ExitBlocks.begin(),
                                             ExitBlocks.end());
#endif

  // Clones of this instruction. Don't create more than one per exit block!
  SmallDenseMap<BasicBlock *, Instruction *, 32> SunkCopies;

  // If this instruction is only used outside of the loop, then all users are
  // PHI nodes in exit blocks due to LCSSA form. Just RAUW them with clones of
  // the instruction.
  while (!I.use_empty()) {
    Value::user_iterator UI = I.user_begin();
    auto *User = cast<Instruction>(*UI);
    if (!DT->isReachableFromEntry(User->getParent())) {
      User->replaceUsesOfWith(&I, UndefValue::get(I.getType()));
      continue;
    }
    // The user must be a PHI node.
    PHINode *PN = cast<PHINode>(User);

    // Surprisingly, instructions can be used outside of loops without any
    // exits.  This can only happen in PHI nodes if the incoming block is
    // unreachable.
    Use &U = UI.getUse();
    BasicBlock *BB = PN->getIncomingBlock(U);
    if (!DT->isReachableFromEntry(BB)) {
      U = UndefValue::get(I.getType());
      continue;
    }

    BasicBlock *ExitBlock = PN->getParent();
    assert(ExitBlockSet.count(ExitBlock) &&
           "The LCSSA PHI is not in an exit block!");

    Instruction *New;
    auto It = SunkCopies.find(ExitBlock);
    if (It != SunkCopies.end())
      New = It->second;
    else
      New = SunkCopies[ExitBlock] =
          CloneInstructionInExitBlock(I, *ExitBlock, *PN, LI, SafetyInfo);

    PN->replaceAllUsesWith(New);
    PN->eraseFromParent();
  }

  CurAST->deleteValue(&I);
  I.eraseFromParent();
  return Changed;
}

/// When an instruction is found to only use loop invariant operands that
/// is safe to hoist, this instruction is called to do the dirty work.
///
static bool hoist(Instruction &I, const DominatorTree *DT, const Loop *CurLoop,
                  const LoopSafetyInfo *SafetyInfo,
                  OptimizationRemarkEmitter *ORE) {
  auto *Preheader = CurLoop->getLoopPreheader();
  DEBUG(dbgs() << "LICM hoisting to " << Preheader->getName() << ": " << I
               << "\n");
  ORE->emit(OptimizationRemark(DEBUG_TYPE, "Hoisted", &I)
            << "hoisting " << ore::NV("Inst", &I));

  // Metadata can be dependent on conditions we are hoisting above.
  // Conservatively strip all metadata on the instruction unless we were
  // guaranteed to execute I if we entered the loop, in which case the metadata
  // is valid in the loop preheader.
  if (I.hasMetadataOtherThanDebugLoc() &&
      // The check on hasMetadataOtherThanDebugLoc is to prevent us from burning
      // time in isGuaranteedToExecute if we don't actually have anything to
      // drop.  It is a compile time optimization, not required for correctness.
      !isGuaranteedToExecute(I, DT, CurLoop, SafetyInfo))
    I.dropUnknownNonDebugMetadata();

  // Move the new node to the Preheader, before its terminator.
  I.moveBefore(Preheader->getTerminator());

  // Do not retain debug locations when we are moving instructions to different
  // basic blocks, because we want to avoid jumpy line tables. Calls, however,
  // need to retain their debug locs because they may be inlined.
  // FIXME: How do we retain source locations without causing poor debugging
  // behavior?
  if (!isa<CallInst>(I))
    I.setDebugLoc(DebugLoc());

  if (isa<LoadInst>(I))
    ++NumMovedLoads;
  else if (isa<CallInst>(I))
    ++NumMovedCalls;
  ++NumHoisted;
  return true;
}

/// Only sink or hoist an instruction if it is not a trapping instruction,
/// or if the instruction is known not to trap when moved to the preheader.
/// or if it is a trapping instruction and is guaranteed to execute.
static bool isSafeToExecuteUnconditionally(Instruction &Inst,
                                           const DominatorTree *DT,
                                           const Loop *CurLoop,
                                           const LoopSafetyInfo *SafetyInfo,
                                           OptimizationRemarkEmitter *ORE,
                                           const Instruction *CtxI) {
  if (isSafeToSpeculativelyExecute(&Inst, CtxI, DT))
    return true;

  bool GuaranteedToExecute =
      isGuaranteedToExecute(Inst, DT, CurLoop, SafetyInfo);

  if (!GuaranteedToExecute) {
    auto *LI = dyn_cast<LoadInst>(&Inst);
    if (LI && CurLoop->isLoopInvariant(LI->getPointerOperand()))
      ORE->emit(OptimizationRemarkMissed(
                    DEBUG_TYPE, "LoadWithLoopInvariantAddressCondExecuted", LI)
                << "failed to hoist load with loop-invariant address "
                   "because load is conditionally executed");
  }

  return GuaranteedToExecute;
}

namespace {
class LoopPromoter : public LoadAndStorePromoter {
  Value *SomePtr; // Designated pointer to store to.
  SmallPtrSetImpl<Value *> &PointerMustAliases;
  SmallVectorImpl<BasicBlock *> &LoopExitBlocks;
  SmallVectorImpl<Instruction *> &LoopInsertPts;
  PredIteratorCache &PredCache;
  AliasSetTracker &AST;
  LoopInfo &LI;
  DebugLoc DL;
  int Alignment;
  bool UnorderedAtomic;
  AAMDNodes AATags;

  Value *maybeInsertLCSSAPHI(Value *V, BasicBlock *BB) const {
    if (Instruction *I = dyn_cast<Instruction>(V))
      if (Loop *L = LI.getLoopFor(I->getParent()))
        if (!L->contains(BB)) {
          // We need to create an LCSSA PHI node for the incoming value and
          // store that.
          PHINode *PN = PHINode::Create(I->getType(), PredCache.size(BB),
                                        I->getName() + ".lcssa", &BB->front());
          for (BasicBlock *Pred : PredCache.get(BB))
            PN->addIncoming(I, Pred);
          return PN;
        }
    return V;
  }

public:
  LoopPromoter(Value *SP, ArrayRef<const Instruction *> Insts, SSAUpdater &S,
               SmallPtrSetImpl<Value *> &PMA,
               SmallVectorImpl<BasicBlock *> &LEB,
               SmallVectorImpl<Instruction *> &LIP, PredIteratorCache &PIC,
               AliasSetTracker &ast, LoopInfo &li, DebugLoc dl, int alignment,
               bool UnorderedAtomic, const AAMDNodes &AATags)
      : LoadAndStorePromoter(Insts, S), SomePtr(SP), PointerMustAliases(PMA),
        LoopExitBlocks(LEB), LoopInsertPts(LIP), PredCache(PIC), AST(ast),
        LI(li), DL(std::move(dl)), Alignment(alignment),
        UnorderedAtomic(UnorderedAtomic),AATags(AATags) {}

  bool isInstInList(Instruction *I,
                    const SmallVectorImpl<Instruction *> &) const override {
    Value *Ptr;
    if (LoadInst *LI = dyn_cast<LoadInst>(I))
      Ptr = LI->getOperand(0);
    else
      Ptr = cast<StoreInst>(I)->getPointerOperand();
    return PointerMustAliases.count(Ptr);
  }

  void doExtraRewritesBeforeFinalDeletion() const override {
    // Insert stores after in the loop exit blocks.  Each exit block gets a
    // store of the live-out values that feed them.  Since we've already told
    // the SSA updater about the defs in the loop and the preheader
    // definition, it is all set and we can start using it.
    for (unsigned i = 0, e = LoopExitBlocks.size(); i != e; ++i) {
      BasicBlock *ExitBlock = LoopExitBlocks[i];
      Value *LiveInValue = SSA.GetValueInMiddleOfBlock(ExitBlock);
      LiveInValue = maybeInsertLCSSAPHI(LiveInValue, ExitBlock);
      Value *Ptr = maybeInsertLCSSAPHI(SomePtr, ExitBlock);
      Instruction *InsertPos = LoopInsertPts[i];
      StoreInst *NewSI = new StoreInst(LiveInValue, Ptr, InsertPos);
      if (UnorderedAtomic)
        NewSI->setOrdering(AtomicOrdering::Unordered);
      NewSI->setAlignment(Alignment);
      NewSI->setDebugLoc(DL);
      if (AATags)
        NewSI->setAAMetadata(AATags);
    }
  }

  void replaceLoadWithValue(LoadInst *LI, Value *V) const override {
    // Update alias analysis.
    AST.copyValue(LI, V);
  }
  void instructionDeleted(Instruction *I) const override { AST.deleteValue(I); }
};
} // end anon namespace

/// Try to promote memory values to scalars by sinking stores out of the
/// loop and moving loads to before the loop.  We do this by looping over
/// the stores in the loop, looking for stores to Must pointers which are
/// loop invariant.
///
bool llvm::promoteLoopAccessesToScalars(
    AliasSet &AS, SmallVectorImpl<BasicBlock *> &ExitBlocks,
    SmallVectorImpl<Instruction *> &InsertPts, PredIteratorCache &PIC,
    LoopInfo *LI, DominatorTree *DT, const TargetLibraryInfo *TLI,
    Loop *CurLoop, AliasSetTracker *CurAST, LoopSafetyInfo *SafetyInfo,
    OptimizationRemarkEmitter *ORE) {
  // Verify inputs.
  assert(LI != nullptr && DT != nullptr && CurLoop != nullptr &&
         CurAST != nullptr && SafetyInfo != nullptr &&
         "Unexpected Input to promoteLoopAccessesToScalars");

  // We can promote this alias set if it has a store, if it is a "Must" alias
  // set, if the pointer is loop invariant, and if we are not eliminating any
  // volatile loads or stores.
  if (AS.isForwardingAliasSet() || !AS.isMod() || !AS.isMustAlias() ||
      AS.isVolatile() || !CurLoop->isLoopInvariant(AS.begin()->getValue()))
    return false;

  assert(!AS.empty() &&
         "Must alias set should have at least one pointer element in it!");

  Value *SomePtr = AS.begin()->getValue();
  BasicBlock *Preheader = CurLoop->getLoopPreheader();

  // It isn't safe to promote a load/store from the loop if the load/store is
  // conditional.  For example, turning:
  //
  //    for () { if (c) *P += 1; }
  //
  // into:
  //
  //    tmp = *P;  for () { if (c) tmp +=1; } *P = tmp;
  //
  // is not safe, because *P may only be valid to access if 'c' is true.
  //
  // The safety property divides into two parts:
  // p1) The memory may not be dereferenceable on entry to the loop.  In this
  //    case, we can't insert the required load in the preheader.
  // p2) The memory model does not allow us to insert a store along any dynamic
  //    path which did not originally have one.
  //
  // If at least one store is guaranteed to execute, both properties are
  // satisfied, and promotion is legal.
  //
  // This, however, is not a necessary condition. Even if no store/load is
  // guaranteed to execute, we can still establish these properties.
  // We can establish (p1) by proving that hoisting the load into the preheader
  // is safe (i.e. proving dereferenceability on all paths through the loop). We
  // can use any access within the alias set to prove dereferenceability,
  // since they're all must alias.
  // 
  // There are two ways establish (p2): 
  // a) Prove the location is thread-local. In this case the memory model
  // requirement does not apply, and stores are safe to insert.
  // b) Prove a store dominates every exit block. In this case, if an exit
  // blocks is reached, the original dynamic path would have taken us through
  // the store, so inserting a store into the exit block is safe. Note that this
  // is different from the store being guaranteed to execute. For instance,
  // if an exception is thrown on the first iteration of the loop, the original
  // store is never executed, but the exit blocks are not executed either.

  bool DereferenceableInPH = false;
  bool SafeToInsertStore = false;

  SmallVector<Instruction *, 64> LoopUses;
  SmallPtrSet<Value *, 4> PointerMustAliases;

  // We start with an alignment of one and try to find instructions that allow
  // us to prove better alignment.
  unsigned Alignment = 1;
  // Keep track of which types of access we see
  bool SawUnorderedAtomic = false; 
  bool SawNotAtomic = false;
  AAMDNodes AATags;

  const DataLayout &MDL = Preheader->getModule()->getDataLayout();

  // Do we know this object does not escape ?
  bool IsKnownNonEscapingObject = false;
  if (SafetyInfo->MayThrow) {
    // If a loop can throw, we have to insert a store along each unwind edge.
    // That said, we can't actually make the unwind edge explicit. Therefore,
    // we have to prove that the store is dead along the unwind edge.
    //
    // If the underlying object is not an alloca, nor a pointer that does not
    // escape, then we can not effectively prove that the store is dead along
    // the unwind edge. i.e. the caller of this function could have ways to
    // access the pointed object.
    Value *Object = GetUnderlyingObject(SomePtr, MDL);
    // If this is a base pointer we do not understand, simply bail.
    // We only handle alloca and return value from alloc-like fn right now.
    if (!isa<AllocaInst>(Object)) {
        if (!isAllocLikeFn(Object, TLI))
          return false;
      // If this is an alloc like fn. There are more constraints we need to verify.
      // More specifically, we must make sure that the pointer can not escape.
      //
      // NOTE: PointerMayBeCaptured is not enough as the pointer may have escaped
      // even though its not captured by the enclosing function. Standard allocation
      // functions like malloc, calloc, and operator new return values which can
      // be assumed not to have previously escaped.
      if (PointerMayBeCaptured(Object, true, true))
        return false;
      IsKnownNonEscapingObject = true;
    }
  }

  // Check that all of the pointers in the alias set have the same type.  We
  // cannot (yet) promote a memory location that is loaded and stored in
  // different sizes.  While we are at it, collect alignment and AA info.
  for (const auto &ASI : AS) {
    Value *ASIV = ASI.getValue();
    PointerMustAliases.insert(ASIV);

    // Check that all of the pointers in the alias set have the same type.  We
    // cannot (yet) promote a memory location that is loaded and stored in
    // different sizes.
    if (SomePtr->getType() != ASIV->getType())
      return false;

    for (User *U : ASIV->users()) {
      // Ignore instructions that are outside the loop.
      Instruction *UI = dyn_cast<Instruction>(U);
      if (!UI || !CurLoop->contains(UI))
        continue;

      // If there is an non-load/store instruction in the loop, we can't promote
      // it.
      if (LoadInst *Load = dyn_cast<LoadInst>(UI)) {
        assert(!Load->isVolatile() && "AST broken");
        if (!Load->isUnordered())
          return false;
        
        SawUnorderedAtomic |= Load->isAtomic();
        SawNotAtomic |= !Load->isAtomic();

        if (!DereferenceableInPH)
          DereferenceableInPH = isSafeToExecuteUnconditionally(
              *Load, DT, CurLoop, SafetyInfo, ORE, Preheader->getTerminator());
      } else if (const StoreInst *Store = dyn_cast<StoreInst>(UI)) {
        // Stores *of* the pointer are not interesting, only stores *to* the
        // pointer.
        if (UI->getOperand(1) != ASIV)
          continue;
        assert(!Store->isVolatile() && "AST broken");
        if (!Store->isUnordered())
          return false;

        SawUnorderedAtomic |= Store->isAtomic();
        SawNotAtomic |= !Store->isAtomic();

        // If the store is guaranteed to execute, both properties are satisfied.
        // We may want to check if a store is guaranteed to execute even if we
        // already know that promotion is safe, since it may have higher
        // alignment than any other guaranteed stores, in which case we can
        // raise the alignment on the promoted store.
        unsigned InstAlignment = Store->getAlignment();
        if (!InstAlignment)
          InstAlignment =
              MDL.getABITypeAlignment(Store->getValueOperand()->getType());

        if (!DereferenceableInPH || !SafeToInsertStore ||
            (InstAlignment > Alignment)) {
          if (isGuaranteedToExecute(*UI, DT, CurLoop, SafetyInfo)) {
            DereferenceableInPH = true;
            SafeToInsertStore = true;
            Alignment = std::max(Alignment, InstAlignment);
          }
        }

        // If a store dominates all exit blocks, it is safe to sink.
        // As explained above, if an exit block was executed, a dominating
        // store must have been been executed at least once, so we are not
        // introducing stores on paths that did not have them.
        // Note that this only looks at explicit exit blocks. If we ever
        // start sinking stores into unwind edges (see above), this will break.
        if (!SafeToInsertStore)
          SafeToInsertStore = llvm::all_of(ExitBlocks, [&](BasicBlock *Exit) {
            return DT->dominates(Store->getParent(), Exit);
          });

        // If the store is not guaranteed to execute, we may still get
        // deref info through it.
        if (!DereferenceableInPH) {
          DereferenceableInPH = isDereferenceableAndAlignedPointer(
              Store->getPointerOperand(), Store->getAlignment(), MDL,
              Preheader->getTerminator(), DT);
        }
      } else
        return false; // Not a load or store.

      // Merge the AA tags.
      if (LoopUses.empty()) {
        // On the first load/store, just take its AA tags.
        UI->getAAMetadata(AATags);
      } else if (AATags) {
        UI->getAAMetadata(AATags, /* Merge = */ true);
      }

      LoopUses.push_back(UI);
    }
  }

  // If we found both an unordered atomic instruction and a non-atomic memory
  // access, bail.  We can't blindly promote non-atomic to atomic since we
  // might not be able to lower the result.  We can't downgrade since that
  // would violate memory model.  Also, align 0 is an error for atomics.
  if (SawUnorderedAtomic && SawNotAtomic)
    return false;

  // If we couldn't prove we can hoist the load, bail.
  if (!DereferenceableInPH)
    return false;

  // We know we can hoist the load, but don't have a guaranteed store.
  // Check whether the location is thread-local. If it is, then we can insert
  // stores along paths which originally didn't have them without violating the
  // memory model.
  if (!SafeToInsertStore) {
    // If this is a known non-escaping object, it is safe to insert the stores.
    if (IsKnownNonEscapingObject)
      SafeToInsertStore = true;
    else {
      Value *Object = GetUnderlyingObject(SomePtr, MDL);
      SafeToInsertStore =
        (isAllocLikeFn(Object, TLI) || isa<AllocaInst>(Object)) && 
        !PointerMayBeCaptured(Object, true, true);
    }
  }

  // If we've still failed to prove we can sink the store, give up.
  if (!SafeToInsertStore)
    return false;

  // Otherwise, this is safe to promote, lets do it!
  DEBUG(dbgs() << "LICM: Promoting value stored to in loop: " << *SomePtr
               << '\n');
  ORE->emit(
      OptimizationRemark(DEBUG_TYPE, "PromoteLoopAccessesToScalar", LoopUses[0])
      << "Moving accesses to memory location out of the loop");
  ++NumPromoted;

  // Grab a debug location for the inserted loads/stores; given that the
  // inserted loads/stores have little relation to the original loads/stores,
  // this code just arbitrarily picks a location from one, since any debug
  // location is better than none.
  DebugLoc DL = LoopUses[0]->getDebugLoc();

  // We use the SSAUpdater interface to insert phi nodes as required.
  SmallVector<PHINode *, 16> NewPHIs;
  SSAUpdater SSA(&NewPHIs);
  LoopPromoter Promoter(SomePtr, LoopUses, SSA, PointerMustAliases, ExitBlocks,
                        InsertPts, PIC, *CurAST, *LI, DL, Alignment,
                        SawUnorderedAtomic, AATags);

  // Set up the preheader to have a definition of the value.  It is the live-out
  // value from the preheader that uses in the loop will use.
  LoadInst *PreheaderLoad = new LoadInst(
      SomePtr, SomePtr->getName() + ".promoted", Preheader->getTerminator());
  if (SawUnorderedAtomic)
    PreheaderLoad->setOrdering(AtomicOrdering::Unordered);
  PreheaderLoad->setAlignment(Alignment);
  PreheaderLoad->setDebugLoc(DL);
  if (AATags)
    PreheaderLoad->setAAMetadata(AATags);
  SSA.AddAvailableValue(Preheader, PreheaderLoad);

  // Rewrite all the loads in the loop and remember all the definitions from
  // stores in the loop.
  Promoter.run(LoopUses);

  // If the SSAUpdater didn't use the load in the preheader, just zap it now.
  if (PreheaderLoad->use_empty())
    PreheaderLoad->eraseFromParent();

  return true;
}

/// Returns an owning pointer to an alias set which incorporates aliasing info
/// from L and all subloops of L.
/// FIXME: In new pass manager, there is no helper function to handle loop
/// analysis such as cloneBasicBlockAnalysis, so the AST needs to be recomputed
/// from scratch for every loop. Hook up with the helper functions when
/// available in the new pass manager to avoid redundant computation.
AliasSetTracker *
LoopInvariantCodeMotion::collectAliasInfoForLoop(Loop *L, LoopInfo *LI,
                                                 AliasAnalysis *AA) {
  AliasSetTracker *CurAST = nullptr;
  SmallVector<Loop *, 4> RecomputeLoops;
  for (Loop *InnerL : L->getSubLoops()) {
    auto MapI = LoopToAliasSetMap.find(InnerL);
    // If the AST for this inner loop is missing it may have been merged into
    // some other loop's AST and then that loop unrolled, and so we need to
    // recompute it.
    if (MapI == LoopToAliasSetMap.end()) {
      RecomputeLoops.push_back(InnerL);
      continue;
    }
    AliasSetTracker *InnerAST = MapI->second;

    if (CurAST != nullptr) {
      // What if InnerLoop was modified by other passes ?
      CurAST->add(*InnerAST);

      // Once we've incorporated the inner loop's AST into ours, we don't need
      // the subloop's anymore.
      delete InnerAST;
    } else {
      CurAST = InnerAST;
    }
    LoopToAliasSetMap.erase(MapI);
  }
  if (CurAST == nullptr)
    CurAST = new AliasSetTracker(*AA);

  auto mergeLoop = [&](Loop *L) {
    // Loop over the body of this loop, looking for calls, invokes, and stores.
    for (BasicBlock *BB : L->blocks())
        CurAST->add(*BB);          // Incorporate the specified basic block
  };

  // Add everything from the sub loops that are no longer directly available.
  for (Loop *InnerL : RecomputeLoops)
    mergeLoop(InnerL);

  // And merge in this loop.
  mergeLoop(L);

  return CurAST;
}

/// Simple analysis hook. Clone alias set info.
///
void LegacyLICMPass::cloneBasicBlockAnalysis(BasicBlock *From, BasicBlock *To,
                                             Loop *L) {
  AliasSetTracker *AST = LICM.getLoopToAliasSetMap().lookup(L);
  if (!AST)
    return;

  AST->copyValue(From, To);
}

/// Simple Analysis hook. Delete value V from alias set
///
void LegacyLICMPass::deleteAnalysisValue(Value *V, Loop *L) {
  AliasSetTracker *AST = LICM.getLoopToAliasSetMap().lookup(L);
  if (!AST)
    return;

  AST->deleteValue(V);
}

/// Simple Analysis hook. Delete value L from alias set map.
///
void LegacyLICMPass::deleteAnalysisLoop(Loop *L) {
  AliasSetTracker *AST = LICM.getLoopToAliasSetMap().lookup(L);
  if (!AST)
    return;

  delete AST;
  LICM.getLoopToAliasSetMap().erase(L);
}

/// Return true if the body of this loop may store into the memory
/// location pointed to by V.
///
static bool pointerInvalidatedByLoop(Value *V, uint64_t Size,
                                     const AAMDNodes &AAInfo,
                                     AliasSetTracker *CurAST) {
  // Check to see if any of the basic blocks in CurLoop invalidate *V.
  return CurAST->getAliasSetForPointer(V, Size, AAInfo).isMod();
}

/// Little predicate that returns true if the specified basic block is in
/// a subloop of the current one, not the current one itself.
///
static bool inSubLoop(BasicBlock *BB, Loop *CurLoop, LoopInfo *LI) {
  assert(CurLoop->contains(BB) && "Only valid if BB is IN the loop");
  return LI->getLoopFor(BB) != CurLoop;
}
