//===--- Core.cpp - Core ORC APIs (MaterializationUnit, JITDylib, etc.) ---===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "llvm/ExecutionEngine/Orc/Core.h"
#include "llvm/Config/llvm-config.h"
#include "llvm/ExecutionEngine/Orc/OrcError.h"
#include "llvm/IR/Mangler.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/Format.h"

#if LLVM_ENABLE_THREADS
#include <future>
#endif

#define DEBUG_TYPE "orc"

using namespace llvm;

namespace {

#ifndef NDEBUG

cl::opt<bool> PrintHidden("debug-orc-print-hidden", cl::init(true),
                          cl::desc("debug print hidden symbols defined by "
                                   "materialization units"),
                          cl::Hidden);

cl::opt<bool> PrintCallable("debug-orc-print-callable", cl::init(true),
                            cl::desc("debug print callable symbols defined by "
                                     "materialization units"),
                            cl::Hidden);

cl::opt<bool> PrintData("debug-orc-print-data", cl::init(true),
                        cl::desc("debug print data symbols defined by "
                                 "materialization units"),
                        cl::Hidden);

#endif // NDEBUG

// SetPrinter predicate that prints every element.
template <typename T> struct PrintAll {
  bool operator()(const T &E) { return true; }
};

bool anyPrintSymbolOptionSet() {
#ifndef NDEBUG
  return PrintHidden || PrintCallable || PrintData;
#else
  return false;
#endif // NDEBUG
}

bool flagsMatchCLOpts(const JITSymbolFlags &Flags) {
#ifndef NDEBUG
  // Bail out early if this is a hidden symbol and we're not printing hiddens.
  if (!PrintHidden && !Flags.isExported())
    return false;

  // Return true if this is callable and we're printing callables.
  if (PrintCallable && Flags.isCallable())
    return true;

  // Return true if this is data and we're printing data.
  if (PrintData && !Flags.isCallable())
    return true;

  // otherwise return false.
  return false;
#else
  return false;
#endif // NDEBUG
}

// Prints a set of items, filtered by an user-supplied predicate.
template <typename Set, typename Pred = PrintAll<typename Set::value_type>>
class SetPrinter {
public:
  SetPrinter(const Set &S, Pred ShouldPrint = Pred())
      : S(S), ShouldPrint(std::move(ShouldPrint)) {}

  void printTo(llvm::raw_ostream &OS) const {
    bool PrintComma = false;
    OS << "{";
    for (auto &E : S) {
      if (ShouldPrint(E)) {
        if (PrintComma)
          OS << ',';
        OS << ' ' << E;
        PrintComma = true;
      }
    }
    OS << " }";
  }

private:
  const Set &S;
  mutable Pred ShouldPrint;
};

template <typename Set, typename Pred>
SetPrinter<Set, Pred> printSet(const Set &S, Pred P = Pred()) {
  return SetPrinter<Set, Pred>(S, std::move(P));
}

// Render a SetPrinter by delegating to its printTo method.
template <typename Set, typename Pred>
llvm::raw_ostream &operator<<(llvm::raw_ostream &OS,
                              const SetPrinter<Set, Pred> &Printer) {
  Printer.printTo(OS);
  return OS;
}

struct PrintSymbolFlagsMapElemsMatchingCLOpts {
  bool operator()(const orc::SymbolFlagsMap::value_type &KV) {
    return flagsMatchCLOpts(KV.second);
  }
};

struct PrintSymbolMapElemsMatchingCLOpts {
  bool operator()(const orc::SymbolMap::value_type &KV) {
    return flagsMatchCLOpts(KV.second.getFlags());
  }
};

} // end anonymous namespace

