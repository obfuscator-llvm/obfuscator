//===- PDBTypes.h - Defines enums for various fields contained in PDB ----====//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_DEBUGINFO_PDB_PDBTYPES_H
#define LLVM_DEBUGINFO_PDB_PDBTYPES_H

#include "llvm/DebugInfo/CodeView/CodeView.h"
#include "llvm/DebugInfo/PDB/IPDBEnumChildren.h"
#include "llvm/DebugInfo/PDB/Native/RawTypes.h"
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>

namespace llvm {
namespace pdb {

class IPDBDataStream;
class IPDBLineNumber;
class IPDBSourceFile;
class PDBSymDumper;
class PDBSymbol;
class PDBSymbolExe;
class PDBSymbolCompiland;
class PDBSymbolCompilandDetails;
class PDBSymbolCompilandEnv;
class PDBSymbolFunc;
class PDBSymbolBlock;
class PDBSymbolData;
class PDBSymbolAnnotation;
class PDBSymbolLabel;
class PDBSymbolPublicSymbol;
class PDBSymbolTypeUDT;
class PDBSymbolTypeEnum;
class PDBSymbolTypeFunctionSig;
class PDBSymbolTypePointer;
class PDBSymbolTypeArray;
class PDBSymbolTypeBuiltin;
class PDBSymbolTypeTypedef;
class PDBSymbolTypeBaseClass;
class PDBSymbolTypeFriend;
class PDBSymbolTypeFunctionArg;
class PDBSymbolFuncDebugStart;
class PDBSymbolFuncDebugEnd;
class PDBSymbolUsingNamespace;
class PDBSymbolTypeVTableShape;
class PDBSymbolTypeVTable;
class PDBSymbolCustom;
class PDBSymbolThunk;
class PDBSymbolTypeCustom;
class PDBSymbolTypeManaged;
class PDBSymbolTypeDimension;
class PDBSymbolUnknown;

using IPDBEnumSymbols = IPDBEnumChildren<PDBSymbol>;
using IPDBEnumSourceFiles = IPDBEnumChildren<IPDBSourceFile>;
using IPDBEnumDataStreams = IPDBEnumChildren<IPDBDataStream>;
using IPDBEnumLineNumbers = IPDBEnumChildren<IPDBLineNumber>;

/// Specifies which PDB reader implementation is to be used.  Only a value
/// of PDB_ReaderType::DIA is currently supported, but Native is in the works.
enum class PDB_ReaderType {
  DIA = 0,
  Native = 1,
};

/// An enumeration indicating the type of data contained in this table.
enum class PDB_TableType {
  Symbols,
  SourceFiles,
  LineNumbers,
  SectionContribs,
  Segments,
  InjectedSources,
  FrameData
};

/// Defines flags used for enumerating child symbols.  This corresponds to the
/// NameSearchOptions enumeration which is documented here:
/// https://msdn.microsoft.com/en-us/library/yat28ads.aspx
enum PDB_NameSearchFlags {
  NS_Default = 0x0,
  NS_CaseSensitive = 0x1,
  NS_CaseInsensitive = 0x2,
  NS_FileNameExtMatch = 0x4,
  NS_Regex = 0x8,
  NS_UndecoratedName = 0x10
};

/// Specifies the hash algorithm that a source file from a PDB was hashed with.
/// This corresponds to the CV_SourceChksum_t enumeration and are documented
/// here: https://msdn.microsoft.com/en-us/library/e96az21x.aspx
enum class PDB_Checksum { None = 0, MD5 = 1, SHA1 = 2 };

/// These values correspond to the CV_CPU_TYPE_e enumeration, and are documented
/// here: https://msdn.microsoft.com/en-us/library/b2fc64ek.aspx
using PDB_Cpu = codeview::CPUType;

enum class PDB_Machine {
  Invalid = 0xffff,
  Unknown = 0x0,
  Am33 = 0x13,
  Amd64 = 0x8664,
  Arm = 0x1C0,
  ArmNT = 0x1C4,
  Ebc = 0xEBC,
  x86 = 0x14C,
  Ia64 = 0x200,
  M32R = 0x9041,
  Mips16 = 0x266,
  MipsFpu = 0x366,
  MipsFpu16 = 0x466,
  PowerPC = 0x1F0,
  PowerPCFP = 0x1F1,
  R4000 = 0x166,
  SH3 = 0x1A2,
  SH3DSP = 0x1A3,
  SH4 = 0x1A6,
  SH5 = 0x1A8,
  Thumb = 0x1C2,
  WceMipsV2 = 0x169
};

/// These values correspond to the CV_call_e enumeration, and are documented
/// at the following locations:
///   https://msdn.microsoft.com/en-us/library/b2fc64ek.aspx
///   https://msdn.microsoft.com/en-us/library/windows/desktop/ms680207(v=vs.85).aspx
using PDB_CallingConv = codeview::CallingConvention;

/// These values correspond to the CV_CFL_LANG enumeration, and are documented
/// here: https://msdn.microsoft.com/en-us/library/bw3aekw6.aspx
using PDB_Lang = codeview::SourceLanguage;

/// These values correspond to the DataKind enumeration, and are documented
/// here: https://msdn.microsoft.com/en-us/library/b2x2t313.aspx
enum class PDB_DataKind {
  Unknown,
  Local,
  StaticLocal,
  Param,
  ObjectPtr,
  FileStatic,
  Global,
  Member,
  StaticMember,
  Constant
};

/// These values correspond to the SymTagEnum enumeration, and are documented
/// here: https://msdn.microsoft.com/en-us/library/bkedss5f.aspx
enum class PDB_SymType {
  None,
  Exe,
  Compiland,
  CompilandDetails,
  CompilandEnv,
  Function,
  Block,
  Data,
  Annotation,
  Label,
  PublicSymbol,
  UDT,
  Enum,
  FunctionSig,
  PointerType,
  ArrayType,
  BuiltinType,
  Typedef,
  BaseClass,
  Friend,
  FunctionArg,
  FuncDebugStart,
  FuncDebugEnd,
  UsingNamespace,
  VTableShape,
  VTable,
  Custom,
  Thunk,
  CustomType,
  ManagedType,
  Dimension,
  Max
};

/// These values correspond to the LocationType enumeration, and are documented
/// here: https://msdn.microsoft.com/en-us/library/f57kaez3.aspx
enum class PDB_LocType {
  Null,
  Static,
  TLS,
  RegRel,
  ThisRel,
  Enregistered,
  BitField,
  Slot,
  IlRel,
  MetaData,
  Constant,
  Max
};

/// These values correspond to the UdtKind enumeration, and are documented
/// here: https://msdn.microsoft.com/en-us/library/wcstk66t.aspx
enum class PDB_UdtType { Struct, Class, Union, Interface };

/// These values correspond to the StackFrameTypeEnum enumeration, and are
/// documented here: https://msdn.microsoft.com/en-us/library/bc5207xw.aspx.
enum class PDB_StackFrameType { FPO, KernelTrap, KernelTSS, EBP, FrameData };

/// These values correspond to the StackFrameTypeEnum enumeration, and are
/// documented here: https://msdn.microsoft.com/en-us/library/bc5207xw.aspx.
enum class PDB_MemoryType { Code, Data, Stack, HeapCode };

/// These values correspond to the Basictype enumeration, and are documented
/// here: https://msdn.microsoft.com/en-us/library/4szdtzc3.aspx
enum class PDB_BuiltinType {
  None = 0,
  Void = 1,
  Char = 2,
  WCharT = 3,
  Int = 6,
  UInt = 7,
  Float = 8,
  BCD = 9,
  Bool = 10,
  Long = 13,
  ULong = 14,
  Currency = 25,
  Date = 26,
  Variant = 27,
  Complex = 28,
  Bitfield = 29,
  BSTR = 30,
  HResult = 31
};

enum class PDB_MemberAccess { Private = 1, Protected = 2, Public = 3 };

struct VersionInfo {
  uint32_t Major;
  uint32_t Minor;
  uint32_t Build;
  uint32_t QFE;
};

enum PDB_VariantType {
  Empty,
  Unknown,
  Int8,
  Int16,
  Int32,
  Int64,
  Single,
  Double,
  UInt8,
  UInt16,
  UInt32,
  UInt64,
  Bool,
  String
};

struct Variant {
  Variant() = default;

