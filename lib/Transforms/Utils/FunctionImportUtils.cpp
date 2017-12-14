//===- lib/Transforms/Utils/FunctionImportUtils.cpp - Importing utilities -===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implements the FunctionImportGlobalProcessing class, used
// to perform the necessary global value handling for function importing.
//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/Utils/FunctionImportUtils.h"
#include "llvm/Analysis/ModuleSummaryAnalysis.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instructions.h"
using namespace llvm;

/// Checks if we should import SGV as a definition, otherwise import as a
/// declaration.
bool FunctionImportGlobalProcessing::doImportAsDefinition(
    const GlobalValue *SGV, SetVector<GlobalValue *> *GlobalsToImport) {

  // For alias, we tie the definition to the base object. Extract it and recurse
  if (auto *GA = dyn_cast<GlobalAlias>(SGV)) {
    if (GA->isInterposable())
      return false;
    const GlobalObject *GO = GA->getBaseObject();
    if (!GO->hasLinkOnceODRLinkage())
      return false;
    return FunctionImportGlobalProcessing::doImportAsDefinition(
        GO, GlobalsToImport);
  }
  // Only import the globals requested for importing.
  if (GlobalsToImport->count(const_cast<GlobalValue *>(SGV)))
    return true;
  // Otherwise no.
  return false;
}

bool FunctionImportGlobalProcessing::doImportAsDefinition(
    const GlobalValue *SGV) {
  if (!isPerformingImport())
    return false;
  return FunctionImportGlobalProcessing::doImportAsDefinition(SGV,
                                                              GlobalsToImport);
}

bool FunctionImportGlobalProcessing::shouldPromoteLocalToGlobal(
    const GlobalValue *SGV) {
  assert(SGV->hasLocalLinkage());
  // Both the imported references and the original local variable must
  // be promoted.
  if (!isPerformingImport() && !isModuleExporting())
    return false;

  if (isPerformingImport()) {
    assert((!GlobalsToImport->count(const_cast<GlobalValue *>(SGV)) ||
            !isNonRenamableLocal(*SGV)) &&
           "Attempting to promote non-renamable local");
    // We don't know for sure yet if we are importing this value (as either
    // a reference or a def), since we are simply walking all values in the
    // module. But by necessity if we end up importing it and it is local,
    // it must be promoted, so unconditionally promote all values in the
    // importing module.
    return true;
  }

  // When exporting, consult the index. We can have more than one local
  // with the same GUID, in the case of same-named locals in different but
  // same-named source files that were compiled in their respective directories
  // (so the source file name and resulting GUID is the same). Find the one
  // in this module.
  auto Summary = ImportIndex.findSummaryInModule(
      SGV->getGUID(), SGV->getParent()->getModuleIdentifier());
  assert(Summary && "Missing summary for global value when exporting");
  auto Linkage = Summary->linkage();
  if (!GlobalValue::isLocalLinkage(Linkage)) {
    assert(!isNonRenamableLocal(*SGV) &&
           "Attempting to promote non-renamable local");
    return true;
  }

  return false;
}

#ifndef NDEBUG
bool FunctionImportGlobalProcessing::isNonRenamableLocal(
    const GlobalValue &GV) const {
  if (!GV.hasLocalLinkage())
    return false;
  // This needs to stay in sync with the logic in buildModuleSummaryIndex.
  if (GV.hasSection())
    return true;
  if (Used.count(const_cast<GlobalValue *>(&GV)))
    return true;
  return false;
}
#endif

std::string FunctionImportGlobalProcessing::getName(const GlobalValue *SGV,
                                                    bool DoPromote) {
  // For locals that must be promoted to global scope, ensure that
  // the promoted name uniquely identifies the copy in the original module,
  // using the ID assigned during combined index creation. When importing,
  // we rename all locals (not just those that are promoted) in order to
  // avoid naming conflicts between locals imported from different modules.
  if (SGV->hasLocalLinkage() && (DoPromote || isPerformingImport()))
    return ModuleSummaryIndex::getGlobalNameForLocal(
        SGV->getName(),
        ImportIndex.getModuleHash(SGV->getParent()->getModuleIdentifier()));
  return SGV->getName();
}

