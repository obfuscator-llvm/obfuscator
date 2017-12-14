//===-- CoalesceBranches.cpp - Coalesce blocks with the same condition ---===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
///
/// \file
/// Coalesce basic blocks guarded by the same branch condition into a single
/// basic block.
///
//===----------------------------------------------------------------------===//

#include "llvm/ADT/BitVector.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/CodeGen/MachineDominators.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachinePostDominators.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/CodeGen/Passes.h"
#include "llvm/Support/Debug.h"
#include "llvm/Target/TargetFrameLowering.h"
#include "llvm/Target/TargetInstrInfo.h"
#include "llvm/Target/TargetSubtargetInfo.h"

using namespace llvm;

#define DEBUG_TYPE "branch-coalescing"

static cl::opt<cl::boolOrDefault>
    EnableBranchCoalescing("enable-branch-coalesce", cl::Hidden,
                           cl::desc("enable coalescing of duplicate branches"));

STATISTIC(NumBlocksCoalesced, "Number of blocks coalesced");
STATISTIC(NumPHINotMoved, "Number of PHI Nodes that cannot be merged");
STATISTIC(NumBlocksNotCoalesced, "Number of blocks not coalesced");

//===----------------------------------------------------------------------===//
//                               BranchCoalescing
//===----------------------------------------------------------------------===//
///
/// Improve scheduling by coalescing branches that depend on the same condition.
/// This pass looks for blocks that are guarded by the same branch condition
/// and attempts to merge the blocks together. Such opportunities arise from
/// the expansion of select statements in the IR.
///
/// For example, consider the following LLVM IR:
///
/// %test = icmp eq i32 %x 0
/// %tmp1 = select i1 %test, double %a, double 2.000000e-03
/// %tmp2 = select i1 %test, double %b, double 5.000000e-03
///
/// This IR expands to the following machine code on PowerPC:
///
/// BB#0: derived from LLVM BB %entry
///    Live Ins: %F1 %F3 %X6
///        <SNIP1>
///        %vreg0<def> = COPY %F1; F8RC:%vreg0
///        %vreg5<def> = CMPLWI %vreg4<kill>, 0; CRRC:%vreg5 GPRC:%vreg4
///        %vreg8<def> = LXSDX %ZERO8, %vreg7<kill>, %RM<imp-use>;
///                    mem:LD8[ConstantPool] F8RC:%vreg8 G8RC:%vreg7
///        BCC 76, %vreg5, <BB#2>; CRRC:%vreg5
///    Successors according to CFG: BB#1(?%) BB#2(?%)
///
/// BB#1: derived from LLVM BB %entry
///    Predecessors according to CFG: BB#0
///    Successors according to CFG: BB#2(?%)
///
/// BB#2: derived from LLVM BB %entry
///    Predecessors according to CFG: BB#0 BB#1
///        %vreg9<def> = PHI %vreg8, <BB#1>, %vreg0, <BB#0>;
///                    F8RC:%vreg9,%vreg8,%vreg0
///        <SNIP2>
///        BCC 76, %vreg5, <BB#4>; CRRC:%vreg5
///    Successors according to CFG: BB#3(?%) BB#4(?%)
///
/// BB#3: derived from LLVM BB %entry
///    Predecessors according to CFG: BB#2
///    Successors according to CFG: BB#4(?%)
///
/// BB#4: derived from LLVM BB %entry
///    Predecessors according to CFG: BB#2 BB#3
///        %vreg13<def> = PHI %vreg12, <BB#3>, %vreg2, <BB#2>;
///                     F8RC:%vreg13,%vreg12,%vreg2
///        <SNIP3>
///        BLR8 %LR8<imp-use>, %RM<imp-use>, %F1<imp-use>
///
/// When this pattern is detected, branch coalescing will try to collapse
/// it by moving code in BB#2 to BB#0 and/or BB#4 and removing BB#3.
///
/// If all conditions are meet, IR should collapse to:
///
/// BB#0: derived from LLVM BB %entry
///    Live Ins: %F1 %F3 %X6
///        <SNIP1>
///        %vreg0<def> = COPY %F1; F8RC:%vreg0
///        %vreg5<def> = CMPLWI %vreg4<kill>, 0; CRRC:%vreg5 GPRC:%vreg4
///        %vreg8<def> = LXSDX %ZERO8, %vreg7<kill>, %RM<imp-use>;
///                     mem:LD8[ConstantPool] F8RC:%vreg8 G8RC:%vreg7
///        <SNIP2>
///        BCC 76, %vreg5, <BB#4>; CRRC:%vreg5
///    Successors according to CFG: BB#1(0x2aaaaaaa / 0x80000000 = 33.33%)
///      BB#4(0x55555554 / 0x80000000 = 66.67%)
///
/// BB#1: derived from LLVM BB %entry
///    Predecessors according to CFG: BB#0
///    Successors according to CFG: BB#4(0x40000000 / 0x80000000 = 50.00%)
///
/// BB#4: derived from LLVM BB %entry
///    Predecessors according to CFG: BB#0 BB#1
///        %vreg9<def> = PHI %vreg8, <BB#1>, %vreg0, <BB#0>;
///                    F8RC:%vreg9,%vreg8,%vreg0
///        %vreg13<def> = PHI %vreg12, <BB#1>, %vreg2, <BB#0>;
///                     F8RC:%vreg13,%vreg12,%vreg2
///        <SNIP3>
///        BLR8 %LR8<imp-use>, %RM<imp-use>, %F1<imp-use>
///
/// Branch Coalescing does not split blocks, it moves everything in the same
/// direction ensuring it does not break use/definition semantics.
///
/// PHI nodes and its corresponding use instructions are moved to its successor
/// block if there are no uses within the successor block PHI nodes.  PHI
/// node ordering cannot be assumed.
///
/// Non-PHI can be moved up to the predecessor basic block or down to the
/// successor basic block following any PHI instructions. Whether it moves
/// up or down depends on whether the register(s) defined in the instructions
/// are used in current block or in any PHI instructions at the beginning of
/// the successor block.

