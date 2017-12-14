//===- GVN.h - Eliminate redundant values and loads -------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
/// \file
/// This file provides the interface for LLVM's Global Value Numbering pass
/// which eliminates fully redundant instructions. It also does somewhat Ad-Hoc
/// PRE and dead load elimination.
///
//===----------------------------------------------------------------------===//

#ifndef LLVM_TRANSFORMS_SCALAR_GVN_H
#define LLVM_TRANSFORMS_SCALAR_GVN_H

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/MapVector.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/Analysis/AliasAnalysis.h"
#include "llvm/Analysis/AssumptionCache.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/MemoryDependenceAnalysis.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/PassManager.h"

namespace llvm {
class OptimizationRemarkEmitter;

/// A private "module" namespace for types and utilities used by GVN. These
/// are implementation details and should not be used by clients.
namespace gvn LLVM_LIBRARY_VISIBILITY {
struct AvailableValue;
struct AvailableValueInBlock;
class GVNLegacyPass;
}

/// The core GVN pass object.
///
/// FIXME: We should have a good summary of the GVN algorithm implemented by
/// this particular pass here.
class GVN : public PassInfoMixin<GVN> {
public:

  /// \brief Run the pass over the function.
  PreservedAnalyses run(Function &F, FunctionAnalysisManager &AM);

  /// This removes the specified instruction from
  /// our various maps and marks it for deletion.
  void markInstructionForDeletion(Instruction *I) {
    VN.erase(I);
    InstrsToErase.push_back(I);
  }

  DominatorTree &getDominatorTree() const { return *DT; }
  AliasAnalysis *getAliasAnalysis() const { return VN.getAliasAnalysis(); }
  MemoryDependenceResults &getMemDep() const { return *MD; }

  struct Expression;

  /// This class holds the mapping between values and value numbers.  It is used
  /// as an efficient mechanism to determine the expression-wise equivalence of
  /// two values.
  class ValueTable {
    DenseMap<Value *, uint32_t> valueNumbering;
    DenseMap<Expression, uint32_t> expressionNumbering;
    AliasAnalysis *AA;
    MemoryDependenceResults *MD;
    DominatorTree *DT;

    uint32_t nextValueNumber;

    Expression createExpr(Instruction *I);
    Expression createCmpExpr(unsigned Opcode, CmpInst::Predicate Predicate,
                             Value *LHS, Value *RHS);
    Expression createExtractvalueExpr(ExtractValueInst *EI);
    uint32_t lookupOrAddCall(CallInst *C);

  public:
    ValueTable();
    ValueTable(const ValueTable &Arg);
    ValueTable(ValueTable &&Arg);
    ~ValueTable();

    uint32_t lookupOrAdd(Value *V);
    uint32_t lookup(Value *V) const;
    uint32_t lookupOrAddCmp(unsigned Opcode, CmpInst::Predicate Pred,
                            Value *LHS, Value *RHS);
    bool exists(Value *V) const;
    void add(Value *V, uint32_t num);
    void clear();
    void erase(Value *v);
    void setAliasAnalysis(AliasAnalysis *A) { AA = A; }
    AliasAnalysis *getAliasAnalysis() const { return AA; }
    void setMemDep(MemoryDependenceResults *M) { MD = M; }
    void setDomTree(DominatorTree *D) { DT = D; }
    uint32_t getNextUnusedValueNumber() { return nextValueNumber; }
    void verifyRemoved(const Value *) const;
  };

private:
  friend class gvn::GVNLegacyPass;
  friend struct DenseMapInfo<Expression>;

  MemoryDependenceResults *MD;
  DominatorTree *DT;
  const TargetLibraryInfo *TLI;
  AssumptionCache *AC;
  SetVector<BasicBlock *> DeadBlocks;
  OptimizationRemarkEmitter *ORE;

  ValueTable VN;

  /// A mapping from value numbers to lists of Value*'s that
  /// have that value number.  Use findLeader to query it.
  struct LeaderTableEntry {
    Value *Val;
    const BasicBlock *BB;
    LeaderTableEntry *Next;
  };
  DenseMap<uint32_t, LeaderTableEntry> LeaderTable;
  BumpPtrAllocator TableAllocator;

  // Block-local map of equivalent values to their leader, does not
  // propagate to any successors. Entries added mid-block are applied
  // to the remaining instructions in the block.
  SmallMapVector<llvm::Value *, llvm::Constant *, 4> ReplaceWithConstMap;
  SmallVector<Instruction *, 8> InstrsToErase;

  typedef SmallVector<NonLocalDepResult, 64> LoadDepVect;
  typedef SmallVector<gvn::AvailableValueInBlock, 64> AvailValInBlkVect;
  typedef SmallVector<BasicBlock *, 64> UnavailBlkVect;

