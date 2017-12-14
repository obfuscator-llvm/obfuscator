//===- MergeFunctions.cpp - Merge identical functions ---------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This pass looks for equivalent functions that are mergable and folds them.
//
// Order relation is defined on set of functions. It was made through
// special function comparison procedure that returns
// 0 when functions are equal,
// -1 when Left function is less than right function, and
// 1 for opposite case. We need total-ordering, so we need to maintain
// four properties on the functions set:
// a <= a (reflexivity)
// if a <= b and b <= a then a = b (antisymmetry)
// if a <= b and b <= c then a <= c (transitivity).
// for all a and b: a <= b or b <= a (totality).
//
// Comparison iterates through each instruction in each basic block.
// Functions are kept on binary tree. For each new function F we perform
// lookup in binary tree.
// In practice it works the following way:
// -- We define Function* container class with custom "operator<" (FunctionPtr).
// -- "FunctionPtr" instances are stored in std::set collection, so every
//    std::set::insert operation will give you result in log(N) time.
// 
// As an optimization, a hash of the function structure is calculated first, and
// two functions are only compared if they have the same hash. This hash is
// cheap to compute, and has the property that if function F == G according to
// the comparison function, then hash(F) == hash(G). This consistency property
// is critical to ensuring all possible merging opportunities are exploited.
// Collisions in the hash affect the speed of the pass but not the correctness
// or determinism of the resulting transformation.
//
// When a match is found the functions are folded. If both functions are
// overridable, we move the functionality into a new internal function and
// leave two overridable thunks to it.
//
//===----------------------------------------------------------------------===//
//
// Future work:
//
// * virtual functions.
//
// Many functions have their address taken by the virtual function table for
// the object they belong to. However, as long as it's only used for a lookup
// and call, this is irrelevant, and we'd like to fold such functions.
//
// * be smarter about bitcasts.
//
// In order to fold functions, we will sometimes add either bitcast instructions
// or bitcast constant expressions. Unfortunately, this can confound further
// analysis since the two functions differ where one has a bitcast and the
// other doesn't. We should learn to look through bitcasts.
//
// * Compare complex types with pointer types inside.
// * Compare cross-reference cases.
// * Compare complex expressions.
//
// All the three issues above could be described as ability to prove that
// fA == fB == fC == fE == fF == fG in example below:
//
//  void fA() {
//    fB();
//  }
//  void fB() {
//    fA();
//  }
//
//  void fE() {
//    fF();
//  }
//  void fF() {
//    fG();
//  }
//  void fG() {
//    fE();
//  }
//
// Simplest cross-reference case (fA <--> fB) was implemented in previous
// versions of MergeFunctions, though it presented only in two function pairs
// in test-suite (that counts >50k functions)
// Though possibility to detect complex cross-referencing (e.g.: A->B->C->D->A)
// could cover much more cases.
//
//===----------------------------------------------------------------------===//

#include "llvm/ADT/Hashing.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallSet.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/IR/CallSite.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/DebugInfo.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/ValueHandle.h"
#include "llvm/IR/ValueMap.h"
#include "llvm/Pass.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/IPO.h"
#include "llvm/Transforms/Utils/FunctionComparator.h"
#include <vector>

using namespace llvm;

#define DEBUG_TYPE "mergefunc"

STATISTIC(NumFunctionsMerged, "Number of functions merged");
STATISTIC(NumThunksWritten, "Number of thunks generated");
STATISTIC(NumAliasesWritten, "Number of aliases generated");
STATISTIC(NumDoubleWeak, "Number of new functions created");

static cl::opt<unsigned> NumFunctionsForSanityCheck(
    "mergefunc-sanity",
    cl::desc("How many functions in module could be used for "
             "MergeFunctions pass sanity check. "
             "'0' disables this check. Works only with '-debug' key."),
    cl::init(0), cl::Hidden);

// Under option -mergefunc-preserve-debug-info we:
// - Do not create a new function for a thunk.
// - Retain the debug info for a thunk's parameters (and associated
//   instructions for the debug info) from the entry block.
//   Note: -debug will display the algorithm at work.
// - Create debug-info for the call (to the shared implementation) made by
//   a thunk and its return value.
// - Erase the rest of the function, retaining the (minimally sized) entry
//   block to create a thunk.
// - Preserve a thunk's call site to point to the thunk even when both occur
//   within the same translation unit, to aid debugability. Note that this
//   behaviour differs from the underlying -mergefunc implementation which
//   modifies the thunk's call site to point to the shared implementation
//   when both occur within the same translation unit.
static cl::opt<bool>
    MergeFunctionsPDI("mergefunc-preserve-debug-info", cl::Hidden,
                      cl::init(false),
                      cl::desc("Preserve debug info in thunk when mergefunc "
                               "transformations are made."));