namespace {

class BranchCoalescing : public MachineFunctionPass {
  struct CoalescingCandidateInfo {
    MachineBasicBlock *BranchBlock;       // Block containing the branch
    MachineBasicBlock *BranchTargetBlock; // Block branched to
    MachineBasicBlock *FallThroughBlock;  // Fall-through if branch not taken
    SmallVector<MachineOperand, 4> Cond;
    bool MustMoveDown;
    bool MustMoveUp;

    CoalescingCandidateInfo();
    void clear();
  };

  MachineDominatorTree *MDT;
  MachinePostDominatorTree *MPDT;
  const TargetInstrInfo *TII;
  MachineRegisterInfo *MRI;

  void initialize(MachineFunction &F);
  bool canCoalesceBranch(CoalescingCandidateInfo &Cand);
  bool identicalOperands(ArrayRef<MachineOperand> OperandList1,
                         ArrayRef<MachineOperand> OperandList2) const;
  bool validateCandidates(CoalescingCandidateInfo &SourceRegion,
                          CoalescingCandidateInfo &TargetRegion) const;

  static bool isBranchCoalescingEnabled() {
    return EnableBranchCoalescing == cl::BOU_TRUE;
  }

public:
  static char ID;

  BranchCoalescing() : MachineFunctionPass(ID) {
    initializeBranchCoalescingPass(*PassRegistry::getPassRegistry());
  }

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.addRequired<MachineDominatorTree>();
    AU.addRequired<MachinePostDominatorTree>();
    MachineFunctionPass::getAnalysisUsage(AU);
  }

  StringRef getPassName() const override { return "Branch Coalescing"; }

