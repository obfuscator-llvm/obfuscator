//===--- CGStmtOpenMP.cpp - Emit LLVM Code from Statements ----------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This contains code to emit OpenMP nodes as LLVM code.
//
//===----------------------------------------------------------------------===//

#include "CGCleanup.h"
#include "CGOpenMPRuntime.h"
#include "CodeGenFunction.h"
#include "CodeGenModule.h"
#include "TargetInfo.h"
#include "clang/AST/Stmt.h"
#include "clang/AST/StmtOpenMP.h"
#include "clang/AST/DeclOpenMP.h"
#include "llvm/IR/CallSite.h"
using namespace clang;
using namespace CodeGen;

namespace {
/// Lexical scope for OpenMP executable constructs, that handles correct codegen
/// for captured expressions.
class OMPLexicalScope : public CodeGenFunction::LexicalScope {
  void emitPreInitStmt(CodeGenFunction &CGF, const OMPExecutableDirective &S) {
    for (const auto *C : S.clauses()) {
      if (auto *CPI = OMPClauseWithPreInit::get(C)) {
        if (auto *PreInit = cast_or_null<DeclStmt>(CPI->getPreInitStmt())) {
          for (const auto *I : PreInit->decls()) {
            if (!I->hasAttr<OMPCaptureNoInitAttr>())
              CGF.EmitVarDecl(cast<VarDecl>(*I));
            else {
              CodeGenFunction::AutoVarEmission Emission =
                  CGF.EmitAutoVarAlloca(cast<VarDecl>(*I));
              CGF.EmitAutoVarCleanups(Emission);
            }
          }
        }
      }
    }
  }
  CodeGenFunction::OMPPrivateScope InlinedShareds;

  static bool isCapturedVar(CodeGenFunction &CGF, const VarDecl *VD) {
    return CGF.LambdaCaptureFields.lookup(VD) ||
           (CGF.CapturedStmtInfo && CGF.CapturedStmtInfo->lookup(VD)) ||
           (CGF.CurCodeDecl && isa<BlockDecl>(CGF.CurCodeDecl));
  }

public:
  OMPLexicalScope(CodeGenFunction &CGF, const OMPExecutableDirective &S,
                  bool AsInlined = false, bool EmitPreInitStmt = true)
      : CodeGenFunction::LexicalScope(CGF, S.getSourceRange()),
        InlinedShareds(CGF) {
    if (EmitPreInitStmt)
      emitPreInitStmt(CGF, S);
    if (AsInlined) {
      if (S.hasAssociatedStmt()) {
        auto *CS = cast<CapturedStmt>(S.getAssociatedStmt());
        for (auto &C : CS->captures()) {
          if (C.capturesVariable() || C.capturesVariableByCopy()) {
            auto *VD = C.getCapturedVar();
            DeclRefExpr DRE(const_cast<VarDecl *>(VD),
                            isCapturedVar(CGF, VD) ||
                                (CGF.CapturedStmtInfo &&
                                 InlinedShareds.isGlobalVarCaptured(VD)),
                            VD->getType().getNonReferenceType(), VK_LValue,
                            SourceLocation());
            InlinedShareds.addPrivate(VD, [&CGF, &DRE]() -> Address {
              return CGF.EmitLValue(&DRE).getAddress();
            });
          }
        }
        (void)InlinedShareds.Privatize();
      }
    }
  }
};

/// Lexical scope for OpenMP parallel construct, that handles correct codegen
/// for captured expressions.
class OMPParallelScope final : public OMPLexicalScope {
  bool EmitPreInitStmt(const OMPExecutableDirective &S) {
    OpenMPDirectiveKind Kind = S.getDirectiveKind();
    return !(isOpenMPTargetExecutionDirective(Kind) ||
             isOpenMPLoopBoundSharingDirective(Kind)) &&
           isOpenMPParallelDirective(Kind);
  }

public:
  OMPParallelScope(CodeGenFunction &CGF, const OMPExecutableDirective &S)
      : OMPLexicalScope(CGF, S,
                        /*AsInlined=*/false,
                        /*EmitPreInitStmt=*/EmitPreInitStmt(S)) {}
};

/// Lexical scope for OpenMP teams construct, that handles correct codegen
/// for captured expressions.
class OMPTeamsScope final : public OMPLexicalScope {
  bool EmitPreInitStmt(const OMPExecutableDirective &S) {
    OpenMPDirectiveKind Kind = S.getDirectiveKind();
    return !isOpenMPTargetExecutionDirective(Kind) &&
           isOpenMPTeamsDirective(Kind);
  }

public:
  OMPTeamsScope(CodeGenFunction &CGF, const OMPExecutableDirective &S)
      : OMPLexicalScope(CGF, S,
                        /*AsInlined=*/false,
                        /*EmitPreInitStmt=*/EmitPreInitStmt(S)) {}
};

/// Private scope for OpenMP loop-based directives, that supports capturing
/// of used expression from loop statement.
class OMPLoopScope : public CodeGenFunction::RunCleanupsScope {
  void emitPreInitStmt(CodeGenFunction &CGF, const OMPLoopDirective &S) {
    if (auto *LD = dyn_cast<OMPLoopDirective>(&S)) {
      if (auto *PreInits = cast_or_null<DeclStmt>(LD->getPreInits())) {
        for (const auto *I : PreInits->decls())
          CGF.EmitVarDecl(cast<VarDecl>(*I));
      }
    }
  }

public:
  OMPLoopScope(CodeGenFunction &CGF, const OMPLoopDirective &S)
      : CodeGenFunction::RunCleanupsScope(CGF) {
    emitPreInitStmt(CGF, S);
  }
};

} // namespace

llvm::Value *CodeGenFunction::getTypeSize(QualType Ty) {
  auto &C = getContext();
  llvm::Value *Size = nullptr;
  auto SizeInChars = C.getTypeSizeInChars(Ty);
  if (SizeInChars.isZero()) {
    // getTypeSizeInChars() returns 0 for a VLA.
    while (auto *VAT = C.getAsVariableArrayType(Ty)) {
      llvm::Value *ArraySize;
      std::tie(ArraySize, Ty) = getVLASize(VAT);
      Size = Size ? Builder.CreateNUWMul(Size, ArraySize) : ArraySize;
    }
    SizeInChars = C.getTypeSizeInChars(Ty);
    if (SizeInChars.isZero())
      return llvm::ConstantInt::get(SizeTy, /*V=*/0);
    Size = Builder.CreateNUWMul(Size, CGM.getSize(SizeInChars));
  } else
    Size = CGM.getSize(SizeInChars);
  return Size;
}

void CodeGenFunction::GenerateOpenMPCapturedVars(
    const CapturedStmt &S, SmallVectorImpl<llvm::Value *> &CapturedVars) {
  const RecordDecl *RD = S.getCapturedRecordDecl();
  auto CurField = RD->field_begin();
  auto CurCap = S.captures().begin();
  for (CapturedStmt::const_capture_init_iterator I = S.capture_init_begin(),
                                                 E = S.capture_init_end();
       I != E; ++I, ++CurField, ++CurCap) {
    if (CurField->hasCapturedVLAType()) {
      auto VAT = CurField->getCapturedVLAType();
      auto *Val = VLASizeMap[VAT->getSizeExpr()];
      CapturedVars.push_back(Val);
    } else if (CurCap->capturesThis())
      CapturedVars.push_back(CXXThisValue);
    else if (CurCap->capturesVariableByCopy()) {
      llvm::Value *CV =
          EmitLoadOfLValue(EmitLValue(*I), SourceLocation()).getScalarVal();

      // If the field is not a pointer, we need to save the actual value
      // and load it as a void pointer.
      if (!CurField->getType()->isAnyPointerType()) {
        auto &Ctx = getContext();
        auto DstAddr = CreateMemTemp(
            Ctx.getUIntPtrType(),
            Twine(CurCap->getCapturedVar()->getName()) + ".casted");
        LValue DstLV = MakeAddrLValue(DstAddr, Ctx.getUIntPtrType());

        auto *SrcAddrVal = EmitScalarConversion(
            DstAddr.getPointer(), Ctx.getPointerType(Ctx.getUIntPtrType()),
            Ctx.getPointerType(CurField->getType()), SourceLocation());
        LValue SrcLV =
            MakeNaturalAlignAddrLValue(SrcAddrVal, CurField->getType());

        // Store the value using the source type pointer.
        EmitStoreThroughLValue(RValue::get(CV), SrcLV);

        // Load the value using the destination type pointer.
        CV = EmitLoadOfLValue(DstLV, SourceLocation()).getScalarVal();
      }
      CapturedVars.push_back(CV);
    } else {
      assert(CurCap->capturesVariable() && "Expected capture by reference.");
      CapturedVars.push_back(EmitLValue(*I).getAddress().getPointer());
    }
  }
}

static Address castValueFromUintptr(CodeGenFunction &CGF, QualType DstType,
                                    StringRef Name, LValue AddrLV,
                                    bool isReferenceType = false) {
  ASTContext &Ctx = CGF.getContext();

  auto *CastedPtr = CGF.EmitScalarConversion(
      AddrLV.getAddress().getPointer(), Ctx.getUIntPtrType(),
      Ctx.getPointerType(DstType), SourceLocation());
  auto TmpAddr =
      CGF.MakeNaturalAlignAddrLValue(CastedPtr, Ctx.getPointerType(DstType))
          .getAddress();

  // If we are dealing with references we need to return the address of the
  // reference instead of the reference of the value.
  if (isReferenceType) {
    QualType RefType = Ctx.getLValueReferenceType(DstType);
    auto *RefVal = TmpAddr.getPointer();
    TmpAddr = CGF.CreateMemTemp(RefType, Twine(Name) + ".ref");
    auto TmpLVal = CGF.MakeAddrLValue(TmpAddr, RefType);
    CGF.EmitStoreThroughLValue(RValue::get(RefVal), TmpLVal, /*isInit*/ true);
  }

  return TmpAddr;
}

static QualType getCanonicalParamType(ASTContext &C, QualType T) {
  if (T->isLValueReferenceType()) {
    return C.getLValueReferenceType(
        getCanonicalParamType(C, T.getNonReferenceType()),
        /*SpelledAsLValue=*/false);
  }
  if (T->isPointerType())
    return C.getPointerType(getCanonicalParamType(C, T->getPointeeType()));
  return C.getCanonicalParamType(T);
}

namespace {
  /// Contains required data for proper outlined function codegen.
  struct FunctionOptions {
    /// Captured statement for which the function is generated.
    const CapturedStmt *S = nullptr;
    /// true if cast to/from  UIntPtr is required for variables captured by
    /// value.
    bool UIntPtrCastRequired = true;
    /// true if only casted argumefnts must be registered as local args or VLA
    /// sizes.
    bool RegisterCastedArgsOnly = false;
    /// Name of the generated function.
    StringRef FunctionName;
    explicit FunctionOptions(const CapturedStmt *S, bool UIntPtrCastRequired,
                             bool RegisterCastedArgsOnly,
                             StringRef FunctionName)
        : S(S), UIntPtrCastRequired(UIntPtrCastRequired),
          RegisterCastedArgsOnly(UIntPtrCastRequired && RegisterCastedArgsOnly),
          FunctionName(FunctionName) {}
  };
}

static std::pair<llvm::Function *, bool> emitOutlinedFunctionPrologue(
    CodeGenFunction &CGF, FunctionArgList &Args,
    llvm::DenseMap<const Decl *, std::pair<const VarDecl *, Address>>
        &LocalAddrs,
    llvm::DenseMap<const Decl *, std::pair<const Expr *, llvm::Value *>>
        &VLASizes,
    llvm::Value *&CXXThisValue, const FunctionOptions &FO) {
  const CapturedDecl *CD = FO.S->getCapturedDecl();
  const RecordDecl *RD = FO.S->getCapturedRecordDecl();
  assert(CD->hasBody() && "missing CapturedDecl body");

  CXXThisValue = nullptr;
  // Build the argument list.
  CodeGenModule &CGM = CGF.CGM;
  ASTContext &Ctx = CGM.getContext();
  bool HasUIntPtrArgs = false;
  Args.append(CD->param_begin(),
              std::next(CD->param_begin(), CD->getContextParamPosition()));
  auto I = FO.S->captures().begin();
  for (auto *FD : RD->fields()) {
    QualType ArgType = FD->getType();
    IdentifierInfo *II = nullptr;
    VarDecl *CapVar = nullptr;

    // If this is a capture by copy and the type is not a pointer, the outlined
    // function argument type should be uintptr and the value properly casted to
    // uintptr. This is necessary given that the runtime library is only able to
    // deal with pointers. We can pass in the same way the VLA type sizes to the
    // outlined function.
    if ((I->capturesVariableByCopy() && !ArgType->isAnyPointerType()) ||
        I->capturesVariableArrayType()) {
      HasUIntPtrArgs = true;
      if (FO.UIntPtrCastRequired)
        ArgType = Ctx.getUIntPtrType();
    }

    if (I->capturesVariable() || I->capturesVariableByCopy()) {
      CapVar = I->getCapturedVar();
      II = CapVar->getIdentifier();
    } else if (I->capturesThis())
      II = &Ctx.Idents.get("this");
    else {
      assert(I->capturesVariableArrayType());
      II = &Ctx.Idents.get("vla");
    }
    if (ArgType->isVariablyModifiedType())
      ArgType = getCanonicalParamType(Ctx, ArgType.getNonReferenceType());
    Args.push_back(ImplicitParamDecl::Create(Ctx, /*DC=*/nullptr,
                                             FD->getLocation(), II, ArgType,
                                             ImplicitParamDecl::Other));
    ++I;
  }
  Args.append(
      std::next(CD->param_begin(), CD->getContextParamPosition() + 1),
      CD->param_end());

  // Create the function declaration.
  FunctionType::ExtInfo ExtInfo;
  const CGFunctionInfo &FuncInfo =
      CGM.getTypes().arrangeBuiltinFunctionDeclaration(Ctx.VoidTy, Args);
  llvm::FunctionType *FuncLLVMTy = CGM.getTypes().GetFunctionType(FuncInfo);

  llvm::Function *F =
      llvm::Function::Create(FuncLLVMTy, llvm::GlobalValue::InternalLinkage,
                             FO.FunctionName, &CGM.getModule());
  CGM.SetInternalFunctionAttributes(CD, F, FuncInfo);
  if (CD->isNothrow())
    F->addFnAttr(llvm::Attribute::NoUnwind);

  // Generate the function.
  CGF.StartFunction(CD, Ctx.VoidTy, F, FuncInfo, Args, CD->getLocation(),
                    CD->getBody()->getLocStart());
  unsigned Cnt = CD->getContextParamPosition();
  I = FO.S->captures().begin();
  for (auto *FD : RD->fields()) {
    // If we are capturing a pointer by copy we don't need to do anything, just
    // use the value that we get from the arguments.
    if (I->capturesVariableByCopy() && FD->getType()->isAnyPointerType()) {
      const VarDecl *CurVD = I->getCapturedVar();
      Address LocalAddr = CGF.GetAddrOfLocalVar(Args[Cnt]);
      // If the variable is a reference we need to materialize it here.
      if (CurVD->getType()->isReferenceType()) {
        Address RefAddr = CGF.CreateMemTemp(
            CurVD->getType(), CGM.getPointerAlign(), ".materialized_ref");
        CGF.EmitStoreOfScalar(LocalAddr.getPointer(), RefAddr,
                              /*Volatile=*/false, CurVD->getType());
        LocalAddr = RefAddr;
      }
      if (!FO.RegisterCastedArgsOnly)
        LocalAddrs.insert({Args[Cnt], {CurVD, LocalAddr}});
      ++Cnt;
      ++I;
      continue;
    }

    LValueBaseInfo BaseInfo(AlignmentSource::Decl, false);
    LValue ArgLVal = CGF.MakeAddrLValue(CGF.GetAddrOfLocalVar(Args[Cnt]),
                                        Args[Cnt]->getType(), BaseInfo);
    if (FD->hasCapturedVLAType()) {
      if (FO.UIntPtrCastRequired) {
        ArgLVal = CGF.MakeAddrLValue(castValueFromUintptr(CGF, FD->getType(),
                                                          Args[Cnt]->getName(),
                                                          ArgLVal),
                                     FD->getType(), BaseInfo);
      }
      auto *ExprArg =
          CGF.EmitLoadOfLValue(ArgLVal, SourceLocation()).getScalarVal();
      auto VAT = FD->getCapturedVLAType();
      VLASizes.insert({Args[Cnt], {VAT->getSizeExpr(), ExprArg}});
    } else if (I->capturesVariable()) {
      auto *Var = I->getCapturedVar();
      QualType VarTy = Var->getType();
      Address ArgAddr = ArgLVal.getAddress();
      if (!VarTy->isReferenceType()) {
        if (ArgLVal.getType()->isLValueReferenceType()) {
          ArgAddr = CGF.EmitLoadOfReference(
              ArgAddr, ArgLVal.getType()->castAs<ReferenceType>());
        } else if (!VarTy->isVariablyModifiedType() || !VarTy->isPointerType()) {
          assert(ArgLVal.getType()->isPointerType());
          ArgAddr = CGF.EmitLoadOfPointer(
              ArgAddr, ArgLVal.getType()->castAs<PointerType>());
        }
      }
      if (!FO.RegisterCastedArgsOnly) {
        LocalAddrs.insert(
            {Args[Cnt],
             {Var, Address(ArgAddr.getPointer(), Ctx.getDeclAlign(Var))}});
      }
    } else if (I->capturesVariableByCopy()) {
      assert(!FD->getType()->isAnyPointerType() &&
             "Not expecting a captured pointer.");
      auto *Var = I->getCapturedVar();
      QualType VarTy = Var->getType();
      LocalAddrs.insert(
          {Args[Cnt],
           {Var,
            FO.UIntPtrCastRequired
                ? castValueFromUintptr(CGF, FD->getType(), Args[Cnt]->getName(),
                                       ArgLVal, VarTy->isReferenceType())
                : ArgLVal.getAddress()}});
    } else {
      // If 'this' is captured, load it into CXXThisValue.
      assert(I->capturesThis());
      CXXThisValue = CGF.EmitLoadOfLValue(ArgLVal, Args[Cnt]->getLocation())
                         .getScalarVal();
      LocalAddrs.insert({Args[Cnt], {nullptr, ArgLVal.getAddress()}});
    }
    ++Cnt;
    ++I;
  }

  return {F, HasUIntPtrArgs};
}

llvm::Function *
CodeGenFunction::GenerateOpenMPCapturedStmtFunction(const CapturedStmt &S) {
  assert(
      CapturedStmtInfo &&
      "CapturedStmtInfo should be set when generating the captured function");
  const CapturedDecl *CD = S.getCapturedDecl();
  // Build the argument list.
  bool NeedWrapperFunction =
      getDebugInfo() &&
      CGM.getCodeGenOpts().getDebugInfo() >= codegenoptions::LimitedDebugInfo;
  FunctionArgList Args;
  llvm::DenseMap<const Decl *, std::pair<const VarDecl *, Address>> LocalAddrs;
  llvm::DenseMap<const Decl *, std::pair<const Expr *, llvm::Value *>> VLASizes;
  FunctionOptions FO(&S, !NeedWrapperFunction, /*RegisterCastedArgsOnly=*/false,
                     CapturedStmtInfo->getHelperName());
  llvm::Function *F;
  bool HasUIntPtrArgs;
  std::tie(F, HasUIntPtrArgs) = emitOutlinedFunctionPrologue(
      *this, Args, LocalAddrs, VLASizes, CXXThisValue, FO);
  for (const auto &LocalAddrPair : LocalAddrs) {
    if (LocalAddrPair.second.first) {
      setAddrOfLocalVar(LocalAddrPair.second.first,
                        LocalAddrPair.second.second);
    }
  }
  for (const auto &VLASizePair : VLASizes)
    VLASizeMap[VLASizePair.second.first] = VLASizePair.second.second;
  PGO.assignRegionCounters(GlobalDecl(CD), F);
  CapturedStmtInfo->EmitBody(*this, CD->getBody());
  FinishFunction(CD->getBodyRBrace());
  if (!NeedWrapperFunction || !HasUIntPtrArgs)
    return F;

  FunctionOptions WrapperFO(&S, /*UIntPtrCastRequired=*/true,
                            /*RegisterCastedArgsOnly=*/true,
                            ".nondebug_wrapper.");
  CodeGenFunction WrapperCGF(CGM, /*suppressNewContext=*/true);
  WrapperCGF.disableDebugInfo();
  Args.clear();
  LocalAddrs.clear();
  VLASizes.clear();
  llvm::Function *WrapperF =
      emitOutlinedFunctionPrologue(WrapperCGF, Args, LocalAddrs, VLASizes,
                                   WrapperCGF.CXXThisValue, WrapperFO).first;
  LValueBaseInfo BaseInfo(AlignmentSource::Decl, false);
  llvm::SmallVector<llvm::Value *, 4> CallArgs;
  for (const auto *Arg : Args) {
    llvm::Value *CallArg;
    auto I = LocalAddrs.find(Arg);
    if (I != LocalAddrs.end()) {
      LValue LV =
          WrapperCGF.MakeAddrLValue(I->second.second, Arg->getType(), BaseInfo);
      CallArg = WrapperCGF.EmitLoadOfScalar(LV, SourceLocation());
    } else {
      auto EI = VLASizes.find(Arg);
      if (EI != VLASizes.end())
        CallArg = EI->second.second;
      else {
        LValue LV = WrapperCGF.MakeAddrLValue(WrapperCGF.GetAddrOfLocalVar(Arg),
                                              Arg->getType(), BaseInfo);
        CallArg = WrapperCGF.EmitLoadOfScalar(LV, SourceLocation());
      }
    }
    CallArgs.emplace_back(CallArg);
  }
  WrapperCGF.Builder.CreateCall(F, CallArgs);
  WrapperCGF.FinishFunction();
  return WrapperF;
}

//===----------------------------------------------------------------------===//
//                              OpenMP Directive Emission
//===----------------------------------------------------------------------===//
void CodeGenFunction::EmitOMPAggregateAssign(
    Address DestAddr, Address SrcAddr, QualType OriginalType,
    const llvm::function_ref<void(Address, Address)> &CopyGen) {
  // Perform element-by-element initialization.
  QualType ElementTy;

  // Drill down to the base element type on both arrays.
  auto ArrayTy = OriginalType->getAsArrayTypeUnsafe();
  auto NumElements = emitArrayLength(ArrayTy, ElementTy, DestAddr);
  SrcAddr = Builder.CreateElementBitCast(SrcAddr, DestAddr.getElementType());

  auto SrcBegin = SrcAddr.getPointer();
  auto DestBegin = DestAddr.getPointer();
  // Cast from pointer to array type to pointer to single element.
  auto DestEnd = Builder.CreateGEP(DestBegin, NumElements);
  // The basic structure here is a while-do loop.
  auto BodyBB = createBasicBlock("omp.arraycpy.body");
  auto DoneBB = createBasicBlock("omp.arraycpy.done");
  auto IsEmpty =
      Builder.CreateICmpEQ(DestBegin, DestEnd, "omp.arraycpy.isempty");
  Builder.CreateCondBr(IsEmpty, DoneBB, BodyBB);

  // Enter the loop body, making that address the current address.
  auto EntryBB = Builder.GetInsertBlock();
  EmitBlock(BodyBB);

  CharUnits ElementSize = getContext().getTypeSizeInChars(ElementTy);

  llvm::PHINode *SrcElementPHI =
    Builder.CreatePHI(SrcBegin->getType(), 2, "omp.arraycpy.srcElementPast");
  SrcElementPHI->addIncoming(SrcBegin, EntryBB);
  Address SrcElementCurrent =
      Address(SrcElementPHI,
              SrcAddr.getAlignment().alignmentOfArrayElement(ElementSize));

  llvm::PHINode *DestElementPHI =
    Builder.CreatePHI(DestBegin->getType(), 2, "omp.arraycpy.destElementPast");
  DestElementPHI->addIncoming(DestBegin, EntryBB);
  Address DestElementCurrent =
    Address(DestElementPHI,
            DestAddr.getAlignment().alignmentOfArrayElement(ElementSize));

  // Emit copy.
  CopyGen(DestElementCurrent, SrcElementCurrent);

  // Shift the address forward by one element.
  auto DestElementNext = Builder.CreateConstGEP1_32(
      DestElementPHI, /*Idx0=*/1, "omp.arraycpy.dest.element");
  auto SrcElementNext = Builder.CreateConstGEP1_32(
      SrcElementPHI, /*Idx0=*/1, "omp.arraycpy.src.element");
  // Check whether we've reached the end.
  auto Done =
      Builder.CreateICmpEQ(DestElementNext, DestEnd, "omp.arraycpy.done");
  Builder.CreateCondBr(Done, DoneBB, BodyBB);
  DestElementPHI->addIncoming(DestElementNext, Builder.GetInsertBlock());
  SrcElementPHI->addIncoming(SrcElementNext, Builder.GetInsertBlock());

  // Done.
  EmitBlock(DoneBB, /*IsFinished=*/true);
}

