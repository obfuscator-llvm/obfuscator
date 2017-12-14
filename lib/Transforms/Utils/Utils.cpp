//===-- Utils.cpp - TransformUtils Infrastructure -------------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file defines the common initialization infrastructure for the
// TransformUtils library.
//
//===----------------------------------------------------------------------===//

#include "llvm-c/Initialization.h"
#include "llvm/InitializePasses.h"
#include "llvm/PassRegistry.h"

using namespace llvm;

/// initializeTransformUtils - Initialize all passes in the TransformUtils
/// library.
void llvm::initializeTransformUtils(PassRegistry &Registry) {
  initializeAddDiscriminatorsLegacyPassPass(Registry);
  initializeBreakCriticalEdgesPass(Registry);
  initializeInstNamerPass(Registry);
  initializeLCSSAWrapperPassPass(Registry);
  initializeLibCallsShrinkWrapLegacyPassPass(Registry);
  initializeLoopSimplifyPass(Registry);
  initializeLowerInvokeLegacyPassPass(Registry);
  initializeLowerSwitchPass(Registry);
  initializeNameAnonGlobalLegacyPassPass(Registry);
  initializePromoteLegacyPassPass(Registry);
  initializeStripNonLineTableDebugInfoPass(Registry);
  initializeUnifyFunctionExitNodesPass(Registry);
  initializeInstSimplifierPass(Registry);
  initializeMetaRenamerPass(Registry);
  initializeStripGCRelocatesPass(Registry);
  initializePredicateInfoPrinterLegacyPassPass(Registry);
}

/// LLVMInitializeTransformUtils - C binding for initializeTransformUtilsPasses.
void LLVMInitializeTransformUtils(LLVMPassRegistryRef R) {
  initializeTransformUtils(*unwrap(R));
}