  bool mergeCandidates(CoalescingCandidateInfo &SourceRegion,
                       CoalescingCandidateInfo &TargetRegion);
  bool canMoveToBeginning(const MachineInstr &MI,
                          const MachineBasicBlock &MBB) const;
  bool canMoveToEnd(const MachineInstr &MI,
                    const MachineBasicBlock &MBB) const;
  bool canMerge(CoalescingCandidateInfo &SourceRegion,
                CoalescingCandidateInfo &TargetRegion) const;
  void moveAndUpdatePHIs(MachineBasicBlock *SourceRegionMBB,
                         MachineBasicBlock *TargetRegionMBB);
  bool runOnMachineFunction(MachineFunction &MF) override;
};
} // End anonymous namespace.

char BranchCoalescing::ID = 0;
char &llvm::BranchCoalescingID = BranchCoalescing::ID;

INITIALIZE_PASS_BEGIN(BranchCoalescing, DEBUG_TYPE,
                      "Branch Coalescing", false, false)
INITIALIZE_PASS_DEPENDENCY(MachineDominatorTree)
INITIALIZE_PASS_DEPENDENCY(MachinePostDominatorTree)
INITIALIZE_PASS_END(BranchCoalescing, DEBUG_TYPE, "Branch Coalescing",
                    false, false)

BranchCoalescing::CoalescingCandidateInfo::CoalescingCandidateInfo()
    : BranchBlock(nullptr), BranchTargetBlock(nullptr),
      FallThroughBlock(nullptr), MustMoveDown(false), MustMoveUp(false) {}

void BranchCoalescing::CoalescingCandidateInfo::clear() {
  BranchBlock = nullptr;
  BranchTargetBlock = nullptr;
  FallThroughBlock = nullptr;
  Cond.clear();
  MustMoveDown = false;
  MustMoveUp = false;
}

void BranchCoalescing::initialize(MachineFunction &MF) {
  MDT = &getAnalysis<MachineDominatorTree>();
  MPDT = &getAnalysis<MachinePostDominatorTree>();
  TII = MF.getSubtarget().getInstrInfo();
  MRI = &MF.getRegInfo();
}

///
/// Analyze the branch statement to determine if it can be coalesced. This
/// method analyses the branch statement for the given candidate to determine
/// if it can be coalesced. If the branch can be coalesced, then the
/// BranchTargetBlock and the FallThroughBlock are recorded in the specified
/// Candidate.
///
///\param[in,out] Cand The coalescing candidate to analyze
///\return true if and only if the branch can be coalesced, false otherwise
///
bool BranchCoalescing::canCoalesceBranch(CoalescingCandidateInfo &Cand) {
  DEBUG(dbgs() << "Determine if branch block " << Cand.BranchBlock->getNumber()
               << " can be coalesced:");
  MachineBasicBlock *FalseMBB = nullptr;

  if (TII->analyzeBranch(*Cand.BranchBlock, Cand.BranchTargetBlock, FalseMBB,
                         Cand.Cond)) {
    DEBUG(dbgs() << "TII unable to Analyze Branch - skip\n");
    return false;
  }

  for (auto &I : Cand.BranchBlock->terminators()) {
    DEBUG(dbgs() << "Looking at terminator : " << I << "\n");
    if (!I.isBranch())
      continue;

    if (I.getNumOperands() != I.getNumExplicitOperands()) {
      DEBUG(dbgs() << "Terminator contains implicit operands - skip : " << I
                   << "\n");
      return false;
    }
  }

  if (Cand.BranchBlock->isEHPad() || Cand.BranchBlock->hasEHPadSuccessor()) {
    DEBUG(dbgs() << "EH Pad - skip\n");
    return false;
  }

  // For now only consider triangles (i.e, BranchTargetBlock is set,
  // FalseMBB is null, and BranchTargetBlock is a successor to BranchBlock)
  if (!Cand.BranchTargetBlock || FalseMBB ||
      !Cand.BranchBlock->isSuccessor(Cand.BranchTargetBlock)) {
    DEBUG(dbgs() << "Does not form a triangle - skip\n");
    return false;
  }

  // Ensure there are only two successors
  if (Cand.BranchBlock->succ_size() != 2) {
    DEBUG(dbgs() << "Does not have 2 successors - skip\n");
    return false;
  }

  // Sanity check - the block must be able to fall through
  assert(Cand.BranchBlock->canFallThrough() &&
         "Expecting the block to fall through!");

  // We have already ensured there are exactly two successors to
  // BranchBlock and that BranchTargetBlock is a successor to BranchBlock.
  // Ensure the single fall though block is empty.
  MachineBasicBlock *Succ =
    (*Cand.BranchBlock->succ_begin() == Cand.BranchTargetBlock)
    ? *Cand.BranchBlock->succ_rbegin()
    : *Cand.BranchBlock->succ_begin();

  assert(Succ && "Expecting a valid fall-through block\n");

  if (!Succ->empty()) {
      DEBUG(dbgs() << "Fall-through block contains code -- skip\n");
      return false;
  }

  if (!Succ->isSuccessor(Cand.BranchTargetBlock)) {
      DEBUG(dbgs()
            << "Successor of fall through block is not branch taken block\n");
      return false;
  }

  Cand.FallThroughBlock = Succ;
  DEBUG(dbgs() << "Valid Candidate\n");
  return true;
}

