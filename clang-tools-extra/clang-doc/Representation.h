///===-- Representation.h - ClangDoc Representation -------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file defines the internal representations of different declaration
// types for the clang-doc tool.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANG_DOC_REPRESENTATION_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANG_DOC_REPRESENTATION_H

#include "clang/AST/Type.h"
#include "clang/Basic/Specifiers.h"
#include "clang/Tooling/StandaloneExecution.h"
#include "llvm/ADT/Optional.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringExtras.h"
#include <array>
#include <string>

namespace clang {
namespace doc {

// SHA1'd hash of a USR.
using SymbolID = std::array<uint8_t, 20>;

struct Info;
struct FunctionInfo;
struct EnumInfo;

enum class InfoType {
  IT_default,
  IT_namespace,
  IT_record,
  IT_function,
  IT_enum
};

// A representation of a parsed comment.
struct CommentInfo {
  CommentInfo() = default;
  CommentInfo(CommentInfo &Other) = delete;
  CommentInfo(CommentInfo &&Other) = default;
  CommentInfo &operator=(CommentInfo &&Other) = default;

  bool operator==(const CommentInfo &Other) const {
    auto FirstCI = std::tie(Kind, Text, Name, Direction, ParamName, CloseName,
                            SelfClosing, Explicit, AttrKeys, AttrValues, Args);
    auto SecondCI =
        std::tie(Other.Kind, Other.Text, Other.Name, Other.Direction,
                 Other.ParamName, Other.CloseName, Other.SelfClosing,
                 Other.Explicit, Other.AttrKeys, Other.AttrValues, Other.Args);

    if (FirstCI != SecondCI || Children.size() != Other.Children.size())
      return false;

    return std::equal(Children.begin(), Children.end(), Other.Children.begin(),
                      llvm::deref<llvm::equal>{});
  }

  // This operator is used to sort a vector of CommentInfos.
  // No specific order (attributes more important than others) is required. Any
  // sort is enough, the order is only needed to call std::unique after sorting
  // the vector.
  bool operator<(const CommentInfo &Other) const {
    auto FirstCI = std::tie(Kind, Text, Name, Direction, ParamName, CloseName,
                            SelfClosing, Explicit, AttrKeys, AttrValues, Args);
    auto SecondCI =
        std::tie(Other.Kind, Other.Text, Other.Name, Other.Direction,
                 Other.ParamName, Other.CloseName, Other.SelfClosing,
                 Other.Explicit, Other.AttrKeys, Other.AttrValues, Other.Args);

    if (FirstCI < SecondCI)
      return true;

    if (FirstCI == SecondCI) {
      return std::lexicographical_compare(
          Children.begin(), Children.end(), Other.Children.begin(),
          Other.Children.end(), llvm::deref<llvm::less>());
    }

    return false;
  }

  SmallString<16>
      Kind; // Kind of comment (FullComment, ParagraphComment, TextComment,
            // InlineCommandComment, HTMLStartTagComment, HTMLEndTagComment,
            // BlockCommandComment, ParamCommandComment,
            // TParamCommandComment, VerbatimBlockComment,
            // VerbatimBlockLineComment, VerbatimLineComment).
  SmallString<64> Text;      // Text of the comment.
  SmallString<16> Name;      // Name of the comment (for Verbatim and HTML).
  SmallString<8> Direction;  // Parameter direction (for (T)ParamCommand).
  SmallString<16> ParamName; // Parameter name (for (T)ParamCommand).
  SmallString<16> CloseName; // Closing tag name (for VerbatimBlock).
  bool SelfClosing = false;  // Indicates if tag is self-closing (for HTML).
  bool Explicit = false; // Indicates if the direction of a param is explicit
                         // (for (T)ParamCommand).
  llvm::SmallVector<SmallString<16>, 4>
      AttrKeys; // List of attribute keys (for HTML).
  llvm::SmallVector<SmallString<16>, 4>
      AttrValues; // List of attribute values for each key (for HTML).
  llvm::SmallVector<SmallString<16>, 4>
      Args; // List of arguments to commands (for InlineCommand).
  std::vector<std::unique_ptr<CommentInfo>>
      Children; // List of child comments for this CommentInfo.
};

struct Reference {
  Reference() = default;
  Reference(llvm::StringRef Name) : Name(Name) {}
  Reference(llvm::StringRef Name, StringRef Path) : Name(Name), Path(Path) {}
  Reference(SymbolID USR, StringRef Name, InfoType IT)
      : USR(USR), Name(Name), RefType(IT) {}
  Reference(SymbolID USR, StringRef Name, InfoType IT, StringRef Path)
      : USR(USR), Name(Name), RefType(IT), Path(Path) {}

  bool operator==(const Reference &Other) const {
    return std::tie(USR, Name, RefType) ==
           std::tie(Other.USR, Other.Name, Other.RefType);
  }

