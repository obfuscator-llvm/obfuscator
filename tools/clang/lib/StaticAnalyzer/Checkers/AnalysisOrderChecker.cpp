//===- AnalysisOrderChecker - Print callbacks called ------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This checker prints callbacks that are called during analysis.
// This is required to ensure that callbacks are fired in order
// and do not duplicate or get lost.
// Feel free to extend this checker with any callback you need to check.
//
//===----------------------------------------------------------------------===//

#include "ClangSACheckers.h"
#include "clang/StaticAnalyzer/Core/Checker.h"
#include "clang/StaticAnalyzer/Core/CheckerManager.h"
#include "clang/StaticAnalyzer/Core/PathSensitive/CheckerContext.h"

using namespace clang;
using namespace ento;

namespace {

class AnalysisOrderChecker
    : public Checker<check::PreStmt<CastExpr>,
                     check::PostStmt<CastExpr>,
                     check::PreStmt<ArraySubscriptExpr>,
                     check::PostStmt<ArraySubscriptExpr>,
                     check::Bind,
                     check::RegionChanges> {
  bool isCallbackEnabled(AnalyzerOptions &Opts, StringRef CallbackName) const {
    return Opts.getBooleanOption("*", false, this) ||
        Opts.getBooleanOption(CallbackName, false, this);
  }

  bool isCallbackEnabled(CheckerContext &C, StringRef CallbackName) const {
    AnalyzerOptions &Opts = C.getAnalysisManager().getAnalyzerOptions();
    return isCallbackEnabled(Opts, CallbackName);
  }

  bool isCallbackEnabled(ProgramStateRef State, StringRef CallbackName) const {
    AnalyzerOptions &Opts = State->getStateManager().getOwningEngine()
                                 ->getAnalysisManager().getAnalyzerOptions();
    return isCallbackEnabled(Opts, CallbackName);
  }

public:
  void checkPreStmt(const CastExpr *CE, CheckerContext &C) const {
    if (isCallbackEnabled(C, "PreStmtCastExpr"))
      llvm::errs() << "PreStmt<CastExpr> (Kind : " << CE->getCastKindName()
                   << ")\n";
  }

  void checkPostStmt(const CastExpr *CE, CheckerContext &C) const {
    if (isCallbackEnabled(C, "PostStmtCastExpr"))
      llvm::errs() << "PostStmt<CastExpr> (Kind : " << CE->getCastKindName()
                   << ")\n";
  }

  void checkPreStmt(const ArraySubscriptExpr *SubExpr,
                    CheckerContext &C) const {
    if (isCallbackEnabled(C, "PreStmtArraySubscriptExpr"))
      llvm::errs() << "PreStmt<ArraySubscriptExpr>\n";
  }

  void checkPostStmt(const ArraySubscriptExpr *SubExpr,
                     CheckerContext &C) const {
    if (isCallbackEnabled(C, "PostStmtArraySubscriptExpr"))
      llvm::errs() << "PostStmt<ArraySubscriptExpr>\n";
  }

  void checkBind(SVal Loc, SVal Val, const Stmt *S, CheckerContext &C) const {
    if (isCallbackEnabled(C, "Bind"))
      llvm::errs() << "Bind\n";
  }

  ProgramStateRef
  checkRegionChanges(ProgramStateRef State,
                     const InvalidatedSymbols *Invalidated,
                     ArrayRef<const MemRegion *> ExplicitRegions,
                     ArrayRef<const MemRegion *> Regions,
                     const LocationContext *LCtx, const CallEvent *Call) const {
    if (isCallbackEnabled(State, "RegionChanges"))
      llvm::errs() << "RegionChanges\n";
    return State;
  }
};
} // end anonymous namespace

//===----------------------------------------------------------------------===//
// Registration.
//===----------------------------------------------------------------------===//

void ento::registerAnalysisOrderChecker(CheckerManager &mgr) {
  mgr.registerChecker<AnalysisOrderChecker>();
}
