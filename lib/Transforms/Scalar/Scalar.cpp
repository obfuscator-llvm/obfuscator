//===-- Scalar.cpp --------------------------------------------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implements common infrastructure for libLLVMScalarOpts.a, which
// implements several scalar transformations over the LLVM intermediate
// representation, including the C bindings for that library.
//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/Scalar.h"
#include "llvm-c/Initialization.h"
#include "llvm-c/Transforms/Scalar.h"
#include "llvm/Analysis/BasicAliasAnalysis.h"
#include "llvm/Analysis/Passes.h"
#include "llvm/Analysis/ScopedNoAliasAA.h"
#include "llvm/Analysis/TypeBasedAliasAnalysis.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Verifier.h"
#include "llvm/InitializePasses.h"
#include "llvm/Transforms/Scalar/GVN.h"
#include "llvm/Transforms/Scalar/SimpleLoopUnswitch.h"

using namespace llvm;

/// initializeScalarOptsPasses - Initialize all passes linked into the
/// ScalarOpts library.
void llvm::initializeScalarOpts(PassRegistry &Registry) {
  initializeADCELegacyPassPass(Registry);
  initializeBDCELegacyPassPass(Registry);
  initializeAlignmentFromAssumptionsPass(Registry);
  initializeConstantHoistingLegacyPassPass(Registry);
  initializeConstantPropagationPass(Registry);
  initializeCorrelatedValuePropagationPass(Registry);
  initializeDCELegacyPassPass(Registry);
  initializeDeadInstEliminationPass(Registry);
  initializeScalarizerPass(Registry);
  initializeDSELegacyPassPass(Registry);
  initializeGuardWideningLegacyPassPass(Registry);
  initializeGVNLegacyPassPass(Registry);
  initializeNewGVNLegacyPassPass(Registry);
  initializeEarlyCSELegacyPassPass(Registry);
  initializeEarlyCSEMemSSALegacyPassPass(Registry);
  initializeGVNHoistLegacyPassPass(Registry);
  initializeGVNSinkLegacyPassPass(Registry);
  initializeFlattenCFGPassPass(Registry);
  initializeInductiveRangeCheckEliminationPass(Registry);
  initializeIndVarSimplifyLegacyPassPass(Registry);
  initializeInferAddressSpacesPass(Registry);
  initializeJumpThreadingPass(Registry);
  initializeLegacyLICMPassPass(Registry);
  initializeLegacyLoopSinkPassPass(Registry);
  initializeLoopDataPrefetchLegacyPassPass(Registry);
  initializeLoopDeletionLegacyPassPass(Registry);
  initializeLoopAccessLegacyAnalysisPass(Registry);
  initializeLoopInstSimplifyLegacyPassPass(Registry);
  initializeLoopInterchangePass(Registry);
  initializeLoopPredicationLegacyPassPass(Registry);
  initializeLoopRotateLegacyPassPass(Registry);
  initializeLoopStrengthReducePass(Registry);
  initializeLoopRerollPass(Registry);
  initializeLoopUnrollPass(Registry);
  initializeLoopUnswitchPass(Registry);
  initializeLoopVersioningLICMPass(Registry);
  initializeLoopIdiomRecognizeLegacyPassPass(Registry);
  initializeLowerAtomicLegacyPassPass(Registry);
  initializeLowerExpectIntrinsicPass(Registry);
  initializeLowerGuardIntrinsicLegacyPassPass(Registry);
  initializeMemCpyOptLegacyPassPass(Registry);
  initializeMergedLoadStoreMotionLegacyPassPass(Registry);
  initializeNaryReassociateLegacyPassPass(Registry);
  initializePartiallyInlineLibCallsLegacyPassPass(Registry);
  initializeReassociateLegacyPassPass(Registry);
  initializeRegToMemPass(Registry);
  initializeRewriteStatepointsForGCPass(Registry);
  initializeSCCPLegacyPassPass(Registry);
  initializeIPSCCPLegacyPassPass(Registry);
  initializeSROALegacyPassPass(Registry);
  initializeCFGSimplifyPassPass(Registry);
  initializeLateCFGSimplifyPassPass(Registry);
  initializeStructurizeCFGPass(Registry);
  initializeSimpleLoopUnswitchLegacyPassPass(Registry);
  initializeSinkingLegacyPassPass(Registry);
  initializeTailCallElimPass(Registry);
  initializeSeparateConstOffsetFromGEPPass(Registry);
  initializeSpeculativeExecutionLegacyPassPass(Registry);
  initializeStraightLineStrengthReducePass(Registry);
  initializePlaceBackedgeSafepointsImplPass(Registry);
  initializePlaceSafepointsPass(Registry);
  initializeFloat2IntLegacyPassPass(Registry);
  initializeLoopDistributeLegacyPass(Registry);
  initializeLoopLoadEliminationPass(Registry);
  initializeLoopSimplifyCFGLegacyPassPass(Registry);
  initializeLoopVersioningPassPass(Registry);
}