void CodeGenFunction::EmitOMPCopy(QualType OriginalType, Address DestAddr,
                                  Address SrcAddr, const VarDecl *DestVD,
                                  const VarDecl *SrcVD, const Expr *Copy) {
  if (OriginalType->isArrayType()) {
    auto *BO = dyn_cast<BinaryOperator>(Copy);
    if (BO && BO->getOpcode() == BO_Assign) {
      // Perform simple memcpy for simple copying.
      EmitAggregateAssign(DestAddr, SrcAddr, OriginalType);
    } else {
      // For arrays with complex element types perform element by element
      // copying.
      EmitOMPAggregateAssign(
          DestAddr, SrcAddr, OriginalType,
          [this, Copy, SrcVD, DestVD](Address DestElement, Address SrcElement) {
            // Working with the single array element, so have to remap
            // destination and source variables to corresponding array
            // elements.
            CodeGenFunction::OMPPrivateScope Remap(*this);
            Remap.addPrivate(DestVD, [DestElement]() -> Address {
              return DestElement;
            });
            Remap.addPrivate(
                SrcVD, [SrcElement]() -> Address { return SrcElement; });
            (void)Remap.Privatize();
            EmitIgnoredExpr(Copy);
          });
    }
  } else {
    // Remap pseudo source variable to private copy.
    CodeGenFunction::OMPPrivateScope Remap(*this);
    Remap.addPrivate(SrcVD, [SrcAddr]() -> Address { return SrcAddr; });
    Remap.addPrivate(DestVD, [DestAddr]() -> Address { return DestAddr; });
    (void)Remap.Privatize();
    // Emit copying of the whole variable.
    EmitIgnoredExpr(Copy);
  }
}

bool CodeGenFunction::EmitOMPFirstprivateClause(const OMPExecutableDirective &D,
                                                OMPPrivateScope &PrivateScope) {
  if (!HaveInsertPoint())
    return false;
  bool FirstprivateIsLastprivate = false;
  llvm::DenseSet<const VarDecl *> Lastprivates;
  for (const auto *C : D.getClausesOfKind<OMPLastprivateClause>()) {
    for (const auto *D : C->varlists())
      Lastprivates.insert(
          cast<VarDecl>(cast<DeclRefExpr>(D)->getDecl())->getCanonicalDecl());
  }
  llvm::DenseSet<const VarDecl *> EmittedAsFirstprivate;
  CGCapturedStmtInfo CapturesInfo(cast<CapturedStmt>(*D.getAssociatedStmt()));
  for (const auto *C : D.getClausesOfKind<OMPFirstprivateClause>()) {
    auto IRef = C->varlist_begin();
    auto InitsRef = C->inits().begin();
    for (auto IInit : C->private_copies()) {
      auto *OrigVD = cast<VarDecl>(cast<DeclRefExpr>(*IRef)->getDecl());
      bool ThisFirstprivateIsLastprivate =
          Lastprivates.count(OrigVD->getCanonicalDecl()) > 0;
      auto *CapFD = CapturesInfo.lookup(OrigVD);
      auto *FD = CapturedStmtInfo->lookup(OrigVD);
      if (!ThisFirstprivateIsLastprivate && FD && (FD == CapFD) &&
          !FD->getType()->isReferenceType()) {
        EmittedAsFirstprivate.insert(OrigVD->getCanonicalDecl());
        ++IRef;
        ++InitsRef;
        continue;
      }
      FirstprivateIsLastprivate =
          FirstprivateIsLastprivate || ThisFirstprivateIsLastprivate;
      if (EmittedAsFirstprivate.insert(OrigVD->getCanonicalDecl()).second) {
        auto *VD = cast<VarDecl>(cast<DeclRefExpr>(IInit)->getDecl());
        auto *VDInit = cast<VarDecl>(cast<DeclRefExpr>(*InitsRef)->getDecl());
        bool IsRegistered;
        DeclRefExpr DRE(const_cast<VarDecl *>(OrigVD),
                        /*RefersToEnclosingVariableOrCapture=*/FD != nullptr,
                        (*IRef)->getType(), VK_LValue, (*IRef)->getExprLoc());
        Address OriginalAddr = EmitLValue(&DRE).getAddress();
        QualType Type = VD->getType();
        if (Type->isArrayType()) {
          // Emit VarDecl with copy init for arrays.
          // Get the address of the original variable captured in current
          // captured region.
          IsRegistered = PrivateScope.addPrivate(OrigVD, [&]() -> Address {
            auto Emission = EmitAutoVarAlloca(*VD);
            auto *Init = VD->getInit();
            if (!isa<CXXConstructExpr>(Init) || isTrivialInitializer(Init)) {
              // Perform simple memcpy.
              EmitAggregateAssign(Emission.getAllocatedAddress(), OriginalAddr,
                                  Type);
            } else {
              EmitOMPAggregateAssign(
                  Emission.getAllocatedAddress(), OriginalAddr, Type,
                  [this, VDInit, Init](Address DestElement,
                                       Address SrcElement) {
                    // Clean up any temporaries needed by the initialization.
                    RunCleanupsScope InitScope(*this);
                    // Emit initialization for single element.
                    setAddrOfLocalVar(VDInit, SrcElement);
                    EmitAnyExprToMem(Init, DestElement,
                                     Init->getType().getQualifiers(),
                                     /*IsInitializer*/ false);
                    LocalDeclMap.erase(VDInit);
                  });
            }
            EmitAutoVarCleanups(Emission);
            return Emission.getAllocatedAddress();
          });
        } else {
          IsRegistered = PrivateScope.addPrivate(OrigVD, [&]() -> Address {
            // Emit private VarDecl with copy init.
            // Remap temp VDInit variable to the address of the original
            // variable
            // (for proper handling of captured global variables).
            setAddrOfLocalVar(VDInit, OriginalAddr);
            EmitDecl(*VD);
            LocalDeclMap.erase(VDInit);
            return GetAddrOfLocalVar(VD);
          });
        }
        assert(IsRegistered &&
               "firstprivate var already registered as private");
        // Silence the warning about unused variable.
        (void)IsRegistered;
      }
      ++IRef;
      ++InitsRef;
    }
  }
  return FirstprivateIsLastprivate && !EmittedAsFirstprivate.empty();
}

void CodeGenFunction::EmitOMPPrivateClause(
    const OMPExecutableDirective &D,
    CodeGenFunction::OMPPrivateScope &PrivateScope) {
  if (!HaveInsertPoint())
    return;
  llvm::DenseSet<const VarDecl *> EmittedAsPrivate;
  for (const auto *C : D.getClausesOfKind<OMPPrivateClause>()) {
    auto IRef = C->varlist_begin();
    for (auto IInit : C->private_copies()) {
      auto *OrigVD = cast<VarDecl>(cast<DeclRefExpr>(*IRef)->getDecl());
      if (EmittedAsPrivate.insert(OrigVD->getCanonicalDecl()).second) {
        auto VD = cast<VarDecl>(cast<DeclRefExpr>(IInit)->getDecl());
        bool IsRegistered =
            PrivateScope.addPrivate(OrigVD, [&]() -> Address {
              // Emit private VarDecl with copy init.
              EmitDecl(*VD);
              return GetAddrOfLocalVar(VD);
            });
        assert(IsRegistered && "private var already registered as private");
        // Silence the warning about unused variable.
        (void)IsRegistered;
      }
      ++IRef;
    }
  }
}

bool CodeGenFunction::EmitOMPCopyinClause(const OMPExecutableDirective &D) {
  if (!HaveInsertPoint())
    return false;
  // threadprivate_var1 = master_threadprivate_var1;
  // operator=(threadprivate_var2, master_threadprivate_var2);
  // ...
  // __kmpc_barrier(&loc, global_tid);
  llvm::DenseSet<const VarDecl *> CopiedVars;
  llvm::BasicBlock *CopyBegin = nullptr, *CopyEnd = nullptr;
  for (const auto *C : D.getClausesOfKind<OMPCopyinClause>()) {
    auto IRef = C->varlist_begin();
    auto ISrcRef = C->source_exprs().begin();
    auto IDestRef = C->destination_exprs().begin();
    for (auto *AssignOp : C->assignment_ops()) {
      auto *VD = cast<VarDecl>(cast<DeclRefExpr>(*IRef)->getDecl());
      QualType Type = VD->getType();
      if (CopiedVars.insert(VD->getCanonicalDecl()).second) {
        // Get the address of the master variable. If we are emitting code with
        // TLS support, the address is passed from the master as field in the
        // captured declaration.
        Address MasterAddr = Address::invalid();
        if (getLangOpts().OpenMPUseTLS &&
            getContext().getTargetInfo().isTLSSupported()) {
          assert(CapturedStmtInfo->lookup(VD) &&
                 "Copyin threadprivates should have been captured!");
          DeclRefExpr DRE(const_cast<VarDecl *>(VD), true, (*IRef)->getType(),
                          VK_LValue, (*IRef)->getExprLoc());
          MasterAddr = EmitLValue(&DRE).getAddress();
          LocalDeclMap.erase(VD);
        } else {
          MasterAddr =
            Address(VD->isStaticLocal() ? CGM.getStaticLocalDeclAddress(VD)
                                        : CGM.GetAddrOfGlobal(VD),
                    getContext().getDeclAlign(VD));
        }
        // Get the address of the threadprivate variable.
        Address PrivateAddr = EmitLValue(*IRef).getAddress();
        if (CopiedVars.size() == 1) {
          // At first check if current thread is a master thread. If it is, no
          // need to copy data.
          CopyBegin = createBasicBlock("copyin.not.master");
          CopyEnd = createBasicBlock("copyin.not.master.end");
          Builder.CreateCondBr(
              Builder.CreateICmpNE(
                  Builder.CreatePtrToInt(MasterAddr.getPointer(), CGM.IntPtrTy),
                  Builder.CreatePtrToInt(PrivateAddr.getPointer(), CGM.IntPtrTy)),
              CopyBegin, CopyEnd);
          EmitBlock(CopyBegin);
        }
        auto *SrcVD = cast<VarDecl>(cast<DeclRefExpr>(*ISrcRef)->getDecl());
        auto *DestVD = cast<VarDecl>(cast<DeclRefExpr>(*IDestRef)->getDecl());
        EmitOMPCopy(Type, PrivateAddr, MasterAddr, DestVD, SrcVD, AssignOp);
      }
      ++IRef;
      ++ISrcRef;
      ++IDestRef;
    }
  }
  if (CopyEnd) {
    // Exit out of copying procedure for non-master thread.
    EmitBlock(CopyEnd, /*IsFinished=*/true);
    return true;
  }
  return false;
}

bool CodeGenFunction::EmitOMPLastprivateClauseInit(
    const OMPExecutableDirective &D, OMPPrivateScope &PrivateScope) {
  if (!HaveInsertPoint())
    return false;
  bool HasAtLeastOneLastprivate = false;
  llvm::DenseSet<const VarDecl *> SIMDLCVs;
  if (isOpenMPSimdDirective(D.getDirectiveKind())) {
    auto *LoopDirective = cast<OMPLoopDirective>(&D);
    for (auto *C : LoopDirective->counters()) {
      SIMDLCVs.insert(
          cast<VarDecl>(cast<DeclRefExpr>(C)->getDecl())->getCanonicalDecl());
    }
  }
  llvm::DenseSet<const VarDecl *> AlreadyEmittedVars;
  for (const auto *C : D.getClausesOfKind<OMPLastprivateClause>()) {
    HasAtLeastOneLastprivate = true;
    if (isOpenMPTaskLoopDirective(D.getDirectiveKind()))
      break;
    auto IRef = C->varlist_begin();
    auto IDestRef = C->destination_exprs().begin();
    for (auto *IInit : C->private_copies()) {
      // Keep the address of the original variable for future update at the end
      // of the loop.
      auto *OrigVD = cast<VarDecl>(cast<DeclRefExpr>(*IRef)->getDecl());
      // Taskloops do not require additional initialization, it is done in
      // runtime support library.
      if (AlreadyEmittedVars.insert(OrigVD->getCanonicalDecl()).second) {
        auto *DestVD = cast<VarDecl>(cast<DeclRefExpr>(*IDestRef)->getDecl());
        PrivateScope.addPrivate(DestVD, [this, OrigVD, IRef]() -> Address {
          DeclRefExpr DRE(
              const_cast<VarDecl *>(OrigVD),
              /*RefersToEnclosingVariableOrCapture=*/CapturedStmtInfo->lookup(
                  OrigVD) != nullptr,
              (*IRef)->getType(), VK_LValue, (*IRef)->getExprLoc());
          return EmitLValue(&DRE).getAddress();
        });
        // Check if the variable is also a firstprivate: in this case IInit is
        // not generated. Initialization of this variable will happen in codegen
        // for 'firstprivate' clause.
        if (IInit && !SIMDLCVs.count(OrigVD->getCanonicalDecl())) {
          auto *VD = cast<VarDecl>(cast<DeclRefExpr>(IInit)->getDecl());
          bool IsRegistered = PrivateScope.addPrivate(OrigVD, [&]() -> Address {
            // Emit private VarDecl with copy init.
            EmitDecl(*VD);
            return GetAddrOfLocalVar(VD);
          });
          assert(IsRegistered &&
                 "lastprivate var already registered as private");
          (void)IsRegistered;
        }
      }
      ++IRef;
      ++IDestRef;
    }
  }
  return HasAtLeastOneLastprivate;
}

void CodeGenFunction::EmitOMPLastprivateClauseFinal(
    const OMPExecutableDirective &D, bool NoFinals,
    llvm::Value *IsLastIterCond) {
  if (!HaveInsertPoint())
    return;
  // Emit following code:
  // if (<IsLastIterCond>) {
  //   orig_var1 = private_orig_var1;
  //   ...
  //   orig_varn = private_orig_varn;
  // }
  llvm::BasicBlock *ThenBB = nullptr;
  llvm::BasicBlock *DoneBB = nullptr;
  if (IsLastIterCond) {
    ThenBB = createBasicBlock(".omp.lastprivate.then");
    DoneBB = createBasicBlock(".omp.lastprivate.done");
    Builder.CreateCondBr(IsLastIterCond, ThenBB, DoneBB);
    EmitBlock(ThenBB);
  }
  llvm::DenseSet<const VarDecl *> AlreadyEmittedVars;
  llvm::DenseMap<const VarDecl *, const Expr *> LoopCountersAndUpdates;
  if (auto *LoopDirective = dyn_cast<OMPLoopDirective>(&D)) {
    auto IC = LoopDirective->counters().begin();
    for (auto F : LoopDirective->finals()) {
      auto *D =
          cast<VarDecl>(cast<DeclRefExpr>(*IC)->getDecl())->getCanonicalDecl();
      if (NoFinals)
        AlreadyEmittedVars.insert(D);
      else
        LoopCountersAndUpdates[D] = F;
      ++IC;
    }
  }
  for (const auto *C : D.getClausesOfKind<OMPLastprivateClause>()) {
    auto IRef = C->varlist_begin();
    auto ISrcRef = C->source_exprs().begin();
    auto IDestRef = C->destination_exprs().begin();
    for (auto *AssignOp : C->assignment_ops()) {
      auto *PrivateVD = cast<VarDecl>(cast<DeclRefExpr>(*IRef)->getDecl());
      QualType Type = PrivateVD->getType();
      auto *CanonicalVD = PrivateVD->getCanonicalDecl();
      if (AlreadyEmittedVars.insert(CanonicalVD).second) {
        // If lastprivate variable is a loop control variable for loop-based
        // directive, update its value before copyin back to original
        // variable.
        if (auto *FinalExpr = LoopCountersAndUpdates.lookup(CanonicalVD))
          EmitIgnoredExpr(FinalExpr);
        auto *SrcVD = cast<VarDecl>(cast<DeclRefExpr>(*ISrcRef)->getDecl());
        auto *DestVD = cast<VarDecl>(cast<DeclRefExpr>(*IDestRef)->getDecl());
        // Get the address of the original variable.
        Address OriginalAddr = GetAddrOfLocalVar(DestVD);
        // Get the address of the private variable.
        Address PrivateAddr = GetAddrOfLocalVar(PrivateVD);
        if (auto RefTy = PrivateVD->getType()->getAs<ReferenceType>())
          PrivateAddr =
              Address(Builder.CreateLoad(PrivateAddr),
                      getNaturalTypeAlignment(RefTy->getPointeeType()));
        EmitOMPCopy(Type, OriginalAddr, PrivateAddr, DestVD, SrcVD, AssignOp);
      }
      ++IRef;
      ++ISrcRef;
      ++IDestRef;
    }
    if (auto *PostUpdate = C->getPostUpdateExpr())
      EmitIgnoredExpr(PostUpdate);
  }
  if (IsLastIterCond)
    EmitBlock(DoneBB, /*IsFinished=*/true);
}

void CodeGenFunction::EmitOMPReductionClauseInit(
    const OMPExecutableDirective &D,
    CodeGenFunction::OMPPrivateScope &PrivateScope) {
  if (!HaveInsertPoint())
    return;
  SmallVector<const Expr *, 4> Shareds;
  SmallVector<const Expr *, 4> Privates;
  SmallVector<const Expr *, 4> ReductionOps;
  SmallVector<const Expr *, 4> LHSs;
  SmallVector<const Expr *, 4> RHSs;
  for (const auto *C : D.getClausesOfKind<OMPReductionClause>()) {
    auto IPriv = C->privates().begin();
    auto IRed = C->reduction_ops().begin();
    auto ILHS = C->lhs_exprs().begin();
    auto IRHS = C->rhs_exprs().begin();
    for (const auto *Ref : C->varlists()) {
      Shareds.emplace_back(Ref);
      Privates.emplace_back(*IPriv);
      ReductionOps.emplace_back(*IRed);
      LHSs.emplace_back(*ILHS);
      RHSs.emplace_back(*IRHS);
      std::advance(IPriv, 1);
      std::advance(IRed, 1);
      std::advance(ILHS, 1);
      std::advance(IRHS, 1);
    }
  }
  ReductionCodeGen RedCG(Shareds, Privates, ReductionOps);
  unsigned Count = 0;
  auto ILHS = LHSs.begin();
  auto IRHS = RHSs.begin();
  auto IPriv = Privates.begin();
  for (const auto *IRef : Shareds) {
    auto *PrivateVD = cast<VarDecl>(cast<DeclRefExpr>(*IPriv)->getDecl());
    // Emit private VarDecl with reduction init.
    RedCG.emitSharedLValue(*this, Count);
    RedCG.emitAggregateType(*this, Count);
    auto Emission = EmitAutoVarAlloca(*PrivateVD);
    RedCG.emitInitialization(*this, Count, Emission.getAllocatedAddress(),
                             RedCG.getSharedLValue(Count),
                             [&Emission](CodeGenFunction &CGF) {
                               CGF.EmitAutoVarInit(Emission);
                               return true;
                             });
    EmitAutoVarCleanups(Emission);
    Address BaseAddr = RedCG.adjustPrivateAddress(
        *this, Count, Emission.getAllocatedAddress());
    bool IsRegistered = PrivateScope.addPrivate(
        RedCG.getBaseDecl(Count), [BaseAddr]() -> Address { return BaseAddr; });
    assert(IsRegistered && "private var already registered as private");
    // Silence the warning about unused variable.
    (void)IsRegistered;

    auto *LHSVD = cast<VarDecl>(cast<DeclRefExpr>(*ILHS)->getDecl());
    auto *RHSVD = cast<VarDecl>(cast<DeclRefExpr>(*IRHS)->getDecl());
    if (isa<OMPArraySectionExpr>(IRef)) {
      // Store the address of the original variable associated with the LHS
      // implicit variable.
      PrivateScope.addPrivate(LHSVD, [&RedCG, Count]() -> Address {
        return RedCG.getSharedLValue(Count).getAddress();
      });
      PrivateScope.addPrivate(RHSVD, [this, PrivateVD]() -> Address {
        return GetAddrOfLocalVar(PrivateVD);
      });
    } else if (isa<ArraySubscriptExpr>(IRef)) {
      // Store the address of the original variable associated with the LHS
      // implicit variable.
      PrivateScope.addPrivate(LHSVD, [&RedCG, Count]() -> Address {
        return RedCG.getSharedLValue(Count).getAddress();
      });
      PrivateScope.addPrivate(RHSVD, [this, PrivateVD, RHSVD]() -> Address {
        return Builder.CreateElementBitCast(GetAddrOfLocalVar(PrivateVD),
                                            ConvertTypeForMem(RHSVD->getType()),
                                            "rhs.begin");
      });
    } else {
      QualType Type = PrivateVD->getType();
      bool IsArray = getContext().getAsArrayType(Type) != nullptr;
      Address OriginalAddr = RedCG.getSharedLValue(Count).getAddress();
      // Store the address of the original variable associated with the LHS
      // implicit variable.
      if (IsArray) {
        OriginalAddr = Builder.CreateElementBitCast(
            OriginalAddr, ConvertTypeForMem(LHSVD->getType()), "lhs.begin");
      }
      PrivateScope.addPrivate(
          LHSVD, [OriginalAddr]() -> Address { return OriginalAddr; });
      PrivateScope.addPrivate(
          RHSVD, [this, PrivateVD, RHSVD, IsArray]() -> Address {
            return IsArray
                       ? Builder.CreateElementBitCast(
                             GetAddrOfLocalVar(PrivateVD),
                             ConvertTypeForMem(RHSVD->getType()), "rhs.begin")
                       : GetAddrOfLocalVar(PrivateVD);
          });
    }
    ++ILHS;
    ++IRHS;
    ++IPriv;
    ++Count;
  }
}

void CodeGenFunction::EmitOMPReductionClauseFinal(
    const OMPExecutableDirective &D, const OpenMPDirectiveKind ReductionKind) {
  if (!HaveInsertPoint())
    return;
  llvm::SmallVector<const Expr *, 8> Privates;
  llvm::SmallVector<const Expr *, 8> LHSExprs;
  llvm::SmallVector<const Expr *, 8> RHSExprs;
  llvm::SmallVector<const Expr *, 8> ReductionOps;
  bool HasAtLeastOneReduction = false;
  for (const auto *C : D.getClausesOfKind<OMPReductionClause>()) {
    HasAtLeastOneReduction = true;
    Privates.append(C->privates().begin(), C->privates().end());
    LHSExprs.append(C->lhs_exprs().begin(), C->lhs_exprs().end());
    RHSExprs.append(C->rhs_exprs().begin(), C->rhs_exprs().end());
    ReductionOps.append(C->reduction_ops().begin(), C->reduction_ops().end());
  }
  if (HasAtLeastOneReduction) {
    bool WithNowait = D.getSingleClause<OMPNowaitClause>() ||
                      isOpenMPParallelDirective(D.getDirectiveKind()) ||
                      D.getDirectiveKind() == OMPD_simd;
    bool SimpleReduction = D.getDirectiveKind() == OMPD_simd;
    // Emit nowait reduction if nowait clause is present or directive is a
    // parallel directive (it always has implicit barrier).
    CGM.getOpenMPRuntime().emitReduction(
        *this, D.getLocEnd(), Privates, LHSExprs, RHSExprs, ReductionOps,
        {WithNowait, SimpleReduction, ReductionKind});
  }
}

static void emitPostUpdateForReductionClause(
    CodeGenFunction &CGF, const OMPExecutableDirective &D,
    const llvm::function_ref<llvm::Value *(CodeGenFunction &)> &CondGen) {
  if (!CGF.HaveInsertPoint())
    return;
  llvm::BasicBlock *DoneBB = nullptr;
  for (const auto *C : D.getClausesOfKind<OMPReductionClause>()) {
    if (auto *PostUpdate = C->getPostUpdateExpr()) {
      if (!DoneBB) {
        if (auto *Cond = CondGen(CGF)) {
          // If the first post-update expression is found, emit conditional
          // block if it was requested.
          auto *ThenBB = CGF.createBasicBlock(".omp.reduction.pu");
          DoneBB = CGF.createBasicBlock(".omp.reduction.pu.done");
          CGF.Builder.CreateCondBr(Cond, ThenBB, DoneBB);
          CGF.EmitBlock(ThenBB);
        }
      }
      CGF.EmitIgnoredExpr(PostUpdate);
    }
  }
  if (DoneBB)
    CGF.EmitBlock(DoneBB, /*IsFinished=*/true);
}

namespace {
/// Codegen lambda for appending distribute lower and upper bounds to outlined
/// parallel function. This is necessary for combined constructs such as
/// 'distribute parallel for'
typedef llvm::function_ref<void(CodeGenFunction &,
                                const OMPExecutableDirective &,
                                llvm::SmallVectorImpl<llvm::Value *> &)>
    CodeGenBoundParametersTy;
} // anonymous namespace

