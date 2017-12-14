//===----- CGCall.h - Encapsulate calling convention details ----*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// These classes wrap the information about a call or function
// definition used to handle ABI compliancy.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_LIB_CODEGEN_CGCALL_H
#define LLVM_CLANG_LIB_CODEGEN_CGCALL_H

#include "CGValue.h"
#include "EHScopeStack.h"
#include "clang/AST/CanonicalType.h"
#include "clang/AST/Type.h"
#include "llvm/IR/Value.h"

// FIXME: Restructure so we don't have to expose so much stuff.
#include "ABIInfo.h"

namespace llvm {
class AttributeList;
class Function;
class Type;
class Value;
}

namespace clang {
  class ASTContext;
  class Decl;
  class FunctionDecl;
  class ObjCMethodDecl;
  class VarDecl;

namespace CodeGen {

/// Abstract information about a function or function prototype.
class CGCalleeInfo {
  /// \brief The function prototype of the callee.
  const FunctionProtoType *CalleeProtoTy;
  /// \brief The function declaration of the callee.
  const Decl *CalleeDecl;

public:
  explicit CGCalleeInfo() : CalleeProtoTy(nullptr), CalleeDecl(nullptr) {}
  CGCalleeInfo(const FunctionProtoType *calleeProtoTy, const Decl *calleeDecl)
      : CalleeProtoTy(calleeProtoTy), CalleeDecl(calleeDecl) {}
  CGCalleeInfo(const FunctionProtoType *calleeProtoTy)
      : CalleeProtoTy(calleeProtoTy), CalleeDecl(nullptr) {}
  CGCalleeInfo(const Decl *calleeDecl)
      : CalleeProtoTy(nullptr), CalleeDecl(calleeDecl) {}

  const FunctionProtoType *getCalleeFunctionProtoType() const {
    return CalleeProtoTy;
  }
  const Decl *getCalleeDecl() const { return CalleeDecl; }
  };

  /// All available information about a concrete callee.
  class CGCallee {
    enum class SpecialKind : uintptr_t {
      Invalid,
      Builtin,
      PseudoDestructor,

      Last = PseudoDestructor
    };

    struct BuiltinInfoStorage {
      const FunctionDecl *Decl;
      unsigned ID;
    };
    struct PseudoDestructorInfoStorage {
      const CXXPseudoDestructorExpr *Expr;
    };

    SpecialKind KindOrFunctionPointer;
    union {
      CGCalleeInfo AbstractInfo;
      BuiltinInfoStorage BuiltinInfo;
      PseudoDestructorInfoStorage PseudoDestructorInfo;
    };

    explicit CGCallee(SpecialKind kind) : KindOrFunctionPointer(kind) {}

    CGCallee(const FunctionDecl *builtinDecl, unsigned builtinID)
        : KindOrFunctionPointer(SpecialKind::Builtin) {
      BuiltinInfo.Decl = builtinDecl;
      BuiltinInfo.ID = builtinID;
    }

  public:
    CGCallee() : KindOrFunctionPointer(SpecialKind::Invalid) {}

    /// Construct a callee.  Call this constructor directly when this
    /// isn't a direct call.
    CGCallee(const CGCalleeInfo &abstractInfo, llvm::Value *functionPtr)
        : KindOrFunctionPointer(SpecialKind(uintptr_t(functionPtr))) {
      AbstractInfo = abstractInfo;
      assert(functionPtr && "configuring callee without function pointer");
      assert(functionPtr->getType()->isPointerTy());
      assert(functionPtr->getType()->getPointerElementType()->isFunctionTy());
    }

    static CGCallee forBuiltin(unsigned builtinID,
                               const FunctionDecl *builtinDecl) {
      CGCallee result(SpecialKind::Builtin);
      result.BuiltinInfo.Decl = builtinDecl;
      result.BuiltinInfo.ID = builtinID;
      return result;
    }

    static CGCallee forPseudoDestructor(const CXXPseudoDestructorExpr *E) {
      CGCallee result(SpecialKind::PseudoDestructor);
      result.PseudoDestructorInfo.Expr = E;
      return result;
    }

    static CGCallee forDirect(llvm::Constant *functionPtr,
                        const CGCalleeInfo &abstractInfo = CGCalleeInfo()) {
      return CGCallee(abstractInfo, functionPtr);
    }

    bool isBuiltin() const {
      return KindOrFunctionPointer == SpecialKind::Builtin;
    }
    const FunctionDecl *getBuiltinDecl() const {
      assert(isBuiltin());
      return BuiltinInfo.Decl;
    }
    unsigned getBuiltinID() const {
      assert(isBuiltin());
      return BuiltinInfo.ID;
    }

    bool isPseudoDestructor() const {
      return KindOrFunctionPointer == SpecialKind::PseudoDestructor;
    }
    const CXXPseudoDestructorExpr *getPseudoDestructorExpr() const {
      assert(isPseudoDestructor());
      return PseudoDestructorInfo.Expr;
    }

    bool isOrdinary() const {
      return uintptr_t(KindOrFunctionPointer) > uintptr_t(SpecialKind::Last);
    }
    const CGCalleeInfo &getAbstractInfo() const {
      assert(isOrdinary());
      return AbstractInfo;
    }
    llvm::Value *getFunctionPointer() const {
      assert(isOrdinary());
      return reinterpret_cast<llvm::Value*>(uintptr_t(KindOrFunctionPointer));
    }
    llvm::FunctionType *getFunctionType() const {
      return cast<llvm::FunctionType>(
                    getFunctionPointer()->getType()->getPointerElementType());
    }
    void setFunctionPointer(llvm::Value *functionPtr) {
      assert(isOrdinary());
      KindOrFunctionPointer = SpecialKind(uintptr_t(functionPtr));
    }
  };