///
/// Determine if the two operand lists are identical
///
/// \param[in] OpList1 operand list
/// \param[in] OpList2 operand list
/// \return true if and only if the operands lists are identical
///
bool BranchCoalescing::identicalOperands(
    ArrayRef<MachineOperand> OpList1, ArrayRef<MachineOperand> OpList2) const {

  if (OpList1.size() != OpList2.size()) {
    DEBUG(dbgs() << "Operand list is different size\n");
    return false;
  }

  for (unsigned i = 0; i < OpList1.size(); ++i) {
    const MachineOperand &Op1 = OpList1[i];
    const MachineOperand &Op2 = OpList2[i];

    DEBUG(dbgs() << "Op1: " << Op1 << "\n"
                 << "Op2: " << Op2 << "\n");

    if (Op1.isIdenticalTo(Op2)) {
      DEBUG(dbgs() << "Op1 and Op2 are identical!\n");
      continue;
    }

    // If the operands are not identical, but are registers, check to see if the
    // definition of the register produces the same value. If they produce the
    // same value, consider them to be identical.
    if (Op1.isReg() && Op2.isReg() &&
        TargetRegisterInfo::isVirtualRegister(Op1.getReg()) &&
        TargetRegisterInfo::isVirtualRegister(Op2.getReg())) {
      MachineInstr *Op1Def = MRI->getVRegDef(Op1.getReg());
      MachineInstr *Op2Def = MRI->getVRegDef(Op2.getReg());
      if (TII->produceSameValue(*Op1Def, *Op2Def, MRI)) {
        DEBUG(dbgs() << "Op1Def: " << *Op1Def << " and " << *Op2Def
                     << " produce the same value!\n");
      } else {
        DEBUG(dbgs() << "Operands produce different values\n");
        return false;
      }
    } else {
      DEBUG(dbgs() << "The operands are not provably identical.\n");
      return false;
    }
  }
  return true;
}

///
/// Moves ALL PHI instructions in SourceMBB to beginning of TargetMBB
/// and update them to refer to the new block.  PHI node ordering
/// cannot be assumed so it does not matter where the PHI instructions
/// are moved to in TargetMBB.
///
/// \param[in] SourceMBB block to move PHI instructions from
/// \param[in] TargetMBB block to move PHI instructions to
///
void BranchCoalescing::moveAndUpdatePHIs(MachineBasicBlock *SourceMBB,
                                         MachineBasicBlock *TargetMBB) {

  MachineBasicBlock::iterator MI = SourceMBB->begin();
  MachineBasicBlock::iterator ME = SourceMBB->getFirstNonPHI();

  if (MI == ME) {
    DEBUG(dbgs() << "SourceMBB contains no PHI instructions.\n");
    return;
  }

  // Update all PHI instructions in SourceMBB and move to top of TargetMBB
  for (MachineBasicBlock::iterator Iter = MI; Iter != ME; Iter++) {
    MachineInstr &PHIInst = *Iter;
    for (unsigned i = 2, e = PHIInst.getNumOperands() + 1; i != e; i += 2) {
      MachineOperand &MO = PHIInst.getOperand(i);
      if (MO.getMBB() == SourceMBB)
        MO.setMBB(TargetMBB);
    }
  }
  TargetMBB->splice(TargetMBB->begin(), SourceMBB, MI, ME);
}