  bool runImpl(Function &F, AssumptionCache &RunAC, DominatorTree &RunDT,
               const TargetLibraryInfo &RunTLI, AAResults &RunAA,
               MemoryDependenceResults *RunMD, LoopInfo *LI,
               OptimizationRemarkEmitter *ORE);

  /// Push a new Value to the LeaderTable onto the list for its value number.
  void addToLeaderTable(uint32_t N, Value *V, const BasicBlock *BB) {
    LeaderTableEntry &Curr = LeaderTable[N];
    if (!Curr.Val) {
      Curr.Val = V;
      Curr.BB = BB;
      return;
    }

    LeaderTableEntry *Node = TableAllocator.Allocate<LeaderTableEntry>();
    Node->Val = V;
    Node->BB = BB;
    Node->Next = Curr.Next;
    Curr.Next = Node;
  }

  /// Scan the list of values corresponding to a given
  /// value number, and remove the given instruction if encountered.
  void removeFromLeaderTable(uint32_t N, Instruction *I, BasicBlock *BB) {
    LeaderTableEntry *Prev = nullptr;
    LeaderTableEntry *Curr = &LeaderTable[N];

    while (Curr && (Curr->Val != I || Curr->BB != BB)) {
      Prev = Curr;
      Curr = Curr->Next;
    }

    if (!Curr)
      return;

    if (Prev) {
      Prev->Next = Curr->Next;
    } else {
      if (!Curr->Next) {
        Curr->Val = nullptr;
        Curr->BB = nullptr;
      } else {
        LeaderTableEntry *Next = Curr->Next;
        Curr->Val = Next->Val;
        Curr->BB = Next->BB;
        Curr->Next = Next->Next;
      }
    }
  }

  // List of critical edges to be split between iterations.
  SmallVector<std::pair<TerminatorInst *, unsigned>, 4> toSplit;

  // Helper functions of redundant load elimination
  bool processLoad(LoadInst *L);
  bool processNonLocalLoad(LoadInst *L);
  bool processAssumeIntrinsic(IntrinsicInst *II);
  /// Given a local dependency (Def or Clobber) determine if a value is
  /// available for the load.  Returns true if an value is known to be
  /// available and populates Res.  Returns false otherwise.
  bool AnalyzeLoadAvailability(LoadInst *LI, MemDepResult DepInfo,
                               Value *Address, gvn::AvailableValue &Res);
  /// Given a list of non-local dependencies, determine if a value is
  /// available for the load in each specified block.  If it is, add it to
  /// ValuesPerBlock.  If not, add it to UnavailableBlocks.
  void AnalyzeLoadAvailability(LoadInst *LI, LoadDepVect &Deps,
                               AvailValInBlkVect &ValuesPerBlock,
                               UnavailBlkVect &UnavailableBlocks);
  bool PerformLoadPRE(LoadInst *LI, AvailValInBlkVect &ValuesPerBlock,
                      UnavailBlkVect &UnavailableBlocks);

  // Other helper routines
  bool processInstruction(Instruction *I);
  bool processBlock(BasicBlock *BB);
  void dump(DenseMap<uint32_t, Value *> &d) const;
  bool iterateOnFunction(Function &F);
  bool performPRE(Function &F);
  bool performScalarPRE(Instruction *I);
  bool performScalarPREInsertion(Instruction *Instr, BasicBlock *Pred,
                                 unsigned int ValNo);
  Value *findLeader(const BasicBlock *BB, uint32_t num);
  void cleanupGlobalSets();
  void verifyRemoved(const Instruction *I) const;
  bool splitCriticalEdges();
  BasicBlock *splitCriticalEdges(BasicBlock *Pred, BasicBlock *Succ);
  bool replaceOperandsWithConsts(Instruction *I) const;
  bool propagateEquality(Value *LHS, Value *RHS, const BasicBlockEdge &Root,
                         bool DominatesByEdge);
  bool processFoldableCondBr(BranchInst *BI);
  void addDeadBlock(BasicBlock *BB);
  void assignValNumForDeadCode();
};

/// Create a legacy GVN pass. This also allows parameterizing whether or not
/// loads are eliminated by the pass.
FunctionPass *createGVNPass(bool NoLoads = false);

/// \brief A simple and fast domtree-based GVN pass to hoist common expressions
/// from sibling branches.
struct GVNHoistPass : PassInfoMixin<GVNHoistPass> {
  /// \brief Run the pass over the function.
  PreservedAnalyses run(Function &F, FunctionAnalysisManager &AM);
};
/// \brief Uses an "inverted" value numbering to decide the similarity of
/// expressions and sinks similar expressions into successors.
struct GVNSinkPass : PassInfoMixin<GVNSinkPass> {
  /// \brief Run the pass over the function.
  PreservedAnalyses run(Function &F, FunctionAnalysisManager &AM);
};
}

#endif
