//===-- DeclarationName.cpp - Declaration names implementation --*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implements the DeclarationName and DeclarationNameTable
// classes.
//
//===----------------------------------------------------------------------===//
#include "clang/AST/DeclarationName.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/DeclCXX.h"
#include "clang/AST/DeclTemplate.h"
#include "clang/AST/Type.h"
#include "clang/AST/TypeLoc.h"
#include "clang/AST/TypeOrdering.h"
#include "clang/Basic/IdentifierTable.h"
#include "llvm/ADT/FoldingSet.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/raw_ostream.h"
using namespace clang;

namespace clang {
/// CXXSpecialName - Records the type associated with one of the
/// "special" kinds of declaration names in C++, e.g., constructors,
/// destructors, and conversion functions.
class CXXSpecialName
  : public DeclarationNameExtra, public llvm::FoldingSetNode {
public:
  /// Type - The type associated with this declaration name.
  QualType Type;

  /// FETokenInfo - Extra information associated with this declaration
  /// name that can be used by the front end.
  void *FETokenInfo;

  void Profile(llvm::FoldingSetNodeID &ID) {
    ID.AddInteger(ExtraKindOrNumArgs);
    ID.AddPointer(Type.getAsOpaquePtr());
  }
};

/// Contains extra information for the name of a C++ deduction guide.
class CXXDeductionGuideNameExtra : public DeclarationNameExtra,
                                   public llvm::FoldingSetNode {
public:
  /// The template named by the deduction guide.
  TemplateDecl *Template;

  /// FETokenInfo - Extra information associated with this operator
  /// name that can be used by the front end.
  void *FETokenInfo;

  void Profile(llvm::FoldingSetNodeID &ID) {
    ID.AddPointer(Template);
  }
};

/// CXXOperatorIdName - Contains extra information for the name of an
/// overloaded operator in C++, such as "operator+.
class CXXOperatorIdName : public DeclarationNameExtra {
public:
  /// FETokenInfo - Extra information associated with this operator
  /// name that can be used by the front end.
  void *FETokenInfo;
};

/// CXXLiteralOperatorName - Contains the actual identifier that makes up the
/// name.
///
/// This identifier is stored here rather than directly in DeclarationName so as
/// to allow Objective-C selectors, which are about a million times more common,
/// to consume minimal memory.
class CXXLiteralOperatorIdName
  : public DeclarationNameExtra, public llvm::FoldingSetNode {
public:
  IdentifierInfo *ID;

  /// FETokenInfo - Extra information associated with this operator
  /// name that can be used by the front end.
  void *FETokenInfo;

  void Profile(llvm::FoldingSetNodeID &FSID) {
    FSID.AddPointer(ID);
  }
};

static int compareInt(unsigned A, unsigned B) {
  return (A < B ? -1 : (A > B ? 1 : 0));
}

int DeclarationName::compare(DeclarationName LHS, DeclarationName RHS) {
  if (LHS.getNameKind() != RHS.getNameKind())
    return (LHS.getNameKind() < RHS.getNameKind() ? -1 : 1);
  
  switch (LHS.getNameKind()) {
  case DeclarationName::Identifier: {
    IdentifierInfo *LII = LHS.getAsIdentifierInfo();
    IdentifierInfo *RII = RHS.getAsIdentifierInfo();
    if (!LII) return RII ? -1 : 0;
    if (!RII) return 1;
    
    return LII->getName().compare(RII->getName());
  }

  case DeclarationName::ObjCZeroArgSelector:
  case DeclarationName::ObjCOneArgSelector:
  case DeclarationName::ObjCMultiArgSelector: {
    Selector LHSSelector = LHS.getObjCSelector();
    Selector RHSSelector = RHS.getObjCSelector();
    // getNumArgs for ZeroArgSelector returns 0, but we still need to compare.
    if (LHS.getNameKind() == DeclarationName::ObjCZeroArgSelector &&
        RHS.getNameKind() == DeclarationName::ObjCZeroArgSelector) {
      return LHSSelector.getAsIdentifierInfo()->getName().compare(
             RHSSelector.getAsIdentifierInfo()->getName());
    }
    unsigned LN = LHSSelector.getNumArgs(), RN = RHSSelector.getNumArgs();
    for (unsigned I = 0, N = std::min(LN, RN); I != N; ++I) {
      switch (LHSSelector.getNameForSlot(I).compare(
                                               RHSSelector.getNameForSlot(I))) {
      case -1: return -1;
      case 1: return 1;
      default: break;
      }
    }

    return compareInt(LN, RN);
  }
  
  case DeclarationName::CXXConstructorName:
  case DeclarationName::CXXDestructorName:
  case DeclarationName::CXXConversionFunctionName:
    if (QualTypeOrdering()(LHS.getCXXNameType(), RHS.getCXXNameType()))
      return -1;
    if (QualTypeOrdering()(RHS.getCXXNameType(), LHS.getCXXNameType()))
      return 1;
    return 0;

  case DeclarationName::CXXDeductionGuideName:
    // We never want to compare deduction guide names for templates from
    // different scopes, so just compare the template-name.
    return compare(LHS.getCXXDeductionGuideTemplate()->getDeclName(),
                   RHS.getCXXDeductionGuideTemplate()->getDeclName());

  case DeclarationName::CXXOperatorName:
    return compareInt(LHS.getCXXOverloadedOperator(),
                      RHS.getCXXOverloadedOperator());

  case DeclarationName::CXXLiteralOperatorName:
    return LHS.getCXXLiteralIdentifier()->getName().compare(
                                   RHS.getCXXLiteralIdentifier()->getName());
              
  case DeclarationName::CXXUsingDirective:
    return 0;
  }

  llvm_unreachable("Invalid DeclarationName Kind!");
}

static void printCXXConstructorDestructorName(QualType ClassType,
                                              raw_ostream &OS,
                                              PrintingPolicy Policy) {
  // We know we're printing C++ here. Ensure we print types properly.
  Policy.adjustForCPlusPlus();

  if (const RecordType *ClassRec = ClassType->getAs<RecordType>()) {
    OS << *ClassRec->getDecl();
    return;
  }
  if (Policy.SuppressTemplateArgsInCXXConstructors) {
    if (auto *InjTy = ClassType->getAs<InjectedClassNameType>()) {
      OS << *InjTy->getDecl();
      return;
    }
  }
  ClassType.print(OS, Policy);
}

void DeclarationName::print(raw_ostream &OS, const PrintingPolicy &Policy) {
  DeclarationName &N = *this;
  switch (N.getNameKind()) {
  case DeclarationName::Identifier:
    if (const IdentifierInfo *II = N.getAsIdentifierInfo())
      OS << II->getName();
    return;

  case DeclarationName::ObjCZeroArgSelector:
  case DeclarationName::ObjCOneArgSelector:
  case DeclarationName::ObjCMultiArgSelector:
    N.getObjCSelector().print(OS);
    return;

  case DeclarationName::CXXConstructorName:
    return printCXXConstructorDestructorName(N.getCXXNameType(), OS, Policy);

  case DeclarationName::CXXDestructorName: {
    OS << '~';
    return printCXXConstructorDestructorName(N.getCXXNameType(), OS, Policy);
  }

  case DeclarationName::CXXDeductionGuideName:
    OS << "<deduction guide for ";
    getCXXDeductionGuideTemplate()->getDeclName().print(OS, Policy);
    OS << '>';
    return;

  case DeclarationName::CXXOperatorName: {
    static const char* const OperatorNames[NUM_OVERLOADED_OPERATORS] = {
      nullptr,
#define OVERLOADED_OPERATOR(Name,Spelling,Token,Unary,Binary,MemberOnly) \
      Spelling,
#include "clang/Basic/OperatorKinds.def"
    };
    const char *OpName = OperatorNames[N.getCXXOverloadedOperator()];
    assert(OpName && "not an overloaded operator");

    OS << "operator";
    if (OpName[0] >= 'a' && OpName[0] <= 'z')
      OS << ' ';
    OS << OpName;
    return;
  }

  case DeclarationName::CXXLiteralOperatorName:
    OS << "operator\"\"" << N.getCXXLiteralIdentifier()->getName();
    return;

  case DeclarationName::CXXConversionFunctionName: {
    OS << "operator ";
    QualType Type = N.getCXXNameType();
    if (const RecordType *Rec = Type->getAs<RecordType>()) {
      OS << *Rec->getDecl();
      return;
    }
    // We know we're printing C++ here, ensure we print 'bool' properly.
    PrintingPolicy CXXPolicy = Policy;
    CXXPolicy.adjustForCPlusPlus();
    Type.print(OS, CXXPolicy);
    return;
  }
  case DeclarationName::CXXUsingDirective:
    OS << "<using-directive>";
    return;
  }

  llvm_unreachable("Unexpected declaration name kind");
}

raw_ostream &operator<<(raw_ostream &OS, DeclarationName N) {
  LangOptions LO;
  N.print(OS, PrintingPolicy(LO));
  return OS;
}

} // end namespace clang

