//===-- LLVMContext.cpp - Implement LLVMContext ---------------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
//  This file implements LLVMContext, as a wrapper around the opaque
//  class LLVMContextImpl.
//
//===----------------------------------------------------------------------===//

#include "llvm/IR/LLVMContext.h"
#include "LLVMContextImpl.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/Twine.h"
#include "llvm/IR/DiagnosticInfo.h"
#include "llvm/IR/DiagnosticPrinter.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/raw_ostream.h"
#include <cassert>
#include <cstdlib>
#include <string>
#include <utility>

using namespace llvm;

LLVMContext::LLVMContext() : pImpl(new LLVMContextImpl(*this)) {
  // Create the fixed metadata kinds. This is done in the same order as the
  // MD_* enum values so that they correspond.
  std::pair<unsigned, StringRef> MDKinds[] = {
    {MD_dbg, "dbg"},
    {MD_tbaa, "tbaa"},
    {MD_prof, "prof"},
    {MD_fpmath, "fpmath"},
    {MD_range, "range"},
    {MD_tbaa_struct, "tbaa.struct"},
    {MD_invariant_load, "invariant.load"},
    {MD_alias_scope, "alias.scope"},
    {MD_noalias, "noalias"},
    {MD_nontemporal, "nontemporal"},
    {MD_mem_parallel_loop_access, "llvm.mem.parallel_loop_access"},
    {MD_nonnull, "nonnull"},
    {MD_dereferenceable, "dereferenceable"},
    {MD_dereferenceable_or_null, "dereferenceable_or_null"},
    {MD_make_implicit, "make.implicit"},
    {MD_unpredictable, "unpredictable"},
    {MD_invariant_group, "invariant.group"},
    {MD_align, "align"},
    {MD_loop, "llvm.loop"},
    {MD_type, "type"},
    {MD_section_prefix, "section_prefix"},
    {MD_absolute_symbol, "absolute_symbol"},
    {MD_associated, "associated"},
  };

  for (auto &MDKind : MDKinds) {
    unsigned ID = getMDKindID(MDKind.second);
    assert(ID == MDKind.first && "metadata kind id drifted");
    (void)ID;
  }

  auto *DeoptEntry = pImpl->getOrInsertBundleTag("deopt");
  assert(DeoptEntry->second == LLVMContext::OB_deopt &&
         "deopt operand bundle id drifted!");
  (void)DeoptEntry;

  auto *FuncletEntry = pImpl->getOrInsertBundleTag("funclet");
  assert(FuncletEntry->second == LLVMContext::OB_funclet &&
         "funclet operand bundle id drifted!");
  (void)FuncletEntry;

  auto *GCTransitionEntry = pImpl->getOrInsertBundleTag("gc-transition");
  assert(GCTransitionEntry->second == LLVMContext::OB_gc_transition &&
         "gc-transition operand bundle id drifted!");
  (void)GCTransitionEntry;

  SyncScope::ID SingleThreadSSID =
      pImpl->getOrInsertSyncScopeID("singlethread");
  assert(SingleThreadSSID == SyncScope::SingleThread &&
         "singlethread synchronization scope ID drifted!");
  (void)SingleThreadSSID;

  SyncScope::ID SystemSSID =
      pImpl->getOrInsertSyncScopeID("");
  assert(SystemSSID == SyncScope::System &&
         "system synchronization scope ID drifted!");
  (void)SystemSSID;
}

LLVMContext::~LLVMContext() { delete pImpl; }

void LLVMContext::addModule(Module *M) {
  pImpl->OwnedModules.insert(M);
}

void LLVMContext::removeModule(Module *M) {
  pImpl->OwnedModules.erase(M);
}

//===----------------------------------------------------------------------===//
// Recoverable Backend Errors
//===----------------------------------------------------------------------===//

void LLVMContext::
setInlineAsmDiagnosticHandler(InlineAsmDiagHandlerTy DiagHandler,
                              void *DiagContext) {
  pImpl->InlineAsmDiagHandler = DiagHandler;
  pImpl->InlineAsmDiagContext = DiagContext;
}

/// getInlineAsmDiagnosticHandler - Return the diagnostic handler set by
/// setInlineAsmDiagnosticHandler.
LLVMContext::InlineAsmDiagHandlerTy
LLVMContext::getInlineAsmDiagnosticHandler() const {
  return pImpl->InlineAsmDiagHandler;
}

/// getInlineAsmDiagnosticContext - Return the diagnostic context set by
/// setInlineAsmDiagnosticHandler.
void *LLVMContext::getInlineAsmDiagnosticContext() const {
  return pImpl->InlineAsmDiagContext;
}

void LLVMContext::setDiagnosticHandler(DiagnosticHandlerTy DiagnosticHandler,
                                       void *DiagnosticContext,
                                       bool RespectFilters) {
  pImpl->DiagnosticHandler = DiagnosticHandler;
  pImpl->DiagnosticContext = DiagnosticContext;
  pImpl->RespectDiagnosticFilters = RespectFilters;
}

void LLVMContext::setDiagnosticsHotnessRequested(bool Requested) {
  pImpl->DiagnosticsHotnessRequested = Requested;
}
bool LLVMContext::getDiagnosticsHotnessRequested() const {
  return pImpl->DiagnosticsHotnessRequested;
}

void LLVMContext::setDiagnosticsHotnessThreshold(uint64_t Threshold) {
  pImpl->DiagnosticsHotnessThreshold = Threshold;
}
uint64_t LLVMContext::getDiagnosticsHotnessThreshold() const {
  return pImpl->DiagnosticsHotnessThreshold;
}

yaml::Output *LLVMContext::getDiagnosticsOutputFile() {
  return pImpl->DiagnosticsOutputFile.get();
}

