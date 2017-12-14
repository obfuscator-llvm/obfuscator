//===------ utils/wasm2yaml.cpp - obj2yaml conversion tool ------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "obj2yaml.h"
#include "llvm/Object/COFF.h"
#include "llvm/ObjectYAML/WasmYAML.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/YAMLTraits.h"

using namespace llvm;
using object::WasmSection;

namespace {

class WasmDumper {
  const object::WasmObjectFile &Obj;

public:
  WasmDumper(const object::WasmObjectFile &O) : Obj(O) {}

  ErrorOr<WasmYAML::Object *> dump();

  std::unique_ptr<WasmYAML::CustomSection>
  dumpCustomSection(const WasmSection &WasmSec);
};

} // namespace

static WasmYAML::Table make_table(const wasm::WasmTable &Table) {
  WasmYAML::Table T;
  T.ElemType = Table.ElemType;
  T.TableLimits.Flags = Table.Limits.Flags;
  T.TableLimits.Initial = Table.Limits.Initial;
  T.TableLimits.Maximum = Table.Limits.Maximum;
  return T;
}

static WasmYAML::Limits make_limits(const wasm::WasmLimits &Limits) {
  WasmYAML::Limits L;
  L.Flags = Limits.Flags;
  L.Initial = Limits.Initial;
  L.Maximum = Limits.Maximum;
  return L;
}

std::unique_ptr<WasmYAML::CustomSection> WasmDumper::dumpCustomSection(const WasmSection &WasmSec) {
  std::unique_ptr<WasmYAML::CustomSection> CustomSec;
  if (WasmSec.Name == "name") {
    std::unique_ptr<WasmYAML::NameSection> NameSec = make_unique<WasmYAML::NameSection>();
    for (const object::SymbolRef& Sym: Obj.symbols()) {
      uint32_t Flags = Sym.getFlags();
      // Skip over symbols that come from imports or exports
      if (Flags &
          (object::SymbolRef::SF_Global | object::SymbolRef::SF_Undefined))
        continue;
      Expected<StringRef> NameOrError = Sym.getName();
      if (!NameOrError)
        continue;
      WasmYAML::NameEntry NameEntry;
      NameEntry.Name = *NameOrError;
      NameEntry.Index = Sym.getValue();
      NameSec->FunctionNames.push_back(NameEntry);
    }
    CustomSec = std::move(NameSec);
  } else if (WasmSec.Name == "linking") {
    std::unique_ptr<WasmYAML::LinkingSection> LinkingSec = make_unique<WasmYAML::LinkingSection>();
    for (const object::SymbolRef& Sym: Obj.symbols()) {
      const object::WasmSymbol Symbol = Obj.getWasmSymbol(Sym);
      if (Symbol.Flags != 0) {
        WasmYAML::SymbolInfo Info = { Symbol.Name, Symbol.Flags };
        LinkingSec->SymbolInfos.push_back(Info);
      }
    }
    LinkingSec->DataSize = Obj.linkingData().DataSize;
    LinkingSec->DataAlignment = Obj.linkingData().DataAlignment;
    CustomSec = std::move(LinkingSec);
  } else {
    CustomSec = make_unique<WasmYAML::CustomSection>(WasmSec.Name);
  }
  CustomSec->Payload = yaml::BinaryRef(WasmSec.Content);
  return CustomSec;
}