namespace {

class FunctionNode {
  mutable AssertingVH<Function> F;
  FunctionComparator::FunctionHash Hash;
public:
  // Note the hash is recalculated potentially multiple times, but it is cheap.
  FunctionNode(Function *F)
    : F(F), Hash(FunctionComparator::functionHash(*F))  {}
  Function *getFunc() const { return F; }
  FunctionComparator::FunctionHash getHash() const { return Hash; }

  /// Replace the reference to the function F by the function G, assuming their
  /// implementations are equal.
  void replaceBy(Function *G) const {
    F = G;
  }

  void release() { F = nullptr; }
};

/// MergeFunctions finds functions which will generate identical machine code,
/// by considering all pointer types to be equivalent. Once identified,
/// MergeFunctions will fold them by replacing a call to one to a call to a
/// bitcast of the other.
///
class MergeFunctions : public ModulePass {
public:
  static char ID;
  MergeFunctions()
    : ModulePass(ID), FnTree(FunctionNodeCmp(&GlobalNumbers)), FNodesInTree(),
      HasGlobalAliases(false) {
    initializeMergeFunctionsPass(*PassRegistry::getPassRegistry());
  }

  bool runOnModule(Module &M) override;

private:
  // The function comparison operator is provided here so that FunctionNodes do
  // not need to become larger with another pointer.
  class FunctionNodeCmp {
    GlobalNumberState* GlobalNumbers;
  public:
    FunctionNodeCmp(GlobalNumberState* GN) : GlobalNumbers(GN) {}
    bool operator()(const FunctionNode &LHS, const FunctionNode &RHS) const {
      // Order first by hashes, then full function comparison.
      if (LHS.getHash() != RHS.getHash())
        return LHS.getHash() < RHS.getHash();
      FunctionComparator FCmp(LHS.getFunc(), RHS.getFunc(), GlobalNumbers);
      return FCmp.compare() == -1;
    }
  };
  typedef std::set<FunctionNode, FunctionNodeCmp> FnTreeType;

  GlobalNumberState GlobalNumbers;

  /// A work queue of functions that may have been modified and should be
  /// analyzed again.
  std::vector<WeakTrackingVH> Deferred;

  /// Checks the rules of order relation introduced among functions set.
  /// Returns true, if sanity check has been passed, and false if failed.
#ifndef NDEBUG
  bool doSanityCheck(std::vector<WeakTrackingVH> &Worklist);
#endif

  /// Insert a ComparableFunction into the FnTree, or merge it away if it's
  /// equal to one that's already present.
  bool insert(Function *NewFunction);

  /// Remove a Function from the FnTree and queue it up for a second sweep of
  /// analysis.
  void remove(Function *F);

  /// Find the functions that use this Value and remove them from FnTree and
  /// queue the functions.
  void removeUsers(Value *V);

  /// Replace all direct calls of Old with calls of New. Will bitcast New if
  /// necessary to make types match.
  void replaceDirectCallers(Function *Old, Function *New);

  /// Merge two equivalent functions. Upon completion, G may be deleted, or may
  /// be converted into a thunk. In either case, it should never be visited
  /// again.
  void mergeTwoFunctions(Function *F, Function *G);

  /// Replace G with a thunk or an alias to F. Deletes G.
  void writeThunkOrAlias(Function *F, Function *G);

  /// Fill PDIUnrelatedWL with instructions from the entry block that are
  /// unrelated to parameter related debug info.
  void filterInstsUnrelatedToPDI(BasicBlock *GEntryBlock,
                                 std::vector<Instruction *> &PDIUnrelatedWL);

  /// Erase the rest of the CFG (i.e. barring the entry block).
  void eraseTail(Function *G);

  /// Erase the instructions in PDIUnrelatedWL as they are unrelated to the
  /// parameter debug info, from the entry block.
  void eraseInstsUnrelatedToPDI(std::vector<Instruction *> &PDIUnrelatedWL);