void LLVMContext::setDiagnosticsOutputFile(std::unique_ptr<yaml::Output> F) {
  pImpl->DiagnosticsOutputFile = std::move(F);
}

LLVMContext::DiagnosticHandlerTy LLVMContext::getDiagnosticHandler() const {
  return pImpl->DiagnosticHandler;
}

void *LLVMContext::getDiagnosticContext() const {
  return pImpl->DiagnosticContext;
}

void LLVMContext::setYieldCallback(YieldCallbackTy Callback, void *OpaqueHandle)
{
  pImpl->YieldCallback = Callback;
  pImpl->YieldOpaqueHandle = OpaqueHandle;
}

void LLVMContext::yield() {
  if (pImpl->YieldCallback)
    pImpl->YieldCallback(this, pImpl->YieldOpaqueHandle);
}

void LLVMContext::emitError(const Twine &ErrorStr) {
  diagnose(DiagnosticInfoInlineAsm(ErrorStr));
}

void LLVMContext::emitError(const Instruction *I, const Twine &ErrorStr) {
  assert (I && "Invalid instruction");
  diagnose(DiagnosticInfoInlineAsm(*I, ErrorStr));
}

static bool isDiagnosticEnabled(const DiagnosticInfo &DI) {
  // Optimization remarks are selective. They need to check whether the regexp
  // pattern, passed via one of the -pass-remarks* flags, matches the name of
  // the pass that is emitting the diagnostic. If there is no match, ignore the
  // diagnostic and return.
  if (auto *Remark = dyn_cast<DiagnosticInfoOptimizationBase>(&DI))
    return Remark->isEnabled();

  return true;
}

const char *
LLVMContext::getDiagnosticMessagePrefix(DiagnosticSeverity Severity) {
  switch (Severity) {
  case DS_Error:
    return "error";
  case DS_Warning:
    return "warning";
  case DS_Remark:
    return "remark";
  case DS_Note:
    return "note";
  }
  llvm_unreachable("Unknown DiagnosticSeverity");
}

void LLVMContext::diagnose(const DiagnosticInfo &DI) {
  // If there is a report handler, use it.
  if (pImpl->DiagnosticHandler) {
    if (!pImpl->RespectDiagnosticFilters || isDiagnosticEnabled(DI))
      pImpl->DiagnosticHandler(DI, pImpl->DiagnosticContext);
    return;
  }

  if (!isDiagnosticEnabled(DI))
    return;

  // Otherwise, print the message with a prefix based on the severity.
  DiagnosticPrinterRawOStream DP(errs());
  errs() << getDiagnosticMessagePrefix(DI.getSeverity()) << ": ";
  DI.print(DP);
  errs() << "\n";
  if (DI.getSeverity() == DS_Error)
    exit(1);
}

void LLVMContext::emitError(unsigned LocCookie, const Twine &ErrorStr) {
  diagnose(DiagnosticInfoInlineAsm(LocCookie, ErrorStr));
}

//===----------------------------------------------------------------------===//
// Metadata Kind Uniquing
//===----------------------------------------------------------------------===//

/// Return a unique non-zero ID for the specified metadata kind.
unsigned LLVMContext::getMDKindID(StringRef Name) const {
  // If this is new, assign it its ID.
  return pImpl->CustomMDKindNames.insert(
                                     std::make_pair(
                                         Name, pImpl->CustomMDKindNames.size()))
      .first->second;
}

/// getHandlerNames - Populate client-supplied smallvector using custom
/// metadata name and ID.
void LLVMContext::getMDKindNames(SmallVectorImpl<StringRef> &Names) const {
  Names.resize(pImpl->CustomMDKindNames.size());
  for (StringMap<unsigned>::const_iterator I = pImpl->CustomMDKindNames.begin(),
       E = pImpl->CustomMDKindNames.end(); I != E; ++I)
    Names[I->second] = I->first();
}

void LLVMContext::getOperandBundleTags(SmallVectorImpl<StringRef> &Tags) const {
  pImpl->getOperandBundleTags(Tags);
}

uint32_t LLVMContext::getOperandBundleTagID(StringRef Tag) const {
  return pImpl->getOperandBundleTagID(Tag);
}

SyncScope::ID LLVMContext::getOrInsertSyncScopeID(StringRef SSN) {
  return pImpl->getOrInsertSyncScopeID(SSN);
}

void LLVMContext::getSyncScopeNames(SmallVectorImpl<StringRef> &SSNs) const {
  pImpl->getSyncScopeNames(SSNs);
}

void LLVMContext::setGC(const Function &Fn, std::string GCName) {
  auto It = pImpl->GCNames.find(&Fn);

  if (It == pImpl->GCNames.end()) {
    pImpl->GCNames.insert(std::make_pair(&Fn, std::move(GCName)));
    return;
  }
  It->second = std::move(GCName);
}

const std::string &LLVMContext::getGC(const Function &Fn) {
  return pImpl->GCNames[&Fn];
}

void LLVMContext::deleteGC(const Function &Fn) {
  pImpl->GCNames.erase(&Fn);
}

bool LLVMContext::shouldDiscardValueNames() const {
  return pImpl->DiscardValueNames;
}

bool LLVMContext::isODRUniquingDebugTypes() const { return !!pImpl->DITypeMap; }

void LLVMContext::enableDebugTypeODRUniquing() {
  if (pImpl->DITypeMap)
    return;

  pImpl->DITypeMap.emplace();
}

void LLVMContext::disableDebugTypeODRUniquing() { pImpl->DITypeMap.reset(); }

void LLVMContext::setDiscardValueNames(bool Discard) {
  pImpl->DiscardValueNames = Discard;
}

OptBisect &LLVMContext::getOptBisect() {
  return pImpl->getOptBisect();
}
