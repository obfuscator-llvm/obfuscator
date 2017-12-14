//===- ArgumentPromotion.h - Promote by-reference arguments -----*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TRANSFORMS_IPO_ARGUMENTPROMOTION_H
#define LLVM_TRANSFORMS_IPO_ARGUMENTPROMOTION_H

#include "llvm/Analysis/CGSCCPassManager.h"
#include "llvm/Analysis/LazyCallGraph.h"

namespace llvm {

/// Argument promotion pass.
///
/// This pass walks the functions in each SCC and for each one tries to
/// transform it and all of its callers to replace indirect arguments with
/// direct (by-value) arguments.
class ArgumentPromotionPass : public PassInfoMixin<ArgumentPromotionPass> {
public:
  PreservedAnalyses run(LazyCallGraph::SCC &C, CGSCCAnalysisManager &AM,
                        LazyCallGraph &CG, CGSCCUpdateResult &UR);
};

}

#endif