  SymbolID USR = SymbolID(); // Unique identifer for referenced decl
  SmallString<16> Name;      // Name of type (possibly unresolved).
  InfoType RefType = InfoType::IT_default; // Indicates the type of this
                                           // Reference (namespace, record,
                                           // function, enum, default).
  llvm::SmallString<128> Path; // Path of directory where the clang-doc
                               // generated file will be saved
};

// A base struct for TypeInfos
struct TypeInfo {
  TypeInfo() = default;
  TypeInfo(SymbolID Type, StringRef Field, InfoType IT)
      : Type(Type, Field, IT) {}
  TypeInfo(SymbolID Type, StringRef Field, InfoType IT, StringRef Path)
      : Type(Type, Field, IT, Path) {}
  TypeInfo(llvm::StringRef RefName) : Type(RefName) {}
  TypeInfo(llvm::StringRef RefName, StringRef Path) : Type(RefName, Path) {}

  bool operator==(const TypeInfo &Other) const { return Type == Other.Type; }

  Reference Type; // Referenced type in this info.
};

// Info for field types.
struct FieldTypeInfo : public TypeInfo {
  FieldTypeInfo() = default;
  FieldTypeInfo(SymbolID Type, StringRef Field, InfoType IT, StringRef Path,
                llvm::StringRef Name)
      : TypeInfo(Type, Field, IT, Path), Name(Name) {}
  FieldTypeInfo(llvm::StringRef RefName, llvm::StringRef Name)
      : TypeInfo(RefName), Name(Name) {}
  FieldTypeInfo(llvm::StringRef RefName, StringRef Path, llvm::StringRef Name)
      : TypeInfo(RefName, Path), Name(Name) {}

  bool operator==(const FieldTypeInfo &Other) const {
    return std::tie(Type, Name) == std::tie(Other.Type, Other.Name);
  }

  SmallString<16> Name; // Name associated with this info.
};

// Info for member types.
struct MemberTypeInfo : public FieldTypeInfo {
  MemberTypeInfo() = default;
  MemberTypeInfo(SymbolID Type, StringRef Field, InfoType IT, StringRef Path,
                 llvm::StringRef Name, AccessSpecifier Access)
      : FieldTypeInfo(Type, Field, IT, Path, Name), Access(Access) {}
  MemberTypeInfo(llvm::StringRef RefName, llvm::StringRef Name,
                 AccessSpecifier Access)
      : FieldTypeInfo(RefName, Name), Access(Access) {}
  MemberTypeInfo(llvm::StringRef RefName, StringRef Path, llvm::StringRef Name,
                 AccessSpecifier Access)
      : FieldTypeInfo(RefName, Path, Name), Access(Access) {}

  bool operator==(const MemberTypeInfo &Other) const {
    return std::tie(Type, Name, Access) ==
           std::tie(Other.Type, Other.Name, Other.Access);
  }

  AccessSpecifier Access = AccessSpecifier::AS_none; // Access level associated
                                                     // with this info (public,
                                                     // protected, private,
                                                     // none).
};

struct Location {
  Location() = default;
  Location(int LineNumber, SmallString<16> Filename)
      : LineNumber(LineNumber), Filename(std::move(Filename)) {}

  bool operator==(const Location &Other) const {
    return std::tie(LineNumber, Filename) ==
           std::tie(Other.LineNumber, Other.Filename);
  }

  // This operator is used to sort a vector of Locations.
  // No specific order (attributes more important than others) is required. Any
  // sort is enough, the order is only needed to call std::unique after sorting
  // the vector.
  bool operator<(const Location &Other) const {
    return std::tie(LineNumber, Filename) <
           std::tie(Other.LineNumber, Other.Filename);
  }

  int LineNumber;           // Line number of this Location.
  SmallString<32> Filename; // File for this Location.
};

/// A base struct for Infos.
struct Info {
  Info() = default;
  Info(InfoType IT) : IT(IT) {}
  Info(InfoType IT, SymbolID USR) : USR(USR), IT(IT) {}
  Info(InfoType IT, SymbolID USR, StringRef Name)
      : USR(USR), IT(IT), Name(Name) {}
  Info(const Info &Other) = delete;
  Info(Info &&Other) = default;

  virtual ~Info() = default;

  SymbolID USR =
      SymbolID(); // Unique identifier for the decl described by this Info.
  const InfoType IT = InfoType::IT_default; // InfoType of this particular Info.
  SmallString<16> Name;                     // Unqualified name of the decl.
  llvm::SmallVector<Reference, 4>
      Namespace; // List of parent namespaces for this decl.
  std::vector<CommentInfo> Description; // Comment description of this decl.
  llvm::SmallString<128> Path;          // Path of directory where the clang-doc
                                        // generated file will be saved