DeclarationName::NameKind DeclarationName::getNameKind() const {
  switch (getStoredNameKind()) {
  case StoredIdentifier:          return Identifier;
  case StoredObjCZeroArgSelector: return ObjCZeroArgSelector;
  case StoredObjCOneArgSelector:  return ObjCOneArgSelector;

  case StoredDeclarationNameExtra:
    switch (getExtra()->ExtraKindOrNumArgs) {
    case DeclarationNameExtra::CXXConstructor:
      return CXXConstructorName;

    case DeclarationNameExtra::CXXDestructor:
      return CXXDestructorName;

    case DeclarationNameExtra::CXXDeductionGuide:
      return CXXDeductionGuideName;

    case DeclarationNameExtra::CXXConversionFunction:
      return CXXConversionFunctionName;

    case DeclarationNameExtra::CXXLiteralOperator:
      return CXXLiteralOperatorName;

    case DeclarationNameExtra::CXXUsingDirective:
      return CXXUsingDirective;

    default:
      // Check if we have one of the CXXOperator* enumeration values.
      if (getExtra()->ExtraKindOrNumArgs <
            DeclarationNameExtra::CXXUsingDirective)
        return CXXOperatorName;

      return ObjCMultiArgSelector;
    }
  }

  // Can't actually get here.
  llvm_unreachable("This should be unreachable!");
}