static void emitCommonOMPParallelDirective(
    CodeGenFunction &CGF, const OMPExecutableDirective &S,
    OpenMPDirectiveKind InnermostKind, const RegionCodeGenTy &CodeGen,
    const CodeGenBoundParametersTy &CodeGenBoundParameters) {
  const CapturedStmt *CS = S.getCapturedStmt(OMPD_parallel);
  auto OutlinedFn = CGF.CGM.getOpenMPRuntime().emitParallelOutlinedFunction(
      S, *CS->getCapturedDecl()->param_begin(), InnermostKind, CodeGen);
  if (const auto *NumThreadsClause = S.getSingleClause<OMPNumThreadsClause>()) {
    CodeGenFunction::RunCleanupsScope NumThreadsScope(CGF);
    auto NumThreads = CGF.EmitScalarExpr(NumThreadsClause->getNumThreads(),
                                         /*IgnoreResultAssign*/ true);
    CGF.CGM.getOpenMPRuntime().emitNumThreadsClause(
        CGF, NumThreads, NumThreadsClause->getLocStart());
  }
  if (const auto *ProcBindClause = S.getSingleClause<OMPProcBindClause>()) {
    CodeGenFunction::RunCleanupsScope ProcBindScope(CGF);
    CGF.CGM.getOpenMPRuntime().emitProcBindClause(
        CGF, ProcBindClause->getProcBindKind(), ProcBindClause->getLocStart());
  }
  const Expr *IfCond = nullptr;
  for (const auto *C : S.getClausesOfKind<OMPIfClause>()) {
    if (C->getNameModifier() == OMPD_unknown ||
        C->getNameModifier() == OMPD_parallel) {
      IfCond = C->getCondition();
      break;
    }
  }

  OMPParallelScope Scope(CGF, S);
  llvm::SmallVector<llvm::Value *, 16> CapturedVars;
  // Combining 'distribute' with 'for' requires sharing each 'distribute' chunk
  // lower and upper bounds with the pragma 'for' chunking mechanism.
  // The following lambda takes care of appending the lower and upper bound
  // parameters when necessary
  CodeGenBoundParameters(CGF, S, CapturedVars);
  CGF.GenerateOpenMPCapturedVars(*CS, CapturedVars);
  CGF.CGM.getOpenMPRuntime().emitParallelCall(CGF, S.getLocStart(), OutlinedFn,
                                              CapturedVars, IfCond);
}

static void emitEmptyBoundParameters(CodeGenFunction &,
                                     const OMPExecutableDirective &,
                                     llvm::SmallVectorImpl<llvm::Value *> &) {}

void CodeGenFunction::EmitOMPParallelDirective(const OMPParallelDirective &S) {
  // Emit parallel region as a standalone region.
  auto &&CodeGen = [&S](CodeGenFunction &CGF, PrePostActionTy &) {
    OMPPrivateScope PrivateScope(CGF);
    bool Copyins = CGF.EmitOMPCopyinClause(S);
    (void)CGF.EmitOMPFirstprivateClause(S, PrivateScope);
    if (Copyins) {
      // Emit implicit barrier to synchronize threads and avoid data races on
      // propagation master's thread values of threadprivate variables to local
      // instances of that variables of all other implicit threads.
      CGF.CGM.getOpenMPRuntime().emitBarrierCall(
          CGF, S.getLocStart(), OMPD_unknown, /*EmitChecks=*/false,
          /*ForceSimpleCall=*/true);
    }
    CGF.EmitOMPPrivateClause(S, PrivateScope);
    CGF.EmitOMPReductionClauseInit(S, PrivateScope);
    (void)PrivateScope.Privatize();
    CGF.EmitStmt(cast<CapturedStmt>(S.getAssociatedStmt())->getCapturedStmt());
    CGF.EmitOMPReductionClauseFinal(S, /*ReductionKind=*/OMPD_parallel);
  };
  emitCommonOMPParallelDirective(*this, S, OMPD_parallel, CodeGen,
                                 emitEmptyBoundParameters);
  emitPostUpdateForReductionClause(
      *this, S, [](CodeGenFunction &) -> llvm::Value * { return nullptr; });
}

void CodeGenFunction::EmitOMPLoopBody(const OMPLoopDirective &D,
                                      JumpDest LoopExit) {
  RunCleanupsScope BodyScope(*this);
  // Update counters values on current iteration.
  for (auto I : D.updates()) {
    EmitIgnoredExpr(I);
  }
  // Update the linear variables.
  for (const auto *C : D.getClausesOfKind<OMPLinearClause>()) {
    for (auto *U : C->updates())
      EmitIgnoredExpr(U);
  }

  // On a continue in the body, jump to the end.
  auto Continue = getJumpDestInCurrentScope("omp.body.continue");
  BreakContinueStack.push_back(BreakContinue(LoopExit, Continue));
  // Emit loop body.
  EmitStmt(D.getBody());
  // The end (updates/cleanups).
  EmitBlock(Continue.getBlock());
  BreakContinueStack.pop_back();
}

void CodeGenFunction::EmitOMPInnerLoop(
    const Stmt &S, bool RequiresCleanup, const Expr *LoopCond,
    const Expr *IncExpr,
    const llvm::function_ref<void(CodeGenFunction &)> &BodyGen,
    const llvm::function_ref<void(CodeGenFunction &)> &PostIncGen) {
  auto LoopExit = getJumpDestInCurrentScope("omp.inner.for.end");

  // Start the loop with a block that tests the condition.
  auto CondBlock = createBasicBlock("omp.inner.for.cond");
  EmitBlock(CondBlock);
  const SourceRange &R = S.getSourceRange();
  LoopStack.push(CondBlock, SourceLocToDebugLoc(R.getBegin()),
                 SourceLocToDebugLoc(R.getEnd()));

  // If there are any cleanups between here and the loop-exit scope,
  // create a block to stage a loop exit along.
  auto ExitBlock = LoopExit.getBlock();
  if (RequiresCleanup)
    ExitBlock = createBasicBlock("omp.inner.for.cond.cleanup");

  auto LoopBody = createBasicBlock("omp.inner.for.body");

  // Emit condition.
  EmitBranchOnBoolExpr(LoopCond, LoopBody, ExitBlock, getProfileCount(&S));
  if (ExitBlock != LoopExit.getBlock()) {
    EmitBlock(ExitBlock);
    EmitBranchThroughCleanup(LoopExit);
  }

  EmitBlock(LoopBody);
  incrementProfileCounter(&S);

  // Create a block for the increment.
  auto Continue = getJumpDestInCurrentScope("omp.inner.for.inc");
  BreakContinueStack.push_back(BreakContinue(LoopExit, Continue));

  BodyGen(*this);

  // Emit "IV = IV + 1" and a back-edge to the condition block.
  EmitBlock(Continue.getBlock());
  EmitIgnoredExpr(IncExpr);
  PostIncGen(*this);
  BreakContinueStack.pop_back();
  EmitBranch(CondBlock);
  LoopStack.pop();
  // Emit the fall-through block.
  EmitBlock(LoopExit.getBlock());
}

bool CodeGenFunction::EmitOMPLinearClauseInit(const OMPLoopDirective &D) {
  if (!HaveInsertPoint())
    return false;
  // Emit inits for the linear variables.
  bool HasLinears = false;
  for (const auto *C : D.getClausesOfKind<OMPLinearClause>()) {
    for (auto *Init : C->inits()) {
      HasLinears = true;
      auto *VD = cast<VarDecl>(cast<DeclRefExpr>(Init)->getDecl());
      if (auto *Ref = dyn_cast<DeclRefExpr>(VD->getInit()->IgnoreImpCasts())) {
        AutoVarEmission Emission = EmitAutoVarAlloca(*VD);
        auto *OrigVD = cast<VarDecl>(Ref->getDecl());
        DeclRefExpr DRE(const_cast<VarDecl *>(OrigVD),
                        CapturedStmtInfo->lookup(OrigVD) != nullptr,
                        VD->getInit()->getType(), VK_LValue,
                        VD->getInit()->getExprLoc());
        EmitExprAsInit(&DRE, VD, MakeAddrLValue(Emission.getAllocatedAddress(),
                                                VD->getType()),
                       /*capturedByInit=*/false);
        EmitAutoVarCleanups(Emission);
      } else
        EmitVarDecl(*VD);
    }
    // Emit the linear steps for the linear clauses.
    // If a step is not constant, it is pre-calculated before the loop.
    if (auto CS = cast_or_null<BinaryOperator>(C->getCalcStep()))
      if (auto SaveRef = cast<DeclRefExpr>(CS->getLHS())) {
        EmitVarDecl(*cast<VarDecl>(SaveRef->getDecl()));
        // Emit calculation of the linear step.
        EmitIgnoredExpr(CS);
      }
  }
  return HasLinears;
}

void CodeGenFunction::EmitOMPLinearClauseFinal(
    const OMPLoopDirective &D,
    const llvm::function_ref<llvm::Value *(CodeGenFunction &)> &CondGen) {
  if (!HaveInsertPoint())
    return;
  llvm::BasicBlock *DoneBB = nullptr;
  // Emit the final values of the linear variables.
  for (const auto *C : D.getClausesOfKind<OMPLinearClause>()) {
    auto IC = C->varlist_begin();
    for (auto *F : C->finals()) {
      if (!DoneBB) {
        if (auto *Cond = CondGen(*this)) {
          // If the first post-update expression is found, emit conditional
          // block if it was requested.
          auto *ThenBB = createBasicBlock(".omp.linear.pu");
          DoneBB = createBasicBlock(".omp.linear.pu.done");
          Builder.CreateCondBr(Cond, ThenBB, DoneBB);
          EmitBlock(ThenBB);
        }
      }
      auto *OrigVD = cast<VarDecl>(cast<DeclRefExpr>(*IC)->getDecl());
      DeclRefExpr DRE(const_cast<VarDecl *>(OrigVD),
                      CapturedStmtInfo->lookup(OrigVD) != nullptr,
                      (*IC)->getType(), VK_LValue, (*IC)->getExprLoc());
      Address OrigAddr = EmitLValue(&DRE).getAddress();
      CodeGenFunction::OMPPrivateScope VarScope(*this);
      VarScope.addPrivate(OrigVD, [OrigAddr]() -> Address { return OrigAddr; });
      (void)VarScope.Privatize();
      EmitIgnoredExpr(F);
      ++IC;
    }
    if (auto *PostUpdate = C->getPostUpdateExpr())
      EmitIgnoredExpr(PostUpdate);
  }
  if (DoneBB)
    EmitBlock(DoneBB, /*IsFinished=*/true);
}

static void emitAlignedClause(CodeGenFunction &CGF,
                              const OMPExecutableDirective &D) {
  if (!CGF.HaveInsertPoint())
    return;
  for (const auto *Clause : D.getClausesOfKind<OMPAlignedClause>()) {
    unsigned ClauseAlignment = 0;
    if (auto AlignmentExpr = Clause->getAlignment()) {
      auto AlignmentCI =
          cast<llvm::ConstantInt>(CGF.EmitScalarExpr(AlignmentExpr));
      ClauseAlignment = static_cast<unsigned>(AlignmentCI->getZExtValue());
    }
    for (auto E : Clause->varlists()) {
      unsigned Alignment = ClauseAlignment;
      if (Alignment == 0) {
        // OpenMP [2.8.1, Description]
        // If no optional parameter is specified, implementation-defined default
        // alignments for SIMD instructions on the target platforms are assumed.
        Alignment =
            CGF.getContext()
                .toCharUnitsFromBits(CGF.getContext().getOpenMPDefaultSimdAlign(
                    E->getType()->getPointeeType()))
                .getQuantity();
      }
      assert((Alignment == 0 || llvm::isPowerOf2_32(Alignment)) &&
             "alignment is not power of 2");
      if (Alignment != 0) {
        llvm::Value *PtrValue = CGF.EmitScalarExpr(E);
        CGF.EmitAlignmentAssumption(PtrValue, Alignment);
      }
    }
  }
}

void CodeGenFunction::EmitOMPPrivateLoopCounters(
    const OMPLoopDirective &S, CodeGenFunction::OMPPrivateScope &LoopScope) {
  if (!HaveInsertPoint())
    return;
  auto I = S.private_counters().begin();
  for (auto *E : S.counters()) {
    auto *VD = cast<VarDecl>(cast<DeclRefExpr>(E)->getDecl());
    auto *PrivateVD = cast<VarDecl>(cast<DeclRefExpr>(*I)->getDecl());
    (void)LoopScope.addPrivate(VD, [&]() -> Address {
      // Emit var without initialization.
      if (!LocalDeclMap.count(PrivateVD)) {
        auto VarEmission = EmitAutoVarAlloca(*PrivateVD);
        EmitAutoVarCleanups(VarEmission);
      }
      DeclRefExpr DRE(const_cast<VarDecl *>(PrivateVD),
                      /*RefersToEnclosingVariableOrCapture=*/false,
                      (*I)->getType(), VK_LValue, (*I)->getExprLoc());
      return EmitLValue(&DRE).getAddress();
    });
    if (LocalDeclMap.count(VD) || CapturedStmtInfo->lookup(VD) ||
        VD->hasGlobalStorage()) {
      (void)LoopScope.addPrivate(PrivateVD, [&]() -> Address {
        DeclRefExpr DRE(const_cast<VarDecl *>(VD),
                        LocalDeclMap.count(VD) || CapturedStmtInfo->lookup(VD),
                        E->getType(), VK_LValue, E->getExprLoc());
        return EmitLValue(&DRE).getAddress();
      });
    }
    ++I;
  }
}

static void emitPreCond(CodeGenFunction &CGF, const OMPLoopDirective &S,
                        const Expr *Cond, llvm::BasicBlock *TrueBlock,
                        llvm::BasicBlock *FalseBlock, uint64_t TrueCount) {
  if (!CGF.HaveInsertPoint())
    return;
  {
    CodeGenFunction::OMPPrivateScope PreCondScope(CGF);
    CGF.EmitOMPPrivateLoopCounters(S, PreCondScope);
    (void)PreCondScope.Privatize();
    // Get initial values of real counters.
    for (auto I : S.inits()) {
      CGF.EmitIgnoredExpr(I);
    }
  }
  // Check that loop is executed at least one time.
  CGF.EmitBranchOnBoolExpr(Cond, TrueBlock, FalseBlock, TrueCount);
}

void CodeGenFunction::EmitOMPLinearClause(
    const OMPLoopDirective &D, CodeGenFunction::OMPPrivateScope &PrivateScope) {
  if (!HaveInsertPoint())
    return;
  llvm::DenseSet<const VarDecl *> SIMDLCVs;
  if (isOpenMPSimdDirective(D.getDirectiveKind())) {
    auto *LoopDirective = cast<OMPLoopDirective>(&D);
    for (auto *C : LoopDirective->counters()) {
      SIMDLCVs.insert(
          cast<VarDecl>(cast<DeclRefExpr>(C)->getDecl())->getCanonicalDecl());
    }
  }
  for (const auto *C : D.getClausesOfKind<OMPLinearClause>()) {
    auto CurPrivate = C->privates().begin();
    for (auto *E : C->varlists()) {
      auto *VD = cast<VarDecl>(cast<DeclRefExpr>(E)->getDecl());
      auto *PrivateVD =
          cast<VarDecl>(cast<DeclRefExpr>(*CurPrivate)->getDecl());
      if (!SIMDLCVs.count(VD->getCanonicalDecl())) {
        bool IsRegistered = PrivateScope.addPrivate(VD, [&]() -> Address {
          // Emit private VarDecl with copy init.
          EmitVarDecl(*PrivateVD);
          return GetAddrOfLocalVar(PrivateVD);
        });
        assert(IsRegistered && "linear var already registered as private");
        // Silence the warning about unused variable.
        (void)IsRegistered;
      } else
        EmitVarDecl(*PrivateVD);
      ++CurPrivate;
    }
  }
}

static void emitSimdlenSafelenClause(CodeGenFunction &CGF,
                                     const OMPExecutableDirective &D,
                                     bool IsMonotonic) {
  if (!CGF.HaveInsertPoint())
    return;
  if (const auto *C = D.getSingleClause<OMPSimdlenClause>()) {
    RValue Len = CGF.EmitAnyExpr(C->getSimdlen(), AggValueSlot::ignored(),
                                 /*ignoreResult=*/true);
    llvm::ConstantInt *Val = cast<llvm::ConstantInt>(Len.getScalarVal());
    CGF.LoopStack.setVectorizeWidth(Val->getZExtValue());
    // In presence of finite 'safelen', it may be unsafe to mark all
    // the memory instructions parallel, because loop-carried
    // dependences of 'safelen' iterations are possible.
    if (!IsMonotonic)
      CGF.LoopStack.setParallel(!D.getSingleClause<OMPSafelenClause>());
  } else if (const auto *C = D.getSingleClause<OMPSafelenClause>()) {
    RValue Len = CGF.EmitAnyExpr(C->getSafelen(), AggValueSlot::ignored(),
                                 /*ignoreResult=*/true);
    llvm::ConstantInt *Val = cast<llvm::ConstantInt>(Len.getScalarVal());
    CGF.LoopStack.setVectorizeWidth(Val->getZExtValue());
    // In presence of finite 'safelen', it may be unsafe to mark all
    // the memory instructions parallel, because loop-carried
    // dependences of 'safelen' iterations are possible.
    CGF.LoopStack.setParallel(false);
  }
}

void CodeGenFunction::EmitOMPSimdInit(const OMPLoopDirective &D,
                                      bool IsMonotonic) {
  // Walk clauses and process safelen/lastprivate.
  LoopStack.setParallel(!IsMonotonic);
  LoopStack.setVectorizeEnable(true);
  emitSimdlenSafelenClause(*this, D, IsMonotonic);
}

void CodeGenFunction::EmitOMPSimdFinal(
    const OMPLoopDirective &D,
    const llvm::function_ref<llvm::Value *(CodeGenFunction &)> &CondGen) {
  if (!HaveInsertPoint())
    return;
  llvm::BasicBlock *DoneBB = nullptr;
  auto IC = D.counters().begin();
  auto IPC = D.private_counters().begin();
  for (auto F : D.finals()) {
    auto *OrigVD = cast<VarDecl>(cast<DeclRefExpr>((*IC))->getDecl());
    auto *PrivateVD = cast<VarDecl>(cast<DeclRefExpr>((*IPC))->getDecl());
    auto *CED = dyn_cast<OMPCapturedExprDecl>(OrigVD);
    if (LocalDeclMap.count(OrigVD) || CapturedStmtInfo->lookup(OrigVD) ||
        OrigVD->hasGlobalStorage() || CED) {
      if (!DoneBB) {
        if (auto *Cond = CondGen(*this)) {
          // If the first post-update expression is found, emit conditional
          // block if it was requested.
          auto *ThenBB = createBasicBlock(".omp.final.then");
          DoneBB = createBasicBlock(".omp.final.done");
          Builder.CreateCondBr(Cond, ThenBB, DoneBB);
          EmitBlock(ThenBB);
        }
      }
      Address OrigAddr = Address::invalid();
      if (CED)
        OrigAddr = EmitLValue(CED->getInit()->IgnoreImpCasts()).getAddress();
      else {
        DeclRefExpr DRE(const_cast<VarDecl *>(PrivateVD),
                        /*RefersToEnclosingVariableOrCapture=*/false,
                        (*IPC)->getType(), VK_LValue, (*IPC)->getExprLoc());
        OrigAddr = EmitLValue(&DRE).getAddress();
      }
      OMPPrivateScope VarScope(*this);
      VarScope.addPrivate(OrigVD,
                          [OrigAddr]() -> Address { return OrigAddr; });
      (void)VarScope.Privatize();
      EmitIgnoredExpr(F);
    }
    ++IC;
    ++IPC;
  }
  if (DoneBB)
    EmitBlock(DoneBB, /*IsFinished=*/true);
}

static void emitOMPLoopBodyWithStopPoint(CodeGenFunction &CGF,
                                         const OMPLoopDirective &S,
                                         CodeGenFunction::JumpDest LoopExit) {
  CGF.EmitOMPLoopBody(S, LoopExit);
  CGF.EmitStopPoint(&S);
}

void CodeGenFunction::EmitOMPSimdDirective(const OMPSimdDirective &S) {
  auto &&CodeGen = [&S](CodeGenFunction &CGF, PrePostActionTy &) {
    OMPLoopScope PreInitScope(CGF, S);
    // if (PreCond) {
    //   for (IV in 0..LastIteration) BODY;
    //   <Final counter/linear vars updates>;
    // }
    //

    // Emit: if (PreCond) - begin.
    // If the condition constant folds and can be elided, avoid emitting the
    // whole loop.
    bool CondConstant;
    llvm::BasicBlock *ContBlock = nullptr;
    if (CGF.ConstantFoldsToSimpleInteger(S.getPreCond(), CondConstant)) {
      if (!CondConstant)
        return;
    } else {
      auto *ThenBlock = CGF.createBasicBlock("simd.if.then");
      ContBlock = CGF.createBasicBlock("simd.if.end");
      emitPreCond(CGF, S, S.getPreCond(), ThenBlock, ContBlock,
                  CGF.getProfileCount(&S));
      CGF.EmitBlock(ThenBlock);
      CGF.incrementProfileCounter(&S);
    }

    // Emit the loop iteration variable.
    const Expr *IVExpr = S.getIterationVariable();
    const VarDecl *IVDecl = cast<VarDecl>(cast<DeclRefExpr>(IVExpr)->getDecl());
    CGF.EmitVarDecl(*IVDecl);
    CGF.EmitIgnoredExpr(S.getInit());

    // Emit the iterations count variable.
    // If it is not a variable, Sema decided to calculate iterations count on
    // each iteration (e.g., it is foldable into a constant).
    if (auto LIExpr = dyn_cast<DeclRefExpr>(S.getLastIteration())) {
      CGF.EmitVarDecl(*cast<VarDecl>(LIExpr->getDecl()));
      // Emit calculation of the iterations count.
      CGF.EmitIgnoredExpr(S.getCalcLastIteration());
    }

    CGF.EmitOMPSimdInit(S);

    emitAlignedClause(CGF, S);
    (void)CGF.EmitOMPLinearClauseInit(S);
    {
      OMPPrivateScope LoopScope(CGF);
      CGF.EmitOMPPrivateLoopCounters(S, LoopScope);
      CGF.EmitOMPLinearClause(S, LoopScope);
      CGF.EmitOMPPrivateClause(S, LoopScope);
      CGF.EmitOMPReductionClauseInit(S, LoopScope);
      bool HasLastprivateClause =
          CGF.EmitOMPLastprivateClauseInit(S, LoopScope);
      (void)LoopScope.Privatize();
      CGF.EmitOMPInnerLoop(S, LoopScope.requiresCleanups(), S.getCond(),
                           S.getInc(),
                           [&S](CodeGenFunction &CGF) {
                             CGF.EmitOMPLoopBody(S, JumpDest());
                             CGF.EmitStopPoint(&S);
                           },
                           [](CodeGenFunction &) {});
      CGF.EmitOMPSimdFinal(
          S, [](CodeGenFunction &) -> llvm::Value * { return nullptr; });
      // Emit final copy of the lastprivate variables at the end of loops.
      if (HasLastprivateClause)
        CGF.EmitOMPLastprivateClauseFinal(S, /*NoFinals=*/true);
      CGF.EmitOMPReductionClauseFinal(S, /*ReductionKind=*/OMPD_simd);
      emitPostUpdateForReductionClause(
          CGF, S, [](CodeGenFunction &) -> llvm::Value * { return nullptr; });
    }
    CGF.EmitOMPLinearClauseFinal(
        S, [](CodeGenFunction &) -> llvm::Value * { return nullptr; });
    // Emit: if (PreCond) - end.
    if (ContBlock) {
      CGF.EmitBranch(ContBlock);
      CGF.EmitBlock(ContBlock, true);
    }
  };
  OMPLexicalScope Scope(*this, S, /*AsInlined=*/true);
  CGM.getOpenMPRuntime().emitInlinedDirective(*this, OMPD_simd, CodeGen);
}