namespace llvm {
namespace orc {

char FailedToMaterialize::ID = 0;
char SymbolsNotFound::ID = 0;
char SymbolsCouldNotBeRemoved::ID = 0;

RegisterDependenciesFunction NoDependenciesToRegister =
    RegisterDependenciesFunction();

void MaterializationUnit::anchor() {}

raw_ostream &operator<<(raw_ostream &OS, const SymbolStringPtr &Sym) {
  return OS << *Sym;
}

raw_ostream &operator<<(raw_ostream &OS, const SymbolNameSet &Symbols) {
  return OS << printSet(Symbols, PrintAll<SymbolStringPtr>());
}

raw_ostream &operator<<(raw_ostream &OS, const JITSymbolFlags &Flags) {
  if (Flags.isCallable())
    OS << "[Callable]";
  else
    OS << "[Data]";
  if (Flags.isWeak())
    OS << "[Weak]";
  else if (Flags.isCommon())
    OS << "[Common]";

  if (!Flags.isExported())
    OS << "[Hidden]";

  return OS;
}

raw_ostream &operator<<(raw_ostream &OS, const JITEvaluatedSymbol &Sym) {
  return OS << format("0x%016" PRIx64, Sym.getAddress()) << " "
            << Sym.getFlags();
}

raw_ostream &operator<<(raw_ostream &OS, const SymbolFlagsMap::value_type &KV) {
  return OS << "(\"" << KV.first << "\", " << KV.second << ")";
}

raw_ostream &operator<<(raw_ostream &OS, const SymbolMap::value_type &KV) {
  return OS << "(\"" << KV.first << "\": " << KV.second << ")";
}

raw_ostream &operator<<(raw_ostream &OS, const SymbolFlagsMap &SymbolFlags) {
  return OS << printSet(SymbolFlags, PrintSymbolFlagsMapElemsMatchingCLOpts());
}

raw_ostream &operator<<(raw_ostream &OS, const SymbolMap &Symbols) {
  return OS << printSet(Symbols, PrintSymbolMapElemsMatchingCLOpts());
}

raw_ostream &operator<<(raw_ostream &OS,
                        const SymbolDependenceMap::value_type &KV) {
  return OS << "(" << KV.first << ", " << KV.second << ")";
}

raw_ostream &operator<<(raw_ostream &OS, const SymbolDependenceMap &Deps) {
  return OS << printSet(Deps, PrintAll<SymbolDependenceMap::value_type>());
}

raw_ostream &operator<<(raw_ostream &OS, const MaterializationUnit &MU) {
  OS << "MU@" << &MU << " (\"" << MU.getName() << "\"";
  if (anyPrintSymbolOptionSet())
    OS << ", " << MU.getSymbols();
  return OS << ")";
}

raw_ostream &operator<<(raw_ostream &OS, const JITDylibSearchList &JDs) {
  OS << "[";
  if (!JDs.empty()) {
    assert(JDs.front().first && "JITDylibList entries must not be null");
    OS << " (\"" << JDs.front().first->getName() << "\", "
       << (JDs.front().second ? "true" : "false") << ")";
    for (auto &KV : make_range(std::next(JDs.begin()), JDs.end())) {
      assert(KV.first && "JITDylibList entries must not be null");
      OS << ", (\"" << KV.first->getName() << "\", "
         << (KV.second ? "true" : "false") << ")";
    }
  }
  OS << " ]";
  return OS;
}

raw_ostream &operator<<(raw_ostream &OS, const SymbolAliasMap &Aliases) {
  OS << "{";
  for (auto &KV : Aliases)
    OS << " " << *KV.first << ": " << KV.second.Aliasee << " "
       << KV.second.AliasFlags;
  OS << " }\n";
  return OS;
}

raw_ostream &operator<<(raw_ostream &OS, const SymbolState &S) {
  switch (S) {
  case SymbolState::Invalid:
    return OS << "Invalid";
  case SymbolState::NeverSearched:
    return OS << "Never-Searched";
  case SymbolState::Materializing:
    return OS << "Materializing";
  case SymbolState::Resolved:
    return OS << "Resolved";
  case SymbolState::Ready:
    return OS << "Ready";
  }
  llvm_unreachable("Invalid state");
}

FailedToMaterialize::FailedToMaterialize(SymbolNameSet Symbols)
    : Symbols(std::move(Symbols)) {
  assert(!this->Symbols.empty() && "Can not fail to resolve an empty set");
}

std::error_code FailedToMaterialize::convertToErrorCode() const {
  return orcError(OrcErrorCode::UnknownORCError);
}

void FailedToMaterialize::log(raw_ostream &OS) const {
  OS << "Failed to materialize symbols: " << Symbols;
}

SymbolsNotFound::SymbolsNotFound(SymbolNameSet Symbols)
    : Symbols(std::move(Symbols)) {
  assert(!this->Symbols.empty() && "Can not fail to resolve an empty set");
}

std::error_code SymbolsNotFound::convertToErrorCode() const {
  return orcError(OrcErrorCode::UnknownORCError);
}

void SymbolsNotFound::log(raw_ostream &OS) const {
  OS << "Symbols not found: " << Symbols;
}

SymbolsCouldNotBeRemoved::SymbolsCouldNotBeRemoved(SymbolNameSet Symbols)
    : Symbols(std::move(Symbols)) {
  assert(!this->Symbols.empty() && "Can not fail to resolve an empty set");
}

std::error_code SymbolsCouldNotBeRemoved::convertToErrorCode() const {
  return orcError(OrcErrorCode::UnknownORCError);
}

void SymbolsCouldNotBeRemoved::log(raw_ostream &OS) const {
  OS << "Symbols could not be removed: " << Symbols;
}

AsynchronousSymbolQuery::AsynchronousSymbolQuery(
    const SymbolNameSet &Symbols, SymbolState RequiredState,
    SymbolsResolvedCallback NotifyComplete)
    : NotifyComplete(std::move(NotifyComplete)), RequiredState(RequiredState) {
  assert(RequiredState >= SymbolState::Resolved &&
         "Cannot query for a symbols that have not reached the resolve state "
         "yet");

  OutstandingSymbolsCount = Symbols.size();

  for (auto &S : Symbols)
    ResolvedSymbols[S] = nullptr;
}

void AsynchronousSymbolQuery::notifySymbolMetRequiredState(
    const SymbolStringPtr &Name, JITEvaluatedSymbol Sym) {
  auto I = ResolvedSymbols.find(Name);
  assert(I != ResolvedSymbols.end() &&
         "Resolving symbol outside the requested set");
  assert(I->second.getAddress() == 0 && "Redundantly resolving symbol Name");
  I->second = std::move(Sym);
  --OutstandingSymbolsCount;
}

void AsynchronousSymbolQuery::handleComplete() {
  assert(OutstandingSymbolsCount == 0 &&
         "Symbols remain, handleComplete called prematurely");

  auto TmpNotifyComplete = std::move(NotifyComplete);
  NotifyComplete = SymbolsResolvedCallback();
  TmpNotifyComplete(std::move(ResolvedSymbols));
}

bool AsynchronousSymbolQuery::canStillFail() { return !!NotifyComplete; }

void AsynchronousSymbolQuery::handleFailed(Error Err) {
  assert(QueryRegistrations.empty() && ResolvedSymbols.empty() &&
         OutstandingSymbolsCount == 0 &&
         "Query should already have been abandoned");
  NotifyComplete(std::move(Err));
  NotifyComplete = SymbolsResolvedCallback();
}

void AsynchronousSymbolQuery::addQueryDependence(JITDylib &JD,
                                                 SymbolStringPtr Name) {
  bool Added = QueryRegistrations[&JD].insert(std::move(Name)).second;
  (void)Added;
  assert(Added && "Duplicate dependence notification?");
}

void AsynchronousSymbolQuery::removeQueryDependence(
    JITDylib &JD, const SymbolStringPtr &Name) {
  auto QRI = QueryRegistrations.find(&JD);
  assert(QRI != QueryRegistrations.end() &&
         "No dependencies registered for JD");
  assert(QRI->second.count(Name) && "No dependency on Name in JD");
  QRI->second.erase(Name);
  if (QRI->second.empty())
    QueryRegistrations.erase(QRI);
}

void AsynchronousSymbolQuery::detach() {
  ResolvedSymbols.clear();
  OutstandingSymbolsCount = 0;
  for (auto &KV : QueryRegistrations)
    KV.first->detachQueryHelper(*this, KV.second);
  QueryRegistrations.clear();
}

MaterializationResponsibility::MaterializationResponsibility(
    JITDylib &JD, SymbolFlagsMap SymbolFlags, VModuleKey K)
    : JD(JD), SymbolFlags(std::move(SymbolFlags)), K(std::move(K)) {
  assert(!this->SymbolFlags.empty() && "Materializing nothing?");
}

MaterializationResponsibility::~MaterializationResponsibility() {
  assert(SymbolFlags.empty() &&
         "All symbols should have been explicitly materialized or failed");
}

SymbolNameSet MaterializationResponsibility::getRequestedSymbols() const {
  return JD.getRequestedSymbols(SymbolFlags);
}

void MaterializationResponsibility::notifyResolved(const SymbolMap &Symbols) {
  LLVM_DEBUG({
    dbgs() << "In " << JD.getName() << " resolving " << Symbols << "\n";
  });
#ifndef NDEBUG
  for (auto &KV : Symbols) {
    auto I = SymbolFlags.find(KV.first);
    assert(I != SymbolFlags.end() &&
           "Resolving symbol outside this responsibility set");
    if (I->second.isWeak())
      assert(I->second == (KV.second.getFlags() | JITSymbolFlags::Weak) &&
             "Resolving symbol with incorrect flags");
    else
      assert(I->second == KV.second.getFlags() &&
             "Resolving symbol with incorrect flags");
  }
#endif

  JD.resolve(Symbols);
}

void MaterializationResponsibility::notifyEmitted() {

  LLVM_DEBUG({
    dbgs() << "In " << JD.getName() << " emitting " << SymbolFlags << "\n";
  });

  JD.emit(SymbolFlags);
  SymbolFlags.clear();
}

Error MaterializationResponsibility::defineMaterializing(
    const SymbolFlagsMap &NewSymbolFlags) {
  // Add the given symbols to this responsibility object.
  // It's ok if we hit a duplicate here: In that case the new version will be
  // discarded, and the JITDylib::defineMaterializing method will return a
  // duplicate symbol error.
  for (auto &KV : NewSymbolFlags)
    SymbolFlags.insert(KV);

  return JD.defineMaterializing(NewSymbolFlags);
}

void MaterializationResponsibility::failMaterialization() {

  LLVM_DEBUG({
    dbgs() << "In " << JD.getName() << " failing materialization for "
           << SymbolFlags << "\n";
  });

  SymbolNameSet FailedSymbols;
  for (auto &KV : SymbolFlags)
    FailedSymbols.insert(KV.first);

  JD.notifyFailed(FailedSymbols);
  SymbolFlags.clear();
}

void MaterializationResponsibility::replace(
    std::unique_ptr<MaterializationUnit> MU) {
  for (auto &KV : MU->getSymbols())
    SymbolFlags.erase(KV.first);

  LLVM_DEBUG(JD.getExecutionSession().runSessionLocked([&]() {
    dbgs() << "In " << JD.getName() << " replacing symbols with " << *MU
           << "\n";
  }););

  JD.replace(std::move(MU));
}

MaterializationResponsibility
MaterializationResponsibility::delegate(const SymbolNameSet &Symbols,
                                        VModuleKey NewKey) {

  if (NewKey == VModuleKey())
    NewKey = K;

  SymbolFlagsMap DelegatedFlags;

  for (auto &Name : Symbols) {
    auto I = SymbolFlags.find(Name);
    assert(I != SymbolFlags.end() &&
           "Symbol is not tracked by this MaterializationResponsibility "
           "instance");

    DelegatedFlags[Name] = std::move(I->second);
    SymbolFlags.erase(I);
  }

  return MaterializationResponsibility(JD, std::move(DelegatedFlags),
                                       std::move(NewKey));
}

void MaterializationResponsibility::addDependencies(
    const SymbolStringPtr &Name, const SymbolDependenceMap &Dependencies) {
  assert(SymbolFlags.count(Name) &&
         "Symbol not covered by this MaterializationResponsibility instance");
  JD.addDependencies(Name, Dependencies);
}

void MaterializationResponsibility::addDependenciesForAll(
    const SymbolDependenceMap &Dependencies) {
  for (auto &KV : SymbolFlags)
    JD.addDependencies(KV.first, Dependencies);
}

AbsoluteSymbolsMaterializationUnit::AbsoluteSymbolsMaterializationUnit(
    SymbolMap Symbols, VModuleKey K)
    : MaterializationUnit(extractFlags(Symbols), std::move(K)),
      Symbols(std::move(Symbols)) {}

StringRef AbsoluteSymbolsMaterializationUnit::getName() const {
  return "<Absolute Symbols>";
}

void AbsoluteSymbolsMaterializationUnit::materialize(
    MaterializationResponsibility R) {
  R.notifyResolved(Symbols);
  R.notifyEmitted();
}

void AbsoluteSymbolsMaterializationUnit::discard(const JITDylib &JD,
                                                 const SymbolStringPtr &Name) {
  assert(Symbols.count(Name) && "Symbol is not part of this MU");
  Symbols.erase(Name);
}

SymbolFlagsMap
AbsoluteSymbolsMaterializationUnit::extractFlags(const SymbolMap &Symbols) {
  SymbolFlagsMap Flags;
  for (const auto &KV : Symbols)
    Flags[KV.first] = KV.second.getFlags();
  return Flags;
}

ReExportsMaterializationUnit::ReExportsMaterializationUnit(
    JITDylib *SourceJD, bool MatchNonExported, SymbolAliasMap Aliases,
    VModuleKey K)
    : MaterializationUnit(extractFlags(Aliases), std::move(K)),
      SourceJD(SourceJD), MatchNonExported(MatchNonExported),
      Aliases(std::move(Aliases)) {}

StringRef ReExportsMaterializationUnit::getName() const {
  return "<Reexports>";
}

void ReExportsMaterializationUnit::materialize(
    MaterializationResponsibility R) {

  auto &ES = R.getTargetJITDylib().getExecutionSession();
  JITDylib &TgtJD = R.getTargetJITDylib();
  JITDylib &SrcJD = SourceJD ? *SourceJD : TgtJD;

  // Find the set of requested aliases and aliasees. Return any unrequested
  // aliases back to the JITDylib so as to not prematurely materialize any
  // aliasees.
  auto RequestedSymbols = R.getRequestedSymbols();
  SymbolAliasMap RequestedAliases;

  for (auto &Name : RequestedSymbols) {
    auto I = Aliases.find(Name);
    assert(I != Aliases.end() && "Symbol not found in aliases map?");
    RequestedAliases[Name] = std::move(I->second);
    Aliases.erase(I);
  }

  LLVM_DEBUG({
    ES.runSessionLocked([&]() {
      dbgs() << "materializing reexports: target = " << TgtJD.getName()
             << ", source = " << SrcJD.getName() << " " << RequestedAliases
             << "\n";
    });
  });

  if (!Aliases.empty()) {
    if (SourceJD)
      R.replace(reexports(*SourceJD, std::move(Aliases), MatchNonExported));
    else
      R.replace(symbolAliases(std::move(Aliases)));
  }

  // The OnResolveInfo struct will hold the aliases and responsibilty for each
  // query in the list.
  struct OnResolveInfo {
    OnResolveInfo(MaterializationResponsibility R, SymbolAliasMap Aliases)
        : R(std::move(R)), Aliases(std::move(Aliases)) {}

    MaterializationResponsibility R;
    SymbolAliasMap Aliases;
  };

  // Build a list of queries to issue. In each round we build the largest set of
  // aliases that we can resolve without encountering a chain definition of the
  // form Foo -> Bar, Bar -> Baz. Such a form would deadlock as the query would
  // be waitin on a symbol that it itself had to resolve. Usually this will just
  // involve one round and a single query.

  std::vector<std::pair<SymbolNameSet, std::shared_ptr<OnResolveInfo>>>
      QueryInfos;
  while (!RequestedAliases.empty()) {
    SymbolNameSet ResponsibilitySymbols;
    SymbolNameSet QuerySymbols;
    SymbolAliasMap QueryAliases;

    // Collect as many aliases as we can without including a chain.
    for (auto &KV : RequestedAliases) {
      // Chain detected. Skip this symbol for this round.
      if (&SrcJD == &TgtJD && (QueryAliases.count(KV.second.Aliasee) ||
                               RequestedAliases.count(KV.second.Aliasee)))
        continue;

      ResponsibilitySymbols.insert(KV.first);
      QuerySymbols.insert(KV.second.Aliasee);
      QueryAliases[KV.first] = std::move(KV.second);
    }

    // Remove the aliases collected this round from the RequestedAliases map.
    for (auto &KV : QueryAliases)
      RequestedAliases.erase(KV.first);

    assert(!QuerySymbols.empty() && "Alias cycle detected!");

    auto QueryInfo = std::make_shared<OnResolveInfo>(
        R.delegate(ResponsibilitySymbols), std::move(QueryAliases));
    QueryInfos.push_back(
        make_pair(std::move(QuerySymbols), std::move(QueryInfo)));
  }

  // Issue the queries.
  while (!QueryInfos.empty()) {
    auto QuerySymbols = std::move(QueryInfos.back().first);
    auto QueryInfo = std::move(QueryInfos.back().second);

    QueryInfos.pop_back();

    auto RegisterDependencies = [QueryInfo,
                                 &SrcJD](const SymbolDependenceMap &Deps) {
      // If there were no materializing symbols, just bail out.
      if (Deps.empty())
        return;

      // Otherwise the only deps should be on SrcJD.
      assert(Deps.size() == 1 && Deps.count(&SrcJD) &&
             "Unexpected dependencies for reexports");

      auto &SrcJDDeps = Deps.find(&SrcJD)->second;
      SymbolDependenceMap PerAliasDepsMap;
      auto &PerAliasDeps = PerAliasDepsMap[&SrcJD];

      for (auto &KV : QueryInfo->Aliases)
        if (SrcJDDeps.count(KV.second.Aliasee)) {
          PerAliasDeps = {KV.second.Aliasee};
          QueryInfo->R.addDependencies(KV.first, PerAliasDepsMap);
        }
    };

    auto OnComplete = [QueryInfo](Expected<SymbolMap> Result) {
      if (Result) {
        SymbolMap ResolutionMap;
        for (auto &KV : QueryInfo->Aliases) {
          assert(Result->count(KV.second.Aliasee) &&
                 "Result map missing entry?");
          ResolutionMap[KV.first] = JITEvaluatedSymbol(
              (*Result)[KV.second.Aliasee].getAddress(), KV.second.AliasFlags);
        }
        QueryInfo->R.notifyResolved(ResolutionMap);
        QueryInfo->R.notifyEmitted();
      } else {
        auto &ES = QueryInfo->R.getTargetJITDylib().getExecutionSession();
        ES.reportError(Result.takeError());
        QueryInfo->R.failMaterialization();
      }
    };

    ES.lookup(JITDylibSearchList({{&SrcJD, MatchNonExported}}), QuerySymbols,
              SymbolState::Resolved, std::move(OnComplete),
              std::move(RegisterDependencies));
  }
}

void ReExportsMaterializationUnit::discard(const JITDylib &JD,
                                           const SymbolStringPtr &Name) {
  assert(Aliases.count(Name) &&
         "Symbol not covered by this MaterializationUnit");
  Aliases.erase(Name);
}

SymbolFlagsMap
ReExportsMaterializationUnit::extractFlags(const SymbolAliasMap &Aliases) {
  SymbolFlagsMap SymbolFlags;
  for (auto &KV : Aliases)
    SymbolFlags[KV.first] = KV.second.AliasFlags;

  return SymbolFlags;
}

Expected<SymbolAliasMap>
buildSimpleReexportsAliasMap(JITDylib &SourceJD, const SymbolNameSet &Symbols) {
  auto Flags = SourceJD.lookupFlags(Symbols);

  if (!Flags)
    return Flags.takeError();

  if (Flags->size() != Symbols.size()) {
    SymbolNameSet Unresolved = Symbols;
    for (auto &KV : *Flags)
      Unresolved.erase(KV.first);
    return make_error<SymbolsNotFound>(std::move(Unresolved));
  }

  SymbolAliasMap Result;
  for (auto &Name : Symbols) {
    assert(Flags->count(Name) && "Missing entry in flags map");
    Result[Name] = SymbolAliasMapEntry(Name, (*Flags)[Name]);
  }

  return Result;
}

ReexportsGenerator::ReexportsGenerator(JITDylib &SourceJD,
                                       bool MatchNonExported,
                                       SymbolPredicate Allow)
    : SourceJD(SourceJD), MatchNonExported(MatchNonExported),
      Allow(std::move(Allow)) {}

Expected<SymbolNameSet>
ReexportsGenerator::operator()(JITDylib &JD, const SymbolNameSet &Names) {
  orc::SymbolNameSet Added;
  orc::SymbolAliasMap AliasMap;

  auto Flags = SourceJD.lookupFlags(Names);

  if (!Flags)
    return Flags.takeError();

  for (auto &KV : *Flags) {
    if (Allow && !Allow(KV.first))
      continue;
    AliasMap[KV.first] = SymbolAliasMapEntry(KV.first, KV.second);
    Added.insert(KV.first);
  }

  if (!Added.empty())
    cantFail(JD.define(reexports(SourceJD, AliasMap, MatchNonExported)));

  return Added;
}

Error JITDylib::defineMaterializing(const SymbolFlagsMap &SymbolFlags) {
  return ES.runSessionLocked([&]() -> Error {
    std::vector<SymbolTable::iterator> AddedSyms;

    for (auto &KV : SymbolFlags) {
      SymbolTable::iterator EntryItr;
      bool Added;

      std::tie(EntryItr, Added) =
          Symbols.insert(std::make_pair(KV.first, SymbolTableEntry(KV.second)));

      if (Added) {
        AddedSyms.push_back(EntryItr);
        EntryItr->second.setState(SymbolState::Materializing);
      } else {
        // Remove any symbols already added.
        for (auto &SI : AddedSyms)
          Symbols.erase(SI);

        // FIXME: Return all duplicates.
        return make_error<DuplicateDefinition>(*KV.first);
      }
    }

    return Error::success();
  });
}

void JITDylib::replace(std::unique_ptr<MaterializationUnit> MU) {
  assert(MU != nullptr && "Can not replace with a null MaterializationUnit");

  auto MustRunMU =
      ES.runSessionLocked([&, this]() -> std::unique_ptr<MaterializationUnit> {

#ifndef NDEBUG
        for (auto &KV : MU->getSymbols()) {
          auto SymI = Symbols.find(KV.first);
          assert(SymI != Symbols.end() && "Replacing unknown symbol");
          assert(SymI->second.isInMaterializationPhase() &&
                 "Can not call replace on a symbol that is not materializing");
          assert(!SymI->second.hasMaterializerAttached() &&
                 "Symbol should not have materializer attached already");
          assert(UnmaterializedInfos.count(KV.first) == 0 &&
                 "Symbol being replaced should have no UnmaterializedInfo");
        }
#endif // NDEBUG

        // If any symbol has pending queries against it then we need to
        // materialize MU immediately.
        for (auto &KV : MU->getSymbols()) {
          auto MII = MaterializingInfos.find(KV.first);
          if (MII != MaterializingInfos.end()) {
            if (MII->second.hasQueriesPending())
              return std::move(MU);
          }
        }

        // Otherwise, make MU responsible for all the symbols.
        auto UMI = std::make_shared<UnmaterializedInfo>(std::move(MU));
        for (auto &KV : UMI->MU->getSymbols()) {
          auto SymI = Symbols.find(KV.first);
          assert(SymI->second.getState() == SymbolState::Materializing &&
                 "Can not replace a symbol that is not materializing");
          assert(!SymI->second.hasMaterializerAttached() &&
                 "Can not replace a symbol that has a materializer attached");
          assert(UnmaterializedInfos.count(KV.first) == 0 &&
                 "Unexpected materializer entry in map");
          SymI->second.setAddress(SymI->second.getAddress());
          SymI->second.setMaterializerAttached(true);
          UnmaterializedInfos[KV.first] = UMI;
        }

        return nullptr;
      });

  if (MustRunMU)
    ES.dispatchMaterialization(*this, std::move(MustRunMU));
}

SymbolNameSet
JITDylib::getRequestedSymbols(const SymbolFlagsMap &SymbolFlags) const {
  return ES.runSessionLocked([&]() {
    SymbolNameSet RequestedSymbols;

    for (auto &KV : SymbolFlags) {
      assert(Symbols.count(KV.first) && "JITDylib does not cover this symbol?");
      assert(Symbols.find(KV.first)->second.isInMaterializationPhase() &&
             "getRequestedSymbols can only be called for symbols that have "
             "started materializing");
      auto I = MaterializingInfos.find(KV.first);
      if (I == MaterializingInfos.end())
        continue;

      if (I->second.hasQueriesPending())
        RequestedSymbols.insert(KV.first);
    }

    return RequestedSymbols;
  });
}

void JITDylib::addDependencies(const SymbolStringPtr &Name,
                               const SymbolDependenceMap &Dependencies) {
  assert(Symbols.count(Name) && "Name not in symbol table");
  assert(Symbols[Name].isInMaterializationPhase() &&
         "Can not add dependencies for a symbol that is not materializing");

  auto &MI = MaterializingInfos[Name];
  assert(!MI.IsEmitted && "Can not add dependencies to an emitted symbol");

  for (auto &KV : Dependencies) {
    assert(KV.first && "Null JITDylib in dependency?");
    auto &OtherJITDylib = *KV.first;
    auto &DepsOnOtherJITDylib = MI.UnemittedDependencies[&OtherJITDylib];

    for (auto &OtherSymbol : KV.second) {
#ifndef NDEBUG
      // Assert that this symbol exists and has not been emitted already.
      auto SymI = OtherJITDylib.Symbols.find(OtherSymbol);
      assert(SymI != OtherJITDylib.Symbols.end() &&
             (SymI->second.getState() != SymbolState::Ready &&
              "Dependency on emitted symbol"));
#endif

      auto &OtherMI = OtherJITDylib.MaterializingInfos[OtherSymbol];

      if (OtherMI.IsEmitted)
        transferEmittedNodeDependencies(MI, Name, OtherMI);
      else if (&OtherJITDylib != this || OtherSymbol != Name) {
        OtherMI.Dependants[this].insert(Name);
        DepsOnOtherJITDylib.insert(OtherSymbol);
      }
    }

    if (DepsOnOtherJITDylib.empty())
      MI.UnemittedDependencies.erase(&OtherJITDylib);
  }
}

void JITDylib::resolve(const SymbolMap &Resolved) {
  auto CompletedQueries = ES.runSessionLocked([&, this]() {
    AsynchronousSymbolQuerySet CompletedQueries;
    for (const auto &KV : Resolved) {
      auto &Name = KV.first;
      auto Sym = KV.second;

      auto I = Symbols.find(Name);

      assert(I != Symbols.end() && "Symbol not found");
      assert(!I->second.hasMaterializerAttached() &&
             "Resolving symbol with materializer attached?");
      assert(I->second.getState() == SymbolState::Materializing &&
             "Symbol should be materializing");
      assert(I->second.getAddress() == 0 && "Symbol has already been resolved");

      assert((Sym.getFlags() & ~JITSymbolFlags::Weak) ==
                 (I->second.getFlags() & ~JITSymbolFlags::Weak) &&
             "Resolved flags should match the declared flags");

      // Once resolved, symbols can never be weak.
      JITSymbolFlags ResolvedFlags = Sym.getFlags();
      ResolvedFlags &= ~JITSymbolFlags::Weak;
      I->second.setAddress(Sym.getAddress());
      I->second.setFlags(ResolvedFlags);
      I->second.setState(SymbolState::Resolved);

      auto &MI = MaterializingInfos[Name];
      for (auto &Q : MI.takeQueriesMeeting(SymbolState::Resolved)) {
        Q->notifySymbolMetRequiredState(Name, Sym);
        if (Q->isComplete())
          CompletedQueries.insert(std::move(Q));
      }
    }

    return CompletedQueries;
  });

  for (auto &Q : CompletedQueries) {
    assert(Q->isComplete() && "Q not completed");
    Q->handleComplete();
  }
}

void JITDylib::emit(const SymbolFlagsMap &Emitted) {
  auto CompletedQueries = ES.runSessionLocked([&, this]() {
    AsynchronousSymbolQuerySet CompletedQueries;

    for (const auto &KV : Emitted) {
      const auto &Name = KV.first;

      auto MII = MaterializingInfos.find(Name);
      assert(MII != MaterializingInfos.end() &&
             "Missing MaterializingInfo entry");

      auto &MI = MII->second;

      // For each dependant, transfer this node's emitted dependencies to
      // it. If the dependant node is ready (i.e. has no unemitted
      // dependencies) then notify any pending queries.
      for (auto &KV : MI.Dependants) {
        auto &DependantJD = *KV.first;
        for (auto &DependantName : KV.second) {
          auto DependantMII =
              DependantJD.MaterializingInfos.find(DependantName);
          assert(DependantMII != DependantJD.MaterializingInfos.end() &&
                 "Dependant should have MaterializingInfo");

          auto &DependantMI = DependantMII->second;

          // Remove the dependant's dependency on this node.
          assert(DependantMI.UnemittedDependencies[this].count(Name) &&
                 "Dependant does not count this symbol as a dependency?");
          DependantMI.UnemittedDependencies[this].erase(Name);
          if (DependantMI.UnemittedDependencies[this].empty())
            DependantMI.UnemittedDependencies.erase(this);

          // Transfer unemitted dependencies from this node to the dependant.
          DependantJD.transferEmittedNodeDependencies(DependantMI,
                                                      DependantName, MI);

          // If the dependant is emitted and this node was the last of its
          // unemitted dependencies then the dependant node is now ready, so
          // notify any pending queries on the dependant node.
          if (DependantMI.IsEmitted &&
              DependantMI.UnemittedDependencies.empty()) {
            assert(DependantMI.Dependants.empty() &&
                   "Dependants should be empty by now");

            // Since this dependant is now ready, we erase its MaterializingInfo
            // and update its materializing state.
            auto DependantSymI = DependantJD.Symbols.find(DependantName);
            assert(DependantSymI != DependantJD.Symbols.end() &&
                   "Dependant has no entry in the Symbols table");
            DependantSymI->second.setState(SymbolState::Ready);

            for (auto &Q : DependantMI.takeQueriesMeeting(SymbolState::Ready)) {
              Q->notifySymbolMetRequiredState(
                  DependantName, DependantSymI->second.getSymbol());
              if (Q->isComplete())
                CompletedQueries.insert(Q);
              Q->removeQueryDependence(DependantJD, DependantName);
            }

            DependantJD.MaterializingInfos.erase(DependantMII);
          }
        }
      }
      MI.Dependants.clear();
      MI.IsEmitted = true;

      if (MI.UnemittedDependencies.empty()) {
        auto SymI = Symbols.find(Name);
        assert(SymI != Symbols.end() && "Symbol has no entry in Symbols table");
        SymI->second.setState(SymbolState::Ready);
        for (auto &Q : MI.takeQueriesMeeting(SymbolState::Ready)) {
          Q->notifySymbolMetRequiredState(Name, SymI->second.getSymbol());
          if (Q->isComplete())
            CompletedQueries.insert(Q);
          Q->removeQueryDependence(*this, Name);
        }
        MaterializingInfos.erase(MII);
      }
    }

    return CompletedQueries;
  });

  for (auto &Q : CompletedQueries) {
    assert(Q->isComplete() && "Q is not complete");
    Q->handleComplete();
  }
}

void JITDylib::notifyFailed(const SymbolNameSet &FailedSymbols) {

  // FIXME: This should fail any transitively dependant symbols too.

  auto FailedQueriesToNotify = ES.runSessionLocked([&, this]() {
    AsynchronousSymbolQuerySet FailedQueries;
    std::vector<MaterializingInfosMap::iterator> MIIsToRemove;

    for (auto &Name : FailedSymbols) {
      auto I = Symbols.find(Name);
      assert(I != Symbols.end() && "Symbol not present in this JITDylib");
      Symbols.erase(I);

      auto MII = MaterializingInfos.find(Name);

      // If we have not created a MaterializingInfo for this symbol yet then
      // there is nobody to notify.
      if (MII == MaterializingInfos.end())
        continue;

      // Remove this symbol from the dependants list of any dependencies.
      for (auto &KV : MII->second.UnemittedDependencies) {
        auto *DependencyJD = KV.first;
        auto &Dependencies = KV.second;
        for (auto &DependencyName : Dependencies) {
          auto DependencyMII =
              DependencyJD->MaterializingInfos.find(DependencyName);
          assert(DependencyMII != DependencyJD->MaterializingInfos.end() &&
                 "Unemitted dependency must have a MaterializingInfo entry");
          assert(DependencyMII->second.Dependants.count(this) &&
                 "Dependency's dependants list does not contain this JITDylib");
          assert(DependencyMII->second.Dependants[this].count(Name) &&
                 "Dependency's dependants list does not contain dependant");
          DependencyMII->second.Dependants[this].erase(Name);
        }
      }

      // Copy all the queries to the FailedQueries list, then abandon them.
      // This has to be a copy, and the copy has to come before the abandon
      // operation: Each Q.detach() call will reach back into this
      // PendingQueries list to remove Q.
      for (auto &Q : MII->second.pendingQueries())
        FailedQueries.insert(Q);

      MIIsToRemove.push_back(std::move(MII));
    }

    // Detach failed queries.
    for (auto &Q : FailedQueries)
      Q->detach();

    // Remove the MaterializingInfos.
    for (auto &MII : MIIsToRemove) {
      assert(!MII->second.hasQueriesPending() &&
             "Queries remain after symbol was failed");

      MaterializingInfos.erase(MII);
    }

    return FailedQueries;
  });

  for (auto &Q : FailedQueriesToNotify)
    Q->handleFailed(make_error<FailedToMaterialize>(FailedSymbols));
}

void JITDylib::setSearchOrder(JITDylibSearchList NewSearchOrder,
                              bool SearchThisJITDylibFirst,
                              bool MatchNonExportedInThisDylib) {
  if (SearchThisJITDylibFirst) {
    if (NewSearchOrder.empty() || NewSearchOrder.front().first != this)
      NewSearchOrder.insert(NewSearchOrder.begin(),
                            {this, MatchNonExportedInThisDylib});
  }

  ES.runSessionLocked([&]() { SearchOrder = std::move(NewSearchOrder); });
}

void JITDylib::addToSearchOrder(JITDylib &JD, bool MatchNonExported) {
  ES.runSessionLocked([&]() {
    SearchOrder.push_back({&JD, MatchNonExported});
  });
}

void JITDylib::replaceInSearchOrder(JITDylib &OldJD, JITDylib &NewJD,
                                    bool MatchNonExported) {
  ES.runSessionLocked([&]() {
    auto I = std::find_if(SearchOrder.begin(), SearchOrder.end(),
                          [&](const JITDylibSearchList::value_type &KV) {
                            return KV.first == &OldJD;
                          });

    if (I != SearchOrder.end())
      *I = {&NewJD, MatchNonExported};
  });
}

void JITDylib::removeFromSearchOrder(JITDylib &JD) {
  ES.runSessionLocked([&]() {
    auto I = std::find_if(SearchOrder.begin(), SearchOrder.end(),
                          [&](const JITDylibSearchList::value_type &KV) {
                            return KV.first == &JD;
                          });
    if (I != SearchOrder.end())
      SearchOrder.erase(I);
  });
}

Error JITDylib::remove(const SymbolNameSet &Names) {
  return ES.runSessionLocked([&]() -> Error {
    using SymbolMaterializerItrPair =
        std::pair<SymbolTable::iterator, UnmaterializedInfosMap::iterator>;
    std::vector<SymbolMaterializerItrPair> SymbolsToRemove;
    SymbolNameSet Missing;
    SymbolNameSet Materializing;

    for (auto &Name : Names) {
      auto I = Symbols.find(Name);

      // Note symbol missing.
      if (I == Symbols.end()) {
        Missing.insert(Name);
        continue;
      }

      // Note symbol materializing.
      if (I->second.isInMaterializationPhase()) {
        Materializing.insert(Name);
        continue;
      }

      auto UMII = I->second.hasMaterializerAttached()
                      ? UnmaterializedInfos.find(Name)
                      : UnmaterializedInfos.end();
      SymbolsToRemove.push_back(std::make_pair(I, UMII));
    }

    // If any of the symbols are not defined, return an error.
    if (!Missing.empty())
      return make_error<SymbolsNotFound>(std::move(Missing));

    // If any of the symbols are currently materializing, return an error.
    if (!Materializing.empty())
      return make_error<SymbolsCouldNotBeRemoved>(std::move(Materializing));

    // Remove the symbols.
    for (auto &SymbolMaterializerItrPair : SymbolsToRemove) {
      auto UMII = SymbolMaterializerItrPair.second;

      // If there is a materializer attached, call discard.
      if (UMII != UnmaterializedInfos.end()) {
        UMII->second->MU->doDiscard(*this, UMII->first);
        UnmaterializedInfos.erase(UMII);
      }

      auto SymI = SymbolMaterializerItrPair.first;
      Symbols.erase(SymI);
    }

    return Error::success();
  });
}

Expected<SymbolFlagsMap> JITDylib::lookupFlags(const SymbolNameSet &Names) {
  return ES.runSessionLocked([&, this]() -> Expected<SymbolFlagsMap> {
    SymbolFlagsMap Result;
    auto Unresolved = lookupFlagsImpl(Result, Names);
    if (!Unresolved)
      return Unresolved.takeError();

    if (DefGenerator && !Unresolved->empty()) {
      auto NewDefs = DefGenerator(*this, *Unresolved);
      if (!NewDefs)
        return NewDefs.takeError();
      if (!NewDefs->empty()) {
        auto Unresolved2 = lookupFlagsImpl(Result, *NewDefs);
        if (!Unresolved2)
          return Unresolved2.takeError();
        (void)Unresolved2;
        assert(Unresolved2->empty() &&
               "All fallback defs should have been found by lookupFlagsImpl");
      }
    };
    return Result;
  });
}

Expected<SymbolNameSet> JITDylib::lookupFlagsImpl(SymbolFlagsMap &Flags,
                                                  const SymbolNameSet &Names) {
  SymbolNameSet Unresolved;

  for (auto &Name : Names) {
    auto I = Symbols.find(Name);
    if (I != Symbols.end()) {
      assert(!Flags.count(Name) && "Symbol already present in Flags map");
      Flags[Name] = I->second.getFlags();
    } else
      Unresolved.insert(Name);
  }

  return Unresolved;
}

Error JITDylib::lodgeQuery(std::shared_ptr<AsynchronousSymbolQuery> &Q,
                           SymbolNameSet &Unresolved, bool MatchNonExported,
                           MaterializationUnitList &MUs) {
  assert(Q && "Query can not be null");

  lodgeQueryImpl(Q, Unresolved, MatchNonExported, MUs);
  if (DefGenerator && !Unresolved.empty()) {
    auto NewDefs = DefGenerator(*this, Unresolved);
    if (!NewDefs)
      return NewDefs.takeError();
    if (!NewDefs->empty()) {
      for (auto &D : *NewDefs)
        Unresolved.erase(D);
      lodgeQueryImpl(Q, *NewDefs, MatchNonExported, MUs);
      assert(NewDefs->empty() &&
             "All fallback defs should have been found by lookupImpl");
    }
  }

  return Error::success();
}

void JITDylib::lodgeQueryImpl(
    std::shared_ptr<AsynchronousSymbolQuery> &Q, SymbolNameSet &Unresolved,
    bool MatchNonExported,
    std::vector<std::unique_ptr<MaterializationUnit>> &MUs) {

  std::vector<SymbolStringPtr> ToRemove;
  for (auto Name : Unresolved) {

    // Search for the name in Symbols. Skip it if not found.
    auto SymI = Symbols.find(Name);
    if (SymI == Symbols.end())
      continue;

    // If this is a non exported symbol and we're skipping those then skip it.
    if (!SymI->second.getFlags().isExported() && !MatchNonExported)
      continue;

    // If we matched against Name in JD, mark it to be removed from the
    // Unresolved set.
    ToRemove.push_back(Name);

    // If this symbol already meets the required state for then notify the
    // query and continue.
    if (SymI->second.getState() >= Q->getRequiredState()) {
      Q->notifySymbolMetRequiredState(Name, SymI->second.getSymbol());
      continue;
    }

    // Otherwise this symbol does not yet meet the required state. Check whether
    // it has a materializer attached, and if so prepare to run it.
    if (SymI->second.hasMaterializerAttached()) {
      assert(SymI->second.getAddress() == 0 &&
             "Symbol not resolved but already has address?");
      auto UMII = UnmaterializedInfos.find(Name);
      assert(UMII != UnmaterializedInfos.end() &&
             "Lazy symbol should have UnmaterializedInfo");
      auto MU = std::move(UMII->second->MU);
      assert(MU != nullptr && "Materializer should not be null");

      // Move all symbols associated with this MaterializationUnit into
      // materializing state.
      for (auto &KV : MU->getSymbols()) {
        auto SymK = Symbols.find(KV.first);
        SymK->second.setMaterializerAttached(false);
        SymK->second.setState(SymbolState::Materializing);
        UnmaterializedInfos.erase(KV.first);
      }

      // Add MU to the list of MaterializationUnits to be materialized.
      MUs.push_back(std::move(MU));
    }

    // Add the query to the PendingQueries list.
    assert(SymI->second.isInMaterializationPhase() &&
           "By this line the symbol should be materializing");
    auto &MI = MaterializingInfos[Name];
    MI.addQuery(Q);
    Q->addQueryDependence(*this, Name);
  }

  // Remove any symbols that we found.
  for (auto &Name : ToRemove)
    Unresolved.erase(Name);
}

Expected<SymbolNameSet>
JITDylib::legacyLookup(std::shared_ptr<AsynchronousSymbolQuery> Q,
                       SymbolNameSet Names) {
  assert(Q && "Query can not be null");

  ES.runOutstandingMUs();

  bool QueryComplete = false;
  std::vector<std::unique_ptr<MaterializationUnit>> MUs;

  SymbolNameSet Unresolved = std::move(Names);
  auto Err = ES.runSessionLocked([&, this]() -> Error {
    QueryComplete = lookupImpl(Q, MUs, Unresolved);
    if (DefGenerator && !Unresolved.empty()) {
      assert(!QueryComplete && "query complete but unresolved symbols remain?");
      auto NewDefs = DefGenerator(*this, Unresolved);
      if (!NewDefs)
        return NewDefs.takeError();
      if (!NewDefs->empty()) {
        for (auto &D : *NewDefs)
          Unresolved.erase(D);
        QueryComplete = lookupImpl(Q, MUs, *NewDefs);
        assert(NewDefs->empty() &&
               "All fallback defs should have been found by lookupImpl");
      }
    }
    return Error::success();
  });

  if (Err)
    return std::move(Err);

  assert((MUs.empty() || !QueryComplete) &&
         "If action flags are set, there should be no work to do (so no MUs)");

  if (QueryComplete)
    Q->handleComplete();

  // FIXME: Swap back to the old code below once RuntimeDyld works with
  //        callbacks from asynchronous queries.
  // Add MUs to the OutstandingMUs list.
  {
    std::lock_guard<std::recursive_mutex> Lock(ES.OutstandingMUsMutex);
    for (auto &MU : MUs)
      ES.OutstandingMUs.push_back(make_pair(this, std::move(MU)));
  }
  ES.runOutstandingMUs();

  // Dispatch any required MaterializationUnits for materialization.
  // for (auto &MU : MUs)
  //  ES.dispatchMaterialization(*this, std::move(MU));

  return Unresolved;
}

bool JITDylib::lookupImpl(
    std::shared_ptr<AsynchronousSymbolQuery> &Q,
    std::vector<std::unique_ptr<MaterializationUnit>> &MUs,
    SymbolNameSet &Unresolved) {
  bool QueryComplete = false;

  std::vector<SymbolStringPtr> ToRemove;
  for (auto Name : Unresolved) {

    // Search for the name in Symbols. Skip it if not found.
    auto SymI = Symbols.find(Name);
    if (SymI == Symbols.end())
      continue;

    // If we found Name, mark it to be removed from the Unresolved set.
    ToRemove.push_back(Name);

    if (SymI->second.getState() >= Q->getRequiredState()) {
      Q->notifySymbolMetRequiredState(Name, SymI->second.getSymbol());
      if (Q->isComplete())
        QueryComplete = true;
      continue;
    }

    // If the symbol is lazy, get the MaterialiaztionUnit for it.
    if (SymI->second.hasMaterializerAttached()) {
      assert(SymI->second.getAddress() == 0 &&
             "Lazy symbol should not have a resolved address");
      auto UMII = UnmaterializedInfos.find(Name);
      assert(UMII != UnmaterializedInfos.end() &&
             "Lazy symbol should have UnmaterializedInfo");
      auto MU = std::move(UMII->second->MU);
      assert(MU != nullptr && "Materializer should not be null");

      // Kick all symbols associated with this MaterializationUnit into
      // materializing state.
      for (auto &KV : MU->getSymbols()) {
        auto SymK = Symbols.find(KV.first);
        assert(SymK != Symbols.end() && "Missing symbol table entry");
        SymK->second.setState(SymbolState::Materializing);
        SymK->second.setMaterializerAttached(false);
        UnmaterializedInfos.erase(KV.first);
      }

      // Add MU to the list of MaterializationUnits to be materialized.
      MUs.push_back(std::move(MU));
    }

    // Add the query to the PendingQueries list.
    assert(SymI->second.isInMaterializationPhase() &&
           "By this line the symbol should be materializing");
    auto &MI = MaterializingInfos[Name];
    MI.addQuery(Q);
    Q->addQueryDependence(*this, Name);
  }

  // Remove any marked symbols from the Unresolved set.
  for (auto &Name : ToRemove)
    Unresolved.erase(Name);

  return QueryComplete;
}

void JITDylib::dump(raw_ostream &OS) {
  ES.runSessionLocked([&, this]() {
    OS << "JITDylib \"" << JITDylibName << "\" (ES: "
       << format("0x%016" PRIx64, reinterpret_cast<uintptr_t>(&ES)) << "):\n"
       << "Search order: [";
    for (auto &KV : SearchOrder)
      OS << " (\"" << KV.first->getName() << "\", "
         << (KV.second ? "all" : "exported only") << ")";
    OS << " ]\n"
       << "Symbol table:\n";

    for (auto &KV : Symbols) {
      OS << "    \"" << *KV.first << "\": ";
      if (auto Addr = KV.second.getAddress())
        OS << format("0x%016" PRIx64, Addr) << ", " << KV.second.getFlags()
           << " ";
      else
        OS << "<not resolved> ";

      OS << KV.second.getState();

      if (KV.second.hasMaterializerAttached()) {
        OS << " (Materializer ";
        auto I = UnmaterializedInfos.find(KV.first);
        assert(I != UnmaterializedInfos.end() &&
               "Lazy symbol should have UnmaterializedInfo");
        OS << I->second->MU.get() << ")\n";
      } else
        OS << "\n";
    }

    if (!MaterializingInfos.empty())
      OS << "  MaterializingInfos entries:\n";
    for (auto &KV : MaterializingInfos) {
      OS << "    \"" << *KV.first << "\":\n"
         << "      IsEmitted = " << (KV.second.IsEmitted ? "true" : "false")
         << "\n"
         << "      " << KV.second.pendingQueries().size()
         << " pending queries: { ";
      for (const auto &Q : KV.second.pendingQueries())
        OS << Q.get() << " (" << Q->getRequiredState() << ") ";
      OS << "}\n      Dependants:\n";
      for (auto &KV2 : KV.second.Dependants)
        OS << "        " << KV2.first->getName() << ": " << KV2.second << "\n";
      OS << "      Unemitted Dependencies:\n";
      for (auto &KV2 : KV.second.UnemittedDependencies)
        OS << "        " << KV2.first->getName() << ": " << KV2.second << "\n";
    }
  });
}

void JITDylib::MaterializingInfo::addQuery(
    std::shared_ptr<AsynchronousSymbolQuery> Q) {

  auto I = std::lower_bound(
      PendingQueries.rbegin(), PendingQueries.rend(), Q->getRequiredState(),
      [](const std::shared_ptr<AsynchronousSymbolQuery> &V, SymbolState S) {
        return V->getRequiredState() <= S;
      });
  PendingQueries.insert(I.base(), std::move(Q));
}

void JITDylib::MaterializingInfo::removeQuery(
    const AsynchronousSymbolQuery &Q) {
  // FIXME: Implement 'find_as' for shared_ptr<T>/T*.
  auto I =
      std::find_if(PendingQueries.begin(), PendingQueries.end(),
                   [&Q](const std::shared_ptr<AsynchronousSymbolQuery> &V) {
                     return V.get() == &Q;
                   });
  assert(I != PendingQueries.end() &&
         "Query is not attached to this MaterializingInfo");
  PendingQueries.erase(I);
}

JITDylib::AsynchronousSymbolQueryList
JITDylib::MaterializingInfo::takeQueriesMeeting(SymbolState RequiredState) {
  AsynchronousSymbolQueryList Result;
  while (!PendingQueries.empty()) {
    if (PendingQueries.back()->getRequiredState() > RequiredState)
      break;

    Result.push_back(std::move(PendingQueries.back()));
    PendingQueries.pop_back();
  }

  return Result;
}

JITDylib::AsynchronousSymbolQueryList
JITDylib::MaterializingInfo::takeAllQueries() {
  AsynchronousSymbolQueryList Result;
  std::swap(Result, PendingQueries);
  return Result;
}

JITDylib::JITDylib(ExecutionSession &ES, std::string Name)
    : ES(ES), JITDylibName(std::move(Name)) {
  SearchOrder.push_back({this, true});
}

Error JITDylib::defineImpl(MaterializationUnit &MU) {
  SymbolNameSet Duplicates;
  std::vector<SymbolStringPtr> ExistingDefsOverridden;
  std::vector<SymbolStringPtr> MUDefsOverridden;

  for (const auto &KV : MU.getSymbols()) {
    auto I = Symbols.find(KV.first);

    if (I != Symbols.end()) {
      if (KV.second.isStrong()) {
        if (I->second.getFlags().isStrong() ||
            I->second.getState() > SymbolState::NeverSearched)
          Duplicates.insert(KV.first);
        else {
          assert(I->second.getState() == SymbolState::NeverSearched &&
                 "Overridden existing def should be in the never-searched "
                 "state");
          ExistingDefsOverridden.push_back(KV.first);
        }
      } else
        MUDefsOverridden.push_back(KV.first);
    }
  }

  // If there were any duplicate definitions then bail out.
  if (!Duplicates.empty())
    return make_error<DuplicateDefinition>(**Duplicates.begin());

  // Discard any overridden defs in this MU.
  for (auto &S : MUDefsOverridden)
    MU.doDiscard(*this, S);

  // Discard existing overridden defs.
  for (auto &S : ExistingDefsOverridden) {

    auto UMII = UnmaterializedInfos.find(S);
    assert(UMII != UnmaterializedInfos.end() &&
           "Overridden existing def should have an UnmaterializedInfo");
    UMII->second->MU->doDiscard(*this, S);
  }

  // Finally, add the defs from this MU.
  for (auto &KV : MU.getSymbols()) {
    auto &SymEntry = Symbols[KV.first];
    SymEntry.setFlags(KV.second);
    SymEntry.setState(SymbolState::NeverSearched);
    SymEntry.setMaterializerAttached(true);
  }

  return Error::success();
}

void JITDylib::detachQueryHelper(AsynchronousSymbolQuery &Q,
                                 const SymbolNameSet &QuerySymbols) {
  for (auto &QuerySymbol : QuerySymbols) {
    assert(MaterializingInfos.count(QuerySymbol) &&
           "QuerySymbol does not have MaterializingInfo");
    auto &MI = MaterializingInfos[QuerySymbol];
    MI.removeQuery(Q);
  }
}

void JITDylib::transferEmittedNodeDependencies(
    MaterializingInfo &DependantMI, const SymbolStringPtr &DependantName,
    MaterializingInfo &EmittedMI) {
  for (auto &KV : EmittedMI.UnemittedDependencies) {
    auto &DependencyJD = *KV.first;
    SymbolNameSet *UnemittedDependenciesOnDependencyJD = nullptr;

    for (auto &DependencyName : KV.second) {
      auto &DependencyMI = DependencyJD.MaterializingInfos[DependencyName];

      // Do not add self dependencies.
      if (&DependencyMI == &DependantMI)
        continue;

      // If we haven't looked up the dependencies for DependencyJD yet, do it
      // now and cache the result.
      if (!UnemittedDependenciesOnDependencyJD)
        UnemittedDependenciesOnDependencyJD =
            &DependantMI.UnemittedDependencies[&DependencyJD];

      DependencyMI.Dependants[this].insert(DependantName);
      UnemittedDependenciesOnDependencyJD->insert(DependencyName);
    }
  }
}

ExecutionSession::ExecutionSession(std::shared_ptr<SymbolStringPool> SSP)
    : SSP(SSP ? std::move(SSP) : std::make_shared<SymbolStringPool>()) {
  // Construct the main dylib.
  JDs.push_back(std::unique_ptr<JITDylib>(new JITDylib(*this, "<main>")));
}

JITDylib &ExecutionSession::getMainJITDylib() {
  return runSessionLocked([this]() -> JITDylib & { return *JDs.front(); });
}

JITDylib *ExecutionSession::getJITDylibByName(StringRef Name) {
  return runSessionLocked([&, this]() -> JITDylib * {
    for (auto &JD : JDs)
      if (JD->getName() == Name)
        return JD.get();
    return nullptr;
  });
}

JITDylib &ExecutionSession::createJITDylib(std::string Name,
                                           bool AddToMainDylibSearchOrder) {
  assert(!getJITDylibByName(Name) && "JITDylib with that name already exists");
  return runSessionLocked([&, this]() -> JITDylib & {
    JDs.push_back(
        std::unique_ptr<JITDylib>(new JITDylib(*this, std::move(Name))));
    if (AddToMainDylibSearchOrder)
      JDs.front()->addToSearchOrder(*JDs.back());
    return *JDs.back();
  });
}

void ExecutionSession::legacyFailQuery(AsynchronousSymbolQuery &Q, Error Err) {
  assert(!!Err && "Error should be in failure state");

  bool SendErrorToQuery;
  runSessionLocked([&]() {
    Q.detach();
    SendErrorToQuery = Q.canStillFail();
  });

  if (SendErrorToQuery)
    Q.handleFailed(std::move(Err));
  else
    reportError(std::move(Err));
}

Expected<SymbolMap> ExecutionSession::legacyLookup(
    LegacyAsyncLookupFunction AsyncLookup, SymbolNameSet Names,
    SymbolState RequiredState,
    RegisterDependenciesFunction RegisterDependencies) {
#if LLVM_ENABLE_THREADS
  // In the threaded case we use promises to return the results.
  std::promise<SymbolMap> PromisedResult;
  Error ResolutionError = Error::success();
  auto NotifyComplete = [&](Expected<SymbolMap> R) {
    if (R)
      PromisedResult.set_value(std::move(*R));
    else {
      ErrorAsOutParameter _(&ResolutionError);
      ResolutionError = R.takeError();
      PromisedResult.set_value(SymbolMap());
    }
  };
#else
  SymbolMap Result;
  Error ResolutionError = Error::success();

  auto NotifyComplete = [&](Expected<SymbolMap> R) {
    ErrorAsOutParameter _(&ResolutionError);
    if (R)
      Result = std::move(*R);
    else
      ResolutionError = R.takeError();
  };
#endif

  auto Query = std::make_shared<AsynchronousSymbolQuery>(
      Names, RequiredState, std::move(NotifyComplete));
  // FIXME: This should be run session locked along with the registration code
  // and error reporting below.
  SymbolNameSet UnresolvedSymbols = AsyncLookup(Query, std::move(Names));

  // If the query was lodged successfully then register the dependencies,
  // otherwise fail it with an error.
  if (UnresolvedSymbols.empty())
    RegisterDependencies(Query->QueryRegistrations);
  else {
    bool DeliverError = runSessionLocked([&]() {
      Query->detach();
      return Query->canStillFail();
    });
    auto Err = make_error<SymbolsNotFound>(std::move(UnresolvedSymbols));
    if (DeliverError)
      Query->handleFailed(std::move(Err));
    else
      reportError(std::move(Err));
  }

#if LLVM_ENABLE_THREADS
  auto ResultFuture = PromisedResult.get_future();
  auto Result = ResultFuture.get();
  if (ResolutionError)
    return std::move(ResolutionError);
  return std::move(Result);

#else
  if (ResolutionError)
    return std::move(ResolutionError);

  return Result;
#endif
}

void ExecutionSession::lookup(
    const JITDylibSearchList &SearchOrder, SymbolNameSet Symbols,
    SymbolState RequiredState, SymbolsResolvedCallback NotifyComplete,
    RegisterDependenciesFunction RegisterDependencies) {

  LLVM_DEBUG({
    runSessionLocked([&]() {
      dbgs() << "Looking up " << Symbols << " in " << SearchOrder
             << " (required state: " << RequiredState << ")\n";
    });
  });

  // lookup can be re-entered recursively if running on a single thread. Run any
  // outstanding MUs in case this query depends on them, otherwise this lookup
  // will starve waiting for a result from an MU that is stuck in the queue.
  runOutstandingMUs();

  auto Unresolved = std::move(Symbols);
  std::map<JITDylib *, MaterializationUnitList> CollectedMUsMap;
  auto Q = std::make_shared<AsynchronousSymbolQuery>(Unresolved, RequiredState,
                                                     std::move(NotifyComplete));
  bool QueryComplete = false;

  auto LodgingErr = runSessionLocked([&]() -> Error {
    auto LodgeQuery = [&]() -> Error {
      for (auto &KV : SearchOrder) {
        assert(KV.first && "JITDylibList entries must not be null");
        assert(!CollectedMUsMap.count(KV.first) &&
               "JITDylibList should not contain duplicate entries");

        auto &JD = *KV.first;
        auto MatchNonExported = KV.second;
        if (auto Err = JD.lodgeQuery(Q, Unresolved, MatchNonExported,
                                     CollectedMUsMap[&JD]))
          return Err;
      }

      if (!Unresolved.empty())
        return make_error<SymbolsNotFound>(std::move(Unresolved));

      return Error::success();
    };

    if (auto Err = LodgeQuery()) {
      // Query failed.

      // Disconnect the query from its dependencies.
      Q->detach();

      // Replace the MUs.
      for (auto &KV : CollectedMUsMap)
        for (auto &MU : KV.second)
          KV.first->replace(std::move(MU));

      return Err;
    }

    // Query lodged successfully.

    // Record whether this query is fully ready / resolved. We will use
    // this to call handleFullyResolved/handleFullyReady outside the session
    // lock.
    QueryComplete = Q->isComplete();

    // Call the register dependencies function.
    if (RegisterDependencies && !Q->QueryRegistrations.empty())
      RegisterDependencies(Q->QueryRegistrations);

    return Error::success();
  });

  if (LodgingErr) {
    Q->handleFailed(std::move(LodgingErr));
    return;
  }

  if (QueryComplete)
    Q->handleComplete();

  // Move the MUs to the OutstandingMUs list, then materialize.
  {
    std::lock_guard<std::recursive_mutex> Lock(OutstandingMUsMutex);

    for (auto &KV : CollectedMUsMap)
      for (auto &MU : KV.second)
        OutstandingMUs.push_back(std::make_pair(KV.first, std::move(MU)));
  }

  runOutstandingMUs();
}

Expected<SymbolMap>
ExecutionSession::lookup(const JITDylibSearchList &SearchOrder,
                         const SymbolNameSet &Symbols,
                         SymbolState RequiredState,
                         RegisterDependenciesFunction RegisterDependencies) {
#if LLVM_ENABLE_THREADS
  // In the threaded case we use promises to return the results.
  std::promise<SymbolMap> PromisedResult;
  Error ResolutionError = Error::success();

  auto NotifyComplete = [&](Expected<SymbolMap> R) {
    if (R)
      PromisedResult.set_value(std::move(*R));
    else {
      ErrorAsOutParameter _(&ResolutionError);
      ResolutionError = R.takeError();
      PromisedResult.set_value(SymbolMap());
    }
  };

#else
  SymbolMap Result;
  Error ResolutionError = Error::success();

  auto NotifyComplete = [&](Expected<SymbolMap> R) {
    ErrorAsOutParameter _(&ResolutionError);
    if (R)
      Result = std::move(*R);
    else
      ResolutionError = R.takeError();
  };
#endif

  // Perform the asynchronous lookup.
  lookup(SearchOrder, Symbols, RequiredState, NotifyComplete,
         RegisterDependencies);

#if LLVM_ENABLE_THREADS
  auto ResultFuture = PromisedResult.get_future();
  auto Result = ResultFuture.get();

  if (ResolutionError)
    return std::move(ResolutionError);

  return std::move(Result);

#else
  if (ResolutionError)
    return std::move(ResolutionError);

  return Result;
#endif
}

Expected<JITEvaluatedSymbol>
ExecutionSession::lookup(const JITDylibSearchList &SearchOrder,
                         SymbolStringPtr Name) {
  SymbolNameSet Names({Name});

  if (auto ResultMap = lookup(SearchOrder, std::move(Names), SymbolState::Ready,
                              NoDependenciesToRegister)) {
    assert(ResultMap->size() == 1 && "Unexpected number of results");
    assert(ResultMap->count(Name) && "Missing result for symbol");
    return std::move(ResultMap->begin()->second);
  } else
    return ResultMap.takeError();
}

Expected<JITEvaluatedSymbol>
ExecutionSession::lookup(ArrayRef<JITDylib *> SearchOrder,
                         SymbolStringPtr Name) {
  SymbolNameSet Names({Name});

  JITDylibSearchList FullSearchOrder;
  FullSearchOrder.reserve(SearchOrder.size());
  for (auto *JD : SearchOrder)
    FullSearchOrder.push_back({JD, false});

  return lookup(FullSearchOrder, Name);
}

Expected<JITEvaluatedSymbol>
ExecutionSession::lookup(ArrayRef<JITDylib *> SearchOrder, StringRef Name) {
  return lookup(SearchOrder, intern(Name));
}

void ExecutionSession::dump(raw_ostream &OS) {
  runSessionLocked([this, &OS]() {
    for (auto &JD : JDs)
      JD->dump(OS);
  });
}

void ExecutionSession::runOutstandingMUs() {
  while (1) {
    std::pair<JITDylib *, std::unique_ptr<MaterializationUnit>> JITDylibAndMU;

    {
      std::lock_guard<std::recursive_mutex> Lock(OutstandingMUsMutex);
      if (!OutstandingMUs.empty()) {
        JITDylibAndMU = std::move(OutstandingMUs.back());
        OutstandingMUs.pop_back();
      }
    }

    if (JITDylibAndMU.first) {
      assert(JITDylibAndMU.second && "JITDylib, but no MU?");
      dispatchMaterialization(*JITDylibAndMU.first,
                              std::move(JITDylibAndMU.second));
    } else
      break;
  }
}

MangleAndInterner::MangleAndInterner(ExecutionSession &ES, const DataLayout &DL)
    : ES(ES), DL(DL) {}

SymbolStringPtr MangleAndInterner::operator()(StringRef Name) {
  std::string MangledName;
  {
    raw_string_ostream MangledNameStream(MangledName);
    Mangler::getNameWithPrefix(MangledNameStream, Name, DL);
  }
  return ES.intern(MangledName);
}

} // End namespace orc.
} // End namespace llvm.