bool DeclarationName::isDependentName() const {
  QualType T = getCXXNameType();
  if (!T.isNull() && T->isDependentType())
    return true;

  // A class-scope deduction guide in a dependent context has a dependent name.
  auto *TD = getCXXDeductionGuideTemplate();
  if (TD && TD->getDeclContext()->isDependentContext())
    return true;

  return false;
}

std::string DeclarationName::getAsString() const {
  std::string Result;
  llvm::raw_string_ostream OS(Result);
  OS << *this;
  return OS.str();
}

QualType DeclarationName::getCXXNameType() const {
  if (CXXSpecialName *CXXName = getAsCXXSpecialName())
    return CXXName->Type;
  else
    return QualType();
}

TemplateDecl *DeclarationName::getCXXDeductionGuideTemplate() const {
  if (auto *Guide = getAsCXXDeductionGuideNameExtra())
    return Guide->Template;
  return nullptr;
}

OverloadedOperatorKind DeclarationName::getCXXOverloadedOperator() const {
  if (CXXOperatorIdName *CXXOp = getAsCXXOperatorIdName()) {
    unsigned value
      = CXXOp->ExtraKindOrNumArgs - DeclarationNameExtra::CXXConversionFunction;
    return static_cast<OverloadedOperatorKind>(value);
  } else {
    return OO_None;
  }
}

IdentifierInfo *DeclarationName::getCXXLiteralIdentifier() const {
  if (CXXLiteralOperatorIdName *CXXLit = getAsCXXLiteralOperatorIdName())
    return CXXLit->ID;
  else
    return nullptr;
}

void *DeclarationName::getFETokenInfoAsVoidSlow() const {
  switch (getNameKind()) {
  case Identifier:
    llvm_unreachable("Handled by getFETokenInfo()");

  case CXXConstructorName:
  case CXXDestructorName:
  case CXXConversionFunctionName:
    return getAsCXXSpecialName()->FETokenInfo;

  case CXXDeductionGuideName:
    return getAsCXXDeductionGuideNameExtra()->FETokenInfo;

  case CXXOperatorName:
    return getAsCXXOperatorIdName()->FETokenInfo;

  case CXXLiteralOperatorName:
    return getAsCXXLiteralOperatorIdName()->FETokenInfo;

  default:
    llvm_unreachable("Declaration name has no FETokenInfo");
  }
}