///
/// This function checks if MI can be moved to the beginning of the TargetMBB
/// following PHI instructions. A MI instruction can be moved to beginning of
/// the TargetMBB if there are no uses of it within the TargetMBB PHI nodes.
///
/// \param[in] MI the machine instruction to move.
/// \param[in] TargetMBB the machine basic block to move to
/// \return true if it is safe to move MI to beginning of TargetMBB,
///         false otherwise.
///
bool BranchCoalescing::canMoveToBeginning(const MachineInstr &MI,
                                          const MachineBasicBlock &TargetMBB
                                          ) const {

  DEBUG(dbgs() << "Checking if " << MI << " can move to beginning of "
        << TargetMBB.getNumber() << "\n");

  for (auto &Def : MI.defs()) { // Looking at Def
    for (auto &Use : MRI->use_instructions(Def.getReg())) {
      if (Use.isPHI() && Use.getParent() == &TargetMBB) {
        DEBUG(dbgs() << "    *** used in a PHI -- cannot move ***\n");
       return false;
      }
    }
  }

  DEBUG(dbgs() << "  Safe to move to the beginning.\n");
  return true;
}

///
/// This function checks if MI can be moved to the end of the TargetMBB,
/// immediately before the first terminator.  A MI instruction can be moved
/// to then end of the TargetMBB if no PHI node defines what MI uses within
/// it's own MBB.
///
/// \param[in] MI the machine instruction to move.
/// \param[in] TargetMBB the machine basic block to move to
/// \return true if it is safe to move MI to end of TargetMBB,
///         false otherwise.
///
bool BranchCoalescing::canMoveToEnd(const MachineInstr &MI,
                                    const MachineBasicBlock &TargetMBB
                                    ) const {

  DEBUG(dbgs() << "Checking if " << MI << " can move to end of "
        << TargetMBB.getNumber() << "\n");

  for (auto &Use : MI.uses()) {
    if (Use.isReg() && TargetRegisterInfo::isVirtualRegister(Use.getReg())) {
      MachineInstr *DefInst = MRI->getVRegDef(Use.getReg());
      if (DefInst->isPHI() && DefInst->getParent() == MI.getParent()) {
        DEBUG(dbgs() << "    *** Cannot move this instruction ***\n");
        return false;
      } else {
        DEBUG(dbgs() << "    *** def is in another block -- safe to move!\n");
      }
    }
  }

  DEBUG(dbgs() << "  Safe to move to the end.\n");
  return true;
}

///
/// This method checks to ensure the two coalescing candidates follows the
/// expected pattern required for coalescing.
///
/// \param[in] SourceRegion The candidate to move statements from
/// \param[in] TargetRegion The candidate to move statements to
/// \return true if all instructions in SourceRegion.BranchBlock can be merged
/// into a block in TargetRegion; false otherwise.
///
bool BranchCoalescing::validateCandidates(
    CoalescingCandidateInfo &SourceRegion,
    CoalescingCandidateInfo &TargetRegion) const {

  if (TargetRegion.BranchTargetBlock != SourceRegion.BranchBlock)
    llvm_unreachable("Expecting SourceRegion to immediately follow TargetRegion");
  else if (!MDT->dominates(TargetRegion.BranchBlock, SourceRegion.BranchBlock))
    llvm_unreachable("Expecting TargetRegion to dominate SourceRegion");
  else if (!MPDT->dominates(SourceRegion.BranchBlock, TargetRegion.BranchBlock))
    llvm_unreachable("Expecting SourceRegion to post-dominate TargetRegion");
  else if (!TargetRegion.FallThroughBlock->empty() ||
           !SourceRegion.FallThroughBlock->empty())
    llvm_unreachable("Expecting fall-through blocks to be empty");

  return true;
}