  /// Replace G with a simple tail call to bitcast(F). Also (unless
  /// MergeFunctionsPDI holds) replace direct uses of G with bitcast(F),
  /// delete G.
  void writeThunk(Function *F, Function *G);

  /// Replace G with an alias to F. Deletes G.
  void writeAlias(Function *F, Function *G);

  /// Replace function F with function G in the function tree.
  void replaceFunctionInTree(const FunctionNode &FN, Function *G);

  /// The set of all distinct functions. Use the insert() and remove() methods
  /// to modify it. The map allows efficient lookup and deferring of Functions.
  FnTreeType FnTree;
  // Map functions to the iterators of the FunctionNode which contains them
  // in the FnTree. This must be updated carefully whenever the FnTree is
  // modified, i.e. in insert(), remove(), and replaceFunctionInTree(), to avoid
  // dangling iterators into FnTree. The invariant that preserves this is that
  // there is exactly one mapping F -> FN for each FunctionNode FN in FnTree.
  ValueMap<Function*, FnTreeType::iterator> FNodesInTree;

  /// Whether or not the target supports global aliases.
  bool HasGlobalAliases;
};

} // end anonymous namespace

char MergeFunctions::ID = 0;
INITIALIZE_PASS(MergeFunctions, "mergefunc", "Merge Functions", false, false)

ModulePass *llvm::createMergeFunctionsPass() {
  return new MergeFunctions();
}

#ifndef NDEBUG
bool MergeFunctions::doSanityCheck(std::vector<WeakTrackingVH> &Worklist) {
  if (const unsigned Max = NumFunctionsForSanityCheck) {
    unsigned TripleNumber = 0;
    bool Valid = true;

    dbgs() << "MERGEFUNC-SANITY: Started for first " << Max << " functions.\n";

    unsigned i = 0;
    for (std::vector<WeakTrackingVH>::iterator I = Worklist.begin(),
                                               E = Worklist.end();
         I != E && i < Max; ++I, ++i) {
      unsigned j = i;
      for (std::vector<WeakTrackingVH>::iterator J = I; J != E && j < Max;
           ++J, ++j) {
        Function *F1 = cast<Function>(*I);
        Function *F2 = cast<Function>(*J);
        int Res1 = FunctionComparator(F1, F2, &GlobalNumbers).compare();
        int Res2 = FunctionComparator(F2, F1, &GlobalNumbers).compare();

        // If F1 <= F2, then F2 >= F1, otherwise report failure.
        if (Res1 != -Res2) {
          dbgs() << "MERGEFUNC-SANITY: Non-symmetric; triple: " << TripleNumber
                 << "\n";
          dbgs() << *F1 << '\n' << *F2 << '\n';
          Valid = false;
        }

        if (Res1 == 0)
          continue;

        unsigned k = j;
        for (std::vector<WeakTrackingVH>::iterator K = J; K != E && k < Max;
             ++k, ++K, ++TripleNumber) {
          if (K == J)
            continue;

          Function *F3 = cast<Function>(*K);
          int Res3 = FunctionComparator(F1, F3, &GlobalNumbers).compare();
          int Res4 = FunctionComparator(F2, F3, &GlobalNumbers).compare();

          bool Transitive = true;

          if (Res1 != 0 && Res1 == Res4) {
            // F1 > F2, F2 > F3 => F1 > F3
            Transitive = Res3 == Res1;
          } else if (Res3 != 0 && Res3 == -Res4) {
            // F1 > F3, F3 > F2 => F1 > F2
            Transitive = Res3 == Res1;
          } else if (Res4 != 0 && -Res3 == Res4) {
            // F2 > F3, F3 > F1 => F2 > F1
            Transitive = Res4 == -Res1;
          }

          if (!Transitive) {
            dbgs() << "MERGEFUNC-SANITY: Non-transitive; triple: "
                   << TripleNumber << "\n";
            dbgs() << "Res1, Res3, Res4: " << Res1 << ", " << Res3 << ", "
                   << Res4 << "\n";
            dbgs() << *F1 << '\n' << *F2 << '\n' << *F3 << '\n';
            Valid = false;
          }
        }
      }
    }

    dbgs() << "MERGEFUNC-SANITY: " << (Valid ? "Passed." : "Failed.") << "\n";
    return Valid;
  }
  return true;
}
#endif