void CodeGenFunction::EmitOMPOuterLoop(
    bool DynamicOrOrdered, bool IsMonotonic, const OMPLoopDirective &S,
    CodeGenFunction::OMPPrivateScope &LoopScope,
    const CodeGenFunction::OMPLoopArguments &LoopArgs,
    const CodeGenFunction::CodeGenLoopTy &CodeGenLoop,
    const CodeGenFunction::CodeGenOrderedTy &CodeGenOrdered) {
  auto &RT = CGM.getOpenMPRuntime();

  const Expr *IVExpr = S.getIterationVariable();
  const unsigned IVSize = getContext().getTypeSize(IVExpr->getType());
  const bool IVSigned = IVExpr->getType()->hasSignedIntegerRepresentation();

  auto LoopExit = getJumpDestInCurrentScope("omp.dispatch.end");

  // Start the loop with a block that tests the condition.
  auto CondBlock = createBasicBlock("omp.dispatch.cond");
  EmitBlock(CondBlock);
  const SourceRange &R = S.getSourceRange();
  LoopStack.push(CondBlock, SourceLocToDebugLoc(R.getBegin()),
                 SourceLocToDebugLoc(R.getEnd()));

  llvm::Value *BoolCondVal = nullptr;
  if (!DynamicOrOrdered) {
    // UB = min(UB, GlobalUB) or
    // UB = min(UB, PrevUB) for combined loop sharing constructs (e.g.
    // 'distribute parallel for')
    EmitIgnoredExpr(LoopArgs.EUB);
    // IV = LB
    EmitIgnoredExpr(LoopArgs.Init);
    // IV < UB
    BoolCondVal = EvaluateExprAsBool(LoopArgs.Cond);
  } else {
    BoolCondVal =
        RT.emitForNext(*this, S.getLocStart(), IVSize, IVSigned, LoopArgs.IL,
                       LoopArgs.LB, LoopArgs.UB, LoopArgs.ST);
  }

  // If there are any cleanups between here and the loop-exit scope,
  // create a block to stage a loop exit along.
  auto ExitBlock = LoopExit.getBlock();
  if (LoopScope.requiresCleanups())
    ExitBlock = createBasicBlock("omp.dispatch.cleanup");

  auto LoopBody = createBasicBlock("omp.dispatch.body");
  Builder.CreateCondBr(BoolCondVal, LoopBody, ExitBlock);
  if (ExitBlock != LoopExit.getBlock()) {
    EmitBlock(ExitBlock);
    EmitBranchThroughCleanup(LoopExit);
  }
  EmitBlock(LoopBody);

  // Emit "IV = LB" (in case of static schedule, we have already calculated new
  // LB for loop condition and emitted it above).
  if (DynamicOrOrdered)
    EmitIgnoredExpr(LoopArgs.Init);

  // Create a block for the increment.
  auto Continue = getJumpDestInCurrentScope("omp.dispatch.inc");
  BreakContinueStack.push_back(BreakContinue(LoopExit, Continue));

  // Generate !llvm.loop.parallel metadata for loads and stores for loops
  // with dynamic/guided scheduling and without ordered clause.
  if (!isOpenMPSimdDirective(S.getDirectiveKind()))
    LoopStack.setParallel(!IsMonotonic);
  else
    EmitOMPSimdInit(S, IsMonotonic);

  SourceLocation Loc = S.getLocStart();

  // when 'distribute' is not combined with a 'for':
  // while (idx <= UB) { BODY; ++idx; }
  // when 'distribute' is combined with a 'for'
  // (e.g. 'distribute parallel for')
  // while (idx <= UB) { <CodeGen rest of pragma>; idx += ST; }
  EmitOMPInnerLoop(
      S, LoopScope.requiresCleanups(), LoopArgs.Cond, LoopArgs.IncExpr,
      [&S, LoopExit, &CodeGenLoop](CodeGenFunction &CGF) {
        CodeGenLoop(CGF, S, LoopExit);
      },
      [IVSize, IVSigned, Loc, &CodeGenOrdered](CodeGenFunction &CGF) {
        CodeGenOrdered(CGF, Loc, IVSize, IVSigned);
      });

  EmitBlock(Continue.getBlock());
  BreakContinueStack.pop_back();
  if (!DynamicOrOrdered) {
    // Emit "LB = LB + Stride", "UB = UB + Stride".
    EmitIgnoredExpr(LoopArgs.NextLB);
    EmitIgnoredExpr(LoopArgs.NextUB);
  }

  EmitBranch(CondBlock);
  LoopStack.pop();
  // Emit the fall-through block.
  EmitBlock(LoopExit.getBlock());

  // Tell the runtime we are done.
  auto &&CodeGen = [DynamicOrOrdered, &S](CodeGenFunction &CGF) {
    if (!DynamicOrOrdered)
      CGF.CGM.getOpenMPRuntime().emitForStaticFinish(CGF, S.getLocEnd());
  };
  OMPCancelStack.emitExit(*this, S.getDirectiveKind(), CodeGen);
}

void CodeGenFunction::EmitOMPForOuterLoop(
    const OpenMPScheduleTy &ScheduleKind, bool IsMonotonic,
    const OMPLoopDirective &S, OMPPrivateScope &LoopScope, bool Ordered,
    const OMPLoopArguments &LoopArgs,
    const CodeGenDispatchBoundsTy &CGDispatchBounds) {
  auto &RT = CGM.getOpenMPRuntime();

  // Dynamic scheduling of the outer loop (dynamic, guided, auto, runtime).
  const bool DynamicOrOrdered =
      Ordered || RT.isDynamic(ScheduleKind.Schedule);

  assert((Ordered ||
          !RT.isStaticNonchunked(ScheduleKind.Schedule,
                                 LoopArgs.Chunk != nullptr)) &&
         "static non-chunked schedule does not need outer loop");

  // Emit outer loop.
  //
  // OpenMP [2.7.1, Loop Construct, Description, table 2-1]
  // When schedule(dynamic,chunk_size) is specified, the iterations are
  // distributed to threads in the team in chunks as the threads request them.
  // Each thread executes a chunk of iterations, then requests another chunk,
  // until no chunks remain to be distributed. Each chunk contains chunk_size
  // iterations, except for the last chunk to be distributed, which may have
  // fewer iterations. When no chunk_size is specified, it defaults to 1.
  //
  // When schedule(guided,chunk_size) is specified, the iterations are assigned
  // to threads in the team in chunks as the executing threads request them.
  // Each thread executes a chunk of iterations, then requests another chunk,
  // until no chunks remain to be assigned. For a chunk_size of 1, the size of
  // each chunk is proportional to the number of unassigned iterations divided
  // by the number of threads in the team, decreasing to 1. For a chunk_size
  // with value k (greater than 1), the size of each chunk is determined in the
  // same way, with the restriction that the chunks do not contain fewer than k
  // iterations (except for the last chunk to be assigned, which may have fewer
  // than k iterations).
  //
  // When schedule(auto) is specified, the decision regarding scheduling is
  // delegated to the compiler and/or runtime system. The programmer gives the
  // implementation the freedom to choose any possible mapping of iterations to
  // threads in the team.
  //
  // When schedule(runtime) is specified, the decision regarding scheduling is
  // deferred until run time, and the schedule and chunk size are taken from the
  // run-sched-var ICV. If the ICV is set to auto, the schedule is
  // implementation defined
  //
  // while(__kmpc_dispatch_next(&LB, &UB)) {
  //   idx = LB;
  //   while (idx <= UB) { BODY; ++idx;
  //   __kmpc_dispatch_fini_(4|8)[u](); // For ordered loops only.
  //   } // inner loop
  // }
  //
  // OpenMP [2.7.1, Loop Construct, Description, table 2-1]
  // When schedule(static, chunk_size) is specified, iterations are divided into
  // chunks of size chunk_size, and the chunks are assigned to the threads in
  // the team in a round-robin fashion in the order of the thread number.
  //
  // while(UB = min(UB, GlobalUB), idx = LB, idx < UB) {
  //   while (idx <= UB) { BODY; ++idx; } // inner loop
  //   LB = LB + ST;
  //   UB = UB + ST;
  // }
  //

  const Expr *IVExpr = S.getIterationVariable();
  const unsigned IVSize = getContext().getTypeSize(IVExpr->getType());
  const bool IVSigned = IVExpr->getType()->hasSignedIntegerRepresentation();

  if (DynamicOrOrdered) {
    auto DispatchBounds = CGDispatchBounds(*this, S, LoopArgs.LB, LoopArgs.UB);
    llvm::Value *LBVal = DispatchBounds.first;
    llvm::Value *UBVal = DispatchBounds.second;
    CGOpenMPRuntime::DispatchRTInput DipatchRTInputValues = {LBVal, UBVal,
                                                             LoopArgs.Chunk};
    RT.emitForDispatchInit(*this, S.getLocStart(), ScheduleKind, IVSize,
                           IVSigned, Ordered, DipatchRTInputValues);
  } else {
    RT.emitForStaticInit(*this, S.getLocStart(), ScheduleKind, IVSize, IVSigned,
                         Ordered, LoopArgs.IL, LoopArgs.LB, LoopArgs.UB,
                         LoopArgs.ST, LoopArgs.Chunk);
  }

  auto &&CodeGenOrdered = [Ordered](CodeGenFunction &CGF, SourceLocation Loc,
                                    const unsigned IVSize,
                                    const bool IVSigned) {
    if (Ordered) {
      CGF.CGM.getOpenMPRuntime().emitForOrderedIterationEnd(CGF, Loc, IVSize,
                                                            IVSigned);
    }
  };

  OMPLoopArguments OuterLoopArgs(LoopArgs.LB, LoopArgs.UB, LoopArgs.ST,
                                 LoopArgs.IL, LoopArgs.Chunk, LoopArgs.EUB);
  OuterLoopArgs.IncExpr = S.getInc();
  OuterLoopArgs.Init = S.getInit();
  OuterLoopArgs.Cond = S.getCond();
  OuterLoopArgs.NextLB = S.getNextLowerBound();
  OuterLoopArgs.NextUB = S.getNextUpperBound();
  EmitOMPOuterLoop(DynamicOrOrdered, IsMonotonic, S, LoopScope, OuterLoopArgs,
                   emitOMPLoopBodyWithStopPoint, CodeGenOrdered);
}

static void emitEmptyOrdered(CodeGenFunction &, SourceLocation Loc,
                             const unsigned IVSize, const bool IVSigned) {}

void CodeGenFunction::EmitOMPDistributeOuterLoop(
    OpenMPDistScheduleClauseKind ScheduleKind, const OMPLoopDirective &S,
    OMPPrivateScope &LoopScope, const OMPLoopArguments &LoopArgs,
    const CodeGenLoopTy &CodeGenLoopContent) {

  auto &RT = CGM.getOpenMPRuntime();

  // Emit outer loop.
  // Same behavior as a OMPForOuterLoop, except that schedule cannot be
  // dynamic
  //

  const Expr *IVExpr = S.getIterationVariable();
  const unsigned IVSize = getContext().getTypeSize(IVExpr->getType());
  const bool IVSigned = IVExpr->getType()->hasSignedIntegerRepresentation();

  RT.emitDistributeStaticInit(*this, S.getLocStart(), ScheduleKind, IVSize,
                              IVSigned, /* Ordered = */ false, LoopArgs.IL,
                              LoopArgs.LB, LoopArgs.UB, LoopArgs.ST,
                              LoopArgs.Chunk);

  // for combined 'distribute' and 'for' the increment expression of distribute
  // is store in DistInc. For 'distribute' alone, it is in Inc.
  Expr *IncExpr;
  if (isOpenMPLoopBoundSharingDirective(S.getDirectiveKind()))
    IncExpr = S.getDistInc();
  else
    IncExpr = S.getInc();

  // this routine is shared by 'omp distribute parallel for' and
  // 'omp distribute': select the right EUB expression depending on the
  // directive
  OMPLoopArguments OuterLoopArgs;
  OuterLoopArgs.LB = LoopArgs.LB;
  OuterLoopArgs.UB = LoopArgs.UB;
  OuterLoopArgs.ST = LoopArgs.ST;
  OuterLoopArgs.IL = LoopArgs.IL;
  OuterLoopArgs.Chunk = LoopArgs.Chunk;
  OuterLoopArgs.EUB = isOpenMPLoopBoundSharingDirective(S.getDirectiveKind())
                          ? S.getCombinedEnsureUpperBound()
                          : S.getEnsureUpperBound();
  OuterLoopArgs.IncExpr = IncExpr;
  OuterLoopArgs.Init = isOpenMPLoopBoundSharingDirective(S.getDirectiveKind())
                           ? S.getCombinedInit()
                           : S.getInit();
  OuterLoopArgs.Cond = isOpenMPLoopBoundSharingDirective(S.getDirectiveKind())
                           ? S.getCombinedCond()
                           : S.getCond();
  OuterLoopArgs.NextLB = isOpenMPLoopBoundSharingDirective(S.getDirectiveKind())
                             ? S.getCombinedNextLowerBound()
                             : S.getNextLowerBound();
  OuterLoopArgs.NextUB = isOpenMPLoopBoundSharingDirective(S.getDirectiveKind())
                             ? S.getCombinedNextUpperBound()
                             : S.getNextUpperBound();

  EmitOMPOuterLoop(/* DynamicOrOrdered = */ false, /* IsMonotonic = */ false, S,
                   LoopScope, OuterLoopArgs, CodeGenLoopContent,
                   emitEmptyOrdered);
}

/// Emit a helper variable and return corresponding lvalue.
static LValue EmitOMPHelperVar(CodeGenFunction &CGF,
                               const DeclRefExpr *Helper) {
  auto VDecl = cast<VarDecl>(Helper->getDecl());
  CGF.EmitVarDecl(*VDecl);
  return CGF.EmitLValue(Helper);
}

static std::pair<LValue, LValue>
emitDistributeParallelForInnerBounds(CodeGenFunction &CGF,
                                     const OMPExecutableDirective &S) {
  const OMPLoopDirective &LS = cast<OMPLoopDirective>(S);
  LValue LB =
      EmitOMPHelperVar(CGF, cast<DeclRefExpr>(LS.getLowerBoundVariable()));
  LValue UB =
      EmitOMPHelperVar(CGF, cast<DeclRefExpr>(LS.getUpperBoundVariable()));

  // When composing 'distribute' with 'for' (e.g. as in 'distribute
  // parallel for') we need to use the 'distribute'
  // chunk lower and upper bounds rather than the whole loop iteration
  // space. These are parameters to the outlined function for 'parallel'
  // and we copy the bounds of the previous schedule into the
  // the current ones.
  LValue PrevLB = CGF.EmitLValue(LS.getPrevLowerBoundVariable());
  LValue PrevUB = CGF.EmitLValue(LS.getPrevUpperBoundVariable());
  llvm::Value *PrevLBVal = CGF.EmitLoadOfScalar(PrevLB, SourceLocation());
  PrevLBVal = CGF.EmitScalarConversion(
      PrevLBVal, LS.getPrevLowerBoundVariable()->getType(),
      LS.getIterationVariable()->getType(), SourceLocation());
  llvm::Value *PrevUBVal = CGF.EmitLoadOfScalar(PrevUB, SourceLocation());
  PrevUBVal = CGF.EmitScalarConversion(
      PrevUBVal, LS.getPrevUpperBoundVariable()->getType(),
      LS.getIterationVariable()->getType(), SourceLocation());

  CGF.EmitStoreOfScalar(PrevLBVal, LB);
  CGF.EmitStoreOfScalar(PrevUBVal, UB);

  return {LB, UB};
}

/// if the 'for' loop has a dispatch schedule (e.g. dynamic, guided) then
/// we need to use the LB and UB expressions generated by the worksharing
/// code generation support, whereas in non combined situations we would
/// just emit 0 and the LastIteration expression
/// This function is necessary due to the difference of the LB and UB
/// types for the RT emission routines for 'for_static_init' and
/// 'for_dispatch_init'
static std::pair<llvm::Value *, llvm::Value *>
emitDistributeParallelForDispatchBounds(CodeGenFunction &CGF,
                                        const OMPExecutableDirective &S,
                                        Address LB, Address UB) {
  const OMPLoopDirective &LS = cast<OMPLoopDirective>(S);
  const Expr *IVExpr = LS.getIterationVariable();
  // when implementing a dynamic schedule for a 'for' combined with a
  // 'distribute' (e.g. 'distribute parallel for'), the 'for' loop
  // is not normalized as each team only executes its own assigned
  // distribute chunk
  QualType IteratorTy = IVExpr->getType();
  llvm::Value *LBVal = CGF.EmitLoadOfScalar(LB, /*Volatile=*/false, IteratorTy,
                                            SourceLocation());
  llvm::Value *UBVal = CGF.EmitLoadOfScalar(UB, /*Volatile=*/false, IteratorTy,
                                            SourceLocation());
  return {LBVal, UBVal};
}

static void emitDistributeParallelForDistributeInnerBoundParams(
    CodeGenFunction &CGF, const OMPExecutableDirective &S,
    llvm::SmallVectorImpl<llvm::Value *> &CapturedVars) {
  const auto &Dir = cast<OMPLoopDirective>(S);
  LValue LB =
      CGF.EmitLValue(cast<DeclRefExpr>(Dir.getCombinedLowerBoundVariable()));
  auto LBCast = CGF.Builder.CreateIntCast(
      CGF.Builder.CreateLoad(LB.getAddress()), CGF.SizeTy, /*isSigned=*/false);
  CapturedVars.push_back(LBCast);
  LValue UB =
      CGF.EmitLValue(cast<DeclRefExpr>(Dir.getCombinedUpperBoundVariable()));

  auto UBCast = CGF.Builder.CreateIntCast(
      CGF.Builder.CreateLoad(UB.getAddress()), CGF.SizeTy, /*isSigned=*/false);
  CapturedVars.push_back(UBCast);
}

static void
emitInnerParallelForWhenCombined(CodeGenFunction &CGF,
                                 const OMPLoopDirective &S,
                                 CodeGenFunction::JumpDest LoopExit) {
  auto &&CGInlinedWorksharingLoop = [&S](CodeGenFunction &CGF,
                                         PrePostActionTy &) {
    CGF.EmitOMPWorksharingLoop(S, S.getPrevEnsureUpperBound(),
                               emitDistributeParallelForInnerBounds,
                               emitDistributeParallelForDispatchBounds);
  };

  emitCommonOMPParallelDirective(
      CGF, S, OMPD_for, CGInlinedWorksharingLoop,
      emitDistributeParallelForDistributeInnerBoundParams);
}

void CodeGenFunction::EmitOMPDistributeParallelForDirective(
    const OMPDistributeParallelForDirective &S) {
  auto &&CodeGen = [&S](CodeGenFunction &CGF, PrePostActionTy &) {
    CGF.EmitOMPDistributeLoop(S, emitInnerParallelForWhenCombined,
                              S.getDistInc());
  };
  OMPLexicalScope Scope(*this, S, /*AsInlined=*/true);
  OMPCancelStackRAII CancelRegion(*this, OMPD_distribute_parallel_for,
                                  /*HasCancel=*/false);
  CGM.getOpenMPRuntime().emitInlinedDirective(*this, OMPD_distribute, CodeGen,
                                              /*HasCancel=*/false);
}

void CodeGenFunction::EmitOMPDistributeParallelForSimdDirective(
    const OMPDistributeParallelForSimdDirective &S) {
  OMPLexicalScope Scope(*this, S, /*AsInlined=*/true);
  CGM.getOpenMPRuntime().emitInlinedDirective(
      *this, OMPD_distribute_parallel_for_simd,
      [&S](CodeGenFunction &CGF, PrePostActionTy &) {
        OMPLoopScope PreInitScope(CGF, S);
        CGF.EmitStmt(
            cast<CapturedStmt>(S.getAssociatedStmt())->getCapturedStmt());
      });
}

void CodeGenFunction::EmitOMPDistributeSimdDirective(
    const OMPDistributeSimdDirective &S) {
  OMPLexicalScope Scope(*this, S, /*AsInlined=*/true);
  CGM.getOpenMPRuntime().emitInlinedDirective(
      *this, OMPD_distribute_simd,
      [&S](CodeGenFunction &CGF, PrePostActionTy &) {
        OMPLoopScope PreInitScope(CGF, S);
        CGF.EmitStmt(
            cast<CapturedStmt>(S.getAssociatedStmt())->getCapturedStmt());
      });
}

void CodeGenFunction::EmitOMPTargetParallelForSimdDirective(
    const OMPTargetParallelForSimdDirective &S) {
  OMPLexicalScope Scope(*this, S, /*AsInlined=*/true);
  CGM.getOpenMPRuntime().emitInlinedDirective(
      *this, OMPD_target_parallel_for_simd,
      [&S](CodeGenFunction &CGF, PrePostActionTy &) {
        OMPLoopScope PreInitScope(CGF, S);
        CGF.EmitStmt(
            cast<CapturedStmt>(S.getAssociatedStmt())->getCapturedStmt());
      });
}

void CodeGenFunction::EmitOMPTargetSimdDirective(
    const OMPTargetSimdDirective &S) {
  OMPLexicalScope Scope(*this, S, /*AsInlined=*/true);
  CGM.getOpenMPRuntime().emitInlinedDirective(
      *this, OMPD_target_simd, [&S](CodeGenFunction &CGF, PrePostActionTy &) {
        OMPLoopScope PreInitScope(CGF, S);
        CGF.EmitStmt(
            cast<CapturedStmt>(S.getAssociatedStmt())->getCapturedStmt());
      });
}

void CodeGenFunction::EmitOMPTeamsDistributeDirective(
    const OMPTeamsDistributeDirective &S) {
  OMPLexicalScope Scope(*this, S, /*AsInlined=*/true);
  CGM.getOpenMPRuntime().emitInlinedDirective(
      *this, OMPD_teams_distribute,
      [&S](CodeGenFunction &CGF, PrePostActionTy &) {
        OMPLoopScope PreInitScope(CGF, S);
        CGF.EmitStmt(
            cast<CapturedStmt>(S.getAssociatedStmt())->getCapturedStmt());
      });
}

void CodeGenFunction::EmitOMPTeamsDistributeSimdDirective(
    const OMPTeamsDistributeSimdDirective &S) {
  OMPLexicalScope Scope(*this, S, /*AsInlined=*/true);
  CGM.getOpenMPRuntime().emitInlinedDirective(
      *this, OMPD_teams_distribute_simd,
      [&S](CodeGenFunction &CGF, PrePostActionTy &) {
        OMPLoopScope PreInitScope(CGF, S);
        CGF.EmitStmt(
            cast<CapturedStmt>(S.getAssociatedStmt())->getCapturedStmt());
      });
}

void CodeGenFunction::EmitOMPTeamsDistributeParallelForSimdDirective(
    const OMPTeamsDistributeParallelForSimdDirective &S) {
  OMPLexicalScope Scope(*this, S, /*AsInlined=*/true);
  CGM.getOpenMPRuntime().emitInlinedDirective(
      *this, OMPD_teams_distribute_parallel_for_simd,
      [&S](CodeGenFunction &CGF, PrePostActionTy &) {
        OMPLoopScope PreInitScope(CGF, S);
        CGF.EmitStmt(
            cast<CapturedStmt>(S.getAssociatedStmt())->getCapturedStmt());
      });
}

void CodeGenFunction::EmitOMPTeamsDistributeParallelForDirective(
    const OMPTeamsDistributeParallelForDirective &S) {
  OMPLexicalScope Scope(*this, S, /*AsInlined=*/true);
  CGM.getOpenMPRuntime().emitInlinedDirective(
      *this, OMPD_teams_distribute_parallel_for,
      [&S](CodeGenFunction &CGF, PrePostActionTy &) {
        OMPLoopScope PreInitScope(CGF, S);
        CGF.EmitStmt(
            cast<CapturedStmt>(S.getAssociatedStmt())->getCapturedStmt());
      });
}

void CodeGenFunction::EmitOMPTargetTeamsDistributeDirective(
    const OMPTargetTeamsDistributeDirective &S) {
  CGM.getOpenMPRuntime().emitInlinedDirective(
      *this, OMPD_target_teams_distribute,
      [&S](CodeGenFunction &CGF, PrePostActionTy &) {
        CGF.EmitStmt(
            cast<CapturedStmt>(S.getAssociatedStmt())->getCapturedStmt());
      });
}

void CodeGenFunction::EmitOMPTargetTeamsDistributeParallelForDirective(
    const OMPTargetTeamsDistributeParallelForDirective &S) {
  CGM.getOpenMPRuntime().emitInlinedDirective(
      *this, OMPD_target_teams_distribute_parallel_for,
      [&S](CodeGenFunction &CGF, PrePostActionTy &) {
        CGF.EmitStmt(
            cast<CapturedStmt>(S.getAssociatedStmt())->getCapturedStmt());
      });
}

void CodeGenFunction::EmitOMPTargetTeamsDistributeParallelForSimdDirective(
    const OMPTargetTeamsDistributeParallelForSimdDirective &S) {
  CGM.getOpenMPRuntime().emitInlinedDirective(
      *this, OMPD_target_teams_distribute_parallel_for_simd,
      [&S](CodeGenFunction &CGF, PrePostActionTy &) {
        CGF.EmitStmt(
            cast<CapturedStmt>(S.getAssociatedStmt())->getCapturedStmt());
      });
}

void CodeGenFunction::EmitOMPTargetTeamsDistributeSimdDirective(
    const OMPTargetTeamsDistributeSimdDirective &S) {
  CGM.getOpenMPRuntime().emitInlinedDirective(
      *this, OMPD_target_teams_distribute_simd,
      [&S](CodeGenFunction &CGF, PrePostActionTy &) {
        CGF.EmitStmt(
            cast<CapturedStmt>(S.getAssociatedStmt())->getCapturedStmt());
      });
}

namespace {
  struct ScheduleKindModifiersTy {
    OpenMPScheduleClauseKind Kind;
    OpenMPScheduleClauseModifier M1;
    OpenMPScheduleClauseModifier M2;
    ScheduleKindModifiersTy(OpenMPScheduleClauseKind Kind,
                            OpenMPScheduleClauseModifier M1,
                            OpenMPScheduleClauseModifier M2)
        : Kind(Kind), M1(M1), M2(M2) {}
  };
} // namespace