///
/// This method determines whether the two coalescing candidates can be merged.
/// In order to be merged, all instructions must be able to
///   1. Move to the beginning of the SourceRegion.BranchTargetBlock;
///   2. Move to the end of the TargetRegion.BranchBlock.
/// Merging involves moving the instructions in the
/// TargetRegion.BranchTargetBlock (also SourceRegion.BranchBlock).
///
/// This function first try to move instructions from the
/// TargetRegion.BranchTargetBlock down, to the beginning of the
/// SourceRegion.BranchTargetBlock. This is not possible if any register defined
/// in TargetRegion.BranchTargetBlock is used in a PHI node in the
/// SourceRegion.BranchTargetBlock. In this case, check whether the statement
/// can be moved up, to the end of the TargetRegion.BranchBlock (immediately
/// before the branch statement). If it cannot move, then these blocks cannot
/// be merged.
///
/// Note that there is no analysis for moving instructions past the fall-through
/// blocks because they are confirmed to be empty. An assert is thrown if they
/// are not.
///
/// \param[in] SourceRegion The candidate to move statements from
/// \param[in] TargetRegion The candidate to move statements to
/// \return true if all instructions in SourceRegion.BranchBlock can be merged
///         into a block in TargetRegion, false otherwise.
///
bool BranchCoalescing::canMerge(CoalescingCandidateInfo &SourceRegion,
                                CoalescingCandidateInfo &TargetRegion) const {
  if (!validateCandidates(SourceRegion, TargetRegion))
    return false;

  // Walk through PHI nodes first and see if they force the merge into the
  // SourceRegion.BranchTargetBlock.
  for (MachineBasicBlock::iterator
           I = SourceRegion.BranchBlock->instr_begin(),
           E = SourceRegion.BranchBlock->getFirstNonPHI();
       I != E; ++I) {
    for (auto &Def : I->defs())
      for (auto &Use : MRI->use_instructions(Def.getReg())) {
        if (Use.isPHI() && Use.getParent() == SourceRegion.BranchTargetBlock) {
          DEBUG(dbgs() << "PHI " << *I << " defines register used in another "
                          "PHI within branch target block -- can't merge\n");
          NumPHINotMoved++;
          return false;
        }
        if (Use.getParent() == SourceRegion.BranchBlock) {
          DEBUG(dbgs() << "PHI " << *I
                       << " defines register used in this "
                          "block -- all must move down\n");
          SourceRegion.MustMoveDown = true;
        }
      }
  }

  // Walk through the MI to see if they should be merged into
  // TargetRegion.BranchBlock (up) or SourceRegion.BranchTargetBlock (down)
  for (MachineBasicBlock::iterator
           I = SourceRegion.BranchBlock->getFirstNonPHI(),
           E = SourceRegion.BranchBlock->end();
       I != E; ++I) {
    if (!canMoveToBeginning(*I, *SourceRegion.BranchTargetBlock)) {
      DEBUG(dbgs() << "Instruction " << *I
                   << " cannot move down - must move up!\n");
      SourceRegion.MustMoveUp = true;
    }
    if (!canMoveToEnd(*I, *TargetRegion.BranchBlock)) {
      DEBUG(dbgs() << "Instruction " << *I
                   << " cannot move up - must move down!\n");
      SourceRegion.MustMoveDown = true;
    }
  }

  return (SourceRegion.MustMoveUp && SourceRegion.MustMoveDown) ? false : true;
}