bool MergeFunctions::runOnModule(Module &M) {
  if (skipModule(M))
    return false;

  bool Changed = false;

  // All functions in the module, ordered by hash. Functions with a unique
  // hash value are easily eliminated.
  std::vector<std::pair<FunctionComparator::FunctionHash, Function *>>
    HashedFuncs;
  for (Function &Func : M) {
    if (!Func.isDeclaration() && !Func.hasAvailableExternallyLinkage()) {
      HashedFuncs.push_back({FunctionComparator::functionHash(Func), &Func});
    } 
  }

  std::stable_sort(
      HashedFuncs.begin(), HashedFuncs.end(),
      [](const std::pair<FunctionComparator::FunctionHash, Function *> &a,
         const std::pair<FunctionComparator::FunctionHash, Function *> &b) {
        return a.first < b.first;
      });

  auto S = HashedFuncs.begin();
  for (auto I = HashedFuncs.begin(), IE = HashedFuncs.end(); I != IE; ++I) {
    // If the hash value matches the previous value or the next one, we must
    // consider merging it. Otherwise it is dropped and never considered again.
    if ((I != S && std::prev(I)->first == I->first) ||
        (std::next(I) != IE && std::next(I)->first == I->first) ) {
      Deferred.push_back(WeakTrackingVH(I->second));
    }
  }
  
  do {
    std::vector<WeakTrackingVH> Worklist;
    Deferred.swap(Worklist);

    DEBUG(doSanityCheck(Worklist));

    DEBUG(dbgs() << "size of module: " << M.size() << '\n');
    DEBUG(dbgs() << "size of worklist: " << Worklist.size() << '\n');

    // Insert functions and merge them.
    for (WeakTrackingVH &I : Worklist) {
      if (!I)
        continue;
      Function *F = cast<Function>(I);
      if (!F->isDeclaration() && !F->hasAvailableExternallyLinkage()) {
        Changed |= insert(F);
      }
    }
    DEBUG(dbgs() << "size of FnTree: " << FnTree.size() << '\n');
  } while (!Deferred.empty());

  FnTree.clear();
  GlobalNumbers.clear();

  return Changed;
}

// Replace direct callers of Old with New.
void MergeFunctions::replaceDirectCallers(Function *Old, Function *New) {
  Constant *BitcastNew = ConstantExpr::getBitCast(New, Old->getType());
  for (auto UI = Old->use_begin(), UE = Old->use_end(); UI != UE;) {
    Use *U = &*UI;
    ++UI;
    CallSite CS(U->getUser());
    if (CS && CS.isCallee(U)) {
      // Transfer the called function's attributes to the call site. Due to the
      // bitcast we will 'lose' ABI changing attributes because the 'called
      // function' is no longer a Function* but the bitcast. Code that looks up
      // the attributes from the called function will fail.

      // FIXME: This is not actually true, at least not anymore. The callsite
      // will always have the same ABI affecting attributes as the callee,
      // because otherwise the original input has UB. Note that Old and New
      // always have matching ABI, so no attributes need to be changed.
      // Transferring other attributes may help other optimizations, but that
      // should be done uniformly and not in this ad-hoc way.
      auto &Context = New->getContext();
      auto NewPAL = New->getAttributes();
      SmallVector<AttributeSet, 4> NewArgAttrs;
      for (unsigned argIdx = 0; argIdx < CS.arg_size(); argIdx++)
        NewArgAttrs.push_back(NewPAL.getParamAttributes(argIdx));
      // Don't transfer attributes from the function to the callee. Function
      // attributes typically aren't relevant to the calling convention or ABI.
      CS.setAttributes(AttributeList::get(Context, /*FnAttrs=*/AttributeSet(),
                                          NewPAL.getRetAttributes(),
                                          NewArgAttrs));

      remove(CS.getInstruction()->getParent()->getParent());
      U->set(BitcastNew);
    }
  }
}

// Replace G with an alias to F if possible, or else a thunk to F. Deletes G.
void MergeFunctions::writeThunkOrAlias(Function *F, Function *G) {
  if (HasGlobalAliases && G->hasGlobalUnnamedAddr()) {
    if (G->hasExternalLinkage() || G->hasLocalLinkage() ||
        G->hasWeakLinkage()) {
      writeAlias(F, G);
      return;
    }
  }

  writeThunk(F, G);
}