GlobalValue::LinkageTypes
FunctionImportGlobalProcessing::getLinkage(const GlobalValue *SGV,
                                           bool DoPromote) {
  // Any local variable that is referenced by an exported function needs
  // to be promoted to global scope. Since we don't currently know which
  // functions reference which local variables/functions, we must treat
  // all as potentially exported if this module is exporting anything.
  if (isModuleExporting()) {
    if (SGV->hasLocalLinkage() && DoPromote)
      return GlobalValue::ExternalLinkage;
    return SGV->getLinkage();
  }

  // Otherwise, if we aren't importing, no linkage change is needed.
  if (!isPerformingImport())
    return SGV->getLinkage();

  switch (SGV->getLinkage()) {
  case GlobalValue::ExternalLinkage:
    // External defnitions are converted to available_externally
    // definitions upon import, so that they are available for inlining
    // and/or optimization, but are turned into declarations later
    // during the EliminateAvailableExternally pass.
    if (doImportAsDefinition(SGV) && !dyn_cast<GlobalAlias>(SGV))
      return GlobalValue::AvailableExternallyLinkage;
    // An imported external declaration stays external.
    return SGV->getLinkage();

  case GlobalValue::AvailableExternallyLinkage:
    // An imported available_externally definition converts
    // to external if imported as a declaration.
    if (!doImportAsDefinition(SGV))
      return GlobalValue::ExternalLinkage;
    // An imported available_externally declaration stays that way.
    return SGV->getLinkage();

  case GlobalValue::LinkOnceAnyLinkage:
  case GlobalValue::LinkOnceODRLinkage:
    // These both stay the same when importing the definition.
    // The ThinLTO pass will eventually force-import their definitions.
    return SGV->getLinkage();

  case GlobalValue::WeakAnyLinkage:
    // Can't import weak_any definitions correctly, or we might change the
    // program semantics, since the linker will pick the first weak_any
    // definition and importing would change the order they are seen by the
    // linker. The module linking caller needs to enforce this.
    assert(!doImportAsDefinition(SGV));
    // If imported as a declaration, it becomes external_weak.
    return SGV->getLinkage();

  case GlobalValue::WeakODRLinkage:
    // For weak_odr linkage, there is a guarantee that all copies will be
    // equivalent, so the issue described above for weak_any does not exist,
    // and the definition can be imported. It can be treated similarly
    // to an imported externally visible global value.
    if (doImportAsDefinition(SGV) && !dyn_cast<GlobalAlias>(SGV))
      return GlobalValue::AvailableExternallyLinkage;
    else
      return GlobalValue::ExternalLinkage;

  case GlobalValue::AppendingLinkage:
    // It would be incorrect to import an appending linkage variable,
    // since it would cause global constructors/destructors to be
    // executed multiple times. This should have already been handled
    // by linkIfNeeded, and we will assert in shouldLinkFromSource
    // if we try to import, so we simply return AppendingLinkage.
    return GlobalValue::AppendingLinkage;

  case GlobalValue::InternalLinkage:
  case GlobalValue::PrivateLinkage:
    // If we are promoting the local to global scope, it is handled
    // similarly to a normal externally visible global.
    if (DoPromote) {
      if (doImportAsDefinition(SGV) && !dyn_cast<GlobalAlias>(SGV))
        return GlobalValue::AvailableExternallyLinkage;
      else
        return GlobalValue::ExternalLinkage;
    }
    // A non-promoted imported local definition stays local.
    // The ThinLTO pass will eventually force-import their definitions.
    return SGV->getLinkage();

  case GlobalValue::ExternalWeakLinkage:
    // External weak doesn't apply to definitions, must be a declaration.
    assert(!doImportAsDefinition(SGV));
    // Linkage stays external_weak.
    return SGV->getLinkage();

  case GlobalValue::CommonLinkage:
    // Linkage stays common on definitions.
    // The ThinLTO pass will eventually force-import their definitions.
    return SGV->getLinkage();
  }

  llvm_unreachable("unknown linkage type");
}

void FunctionImportGlobalProcessing::processGlobalForThinLTO(GlobalValue &GV) {
  bool DoPromote = false;
  if (GV.hasLocalLinkage() &&
      ((DoPromote = shouldPromoteLocalToGlobal(&GV)) || isPerformingImport())) {
    // Once we change the name or linkage it is difficult to determine
    // again whether we should promote since shouldPromoteLocalToGlobal needs
    // to locate the summary (based on GUID from name and linkage). Therefore,
    // use DoPromote result saved above.
    GV.setName(getName(&GV, DoPromote));
    GV.setLinkage(getLinkage(&GV, DoPromote));
    if (!GV.hasLocalLinkage())
      GV.setVisibility(GlobalValue::HiddenVisibility);
  } else
    GV.setLinkage(getLinkage(&GV, /* DoPromote */ false));

  // Remove functions imported as available externally defs from comdats,
  // as this is a declaration for the linker, and will be dropped eventually.
  // It is illegal for comdats to contain declarations.
  auto *GO = dyn_cast_or_null<GlobalObject>(&GV);
  if (GO && GO->isDeclarationForLinker() && GO->hasComdat()) {
    // The IRMover should not have placed any imported declarations in
    // a comdat, so the only declaration that should be in a comdat
    // at this point would be a definition imported as available_externally.
    assert(GO->hasAvailableExternallyLinkage() &&
           "Expected comdat on definition (possibly available external)");
    GO->setComdat(nullptr);
  }
}

void FunctionImportGlobalProcessing::processGlobalsForThinLTO() {
  for (GlobalVariable &GV : M.globals())
    processGlobalForThinLTO(GV);
  for (Function &SF : M)
    processGlobalForThinLTO(SF);
  for (GlobalAlias &GA : M.aliases())
    processGlobalForThinLTO(GA);
}

bool FunctionImportGlobalProcessing::run() {
  processGlobalsForThinLTO();
  return false;
}

bool llvm::renameModuleForThinLTO(Module &M, const ModuleSummaryIndex &Index,
                                  SetVector<GlobalValue *> *GlobalsToImport) {
  FunctionImportGlobalProcessing ThinLTOProcessing(M, Index, GlobalsToImport);
  return ThinLTOProcessing.run();
}