bool CodeGenFunction::EmitOMPWorksharingLoop(
    const OMPLoopDirective &S, Expr *EUB,
    const CodeGenLoopBoundsTy &CodeGenLoopBounds,
    const CodeGenDispatchBoundsTy &CGDispatchBounds) {
  // Emit the loop iteration variable.
  auto IVExpr = cast<DeclRefExpr>(S.getIterationVariable());
  auto IVDecl = cast<VarDecl>(IVExpr->getDecl());
  EmitVarDecl(*IVDecl);

  // Emit the iterations count variable.
  // If it is not a variable, Sema decided to calculate iterations count on each
  // iteration (e.g., it is foldable into a constant).
  if (auto LIExpr = dyn_cast<DeclRefExpr>(S.getLastIteration())) {
    EmitVarDecl(*cast<VarDecl>(LIExpr->getDecl()));
    // Emit calculation of the iterations count.
    EmitIgnoredExpr(S.getCalcLastIteration());
  }

  auto &RT = CGM.getOpenMPRuntime();

  bool HasLastprivateClause;
  // Check pre-condition.
  {
    OMPLoopScope PreInitScope(*this, S);
    // Skip the entire loop if we don't meet the precondition.
    // If the condition constant folds and can be elided, avoid emitting the
    // whole loop.
    bool CondConstant;
    llvm::BasicBlock *ContBlock = nullptr;
    if (ConstantFoldsToSimpleInteger(S.getPreCond(), CondConstant)) {
      if (!CondConstant)
        return false;
    } else {
      auto *ThenBlock = createBasicBlock("omp.precond.then");
      ContBlock = createBasicBlock("omp.precond.end");
      emitPreCond(*this, S, S.getPreCond(), ThenBlock, ContBlock,
                  getProfileCount(&S));
      EmitBlock(ThenBlock);
      incrementProfileCounter(&S);
    }

    bool Ordered = false;
    if (auto *OrderedClause = S.getSingleClause<OMPOrderedClause>()) {
      if (OrderedClause->getNumForLoops())
        RT.emitDoacrossInit(*this, S);
      else
        Ordered = true;
    }

    llvm::DenseSet<const Expr *> EmittedFinals;
    emitAlignedClause(*this, S);
    bool HasLinears = EmitOMPLinearClauseInit(S);
    // Emit helper vars inits.

    std::pair<LValue, LValue> Bounds = CodeGenLoopBounds(*this, S);
    LValue LB = Bounds.first;
    LValue UB = Bounds.second;
    LValue ST =
        EmitOMPHelperVar(*this, cast<DeclRefExpr>(S.getStrideVariable()));
    LValue IL =
        EmitOMPHelperVar(*this, cast<DeclRefExpr>(S.getIsLastIterVariable()));

    // Emit 'then' code.
    {
      OMPPrivateScope LoopScope(*this);
      if (EmitOMPFirstprivateClause(S, LoopScope) || HasLinears) {
        // Emit implicit barrier to synchronize threads and avoid data races on
        // initialization of firstprivate variables and post-update of
        // lastprivate variables.
        CGM.getOpenMPRuntime().emitBarrierCall(
            *this, S.getLocStart(), OMPD_unknown, /*EmitChecks=*/false,
            /*ForceSimpleCall=*/true);
      }
      EmitOMPPrivateClause(S, LoopScope);
      HasLastprivateClause = EmitOMPLastprivateClauseInit(S, LoopScope);
      EmitOMPReductionClauseInit(S, LoopScope);
      EmitOMPPrivateLoopCounters(S, LoopScope);
      EmitOMPLinearClause(S, LoopScope);
      (void)LoopScope.Privatize();

      // Detect the loop schedule kind and chunk.
      llvm::Value *Chunk = nullptr;
      OpenMPScheduleTy ScheduleKind;
      if (auto *C = S.getSingleClause<OMPScheduleClause>()) {
        ScheduleKind.Schedule = C->getScheduleKind();
        ScheduleKind.M1 = C->getFirstScheduleModifier();
        ScheduleKind.M2 = C->getSecondScheduleModifier();
        if (const auto *Ch = C->getChunkSize()) {
          Chunk = EmitScalarExpr(Ch);
          Chunk = EmitScalarConversion(Chunk, Ch->getType(),
                                       S.getIterationVariable()->getType(),
                                       S.getLocStart());
        }
      }
      const unsigned IVSize = getContext().getTypeSize(IVExpr->getType());
      const bool IVSigned = IVExpr->getType()->hasSignedIntegerRepresentation();
      // OpenMP 4.5, 2.7.1 Loop Construct, Description.
      // If the static schedule kind is specified or if the ordered clause is
      // specified, and if no monotonic modifier is specified, the effect will
      // be as if the monotonic modifier was specified.
      if (RT.isStaticNonchunked(ScheduleKind.Schedule,
                                /* Chunked */ Chunk != nullptr) &&
          !Ordered) {
        if (isOpenMPSimdDirective(S.getDirectiveKind()))
          EmitOMPSimdInit(S, /*IsMonotonic=*/true);
        // OpenMP [2.7.1, Loop Construct, Description, table 2-1]
        // When no chunk_size is specified, the iteration space is divided into
        // chunks that are approximately equal in size, and at most one chunk is
        // distributed to each thread. Note that the size of the chunks is
        // unspecified in this case.
        RT.emitForStaticInit(*this, S.getLocStart(), ScheduleKind,
                             IVSize, IVSigned, Ordered,
                             IL.getAddress(), LB.getAddress(),
                             UB.getAddress(), ST.getAddress());
        auto LoopExit =
            getJumpDestInCurrentScope(createBasicBlock("omp.loop.exit"));
        // UB = min(UB, GlobalUB);
        EmitIgnoredExpr(S.getEnsureUpperBound());
        // IV = LB;
        EmitIgnoredExpr(S.getInit());
        // while (idx <= UB) { BODY; ++idx; }
        EmitOMPInnerLoop(S, LoopScope.requiresCleanups(), S.getCond(),
                         S.getInc(),
                         [&S, LoopExit](CodeGenFunction &CGF) {
                           CGF.EmitOMPLoopBody(S, LoopExit);
                           CGF.EmitStopPoint(&S);
                         },
                         [](CodeGenFunction &) {});
        EmitBlock(LoopExit.getBlock());
        // Tell the runtime we are done.
        auto &&CodeGen = [&S](CodeGenFunction &CGF) {
          CGF.CGM.getOpenMPRuntime().emitForStaticFinish(CGF, S.getLocEnd());
        };
        OMPCancelStack.emitExit(*this, S.getDirectiveKind(), CodeGen);
      } else {
        const bool IsMonotonic =
            Ordered || ScheduleKind.Schedule == OMPC_SCHEDULE_static ||
            ScheduleKind.Schedule == OMPC_SCHEDULE_unknown ||
            ScheduleKind.M1 == OMPC_SCHEDULE_MODIFIER_monotonic ||
            ScheduleKind.M2 == OMPC_SCHEDULE_MODIFIER_monotonic;
        // Emit the outer loop, which requests its work chunk [LB..UB] from
        // runtime and runs the inner loop to process it.
        const OMPLoopArguments LoopArguments(LB.getAddress(), UB.getAddress(),
                                             ST.getAddress(), IL.getAddress(),
                                             Chunk, EUB);
        EmitOMPForOuterLoop(ScheduleKind, IsMonotonic, S, LoopScope, Ordered,
                            LoopArguments, CGDispatchBounds);
      }
      if (isOpenMPSimdDirective(S.getDirectiveKind())) {
        EmitOMPSimdFinal(S,
                         [&](CodeGenFunction &CGF) -> llvm::Value * {
                           return CGF.Builder.CreateIsNotNull(
                               CGF.EmitLoadOfScalar(IL, S.getLocStart()));
                         });
      }
      EmitOMPReductionClauseFinal(
          S, /*ReductionKind=*/isOpenMPSimdDirective(S.getDirectiveKind())
                 ? /*Parallel and Simd*/ OMPD_parallel_for_simd
                 : /*Parallel only*/ OMPD_parallel);
      // Emit post-update of the reduction variables if IsLastIter != 0.
      emitPostUpdateForReductionClause(
          *this, S, [&](CodeGenFunction &CGF) -> llvm::Value * {
            return CGF.Builder.CreateIsNotNull(
                CGF.EmitLoadOfScalar(IL, S.getLocStart()));
          });
      // Emit final copy of the lastprivate variables if IsLastIter != 0.
      if (HasLastprivateClause)
        EmitOMPLastprivateClauseFinal(
            S, isOpenMPSimdDirective(S.getDirectiveKind()),
            Builder.CreateIsNotNull(EmitLoadOfScalar(IL, S.getLocStart())));
    }
    EmitOMPLinearClauseFinal(S, [&](CodeGenFunction &CGF) -> llvm::Value * {
      return CGF.Builder.CreateIsNotNull(
          CGF.EmitLoadOfScalar(IL, S.getLocStart()));
    });
    // We're now done with the loop, so jump to the continuation block.
    if (ContBlock) {
      EmitBranch(ContBlock);
      EmitBlock(ContBlock, true);
    }
  }
  return HasLastprivateClause;
}

/// The following two functions generate expressions for the loop lower
/// and upper bounds in case of static and dynamic (dispatch) schedule
/// of the associated 'for' or 'distribute' loop.
static std::pair<LValue, LValue>
emitForLoopBounds(CodeGenFunction &CGF, const OMPExecutableDirective &S) {
  const OMPLoopDirective &LS = cast<OMPLoopDirective>(S);
  LValue LB =
      EmitOMPHelperVar(CGF, cast<DeclRefExpr>(LS.getLowerBoundVariable()));
  LValue UB =
      EmitOMPHelperVar(CGF, cast<DeclRefExpr>(LS.getUpperBoundVariable()));
  return {LB, UB};
}

/// When dealing with dispatch schedules (e.g. dynamic, guided) we do not
/// consider the lower and upper bound expressions generated by the
/// worksharing loop support, but we use 0 and the iteration space size as
/// constants
static std::pair<llvm::Value *, llvm::Value *>
emitDispatchForLoopBounds(CodeGenFunction &CGF, const OMPExecutableDirective &S,
                          Address LB, Address UB) {
  const OMPLoopDirective &LS = cast<OMPLoopDirective>(S);
  const Expr *IVExpr = LS.getIterationVariable();
  const unsigned IVSize = CGF.getContext().getTypeSize(IVExpr->getType());
  llvm::Value *LBVal = CGF.Builder.getIntN(IVSize, 0);
  llvm::Value *UBVal = CGF.EmitScalarExpr(LS.getLastIteration());
  return {LBVal, UBVal};
}

void CodeGenFunction::EmitOMPForDirective(const OMPForDirective &S) {
  bool HasLastprivates = false;
  auto &&CodeGen = [&S, &HasLastprivates](CodeGenFunction &CGF,
                                          PrePostActionTy &) {
    OMPCancelStackRAII CancelRegion(CGF, OMPD_for, S.hasCancel());
    HasLastprivates = CGF.EmitOMPWorksharingLoop(S, S.getEnsureUpperBound(),
                                                 emitForLoopBounds,
                                                 emitDispatchForLoopBounds);
  };
  {
    OMPLexicalScope Scope(*this, S, /*AsInlined=*/true);
    CGM.getOpenMPRuntime().emitInlinedDirective(*this, OMPD_for, CodeGen,
                                                S.hasCancel());
  }

  // Emit an implicit barrier at the end.
  if (!S.getSingleClause<OMPNowaitClause>() || HasLastprivates) {
    CGM.getOpenMPRuntime().emitBarrierCall(*this, S.getLocStart(), OMPD_for);
  }
}

void CodeGenFunction::EmitOMPForSimdDirective(const OMPForSimdDirective &S) {
  bool HasLastprivates = false;
  auto &&CodeGen = [&S, &HasLastprivates](CodeGenFunction &CGF,
                                          PrePostActionTy &) {
    HasLastprivates = CGF.EmitOMPWorksharingLoop(S, S.getEnsureUpperBound(),
                                                 emitForLoopBounds,
                                                 emitDispatchForLoopBounds);
  };
  {
    OMPLexicalScope Scope(*this, S, /*AsInlined=*/true);
    CGM.getOpenMPRuntime().emitInlinedDirective(*this, OMPD_simd, CodeGen);
  }

  // Emit an implicit barrier at the end.
  if (!S.getSingleClause<OMPNowaitClause>() || HasLastprivates) {
    CGM.getOpenMPRuntime().emitBarrierCall(*this, S.getLocStart(), OMPD_for);
  }
}

static LValue createSectionLVal(CodeGenFunction &CGF, QualType Ty,
                                const Twine &Name,
                                llvm::Value *Init = nullptr) {
  auto LVal = CGF.MakeAddrLValue(CGF.CreateMemTemp(Ty, Name), Ty);
  if (Init)
    CGF.EmitStoreThroughLValue(RValue::get(Init), LVal, /*isInit*/ true);
  return LVal;
}

void CodeGenFunction::EmitSections(const OMPExecutableDirective &S) {
  auto *Stmt = cast<CapturedStmt>(S.getAssociatedStmt())->getCapturedStmt();
  auto *CS = dyn_cast<CompoundStmt>(Stmt);
  bool HasLastprivates = false;
  auto &&CodeGen = [&S, Stmt, CS, &HasLastprivates](CodeGenFunction &CGF,
                                                    PrePostActionTy &) {
    auto &C = CGF.CGM.getContext();
    auto KmpInt32Ty = C.getIntTypeForBitwidth(/*DestWidth=*/32, /*Signed=*/1);
    // Emit helper vars inits.
    LValue LB = createSectionLVal(CGF, KmpInt32Ty, ".omp.sections.lb.",
                                  CGF.Builder.getInt32(0));
    auto *GlobalUBVal = CS != nullptr ? CGF.Builder.getInt32(CS->size() - 1)
                                      : CGF.Builder.getInt32(0);
    LValue UB =
        createSectionLVal(CGF, KmpInt32Ty, ".omp.sections.ub.", GlobalUBVal);
    LValue ST = createSectionLVal(CGF, KmpInt32Ty, ".omp.sections.st.",
                                  CGF.Builder.getInt32(1));
    LValue IL = createSectionLVal(CGF, KmpInt32Ty, ".omp.sections.il.",
                                  CGF.Builder.getInt32(0));
    // Loop counter.
    LValue IV = createSectionLVal(CGF, KmpInt32Ty, ".omp.sections.iv.");
    OpaqueValueExpr IVRefExpr(S.getLocStart(), KmpInt32Ty, VK_LValue);
    CodeGenFunction::OpaqueValueMapping OpaqueIV(CGF, &IVRefExpr, IV);
    OpaqueValueExpr UBRefExpr(S.getLocStart(), KmpInt32Ty, VK_LValue);
    CodeGenFunction::OpaqueValueMapping OpaqueUB(CGF, &UBRefExpr, UB);
    // Generate condition for loop.
    BinaryOperator Cond(&IVRefExpr, &UBRefExpr, BO_LE, C.BoolTy, VK_RValue,
                        OK_Ordinary, S.getLocStart(), FPOptions());
    // Increment for loop counter.
    UnaryOperator Inc(&IVRefExpr, UO_PreInc, KmpInt32Ty, VK_RValue, OK_Ordinary,
                      S.getLocStart());
    auto BodyGen = [Stmt, CS, &S, &IV](CodeGenFunction &CGF) {
      // Iterate through all sections and emit a switch construct:
      // switch (IV) {
      //   case 0:
      //     <SectionStmt[0]>;
      //     break;
      // ...
      //   case <NumSection> - 1:
      //     <SectionStmt[<NumSection> - 1]>;
      //     break;
      // }
      // .omp.sections.exit:
      auto *ExitBB = CGF.createBasicBlock(".omp.sections.exit");
      auto *SwitchStmt = CGF.Builder.CreateSwitch(
          CGF.EmitLoadOfLValue(IV, S.getLocStart()).getScalarVal(), ExitBB,
          CS == nullptr ? 1 : CS->size());
      if (CS) {
        unsigned CaseNumber = 0;
        for (auto *SubStmt : CS->children()) {
          auto CaseBB = CGF.createBasicBlock(".omp.sections.case");
          CGF.EmitBlock(CaseBB);
          SwitchStmt->addCase(CGF.Builder.getInt32(CaseNumber), CaseBB);
          CGF.EmitStmt(SubStmt);
          CGF.EmitBranch(ExitBB);
          ++CaseNumber;
        }
      } else {
        auto CaseBB = CGF.createBasicBlock(".omp.sections.case");
        CGF.EmitBlock(CaseBB);
        SwitchStmt->addCase(CGF.Builder.getInt32(0), CaseBB);
        CGF.EmitStmt(Stmt);
        CGF.EmitBranch(ExitBB);
      }
      CGF.EmitBlock(ExitBB, /*IsFinished=*/true);
    };

    CodeGenFunction::OMPPrivateScope LoopScope(CGF);
    if (CGF.EmitOMPFirstprivateClause(S, LoopScope)) {
      // Emit implicit barrier to synchronize threads and avoid data races on
      // initialization of firstprivate variables and post-update of lastprivate
      // variables.
      CGF.CGM.getOpenMPRuntime().emitBarrierCall(
          CGF, S.getLocStart(), OMPD_unknown, /*EmitChecks=*/false,
          /*ForceSimpleCall=*/true);
    }
    CGF.EmitOMPPrivateClause(S, LoopScope);
    HasLastprivates = CGF.EmitOMPLastprivateClauseInit(S, LoopScope);
    CGF.EmitOMPReductionClauseInit(S, LoopScope);
    (void)LoopScope.Privatize();

    // Emit static non-chunked loop.
    OpenMPScheduleTy ScheduleKind;
    ScheduleKind.Schedule = OMPC_SCHEDULE_static;
    CGF.CGM.getOpenMPRuntime().emitForStaticInit(
        CGF, S.getLocStart(), ScheduleKind, /*IVSize=*/32,
        /*IVSigned=*/true, /*Ordered=*/false, IL.getAddress(), LB.getAddress(),
        UB.getAddress(), ST.getAddress());
    // UB = min(UB, GlobalUB);
    auto *UBVal = CGF.EmitLoadOfScalar(UB, S.getLocStart());
    auto *MinUBGlobalUB = CGF.Builder.CreateSelect(
        CGF.Builder.CreateICmpSLT(UBVal, GlobalUBVal), UBVal, GlobalUBVal);
    CGF.EmitStoreOfScalar(MinUBGlobalUB, UB);
    // IV = LB;
    CGF.EmitStoreOfScalar(CGF.EmitLoadOfScalar(LB, S.getLocStart()), IV);
    // while (idx <= UB) { BODY; ++idx; }
    CGF.EmitOMPInnerLoop(S, /*RequiresCleanup=*/false, &Cond, &Inc, BodyGen,
                         [](CodeGenFunction &) {});
    // Tell the runtime we are done.
    auto &&CodeGen = [&S](CodeGenFunction &CGF) {
      CGF.CGM.getOpenMPRuntime().emitForStaticFinish(CGF, S.getLocEnd());
    };
    CGF.OMPCancelStack.emitExit(CGF, S.getDirectiveKind(), CodeGen);
    CGF.EmitOMPReductionClauseFinal(S, /*ReductionKind=*/OMPD_parallel);
    // Emit post-update of the reduction variables if IsLastIter != 0.
    emitPostUpdateForReductionClause(
        CGF, S, [&](CodeGenFunction &CGF) -> llvm::Value * {
          return CGF.Builder.CreateIsNotNull(
              CGF.EmitLoadOfScalar(IL, S.getLocStart()));
        });

    // Emit final copy of the lastprivate variables if IsLastIter != 0.
    if (HasLastprivates)
      CGF.EmitOMPLastprivateClauseFinal(
          S, /*NoFinals=*/false,
          CGF.Builder.CreateIsNotNull(
              CGF.EmitLoadOfScalar(IL, S.getLocStart())));
  };

  bool HasCancel = false;
  if (auto *OSD = dyn_cast<OMPSectionsDirective>(&S))
    HasCancel = OSD->hasCancel();
  else if (auto *OPSD = dyn_cast<OMPParallelSectionsDirective>(&S))
    HasCancel = OPSD->hasCancel();
  OMPCancelStackRAII CancelRegion(*this, S.getDirectiveKind(), HasCancel);
  CGM.getOpenMPRuntime().emitInlinedDirective(*this, OMPD_sections, CodeGen,
                                              HasCancel);
  // Emit barrier for lastprivates only if 'sections' directive has 'nowait'
  // clause. Otherwise the barrier will be generated by the codegen for the
  // directive.
  if (HasLastprivates && S.getSingleClause<OMPNowaitClause>()) {
    // Emit implicit barrier to synchronize threads and avoid data races on
    // initialization of firstprivate variables.
    CGM.getOpenMPRuntime().emitBarrierCall(*this, S.getLocStart(),
                                           OMPD_unknown);
  }
}

void CodeGenFunction::EmitOMPSectionsDirective(const OMPSectionsDirective &S) {
  {
    OMPLexicalScope Scope(*this, S, /*AsInlined=*/true);
    EmitSections(S);
  }
  // Emit an implicit barrier at the end.
  if (!S.getSingleClause<OMPNowaitClause>()) {
    CGM.getOpenMPRuntime().emitBarrierCall(*this, S.getLocStart(),
                                           OMPD_sections);
  }
}

void CodeGenFunction::EmitOMPSectionDirective(const OMPSectionDirective &S) {
  auto &&CodeGen = [&S](CodeGenFunction &CGF, PrePostActionTy &) {
    CGF.EmitStmt(cast<CapturedStmt>(S.getAssociatedStmt())->getCapturedStmt());
  };
  OMPLexicalScope Scope(*this, S, /*AsInlined=*/true);
  CGM.getOpenMPRuntime().emitInlinedDirective(*this, OMPD_section, CodeGen,
                                              S.hasCancel());
}

void CodeGenFunction::EmitOMPSingleDirective(const OMPSingleDirective &S) {
  llvm::SmallVector<const Expr *, 8> CopyprivateVars;
  llvm::SmallVector<const Expr *, 8> DestExprs;
  llvm::SmallVector<const Expr *, 8> SrcExprs;
  llvm::SmallVector<const Expr *, 8> AssignmentOps;
  // Check if there are any 'copyprivate' clauses associated with this
  // 'single' construct.
  // Build a list of copyprivate variables along with helper expressions
  // (<source>, <destination>, <destination>=<source> expressions)
  for (const auto *C : S.getClausesOfKind<OMPCopyprivateClause>()) {
    CopyprivateVars.append(C->varlists().begin(), C->varlists().end());
    DestExprs.append(C->destination_exprs().begin(),
                     C->destination_exprs().end());
    SrcExprs.append(C->source_exprs().begin(), C->source_exprs().end());
    AssignmentOps.append(C->assignment_ops().begin(),
                         C->assignment_ops().end());
  }
  // Emit code for 'single' region along with 'copyprivate' clauses
  auto &&CodeGen = [&S](CodeGenFunction &CGF, PrePostActionTy &Action) {
    Action.Enter(CGF);
    OMPPrivateScope SingleScope(CGF);
    (void)CGF.EmitOMPFirstprivateClause(S, SingleScope);
    CGF.EmitOMPPrivateClause(S, SingleScope);
    (void)SingleScope.Privatize();
    CGF.EmitStmt(cast<CapturedStmt>(S.getAssociatedStmt())->getCapturedStmt());
  };
  {
    OMPLexicalScope Scope(*this, S, /*AsInlined=*/true);
    CGM.getOpenMPRuntime().emitSingleRegion(*this, CodeGen, S.getLocStart(),
                                            CopyprivateVars, DestExprs,
                                            SrcExprs, AssignmentOps);
  }
  // Emit an implicit barrier at the end (to avoid data race on firstprivate
  // init or if no 'nowait' clause was specified and no 'copyprivate' clause).
  if (!S.getSingleClause<OMPNowaitClause>() && CopyprivateVars.empty()) {
    CGM.getOpenMPRuntime().emitBarrierCall(
        *this, S.getLocStart(),
        S.getSingleClause<OMPNowaitClause>() ? OMPD_unknown : OMPD_single);
  }
}

void CodeGenFunction::EmitOMPMasterDirective(const OMPMasterDirective &S) {
  auto &&CodeGen = [&S](CodeGenFunction &CGF, PrePostActionTy &Action) {
    Action.Enter(CGF);
    CGF.EmitStmt(cast<CapturedStmt>(S.getAssociatedStmt())->getCapturedStmt());
  };
  OMPLexicalScope Scope(*this, S, /*AsInlined=*/true);
  CGM.getOpenMPRuntime().emitMasterRegion(*this, CodeGen, S.getLocStart());
}

void CodeGenFunction::EmitOMPCriticalDirective(const OMPCriticalDirective &S) {
  auto &&CodeGen = [&S](CodeGenFunction &CGF, PrePostActionTy &Action) {
    Action.Enter(CGF);
    CGF.EmitStmt(cast<CapturedStmt>(S.getAssociatedStmt())->getCapturedStmt());
  };
  Expr *Hint = nullptr;
  if (auto *HintClause = S.getSingleClause<OMPHintClause>())
    Hint = HintClause->getHint();
  OMPLexicalScope Scope(*this, S, /*AsInlined=*/true);
  CGM.getOpenMPRuntime().emitCriticalRegion(*this,
                                            S.getDirectiveName().getAsString(),
                                            CodeGen, S.getLocStart(), Hint);
}