// Helper for writeThunk,
// Selects proper bitcast operation,
// but a bit simpler then CastInst::getCastOpcode.
static Value *createCast(IRBuilder<> &Builder, Value *V, Type *DestTy) {
  Type *SrcTy = V->getType();
  if (SrcTy->isStructTy()) {
    assert(DestTy->isStructTy());
    assert(SrcTy->getStructNumElements() == DestTy->getStructNumElements());
    Value *Result = UndefValue::get(DestTy);
    for (unsigned int I = 0, E = SrcTy->getStructNumElements(); I < E; ++I) {
      Value *Element = createCast(
          Builder, Builder.CreateExtractValue(V, makeArrayRef(I)),
          DestTy->getStructElementType(I));

      Result =
          Builder.CreateInsertValue(Result, Element, makeArrayRef(I));
    }
    return Result;
  }
  assert(!DestTy->isStructTy());
  if (SrcTy->isIntegerTy() && DestTy->isPointerTy())
    return Builder.CreateIntToPtr(V, DestTy);
  else if (SrcTy->isPointerTy() && DestTy->isIntegerTy())
    return Builder.CreatePtrToInt(V, DestTy);
  else
    return Builder.CreateBitCast(V, DestTy);
}

// Erase the instructions in PDIUnrelatedWL as they are unrelated to the
// parameter debug info, from the entry block.
void MergeFunctions::eraseInstsUnrelatedToPDI(
    std::vector<Instruction *> &PDIUnrelatedWL) {

  DEBUG(dbgs() << " Erasing instructions (in reverse order of appearance in "
                  "entry block) unrelated to parameter debug info from entry "
                  "block: {\n");
  while (!PDIUnrelatedWL.empty()) {
    Instruction *I = PDIUnrelatedWL.back();
    DEBUG(dbgs() << "  Deleting Instruction: ");
    DEBUG(I->print(dbgs()));
    DEBUG(dbgs() << "\n");
    I->eraseFromParent();
    PDIUnrelatedWL.pop_back();
  }
  DEBUG(dbgs() << " } // Done erasing instructions unrelated to parameter "
                  "debug info from entry block. \n");
}

// Reduce G to its entry block.
void MergeFunctions::eraseTail(Function *G) {

  std::vector<BasicBlock *> WorklistBB;
  for (Function::iterator BBI = std::next(G->begin()), BBE = G->end();
       BBI != BBE; ++BBI) {
    BBI->dropAllReferences();
    WorklistBB.push_back(&*BBI);
  }
  while (!WorklistBB.empty()) {
    BasicBlock *BB = WorklistBB.back();
    BB->eraseFromParent();
    WorklistBB.pop_back();
  }
}