/// Merge the instructions from SourceRegion.BranchBlock,
/// SourceRegion.BranchTargetBlock, and SourceRegion.FallThroughBlock into
/// TargetRegion.BranchBlock, TargetRegion.BranchTargetBlock and
/// TargetRegion.FallThroughBlock respectively.
///
/// The successors for blocks in TargetRegion will be updated to use the
/// successors from blocks in SourceRegion. Finally, the blocks in SourceRegion
/// will be removed from the function.
///
/// A region consists of a BranchBlock, a FallThroughBlock, and a
/// BranchTargetBlock. Branch coalesce works on patterns where the
/// TargetRegion's BranchTargetBlock must also be the SourceRegions's
/// BranchBlock.
///
///  Before mergeCandidates:
///
///  +---------------------------+
///  |  TargetRegion.BranchBlock |
///  +---------------------------+
///     /        |
///    /   +--------------------------------+
///   |    |  TargetRegion.FallThroughBlock |
///    \   +--------------------------------+
///     \        |
///  +----------------------------------+
///  |  TargetRegion.BranchTargetBlock  |
///  |  SourceRegion.BranchBlock        |
///  +----------------------------------+
///     /        |
///    /   +--------------------------------+
///   |    |  SourceRegion.FallThroughBlock |
///    \   +--------------------------------+
///     \        |
///  +----------------------------------+
///  |  SourceRegion.BranchTargetBlock  |
///  +----------------------------------+
///
///  After mergeCandidates:
///
///  +-----------------------------+
///  |  TargetRegion.BranchBlock   |
///  |  SourceRegion.BranchBlock   |
///  +-----------------------------+
///     /        |
///    /   +---------------------------------+
///   |    |  TargetRegion.FallThroughBlock  |
///   |    |  SourceRegion.FallThroughBlock  |
///    \   +---------------------------------+
///     \        |
///  +----------------------------------+
///  |  SourceRegion.BranchTargetBlock  |
///  +----------------------------------+
///
/// \param[in] SourceRegion The candidate to move blocks from
/// \param[in] TargetRegion The candidate to move blocks to
///
bool BranchCoalescing::mergeCandidates(CoalescingCandidateInfo &SourceRegion,
                                       CoalescingCandidateInfo &TargetRegion) {

  if (SourceRegion.MustMoveUp && SourceRegion.MustMoveDown) {
    llvm_unreachable("Cannot have both MustMoveDown and MustMoveUp set!");
    return false;
  }

  if (!validateCandidates(SourceRegion, TargetRegion))
    return false;

  // Start the merging process by first handling the BranchBlock.
  // Move any PHIs in SourceRegion.BranchBlock down to the branch-taken block
  moveAndUpdatePHIs(SourceRegion.BranchBlock, SourceRegion.BranchTargetBlock);

  // Move remaining instructions in SourceRegion.BranchBlock into
  // TargetRegion.BranchBlock
  MachineBasicBlock::iterator firstInstr =
      SourceRegion.BranchBlock->getFirstNonPHI();
  MachineBasicBlock::iterator lastInstr =
      SourceRegion.BranchBlock->getFirstTerminator();

  MachineBasicBlock *Source = SourceRegion.MustMoveDown
                                  ? SourceRegion.BranchTargetBlock
                                  : TargetRegion.BranchBlock;

  MachineBasicBlock::iterator Target =
      SourceRegion.MustMoveDown
          ? SourceRegion.BranchTargetBlock->getFirstNonPHI()
          : TargetRegion.BranchBlock->getFirstTerminator();

  Source->splice(Target, SourceRegion.BranchBlock, firstInstr, lastInstr);

  // Once PHI and instructions have been moved we need to clean up the
  // control flow.

  // Remove SourceRegion.FallThroughBlock before transferring successors of
  // SourceRegion.BranchBlock to TargetRegion.BranchBlock.
  SourceRegion.BranchBlock->removeSuccessor(SourceRegion.FallThroughBlock);
  TargetRegion.BranchBlock->transferSuccessorsAndUpdatePHIs(
      SourceRegion.BranchBlock);
  // Update branch in TargetRegion.BranchBlock to jump to
  // SourceRegion.BranchTargetBlock
  // In this case, TargetRegion.BranchTargetBlock == SourceRegion.BranchBlock.
  TargetRegion.BranchBlock->ReplaceUsesOfBlockWith(
      SourceRegion.BranchBlock, SourceRegion.BranchTargetBlock);
  // Remove the branch statement(s) in SourceRegion.BranchBlock
  MachineBasicBlock::iterator I =
      SourceRegion.BranchBlock->terminators().begin();
  while (I != SourceRegion.BranchBlock->terminators().end()) {
    MachineInstr &CurrInst = *I;
    ++I;
    if (CurrInst.isBranch())
      CurrInst.eraseFromParent();
  }

  // Fall-through block should be empty since this is part of the condition
  // to coalesce the branches.
  assert(TargetRegion.FallThroughBlock->empty() &&
         "FallThroughBlocks should be empty!");

  // Transfer successor information and move PHIs down to the
  // branch-taken block.
  TargetRegion.FallThroughBlock->transferSuccessorsAndUpdatePHIs(
      SourceRegion.FallThroughBlock);
  TargetRegion.FallThroughBlock->removeSuccessor(SourceRegion.BranchBlock);

  // Remove the blocks from the function.
  assert(SourceRegion.BranchBlock->empty() &&
         "Expecting branch block to be empty!");
  SourceRegion.BranchBlock->eraseFromParent();

  assert(SourceRegion.FallThroughBlock->empty() &&
         "Expecting fall-through block to be empty!\n");
  SourceRegion.FallThroughBlock->eraseFromParent();

  NumBlocksCoalesced++;
  return true;
}