  struct CallArg {
    RValue RV;
    QualType Ty;
    bool NeedsCopy;
    CallArg(RValue rv, QualType ty, bool needscopy)
    : RV(rv), Ty(ty), NeedsCopy(needscopy)
    { }
  };

  /// CallArgList - Type for representing both the value and type of
  /// arguments in a call.
  class CallArgList :
    public SmallVector<CallArg, 16> {
  public:
    CallArgList() : StackBase(nullptr) {}

    struct Writeback {
      /// The original argument.  Note that the argument l-value
      /// is potentially null.
      LValue Source;

      /// The temporary alloca.
      Address Temporary;

      /// A value to "use" after the writeback, or null.
      llvm::Value *ToUse;
    };

    struct CallArgCleanup {
      EHScopeStack::stable_iterator Cleanup;

      /// The "is active" insertion point.  This instruction is temporary and
      /// will be removed after insertion.
      llvm::Instruction *IsActiveIP;
    };

    void add(RValue rvalue, QualType type, bool needscopy = false) {
      push_back(CallArg(rvalue, type, needscopy));
    }

    /// Add all the arguments from another CallArgList to this one. After doing
    /// this, the old CallArgList retains its list of arguments, but must not
    /// be used to emit a call.
    void addFrom(const CallArgList &other) {
      insert(end(), other.begin(), other.end());
      Writebacks.insert(Writebacks.end(),
                        other.Writebacks.begin(), other.Writebacks.end());
      CleanupsToDeactivate.insert(CleanupsToDeactivate.end(),
                                  other.CleanupsToDeactivate.begin(),
                                  other.CleanupsToDeactivate.end());
      assert(!(StackBase && other.StackBase) && "can't merge stackbases");
      if (!StackBase)
        StackBase = other.StackBase;
    }

    void addWriteback(LValue srcLV, Address temporary,
                      llvm::Value *toUse) {
      Writeback writeback = { srcLV, temporary, toUse };
      Writebacks.push_back(writeback);
    }

    bool hasWritebacks() const { return !Writebacks.empty(); }

    typedef llvm::iterator_range<SmallVectorImpl<Writeback>::const_iterator>
      writeback_const_range;

    writeback_const_range writebacks() const {
      return writeback_const_range(Writebacks.begin(), Writebacks.end());
    }

    void addArgCleanupDeactivation(EHScopeStack::stable_iterator Cleanup,
                                   llvm::Instruction *IsActiveIP) {
      CallArgCleanup ArgCleanup;
      ArgCleanup.Cleanup = Cleanup;
      ArgCleanup.IsActiveIP = IsActiveIP;
      CleanupsToDeactivate.push_back(ArgCleanup);
    }

    ArrayRef<CallArgCleanup> getCleanupsToDeactivate() const {
      return CleanupsToDeactivate;
    }

    void allocateArgumentMemory(CodeGenFunction &CGF);
    llvm::Instruction *getStackBase() const { return StackBase; }
    void freeArgumentMemory(CodeGenFunction &CGF) const;

    /// \brief Returns if we're using an inalloca struct to pass arguments in
    /// memory.
    bool isUsingInAlloca() const { return StackBase; }

  private:
    SmallVector<Writeback, 1> Writebacks;

    /// Deactivate these cleanups immediately before making the call.  This
    /// is used to cleanup objects that are owned by the callee once the call
    /// occurs.
    SmallVector<CallArgCleanup, 1> CleanupsToDeactivate;

    /// The stacksave call.  It dominates all of the argument evaluation.
    llvm::CallInst *StackBase;
  };

  /// FunctionArgList - Type for representing both the decl and type
  /// of parameters to a function. The decl must be either a
  /// ParmVarDecl or ImplicitParamDecl.
  class FunctionArgList : public SmallVector<const VarDecl*, 16> {
  };

  /// ReturnValueSlot - Contains the address where the return value of a 
  /// function can be stored, and whether the address is volatile or not.
  class ReturnValueSlot {
    llvm::PointerIntPair<llvm::Value *, 2, unsigned int> Value;
    CharUnits Alignment;

    // Return value slot flags
    enum Flags {
      IS_VOLATILE = 0x1,
      IS_UNUSED = 0x2,
    };

  public:
    ReturnValueSlot() {}
    ReturnValueSlot(Address Addr, bool IsVolatile, bool IsUnused = false)
      : Value(Addr.isValid() ? Addr.getPointer() : nullptr,
              (IsVolatile ? IS_VOLATILE : 0) | (IsUnused ? IS_UNUSED : 0)),
        Alignment(Addr.isValid() ? Addr.getAlignment() : CharUnits::Zero()) {}

    bool isNull() const { return !getValue().isValid(); }

    bool isVolatile() const { return Value.getInt() & IS_VOLATILE; }
    Address getValue() const { return Address(Value.getPointer(), Alignment); }
    bool isUnused() const { return Value.getInt() & IS_UNUSED; }
  };
  
}  // end namespace CodeGen
}  // end namespace clang

#endif