// We are interested in the following instructions from the entry block as being
// related to parameter debug info:
// - @llvm.dbg.declare
// - stores from the incoming parameters to locations on the stack-frame
// - allocas that create these locations on the stack-frame
// - @llvm.dbg.value
// - the entry block's terminator
// The rest are unrelated to debug info for the parameters; fill up
// PDIUnrelatedWL with such instructions.
void MergeFunctions::filterInstsUnrelatedToPDI(
    BasicBlock *GEntryBlock, std::vector<Instruction *> &PDIUnrelatedWL) {

  std::set<Instruction *> PDIRelated;
  for (BasicBlock::iterator BI = GEntryBlock->begin(), BIE = GEntryBlock->end();
       BI != BIE; ++BI) {
    if (auto *DVI = dyn_cast<DbgValueInst>(&*BI)) {
      DEBUG(dbgs() << " Deciding: ");
      DEBUG(BI->print(dbgs()));
      DEBUG(dbgs() << "\n");
      DILocalVariable *DILocVar = DVI->getVariable();
      if (DILocVar->isParameter()) {
        DEBUG(dbgs() << "  Include (parameter): ");
        DEBUG(BI->print(dbgs()));
        DEBUG(dbgs() << "\n");
        PDIRelated.insert(&*BI);
      } else {
        DEBUG(dbgs() << "  Delete (!parameter): ");
        DEBUG(BI->print(dbgs()));
        DEBUG(dbgs() << "\n");
      }
    } else if (auto *DDI = dyn_cast<DbgDeclareInst>(&*BI)) {
      DEBUG(dbgs() << " Deciding: ");
      DEBUG(BI->print(dbgs()));
      DEBUG(dbgs() << "\n");
      DILocalVariable *DILocVar = DDI->getVariable();
      if (DILocVar->isParameter()) {
        DEBUG(dbgs() << "  Parameter: ");
        DEBUG(DILocVar->print(dbgs()));
        AllocaInst *AI = dyn_cast_or_null<AllocaInst>(DDI->getAddress());
        if (AI) {
          DEBUG(dbgs() << "  Processing alloca users: ");
          DEBUG(dbgs() << "\n");
          for (User *U : AI->users()) {
            if (StoreInst *SI = dyn_cast<StoreInst>(U)) {
              if (Value *Arg = SI->getValueOperand()) {
                if (dyn_cast<Argument>(Arg)) {
                  DEBUG(dbgs() << "  Include: ");
                  DEBUG(AI->print(dbgs()));
                  DEBUG(dbgs() << "\n");
                  PDIRelated.insert(AI);
                  DEBUG(dbgs() << "   Include (parameter): ");
                  DEBUG(SI->print(dbgs()));
                  DEBUG(dbgs() << "\n");
                  PDIRelated.insert(SI);
                  DEBUG(dbgs() << "  Include: ");
                  DEBUG(BI->print(dbgs()));
                  DEBUG(dbgs() << "\n");
                  PDIRelated.insert(&*BI);
                } else {
                  DEBUG(dbgs() << "   Delete (!parameter): ");
                  DEBUG(SI->print(dbgs()));
                  DEBUG(dbgs() << "\n");
                }
              }
            } else {
              DEBUG(dbgs() << "   Defer: ");
              DEBUG(U->print(dbgs()));
              DEBUG(dbgs() << "\n");
            }
          }
        } else {
          DEBUG(dbgs() << "  Delete (alloca NULL): ");
          DEBUG(BI->print(dbgs()));
          DEBUG(dbgs() << "\n");
        }
      } else {
        DEBUG(dbgs() << "  Delete (!parameter): ");
        DEBUG(BI->print(dbgs()));
        DEBUG(dbgs() << "\n");
      }
    } else if (dyn_cast<TerminatorInst>(BI) == GEntryBlock->getTerminator()) {
      DEBUG(dbgs() << " Will Include Terminator: ");
      DEBUG(BI->print(dbgs()));
      DEBUG(dbgs() << "\n");
      PDIRelated.insert(&*BI);
    } else {
      DEBUG(dbgs() << " Defer: ");
      DEBUG(BI->print(dbgs()));
      DEBUG(dbgs() << "\n");
    }
  }
  DEBUG(dbgs()
        << " Report parameter debug info related/related instructions: {\n");
  for (BasicBlock::iterator BI = GEntryBlock->begin(), BE = GEntryBlock->end();
       BI != BE; ++BI) {

    Instruction *I = &*BI;
    if (PDIRelated.find(I) == PDIRelated.end()) {
      DEBUG(dbgs() << "  !PDIRelated: ");
      DEBUG(I->print(dbgs()));
      DEBUG(dbgs() << "\n");
      PDIUnrelatedWL.push_back(I);
    } else {
      DEBUG(dbgs() << "   PDIRelated: ");
      DEBUG(I->print(dbgs()));
      DEBUG(dbgs() << "\n");
    }
  }
  DEBUG(dbgs() << " }\n");
}