  void mergeBase(Info &&I);
  bool mergeable(const Info &Other);

  llvm::SmallString<16> extractName();

  // Returns a reference to the parent scope (that is, the immediate parent
  // namespace or class in which this decl resides).
  llvm::Expected<Reference> getEnclosingScope();
};

// Info for namespaces.
struct NamespaceInfo : public Info {
  NamespaceInfo() : Info(InfoType::IT_namespace) {}
  NamespaceInfo(SymbolID USR) : Info(InfoType::IT_namespace, USR) {}
  NamespaceInfo(SymbolID USR, StringRef Name)
      : Info(InfoType::IT_namespace, USR, Name) {}

  void merge(NamespaceInfo &&I);

  // Namespaces and Records are references because they will be properly
  // documented in their own info, while the entirety of Functions and Enums are
  // included here because they should not have separate documentation from
  // their scope.
  std::vector<Reference> ChildNamespaces;
  std::vector<Reference> ChildRecords;
  std::vector<FunctionInfo> ChildFunctions;
  std::vector<EnumInfo> ChildEnums;
};

// Info for symbols.
struct SymbolInfo : public Info {
  SymbolInfo(InfoType IT) : Info(IT) {}
  SymbolInfo(InfoType IT, SymbolID USR) : Info(IT, USR) {}
  SymbolInfo(InfoType IT, SymbolID USR, StringRef Name) : Info(IT, USR, Name) {}

  void merge(SymbolInfo &&I);

  llvm::Optional<Location> DefLoc;    // Location where this decl is defined.
  llvm::SmallVector<Location, 2> Loc; // Locations where this decl is declared.
};

// TODO: Expand to allow for documenting templating and default args.
// Info for functions.
struct FunctionInfo : public SymbolInfo {
  FunctionInfo() : SymbolInfo(InfoType::IT_function) {}
  FunctionInfo(SymbolID USR) : SymbolInfo(InfoType::IT_function, USR) {}

  void merge(FunctionInfo &&I);

  bool IsMethod = false; // Indicates whether this function is a class method.
  Reference Parent;      // Reference to the parent class decl for this method.
  TypeInfo ReturnType;   // Info about the return type of this function.
  llvm::SmallVector<FieldTypeInfo, 4> Params; // List of parameters.
  // Access level for this method (public, private, protected, none).
  AccessSpecifier Access = AccessSpecifier::AS_none;
};

// TODO: Expand to allow for documenting templating, inheritance access,
// friend classes
// Info for types.
struct RecordInfo : public SymbolInfo {
  RecordInfo() : SymbolInfo(InfoType::IT_record) {}
  RecordInfo(SymbolID USR) : SymbolInfo(InfoType::IT_record, USR) {}
  RecordInfo(SymbolID USR, StringRef Name)
      : SymbolInfo(InfoType::IT_record, USR, Name) {}

  void merge(RecordInfo &&I);

  TagTypeKind TagType = TagTypeKind::TTK_Struct; // Type of this record
                                                 // (struct, class, union,
                                                 // interface).
  bool IsTypeDef = false; // Indicates if record was declared using typedef
  llvm::SmallVector<MemberTypeInfo, 4>
      Members;                             // List of info about record members.
  llvm::SmallVector<Reference, 4> Parents; // List of base/parent records
                                           // (does not include virtual
                                           // parents).
  llvm::SmallVector<Reference, 4>
      VirtualParents; // List of virtual base/parent records.

  // Records are references because they will be properly
  // documented in their own info, while the entirety of Functions and Enums are
  // included here because they should not have separate documentation from
  // their scope.
  std::vector<Reference> ChildRecords;
  std::vector<FunctionInfo> ChildFunctions;
  std::vector<EnumInfo> ChildEnums;
};

// TODO: Expand to allow for documenting templating.
// Info for types.
struct EnumInfo : public SymbolInfo {
  EnumInfo() : SymbolInfo(InfoType::IT_enum) {}
  EnumInfo(SymbolID USR) : SymbolInfo(InfoType::IT_enum, USR) {}

  void merge(EnumInfo &&I);

  bool Scoped =
      false; // Indicates whether this enum is scoped (e.g. enum class).
  llvm::SmallVector<SmallString<16>, 4> Members; // List of enum members.
};

// TODO: Add functionality to include separate markdown pages.

// A standalone function to call to merge a vector of infos into one.
// This assumes that all infos in the vector are of the same type, and will fail
// if they are different.
llvm::Expected<std::unique_ptr<Info>>
mergeInfos(std::vector<std::unique_ptr<Info>> &Values);

struct ClangDocContext {
  tooling::ExecutionContext *ECtx;
  bool PublicOnly;
};

} // namespace doc
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANG_DOC_REPRESENTATION_H