void CodeGenFunction::EmitOMPParallelForDirective(
    const OMPParallelForDirective &S) {
  // Emit directive as a combined directive that consists of two implicit
  // directives: 'parallel' with 'for' directive.
  auto &&CodeGen = [&S](CodeGenFunction &CGF, PrePostActionTy &) {
    OMPCancelStackRAII CancelRegion(CGF, OMPD_parallel_for, S.hasCancel());
    CGF.EmitOMPWorksharingLoop(S, S.getEnsureUpperBound(), emitForLoopBounds,
                               emitDispatchForLoopBounds);
  };
  emitCommonOMPParallelDirective(*this, S, OMPD_for, CodeGen,
                                 emitEmptyBoundParameters);
}

void CodeGenFunction::EmitOMPParallelForSimdDirective(
    const OMPParallelForSimdDirective &S) {
  // Emit directive as a combined directive that consists of two implicit
  // directives: 'parallel' with 'for' directive.
  auto &&CodeGen = [&S](CodeGenFunction &CGF, PrePostActionTy &) {
    CGF.EmitOMPWorksharingLoop(S, S.getEnsureUpperBound(), emitForLoopBounds,
                               emitDispatchForLoopBounds);
  };
  emitCommonOMPParallelDirective(*this, S, OMPD_simd, CodeGen,
                                 emitEmptyBoundParameters);
}

void CodeGenFunction::EmitOMPParallelSectionsDirective(
    const OMPParallelSectionsDirective &S) {
  // Emit directive as a combined directive that consists of two implicit
  // directives: 'parallel' with 'sections' directive.
  auto &&CodeGen = [&S](CodeGenFunction &CGF, PrePostActionTy &) {
    CGF.EmitSections(S);
  };
  emitCommonOMPParallelDirective(*this, S, OMPD_sections, CodeGen,
                                 emitEmptyBoundParameters);
}

void CodeGenFunction::EmitOMPTaskBasedDirective(const OMPExecutableDirective &S,
                                                const RegionCodeGenTy &BodyGen,
                                                const TaskGenTy &TaskGen,
                                                OMPTaskDataTy &Data) {
  // Emit outlined function for task construct.
  auto CS = cast<CapturedStmt>(S.getAssociatedStmt());
  auto *I = CS->getCapturedDecl()->param_begin();
  auto *PartId = std::next(I);
  auto *TaskT = std::next(I, 4);
  // Check if the task is final
  if (const auto *Clause = S.getSingleClause<OMPFinalClause>()) {
    // If the condition constant folds and can be elided, try to avoid emitting
    // the condition and the dead arm of the if/else.
    auto *Cond = Clause->getCondition();
    bool CondConstant;
    if (ConstantFoldsToSimpleInteger(Cond, CondConstant))
      Data.Final.setInt(CondConstant);
    else
      Data.Final.setPointer(EvaluateExprAsBool(Cond));
  } else {
    // By default the task is not final.
    Data.Final.setInt(/*IntVal=*/false);
  }
  // Check if the task has 'priority' clause.
  if (const auto *Clause = S.getSingleClause<OMPPriorityClause>()) {
    auto *Prio = Clause->getPriority();
    Data.Priority.setInt(/*IntVal=*/true);
    Data.Priority.setPointer(EmitScalarConversion(
        EmitScalarExpr(Prio), Prio->getType(),
        getContext().getIntTypeForBitwidth(/*DestWidth=*/32, /*Signed=*/1),
        Prio->getExprLoc()));
  }
  // The first function argument for tasks is a thread id, the second one is a
  // part id (0 for tied tasks, >=0 for untied task).
  llvm::DenseSet<const VarDecl *> EmittedAsPrivate;
  // Get list of private variables.
  for (const auto *C : S.getClausesOfKind<OMPPrivateClause>()) {
    auto IRef = C->varlist_begin();
    for (auto *IInit : C->private_copies()) {
      auto *OrigVD = cast<VarDecl>(cast<DeclRefExpr>(*IRef)->getDecl());
      if (EmittedAsPrivate.insert(OrigVD->getCanonicalDecl()).second) {
        Data.PrivateVars.push_back(*IRef);
        Data.PrivateCopies.push_back(IInit);
      }
      ++IRef;
    }
  }
  EmittedAsPrivate.clear();
  // Get list of firstprivate variables.
  for (const auto *C : S.getClausesOfKind<OMPFirstprivateClause>()) {
    auto IRef = C->varlist_begin();
    auto IElemInitRef = C->inits().begin();
    for (auto *IInit : C->private_copies()) {
      auto *OrigVD = cast<VarDecl>(cast<DeclRefExpr>(*IRef)->getDecl());
      if (EmittedAsPrivate.insert(OrigVD->getCanonicalDecl()).second) {
        Data.FirstprivateVars.push_back(*IRef);
        Data.FirstprivateCopies.push_back(IInit);
        Data.FirstprivateInits.push_back(*IElemInitRef);
      }
      ++IRef;
      ++IElemInitRef;
    }
  }
  // Get list of lastprivate variables (for taskloops).
  llvm::DenseMap<const VarDecl *, const DeclRefExpr *> LastprivateDstsOrigs;
  for (const auto *C : S.getClausesOfKind<OMPLastprivateClause>()) {
    auto IRef = C->varlist_begin();
    auto ID = C->destination_exprs().begin();
    for (auto *IInit : C->private_copies()) {
      auto *OrigVD = cast<VarDecl>(cast<DeclRefExpr>(*IRef)->getDecl());
      if (EmittedAsPrivate.insert(OrigVD->getCanonicalDecl()).second) {
        Data.LastprivateVars.push_back(*IRef);
        Data.LastprivateCopies.push_back(IInit);
      }
      LastprivateDstsOrigs.insert(
          {cast<VarDecl>(cast<DeclRefExpr>(*ID)->getDecl()),
           cast<DeclRefExpr>(*IRef)});
      ++IRef;
      ++ID;
    }
  }
  SmallVector<const Expr *, 4> LHSs;
  SmallVector<const Expr *, 4> RHSs;
  for (const auto *C : S.getClausesOfKind<OMPReductionClause>()) {
    auto IPriv = C->privates().begin();
    auto IRed = C->reduction_ops().begin();
    auto ILHS = C->lhs_exprs().begin();
    auto IRHS = C->rhs_exprs().begin();
    for (const auto *Ref : C->varlists()) {
      Data.ReductionVars.emplace_back(Ref);
      Data.ReductionCopies.emplace_back(*IPriv);
      Data.ReductionOps.emplace_back(*IRed);
      LHSs.emplace_back(*ILHS);
      RHSs.emplace_back(*IRHS);
      std::advance(IPriv, 1);
      std::advance(IRed, 1);
      std::advance(ILHS, 1);
      std::advance(IRHS, 1);
    }
  }
  Data.Reductions = CGM.getOpenMPRuntime().emitTaskReductionInit(
      *this, S.getLocStart(), LHSs, RHSs, Data);
  // Build list of dependences.
  for (const auto *C : S.getClausesOfKind<OMPDependClause>())
    for (auto *IRef : C->varlists())
      Data.Dependences.push_back(std::make_pair(C->getDependencyKind(), IRef));
  auto &&CodeGen = [&Data, &S, CS, &BodyGen, &LastprivateDstsOrigs](
      CodeGenFunction &CGF, PrePostActionTy &Action) {
    // Set proper addresses for generated private copies.
    OMPPrivateScope Scope(CGF);
    if (!Data.PrivateVars.empty() || !Data.FirstprivateVars.empty() ||
        !Data.LastprivateVars.empty()) {
      auto *CopyFn = CGF.Builder.CreateLoad(
          CGF.GetAddrOfLocalVar(CS->getCapturedDecl()->getParam(3)));
      auto *PrivatesPtr = CGF.Builder.CreateLoad(
          CGF.GetAddrOfLocalVar(CS->getCapturedDecl()->getParam(2)));
      // Map privates.
      llvm::SmallVector<std::pair<const VarDecl *, Address>, 16> PrivatePtrs;
      llvm::SmallVector<llvm::Value *, 16> CallArgs;
      CallArgs.push_back(PrivatesPtr);
      for (auto *E : Data.PrivateVars) {
        auto *VD = cast<VarDecl>(cast<DeclRefExpr>(E)->getDecl());
        Address PrivatePtr = CGF.CreateMemTemp(
            CGF.getContext().getPointerType(E->getType()), ".priv.ptr.addr");
        PrivatePtrs.push_back(std::make_pair(VD, PrivatePtr));
        CallArgs.push_back(PrivatePtr.getPointer());
      }
      for (auto *E : Data.FirstprivateVars) {
        auto *VD = cast<VarDecl>(cast<DeclRefExpr>(E)->getDecl());
        Address PrivatePtr =
            CGF.CreateMemTemp(CGF.getContext().getPointerType(E->getType()),
                              ".firstpriv.ptr.addr");
        PrivatePtrs.push_back(std::make_pair(VD, PrivatePtr));
        CallArgs.push_back(PrivatePtr.getPointer());
      }
      for (auto *E : Data.LastprivateVars) {
        auto *VD = cast<VarDecl>(cast<DeclRefExpr>(E)->getDecl());
        Address PrivatePtr =
            CGF.CreateMemTemp(CGF.getContext().getPointerType(E->getType()),
                              ".lastpriv.ptr.addr");
        PrivatePtrs.push_back(std::make_pair(VD, PrivatePtr));
        CallArgs.push_back(PrivatePtr.getPointer());
      }
      CGF.EmitRuntimeCall(CopyFn, CallArgs);
      for (auto &&Pair : LastprivateDstsOrigs) {
        auto *OrigVD = cast<VarDecl>(Pair.second->getDecl());
        DeclRefExpr DRE(
            const_cast<VarDecl *>(OrigVD),
            /*RefersToEnclosingVariableOrCapture=*/CGF.CapturedStmtInfo->lookup(
                OrigVD) != nullptr,
            Pair.second->getType(), VK_LValue, Pair.second->getExprLoc());
        Scope.addPrivate(Pair.first, [&CGF, &DRE]() {
          return CGF.EmitLValue(&DRE).getAddress();
        });
      }
      for (auto &&Pair : PrivatePtrs) {
        Address Replacement(CGF.Builder.CreateLoad(Pair.second),
                            CGF.getContext().getDeclAlign(Pair.first));
        Scope.addPrivate(Pair.first, [Replacement]() { return Replacement; });
      }
    }
    if (Data.Reductions) {
      OMPLexicalScope LexScope(CGF, S, /*AsInlined=*/true);
      ReductionCodeGen RedCG(Data.ReductionVars, Data.ReductionCopies,
                             Data.ReductionOps);
      llvm::Value *ReductionsPtr = CGF.Builder.CreateLoad(
          CGF.GetAddrOfLocalVar(CS->getCapturedDecl()->getParam(9)));
      for (unsigned Cnt = 0, E = Data.ReductionVars.size(); Cnt < E; ++Cnt) {
        RedCG.emitSharedLValue(CGF, Cnt);
        RedCG.emitAggregateType(CGF, Cnt);
        Address Replacement = CGF.CGM.getOpenMPRuntime().getTaskReductionItem(
            CGF, S.getLocStart(), ReductionsPtr, RedCG.getSharedLValue(Cnt));
        Replacement =
            Address(CGF.EmitScalarConversion(
                        Replacement.getPointer(), CGF.getContext().VoidPtrTy,
                        CGF.getContext().getPointerType(
                            Data.ReductionCopies[Cnt]->getType()),
                        SourceLocation()),
                    Replacement.getAlignment());
        Replacement = RedCG.adjustPrivateAddress(CGF, Cnt, Replacement);
        Scope.addPrivate(RedCG.getBaseDecl(Cnt),
                         [Replacement]() { return Replacement; });
        // FIXME: This must removed once the runtime library is fixed.
        // Emit required threadprivate variables for
        // initilizer/combiner/finalizer.
        CGF.CGM.getOpenMPRuntime().emitTaskReductionFixups(CGF, S.getLocStart(),
                                                           RedCG, Cnt);
      }
    }
    (void)Scope.Privatize();

    Action.Enter(CGF);
    BodyGen(CGF);
  };
  auto *OutlinedFn = CGM.getOpenMPRuntime().emitTaskOutlinedFunction(
      S, *I, *PartId, *TaskT, S.getDirectiveKind(), CodeGen, Data.Tied,
      Data.NumberOfParts);
  OMPLexicalScope Scope(*this, S);
  TaskGen(*this, OutlinedFn, Data);
}

void CodeGenFunction::EmitOMPTaskDirective(const OMPTaskDirective &S) {
  // Emit outlined function for task construct.
  auto CS = cast<CapturedStmt>(S.getAssociatedStmt());
  auto CapturedStruct = GenerateCapturedStmtArgument(*CS);
  auto SharedsTy = getContext().getRecordType(CS->getCapturedRecordDecl());
  const Expr *IfCond = nullptr;
  for (const auto *C : S.getClausesOfKind<OMPIfClause>()) {
    if (C->getNameModifier() == OMPD_unknown ||
        C->getNameModifier() == OMPD_task) {
      IfCond = C->getCondition();
      break;
    }
  }

  OMPTaskDataTy Data;
  // Check if we should emit tied or untied task.
  Data.Tied = !S.getSingleClause<OMPUntiedClause>();
  auto &&BodyGen = [CS](CodeGenFunction &CGF, PrePostActionTy &) {
    CGF.EmitStmt(CS->getCapturedStmt());
  };
  auto &&TaskGen = [&S, SharedsTy, CapturedStruct,
                    IfCond](CodeGenFunction &CGF, llvm::Value *OutlinedFn,
                            const OMPTaskDataTy &Data) {
    CGF.CGM.getOpenMPRuntime().emitTaskCall(CGF, S.getLocStart(), S, OutlinedFn,
                                            SharedsTy, CapturedStruct, IfCond,
                                            Data);
  };
  EmitOMPTaskBasedDirective(S, BodyGen, TaskGen, Data);
}

void CodeGenFunction::EmitOMPTaskyieldDirective(
    const OMPTaskyieldDirective &S) {
  CGM.getOpenMPRuntime().emitTaskyieldCall(*this, S.getLocStart());
}

void CodeGenFunction::EmitOMPBarrierDirective(const OMPBarrierDirective &S) {
  CGM.getOpenMPRuntime().emitBarrierCall(*this, S.getLocStart(), OMPD_barrier);
}

void CodeGenFunction::EmitOMPTaskwaitDirective(const OMPTaskwaitDirective &S) {
  CGM.getOpenMPRuntime().emitTaskwaitCall(*this, S.getLocStart());
}

void CodeGenFunction::EmitOMPTaskgroupDirective(
    const OMPTaskgroupDirective &S) {
  auto &&CodeGen = [&S](CodeGenFunction &CGF, PrePostActionTy &Action) {
    Action.Enter(CGF);
    CGF.EmitStmt(cast<CapturedStmt>(S.getAssociatedStmt())->getCapturedStmt());
  };
  OMPLexicalScope Scope(*this, S, /*AsInlined=*/true);
  CGM.getOpenMPRuntime().emitTaskgroupRegion(*this, CodeGen, S.getLocStart());
}

void CodeGenFunction::EmitOMPFlushDirective(const OMPFlushDirective &S) {
  CGM.getOpenMPRuntime().emitFlush(*this, [&]() -> ArrayRef<const Expr *> {
    if (const auto *FlushClause = S.getSingleClause<OMPFlushClause>()) {
      return llvm::makeArrayRef(FlushClause->varlist_begin(),
                                FlushClause->varlist_end());
    }
    return llvm::None;
  }(), S.getLocStart());
}

void CodeGenFunction::EmitOMPDistributeLoop(const OMPLoopDirective &S,
                                            const CodeGenLoopTy &CodeGenLoop,
                                            Expr *IncExpr) {
  // Emit the loop iteration variable.
  auto IVExpr = cast<DeclRefExpr>(S.getIterationVariable());
  auto IVDecl = cast<VarDecl>(IVExpr->getDecl());
  EmitVarDecl(*IVDecl);

  // Emit the iterations count variable.
  // If it is not a variable, Sema decided to calculate iterations count on each
  // iteration (e.g., it is foldable into a constant).
  if (auto LIExpr = dyn_cast<DeclRefExpr>(S.getLastIteration())) {
    EmitVarDecl(*cast<VarDecl>(LIExpr->getDecl()));
    // Emit calculation of the iterations count.
    EmitIgnoredExpr(S.getCalcLastIteration());
  }

  auto &RT = CGM.getOpenMPRuntime();

  bool HasLastprivateClause = false;
  // Check pre-condition.
  {
    OMPLoopScope PreInitScope(*this, S);
    // Skip the entire loop if we don't meet the precondition.
    // If the condition constant folds and can be elided, avoid emitting the
    // whole loop.
    bool CondConstant;
    llvm::BasicBlock *ContBlock = nullptr;
    if (ConstantFoldsToSimpleInteger(S.getPreCond(), CondConstant)) {
      if (!CondConstant)
        return;
    } else {
      auto *ThenBlock = createBasicBlock("omp.precond.then");
      ContBlock = createBasicBlock("omp.precond.end");
      emitPreCond(*this, S, S.getPreCond(), ThenBlock, ContBlock,
                  getProfileCount(&S));
      EmitBlock(ThenBlock);
      incrementProfileCounter(&S);
    }

    // Emit 'then' code.
    {
      // Emit helper vars inits.

      LValue LB = EmitOMPHelperVar(
          *this, cast<DeclRefExpr>(
                     (isOpenMPLoopBoundSharingDirective(S.getDirectiveKind())
                          ? S.getCombinedLowerBoundVariable()
                          : S.getLowerBoundVariable())));
      LValue UB = EmitOMPHelperVar(
          *this, cast<DeclRefExpr>(
                     (isOpenMPLoopBoundSharingDirective(S.getDirectiveKind())
                          ? S.getCombinedUpperBoundVariable()
                          : S.getUpperBoundVariable())));
      LValue ST =
          EmitOMPHelperVar(*this, cast<DeclRefExpr>(S.getStrideVariable()));
      LValue IL =
          EmitOMPHelperVar(*this, cast<DeclRefExpr>(S.getIsLastIterVariable()));

      OMPPrivateScope LoopScope(*this);
      if (EmitOMPFirstprivateClause(S, LoopScope)) {
        // Emit implicit barrier to synchronize threads and avoid data races on
        // initialization of firstprivate variables and post-update of
        // lastprivate variables.
        CGM.getOpenMPRuntime().emitBarrierCall(
          *this, S.getLocStart(), OMPD_unknown, /*EmitChecks=*/false,
          /*ForceSimpleCall=*/true);
      }
      EmitOMPPrivateClause(S, LoopScope);
      HasLastprivateClause = EmitOMPLastprivateClauseInit(S, LoopScope);
      EmitOMPPrivateLoopCounters(S, LoopScope);
      (void)LoopScope.Privatize();

      // Detect the distribute schedule kind and chunk.
      llvm::Value *Chunk = nullptr;
      OpenMPDistScheduleClauseKind ScheduleKind = OMPC_DIST_SCHEDULE_unknown;
      if (auto *C = S.getSingleClause<OMPDistScheduleClause>()) {
        ScheduleKind = C->getDistScheduleKind();
        if (const auto *Ch = C->getChunkSize()) {
          Chunk = EmitScalarExpr(Ch);
          Chunk = EmitScalarConversion(Chunk, Ch->getType(),
          S.getIterationVariable()->getType(),
          S.getLocStart());
        }
      }
      const unsigned IVSize = getContext().getTypeSize(IVExpr->getType());
      const bool IVSigned = IVExpr->getType()->hasSignedIntegerRepresentation();

      // OpenMP [2.10.8, distribute Construct, Description]
      // If dist_schedule is specified, kind must be static. If specified,
      // iterations are divided into chunks of size chunk_size, chunks are
      // assigned to the teams of the league in a round-robin fashion in the
      // order of the team number. When no chunk_size is specified, the
      // iteration space is divided into chunks that are approximately equal
      // in size, and at most one chunk is distributed to each team of the
      // league. The size of the chunks is unspecified in this case.
      if (RT.isStaticNonchunked(ScheduleKind,
                                /* Chunked */ Chunk != nullptr)) {
        RT.emitDistributeStaticInit(*this, S.getLocStart(), ScheduleKind,
                             IVSize, IVSigned, /* Ordered = */ false,
                             IL.getAddress(), LB.getAddress(),
                             UB.getAddress(), ST.getAddress());
        auto LoopExit =
            getJumpDestInCurrentScope(createBasicBlock("omp.loop.exit"));
        // UB = min(UB, GlobalUB);
        EmitIgnoredExpr(isOpenMPLoopBoundSharingDirective(S.getDirectiveKind())
                            ? S.getCombinedEnsureUpperBound()
                            : S.getEnsureUpperBound());
        // IV = LB;
        EmitIgnoredExpr(isOpenMPLoopBoundSharingDirective(S.getDirectiveKind())
                            ? S.getCombinedInit()
                            : S.getInit());

        Expr *Cond = isOpenMPLoopBoundSharingDirective(S.getDirectiveKind())
                         ? S.getCombinedCond()
                         : S.getCond();

        // for distribute alone,  codegen
        // while (idx <= UB) { BODY; ++idx; }
        // when combined with 'for' (e.g. as in 'distribute parallel for')
        // while (idx <= UB) { <CodeGen rest of pragma>; idx += ST; }
        EmitOMPInnerLoop(S, LoopScope.requiresCleanups(), Cond, IncExpr,
                         [&S, LoopExit, &CodeGenLoop](CodeGenFunction &CGF) {
                           CodeGenLoop(CGF, S, LoopExit);
                         },
                         [](CodeGenFunction &) {});
        EmitBlock(LoopExit.getBlock());
        // Tell the runtime we are done.
        RT.emitForStaticFinish(*this, S.getLocStart());
      } else {
        // Emit the outer loop, which requests its work chunk [LB..UB] from
        // runtime and runs the inner loop to process it.
        const OMPLoopArguments LoopArguments = {
            LB.getAddress(), UB.getAddress(), ST.getAddress(), IL.getAddress(),
            Chunk};
        EmitOMPDistributeOuterLoop(ScheduleKind, S, LoopScope, LoopArguments,
                                   CodeGenLoop);
      }

      // Emit final copy of the lastprivate variables if IsLastIter != 0.
      if (HasLastprivateClause)
        EmitOMPLastprivateClauseFinal(
            S, /*NoFinals=*/false,
            Builder.CreateIsNotNull(
                EmitLoadOfScalar(IL, S.getLocStart())));
    }

    // We're now done with the loop, so jump to the continuation block.
    if (ContBlock) {
      EmitBranch(ContBlock);
      EmitBlock(ContBlock, true);
    }
  }
}

void CodeGenFunction::EmitOMPDistributeDirective(
    const OMPDistributeDirective &S) {
  auto &&CodeGen = [&S](CodeGenFunction &CGF, PrePostActionTy &) {

    CGF.EmitOMPDistributeLoop(S, emitOMPLoopBodyWithStopPoint, S.getInc());
  };
  OMPLexicalScope Scope(*this, S, /*AsInlined=*/true);
  CGM.getOpenMPRuntime().emitInlinedDirective(*this, OMPD_distribute, CodeGen,
                                              false);
}

static llvm::Function *emitOutlinedOrderedFunction(CodeGenModule &CGM,
                                                   const CapturedStmt *S) {
  CodeGenFunction CGF(CGM, /*suppressNewContext=*/true);
  CodeGenFunction::CGCapturedStmtInfo CapStmtInfo;
  CGF.CapturedStmtInfo = &CapStmtInfo;
  auto *Fn = CGF.GenerateOpenMPCapturedStmtFunction(*S);
  Fn->addFnAttr(llvm::Attribute::NoInline);
  return Fn;
}

void CodeGenFunction::EmitOMPOrderedDirective(const OMPOrderedDirective &S) {
  if (!S.getAssociatedStmt()) {
    for (const auto *DC : S.getClausesOfKind<OMPDependClause>())
      CGM.getOpenMPRuntime().emitDoacrossOrdered(*this, DC);
    return;
  }
  auto *C = S.getSingleClause<OMPSIMDClause>();
  auto &&CodeGen = [&S, C, this](CodeGenFunction &CGF,
                                 PrePostActionTy &Action) {
    if (C) {
      auto CS = cast<CapturedStmt>(S.getAssociatedStmt());
      llvm::SmallVector<llvm::Value *, 16> CapturedVars;
      CGF.GenerateOpenMPCapturedVars(*CS, CapturedVars);
      auto *OutlinedFn = emitOutlinedOrderedFunction(CGM, CS);
      CGF.EmitNounwindRuntimeCall(OutlinedFn, CapturedVars);
    } else {
      Action.Enter(CGF);
      CGF.EmitStmt(
          cast<CapturedStmt>(S.getAssociatedStmt())->getCapturedStmt());
    }
  };
  OMPLexicalScope Scope(*this, S, /*AsInlined=*/true);
  CGM.getOpenMPRuntime().emitOrderedRegion(*this, CodeGen, S.getLocStart(), !C);
}