// Replace G with a simple tail call to bitcast(F). Also (unless
// MergeFunctionsPDI holds) replace direct uses of G with bitcast(F),
// delete G. Under MergeFunctionsPDI, we use G itself for creating
// the thunk as we preserve the debug info (and associated instructions)
// from G's entry block pertaining to G's incoming arguments which are
// passed on as corresponding arguments in the call that G makes to F.
// For better debugability, under MergeFunctionsPDI, we do not modify G's
// call sites to point to F even when within the same translation unit.
void MergeFunctions::writeThunk(Function *F, Function *G) {
  if (!G->isInterposable() && !MergeFunctionsPDI) {
    // Redirect direct callers of G to F. (See note on MergeFunctionsPDI
    // above).
    replaceDirectCallers(G, F);
  }

  // If G was internal then we may have replaced all uses of G with F. If so,
  // stop here and delete G. There's no need for a thunk. (See note on
  // MergeFunctionsPDI above).
  if (G->hasLocalLinkage() && G->use_empty() && !MergeFunctionsPDI) {
    G->eraseFromParent();
    return;
  }

  BasicBlock *GEntryBlock = nullptr;
  std::vector<Instruction *> PDIUnrelatedWL;
  BasicBlock *BB = nullptr;
  Function *NewG = nullptr;
  if (MergeFunctionsPDI) {
    DEBUG(dbgs() << "writeThunk: (MergeFunctionsPDI) Do not create a new "
                    "function as thunk; retain original: "
                 << G->getName() << "()\n");
    GEntryBlock = &G->getEntryBlock();
    DEBUG(dbgs() << "writeThunk: (MergeFunctionsPDI) filter parameter related "
                    "debug info for "
                 << G->getName() << "() {\n");
    filterInstsUnrelatedToPDI(GEntryBlock, PDIUnrelatedWL);
    GEntryBlock->getTerminator()->eraseFromParent();
    BB = GEntryBlock;
  } else {
    NewG = Function::Create(G->getFunctionType(), G->getLinkage(), "",
                            G->getParent());
    BB = BasicBlock::Create(F->getContext(), "", NewG);
  }

  IRBuilder<> Builder(BB);
  Function *H = MergeFunctionsPDI ? G : NewG;
  SmallVector<Value *, 16> Args;
  unsigned i = 0;
  FunctionType *FFTy = F->getFunctionType();
  for (Argument & AI : H->args()) {
    Args.push_back(createCast(Builder, &AI, FFTy->getParamType(i)));
    ++i;
  }

  CallInst *CI = Builder.CreateCall(F, Args);
  ReturnInst *RI = nullptr;
  CI->setTailCall();
  CI->setCallingConv(F->getCallingConv());
  CI->setAttributes(F->getAttributes());
  if (H->getReturnType()->isVoidTy()) {
    RI = Builder.CreateRetVoid();
  } else {
    RI = Builder.CreateRet(createCast(Builder, CI, H->getReturnType()));
  }

  if (MergeFunctionsPDI) {
    DISubprogram *DIS = G->getSubprogram();
    if (DIS) {
      DebugLoc CIDbgLoc = DebugLoc::get(DIS->getScopeLine(), 0, DIS);
      DebugLoc RIDbgLoc = DebugLoc::get(DIS->getScopeLine(), 0, DIS);
      CI->setDebugLoc(CIDbgLoc);
      RI->setDebugLoc(RIDbgLoc);
    } else {
      DEBUG(dbgs() << "writeThunk: (MergeFunctionsPDI) No DISubprogram for "
                   << G->getName() << "()\n");
    }
    eraseTail(G);
    eraseInstsUnrelatedToPDI(PDIUnrelatedWL);
    DEBUG(dbgs() << "} // End of parameter related debug info filtering for: "
                 << G->getName() << "()\n");
  } else {
    NewG->copyAttributesFrom(G);
    NewG->takeName(G);
    removeUsers(G);
    G->replaceAllUsesWith(NewG);
    G->eraseFromParent();
  }

  DEBUG(dbgs() << "writeThunk: " << H->getName() << '\n');
  ++NumThunksWritten;
}

// Replace G with an alias to F and delete G.
void MergeFunctions::writeAlias(Function *F, Function *G) {
  auto *GA = GlobalAlias::create(G->getLinkage(), "", F);
  F->setAlignment(std::max(F->getAlignment(), G->getAlignment()));
  GA->takeName(G);
  GA->setVisibility(G->getVisibility());
  removeUsers(G);
  G->replaceAllUsesWith(GA);
  G->eraseFromParent();

  DEBUG(dbgs() << "writeAlias: " << GA->getName() << '\n');
  ++NumAliasesWritten;
}

// Merge two equivalent functions. Upon completion, Function G is deleted.
void MergeFunctions::mergeTwoFunctions(Function *F, Function *G) {
  if (F->isInterposable()) {
    assert(G->isInterposable());

    // Make them both thunks to the same internal function.
    Function *H = Function::Create(F->getFunctionType(), F->getLinkage(), "",
                                   F->getParent());
    H->copyAttributesFrom(F);
    H->takeName(F);
    removeUsers(F);
    F->replaceAllUsesWith(H);

    unsigned MaxAlignment = std::max(G->getAlignment(), H->getAlignment());

    if (HasGlobalAliases) {
      writeAlias(F, G);
      writeAlias(F, H);
    } else {
      writeThunk(F, G);
      writeThunk(F, H);
    }

    F->setAlignment(MaxAlignment);
    F->setLinkage(GlobalValue::PrivateLinkage);
    ++NumDoubleWeak;
  } else {
    writeThunkOrAlias(F, G);
  }

  ++NumFunctionsMerged;
}

