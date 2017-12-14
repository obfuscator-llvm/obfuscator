//===- ModuleSymbolTable.cpp - symbol table for in-memory IR --------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This class represents a symbol table built from in-memory IR. It provides
// access to GlobalValues and should only be used if such access is required
// (e.g. in the LTO implementation).
//
//===----------------------------------------------------------------------===//

#include "llvm/Object/ModuleSymbolTable.h"
#include "RecordStreamer.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/Triple.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalAlias.h"
#include "llvm/IR/GlobalValue.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/Mangler.h"
#include "llvm/IR/Module.h"
#include "llvm/MC/MCAsmInfo.h"
#include "llvm/MC/MCContext.h"
#include "llvm/MC/MCDirectives.h"
#include "llvm/MC/MCInstrInfo.h"
#include "llvm/MC/MCObjectFileInfo.h"
#include "llvm/MC/MCParser/MCAsmParser.h"
#include "llvm/MC/MCParser/MCTargetAsmParser.h"
#include "llvm/MC/MCRegisterInfo.h"
#include "llvm/MC/MCSubtargetInfo.h"
#include "llvm/MC/MCSymbol.h"
#include "llvm/MC/MCTargetOptions.h"
#include "llvm/Object/SymbolicFile.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/CodeGen.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/SMLoc.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/TargetRegistry.h"
#include "llvm/Support/raw_ostream.h"
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <memory>
#include <string>

using namespace llvm;
using namespace object;

void ModuleSymbolTable::addModule(Module *M) {
  if (FirstMod)
    assert(FirstMod->getTargetTriple() == M->getTargetTriple());
  else
    FirstMod = M;

  for (GlobalValue &GV : M->global_values())
    SymTab.push_back(&GV);

  CollectAsmSymbols(*M, [this](StringRef Name, BasicSymbolRef::Flags Flags) {
    SymTab.push_back(new (AsmSymbols.Allocate()) AsmSymbol(Name, Flags));
  });
}

// Ensure ELF .symver aliases get the same binding as the defined symbol
// they alias with.
static void handleSymverAliases(const Module &M, RecordStreamer &Streamer) {
  if (Streamer.symverAliases().empty())
    return;

  // The name in the assembler will be mangled, but the name in the IR
  // might not, so we first compute a mapping from mangled name to GV.
  Mangler Mang;
  SmallString<64> MangledName;
  StringMap<const GlobalValue *> MangledNameMap;
  auto GetMangledName = [&](const GlobalValue &GV) {
    if (!GV.hasName())
      return;

    MangledName.clear();
    MangledName.reserve(GV.getName().size() + 1);
    Mang.getNameWithPrefix(MangledName, &GV, /*CannotUsePrivateLabel=*/false);
    MangledNameMap[MangledName] = &GV;
  };
  for (const Function &F : M)
    GetMangledName(F);
  for (const GlobalVariable &GV : M.globals())
    GetMangledName(GV);
  for (const GlobalAlias &GA : M.aliases())
    GetMangledName(GA);

  // Walk all the recorded .symver aliases, and set up the binding
  // for each alias.
  for (auto &Symver : Streamer.symverAliases()) {
    const MCSymbol *Aliasee = Symver.first;
    MCSymbolAttr Attr = MCSA_Invalid;

    // First check if the aliasee binding was recorded in the asm.
    RecordStreamer::State state = Streamer.getSymbolState(Aliasee);
    switch (state) {
    case RecordStreamer::Global:
    case RecordStreamer::DefinedGlobal:
      Attr = MCSA_Global;
      break;
    case RecordStreamer::UndefinedWeak:
    case RecordStreamer::DefinedWeak:
      Attr = MCSA_Weak;
      break;
    default:
      break;
    }

    // If we don't have a symbol attribute from assembly, then check if
    // the aliasee was defined in the IR.
    if (Attr == MCSA_Invalid) {
      const auto *GV = M.getNamedValue(Aliasee->getName());
      if (!GV) {
        auto MI = MangledNameMap.find(Aliasee->getName());
        if (MI != MangledNameMap.end())
          GV = MI->second;
        else
          continue;
      }
      if (GV->hasExternalLinkage())
        Attr = MCSA_Global;
      else if (GV->hasLocalLinkage())
        Attr = MCSA_Local;
      else if (GV->isWeakForLinker())
        Attr = MCSA_Weak;
    }
    if (Attr == MCSA_Invalid)
      continue;

    // Set the detected binding on each alias with this aliasee.
    for (auto &Alias : Symver.second)
      Streamer.EmitSymbolAttribute(Alias, Attr);
  }
}