void DeclarationName::setFETokenInfo(void *T) {
  switch (getNameKind()) {
  case Identifier:
    getAsIdentifierInfo()->setFETokenInfo(T);
    break;

  case CXXConstructorName:
  case CXXDestructorName:
  case CXXConversionFunctionName:
    getAsCXXSpecialName()->FETokenInfo = T;
    break;

  case CXXDeductionGuideName:
    getAsCXXDeductionGuideNameExtra()->FETokenInfo = T;
    break;

  case CXXOperatorName:
    getAsCXXOperatorIdName()->FETokenInfo = T;
    break;

  case CXXLiteralOperatorName:
    getAsCXXLiteralOperatorIdName()->FETokenInfo = T;
    break;

  default:
    llvm_unreachable("Declaration name has no FETokenInfo");
  }
}

DeclarationName DeclarationName::getUsingDirectiveName() {
  // Single instance of DeclarationNameExtra for using-directive
  static const DeclarationNameExtra UDirExtra =
    { DeclarationNameExtra::CXXUsingDirective };

  uintptr_t Ptr = reinterpret_cast<uintptr_t>(&UDirExtra);
  Ptr |= StoredDeclarationNameExtra;

  return DeclarationName(Ptr);
}

LLVM_DUMP_METHOD void DeclarationName::dump() const {
  llvm::errs() << *this << '\n';
}

DeclarationNameTable::DeclarationNameTable(const ASTContext &C) : Ctx(C) {
  CXXSpecialNamesImpl = new llvm::FoldingSet<CXXSpecialName>;
  CXXLiteralOperatorNames = new llvm::FoldingSet<CXXLiteralOperatorIdName>;
  CXXDeductionGuideNames = new llvm::FoldingSet<CXXDeductionGuideNameExtra>;

  // Initialize the overloaded operator names.
  CXXOperatorNames = new (Ctx) CXXOperatorIdName[NUM_OVERLOADED_OPERATORS];
  for (unsigned Op = 0; Op < NUM_OVERLOADED_OPERATORS; ++Op) {
    CXXOperatorNames[Op].ExtraKindOrNumArgs
      = Op + DeclarationNameExtra::CXXConversionFunction;
    CXXOperatorNames[Op].FETokenInfo = nullptr;
  }
}

DeclarationNameTable::~DeclarationNameTable() {
  auto *SpecialNames =
      static_cast<llvm::FoldingSet<CXXSpecialName> *>(CXXSpecialNamesImpl);
  auto *LiteralNames =
      static_cast<llvm::FoldingSet<CXXLiteralOperatorIdName> *>(
          CXXLiteralOperatorNames);
  auto *DeductionGuideNames =
      static_cast<llvm::FoldingSet<CXXDeductionGuideNameExtra> *>(
          CXXDeductionGuideNames);

  delete SpecialNames;
  delete LiteralNames;
  delete DeductionGuideNames;
}

DeclarationName DeclarationNameTable::getCXXConstructorName(CanQualType Ty) {
  return getCXXSpecialName(DeclarationName::CXXConstructorName,
                           Ty.getUnqualifiedType());
}

DeclarationName DeclarationNameTable::getCXXDestructorName(CanQualType Ty) {
  return getCXXSpecialName(DeclarationName::CXXDestructorName,
                           Ty.getUnqualifiedType());
}

DeclarationName
DeclarationNameTable::getCXXDeductionGuideName(TemplateDecl *Template) {
  Template = cast<TemplateDecl>(Template->getCanonicalDecl());

  auto *DeductionGuideNames =
      static_cast<llvm::FoldingSet<CXXDeductionGuideNameExtra> *>(
          CXXDeductionGuideNames);

  llvm::FoldingSetNodeID ID;
  ID.AddPointer(Template);

  void *InsertPos = nullptr;
  if (auto *Name = DeductionGuideNames->FindNodeOrInsertPos(ID, InsertPos))
    return DeclarationName(Name);

  auto *Name = new (Ctx) CXXDeductionGuideNameExtra;
  Name->ExtraKindOrNumArgs = DeclarationNameExtra::CXXDeductionGuide;
  Name->Template = Template;
  Name->FETokenInfo = nullptr;

  DeductionGuideNames->InsertNode(Name, InsertPos);
  return DeclarationName(Name);
}

DeclarationName
DeclarationNameTable::getCXXConversionFunctionName(CanQualType Ty) {
  return getCXXSpecialName(DeclarationName::CXXConversionFunctionName, Ty);
}