  Variant(const Variant &Other) {
    *this = Other;
  }

  ~Variant() {
    if (Type == PDB_VariantType::String)
      delete[] Value.String;
  }

  PDB_VariantType Type = PDB_VariantType::Empty;
  union {
    bool Bool;
    int8_t Int8;
    int16_t Int16;
    int32_t Int32;
    int64_t Int64;
    float Single;
    double Double;
    uint8_t UInt8;
    uint16_t UInt16;
    uint32_t UInt32;
    uint64_t UInt64;
    char *String;
  } Value;

#define VARIANT_EQUAL_CASE(Enum)                                               \
  case PDB_VariantType::Enum:                                                  \
    return Value.Enum == Other.Value.Enum;

  bool operator==(const Variant &Other) const {
    if (Type != Other.Type)
      return false;
    switch (Type) {
      VARIANT_EQUAL_CASE(Bool)
      VARIANT_EQUAL_CASE(Int8)
      VARIANT_EQUAL_CASE(Int16)
      VARIANT_EQUAL_CASE(Int32)
      VARIANT_EQUAL_CASE(Int64)
      VARIANT_EQUAL_CASE(Single)
      VARIANT_EQUAL_CASE(Double)
      VARIANT_EQUAL_CASE(UInt8)
      VARIANT_EQUAL_CASE(UInt16)
      VARIANT_EQUAL_CASE(UInt32)
      VARIANT_EQUAL_CASE(UInt64)
      VARIANT_EQUAL_CASE(String)
    default:
      return true;
    }
  }

#undef VARIANT_EQUAL_CASE

  bool operator!=(const Variant &Other) const { return !(*this == Other); }
  Variant &operator=(const Variant &Other) {
    if (this == &Other)
      return *this;
    if (Type == PDB_VariantType::String)
      delete[] Value.String;
    Type = Other.Type;
    Value = Other.Value;
    if (Other.Type == PDB_VariantType::String &&
        Other.Value.String != nullptr) {
      Value.String = new char[strlen(Other.Value.String) + 1];
      ::strcpy(Value.String, Other.Value.String);
    }
    return *this;
  }
};

} // end namespace pdb
} // end namespace llvm

namespace std {

template <> struct hash<llvm::pdb::PDB_SymType> {
  using argument_type = llvm::pdb::PDB_SymType;
  using result_type = std::size_t;

  result_type operator()(const argument_type &Arg) const {
    return std::hash<int>()(static_cast<int>(Arg));
  }
};

} // end namespace std

#endif // LLVM_DEBUGINFO_PDB_PDBTYPES_H