void LLVMInitializeScalarOpts(LLVMPassRegistryRef R) {
  initializeScalarOpts(*unwrap(R));
}

void LLVMAddAggressiveDCEPass(LLVMPassManagerRef PM) {
  unwrap(PM)->add(createAggressiveDCEPass());
}

void LLVMAddBitTrackingDCEPass(LLVMPassManagerRef PM) {
  unwrap(PM)->add(createBitTrackingDCEPass());
}

void LLVMAddAlignmentFromAssumptionsPass(LLVMPassManagerRef PM) {
  unwrap(PM)->add(createAlignmentFromAssumptionsPass());
}

void LLVMAddCFGSimplificationPass(LLVMPassManagerRef PM) {
  unwrap(PM)->add(createCFGSimplificationPass());
}

void LLVMAddLateCFGSimplificationPass(LLVMPassManagerRef PM) {
  unwrap(PM)->add(createLateCFGSimplificationPass());
}

void LLVMAddDeadStoreEliminationPass(LLVMPassManagerRef PM) {
  unwrap(PM)->add(createDeadStoreEliminationPass());
}

void LLVMAddScalarizerPass(LLVMPassManagerRef PM) {
  unwrap(PM)->add(createScalarizerPass());
}

void LLVMAddGVNPass(LLVMPassManagerRef PM) {
  unwrap(PM)->add(createGVNPass());
}

void LLVMAddNewGVNPass(LLVMPassManagerRef PM) {
  unwrap(PM)->add(createNewGVNPass());
}

void LLVMAddMergedLoadStoreMotionPass(LLVMPassManagerRef PM) {
  unwrap(PM)->add(createMergedLoadStoreMotionPass());
}

void LLVMAddIndVarSimplifyPass(LLVMPassManagerRef PM) {
  unwrap(PM)->add(createIndVarSimplifyPass());
}

void LLVMAddInstructionCombiningPass(LLVMPassManagerRef PM) {
  unwrap(PM)->add(createInstructionCombiningPass());
}

void LLVMAddJumpThreadingPass(LLVMPassManagerRef PM) {
  unwrap(PM)->add(createJumpThreadingPass());
}

void LLVMAddLoopSinkPass(LLVMPassManagerRef PM) {
  unwrap(PM)->add(createLoopSinkPass());
}

void LLVMAddLICMPass(LLVMPassManagerRef PM) {
  unwrap(PM)->add(createLICMPass());
}

void LLVMAddLoopDeletionPass(LLVMPassManagerRef PM) {
  unwrap(PM)->add(createLoopDeletionPass());
}

void LLVMAddLoopIdiomPass(LLVMPassManagerRef PM) {
  unwrap(PM)->add(createLoopIdiomPass());
}

void LLVMAddLoopRotatePass(LLVMPassManagerRef PM) {
  unwrap(PM)->add(createLoopRotatePass());
}

void LLVMAddLoopRerollPass(LLVMPassManagerRef PM) {
  unwrap(PM)->add(createLoopRerollPass());
}