static llvm::Value *convertToScalarValue(CodeGenFunction &CGF, RValue Val,
                                         QualType SrcType, QualType DestType,
                                         SourceLocation Loc) {
  assert(CGF.hasScalarEvaluationKind(DestType) &&
         "DestType must have scalar evaluation kind.");
  assert(!Val.isAggregate() && "Must be a scalar or complex.");
  return Val.isScalar()
             ? CGF.EmitScalarConversion(Val.getScalarVal(), SrcType, DestType,
                                        Loc)
             : CGF.EmitComplexToScalarConversion(Val.getComplexVal(), SrcType,
                                                 DestType, Loc);
}

static CodeGenFunction::ComplexPairTy
convertToComplexValue(CodeGenFunction &CGF, RValue Val, QualType SrcType,
                      QualType DestType, SourceLocation Loc) {
  assert(CGF.getEvaluationKind(DestType) == TEK_Complex &&
         "DestType must have complex evaluation kind.");
  CodeGenFunction::ComplexPairTy ComplexVal;
  if (Val.isScalar()) {
    // Convert the input element to the element type of the complex.
    auto DestElementType = DestType->castAs<ComplexType>()->getElementType();
    auto ScalarVal = CGF.EmitScalarConversion(Val.getScalarVal(), SrcType,
                                              DestElementType, Loc);
    ComplexVal = CodeGenFunction::ComplexPairTy(
        ScalarVal, llvm::Constant::getNullValue(ScalarVal->getType()));
  } else {
    assert(Val.isComplex() && "Must be a scalar or complex.");
    auto SrcElementType = SrcType->castAs<ComplexType>()->getElementType();
    auto DestElementType = DestType->castAs<ComplexType>()->getElementType();
    ComplexVal.first = CGF.EmitScalarConversion(
        Val.getComplexVal().first, SrcElementType, DestElementType, Loc);
    ComplexVal.second = CGF.EmitScalarConversion(
        Val.getComplexVal().second, SrcElementType, DestElementType, Loc);
  }
  return ComplexVal;
}

static void emitSimpleAtomicStore(CodeGenFunction &CGF, bool IsSeqCst,
                                  LValue LVal, RValue RVal) {
  if (LVal.isGlobalReg()) {
    CGF.EmitStoreThroughGlobalRegLValue(RVal, LVal);
  } else {
    CGF.EmitAtomicStore(RVal, LVal,
                        IsSeqCst ? llvm::AtomicOrdering::SequentiallyConsistent
                                 : llvm::AtomicOrdering::Monotonic,
                        LVal.isVolatile(), /*IsInit=*/false);
  }
}

void CodeGenFunction::emitOMPSimpleStore(LValue LVal, RValue RVal,
                                         QualType RValTy, SourceLocation Loc) {
  switch (getEvaluationKind(LVal.getType())) {
  case TEK_Scalar:
    EmitStoreThroughLValue(RValue::get(convertToScalarValue(
                               *this, RVal, RValTy, LVal.getType(), Loc)),
                           LVal);
    break;
  case TEK_Complex:
    EmitStoreOfComplex(
        convertToComplexValue(*this, RVal, RValTy, LVal.getType(), Loc), LVal,
        /*isInit=*/false);
    break;
  case TEK_Aggregate:
    llvm_unreachable("Must be a scalar or complex.");
  }
}

static void EmitOMPAtomicReadExpr(CodeGenFunction &CGF, bool IsSeqCst,
                                  const Expr *X, const Expr *V,
                                  SourceLocation Loc) {
  // v = x;
  assert(V->isLValue() && "V of 'omp atomic read' is not lvalue");
  assert(X->isLValue() && "X of 'omp atomic read' is not lvalue");
  LValue XLValue = CGF.EmitLValue(X);
  LValue VLValue = CGF.EmitLValue(V);
  RValue Res = XLValue.isGlobalReg()
                   ? CGF.EmitLoadOfLValue(XLValue, Loc)
                   : CGF.EmitAtomicLoad(
                         XLValue, Loc,
                         IsSeqCst ? llvm::AtomicOrdering::SequentiallyConsistent
                                  : llvm::AtomicOrdering::Monotonic,
                         XLValue.isVolatile());
  // OpenMP, 2.12.6, atomic Construct
  // Any atomic construct with a seq_cst clause forces the atomically
  // performed operation to include an implicit flush operation without a
  // list.
  if (IsSeqCst)
    CGF.CGM.getOpenMPRuntime().emitFlush(CGF, llvm::None, Loc);
  CGF.emitOMPSimpleStore(VLValue, Res, X->getType().getNonReferenceType(), Loc);
}

static void EmitOMPAtomicWriteExpr(CodeGenFunction &CGF, bool IsSeqCst,
                                   const Expr *X, const Expr *E,
                                   SourceLocation Loc) {
  // x = expr;
  assert(X->isLValue() && "X of 'omp atomic write' is not lvalue");
  emitSimpleAtomicStore(CGF, IsSeqCst, CGF.EmitLValue(X), CGF.EmitAnyExpr(E));
  // OpenMP, 2.12.6, atomic Construct
  // Any atomic construct with a seq_cst clause forces the atomically
  // performed operation to include an implicit flush operation without a
  // list.
  if (IsSeqCst)
    CGF.CGM.getOpenMPRuntime().emitFlush(CGF, llvm::None, Loc);
}

static std::pair<bool, RValue> emitOMPAtomicRMW(CodeGenFunction &CGF, LValue X,
                                                RValue Update,
                                                BinaryOperatorKind BO,
                                                llvm::AtomicOrdering AO,
                                                bool IsXLHSInRHSPart) {
  auto &Context = CGF.CGM.getContext();
  // Allow atomicrmw only if 'x' and 'update' are integer values, lvalue for 'x'
  // expression is simple and atomic is allowed for the given type for the
  // target platform.
  if (BO == BO_Comma || !Update.isScalar() ||
      !Update.getScalarVal()->getType()->isIntegerTy() ||
      !X.isSimple() || (!isa<llvm::ConstantInt>(Update.getScalarVal()) &&
                        (Update.getScalarVal()->getType() !=
                         X.getAddress().getElementType())) ||
      !X.getAddress().getElementType()->isIntegerTy() ||
      !Context.getTargetInfo().hasBuiltinAtomic(
          Context.getTypeSize(X.getType()), Context.toBits(X.getAlignment())))
    return std::make_pair(false, RValue::get(nullptr));

  llvm::AtomicRMWInst::BinOp RMWOp;
  switch (BO) {
  case BO_Add:
    RMWOp = llvm::AtomicRMWInst::Add;
    break;
  case BO_Sub:
    if (!IsXLHSInRHSPart)
      return std::make_pair(false, RValue::get(nullptr));
    RMWOp = llvm::AtomicRMWInst::Sub;
    break;
  case BO_And:
    RMWOp = llvm::AtomicRMWInst::And;
    break;
  case BO_Or:
    RMWOp = llvm::AtomicRMWInst::Or;
    break;
  case BO_Xor:
    RMWOp = llvm::AtomicRMWInst::Xor;
    break;
  case BO_LT:
    RMWOp = X.getType()->hasSignedIntegerRepresentation()
                ? (IsXLHSInRHSPart ? llvm::AtomicRMWInst::Min
                                   : llvm::AtomicRMWInst::Max)
                : (IsXLHSInRHSPart ? llvm::AtomicRMWInst::UMin
                                   : llvm::AtomicRMWInst::UMax);
    break;
  case BO_GT:
    RMWOp = X.getType()->hasSignedIntegerRepresentation()
                ? (IsXLHSInRHSPart ? llvm::AtomicRMWInst::Max
                                   : llvm::AtomicRMWInst::Min)
                : (IsXLHSInRHSPart ? llvm::AtomicRMWInst::UMax
                                   : llvm::AtomicRMWInst::UMin);
    break;
  case BO_Assign:
    RMWOp = llvm::AtomicRMWInst::Xchg;
    break;
  case BO_Mul:
  case BO_Div:
  case BO_Rem:
  case BO_Shl:
  case BO_Shr:
  case BO_LAnd:
  case BO_LOr:
    return std::make_pair(false, RValue::get(nullptr));
  case BO_PtrMemD:
  case BO_PtrMemI:
  case BO_LE:
  case BO_GE:
  case BO_EQ:
  case BO_NE:
  case BO_AddAssign:
  case BO_SubAssign:
  case BO_AndAssign:
  case BO_OrAssign:
  case BO_XorAssign:
  case BO_MulAssign:
  case BO_DivAssign:
  case BO_RemAssign:
  case BO_ShlAssign:
  case BO_ShrAssign:
  case BO_Comma:
    llvm_unreachable("Unsupported atomic update operation");
  }
  auto *UpdateVal = Update.getScalarVal();
  if (auto *IC = dyn_cast<llvm::ConstantInt>(UpdateVal)) {
    UpdateVal = CGF.Builder.CreateIntCast(
        IC, X.getAddress().getElementType(),
        X.getType()->hasSignedIntegerRepresentation());
  }
  auto *Res = CGF.Builder.CreateAtomicRMW(RMWOp, X.getPointer(), UpdateVal, AO);
  return std::make_pair(true, RValue::get(Res));
}

std::pair<bool, RValue> CodeGenFunction::EmitOMPAtomicSimpleUpdateExpr(
    LValue X, RValue E, BinaryOperatorKind BO, bool IsXLHSInRHSPart,
    llvm::AtomicOrdering AO, SourceLocation Loc,
    const llvm::function_ref<RValue(RValue)> &CommonGen) {
  // Update expressions are allowed to have the following forms:
  // x binop= expr; -> xrval + expr;
  // x++, ++x -> xrval + 1;
  // x--, --x -> xrval - 1;
  // x = x binop expr; -> xrval binop expr
  // x = expr Op x; - > expr binop xrval;
  auto Res = emitOMPAtomicRMW(*this, X, E, BO, AO, IsXLHSInRHSPart);
  if (!Res.first) {
    if (X.isGlobalReg()) {
      // Emit an update expression: 'xrval' binop 'expr' or 'expr' binop
      // 'xrval'.
      EmitStoreThroughLValue(CommonGen(EmitLoadOfLValue(X, Loc)), X);
    } else {
      // Perform compare-and-swap procedure.
      EmitAtomicUpdate(X, AO, CommonGen, X.getType().isVolatileQualified());
    }
  }
  return Res;
}

static void EmitOMPAtomicUpdateExpr(CodeGenFunction &CGF, bool IsSeqCst,
                                    const Expr *X, const Expr *E,
                                    const Expr *UE, bool IsXLHSInRHSPart,
                                    SourceLocation Loc) {
  assert(isa<BinaryOperator>(UE->IgnoreImpCasts()) &&
         "Update expr in 'atomic update' must be a binary operator.");
  auto *BOUE = cast<BinaryOperator>(UE->IgnoreImpCasts());
  // Update expressions are allowed to have the following forms:
  // x binop= expr; -> xrval + expr;
  // x++, ++x -> xrval + 1;
  // x--, --x -> xrval - 1;
  // x = x binop expr; -> xrval binop expr
  // x = expr Op x; - > expr binop xrval;
  assert(X->isLValue() && "X of 'omp atomic update' is not lvalue");
  LValue XLValue = CGF.EmitLValue(X);
  RValue ExprRValue = CGF.EmitAnyExpr(E);
  auto AO = IsSeqCst ? llvm::AtomicOrdering::SequentiallyConsistent
                     : llvm::AtomicOrdering::Monotonic;
  auto *LHS = cast<OpaqueValueExpr>(BOUE->getLHS()->IgnoreImpCasts());
  auto *RHS = cast<OpaqueValueExpr>(BOUE->getRHS()->IgnoreImpCasts());
  auto *XRValExpr = IsXLHSInRHSPart ? LHS : RHS;
  auto *ERValExpr = IsXLHSInRHSPart ? RHS : LHS;
  auto Gen =
      [&CGF, UE, ExprRValue, XRValExpr, ERValExpr](RValue XRValue) -> RValue {
        CodeGenFunction::OpaqueValueMapping MapExpr(CGF, ERValExpr, ExprRValue);
        CodeGenFunction::OpaqueValueMapping MapX(CGF, XRValExpr, XRValue);
        return CGF.EmitAnyExpr(UE);
      };
  (void)CGF.EmitOMPAtomicSimpleUpdateExpr(
      XLValue, ExprRValue, BOUE->getOpcode(), IsXLHSInRHSPart, AO, Loc, Gen);
  // OpenMP, 2.12.6, atomic Construct
  // Any atomic construct with a seq_cst clause forces the atomically
  // performed operation to include an implicit flush operation without a
  // list.
  if (IsSeqCst)
    CGF.CGM.getOpenMPRuntime().emitFlush(CGF, llvm::None, Loc);
}

static RValue convertToType(CodeGenFunction &CGF, RValue Value,
                            QualType SourceType, QualType ResType,
                            SourceLocation Loc) {
  switch (CGF.getEvaluationKind(ResType)) {
  case TEK_Scalar:
    return RValue::get(
        convertToScalarValue(CGF, Value, SourceType, ResType, Loc));
  case TEK_Complex: {
    auto Res = convertToComplexValue(CGF, Value, SourceType, ResType, Loc);
    return RValue::getComplex(Res.first, Res.second);
  }
  case TEK_Aggregate:
    break;
  }
  llvm_unreachable("Must be a scalar or complex.");
}

static void EmitOMPAtomicCaptureExpr(CodeGenFunction &CGF, bool IsSeqCst,
                                     bool IsPostfixUpdate, const Expr *V,
                                     const Expr *X, const Expr *E,
                                     const Expr *UE, bool IsXLHSInRHSPart,
                                     SourceLocation Loc) {
  assert(X->isLValue() && "X of 'omp atomic capture' is not lvalue");
  assert(V->isLValue() && "V of 'omp atomic capture' is not lvalue");
  RValue NewVVal;
  LValue VLValue = CGF.EmitLValue(V);
  LValue XLValue = CGF.EmitLValue(X);
  RValue ExprRValue = CGF.EmitAnyExpr(E);
  auto AO = IsSeqCst ? llvm::AtomicOrdering::SequentiallyConsistent
                     : llvm::AtomicOrdering::Monotonic;
  QualType NewVValType;
  if (UE) {
    // 'x' is updated with some additional value.
    assert(isa<BinaryOperator>(UE->IgnoreImpCasts()) &&
           "Update expr in 'atomic capture' must be a binary operator.");
    auto *BOUE = cast<BinaryOperator>(UE->IgnoreImpCasts());
    // Update expressions are allowed to have the following forms:
    // x binop= expr; -> xrval + expr;
    // x++, ++x -> xrval + 1;
    // x--, --x -> xrval - 1;
    // x = x binop expr; -> xrval binop expr
    // x = expr Op x; - > expr binop xrval;
    auto *LHS = cast<OpaqueValueExpr>(BOUE->getLHS()->IgnoreImpCasts());
    auto *RHS = cast<OpaqueValueExpr>(BOUE->getRHS()->IgnoreImpCasts());
    auto *XRValExpr = IsXLHSInRHSPart ? LHS : RHS;
    NewVValType = XRValExpr->getType();
    auto *ERValExpr = IsXLHSInRHSPart ? RHS : LHS;
    auto &&Gen = [&CGF, &NewVVal, UE, ExprRValue, XRValExpr, ERValExpr,
                  IsPostfixUpdate](RValue XRValue) -> RValue {
      CodeGenFunction::OpaqueValueMapping MapExpr(CGF, ERValExpr, ExprRValue);
      CodeGenFunction::OpaqueValueMapping MapX(CGF, XRValExpr, XRValue);
      RValue Res = CGF.EmitAnyExpr(UE);
      NewVVal = IsPostfixUpdate ? XRValue : Res;
      return Res;
    };
    auto Res = CGF.EmitOMPAtomicSimpleUpdateExpr(
        XLValue, ExprRValue, BOUE->getOpcode(), IsXLHSInRHSPart, AO, Loc, Gen);
    if (Res.first) {
      // 'atomicrmw' instruction was generated.
      if (IsPostfixUpdate) {
        // Use old value from 'atomicrmw'.
        NewVVal = Res.second;
      } else {
        // 'atomicrmw' does not provide new value, so evaluate it using old
        // value of 'x'.
        CodeGenFunction::OpaqueValueMapping MapExpr(CGF, ERValExpr, ExprRValue);
        CodeGenFunction::OpaqueValueMapping MapX(CGF, XRValExpr, Res.second);
        NewVVal = CGF.EmitAnyExpr(UE);
      }
    }
  } else {
    // 'x' is simply rewritten with some 'expr'.
    NewVValType = X->getType().getNonReferenceType();
    ExprRValue = convertToType(CGF, ExprRValue, E->getType(),
                               X->getType().getNonReferenceType(), Loc);
    auto &&Gen = [&NewVVal, ExprRValue](RValue XRValue) -> RValue {
      NewVVal = XRValue;
      return ExprRValue;
    };
    // Try to perform atomicrmw xchg, otherwise simple exchange.
    auto Res = CGF.EmitOMPAtomicSimpleUpdateExpr(
        XLValue, ExprRValue, /*BO=*/BO_Assign, /*IsXLHSInRHSPart=*/false, AO,
        Loc, Gen);
    if (Res.first) {
      // 'atomicrmw' instruction was generated.
      NewVVal = IsPostfixUpdate ? Res.second : ExprRValue;
    }
  }
  // Emit post-update store to 'v' of old/new 'x' value.
  CGF.emitOMPSimpleStore(VLValue, NewVVal, NewVValType, Loc);
  // OpenMP, 2.12.6, atomic Construct
  // Any atomic construct with a seq_cst clause forces the atomically
  // performed operation to include an implicit flush operation without a
  // list.
  if (IsSeqCst)
    CGF.CGM.getOpenMPRuntime().emitFlush(CGF, llvm::None, Loc);
}

static void EmitOMPAtomicExpr(CodeGenFunction &CGF, OpenMPClauseKind Kind,
                              bool IsSeqCst, bool IsPostfixUpdate,
                              const Expr *X, const Expr *V, const Expr *E,
                              const Expr *UE, bool IsXLHSInRHSPart,
                              SourceLocation Loc) {
  switch (Kind) {
  case OMPC_read:
    EmitOMPAtomicReadExpr(CGF, IsSeqCst, X, V, Loc);
    break;
  case OMPC_write:
    EmitOMPAtomicWriteExpr(CGF, IsSeqCst, X, E, Loc);
    break;
  case OMPC_unknown:
  case OMPC_update:
    EmitOMPAtomicUpdateExpr(CGF, IsSeqCst, X, E, UE, IsXLHSInRHSPart, Loc);
    break;
  case OMPC_capture:
    EmitOMPAtomicCaptureExpr(CGF, IsSeqCst, IsPostfixUpdate, V, X, E, UE,
                             IsXLHSInRHSPart, Loc);
    break;
  case OMPC_if:
  case OMPC_final:
  case OMPC_num_threads:
  case OMPC_private:
  case OMPC_firstprivate:
  case OMPC_lastprivate:
  case OMPC_reduction:
  case OMPC_task_reduction:
  case OMPC_safelen:
  case OMPC_simdlen:
  case OMPC_collapse:
  case OMPC_default:
  case OMPC_seq_cst:
  case OMPC_shared:
  case OMPC_linear:
  case OMPC_aligned:
  case OMPC_copyin:
  case OMPC_copyprivate:
  case OMPC_flush:
  case OMPC_proc_bind:
  case OMPC_schedule:
  case OMPC_ordered:
  case OMPC_nowait:
  case OMPC_untied:
  case OMPC_threadprivate:
  case OMPC_depend:
  case OMPC_mergeable:
  case OMPC_device:
  case OMPC_threads:
  case OMPC_simd:
  case OMPC_map:
  case OMPC_num_teams:
  case OMPC_thread_limit:
  case OMPC_priority:
  case OMPC_grainsize:
  case OMPC_nogroup:
  case OMPC_num_tasks:
  case OMPC_hint:
  case OMPC_dist_schedule:
  case OMPC_defaultmap:
  case OMPC_uniform:
  case OMPC_to:
  case OMPC_from:
  case OMPC_use_device_ptr:
  case OMPC_is_device_ptr:
    llvm_unreachable("Clause is not allowed in 'omp atomic'.");
  }
}

void CodeGenFunction::EmitOMPAtomicDirective(const OMPAtomicDirective &S) {
  bool IsSeqCst = S.getSingleClause<OMPSeqCstClause>();
  OpenMPClauseKind Kind = OMPC_unknown;
  for (auto *C : S.clauses()) {
    // Find first clause (skip seq_cst clause, if it is first).
    if (C->getClauseKind() != OMPC_seq_cst) {
      Kind = C->getClauseKind();
      break;
    }
  }

  const auto *CS =
      S.getAssociatedStmt()->IgnoreContainers(/*IgnoreCaptured=*/true);
  if (const auto *EWC = dyn_cast<ExprWithCleanups>(CS)) {
    enterFullExpression(EWC);
  }
  // Processing for statements under 'atomic capture'.
  if (const auto *Compound = dyn_cast<CompoundStmt>(CS)) {
    for (const auto *C : Compound->body()) {
      if (const auto *EWC = dyn_cast<ExprWithCleanups>(C)) {
        enterFullExpression(EWC);
      }
    }
  }

  auto &&CodeGen = [&S, Kind, IsSeqCst, CS](CodeGenFunction &CGF,
                                            PrePostActionTy &) {
    CGF.EmitStopPoint(CS);
    EmitOMPAtomicExpr(CGF, Kind, IsSeqCst, S.isPostfixUpdate(), S.getX(),
                      S.getV(), S.getExpr(), S.getUpdateExpr(),
                      S.isXLHSInRHSPart(), S.getLocStart());
  };
  OMPLexicalScope Scope(*this, S, /*AsInlined=*/true);
  CGM.getOpenMPRuntime().emitInlinedDirective(*this, OMPD_atomic, CodeGen);
}

static void emitCommonOMPTargetDirective(CodeGenFunction &CGF,
                                         const OMPExecutableDirective &S,
                                         const RegionCodeGenTy &CodeGen) {
  assert(isOpenMPTargetExecutionDirective(S.getDirectiveKind()));
  CodeGenModule &CGM = CGF.CGM;
  const CapturedStmt &CS = *cast<CapturedStmt>(S.getAssociatedStmt());

  llvm::Function *Fn = nullptr;
  llvm::Constant *FnID = nullptr;

  const Expr *IfCond = nullptr;
  // Check for the at most one if clause associated with the target region.
  for (const auto *C : S.getClausesOfKind<OMPIfClause>()) {
    if (C->getNameModifier() == OMPD_unknown ||
        C->getNameModifier() == OMPD_target) {
      IfCond = C->getCondition();
      break;
    }
  }

  // Check if we have any device clause associated with the directive.
  const Expr *Device = nullptr;
  if (auto *C = S.getSingleClause<OMPDeviceClause>()) {
    Device = C->getDevice();
  }

  // Check if we have an if clause whose conditional always evaluates to false
  // or if we do not have any targets specified. If so the target region is not
  // an offload entry point.
  bool IsOffloadEntry = true;
  if (IfCond) {
    bool Val;
    if (CGF.ConstantFoldsToSimpleInteger(IfCond, Val) && !Val)
      IsOffloadEntry = false;
  }
  if (CGM.getLangOpts().OMPTargetTriples.empty())
    IsOffloadEntry = false;

  assert(CGF.CurFuncDecl && "No parent declaration for target region!");
  StringRef ParentName;
  // In case we have Ctors/Dtors we use the complete type variant to produce
  // the mangling of the device outlined kernel.
  if (auto *D = dyn_cast<CXXConstructorDecl>(CGF.CurFuncDecl))
    ParentName = CGM.getMangledName(GlobalDecl(D, Ctor_Complete));
  else if (auto *D = dyn_cast<CXXDestructorDecl>(CGF.CurFuncDecl))
    ParentName = CGM.getMangledName(GlobalDecl(D, Dtor_Complete));
  else
    ParentName =
        CGM.getMangledName(GlobalDecl(cast<FunctionDecl>(CGF.CurFuncDecl)));

  // Emit target region as a standalone region.
  CGM.getOpenMPRuntime().emitTargetOutlinedFunction(S, ParentName, Fn, FnID,
                                                    IsOffloadEntry, CodeGen);
  OMPLexicalScope Scope(CGF, S);
  llvm::SmallVector<llvm::Value *, 16> CapturedVars;
  CGF.GenerateOpenMPCapturedVars(CS, CapturedVars);
  CGM.getOpenMPRuntime().emitTargetCall(CGF, S, Fn, FnID, IfCond, Device,
                                        CapturedVars);
}

static void emitTargetRegion(CodeGenFunction &CGF, const OMPTargetDirective &S,
                             PrePostActionTy &Action) {
  CodeGenFunction::OMPPrivateScope PrivateScope(CGF);
  (void)CGF.EmitOMPFirstprivateClause(S, PrivateScope);
  CGF.EmitOMPPrivateClause(S, PrivateScope);
  (void)PrivateScope.Privatize();

  Action.Enter(CGF);
  CGF.EmitStmt(cast<CapturedStmt>(S.getAssociatedStmt())->getCapturedStmt());
}