DeclarationName
DeclarationNameTable::getCXXSpecialName(DeclarationName::NameKind Kind,
                                        CanQualType Ty) {
  assert(Kind >= DeclarationName::CXXConstructorName &&
         Kind <= DeclarationName::CXXConversionFunctionName &&
         "Kind must be a C++ special name kind");
  llvm::FoldingSet<CXXSpecialName> *SpecialNames
    = static_cast<llvm::FoldingSet<CXXSpecialName>*>(CXXSpecialNamesImpl);

  DeclarationNameExtra::ExtraKind EKind;
  switch (Kind) {
  case DeclarationName::CXXConstructorName:
    EKind = DeclarationNameExtra::CXXConstructor;
    assert(!Ty.hasQualifiers() &&"Constructor type must be unqualified");
    break;
  case DeclarationName::CXXDestructorName:
    EKind = DeclarationNameExtra::CXXDestructor;
    assert(!Ty.hasQualifiers() && "Destructor type must be unqualified");
    break;
  case DeclarationName::CXXConversionFunctionName:
    EKind = DeclarationNameExtra::CXXConversionFunction;
    break;
  default:
    return DeclarationName();
  }

  // Unique selector, to guarantee there is one per name.
  llvm::FoldingSetNodeID ID;
  ID.AddInteger(EKind);
  ID.AddPointer(Ty.getAsOpaquePtr());

  void *InsertPos = nullptr;
  if (CXXSpecialName *Name = SpecialNames->FindNodeOrInsertPos(ID, InsertPos))
    return DeclarationName(Name);

  CXXSpecialName *SpecialName = new (Ctx) CXXSpecialName;
  SpecialName->ExtraKindOrNumArgs = EKind;
  SpecialName->Type = Ty;
  SpecialName->FETokenInfo = nullptr;

  SpecialNames->InsertNode(SpecialName, InsertPos);
  return DeclarationName(SpecialName);
}

DeclarationName
DeclarationNameTable::getCXXOperatorName(OverloadedOperatorKind Op) {
  return DeclarationName(&CXXOperatorNames[(unsigned)Op]);
}

DeclarationName
DeclarationNameTable::getCXXLiteralOperatorName(IdentifierInfo *II) {
  llvm::FoldingSet<CXXLiteralOperatorIdName> *LiteralNames
    = static_cast<llvm::FoldingSet<CXXLiteralOperatorIdName>*>
                                                      (CXXLiteralOperatorNames);

  llvm::FoldingSetNodeID ID;
  ID.AddPointer(II);

  void *InsertPos = nullptr;
  if (CXXLiteralOperatorIdName *Name =
                               LiteralNames->FindNodeOrInsertPos(ID, InsertPos))
    return DeclarationName (Name);
  
  CXXLiteralOperatorIdName *LiteralName = new (Ctx) CXXLiteralOperatorIdName;
  LiteralName->ExtraKindOrNumArgs = DeclarationNameExtra::CXXLiteralOperator;
  LiteralName->ID = II;
  LiteralName->FETokenInfo = nullptr;

  LiteralNames->InsertNode(LiteralName, InsertPos);
  return DeclarationName(LiteralName);
}

DeclarationNameLoc::DeclarationNameLoc(DeclarationName Name) {
  switch (Name.getNameKind()) {
  case DeclarationName::Identifier:
  case DeclarationName::CXXDeductionGuideName:
    break;
  case DeclarationName::CXXConstructorName:
  case DeclarationName::CXXDestructorName:
  case DeclarationName::CXXConversionFunctionName:
    NamedType.TInfo = nullptr;
    break;
  case DeclarationName::CXXOperatorName:
    CXXOperatorName.BeginOpNameLoc = SourceLocation().getRawEncoding();
    CXXOperatorName.EndOpNameLoc = SourceLocation().getRawEncoding();
    break;
  case DeclarationName::CXXLiteralOperatorName:
    CXXLiteralOperatorName.OpNameLoc = SourceLocation().getRawEncoding();
    break;
  case DeclarationName::ObjCZeroArgSelector:
  case DeclarationName::ObjCOneArgSelector:
  case DeclarationName::ObjCMultiArgSelector:
    // FIXME: ?
    break;
  case DeclarationName::CXXUsingDirective:
    break;
  }
}