ErrorOr<WasmYAML::Object *> WasmDumper::dump() {
  auto Y = make_unique<WasmYAML::Object>();

  // Dump header
  Y->Header.Version = Obj.getHeader().Version;

  // Dump sections
  for (const auto &Sec : Obj.sections()) {
    const WasmSection &WasmSec = Obj.getWasmSection(Sec);
    std::unique_ptr<WasmYAML::Section> S;
    switch (WasmSec.Type) {
    case wasm::WASM_SEC_CUSTOM: {
      if (WasmSec.Name.startswith("reloc.")) {
        // Relocations are attached the sections they apply to rather than
        // being represented as a custom section in the YAML output.
        continue;
      }
      S = dumpCustomSection(WasmSec);
      break;
    }
    case wasm::WASM_SEC_TYPE: {
      auto TypeSec = make_unique<WasmYAML::TypeSection>();
      uint32_t Index = 0;
      for (const auto &FunctionSig : Obj.types()) {
        WasmYAML::Signature Sig;
        Sig.Index = Index++;
        Sig.ReturnType = FunctionSig.ReturnType;
        for (const auto &ParamType : FunctionSig.ParamTypes)
          Sig.ParamTypes.push_back(ParamType);
        TypeSec->Signatures.push_back(Sig);
      }
      S = std::move(TypeSec);
      break;
    }
    case wasm::WASM_SEC_IMPORT: {
      auto ImportSec = make_unique<WasmYAML::ImportSection>();
      for (auto &Import : Obj.imports()) {
        WasmYAML::Import Im;
        Im.Module = Import.Module;
        Im.Field = Import.Field;
        Im.Kind = Import.Kind;
        switch (Im.Kind) {
        case wasm::WASM_EXTERNAL_FUNCTION:
          Im.SigIndex = Import.SigIndex;
          break;
        case wasm::WASM_EXTERNAL_GLOBAL:
          Im.GlobalImport.Type = Import.Global.Type;
          Im.GlobalImport.Mutable = Import.Global.Mutable;
          break;
        case wasm::WASM_EXTERNAL_TABLE:
          Im.TableImport = make_table(Import.Table);
          break;
        case wasm::WASM_EXTERNAL_MEMORY:
          Im.Memory = make_limits(Import.Memory);
          break;
        }
        ImportSec->Imports.push_back(Im);
      }
      S = std::move(ImportSec);
      break;
    }
    case wasm::WASM_SEC_FUNCTION: {
      auto FuncSec = make_unique<WasmYAML::FunctionSection>();
      for (const auto &Func : Obj.functionTypes()) {
        FuncSec->FunctionTypes.push_back(Func);
      }
      S = std::move(FuncSec);
      break;
    }
    case wasm::WASM_SEC_TABLE: {
      auto TableSec = make_unique<WasmYAML::TableSection>();
      for (const wasm::WasmTable &Table : Obj.tables()) {
        TableSec->Tables.push_back(make_table(Table));
      }
      S = std::move(TableSec);
      break;
    }
    case wasm::WASM_SEC_MEMORY: {
      auto MemorySec = make_unique<WasmYAML::MemorySection>();
      for (const wasm::WasmLimits &Memory : Obj.memories()) {
        MemorySec->Memories.push_back(make_limits(Memory));
      }
      S = std::move(MemorySec);
      break;
    }
    case wasm::WASM_SEC_GLOBAL: {
      auto GlobalSec = make_unique<WasmYAML::GlobalSection>();
      for (auto &Global : Obj.globals()) {
        WasmYAML::Global G;
        G.Type = Global.Type;
        G.Mutable = Global.Mutable;
        G.InitExpr = Global.InitExpr;
        GlobalSec->Globals.push_back(G);
      }
      S = std::move(GlobalSec);
      break;
    }
    case wasm::WASM_SEC_START: {
      auto StartSec = make_unique<WasmYAML::StartSection>();
      StartSec->StartFunction = Obj.startFunction();
      S = std::move(StartSec);
      break;
    }
    case wasm::WASM_SEC_EXPORT: {
      auto ExportSec = make_unique<WasmYAML::ExportSection>();
      for (auto &Export : Obj.exports()) {
        WasmYAML::Export Ex;
        Ex.Name = Export.Name;
        Ex.Kind = Export.Kind;
        Ex.Index = Export.Index;
        ExportSec->Exports.push_back(Ex);
      }
      S = std::move(ExportSec);
      break;
    }
    case wasm::WASM_SEC_ELEM: {
      auto ElemSec = make_unique<WasmYAML::ElemSection>();
      for (auto &Segment : Obj.elements()) {
        WasmYAML::ElemSegment Seg;
        Seg.TableIndex = Segment.TableIndex;
        Seg.Offset = Segment.Offset;
        for (auto &Func : Segment.Functions) {
          Seg.Functions.push_back(Func);
        }
        ElemSec->Segments.push_back(Seg);
      }
      S = std::move(ElemSec);
      break;
    }
    case wasm::WASM_SEC_CODE: {
      auto CodeSec = make_unique<WasmYAML::CodeSection>();
      for (auto &Func : Obj.functions()) {
        WasmYAML::Function Function;
        for (auto &Local : Func.Locals) {
          WasmYAML::LocalDecl LocalDecl;
          LocalDecl.Type = Local.Type;
          LocalDecl.Count = Local.Count;
          Function.Locals.push_back(LocalDecl);
        }
        Function.Body = yaml::BinaryRef(Func.Body);
        CodeSec->Functions.push_back(Function);
      }
      S = std::move(CodeSec);
      break;
    }
    case wasm::WASM_SEC_DATA: {
      auto DataSec = make_unique<WasmYAML::DataSection>();
      for (auto &Segment : Obj.dataSegments()) {
        WasmYAML::DataSegment Seg;
        Seg.SectionOffset = Segment.SectionOffset;
        Seg.MemoryIndex = Segment.Data.MemoryIndex;
        Seg.Offset = Segment.Data.Offset;
        Seg.Content = yaml::BinaryRef(Segment.Data.Content);
        DataSec->Segments.push_back(Seg);
      }
      S = std::move(DataSec);
      break;
    }
    default:
      llvm_unreachable("Unknown section type");
      break;
    }
    for (const wasm::WasmRelocation &Reloc: WasmSec.Relocations) {
      WasmYAML::Relocation R;
      R.Type = Reloc.Type;
      R.Index = Reloc.Index;
      R.Offset = Reloc.Offset;
      R.Addend = Reloc.Addend;
      S->Relocations.push_back(R);
    }
    Y->Sections.push_back(std::move(S));
  }

  return Y.release();
}

std::error_code wasm2yaml(raw_ostream &Out, const object::WasmObjectFile &Obj) {
  WasmDumper Dumper(Obj);
  ErrorOr<WasmYAML::Object *> YAMLOrErr = Dumper.dump();
  if (std::error_code EC = YAMLOrErr.getError())
    return EC;

  std::unique_ptr<WasmYAML::Object> YAML(YAMLOrErr.get());
  yaml::Output Yout(Out);
  Yout << *YAML;

  return std::error_code();
}