void LLVMAddLoopSimplifyCFGPass(LLVMPassManagerRef PM) {
  unwrap(PM)->add(createLoopSimplifyCFGPass());
}

void LLVMAddLoopUnrollPass(LLVMPassManagerRef PM) {
  unwrap(PM)->add(createLoopUnrollPass());
}

void LLVMAddLoopUnswitchPass(LLVMPassManagerRef PM) {
  unwrap(PM)->add(createLoopUnswitchPass());
}

void LLVMAddMemCpyOptPass(LLVMPassManagerRef PM) {
  unwrap(PM)->add(createMemCpyOptPass());
}

void LLVMAddPartiallyInlineLibCallsPass(LLVMPassManagerRef PM) {
  unwrap(PM)->add(createPartiallyInlineLibCallsPass());
}

void LLVMAddLowerSwitchPass(LLVMPassManagerRef PM) {
  unwrap(PM)->add(createLowerSwitchPass());
}

void LLVMAddPromoteMemoryToRegisterPass(LLVMPassManagerRef PM) {
  unwrap(PM)->add(createPromoteMemoryToRegisterPass());
}

void LLVMAddReassociatePass(LLVMPassManagerRef PM) {
  unwrap(PM)->add(createReassociatePass());
}

void LLVMAddSCCPPass(LLVMPassManagerRef PM) {
  unwrap(PM)->add(createSCCPPass());
}

void LLVMAddScalarReplAggregatesPass(LLVMPassManagerRef PM) {
  unwrap(PM)->add(createSROAPass());
}

void LLVMAddScalarReplAggregatesPassSSA(LLVMPassManagerRef PM) {
  unwrap(PM)->add(createSROAPass());
}

void LLVMAddScalarReplAggregatesPassWithThreshold(LLVMPassManagerRef PM,
                                                  int Threshold) {
  unwrap(PM)->add(createSROAPass());
}

void LLVMAddSimplifyLibCallsPass(LLVMPassManagerRef PM) {
  // NOTE: The simplify-libcalls pass has been removed.
}

void LLVMAddTailCallEliminationPass(LLVMPassManagerRef PM) {
  unwrap(PM)->add(createTailCallEliminationPass());
}

void LLVMAddConstantPropagationPass(LLVMPassManagerRef PM) {
  unwrap(PM)->add(createConstantPropagationPass());
}

void LLVMAddDemoteMemoryToRegisterPass(LLVMPassManagerRef PM) {
  unwrap(PM)->add(createDemoteRegisterToMemoryPass());
}

void LLVMAddVerifierPass(LLVMPassManagerRef PM) {
  unwrap(PM)->add(createVerifierPass());
}

void LLVMAddCorrelatedValuePropagationPass(LLVMPassManagerRef PM) {
  unwrap(PM)->add(createCorrelatedValuePropagationPass());
}

void LLVMAddEarlyCSEPass(LLVMPassManagerRef PM) {
  unwrap(PM)->add(createEarlyCSEPass(false/*=UseMemorySSA*/));
}

void LLVMAddEarlyCSEMemSSAPass(LLVMPassManagerRef PM) {
  unwrap(PM)->add(createEarlyCSEPass(true/*=UseMemorySSA*/));
}

void LLVMAddGVNHoistLegacyPass(LLVMPassManagerRef PM) {
  unwrap(PM)->add(createGVNHoistPass());
}

void LLVMAddTypeBasedAliasAnalysisPass(LLVMPassManagerRef PM) {
  unwrap(PM)->add(createTypeBasedAAWrapperPass());
}

void LLVMAddScopedNoAliasAAPass(LLVMPassManagerRef PM) {
  unwrap(PM)->add(createScopedNoAliasAAWrapperPass());
}

void LLVMAddBasicAliasAnalysisPass(LLVMPassManagerRef PM) {
  unwrap(PM)->add(createBasicAAWrapperPass());
}

void LLVMAddLowerExpectIntrinsicPass(LLVMPassManagerRef PM) {
  unwrap(PM)->add(createLowerExpectIntrinsicPass());
}