void CodeGenFunction::EmitOMPTargetDeviceFunction(CodeGenModule &CGM,
                                                  StringRef ParentName,
                                                  const OMPTargetDirective &S) {
  auto &&CodeGen = [&S](CodeGenFunction &CGF, PrePostActionTy &Action) {
    emitTargetRegion(CGF, S, Action);
  };
  llvm::Function *Fn;
  llvm::Constant *Addr;
  // Emit target region as a standalone region.
  CGM.getOpenMPRuntime().emitTargetOutlinedFunction(
      S, ParentName, Fn, Addr, /*IsOffloadEntry=*/true, CodeGen);
  assert(Fn && Addr && "Target device function emission failed.");
}

void CodeGenFunction::EmitOMPTargetDirective(const OMPTargetDirective &S) {
  auto &&CodeGen = [&S](CodeGenFunction &CGF, PrePostActionTy &Action) {
    emitTargetRegion(CGF, S, Action);
  };
  emitCommonOMPTargetDirective(*this, S, CodeGen);
}

static void emitCommonOMPTeamsDirective(CodeGenFunction &CGF,
                                        const OMPExecutableDirective &S,
                                        OpenMPDirectiveKind InnermostKind,
                                        const RegionCodeGenTy &CodeGen) {
  const CapturedStmt *CS = S.getCapturedStmt(OMPD_teams);
  auto OutlinedFn = CGF.CGM.getOpenMPRuntime().emitTeamsOutlinedFunction(
      S, *CS->getCapturedDecl()->param_begin(), InnermostKind, CodeGen);

  const OMPNumTeamsClause *NT = S.getSingleClause<OMPNumTeamsClause>();
  const OMPThreadLimitClause *TL = S.getSingleClause<OMPThreadLimitClause>();
  if (NT || TL) {
    Expr *NumTeams = (NT) ? NT->getNumTeams() : nullptr;
    Expr *ThreadLimit = (TL) ? TL->getThreadLimit() : nullptr;

    CGF.CGM.getOpenMPRuntime().emitNumTeamsClause(CGF, NumTeams, ThreadLimit,
                                                  S.getLocStart());
  }

  OMPTeamsScope Scope(CGF, S);
  llvm::SmallVector<llvm::Value *, 16> CapturedVars;
  CGF.GenerateOpenMPCapturedVars(*CS, CapturedVars);
  CGF.CGM.getOpenMPRuntime().emitTeamsCall(CGF, S, S.getLocStart(), OutlinedFn,
                                           CapturedVars);
}

void CodeGenFunction::EmitOMPTeamsDirective(const OMPTeamsDirective &S) {
  // Emit teams region as a standalone region.
  auto &&CodeGen = [&S](CodeGenFunction &CGF, PrePostActionTy &) {
    OMPPrivateScope PrivateScope(CGF);
    (void)CGF.EmitOMPFirstprivateClause(S, PrivateScope);
    CGF.EmitOMPPrivateClause(S, PrivateScope);
    CGF.EmitOMPReductionClauseInit(S, PrivateScope);
    (void)PrivateScope.Privatize();
    CGF.EmitStmt(cast<CapturedStmt>(S.getAssociatedStmt())->getCapturedStmt());
    CGF.EmitOMPReductionClauseFinal(S, /*ReductionKind=*/OMPD_teams);
  };
  emitCommonOMPTeamsDirective(*this, S, OMPD_teams, CodeGen);
  emitPostUpdateForReductionClause(
      *this, S, [](CodeGenFunction &) -> llvm::Value * { return nullptr; });
}

static void emitTargetTeamsRegion(CodeGenFunction &CGF, PrePostActionTy &Action,
                                  const OMPTargetTeamsDirective &S) {
  auto *CS = S.getCapturedStmt(OMPD_teams);
  Action.Enter(CGF);
  auto &&CodeGen = [CS](CodeGenFunction &CGF, PrePostActionTy &) {
    // TODO: Add support for clauses.
    CGF.EmitStmt(CS->getCapturedStmt());
  };
  emitCommonOMPTeamsDirective(CGF, S, OMPD_teams, CodeGen);
}

void CodeGenFunction::EmitOMPTargetTeamsDeviceFunction(
    CodeGenModule &CGM, StringRef ParentName,
    const OMPTargetTeamsDirective &S) {
  auto &&CodeGen = [&S](CodeGenFunction &CGF, PrePostActionTy &Action) {
    emitTargetTeamsRegion(CGF, Action, S);
  };
  llvm::Function *Fn;
  llvm::Constant *Addr;
  // Emit target region as a standalone region.
  CGM.getOpenMPRuntime().emitTargetOutlinedFunction(
      S, ParentName, Fn, Addr, /*IsOffloadEntry=*/true, CodeGen);
  assert(Fn && Addr && "Target device function emission failed.");
}

void CodeGenFunction::EmitOMPTargetTeamsDirective(
    const OMPTargetTeamsDirective &S) {
  auto &&CodeGen = [&S](CodeGenFunction &CGF, PrePostActionTy &Action) {
    emitTargetTeamsRegion(CGF, Action, S);
  };
  emitCommonOMPTargetDirective(*this, S, CodeGen);
}

void CodeGenFunction::EmitOMPCancellationPointDirective(
    const OMPCancellationPointDirective &S) {
  CGM.getOpenMPRuntime().emitCancellationPointCall(*this, S.getLocStart(),
                                                   S.getCancelRegion());
}

void CodeGenFunction::EmitOMPCancelDirective(const OMPCancelDirective &S) {
  const Expr *IfCond = nullptr;
  for (const auto *C : S.getClausesOfKind<OMPIfClause>()) {
    if (C->getNameModifier() == OMPD_unknown ||
        C->getNameModifier() == OMPD_cancel) {
      IfCond = C->getCondition();
      break;
    }
  }
  CGM.getOpenMPRuntime().emitCancelCall(*this, S.getLocStart(), IfCond,
                                        S.getCancelRegion());
}

CodeGenFunction::JumpDest
CodeGenFunction::getOMPCancelDestination(OpenMPDirectiveKind Kind) {
  if (Kind == OMPD_parallel || Kind == OMPD_task ||
      Kind == OMPD_target_parallel)
    return ReturnBlock;
  assert(Kind == OMPD_for || Kind == OMPD_section || Kind == OMPD_sections ||
         Kind == OMPD_parallel_sections || Kind == OMPD_parallel_for ||
         Kind == OMPD_distribute_parallel_for ||
         Kind == OMPD_target_parallel_for);
  return OMPCancelStack.getExitBlock();
}

void CodeGenFunction::EmitOMPUseDevicePtrClause(
    const OMPClause &NC, OMPPrivateScope &PrivateScope,
    const llvm::DenseMap<const ValueDecl *, Address> &CaptureDeviceAddrMap) {
  const auto &C = cast<OMPUseDevicePtrClause>(NC);
  auto OrigVarIt = C.varlist_begin();
  auto InitIt = C.inits().begin();
  for (auto PvtVarIt : C.private_copies()) {
    auto *OrigVD = cast<VarDecl>(cast<DeclRefExpr>(*OrigVarIt)->getDecl());
    auto *InitVD = cast<VarDecl>(cast<DeclRefExpr>(*InitIt)->getDecl());
    auto *PvtVD = cast<VarDecl>(cast<DeclRefExpr>(PvtVarIt)->getDecl());

    // In order to identify the right initializer we need to match the
    // declaration used by the mapping logic. In some cases we may get
    // OMPCapturedExprDecl that refers to the original declaration.
    const ValueDecl *MatchingVD = OrigVD;
    if (auto *OED = dyn_cast<OMPCapturedExprDecl>(MatchingVD)) {
      // OMPCapturedExprDecl are used to privative fields of the current
      // structure.
      auto *ME = cast<MemberExpr>(OED->getInit());
      assert(isa<CXXThisExpr>(ME->getBase()) &&
             "Base should be the current struct!");
      MatchingVD = ME->getMemberDecl();
    }

    // If we don't have information about the current list item, move on to
    // the next one.
    auto InitAddrIt = CaptureDeviceAddrMap.find(MatchingVD);
    if (InitAddrIt == CaptureDeviceAddrMap.end())
      continue;

    bool IsRegistered = PrivateScope.addPrivate(OrigVD, [&]() -> Address {
      // Initialize the temporary initialization variable with the address we
      // get from the runtime library. We have to cast the source address
      // because it is always a void *. References are materialized in the
      // privatization scope, so the initialization here disregards the fact
      // the original variable is a reference.
      QualType AddrQTy =
          getContext().getPointerType(OrigVD->getType().getNonReferenceType());
      llvm::Type *AddrTy = ConvertTypeForMem(AddrQTy);
      Address InitAddr = Builder.CreateBitCast(InitAddrIt->second, AddrTy);
      setAddrOfLocalVar(InitVD, InitAddr);

      // Emit private declaration, it will be initialized by the value we
      // declaration we just added to the local declarations map.
      EmitDecl(*PvtVD);

      // The initialization variables reached its purpose in the emission
      // ofthe previous declaration, so we don't need it anymore.
      LocalDeclMap.erase(InitVD);

      // Return the address of the private variable.
      return GetAddrOfLocalVar(PvtVD);
    });
    assert(IsRegistered && "firstprivate var already registered as private");
    // Silence the warning about unused variable.
    (void)IsRegistered;

    ++OrigVarIt;
    ++InitIt;
  }
}

// Generate the instructions for '#pragma omp target data' directive.
void CodeGenFunction::EmitOMPTargetDataDirective(
    const OMPTargetDataDirective &S) {
  CGOpenMPRuntime::TargetDataInfo Info(/*RequiresDevicePointerInfo=*/true);

  // Create a pre/post action to signal the privatization of the device pointer.
  // This action can be replaced by the OpenMP runtime code generation to
  // deactivate privatization.
  bool PrivatizeDevicePointers = false;
  class DevicePointerPrivActionTy : public PrePostActionTy {
    bool &PrivatizeDevicePointers;

  public:
    explicit DevicePointerPrivActionTy(bool &PrivatizeDevicePointers)
        : PrePostActionTy(), PrivatizeDevicePointers(PrivatizeDevicePointers) {}
    void Enter(CodeGenFunction &CGF) override {
      PrivatizeDevicePointers = true;
    }
  };
  DevicePointerPrivActionTy PrivAction(PrivatizeDevicePointers);

  auto &&CodeGen = [&S, &Info, &PrivatizeDevicePointers](
      CodeGenFunction &CGF, PrePostActionTy &Action) {
    auto &&InnermostCodeGen = [&S](CodeGenFunction &CGF, PrePostActionTy &) {
      CGF.EmitStmt(
          cast<CapturedStmt>(S.getAssociatedStmt())->getCapturedStmt());
    };

    // Codegen that selects wheather to generate the privatization code or not.
    auto &&PrivCodeGen = [&S, &Info, &PrivatizeDevicePointers,
                          &InnermostCodeGen](CodeGenFunction &CGF,
                                             PrePostActionTy &Action) {
      RegionCodeGenTy RCG(InnermostCodeGen);
      PrivatizeDevicePointers = false;

      // Call the pre-action to change the status of PrivatizeDevicePointers if
      // needed.
      Action.Enter(CGF);

      if (PrivatizeDevicePointers) {
        OMPPrivateScope PrivateScope(CGF);
        // Emit all instances of the use_device_ptr clause.
        for (const auto *C : S.getClausesOfKind<OMPUseDevicePtrClause>())
          CGF.EmitOMPUseDevicePtrClause(*C, PrivateScope,
                                        Info.CaptureDeviceAddrMap);
        (void)PrivateScope.Privatize();
        RCG(CGF);
      } else
        RCG(CGF);
    };

    // Forward the provided action to the privatization codegen.
    RegionCodeGenTy PrivRCG(PrivCodeGen);
    PrivRCG.setAction(Action);

    // Notwithstanding the body of the region is emitted as inlined directive,
    // we don't use an inline scope as changes in the references inside the
    // region are expected to be visible outside, so we do not privative them.
    OMPLexicalScope Scope(CGF, S);
    CGF.CGM.getOpenMPRuntime().emitInlinedDirective(CGF, OMPD_target_data,
                                                    PrivRCG);
  };

  RegionCodeGenTy RCG(CodeGen);

  // If we don't have target devices, don't bother emitting the data mapping
  // code.
  if (CGM.getLangOpts().OMPTargetTriples.empty()) {
    RCG(*this);
    return;
  }

  // Check if we have any if clause associated with the directive.
  const Expr *IfCond = nullptr;
  if (auto *C = S.getSingleClause<OMPIfClause>())
    IfCond = C->getCondition();

  // Check if we have any device clause associated with the directive.
  const Expr *Device = nullptr;
  if (auto *C = S.getSingleClause<OMPDeviceClause>())
    Device = C->getDevice();

  // Set the action to signal privatization of device pointers.
  RCG.setAction(PrivAction);

  // Emit region code.
  CGM.getOpenMPRuntime().emitTargetDataCalls(*this, S, IfCond, Device, RCG,
                                             Info);
}

void CodeGenFunction::EmitOMPTargetEnterDataDirective(
    const OMPTargetEnterDataDirective &S) {
  // If we don't have target devices, don't bother emitting the data mapping
  // code.
  if (CGM.getLangOpts().OMPTargetTriples.empty())
    return;

  // Check if we have any if clause associated with the directive.
  const Expr *IfCond = nullptr;
  if (auto *C = S.getSingleClause<OMPIfClause>())
    IfCond = C->getCondition();

  // Check if we have any device clause associated with the directive.
  const Expr *Device = nullptr;
  if (auto *C = S.getSingleClause<OMPDeviceClause>())
    Device = C->getDevice();

  CGM.getOpenMPRuntime().emitTargetDataStandAloneCall(*this, S, IfCond, Device);
}

void CodeGenFunction::EmitOMPTargetExitDataDirective(
    const OMPTargetExitDataDirective &S) {
  // If we don't have target devices, don't bother emitting the data mapping
  // code.
  if (CGM.getLangOpts().OMPTargetTriples.empty())
    return;

  // Check if we have any if clause associated with the directive.
  const Expr *IfCond = nullptr;
  if (auto *C = S.getSingleClause<OMPIfClause>())
    IfCond = C->getCondition();

  // Check if we have any device clause associated with the directive.
  const Expr *Device = nullptr;
  if (auto *C = S.getSingleClause<OMPDeviceClause>())
    Device = C->getDevice();

  CGM.getOpenMPRuntime().emitTargetDataStandAloneCall(*this, S, IfCond, Device);
}

static void emitTargetParallelRegion(CodeGenFunction &CGF,
                                     const OMPTargetParallelDirective &S,
                                     PrePostActionTy &Action) {
  // Get the captured statement associated with the 'parallel' region.
  auto *CS = S.getCapturedStmt(OMPD_parallel);
  Action.Enter(CGF);
  auto &&CodeGen = [&S, CS](CodeGenFunction &CGF, PrePostActionTy &) {
    CodeGenFunction::OMPPrivateScope PrivateScope(CGF);
    (void)CGF.EmitOMPFirstprivateClause(S, PrivateScope);
    CGF.EmitOMPPrivateClause(S, PrivateScope);
    CGF.EmitOMPReductionClauseInit(S, PrivateScope);
    (void)PrivateScope.Privatize();
    // TODO: Add support for clauses.
    CGF.EmitStmt(CS->getCapturedStmt());
    CGF.EmitOMPReductionClauseFinal(S, /*ReductionKind=*/OMPD_parallel);
  };
  emitCommonOMPParallelDirective(CGF, S, OMPD_parallel, CodeGen,
                                 emitEmptyBoundParameters);
  emitPostUpdateForReductionClause(
      CGF, S, [](CodeGenFunction &) -> llvm::Value * { return nullptr; });
}

void CodeGenFunction::EmitOMPTargetParallelDeviceFunction(
    CodeGenModule &CGM, StringRef ParentName,
    const OMPTargetParallelDirective &S) {
  auto &&CodeGen = [&S](CodeGenFunction &CGF, PrePostActionTy &Action) {
    emitTargetParallelRegion(CGF, S, Action);
  };
  llvm::Function *Fn;
  llvm::Constant *Addr;
  // Emit target region as a standalone region.
  CGM.getOpenMPRuntime().emitTargetOutlinedFunction(
      S, ParentName, Fn, Addr, /*IsOffloadEntry=*/true, CodeGen);
  assert(Fn && Addr && "Target device function emission failed.");
}

void CodeGenFunction::EmitOMPTargetParallelDirective(
    const OMPTargetParallelDirective &S) {
  auto &&CodeGen = [&S](CodeGenFunction &CGF, PrePostActionTy &Action) {
    emitTargetParallelRegion(CGF, S, Action);
  };
  emitCommonOMPTargetDirective(*this, S, CodeGen);
}

void CodeGenFunction::EmitOMPTargetParallelForDirective(
    const OMPTargetParallelForDirective &S) {
  // TODO: codegen for target parallel for.
}

/// Emit a helper variable and return corresponding lvalue.
static void mapParam(CodeGenFunction &CGF, const DeclRefExpr *Helper,
                     const ImplicitParamDecl *PVD,
                     CodeGenFunction::OMPPrivateScope &Privates) {
  auto *VDecl = cast<VarDecl>(Helper->getDecl());
  Privates.addPrivate(
      VDecl, [&CGF, PVD]() -> Address { return CGF.GetAddrOfLocalVar(PVD); });
}

void CodeGenFunction::EmitOMPTaskLoopBasedDirective(const OMPLoopDirective &S) {
  assert(isOpenMPTaskLoopDirective(S.getDirectiveKind()));
  // Emit outlined function for task construct.
  auto CS = cast<CapturedStmt>(S.getAssociatedStmt());
  auto CapturedStruct = GenerateCapturedStmtArgument(*CS);
  auto SharedsTy = getContext().getRecordType(CS->getCapturedRecordDecl());
  const Expr *IfCond = nullptr;
  for (const auto *C : S.getClausesOfKind<OMPIfClause>()) {
    if (C->getNameModifier() == OMPD_unknown ||
        C->getNameModifier() == OMPD_taskloop) {
      IfCond = C->getCondition();
      break;
    }
  }

  OMPTaskDataTy Data;
  // Check if taskloop must be emitted without taskgroup.
  Data.Nogroup = S.getSingleClause<OMPNogroupClause>();
  // TODO: Check if we should emit tied or untied task.
  Data.Tied = true;
  // Set scheduling for taskloop
  if (const auto* Clause = S.getSingleClause<OMPGrainsizeClause>()) {
    // grainsize clause
    Data.Schedule.setInt(/*IntVal=*/false);
    Data.Schedule.setPointer(EmitScalarExpr(Clause->getGrainsize()));
  } else if (const auto* Clause = S.getSingleClause<OMPNumTasksClause>()) {
    // num_tasks clause
    Data.Schedule.setInt(/*IntVal=*/true);
    Data.Schedule.setPointer(EmitScalarExpr(Clause->getNumTasks()));
  }

  auto &&BodyGen = [CS, &S](CodeGenFunction &CGF, PrePostActionTy &) {
    // if (PreCond) {
    //   for (IV in 0..LastIteration) BODY;
    //   <Final counter/linear vars updates>;
    // }
    //

    // Emit: if (PreCond) - begin.
    // If the condition constant folds and can be elided, avoid emitting the
    // whole loop.
    bool CondConstant;
    llvm::BasicBlock *ContBlock = nullptr;
    OMPLoopScope PreInitScope(CGF, S);
    if (CGF.ConstantFoldsToSimpleInteger(S.getPreCond(), CondConstant)) {
      if (!CondConstant)
        return;
    } else {
      auto *ThenBlock = CGF.createBasicBlock("taskloop.if.then");
      ContBlock = CGF.createBasicBlock("taskloop.if.end");
      emitPreCond(CGF, S, S.getPreCond(), ThenBlock, ContBlock,
                  CGF.getProfileCount(&S));
      CGF.EmitBlock(ThenBlock);
      CGF.incrementProfileCounter(&S);
    }

    if (isOpenMPSimdDirective(S.getDirectiveKind()))
      CGF.EmitOMPSimdInit(S);

    OMPPrivateScope LoopScope(CGF);
    // Emit helper vars inits.
    enum { LowerBound = 5, UpperBound, Stride, LastIter };
    auto *I = CS->getCapturedDecl()->param_begin();
    auto *LBP = std::next(I, LowerBound);
    auto *UBP = std::next(I, UpperBound);
    auto *STP = std::next(I, Stride);
    auto *LIP = std::next(I, LastIter);
    mapParam(CGF, cast<DeclRefExpr>(S.getLowerBoundVariable()), *LBP,
             LoopScope);
    mapParam(CGF, cast<DeclRefExpr>(S.getUpperBoundVariable()), *UBP,
             LoopScope);
    mapParam(CGF, cast<DeclRefExpr>(S.getStrideVariable()), *STP, LoopScope);
    mapParam(CGF, cast<DeclRefExpr>(S.getIsLastIterVariable()), *LIP,
             LoopScope);
    CGF.EmitOMPPrivateLoopCounters(S, LoopScope);
    bool HasLastprivateClause = CGF.EmitOMPLastprivateClauseInit(S, LoopScope);
    (void)LoopScope.Privatize();
    // Emit the loop iteration variable.
    const Expr *IVExpr = S.getIterationVariable();
    const VarDecl *IVDecl = cast<VarDecl>(cast<DeclRefExpr>(IVExpr)->getDecl());
    CGF.EmitVarDecl(*IVDecl);
    CGF.EmitIgnoredExpr(S.getInit());

    // Emit the iterations count variable.
    // If it is not a variable, Sema decided to calculate iterations count on
    // each iteration (e.g., it is foldable into a constant).
    if (auto LIExpr = dyn_cast<DeclRefExpr>(S.getLastIteration())) {
      CGF.EmitVarDecl(*cast<VarDecl>(LIExpr->getDecl()));
      // Emit calculation of the iterations count.
      CGF.EmitIgnoredExpr(S.getCalcLastIteration());
    }

    CGF.EmitOMPInnerLoop(S, LoopScope.requiresCleanups(), S.getCond(),
                         S.getInc(),
                         [&S](CodeGenFunction &CGF) {
                           CGF.EmitOMPLoopBody(S, JumpDest());
                           CGF.EmitStopPoint(&S);
                         },
                         [](CodeGenFunction &) {});
    // Emit: if (PreCond) - end.
    if (ContBlock) {
      CGF.EmitBranch(ContBlock);
      CGF.EmitBlock(ContBlock, true);
    }
    // Emit final copy of the lastprivate variables if IsLastIter != 0.
    if (HasLastprivateClause) {
      CGF.EmitOMPLastprivateClauseFinal(
          S, isOpenMPSimdDirective(S.getDirectiveKind()),
          CGF.Builder.CreateIsNotNull(CGF.EmitLoadOfScalar(
              CGF.GetAddrOfLocalVar(*LIP), /*Volatile=*/false,
              (*LIP)->getType(), S.getLocStart())));
    }
  };
  auto &&TaskGen = [&S, SharedsTy, CapturedStruct,
                    IfCond](CodeGenFunction &CGF, llvm::Value *OutlinedFn,
                            const OMPTaskDataTy &Data) {
    auto &&CodeGen = [&](CodeGenFunction &CGF, PrePostActionTy &) {
      OMPLoopScope PreInitScope(CGF, S);
      CGF.CGM.getOpenMPRuntime().emitTaskLoopCall(CGF, S.getLocStart(), S,
                                                  OutlinedFn, SharedsTy,
                                                  CapturedStruct, IfCond, Data);
    };
    CGF.CGM.getOpenMPRuntime().emitInlinedDirective(CGF, OMPD_taskloop,
                                                    CodeGen);
  };
  if (Data.Nogroup)
    EmitOMPTaskBasedDirective(S, BodyGen, TaskGen, Data);
  else {
    CGM.getOpenMPRuntime().emitTaskgroupRegion(
        *this,
        [&S, &BodyGen, &TaskGen, &Data](CodeGenFunction &CGF,
                                        PrePostActionTy &Action) {
          Action.Enter(CGF);
          CGF.EmitOMPTaskBasedDirective(S, BodyGen, TaskGen, Data);
        },
        S.getLocStart());
  }
}

void CodeGenFunction::EmitOMPTaskLoopDirective(const OMPTaskLoopDirective &S) {
  EmitOMPTaskLoopBasedDirective(S);
}

void CodeGenFunction::EmitOMPTaskLoopSimdDirective(
    const OMPTaskLoopSimdDirective &S) {
  EmitOMPTaskLoopBasedDirective(S);
}

// Generate the instructions for '#pragma omp target update' directive.
void CodeGenFunction::EmitOMPTargetUpdateDirective(
    const OMPTargetUpdateDirective &S) {
  // If we don't have target devices, don't bother emitting the data mapping
  // code.
  if (CGM.getLangOpts().OMPTargetTriples.empty())
    return;

  // Check if we have any if clause associated with the directive.
  const Expr *IfCond = nullptr;
  if (auto *C = S.getSingleClause<OMPIfClause>())
    IfCond = C->getCondition();

  // Check if we have any device clause associated with the directive.
  const Expr *Device = nullptr;
  if (auto *C = S.getSingleClause<OMPDeviceClause>())
    Device = C->getDevice();

  CGM.getOpenMPRuntime().emitTargetDataStandAloneCall(*this, S, IfCond, Device);
}