bool DeclarationNameInfo::containsUnexpandedParameterPack() const {
  switch (Name.getNameKind()) {
  case DeclarationName::Identifier:
  case DeclarationName::ObjCZeroArgSelector:
  case DeclarationName::ObjCOneArgSelector:
  case DeclarationName::ObjCMultiArgSelector:
  case DeclarationName::CXXOperatorName:
  case DeclarationName::CXXLiteralOperatorName:
  case DeclarationName::CXXUsingDirective:
  case DeclarationName::CXXDeductionGuideName:
    return false;

  case DeclarationName::CXXConstructorName:
  case DeclarationName::CXXDestructorName:
  case DeclarationName::CXXConversionFunctionName:
    if (TypeSourceInfo *TInfo = LocInfo.NamedType.TInfo)
      return TInfo->getType()->containsUnexpandedParameterPack();

    return Name.getCXXNameType()->containsUnexpandedParameterPack();
  }
  llvm_unreachable("All name kinds handled.");
}

bool DeclarationNameInfo::isInstantiationDependent() const {
  switch (Name.getNameKind()) {
  case DeclarationName::Identifier:
  case DeclarationName::ObjCZeroArgSelector:
  case DeclarationName::ObjCOneArgSelector:
  case DeclarationName::ObjCMultiArgSelector:
  case DeclarationName::CXXOperatorName:
  case DeclarationName::CXXLiteralOperatorName:
  case DeclarationName::CXXUsingDirective:
  case DeclarationName::CXXDeductionGuideName:
    return false;
    
  case DeclarationName::CXXConstructorName:
  case DeclarationName::CXXDestructorName:
  case DeclarationName::CXXConversionFunctionName:
    if (TypeSourceInfo *TInfo = LocInfo.NamedType.TInfo)
      return TInfo->getType()->isInstantiationDependentType();
    
    return Name.getCXXNameType()->isInstantiationDependentType();
  }
  llvm_unreachable("All name kinds handled.");
}

std::string DeclarationNameInfo::getAsString() const {
  std::string Result;
  llvm::raw_string_ostream OS(Result);
  printName(OS);
  return OS.str();
}

void DeclarationNameInfo::printName(raw_ostream &OS) const {
  switch (Name.getNameKind()) {
  case DeclarationName::Identifier:
  case DeclarationName::ObjCZeroArgSelector:
  case DeclarationName::ObjCOneArgSelector:
  case DeclarationName::ObjCMultiArgSelector:
  case DeclarationName::CXXOperatorName:
  case DeclarationName::CXXLiteralOperatorName:
  case DeclarationName::CXXUsingDirective:
  case DeclarationName::CXXDeductionGuideName:
    OS << Name;
    return;

  case DeclarationName::CXXConstructorName:
  case DeclarationName::CXXDestructorName:
  case DeclarationName::CXXConversionFunctionName:
    if (TypeSourceInfo *TInfo = LocInfo.NamedType.TInfo) {
      if (Name.getNameKind() == DeclarationName::CXXDestructorName)
        OS << '~';
      else if (Name.getNameKind() == DeclarationName::CXXConversionFunctionName)
        OS << "operator ";
      LangOptions LO;
      LO.CPlusPlus = true;
      LO.Bool = true;
      PrintingPolicy PP(LO);
      PP.SuppressScope = true;
      OS << TInfo->getType().getAsString(PP);
    } else
      OS << Name;
    return;
  }
  llvm_unreachable("Unexpected declaration name kind");
}

SourceLocation DeclarationNameInfo::getEndLoc() const {
  switch (Name.getNameKind()) {
  case DeclarationName::Identifier:
  case DeclarationName::CXXDeductionGuideName:
    return NameLoc;

  case DeclarationName::CXXOperatorName: {
    unsigned raw = LocInfo.CXXOperatorName.EndOpNameLoc;
    return SourceLocation::getFromRawEncoding(raw);
  }

  case DeclarationName::CXXLiteralOperatorName: {
    unsigned raw = LocInfo.CXXLiteralOperatorName.OpNameLoc;
    return SourceLocation::getFromRawEncoding(raw);
  }

  case DeclarationName::CXXConstructorName:
  case DeclarationName::CXXDestructorName:
  case DeclarationName::CXXConversionFunctionName:
    if (TypeSourceInfo *TInfo = LocInfo.NamedType.TInfo)
      return TInfo->getTypeLoc().getEndLoc();
    else
      return NameLoc;

    // DNInfo work in progress: FIXME.
  case DeclarationName::ObjCZeroArgSelector:
  case DeclarationName::ObjCOneArgSelector:
  case DeclarationName::ObjCMultiArgSelector:
  case DeclarationName::CXXUsingDirective:
    return NameLoc;
  }
  llvm_unreachable("Unexpected declaration name kind");
}