bool BranchCoalescing::runOnMachineFunction(MachineFunction &MF) {

  if (skipFunction(*MF.getFunction()) || MF.empty() ||
      !isBranchCoalescingEnabled())
    return false;

  bool didSomething = false;

  DEBUG(dbgs() << "******** Branch Coalescing ********\n");
  initialize(MF);

  DEBUG(dbgs() << "Function: "; MF.dump(); dbgs() << "\n");

  CoalescingCandidateInfo Cand1, Cand2;
  // Walk over blocks and find candidates to merge
  // Continue trying to merge with the first candidate found, as long as merging
  // is successfull.
  for (MachineBasicBlock &MBB : MF) {
    bool MergedCandidates = false;
    do {
      MergedCandidates = false;
      Cand1.clear();
      Cand2.clear();

      Cand1.BranchBlock = &MBB;

      // If unable to coalesce the branch, then continue to next block
      if (!canCoalesceBranch(Cand1))
        break;

      Cand2.BranchBlock = Cand1.BranchTargetBlock;
      if (!canCoalesceBranch(Cand2))
        break;

      // Sanity check
      // The branch-taken block of the second candidate should post-dominate the
      // first candidate
      assert(MPDT->dominates(Cand2.BranchTargetBlock, Cand1.BranchBlock) &&
             "Branch-taken block should post-dominate first candidate");

      if (!identicalOperands(Cand1.Cond, Cand2.Cond)) {
        DEBUG(dbgs() << "Blocks " << Cand1.BranchBlock->getNumber() << " and "
                     << Cand2.BranchBlock->getNumber()
                     << " have different branches\n");
        break;
      }
      if (!canMerge(Cand2, Cand1)) {
        DEBUG(dbgs() << "Cannot merge blocks " << Cand1.BranchBlock->getNumber()
                     << " and " << Cand2.BranchBlock->getNumber() << "\n");
        NumBlocksNotCoalesced++;
        continue;
      }
      DEBUG(dbgs() << "Merging blocks " << Cand1.BranchBlock->getNumber()
                   << " and " << Cand1.BranchTargetBlock->getNumber() << "\n");
      MergedCandidates = mergeCandidates(Cand2, Cand1);
      if (MergedCandidates)
        didSomething = true;

      DEBUG(dbgs() << "Function after merging: "; MF.dump(); dbgs() << "\n");
    } while (MergedCandidates);
  }

#ifndef NDEBUG
  // Verify MF is still valid after branch coalescing
  if (didSomething)
    MF.verify(nullptr, "Error in code produced by branch coalescing");
#endif // NDEBUG

  DEBUG(dbgs() << "Finished Branch Coalescing\n");
  return didSomething;
}
