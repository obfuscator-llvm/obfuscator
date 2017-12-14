//===- PDBExtras.cpp - helper functions and classes for PDBs --------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "llvm/DebugInfo/PDB/PDBExtras.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/DebugInfo/CodeView/Formatters.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;
using namespace llvm::pdb;

#define CASE_OUTPUT_ENUM_CLASS_STR(Class, Value, Str, Stream)                  \
  case Class::Value:                                                           \
    Stream << Str;                                                             \
    break;

#define CASE_OUTPUT_ENUM_CLASS_NAME(Class, Value, Stream)                      \
  CASE_OUTPUT_ENUM_CLASS_STR(Class, Value, #Value, Stream)

raw_ostream &llvm::pdb::operator<<(raw_ostream &OS,
                                   const PDB_VariantType &Type) {
  switch (Type) {
    CASE_OUTPUT_ENUM_CLASS_NAME(PDB_VariantType, Bool, OS)
    CASE_OUTPUT_ENUM_CLASS_NAME(PDB_VariantType, Single, OS)
    CASE_OUTPUT_ENUM_CLASS_NAME(PDB_VariantType, Double, OS)
    CASE_OUTPUT_ENUM_CLASS_NAME(PDB_VariantType, Int8, OS)
    CASE_OUTPUT_ENUM_CLASS_NAME(PDB_VariantType, Int16, OS)
    CASE_OUTPUT_ENUM_CLASS_NAME(PDB_VariantType, Int32, OS)
    CASE_OUTPUT_ENUM_CLASS_NAME(PDB_VariantType, Int64, OS)
    CASE_OUTPUT_ENUM_CLASS_NAME(PDB_VariantType, UInt8, OS)
    CASE_OUTPUT_ENUM_CLASS_NAME(PDB_VariantType, UInt16, OS)
    CASE_OUTPUT_ENUM_CLASS_NAME(PDB_VariantType, UInt32, OS)
    CASE_OUTPUT_ENUM_CLASS_NAME(PDB_VariantType, UInt64, OS)
    default:
      OS << "Unknown";
  }
  return OS;
}

raw_ostream &llvm::pdb::operator<<(raw_ostream &OS,
                                   const PDB_CallingConv &Conv) {
  OS << "__";
  switch (Conv) {
    CASE_OUTPUT_ENUM_CLASS_STR(PDB_CallingConv, NearC      , "cdecl", OS)
    CASE_OUTPUT_ENUM_CLASS_STR(PDB_CallingConv, FarC       , "cdecl", OS)
    CASE_OUTPUT_ENUM_CLASS_STR(PDB_CallingConv, NearPascal , "pascal", OS)
    CASE_OUTPUT_ENUM_CLASS_STR(PDB_CallingConv, FarPascal  , "pascal", OS)
    CASE_OUTPUT_ENUM_CLASS_STR(PDB_CallingConv, NearFast   , "fastcall", OS)
    CASE_OUTPUT_ENUM_CLASS_STR(PDB_CallingConv, FarFast    , "fastcall", OS)
    CASE_OUTPUT_ENUM_CLASS_STR(PDB_CallingConv, NearStdCall, "stdcall", OS)
    CASE_OUTPUT_ENUM_CLASS_STR(PDB_CallingConv, FarStdCall , "stdcall", OS)
    CASE_OUTPUT_ENUM_CLASS_STR(PDB_CallingConv, NearSysCall, "syscall", OS)
    CASE_OUTPUT_ENUM_CLASS_STR(PDB_CallingConv, FarSysCall , "syscall", OS)
    CASE_OUTPUT_ENUM_CLASS_STR(PDB_CallingConv, ThisCall   , "thiscall", OS)
    CASE_OUTPUT_ENUM_CLASS_STR(PDB_CallingConv, MipsCall   , "mipscall", OS)
    CASE_OUTPUT_ENUM_CLASS_STR(PDB_CallingConv, Generic    , "genericcall", OS)
    CASE_OUTPUT_ENUM_CLASS_STR(PDB_CallingConv, AlphaCall  , "alphacall", OS)
    CASE_OUTPUT_ENUM_CLASS_STR(PDB_CallingConv, PpcCall    , "ppccall", OS)
    CASE_OUTPUT_ENUM_CLASS_STR(PDB_CallingConv, SHCall     , "superhcall", OS)
    CASE_OUTPUT_ENUM_CLASS_STR(PDB_CallingConv, ArmCall    , "armcall", OS)
    CASE_OUTPUT_ENUM_CLASS_STR(PDB_CallingConv, AM33Call   , "am33call", OS)
    CASE_OUTPUT_ENUM_CLASS_STR(PDB_CallingConv, TriCall    , "tricall", OS)
    CASE_OUTPUT_ENUM_CLASS_STR(PDB_CallingConv, SH5Call    , "sh5call", OS)
    CASE_OUTPUT_ENUM_CLASS_STR(PDB_CallingConv, M32RCall   , "m32rcall", OS)
    CASE_OUTPUT_ENUM_CLASS_STR(PDB_CallingConv, ClrCall    , "clrcall", OS)
    CASE_OUTPUT_ENUM_CLASS_STR(PDB_CallingConv, Inline     , "inlinecall", OS)
    CASE_OUTPUT_ENUM_CLASS_STR(PDB_CallingConv, NearVector , "vectorcall", OS)
  }
  return OS;
}

raw_ostream &llvm::pdb::operator<<(raw_ostream &OS, const PDB_DataKind &Data) {
  switch (Data) {
    CASE_OUTPUT_ENUM_CLASS_STR(PDB_DataKind, Unknown, "unknown", OS)
    CASE_OUTPUT_ENUM_CLASS_STR(PDB_DataKind, Local, "local", OS)
    CASE_OUTPUT_ENUM_CLASS_STR(PDB_DataKind, StaticLocal, "static local", OS)
    CASE_OUTPUT_ENUM_CLASS_STR(PDB_DataKind, Param, "param", OS)
    CASE_OUTPUT_ENUM_CLASS_STR(PDB_DataKind, ObjectPtr, "this ptr", OS)
    CASE_OUTPUT_ENUM_CLASS_STR(PDB_DataKind, FileStatic, "static global", OS)
    CASE_OUTPUT_ENUM_CLASS_STR(PDB_DataKind, Global, "global", OS)
    CASE_OUTPUT_ENUM_CLASS_STR(PDB_DataKind, Member, "member", OS)
    CASE_OUTPUT_ENUM_CLASS_STR(PDB_DataKind, StaticMember, "static member", OS)
    CASE_OUTPUT_ENUM_CLASS_STR(PDB_DataKind, Constant, "const", OS)
  }
  return OS;
}

raw_ostream &llvm::pdb::operator<<(raw_ostream &OS,
                                   const codeview::RegisterId &Reg) {
  switch (Reg) {
    CASE_OUTPUT_ENUM_CLASS_NAME(codeview::RegisterId, AL, OS)
    CASE_OUTPUT_ENUM_CLASS_NAME(codeview::RegisterId, CL, OS)
    CASE_OUTPUT_ENUM_CLASS_NAME(codeview::RegisterId, DL, OS)
    CASE_OUTPUT_ENUM_CLASS_NAME(codeview::RegisterId, BL, OS)
    CASE_OUTPUT_ENUM_CLASS_NAME(codeview::RegisterId, AH, OS)
    CASE_OUTPUT_ENUM_CLASS_NAME(codeview::RegisterId, CH, OS)
    CASE_OUTPUT_ENUM_CLASS_NAME(codeview::RegisterId, DH, OS)
    CASE_OUTPUT_ENUM_CLASS_NAME(codeview::RegisterId, BH, OS)
    CASE_OUTPUT_ENUM_CLASS_NAME(codeview::RegisterId, AX, OS)
    CASE_OUTPUT_ENUM_CLASS_NAME(codeview::RegisterId, CX, OS)
    CASE_OUTPUT_ENUM_CLASS_NAME(codeview::RegisterId, DX, OS)
    CASE_OUTPUT_ENUM_CLASS_NAME(codeview::RegisterId, BX, OS)
    CASE_OUTPUT_ENUM_CLASS_NAME(codeview::RegisterId, SP, OS)
    CASE_OUTPUT_ENUM_CLASS_NAME(codeview::RegisterId, BP, OS)
    CASE_OUTPUT_ENUM_CLASS_NAME(codeview::RegisterId, SI, OS)
    CASE_OUTPUT_ENUM_CLASS_NAME(codeview::RegisterId, DI, OS)
    CASE_OUTPUT_ENUM_CLASS_NAME(codeview::RegisterId, EAX, OS)
    CASE_OUTPUT_ENUM_CLASS_NAME(codeview::RegisterId, ECX, OS)
    CASE_OUTPUT_ENUM_CLASS_NAME(codeview::RegisterId, EDX, OS)
    CASE_OUTPUT_ENUM_CLASS_NAME(codeview::RegisterId, EBX, OS)
    CASE_OUTPUT_ENUM_CLASS_NAME(codeview::RegisterId, ESP, OS)
    CASE_OUTPUT_ENUM_CLASS_NAME(codeview::RegisterId, EBP, OS)
    CASE_OUTPUT_ENUM_CLASS_NAME(codeview::RegisterId, ESI, OS)
    CASE_OUTPUT_ENUM_CLASS_NAME(codeview::RegisterId, EDI, OS)
    CASE_OUTPUT_ENUM_CLASS_NAME(codeview::RegisterId, ES, OS)
    CASE_OUTPUT_ENUM_CLASS_NAME(codeview::RegisterId, CS, OS)
    CASE_OUTPUT_ENUM_CLASS_NAME(codeview::RegisterId, SS, OS)
    CASE_OUTPUT_ENUM_CLASS_NAME(codeview::RegisterId, DS, OS)
    CASE_OUTPUT_ENUM_CLASS_NAME(codeview::RegisterId, FS, OS)
    CASE_OUTPUT_ENUM_CLASS_NAME(codeview::RegisterId, GS, OS)
    CASE_OUTPUT_ENUM_CLASS_NAME(codeview::RegisterId, IP, OS)
    CASE_OUTPUT_ENUM_CLASS_NAME(codeview::RegisterId, RAX, OS)
    CASE_OUTPUT_ENUM_CLASS_NAME(codeview::RegisterId, RBX, OS)
    CASE_OUTPUT_ENUM_CLASS_NAME(codeview::RegisterId, RCX, OS)
    CASE_OUTPUT_ENUM_CLASS_NAME(codeview::RegisterId, RDX, OS)
    CASE_OUTPUT_ENUM_CLASS_NAME(codeview::RegisterId, RSI, OS)
    CASE_OUTPUT_ENUM_CLASS_NAME(codeview::RegisterId, RDI, OS)
    CASE_OUTPUT_ENUM_CLASS_NAME(codeview::RegisterId, RBP, OS)
    CASE_OUTPUT_ENUM_CLASS_NAME(codeview::RegisterId, RSP, OS)
    CASE_OUTPUT_ENUM_CLASS_NAME(codeview::RegisterId, R8, OS)
    CASE_OUTPUT_ENUM_CLASS_NAME(codeview::RegisterId, R9, OS)
    CASE_OUTPUT_ENUM_CLASS_NAME(codeview::RegisterId, R10, OS)
    CASE_OUTPUT_ENUM_CLASS_NAME(codeview::RegisterId, R11, OS)
    CASE_OUTPUT_ENUM_CLASS_NAME(codeview::RegisterId, R12, OS)
    CASE_OUTPUT_ENUM_CLASS_NAME(codeview::RegisterId, R13, OS)
    CASE_OUTPUT_ENUM_CLASS_NAME(codeview::RegisterId, R14, OS)
    CASE_OUTPUT_ENUM_CLASS_NAME(codeview::RegisterId, R15, OS)
  default:
    OS << static_cast<int>(Reg);
  }
  return OS;
}

raw_ostream &llvm::pdb::operator<<(raw_ostream &OS, const PDB_LocType &Loc) {
  switch (Loc) {
    CASE_OUTPUT_ENUM_CLASS_STR(PDB_LocType, Static, "static", OS)
    CASE_OUTPUT_ENUM_CLASS_STR(PDB_LocType, TLS, "tls", OS)
    CASE_OUTPUT_ENUM_CLASS_STR(PDB_LocType, RegRel, "regrel", OS)
    CASE_OUTPUT_ENUM_CLASS_STR(PDB_LocType, ThisRel, "thisrel", OS)
    CASE_OUTPUT_ENUM_CLASS_STR(PDB_LocType, Enregistered, "register", OS)
    CASE_OUTPUT_ENUM_CLASS_STR(PDB_LocType, BitField, "bitfield", OS)
    CASE_OUTPUT_ENUM_CLASS_STR(PDB_LocType, Slot, "slot", OS)
    CASE_OUTPUT_ENUM_CLASS_STR(PDB_LocType, IlRel, "IL rel", OS)
    CASE_OUTPUT_ENUM_CLASS_STR(PDB_LocType, MetaData, "metadata", OS)
    CASE_OUTPUT_ENUM_CLASS_STR(PDB_LocType, Constant, "constant", OS)
  default:
    OS << "Unknown";
  }
  return OS;
}

raw_ostream &llvm::pdb::operator<<(raw_ostream &OS,
                                   const codeview::ThunkOrdinal &Thunk) {
  switch (Thunk) {
    CASE_OUTPUT_ENUM_CLASS_NAME(codeview::ThunkOrdinal, BranchIsland, OS)
    CASE_OUTPUT_ENUM_CLASS_NAME(codeview::ThunkOrdinal, Pcode, OS)
    CASE_OUTPUT_ENUM_CLASS_NAME(codeview::ThunkOrdinal, Standard, OS)
    CASE_OUTPUT_ENUM_CLASS_NAME(codeview::ThunkOrdinal, ThisAdjustor, OS)
    CASE_OUTPUT_ENUM_CLASS_NAME(codeview::ThunkOrdinal, TrampIncremental, OS)
    CASE_OUTPUT_ENUM_CLASS_NAME(codeview::ThunkOrdinal, UnknownLoad, OS)
    CASE_OUTPUT_ENUM_CLASS_NAME(codeview::ThunkOrdinal, Vcall, OS)
  }
  return OS;
}

raw_ostream &llvm::pdb::operator<<(raw_ostream &OS,
                                   const PDB_Checksum &Checksum) {
  switch (Checksum) {
    CASE_OUTPUT_ENUM_CLASS_NAME(PDB_Checksum, None, OS)
    CASE_OUTPUT_ENUM_CLASS_NAME(PDB_Checksum, MD5, OS)
    CASE_OUTPUT_ENUM_CLASS_NAME(PDB_Checksum, SHA1, OS)
  }
  return OS;
}

raw_ostream &llvm::pdb::operator<<(raw_ostream &OS, const PDB_Lang &Lang) {
  switch (Lang) {
    CASE_OUTPUT_ENUM_CLASS_NAME(PDB_Lang, C, OS)
    CASE_OUTPUT_ENUM_CLASS_STR(PDB_Lang, Cpp, "C++", OS)
    CASE_OUTPUT_ENUM_CLASS_NAME(PDB_Lang, Fortran, OS)
    CASE_OUTPUT_ENUM_CLASS_NAME(PDB_Lang, Masm, OS)
    CASE_OUTPUT_ENUM_CLASS_NAME(PDB_Lang, Pascal, OS)
    CASE_OUTPUT_ENUM_CLASS_NAME(PDB_Lang, Basic, OS)
    CASE_OUTPUT_ENUM_CLASS_NAME(PDB_Lang, Cobol, OS)
    CASE_OUTPUT_ENUM_CLASS_NAME(PDB_Lang, Link, OS)
    CASE_OUTPUT_ENUM_CLASS_NAME(PDB_Lang, Cvtres, OS)
    CASE_OUTPUT_ENUM_CLASS_NAME(PDB_Lang, Cvtpgd, OS)
    CASE_OUTPUT_ENUM_CLASS_NAME(PDB_Lang, CSharp, OS)
    CASE_OUTPUT_ENUM_CLASS_NAME(PDB_Lang, VB, OS)
    CASE_OUTPUT_ENUM_CLASS_NAME(PDB_Lang, ILAsm, OS)
    CASE_OUTPUT_ENUM_CLASS_NAME(PDB_Lang, Java, OS)
    CASE_OUTPUT_ENUM_CLASS_NAME(PDB_Lang, JScript, OS)
    CASE_OUTPUT_ENUM_CLASS_NAME(PDB_Lang, MSIL, OS)
    CASE_OUTPUT_ENUM_CLASS_NAME(PDB_Lang, HLSL, OS)
  }
  return OS;
}

raw_ostream &llvm::pdb::operator<<(raw_ostream &OS, const PDB_SymType &Tag) {
  switch (Tag) {
    CASE_OUTPUT_ENUM_CLASS_NAME(PDB_SymType, Exe, OS)
    CASE_OUTPUT_ENUM_CLASS_NAME(PDB_SymType, Compiland, OS)
    CASE_OUTPUT_ENUM_CLASS_NAME(PDB_SymType, CompilandDetails, OS)
    CASE_OUTPUT_ENUM_CLASS_NAME(PDB_SymType, CompilandEnv, OS)
    CASE_OUTPUT_ENUM_CLASS_NAME(PDB_SymType, Function, OS)
    CASE_OUTPUT_ENUM_CLASS_NAME(PDB_SymType, Block, OS)
    CASE_OUTPUT_ENUM_CLASS_NAME(PDB_SymType, Data, OS)
    CASE_OUTPUT_ENUM_CLASS_NAME(PDB_SymType, Annotation, OS)
    CASE_OUTPUT_ENUM_CLASS_NAME(PDB_SymType, Label, OS)
    CASE_OUTPUT_ENUM_CLASS_NAME(PDB_SymType, PublicSymbol, OS)
    CASE_OUTPUT_ENUM_CLASS_NAME(PDB_SymType, UDT, OS)
    CASE_OUTPUT_ENUM_CLASS_NAME(PDB_SymType, Enum, OS)
    CASE_OUTPUT_ENUM_CLASS_NAME(PDB_SymType, FunctionSig, OS)
    CASE_OUTPUT_ENUM_CLASS_NAME(PDB_SymType, PointerType, OS)
    CASE_OUTPUT_ENUM_CLASS_NAME(PDB_SymType, ArrayType, OS)
    CASE_OUTPUT_ENUM_CLASS_NAME(PDB_SymType, BuiltinType, OS)
    CASE_OUTPUT_ENUM_CLASS_NAME(PDB_SymType, Typedef, OS)
    CASE_OUTPUT_ENUM_CLASS_NAME(PDB_SymType, BaseClass, OS)
    CASE_OUTPUT_ENUM_CLASS_NAME(PDB_SymType, Friend, OS)
    CASE_OUTPUT_ENUM_CLASS_NAME(PDB_SymType, FunctionArg, OS)
    CASE_OUTPUT_ENUM_CLASS_NAME(PDB_SymType, FuncDebugStart, OS)
    CASE_OUTPUT_ENUM_CLASS_NAME(PDB_SymType, FuncDebugEnd, OS)
    CASE_OUTPUT_ENUM_CLASS_NAME(PDB_SymType, UsingNamespace, OS)
    CASE_OUTPUT_ENUM_CLASS_NAME(PDB_SymType, VTableShape, OS)
    CASE_OUTPUT_ENUM_CLASS_NAME(PDB_SymType, VTable, OS)
    CASE_OUTPUT_ENUM_CLASS_NAME(PDB_SymType, Custom, OS)
    CASE_OUTPUT_ENUM_CLASS_NAME(PDB_SymType, Thunk, OS)
    CASE_OUTPUT_ENUM_CLASS_NAME(PDB_SymType, CustomType, OS)
    CASE_OUTPUT_ENUM_CLASS_NAME(PDB_SymType, ManagedType, OS)
    CASE_OUTPUT_ENUM_CLASS_NAME(PDB_SymType, Dimension, OS)
  default:
    OS << "Unknown";
  }
  return OS;
}

raw_ostream &llvm::pdb::operator<<(raw_ostream &OS,
                                   const PDB_MemberAccess &Access) {
  switch (Access) {
    CASE_OUTPUT_ENUM_CLASS_STR(PDB_MemberAccess, Public, "public", OS)
    CASE_OUTPUT_ENUM_CLASS_STR(PDB_MemberAccess, Protected, "protected", OS)
    CASE_OUTPUT_ENUM_CLASS_STR(PDB_MemberAccess, Private, "private", OS)
  }
  return OS;
}

raw_ostream &llvm::pdb::operator<<(raw_ostream &OS, const PDB_UdtType &Type) {
  switch (Type) {
    CASE_OUTPUT_ENUM_CLASS_STR(PDB_UdtType, Class, "class", OS)
    CASE_OUTPUT_ENUM_CLASS_STR(PDB_UdtType, Struct, "struct", OS)
    CASE_OUTPUT_ENUM_CLASS_STR(PDB_UdtType, Interface, "interface", OS)
    CASE_OUTPUT_ENUM_CLASS_STR(PDB_UdtType, Union, "union", OS)
  }
  return OS;
}

raw_ostream &llvm::pdb::operator<<(raw_ostream &OS,
                                   const PDB_Machine &Machine) {
  switch (Machine) {
    CASE_OUTPUT_ENUM_CLASS_NAME(PDB_Machine, Am33, OS)
    CASE_OUTPUT_ENUM_CLASS_NAME(PDB_Machine, Amd64, OS)
    CASE_OUTPUT_ENUM_CLASS_NAME(PDB_Machine, Arm, OS)
    CASE_OUTPUT_ENUM_CLASS_NAME(PDB_Machine, ArmNT, OS)
    CASE_OUTPUT_ENUM_CLASS_NAME(PDB_Machine, Ebc, OS)
    CASE_OUTPUT_ENUM_CLASS_NAME(PDB_Machine, x86, OS)
    CASE_OUTPUT_ENUM_CLASS_NAME(PDB_Machine, Ia64, OS)
    CASE_OUTPUT_ENUM_CLASS_NAME(PDB_Machine, M32R, OS)
    CASE_OUTPUT_ENUM_CLASS_NAME(PDB_Machine, Mips16, OS)
    CASE_OUTPUT_ENUM_CLASS_NAME(PDB_Machine, MipsFpu, OS)
    CASE_OUTPUT_ENUM_CLASS_NAME(PDB_Machine, MipsFpu16, OS)
    CASE_OUTPUT_ENUM_CLASS_NAME(PDB_Machine, PowerPC, OS)
    CASE_OUTPUT_ENUM_CLASS_NAME(PDB_Machine, PowerPCFP, OS)
    CASE_OUTPUT_ENUM_CLASS_NAME(PDB_Machine, R4000, OS)
    CASE_OUTPUT_ENUM_CLASS_NAME(PDB_Machine, SH3, OS)
    CASE_OUTPUT_ENUM_CLASS_NAME(PDB_Machine, SH3DSP, OS)
    CASE_OUTPUT_ENUM_CLASS_NAME(PDB_Machine, SH4, OS)
    CASE_OUTPUT_ENUM_CLASS_NAME(PDB_Machine, SH5, OS)
    CASE_OUTPUT_ENUM_CLASS_NAME(PDB_Machine, Thumb, OS)
    CASE_OUTPUT_ENUM_CLASS_NAME(PDB_Machine, WceMipsV2, OS)
  default:
    OS << "Unknown";
  }
  return OS;
}

raw_ostream &llvm::pdb::operator<<(raw_ostream &OS, const Variant &Value) {
  switch (Value.Type) {
    case PDB_VariantType::Bool:
      OS << (Value.Value.Bool ? "true" : "false");
      break;
    case PDB_VariantType::Double:
      OS << Value.Value.Double;
      break;
    case PDB_VariantType::Int16:
      OS << Value.Value.Int16;
      break;
    case PDB_VariantType::Int32:
      OS << Value.Value.Int32;
      break;
    case PDB_VariantType::Int64:
      OS << Value.Value.Int64;
      break;
    case PDB_VariantType::Int8:
      OS << static_cast<int>(Value.Value.Int8);
      break;
    case PDB_VariantType::Single:
      OS << Value.Value.Single;
      break;
    case PDB_VariantType::UInt16:
      OS << Value.Value.Double;
      break;
    case PDB_VariantType::UInt32:
      OS << Value.Value.UInt32;
      break;
    case PDB_VariantType::UInt64:
      OS << Value.Value.UInt64;
      break;
    case PDB_VariantType::UInt8:
      OS << static_cast<unsigned>(Value.Value.UInt8);
      break;
    case PDB_VariantType::String:
      OS << Value.Value.String;
      break;
    default:
      OS << Value.Type;
  }
  return OS;
}

raw_ostream &llvm::pdb::operator<<(raw_ostream &OS,
                                   const VersionInfo &Version) {
  OS << Version.Major << "." << Version.Minor << "." << Version.Build;
  return OS;
}

raw_ostream &llvm::pdb::operator<<(raw_ostream &OS, const TagStats &Stats) {
  for (auto Tag : Stats) {
    OS << Tag.first << ":" << Tag.second << " ";
  }
  return OS;
}