/// Replace function F by function G.
void MergeFunctions::replaceFunctionInTree(const FunctionNode &FN,
                                           Function *G) {
  Function *F = FN.getFunc();
  assert(FunctionComparator(F, G, &GlobalNumbers).compare() == 0 &&
         "The two functions must be equal");
  
  auto I = FNodesInTree.find(F);
  assert(I != FNodesInTree.end() && "F should be in FNodesInTree");
  assert(FNodesInTree.count(G) == 0 && "FNodesInTree should not contain G");
  
  FnTreeType::iterator IterToFNInFnTree = I->second;
  assert(&(*IterToFNInFnTree) == &FN && "F should map to FN in FNodesInTree.");
  // Remove F -> FN and insert G -> FN
  FNodesInTree.erase(I);
  FNodesInTree.insert({G, IterToFNInFnTree});
  // Replace F with G in FN, which is stored inside the FnTree.
  FN.replaceBy(G);
}

// Insert a ComparableFunction into the FnTree, or merge it away if equal to one
// that was already inserted.
bool MergeFunctions::insert(Function *NewFunction) {
  std::pair<FnTreeType::iterator, bool> Result =
      FnTree.insert(FunctionNode(NewFunction));

  if (Result.second) {
    assert(FNodesInTree.count(NewFunction) == 0);
    FNodesInTree.insert({NewFunction, Result.first});
    DEBUG(dbgs() << "Inserting as unique: " << NewFunction->getName() << '\n');
    return false;
  }

  const FunctionNode &OldF = *Result.first;

  // Don't merge tiny functions, since it can just end up making the function
  // larger.
  // FIXME: Should still merge them if they are unnamed_addr and produce an
  // alias.
  if (NewFunction->size() == 1) {
    if (NewFunction->front().size() <= 2) {
      DEBUG(dbgs() << NewFunction->getName()
                   << " is to small to bother merging\n");
      return false;
    }
  }

  // Impose a total order (by name) on the replacement of functions. This is
  // important when operating on more than one module independently to prevent
  // cycles of thunks calling each other when the modules are linked together.
  //
  // First of all, we process strong functions before weak functions.
  if ((OldF.getFunc()->isInterposable() && !NewFunction->isInterposable()) ||
     (OldF.getFunc()->isInterposable() == NewFunction->isInterposable() &&
       OldF.getFunc()->getName() > NewFunction->getName())) {
    // Swap the two functions.
    Function *F = OldF.getFunc();
    replaceFunctionInTree(*Result.first, NewFunction);
    NewFunction = F;
    assert(OldF.getFunc() != F && "Must have swapped the functions.");
  }

  DEBUG(dbgs() << "  " << OldF.getFunc()->getName()
               << " == " << NewFunction->getName() << '\n');

  Function *DeleteF = NewFunction;
  mergeTwoFunctions(OldF.getFunc(), DeleteF);
  return true;
}

// Remove a function from FnTree. If it was already in FnTree, add
// it to Deferred so that we'll look at it in the next round.
void MergeFunctions::remove(Function *F) {
  auto I = FNodesInTree.find(F);
  if (I != FNodesInTree.end()) {
    DEBUG(dbgs() << "Deferred " << F->getName()<< ".\n");
    FnTree.erase(I->second);
    // I->second has been invalidated, remove it from the FNodesInTree map to
    // preserve the invariant.
    FNodesInTree.erase(I);
    Deferred.emplace_back(F);
  }
}

// For each instruction used by the value, remove() the function that contains
// the instruction. This should happen right before a call to RAUW.
void MergeFunctions::removeUsers(Value *V) {
  std::vector<Value *> Worklist;
  Worklist.push_back(V);
  SmallSet<Value*, 8> Visited;
  Visited.insert(V);
  while (!Worklist.empty()) {
    Value *V = Worklist.back();
    Worklist.pop_back();

    for (User *U : V->users()) {
      if (Instruction *I = dyn_cast<Instruction>(U)) {
        remove(I->getParent()->getParent());
      } else if (isa<GlobalValue>(U)) {
        // do nothing
      } else if (Constant *C = dyn_cast<Constant>(U)) {
        for (User *UU : C->users()) {
          if (!Visited.insert(UU).second)
            Worklist.push_back(UU);
        }
      }
    }
  }
}