void ModuleSymbolTable::CollectAsmSymbols(
    const Module &M,
    function_ref<void(StringRef, BasicSymbolRef::Flags)> AsmSymbol) {
  StringRef InlineAsm = M.getModuleInlineAsm();
  if (InlineAsm.empty())
    return;

  std::string Err;
  const Triple TT(M.getTargetTriple());
  const Target *T = TargetRegistry::lookupTarget(TT.str(), Err);
  assert(T && T->hasMCAsmParser());

  std::unique_ptr<MCRegisterInfo> MRI(T->createMCRegInfo(TT.str()));
  if (!MRI)
    return;

  std::unique_ptr<MCAsmInfo> MAI(T->createMCAsmInfo(*MRI, TT.str()));
  if (!MAI)
    return;

  std::unique_ptr<MCSubtargetInfo> STI(
      T->createMCSubtargetInfo(TT.str(), "", ""));
  if (!STI)
    return;

  std::unique_ptr<MCInstrInfo> MCII(T->createMCInstrInfo());
  if (!MCII)
    return;

  MCObjectFileInfo MOFI;
  MCContext MCCtx(MAI.get(), MRI.get(), &MOFI);
  MOFI.InitMCObjectFileInfo(TT, /*PIC*/ false, CodeModel::Default, MCCtx);
  RecordStreamer Streamer(MCCtx);
  T->createNullTargetStreamer(Streamer);

  std::unique_ptr<MemoryBuffer> Buffer(MemoryBuffer::getMemBuffer(InlineAsm));
  SourceMgr SrcMgr;
  SrcMgr.AddNewSourceBuffer(std::move(Buffer), SMLoc());
  std::unique_ptr<MCAsmParser> Parser(
      createMCAsmParser(SrcMgr, MCCtx, Streamer, *MAI));

  MCTargetOptions MCOptions;
  std::unique_ptr<MCTargetAsmParser> TAP(
      T->createMCAsmParser(*STI, *Parser, *MCII, MCOptions));
  if (!TAP)
    return;

  Parser->setTargetParser(*TAP);
  if (Parser->Run(false))
    return;

  handleSymverAliases(M, Streamer);

  for (auto &KV : Streamer) {
    StringRef Key = KV.first();
    RecordStreamer::State Value = KV.second;
    // FIXME: For now we just assume that all asm symbols are executable.
    uint32_t Res = BasicSymbolRef::SF_Executable;
    switch (Value) {
    case RecordStreamer::NeverSeen:
      llvm_unreachable("NeverSeen should have been replaced earlier");
    case RecordStreamer::DefinedGlobal:
      Res |= BasicSymbolRef::SF_Global;
      break;
    case RecordStreamer::Defined:
      break;
    case RecordStreamer::Global:
    case RecordStreamer::Used:
      Res |= BasicSymbolRef::SF_Undefined;
      Res |= BasicSymbolRef::SF_Global;
      break;
    case RecordStreamer::DefinedWeak:
      Res |= BasicSymbolRef::SF_Weak;
      Res |= BasicSymbolRef::SF_Global;
      break;
    case RecordStreamer::UndefinedWeak:
      Res |= BasicSymbolRef::SF_Weak;
      Res |= BasicSymbolRef::SF_Undefined;
    }
    AsmSymbol(Key, BasicSymbolRef::Flags(Res));
  }
}

void ModuleSymbolTable::printSymbolName(raw_ostream &OS, Symbol S) const {
  if (S.is<AsmSymbol *>()) {
    OS << S.get<AsmSymbol *>()->first;
    return;
  }

  auto *GV = S.get<GlobalValue *>();
  if (GV->hasDLLImportStorageClass())
    OS << "__imp_";

  Mang.getNameWithPrefix(OS, GV, false);
}

uint32_t ModuleSymbolTable::getSymbolFlags(Symbol S) const {
  if (S.is<AsmSymbol *>())
    return S.get<AsmSymbol *>()->second;

  auto *GV = S.get<GlobalValue *>();

  uint32_t Res = BasicSymbolRef::SF_None;
  if (GV->isDeclarationForLinker())
    Res |= BasicSymbolRef::SF_Undefined;
  else if (GV->hasHiddenVisibility() && !GV->hasLocalLinkage())
    Res |= BasicSymbolRef::SF_Hidden;
  if (const GlobalVariable *GVar = dyn_cast<GlobalVariable>(GV)) {
    if (GVar->isConstant())
      Res |= BasicSymbolRef::SF_Const;
  }
  if (dyn_cast_or_null<Function>(GV->getBaseObject()))
    Res |= BasicSymbolRef::SF_Executable;
  if (isa<GlobalAlias>(GV))
    Res |= BasicSymbolRef::SF_Indirect;
  if (GV->hasPrivateLinkage())
    Res |= BasicSymbolRef::SF_FormatSpecific;
  if (!GV->hasLocalLinkage())
    Res |= BasicSymbolRef::SF_Global;
  if (GV->hasCommonLinkage())
    Res |= BasicSymbolRef::SF_Common;
  if (GV->hasLinkOnceLinkage() || GV->hasWeakLinkage() ||
      GV->hasExternalWeakLinkage())
    Res |= BasicSymbolRef::SF_Weak;

  if (GV->getName().startswith("llvm."))
    Res |= BasicSymbolRef::SF_FormatSpecific;
  else if (auto *Var = dyn_cast<GlobalVariable>(GV)) {
    if (Var->getSection() == "llvm.metadata")
      Res |= BasicSymbolRef::SF_FormatSpecific;
  }

  return Res;
}
