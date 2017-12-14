//===------- CGObjCGNU.cpp - Emit LLVM Code from ASTs for a Module --------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This provides Objective-C code generation targeting the GNU runtime.  The
// class in this file generates structures used by the GNU Objective-C runtime
// library.  These structures are defined in objc/objc.h and objc/objc-api.h in
// the GNU runtime distribution.
//
//===----------------------------------------------------------------------===//

#include "CGObjCRuntime.h"
#include "CGCleanup.h"
#include "CodeGenFunction.h"
#include "CodeGenModule.h"
#include "clang/CodeGen/ConstantInitBuilder.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/Decl.h"
#include "clang/AST/DeclObjC.h"
#include "clang/AST/RecordLayout.h"
#include "clang/AST/StmtObjC.h"
#include "clang/Basic/FileManager.h"
#include "clang/Basic/SourceManager.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/IR/CallSite.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/Compiler.h"

using namespace clang;
using namespace CodeGen;

namespace {
/// Class that lazily initialises the runtime function.  Avoids inserting the
/// types and the function declaration into a module if they're not used, and
/// avoids constructing the type more than once if it's used more than once.
class LazyRuntimeFunction {
  CodeGenModule *CGM;
  llvm::FunctionType *FTy;
  const char *FunctionName;
  llvm::Constant *Function;

public:
  /// Constructor leaves this class uninitialized, because it is intended to
  /// be used as a field in another class and not all of the types that are
  /// used as arguments will necessarily be available at construction time.
  LazyRuntimeFunction()
      : CGM(nullptr), FunctionName(nullptr), Function(nullptr) {}

  /// Initialises the lazy function with the name, return type, and the types
  /// of the arguments.
  template <typename... Tys>
  void init(CodeGenModule *Mod, const char *name, llvm::Type *RetTy,
            Tys *... Types) {
    CGM = Mod;
    FunctionName = name;
    Function = nullptr;
    if(sizeof...(Tys)) {
      SmallVector<llvm::Type *, 8> ArgTys({Types...});
      FTy = llvm::FunctionType::get(RetTy, ArgTys, false);
    }
    else {
      FTy = llvm::FunctionType::get(RetTy, None, false);
    }
  }

  llvm::FunctionType *getType() { return FTy; }

  /// Overloaded cast operator, allows the class to be implicitly cast to an
  /// LLVM constant.
  operator llvm::Constant *() {
    if (!Function) {
      if (!FunctionName)
        return nullptr;
      Function =
          cast<llvm::Constant>(CGM->CreateRuntimeFunction(FTy, FunctionName));
    }
    return Function;
  }
  operator llvm::Function *() {
    return cast<llvm::Function>((llvm::Constant *)*this);
  }
};


/// GNU Objective-C runtime code generation.  This class implements the parts of
/// Objective-C support that are specific to the GNU family of runtimes (GCC,
/// GNUstep and ObjFW).
class CGObjCGNU : public CGObjCRuntime {
protected:
  /// The LLVM module into which output is inserted
  llvm::Module &TheModule;
  /// strut objc_super.  Used for sending messages to super.  This structure
  /// contains the receiver (object) and the expected class.
  llvm::StructType *ObjCSuperTy;
  /// struct objc_super*.  The type of the argument to the superclass message
  /// lookup functions.  
  llvm::PointerType *PtrToObjCSuperTy;
  /// LLVM type for selectors.  Opaque pointer (i8*) unless a header declaring
  /// SEL is included in a header somewhere, in which case it will be whatever
  /// type is declared in that header, most likely {i8*, i8*}.
  llvm::PointerType *SelectorTy;
  /// LLVM i8 type.  Cached here to avoid repeatedly getting it in all of the
  /// places where it's used
  llvm::IntegerType *Int8Ty;
  /// Pointer to i8 - LLVM type of char*, for all of the places where the
  /// runtime needs to deal with C strings.
  llvm::PointerType *PtrToInt8Ty;
  /// Instance Method Pointer type.  This is a pointer to a function that takes,
  /// at a minimum, an object and a selector, and is the generic type for
  /// Objective-C methods.  Due to differences between variadic / non-variadic
  /// calling conventions, it must always be cast to the correct type before
  /// actually being used.
  llvm::PointerType *IMPTy;
  /// Type of an untyped Objective-C object.  Clang treats id as a built-in type
  /// when compiling Objective-C code, so this may be an opaque pointer (i8*),
  /// but if the runtime header declaring it is included then it may be a
  /// pointer to a structure.
  llvm::PointerType *IdTy;
  /// Pointer to a pointer to an Objective-C object.  Used in the new ABI
  /// message lookup function and some GC-related functions.
  llvm::PointerType *PtrToIdTy;
  /// The clang type of id.  Used when using the clang CGCall infrastructure to
  /// call Objective-C methods.
  CanQualType ASTIdTy;
  /// LLVM type for C int type.
  llvm::IntegerType *IntTy;
  /// LLVM type for an opaque pointer.  This is identical to PtrToInt8Ty, but is
  /// used in the code to document the difference between i8* meaning a pointer
  /// to a C string and i8* meaning a pointer to some opaque type.
  llvm::PointerType *PtrTy;
  /// LLVM type for C long type.  The runtime uses this in a lot of places where
  /// it should be using intptr_t, but we can't fix this without breaking
  /// compatibility with GCC...
  llvm::IntegerType *LongTy;
  /// LLVM type for C size_t.  Used in various runtime data structures.
  llvm::IntegerType *SizeTy;
  /// LLVM type for C intptr_t.  
  llvm::IntegerType *IntPtrTy;
  /// LLVM type for C ptrdiff_t.  Mainly used in property accessor functions.
  llvm::IntegerType *PtrDiffTy;
  /// LLVM type for C int*.  Used for GCC-ABI-compatible non-fragile instance
  /// variables.
  llvm::PointerType *PtrToIntTy;
  /// LLVM type for Objective-C BOOL type.
  llvm::Type *BoolTy;
  /// 32-bit integer type, to save us needing to look it up every time it's used.
  llvm::IntegerType *Int32Ty;
  /// 64-bit integer type, to save us needing to look it up every time it's used.
  llvm::IntegerType *Int64Ty;
  /// Metadata kind used to tie method lookups to message sends.  The GNUstep
  /// runtime provides some LLVM passes that can use this to do things like
  /// automatic IMP caching and speculative inlining.
  unsigned msgSendMDKind;

  /// Helper function that generates a constant string and returns a pointer to
  /// the start of the string.  The result of this function can be used anywhere
  /// where the C code specifies const char*.  
  llvm::Constant *MakeConstantString(StringRef Str, const char *Name = "") {
    ConstantAddress Array = CGM.GetAddrOfConstantCString(Str, Name);
    return llvm::ConstantExpr::getGetElementPtr(Array.getElementType(),
                                                Array.getPointer(), Zeros);
  }

  /// Emits a linkonce_odr string, whose name is the prefix followed by the
  /// string value.  This allows the linker to combine the strings between
  /// different modules.  Used for EH typeinfo names, selector strings, and a
  /// few other things.
  llvm::Constant *ExportUniqueString(const std::string &Str, StringRef Prefix) {
    std::string Name = Prefix.str() + Str;
    auto *ConstStr = TheModule.getGlobalVariable(Name);
    if (!ConstStr) {
      llvm::Constant *value = llvm::ConstantDataArray::getString(VMContext,Str);
      ConstStr = new llvm::GlobalVariable(TheModule, value->getType(), true,
                                          llvm::GlobalValue::LinkOnceODRLinkage,
                                          value, Name);
    }
    return llvm::ConstantExpr::getGetElementPtr(ConstStr->getValueType(),
                                                ConstStr, Zeros);
  }

  /// Generates a global structure, initialized by the elements in the vector.
  /// The element types must match the types of the structure elements in the
  /// first argument.
  llvm::GlobalVariable *MakeGlobal(llvm::Constant *C,
                                   CharUnits Align,
                                   StringRef Name="",
                                   llvm::GlobalValue::LinkageTypes linkage
                                         =llvm::GlobalValue::InternalLinkage) {
    auto GV = new llvm::GlobalVariable(TheModule, C->getType(), false,
                                       linkage, C, Name);
    GV->setAlignment(Align.getQuantity());
    return GV;
  }

  /// Returns a property name and encoding string.
  llvm::Constant *MakePropertyEncodingString(const ObjCPropertyDecl *PD,
                                             const Decl *Container) {
    const ObjCRuntime &R = CGM.getLangOpts().ObjCRuntime;
    if ((R.getKind() == ObjCRuntime::GNUstep) &&
        (R.getVersion() >= VersionTuple(1, 6))) {
      std::string NameAndAttributes;
      std::string TypeStr =
        CGM.getContext().getObjCEncodingForPropertyDecl(PD, Container);
      NameAndAttributes += '\0';
      NameAndAttributes += TypeStr.length() + 3;
      NameAndAttributes += TypeStr;
      NameAndAttributes += '\0';
      NameAndAttributes += PD->getNameAsString();
      return MakeConstantString(NameAndAttributes);
    }
    return MakeConstantString(PD->getNameAsString());
  }

  /// Push the property attributes into two structure fields. 
  void PushPropertyAttributes(ConstantStructBuilder &Fields,
      ObjCPropertyDecl *property, bool isSynthesized=true, bool
      isDynamic=true) {
    int attrs = property->getPropertyAttributes();
    // For read-only properties, clear the copy and retain flags
    if (attrs & ObjCPropertyDecl::OBJC_PR_readonly) {
      attrs &= ~ObjCPropertyDecl::OBJC_PR_copy;
      attrs &= ~ObjCPropertyDecl::OBJC_PR_retain;
      attrs &= ~ObjCPropertyDecl::OBJC_PR_weak;
      attrs &= ~ObjCPropertyDecl::OBJC_PR_strong;
    }
    // The first flags field has the same attribute values as clang uses internally
    Fields.addInt(Int8Ty, attrs & 0xff);
    attrs >>= 8;
    attrs <<= 2;
    // For protocol properties, synthesized and dynamic have no meaning, so we
    // reuse these flags to indicate that this is a protocol property (both set
    // has no meaning, as a property can't be both synthesized and dynamic)
    attrs |= isSynthesized ? (1<<0) : 0;
    attrs |= isDynamic ? (1<<1) : 0;
    // The second field is the next four fields left shifted by two, with the
    // low bit set to indicate whether the field is synthesized or dynamic.
    Fields.addInt(Int8Ty, attrs & 0xff);
    // Two padding fields
    Fields.addInt(Int8Ty, 0);
    Fields.addInt(Int8Ty, 0);
  }

  /// Ensures that the value has the required type, by inserting a bitcast if
  /// required.  This function lets us avoid inserting bitcasts that are
  /// redundant.
  llvm::Value* EnforceType(CGBuilderTy &B, llvm::Value *V, llvm::Type *Ty) {
    if (V->getType() == Ty) return V;
    return B.CreateBitCast(V, Ty);
  }
  Address EnforceType(CGBuilderTy &B, Address V, llvm::Type *Ty) {
    if (V.getType() == Ty) return V;
    return B.CreateBitCast(V, Ty);
  }

  // Some zeros used for GEPs in lots of places.
  llvm::Constant *Zeros[2];
  /// Null pointer value.  Mainly used as a terminator in various arrays.
  llvm::Constant *NULLPtr;
  /// LLVM context.
  llvm::LLVMContext &VMContext;

private:
  /// Placeholder for the class.  Lots of things refer to the class before we've
  /// actually emitted it.  We use this alias as a placeholder, and then replace
  /// it with a pointer to the class structure before finally emitting the
  /// module.
  llvm::GlobalAlias *ClassPtrAlias;
  /// Placeholder for the metaclass.  Lots of things refer to the class before
  /// we've / actually emitted it.  We use this alias as a placeholder, and then
  /// replace / it with a pointer to the metaclass structure before finally
  /// emitting the / module.
  llvm::GlobalAlias *MetaClassPtrAlias;
  /// All of the classes that have been generated for this compilation units.
  std::vector<llvm::Constant*> Classes;
  /// All of the categories that have been generated for this compilation units.
  std::vector<llvm::Constant*> Categories;
  /// All of the Objective-C constant strings that have been generated for this
  /// compilation units.
  std::vector<llvm::Constant*> ConstantStrings;
  /// Map from string values to Objective-C constant strings in the output.
  /// Used to prevent emitting Objective-C strings more than once.  This should
  /// not be required at all - CodeGenModule should manage this list.
  llvm::StringMap<llvm::Constant*> ObjCStrings;
  /// All of the protocols that have been declared.
  llvm::StringMap<llvm::Constant*> ExistingProtocols;
  /// For each variant of a selector, we store the type encoding and a
  /// placeholder value.  For an untyped selector, the type will be the empty
  /// string.  Selector references are all done via the module's selector table,
  /// so we create an alias as a placeholder and then replace it with the real
  /// value later.
  typedef std::pair<std::string, llvm::GlobalAlias*> TypedSelector;
  /// Type of the selector map.  This is roughly equivalent to the structure
  /// used in the GNUstep runtime, which maintains a list of all of the valid
  /// types for a selector in a table.
  typedef llvm::DenseMap<Selector, SmallVector<TypedSelector, 2> >
    SelectorMap;
  /// A map from selectors to selector types.  This allows us to emit all
  /// selectors of the same name and type together.
  SelectorMap SelectorTable;

  /// Selectors related to memory management.  When compiling in GC mode, we
  /// omit these.
  Selector RetainSel, ReleaseSel, AutoreleaseSel;
  /// Runtime functions used for memory management in GC mode.  Note that clang
  /// supports code generation for calling these functions, but neither GNU
  /// runtime actually supports this API properly yet.
  LazyRuntimeFunction IvarAssignFn, StrongCastAssignFn, MemMoveFn, WeakReadFn, 
    WeakAssignFn, GlobalAssignFn;

  typedef std::pair<std::string, std::string> ClassAliasPair;
  /// All classes that have aliases set for them.
  std::vector<ClassAliasPair> ClassAliases;

protected:
  /// Function used for throwing Objective-C exceptions.
  LazyRuntimeFunction ExceptionThrowFn;
  /// Function used for rethrowing exceptions, used at the end of \@finally or
  /// \@synchronize blocks.
  LazyRuntimeFunction ExceptionReThrowFn;
  /// Function called when entering a catch function.  This is required for
  /// differentiating Objective-C exceptions and foreign exceptions.
  LazyRuntimeFunction EnterCatchFn;
  /// Function called when exiting from a catch block.  Used to do exception
  /// cleanup.
  LazyRuntimeFunction ExitCatchFn;
  /// Function called when entering an \@synchronize block.  Acquires the lock.
  LazyRuntimeFunction SyncEnterFn;
  /// Function called when exiting an \@synchronize block.  Releases the lock.
  LazyRuntimeFunction SyncExitFn;

private:
  /// Function called if fast enumeration detects that the collection is
  /// modified during the update.
  LazyRuntimeFunction EnumerationMutationFn;
  /// Function for implementing synthesized property getters that return an
  /// object.
  LazyRuntimeFunction GetPropertyFn;
  /// Function for implementing synthesized property setters that return an
  /// object.
  LazyRuntimeFunction SetPropertyFn;
  /// Function used for non-object declared property getters.
  LazyRuntimeFunction GetStructPropertyFn;
  /// Function used for non-object declared property setters.
  LazyRuntimeFunction SetStructPropertyFn;

  /// The version of the runtime that this class targets.  Must match the
  /// version in the runtime.
  int RuntimeVersion;
  /// The version of the protocol class.  Used to differentiate between ObjC1
  /// and ObjC2 protocols.  Objective-C 1 protocols can not contain optional
  /// components and can not contain declared properties.  We always emit
  /// Objective-C 2 property structures, but we have to pretend that they're
  /// Objective-C 1 property structures when targeting the GCC runtime or it
  /// will abort.
  const int ProtocolVersion;

  /// Generates an instance variable list structure.  This is a structure
  /// containing a size and an array of structures containing instance variable
  /// metadata.  This is used purely for introspection in the fragile ABI.  In
  /// the non-fragile ABI, it's used for instance variable fixup.
  llvm::Constant *GenerateIvarList(ArrayRef<llvm::Constant *> IvarNames,
                                   ArrayRef<llvm::Constant *> IvarTypes,
                                   ArrayRef<llvm::Constant *> IvarOffsets);

  /// Generates a method list structure.  This is a structure containing a size
  /// and an array of structures containing method metadata.
  ///
  /// This structure is used by both classes and categories, and contains a next
  /// pointer allowing them to be chained together in a linked list.
  llvm::Constant *GenerateMethodList(StringRef ClassName,
      StringRef CategoryName,
      ArrayRef<Selector> MethodSels,
      ArrayRef<llvm::Constant *> MethodTypes,
      bool isClassMethodList);

  /// Emits an empty protocol.  This is used for \@protocol() where no protocol
  /// is found.  The runtime will (hopefully) fix up the pointer to refer to the
  /// real protocol.
  llvm::Constant *GenerateEmptyProtocol(const std::string &ProtocolName);

  /// Generates a list of property metadata structures.  This follows the same
  /// pattern as method and instance variable metadata lists.
  llvm::Constant *GeneratePropertyList(const ObjCImplementationDecl *OID,
        SmallVectorImpl<Selector> &InstanceMethodSels,
        SmallVectorImpl<llvm::Constant*> &InstanceMethodTypes);

  /// Generates a list of referenced protocols.  Classes, categories, and
  /// protocols all use this structure.
  llvm::Constant *GenerateProtocolList(ArrayRef<std::string> Protocols);

  /// To ensure that all protocols are seen by the runtime, we add a category on
  /// a class defined in the runtime, declaring no methods, but adopting the
  /// protocols.  This is a horribly ugly hack, but it allows us to collect all
  /// of the protocols without changing the ABI.
  void GenerateProtocolHolderCategory();

  /// Generates a class structure.
  llvm::Constant *GenerateClassStructure(
      llvm::Constant *MetaClass,
      llvm::Constant *SuperClass,
      unsigned info,
      const char *Name,
      llvm::Constant *Version,
      llvm::Constant *InstanceSize,
      llvm::Constant *IVars,
      llvm::Constant *Methods,
      llvm::Constant *Protocols,
      llvm::Constant *IvarOffsets,
      llvm::Constant *Properties,
      llvm::Constant *StrongIvarBitmap,
      llvm::Constant *WeakIvarBitmap,
      bool isMeta=false);

  /// Generates a method list.  This is used by protocols to define the required
  /// and optional methods.
  llvm::Constant *GenerateProtocolMethodList(
      ArrayRef<llvm::Constant *> MethodNames,
      ArrayRef<llvm::Constant *> MethodTypes);

  /// Returns a selector with the specified type encoding.  An empty string is
  /// used to return an untyped selector (with the types field set to NULL).
  llvm::Value *GetSelector(CodeGenFunction &CGF, Selector Sel,
                           const std::string &TypeEncoding);

  /// Returns the variable used to store the offset of an instance variable.
  llvm::GlobalVariable *ObjCIvarOffsetVariable(const ObjCInterfaceDecl *ID,
      const ObjCIvarDecl *Ivar);
  /// Emits a reference to a class.  This allows the linker to object if there
  /// is no class of the matching name.

protected:
  void EmitClassRef(const std::string &className);

  /// Emits a pointer to the named class
  virtual llvm::Value *GetClassNamed(CodeGenFunction &CGF,
                                     const std::string &Name, bool isWeak);

  /// Looks up the method for sending a message to the specified object.  This
  /// mechanism differs between the GCC and GNU runtimes, so this method must be
  /// overridden in subclasses.
  virtual llvm::Value *LookupIMP(CodeGenFunction &CGF,
                                 llvm::Value *&Receiver,
                                 llvm::Value *cmd,
                                 llvm::MDNode *node,
                                 MessageSendInfo &MSI) = 0;

  /// Looks up the method for sending a message to a superclass.  This
  /// mechanism differs between the GCC and GNU runtimes, so this method must
  /// be overridden in subclasses.
  virtual llvm::Value *LookupIMPSuper(CodeGenFunction &CGF,
                                      Address ObjCSuper,
                                      llvm::Value *cmd,
                                      MessageSendInfo &MSI) = 0;

  /// Libobjc2 uses a bitfield representation where small(ish) bitfields are
  /// stored in a 64-bit value with the low bit set to 1 and the remaining 63
  /// bits set to their values, LSB first, while larger ones are stored in a
  /// structure of this / form:
  /// 
  /// struct { int32_t length; int32_t values[length]; };
  ///
  /// The values in the array are stored in host-endian format, with the least
  /// significant bit being assumed to come first in the bitfield.  Therefore,
  /// a bitfield with the 64th bit set will be (int64_t)&{ 2, [0, 1<<31] },
  /// while a bitfield / with the 63rd bit set will be 1<<64.
  llvm::Constant *MakeBitField(ArrayRef<bool> bits);

public:
  CGObjCGNU(CodeGenModule &cgm, unsigned runtimeABIVersion,
      unsigned protocolClassVersion);

  ConstantAddress GenerateConstantString(const StringLiteral *) override;

  RValue
  GenerateMessageSend(CodeGenFunction &CGF, ReturnValueSlot Return,
                      QualType ResultType, Selector Sel,
                      llvm::Value *Receiver, const CallArgList &CallArgs,
                      const ObjCInterfaceDecl *Class,
                      const ObjCMethodDecl *Method) override;
  RValue
  GenerateMessageSendSuper(CodeGenFunction &CGF, ReturnValueSlot Return,
                           QualType ResultType, Selector Sel,
                           const ObjCInterfaceDecl *Class,
                           bool isCategoryImpl, llvm::Value *Receiver,
                           bool IsClassMessage, const CallArgList &CallArgs,
                           const ObjCMethodDecl *Method) override;
  llvm::Value *GetClass(CodeGenFunction &CGF,
                        const ObjCInterfaceDecl *OID) override;
  llvm::Value *GetSelector(CodeGenFunction &CGF, Selector Sel) override;
  Address GetAddrOfSelector(CodeGenFunction &CGF, Selector Sel) override;
  llvm::Value *GetSelector(CodeGenFunction &CGF,
                           const ObjCMethodDecl *Method) override;
  llvm::Constant *GetEHType(QualType T) override;

  llvm::Function *GenerateMethod(const ObjCMethodDecl *OMD,
                                 const ObjCContainerDecl *CD) override;
  void GenerateCategory(const ObjCCategoryImplDecl *CMD) override;
  void GenerateClass(const ObjCImplementationDecl *ClassDecl) override;
  void RegisterAlias(const ObjCCompatibleAliasDecl *OAD) override;
  llvm::Value *GenerateProtocolRef(CodeGenFunction &CGF,
                                   const ObjCProtocolDecl *PD) override;
  void GenerateProtocol(const ObjCProtocolDecl *PD) override;
  llvm::Function *ModuleInitFunction() override;
  llvm::Constant *GetPropertyGetFunction() override;
  llvm::Constant *GetPropertySetFunction() override;
  llvm::Constant *GetOptimizedPropertySetFunction(bool atomic,
                                                  bool copy) override;
  llvm::Constant *GetSetStructFunction() override;
  llvm::Constant *GetGetStructFunction() override;
  llvm::Constant *GetCppAtomicObjectGetFunction() override;
  llvm::Constant *GetCppAtomicObjectSetFunction() override;
  llvm::Constant *EnumerationMutationFunction() override;

  void EmitTryStmt(CodeGenFunction &CGF,
                   const ObjCAtTryStmt &S) override;
  void EmitSynchronizedStmt(CodeGenFunction &CGF,
                            const ObjCAtSynchronizedStmt &S) override;
  void EmitThrowStmt(CodeGenFunction &CGF,
                     const ObjCAtThrowStmt &S,
                     bool ClearInsertionPoint=true) override;
  llvm::Value * EmitObjCWeakRead(CodeGenFunction &CGF,
                                 Address AddrWeakObj) override;
  void EmitObjCWeakAssign(CodeGenFunction &CGF,
                          llvm::Value *src, Address dst) override;
  void EmitObjCGlobalAssign(CodeGenFunction &CGF,
                            llvm::Value *src, Address dest,
                            bool threadlocal=false) override;
  void EmitObjCIvarAssign(CodeGenFunction &CGF, llvm::Value *src,
                          Address dest, llvm::Value *ivarOffset) override;
  void EmitObjCStrongCastAssign(CodeGenFunction &CGF,
                                llvm::Value *src, Address dest) override;
  void EmitGCMemmoveCollectable(CodeGenFunction &CGF, Address DestPtr,
                                Address SrcPtr,
                                llvm::Value *Size) override;
  LValue EmitObjCValueForIvar(CodeGenFunction &CGF, QualType ObjectTy,
                              llvm::Value *BaseValue, const ObjCIvarDecl *Ivar,
                              unsigned CVRQualifiers) override;
  llvm::Value *EmitIvarOffset(CodeGenFunction &CGF,
                              const ObjCInterfaceDecl *Interface,
                              const ObjCIvarDecl *Ivar) override;
  llvm::Value *EmitNSAutoreleasePoolClassRef(CodeGenFunction &CGF) override;
  llvm::Constant *BuildGCBlockLayout(CodeGenModule &CGM,
                                     const CGBlockInfo &blockInfo) override {
    return NULLPtr;
  }
  llvm::Constant *BuildRCBlockLayout(CodeGenModule &CGM,
                                     const CGBlockInfo &blockInfo) override {
    return NULLPtr;
  }

  llvm::Constant *BuildByrefLayout(CodeGenModule &CGM, QualType T) override {
    return NULLPtr;
  }
};

/// Class representing the legacy GCC Objective-C ABI.  This is the default when
/// -fobjc-nonfragile-abi is not specified.
///
/// The GCC ABI target actually generates code that is approximately compatible
/// with the new GNUstep runtime ABI, but refrains from using any features that
/// would not work with the GCC runtime.  For example, clang always generates
/// the extended form of the class structure, and the extra fields are simply
/// ignored by GCC libobjc.
class CGObjCGCC : public CGObjCGNU {
  /// The GCC ABI message lookup function.  Returns an IMP pointing to the
  /// method implementation for this message.
  LazyRuntimeFunction MsgLookupFn;
  /// The GCC ABI superclass message lookup function.  Takes a pointer to a
  /// structure describing the receiver and the class, and a selector as
  /// arguments.  Returns the IMP for the corresponding method.
  LazyRuntimeFunction MsgLookupSuperFn;

protected:
  llvm::Value *LookupIMP(CodeGenFunction &CGF, llvm::Value *&Receiver,
                         llvm::Value *cmd, llvm::MDNode *node,
                         MessageSendInfo &MSI) override {
    CGBuilderTy &Builder = CGF.Builder;
    llvm::Value *args[] = {
            EnforceType(Builder, Receiver, IdTy),
            EnforceType(Builder, cmd, SelectorTy) };
    llvm::CallSite imp = CGF.EmitRuntimeCallOrInvoke(MsgLookupFn, args);
    imp->setMetadata(msgSendMDKind, node);
    return imp.getInstruction();
  }

  llvm::Value *LookupIMPSuper(CodeGenFunction &CGF, Address ObjCSuper,
                              llvm::Value *cmd, MessageSendInfo &MSI) override {
    CGBuilderTy &Builder = CGF.Builder;
    llvm::Value *lookupArgs[] = {EnforceType(Builder, ObjCSuper,
        PtrToObjCSuperTy).getPointer(), cmd};
    return CGF.EmitNounwindRuntimeCall(MsgLookupSuperFn, lookupArgs);
  }

public:
  CGObjCGCC(CodeGenModule &Mod) : CGObjCGNU(Mod, 8, 2) {
    // IMP objc_msg_lookup(id, SEL);
    MsgLookupFn.init(&CGM, "objc_msg_lookup", IMPTy, IdTy, SelectorTy);
    // IMP objc_msg_lookup_super(struct objc_super*, SEL);
    MsgLookupSuperFn.init(&CGM, "objc_msg_lookup_super", IMPTy,
                          PtrToObjCSuperTy, SelectorTy);
  }
};

/// Class used when targeting the new GNUstep runtime ABI.
class CGObjCGNUstep : public CGObjCGNU {
    /// The slot lookup function.  Returns a pointer to a cacheable structure
    /// that contains (among other things) the IMP.
    LazyRuntimeFunction SlotLookupFn;
    /// The GNUstep ABI superclass message lookup function.  Takes a pointer to
    /// a structure describing the receiver and the class, and a selector as
    /// arguments.  Returns the slot for the corresponding method.  Superclass
    /// message lookup rarely changes, so this is a good caching opportunity.
    LazyRuntimeFunction SlotLookupSuperFn;
    /// Specialised function for setting atomic retain properties
    LazyRuntimeFunction SetPropertyAtomic;
    /// Specialised function for setting atomic copy properties
    LazyRuntimeFunction SetPropertyAtomicCopy;
    /// Specialised function for setting nonatomic retain properties
    LazyRuntimeFunction SetPropertyNonAtomic;
    /// Specialised function for setting nonatomic copy properties
    LazyRuntimeFunction SetPropertyNonAtomicCopy;
    /// Function to perform atomic copies of C++ objects with nontrivial copy
    /// constructors from Objective-C ivars.
    LazyRuntimeFunction CxxAtomicObjectGetFn;
    /// Function to perform atomic copies of C++ objects with nontrivial copy
    /// constructors to Objective-C ivars.
    LazyRuntimeFunction CxxAtomicObjectSetFn;
    /// Type of an slot structure pointer.  This is returned by the various
    /// lookup functions.
    llvm::Type *SlotTy;

  public:
    llvm::Constant *GetEHType(QualType T) override;

  protected:
    llvm::Value *LookupIMP(CodeGenFunction &CGF, llvm::Value *&Receiver,
                           llvm::Value *cmd, llvm::MDNode *node,
                           MessageSendInfo &MSI) override {
      CGBuilderTy &Builder = CGF.Builder;
      llvm::Function *LookupFn = SlotLookupFn;

      // Store the receiver on the stack so that we can reload it later
      Address ReceiverPtr =
        CGF.CreateTempAlloca(Receiver->getType(), CGF.getPointerAlign());
      Builder.CreateStore(Receiver, ReceiverPtr);

      llvm::Value *self;

      if (isa<ObjCMethodDecl>(CGF.CurCodeDecl)) {
        self = CGF.LoadObjCSelf();
      } else {
        self = llvm::ConstantPointerNull::get(IdTy);
      }

      // The lookup function is guaranteed not to capture the receiver pointer.
      LookupFn->addParamAttr(0, llvm::Attribute::NoCapture);

      llvm::Value *args[] = {
              EnforceType(Builder, ReceiverPtr.getPointer(), PtrToIdTy),
              EnforceType(Builder, cmd, SelectorTy),
              EnforceType(Builder, self, IdTy) };
      llvm::CallSite slot = CGF.EmitRuntimeCallOrInvoke(LookupFn, args);
      slot.setOnlyReadsMemory();
      slot->setMetadata(msgSendMDKind, node);

      // Load the imp from the slot
      llvm::Value *imp = Builder.CreateAlignedLoad(
          Builder.CreateStructGEP(nullptr, slot.getInstruction(), 4),
          CGF.getPointerAlign());

      // The lookup function may have changed the receiver, so make sure we use
      // the new one.
      Receiver = Builder.CreateLoad(ReceiverPtr, true);
      return imp;
    }

    llvm::Value *LookupIMPSuper(CodeGenFunction &CGF, Address ObjCSuper,
                                llvm::Value *cmd,
                                MessageSendInfo &MSI) override {
      CGBuilderTy &Builder = CGF.Builder;
      llvm::Value *lookupArgs[] = {ObjCSuper.getPointer(), cmd};

      llvm::CallInst *slot =
        CGF.EmitNounwindRuntimeCall(SlotLookupSuperFn, lookupArgs);
      slot->setOnlyReadsMemory();

      return Builder.CreateAlignedLoad(Builder.CreateStructGEP(nullptr, slot, 4),
                                       CGF.getPointerAlign());
    }

  public:
    CGObjCGNUstep(CodeGenModule &Mod) : CGObjCGNU(Mod, 9, 3) {
      const ObjCRuntime &R = CGM.getLangOpts().ObjCRuntime;

      llvm::StructType *SlotStructTy =
          llvm::StructType::get(PtrTy, PtrTy, PtrTy, IntTy, IMPTy);
      SlotTy = llvm::PointerType::getUnqual(SlotStructTy);
      // Slot_t objc_msg_lookup_sender(id *receiver, SEL selector, id sender);
      SlotLookupFn.init(&CGM, "objc_msg_lookup_sender", SlotTy, PtrToIdTy,
                        SelectorTy, IdTy);
      // Slot_t objc_msg_lookup_super(struct objc_super*, SEL);
      SlotLookupSuperFn.init(&CGM, "objc_slot_lookup_super", SlotTy,
                             PtrToObjCSuperTy, SelectorTy);
      // If we're in ObjC++ mode, then we want to make 
      if (CGM.getLangOpts().CPlusPlus) {
        llvm::Type *VoidTy = llvm::Type::getVoidTy(VMContext);
        // void *__cxa_begin_catch(void *e)
        EnterCatchFn.init(&CGM, "__cxa_begin_catch", PtrTy, PtrTy);
        // void __cxa_end_catch(void)
        ExitCatchFn.init(&CGM, "__cxa_end_catch", VoidTy);
        // void _Unwind_Resume_or_Rethrow(void*)
        ExceptionReThrowFn.init(&CGM, "_Unwind_Resume_or_Rethrow", VoidTy,
                                PtrTy);
      } else if (R.getVersion() >= VersionTuple(1, 7)) {
        llvm::Type *VoidTy = llvm::Type::getVoidTy(VMContext);
        // id objc_begin_catch(void *e)
        EnterCatchFn.init(&CGM, "objc_begin_catch", IdTy, PtrTy);
        // void objc_end_catch(void)
        ExitCatchFn.init(&CGM, "objc_end_catch", VoidTy);
        // void _Unwind_Resume_or_Rethrow(void*)
        ExceptionReThrowFn.init(&CGM, "objc_exception_rethrow", VoidTy, PtrTy);
      }
      llvm::Type *VoidTy = llvm::Type::getVoidTy(VMContext);
      SetPropertyAtomic.init(&CGM, "objc_setProperty_atomic", VoidTy, IdTy,
                             SelectorTy, IdTy, PtrDiffTy);
      SetPropertyAtomicCopy.init(&CGM, "objc_setProperty_atomic_copy", VoidTy,
                                 IdTy, SelectorTy, IdTy, PtrDiffTy);
      SetPropertyNonAtomic.init(&CGM, "objc_setProperty_nonatomic", VoidTy,
                                IdTy, SelectorTy, IdTy, PtrDiffTy);
      SetPropertyNonAtomicCopy.init(&CGM, "objc_setProperty_nonatomic_copy",
                                    VoidTy, IdTy, SelectorTy, IdTy, PtrDiffTy);
      // void objc_setCppObjectAtomic(void *dest, const void *src, void
      // *helper);
      CxxAtomicObjectSetFn.init(&CGM, "objc_setCppObjectAtomic", VoidTy, PtrTy,
                                PtrTy, PtrTy);
      // void objc_getCppObjectAtomic(void *dest, const void *src, void
      // *helper);
      CxxAtomicObjectGetFn.init(&CGM, "objc_getCppObjectAtomic", VoidTy, PtrTy,
                                PtrTy, PtrTy);
    }

    llvm::Constant *GetCppAtomicObjectGetFunction() override {
      // The optimised functions were added in version 1.7 of the GNUstep
      // runtime.
      assert (CGM.getLangOpts().ObjCRuntime.getVersion() >=
          VersionTuple(1, 7));
      return CxxAtomicObjectGetFn;
    }

    llvm::Constant *GetCppAtomicObjectSetFunction() override {
      // The optimised functions were added in version 1.7 of the GNUstep
      // runtime.
      assert (CGM.getLangOpts().ObjCRuntime.getVersion() >=
          VersionTuple(1, 7));
      return CxxAtomicObjectSetFn;
    }

    llvm::Constant *GetOptimizedPropertySetFunction(bool atomic,
                                                    bool copy) override {
      // The optimised property functions omit the GC check, and so are not
      // safe to use in GC mode.  The standard functions are fast in GC mode,
      // so there is less advantage in using them.
      assert ((CGM.getLangOpts().getGC() == LangOptions::NonGC));
      // The optimised functions were added in version 1.7 of the GNUstep
      // runtime.
      assert (CGM.getLangOpts().ObjCRuntime.getVersion() >=
          VersionTuple(1, 7));

      if (atomic) {
        if (copy) return SetPropertyAtomicCopy;
        return SetPropertyAtomic;
      }

      return copy ? SetPropertyNonAtomicCopy : SetPropertyNonAtomic;
    }
};

/// Support for the ObjFW runtime.
class CGObjCObjFW: public CGObjCGNU {
protected:
  /// The GCC ABI message lookup function.  Returns an IMP pointing to the
  /// method implementation for this message.
  LazyRuntimeFunction MsgLookupFn;
  /// stret lookup function.  While this does not seem to make sense at the
  /// first look, this is required to call the correct forwarding function.
  LazyRuntimeFunction MsgLookupFnSRet;
  /// The GCC ABI superclass message lookup function.  Takes a pointer to a
  /// structure describing the receiver and the class, and a selector as
  /// arguments.  Returns the IMP for the corresponding method.
  LazyRuntimeFunction MsgLookupSuperFn, MsgLookupSuperFnSRet;

  llvm::Value *LookupIMP(CodeGenFunction &CGF, llvm::Value *&Receiver,
                         llvm::Value *cmd, llvm::MDNode *node,
                         MessageSendInfo &MSI) override {
    CGBuilderTy &Builder = CGF.Builder;
    llvm::Value *args[] = {
            EnforceType(Builder, Receiver, IdTy),
            EnforceType(Builder, cmd, SelectorTy) };

    llvm::CallSite imp;
    if (CGM.ReturnTypeUsesSRet(MSI.CallInfo))
      imp = CGF.EmitRuntimeCallOrInvoke(MsgLookupFnSRet, args);
    else
      imp = CGF.EmitRuntimeCallOrInvoke(MsgLookupFn, args);

    imp->setMetadata(msgSendMDKind, node);
    return imp.getInstruction();
  }

  llvm::Value *LookupIMPSuper(CodeGenFunction &CGF, Address ObjCSuper,
                              llvm::Value *cmd, MessageSendInfo &MSI) override {
    CGBuilderTy &Builder = CGF.Builder;
    llvm::Value *lookupArgs[] = {
        EnforceType(Builder, ObjCSuper.getPointer(), PtrToObjCSuperTy), cmd,
    };

    if (CGM.ReturnTypeUsesSRet(MSI.CallInfo))
      return CGF.EmitNounwindRuntimeCall(MsgLookupSuperFnSRet, lookupArgs);
    else
      return CGF.EmitNounwindRuntimeCall(MsgLookupSuperFn, lookupArgs);
  }

  llvm::Value *GetClassNamed(CodeGenFunction &CGF, const std::string &Name,
                             bool isWeak) override {
    if (isWeak)
      return CGObjCGNU::GetClassNamed(CGF, Name, isWeak);

    EmitClassRef(Name);
    std::string SymbolName = "_OBJC_CLASS_" + Name;
    llvm::GlobalVariable *ClassSymbol = TheModule.getGlobalVariable(SymbolName);
    if (!ClassSymbol)
      ClassSymbol = new llvm::GlobalVariable(TheModule, LongTy, false,
                                             llvm::GlobalValue::ExternalLinkage,
                                             nullptr, SymbolName);
    return ClassSymbol;
  }

public:
  CGObjCObjFW(CodeGenModule &Mod): CGObjCGNU(Mod, 9, 3) {
    // IMP objc_msg_lookup(id, SEL);
    MsgLookupFn.init(&CGM, "objc_msg_lookup", IMPTy, IdTy, SelectorTy);
    MsgLookupFnSRet.init(&CGM, "objc_msg_lookup_stret", IMPTy, IdTy,
                         SelectorTy);
    // IMP objc_msg_lookup_super(struct objc_super*, SEL);
    MsgLookupSuperFn.init(&CGM, "objc_msg_lookup_super", IMPTy,
                          PtrToObjCSuperTy, SelectorTy);
    MsgLookupSuperFnSRet.init(&CGM, "objc_msg_lookup_super_stret", IMPTy,
                              PtrToObjCSuperTy, SelectorTy);
  }
};
} // end anonymous namespace

/// Emits a reference to a dummy variable which is emitted with each class.
/// This ensures that a linker error will be generated when trying to link
/// together modules where a referenced class is not defined.
void CGObjCGNU::EmitClassRef(const std::string &className) {
  std::string symbolRef = "__objc_class_ref_" + className;
  // Don't emit two copies of the same symbol
  if (TheModule.getGlobalVariable(symbolRef))
    return;
  std::string symbolName = "__objc_class_name_" + className;
  llvm::GlobalVariable *ClassSymbol = TheModule.getGlobalVariable(symbolName);
  if (!ClassSymbol) {
    ClassSymbol = new llvm::GlobalVariable(TheModule, LongTy, false,
                                           llvm::GlobalValue::ExternalLinkage,
                                           nullptr, symbolName);
  }
  new llvm::GlobalVariable(TheModule, ClassSymbol->getType(), true,
    llvm::GlobalValue::WeakAnyLinkage, ClassSymbol, symbolRef);
}

static std::string SymbolNameForMethod( StringRef ClassName,
     StringRef CategoryName, const Selector MethodName,
    bool isClassMethod) {
  std::string MethodNameColonStripped = MethodName.getAsString();
  std::replace(MethodNameColonStripped.begin(), MethodNameColonStripped.end(),
      ':', '_');
  return (Twine(isClassMethod ? "_c_" : "_i_") + ClassName + "_" +
    CategoryName + "_" + MethodNameColonStripped).str();
}

CGObjCGNU::CGObjCGNU(CodeGenModule &cgm, unsigned runtimeABIVersion,
                     unsigned protocolClassVersion)
  : CGObjCRuntime(cgm), TheModule(CGM.getModule()),
    VMContext(cgm.getLLVMContext()), ClassPtrAlias(nullptr),
    MetaClassPtrAlias(nullptr), RuntimeVersion(runtimeABIVersion),
    ProtocolVersion(protocolClassVersion) {

  msgSendMDKind = VMContext.getMDKindID("GNUObjCMessageSend");

  CodeGenTypes &Types = CGM.getTypes();
  IntTy = cast<llvm::IntegerType>(
      Types.ConvertType(CGM.getContext().IntTy));
  LongTy = cast<llvm::IntegerType>(
      Types.ConvertType(CGM.getContext().LongTy));
  SizeTy = cast<llvm::IntegerType>(
      Types.ConvertType(CGM.getContext().getSizeType()));
  PtrDiffTy = cast<llvm::IntegerType>(
      Types.ConvertType(CGM.getContext().getPointerDiffType()));
  BoolTy = CGM.getTypes().ConvertType(CGM.getContext().BoolTy);

  Int8Ty = llvm::Type::getInt8Ty(VMContext);
  // C string type.  Used in lots of places.
  PtrToInt8Ty = llvm::PointerType::getUnqual(Int8Ty);

  Zeros[0] = llvm::ConstantInt::get(LongTy, 0);
  Zeros[1] = Zeros[0];
  NULLPtr = llvm::ConstantPointerNull::get(PtrToInt8Ty);
  // Get the selector Type.
  QualType selTy = CGM.getContext().getObjCSelType();
  if (QualType() == selTy) {
    SelectorTy = PtrToInt8Ty;
  } else {
    SelectorTy = cast<llvm::PointerType>(CGM.getTypes().ConvertType(selTy));
  }

  PtrToIntTy = llvm::PointerType::getUnqual(IntTy);
  PtrTy = PtrToInt8Ty;

  Int32Ty = llvm::Type::getInt32Ty(VMContext);
  Int64Ty = llvm::Type::getInt64Ty(VMContext);

  IntPtrTy =
      CGM.getDataLayout().getPointerSizeInBits() == 32 ? Int32Ty : Int64Ty;

  // Object type
  QualType UnqualIdTy = CGM.getContext().getObjCIdType();
  ASTIdTy = CanQualType();
  if (UnqualIdTy != QualType()) {
    ASTIdTy = CGM.getContext().getCanonicalType(UnqualIdTy);
    IdTy = cast<llvm::PointerType>(CGM.getTypes().ConvertType(ASTIdTy));
  } else {
    IdTy = PtrToInt8Ty;
  }
  PtrToIdTy = llvm::PointerType::getUnqual(IdTy);

  ObjCSuperTy = llvm::StructType::get(IdTy, IdTy);
  PtrToObjCSuperTy = llvm::PointerType::getUnqual(ObjCSuperTy);

  llvm::Type *VoidTy = llvm::Type::getVoidTy(VMContext);

  // void objc_exception_throw(id);
  ExceptionThrowFn.init(&CGM, "objc_exception_throw", VoidTy, IdTy);
  ExceptionReThrowFn.init(&CGM, "objc_exception_throw", VoidTy, IdTy);
  // int objc_sync_enter(id);
  SyncEnterFn.init(&CGM, "objc_sync_enter", IntTy, IdTy);
  // int objc_sync_exit(id);
  SyncExitFn.init(&CGM, "objc_sync_exit", IntTy, IdTy);

  // void objc_enumerationMutation (id)
  EnumerationMutationFn.init(&CGM, "objc_enumerationMutation", VoidTy, IdTy);

  // id objc_getProperty(id, SEL, ptrdiff_t, BOOL)
  GetPropertyFn.init(&CGM, "objc_getProperty", IdTy, IdTy, SelectorTy,
                     PtrDiffTy, BoolTy);
  // void objc_setProperty(id, SEL, ptrdiff_t, id, BOOL, BOOL)
  SetPropertyFn.init(&CGM, "objc_setProperty", VoidTy, IdTy, SelectorTy,
                     PtrDiffTy, IdTy, BoolTy, BoolTy);
  // void objc_setPropertyStruct(void*, void*, ptrdiff_t, BOOL, BOOL)
  GetStructPropertyFn.init(&CGM, "objc_getPropertyStruct", VoidTy, PtrTy, PtrTy,
                           PtrDiffTy, BoolTy, BoolTy);
  // void objc_setPropertyStruct(void*, void*, ptrdiff_t, BOOL, BOOL)
  SetStructPropertyFn.init(&CGM, "objc_setPropertyStruct", VoidTy, PtrTy, PtrTy,
                           PtrDiffTy, BoolTy, BoolTy);

  // IMP type
  llvm::Type *IMPArgs[] = { IdTy, SelectorTy };
  IMPTy = llvm::PointerType::getUnqual(llvm::FunctionType::get(IdTy, IMPArgs,
              true));

  const LangOptions &Opts = CGM.getLangOpts();
  if ((Opts.getGC() != LangOptions::NonGC) || Opts.ObjCAutoRefCount)
    RuntimeVersion = 10;

  // Don't bother initialising the GC stuff unless we're compiling in GC mode
  if (Opts.getGC() != LangOptions::NonGC) {
    // This is a bit of an hack.  We should sort this out by having a proper
    // CGObjCGNUstep subclass for GC, but we may want to really support the old
    // ABI and GC added in ObjectiveC2.framework, so we fudge it a bit for now
    // Get selectors needed in GC mode
    RetainSel = GetNullarySelector("retain", CGM.getContext());
    ReleaseSel = GetNullarySelector("release", CGM.getContext());
    AutoreleaseSel = GetNullarySelector("autorelease", CGM.getContext());

    // Get functions needed in GC mode

    // id objc_assign_ivar(id, id, ptrdiff_t);
    IvarAssignFn.init(&CGM, "objc_assign_ivar", IdTy, IdTy, IdTy, PtrDiffTy);
    // id objc_assign_strongCast (id, id*)
    StrongCastAssignFn.init(&CGM, "objc_assign_strongCast", IdTy, IdTy,
                            PtrToIdTy);
    // id objc_assign_global(id, id*);
    GlobalAssignFn.init(&CGM, "objc_assign_global", IdTy, IdTy, PtrToIdTy);
    // id objc_assign_weak(id, id*);
    WeakAssignFn.init(&CGM, "objc_assign_weak", IdTy, IdTy, PtrToIdTy);
    // id objc_read_weak(id*);
    WeakReadFn.init(&CGM, "objc_read_weak", IdTy, PtrToIdTy);
    // void *objc_memmove_collectable(void*, void *, size_t);
    MemMoveFn.init(&CGM, "objc_memmove_collectable", PtrTy, PtrTy, PtrTy,
                   SizeTy);
  }
}

llvm::Value *CGObjCGNU::GetClassNamed(CodeGenFunction &CGF,
                                      const std::string &Name, bool isWeak) {
  llvm::Constant *ClassName = MakeConstantString(Name);
  // With the incompatible ABI, this will need to be replaced with a direct
  // reference to the class symbol.  For the compatible nonfragile ABI we are
  // still performing this lookup at run time but emitting the symbol for the
  // class externally so that we can make the switch later.
  //
  // Libobjc2 contains an LLVM pass that replaces calls to objc_lookup_class
  // with memoized versions or with static references if it's safe to do so.
  if (!isWeak)
    EmitClassRef(Name);

  llvm::Constant *ClassLookupFn =
    CGM.CreateRuntimeFunction(llvm::FunctionType::get(IdTy, PtrToInt8Ty, true),
                              "objc_lookup_class");
  return CGF.EmitNounwindRuntimeCall(ClassLookupFn, ClassName);
}

// This has to perform the lookup every time, since posing and related
// techniques can modify the name -> class mapping.
llvm::Value *CGObjCGNU::GetClass(CodeGenFunction &CGF,
                                 const ObjCInterfaceDecl *OID) {
  auto *Value =
      GetClassNamed(CGF, OID->getNameAsString(), OID->isWeakImported());
  if (CGM.getTriple().isOSBinFormatCOFF()) {
    if (auto *ClassSymbol = dyn_cast<llvm::GlobalVariable>(Value)) {
      auto DLLStorage = llvm::GlobalValue::DefaultStorageClass;
      if (OID->hasAttr<DLLExportAttr>())
        DLLStorage = llvm::GlobalValue::DLLExportStorageClass;
      else if (OID->hasAttr<DLLImportAttr>())
        DLLStorage = llvm::GlobalValue::DLLImportStorageClass;
      ClassSymbol->setDLLStorageClass(DLLStorage);
    }
  }
  return Value;
}

llvm::Value *CGObjCGNU::EmitNSAutoreleasePoolClassRef(CodeGenFunction &CGF) {
  auto *Value  = GetClassNamed(CGF, "NSAutoreleasePool", false);
  if (CGM.getTriple().isOSBinFormatCOFF()) {
    if (auto *ClassSymbol = dyn_cast<llvm::GlobalVariable>(Value)) {
      IdentifierInfo &II = CGF.CGM.getContext().Idents.get("NSAutoreleasePool");
      TranslationUnitDecl *TUDecl = CGM.getContext().getTranslationUnitDecl();
      DeclContext *DC = TranslationUnitDecl::castToDeclContext(TUDecl);

      const VarDecl *VD = nullptr;
      for (const auto &Result : DC->lookup(&II))
        if ((VD = dyn_cast<VarDecl>(Result)))
          break;

      auto DLLStorage = llvm::GlobalValue::DefaultStorageClass;
      if (!VD || VD->hasAttr<DLLImportAttr>())
        DLLStorage = llvm::GlobalValue::DLLImportStorageClass;
      else if (VD->hasAttr<DLLExportAttr>())
        DLLStorage = llvm::GlobalValue::DLLExportStorageClass;

      ClassSymbol->setDLLStorageClass(DLLStorage);
    }
  }
  return Value;
}

llvm::Value *CGObjCGNU::GetSelector(CodeGenFunction &CGF, Selector Sel,
                                    const std::string &TypeEncoding) {
  SmallVectorImpl<TypedSelector> &Types = SelectorTable[Sel];
  llvm::GlobalAlias *SelValue = nullptr;

  for (SmallVectorImpl<TypedSelector>::iterator i = Types.begin(),
      e = Types.end() ; i!=e ; i++) {
    if (i->first == TypeEncoding) {
      SelValue = i->second;
      break;
    }
  }
  if (!SelValue) {
    SelValue = llvm::GlobalAlias::create(
        SelectorTy->getElementType(), 0, llvm::GlobalValue::PrivateLinkage,
        ".objc_selector_" + Sel.getAsString(), &TheModule);
    Types.emplace_back(TypeEncoding, SelValue);
  }

  return SelValue;
}

Address CGObjCGNU::GetAddrOfSelector(CodeGenFunction &CGF, Selector Sel) {
  llvm::Value *SelValue = GetSelector(CGF, Sel);

  // Store it to a temporary.  Does this satisfy the semantics of
  // GetAddrOfSelector?  Hopefully.
  Address tmp = CGF.CreateTempAlloca(SelValue->getType(),
                                     CGF.getPointerAlign());
  CGF.Builder.CreateStore(SelValue, tmp);
  return tmp;
}

llvm::Value *CGObjCGNU::GetSelector(CodeGenFunction &CGF, Selector Sel) {
  return GetSelector(CGF, Sel, std::string());
}

llvm::Value *CGObjCGNU::GetSelector(CodeGenFunction &CGF,
                                    const ObjCMethodDecl *Method) {
  std::string SelTypes = CGM.getContext().getObjCEncodingForMethodDecl(Method);
  return GetSelector(CGF, Method->getSelector(), SelTypes);
}

llvm::Constant *CGObjCGNU::GetEHType(QualType T) {
  if (T->isObjCIdType() || T->isObjCQualifiedIdType()) {
    // With the old ABI, there was only one kind of catchall, which broke
    // foreign exceptions.  With the new ABI, we use __objc_id_typeinfo as
    // a pointer indicating object catchalls, and NULL to indicate real
    // catchalls
    if (CGM.getLangOpts().ObjCRuntime.isNonFragile()) {
      return MakeConstantString("@id");
    } else {
      return nullptr;
    }
  }

  // All other types should be Objective-C interface pointer types.
  const ObjCObjectPointerType *OPT = T->getAs<ObjCObjectPointerType>();
  assert(OPT && "Invalid @catch type.");
  const ObjCInterfaceDecl *IDecl = OPT->getObjectType()->getInterface();
  assert(IDecl && "Invalid @catch type.");
  return MakeConstantString(IDecl->getIdentifier()->getName());
}

llvm::Constant *CGObjCGNUstep::GetEHType(QualType T) {
  if (!CGM.getLangOpts().CPlusPlus)
    return CGObjCGNU::GetEHType(T);

  // For Objective-C++, we want to provide the ability to catch both C++ and
  // Objective-C objects in the same function.

  // There's a particular fixed type info for 'id'.
  if (T->isObjCIdType() ||
      T->isObjCQualifiedIdType()) {
    llvm::Constant *IDEHType =
      CGM.getModule().getGlobalVariable("__objc_id_type_info");
    if (!IDEHType)
      IDEHType =
        new llvm::GlobalVariable(CGM.getModule(), PtrToInt8Ty,
                                 false,
                                 llvm::GlobalValue::ExternalLinkage,
                                 nullptr, "__objc_id_type_info");
    return llvm::ConstantExpr::getBitCast(IDEHType, PtrToInt8Ty);
  }

  const ObjCObjectPointerType *PT =
    T->getAs<ObjCObjectPointerType>();
  assert(PT && "Invalid @catch type.");
  const ObjCInterfaceType *IT = PT->getInterfaceType();
  assert(IT && "Invalid @catch type.");
  std::string className = IT->getDecl()->getIdentifier()->getName();

  std::string typeinfoName = "__objc_eh_typeinfo_" + className;

  // Return the existing typeinfo if it exists
  llvm::Constant *typeinfo = TheModule.getGlobalVariable(typeinfoName);
  if (typeinfo)
    return llvm::ConstantExpr::getBitCast(typeinfo, PtrToInt8Ty);

  // Otherwise create it.

  // vtable for gnustep::libobjc::__objc_class_type_info
  // It's quite ugly hard-coding this.  Ideally we'd generate it using the host
  // platform's name mangling.
  const char *vtableName = "_ZTVN7gnustep7libobjc22__objc_class_type_infoE";
  auto *Vtable = TheModule.getGlobalVariable(vtableName);
  if (!Vtable) {
    Vtable = new llvm::GlobalVariable(TheModule, PtrToInt8Ty, true,
                                      llvm::GlobalValue::ExternalLinkage,
                                      nullptr, vtableName);
  }
  llvm::Constant *Two = llvm::ConstantInt::get(IntTy, 2);
  auto *BVtable = llvm::ConstantExpr::getBitCast(
      llvm::ConstantExpr::getGetElementPtr(Vtable->getValueType(), Vtable, Two),
      PtrToInt8Ty);

  llvm::Constant *typeName =
    ExportUniqueString(className, "__objc_eh_typename_");

  ConstantInitBuilder builder(CGM);
  auto fields = builder.beginStruct();
  fields.add(BVtable);
  fields.add(typeName);
  llvm::Constant *TI =
    fields.finishAndCreateGlobal("__objc_eh_typeinfo_" + className,
                                 CGM.getPointerAlign(),
                                 /*constant*/ false,
                                 llvm::GlobalValue::LinkOnceODRLinkage);
  return llvm::ConstantExpr::getBitCast(TI, PtrToInt8Ty);
}

/// Generate an NSConstantString object.
ConstantAddress CGObjCGNU::GenerateConstantString(const StringLiteral *SL) {

  std::string Str = SL->getString().str();
  CharUnits Align = CGM.getPointerAlign();

  // Look for an existing one
  llvm::StringMap<llvm::Constant*>::iterator old = ObjCStrings.find(Str);
  if (old != ObjCStrings.end())
    return ConstantAddress(old->getValue(), Align);

  StringRef StringClass = CGM.getLangOpts().ObjCConstantStringClass;

  if (StringClass.empty()) StringClass = "NXConstantString";

  std::string Sym = "_OBJC_CLASS_";
  Sym += StringClass;

  llvm::Constant *isa = TheModule.getNamedGlobal(Sym);

  if (!isa)
    isa = new llvm::GlobalVariable(TheModule, IdTy, /* isConstant */false,
            llvm::GlobalValue::ExternalWeakLinkage, nullptr, Sym);
  else if (isa->getType() != PtrToIdTy)
    isa = llvm::ConstantExpr::getBitCast(isa, PtrToIdTy);

  ConstantInitBuilder Builder(CGM);
  auto Fields = Builder.beginStruct();
  Fields.add(isa);
  Fields.add(MakeConstantString(Str));
  Fields.addInt(IntTy, Str.size());
  llvm::Constant *ObjCStr =
    Fields.finishAndCreateGlobal(".objc_str", Align);
  ObjCStr = llvm::ConstantExpr::getBitCast(ObjCStr, PtrToInt8Ty);
  ObjCStrings[Str] = ObjCStr;
  ConstantStrings.push_back(ObjCStr);
  return ConstantAddress(ObjCStr, Align);
}

///Generates a message send where the super is the receiver.  This is a message
///send to self with special delivery semantics indicating which class's method
///should be called.
RValue
CGObjCGNU::GenerateMessageSendSuper(CodeGenFunction &CGF,
                                    ReturnValueSlot Return,
                                    QualType ResultType,
                                    Selector Sel,
                                    const ObjCInterfaceDecl *Class,
                                    bool isCategoryImpl,
                                    llvm::Value *Receiver,
                                    bool IsClassMessage,
                                    const CallArgList &CallArgs,
                                    const ObjCMethodDecl *Method) {
  CGBuilderTy &Builder = CGF.Builder;
  if (CGM.getLangOpts().getGC() == LangOptions::GCOnly) {
    if (Sel == RetainSel || Sel == AutoreleaseSel) {
      return RValue::get(EnforceType(Builder, Receiver,
                  CGM.getTypes().ConvertType(ResultType)));
    }
    if (Sel == ReleaseSel) {
      return RValue::get(nullptr);
    }
  }

  llvm::Value *cmd = GetSelector(CGF, Sel);
  CallArgList ActualArgs;

  ActualArgs.add(RValue::get(EnforceType(Builder, Receiver, IdTy)), ASTIdTy);
  ActualArgs.add(RValue::get(cmd), CGF.getContext().getObjCSelType());
  ActualArgs.addFrom(CallArgs);

  MessageSendInfo MSI = getMessageSendInfo(Method, ResultType, ActualArgs);

  llvm::Value *ReceiverClass = nullptr;
  if (isCategoryImpl) {
    llvm::Constant *classLookupFunction = nullptr;
    if (IsClassMessage)  {
      classLookupFunction = CGM.CreateRuntimeFunction(llvm::FunctionType::get(
            IdTy, PtrTy, true), "objc_get_meta_class");
    } else {
      classLookupFunction = CGM.CreateRuntimeFunction(llvm::FunctionType::get(
            IdTy, PtrTy, true), "objc_get_class");
    }
    ReceiverClass = Builder.CreateCall(classLookupFunction,
        MakeConstantString(Class->getNameAsString()));
  } else {
    // Set up global aliases for the metaclass or class pointer if they do not
    // already exist.  These will are forward-references which will be set to
    // pointers to the class and metaclass structure created for the runtime
    // load function.  To send a message to super, we look up the value of the
    // super_class pointer from either the class or metaclass structure.
    if (IsClassMessage)  {
      if (!MetaClassPtrAlias) {
        MetaClassPtrAlias = llvm::GlobalAlias::create(
            IdTy->getElementType(), 0, llvm::GlobalValue::InternalLinkage,
            ".objc_metaclass_ref" + Class->getNameAsString(), &TheModule);
      }
      ReceiverClass = MetaClassPtrAlias;
    } else {
      if (!ClassPtrAlias) {
        ClassPtrAlias = llvm::GlobalAlias::create(
            IdTy->getElementType(), 0, llvm::GlobalValue::InternalLinkage,
            ".objc_class_ref" + Class->getNameAsString(), &TheModule);
      }
      ReceiverClass = ClassPtrAlias;
    }
  }
  // Cast the pointer to a simplified version of the class structure
  llvm::Type *CastTy = llvm::StructType::get(IdTy, IdTy);
  ReceiverClass = Builder.CreateBitCast(ReceiverClass,
                                        llvm::PointerType::getUnqual(CastTy));
  // Get the superclass pointer
  ReceiverClass = Builder.CreateStructGEP(CastTy, ReceiverClass, 1);
  // Load the superclass pointer
  ReceiverClass =
    Builder.CreateAlignedLoad(ReceiverClass, CGF.getPointerAlign());
  // Construct the structure used to look up the IMP
  llvm::StructType *ObjCSuperTy =
      llvm::StructType::get(Receiver->getType(), IdTy);

  // FIXME: Is this really supposed to be a dynamic alloca?
  Address ObjCSuper = Address(Builder.CreateAlloca(ObjCSuperTy),
                              CGF.getPointerAlign());

  Builder.CreateStore(Receiver,
                   Builder.CreateStructGEP(ObjCSuper, 0, CharUnits::Zero()));
  Builder.CreateStore(ReceiverClass,
                   Builder.CreateStructGEP(ObjCSuper, 1, CGF.getPointerSize()));

  ObjCSuper = EnforceType(Builder, ObjCSuper, PtrToObjCSuperTy);

  // Get the IMP
  llvm::Value *imp = LookupIMPSuper(CGF, ObjCSuper, cmd, MSI);
  imp = EnforceType(Builder, imp, MSI.MessengerType);

  llvm::Metadata *impMD[] = {
      llvm::MDString::get(VMContext, Sel.getAsString()),
      llvm::MDString::get(VMContext, Class->getSuperClass()->getNameAsString()),
      llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(
          llvm::Type::getInt1Ty(VMContext), IsClassMessage))};
  llvm::MDNode *node = llvm::MDNode::get(VMContext, impMD);

  CGCallee callee(CGCalleeInfo(), imp);

  llvm::Instruction *call;
  RValue msgRet = CGF.EmitCall(MSI.CallInfo, callee, Return, ActualArgs, &call);
  call->setMetadata(msgSendMDKind, node);
  return msgRet;
}

/// Generate code for a message send expression.
RValue
CGObjCGNU::GenerateMessageSend(CodeGenFunction &CGF,
                               ReturnValueSlot Return,
                               QualType ResultType,
                               Selector Sel,
                               llvm::Value *Receiver,
                               const CallArgList &CallArgs,
                               const ObjCInterfaceDecl *Class,
                               const ObjCMethodDecl *Method) {
  CGBuilderTy &Builder = CGF.Builder;

  // Strip out message sends to retain / release in GC mode
  if (CGM.getLangOpts().getGC() == LangOptions::GCOnly) {
    if (Sel == RetainSel || Sel == AutoreleaseSel) {
      return RValue::get(EnforceType(Builder, Receiver,
                  CGM.getTypes().ConvertType(ResultType)));
    }
    if (Sel == ReleaseSel) {
      return RValue::get(nullptr);
    }
  }

  // If the return type is something that goes in an integer register, the
  // runtime will handle 0 returns.  For other cases, we fill in the 0 value
  // ourselves.
  //
  // The language spec says the result of this kind of message send is
  // undefined, but lots of people seem to have forgotten to read that
  // paragraph and insist on sending messages to nil that have structure
  // returns.  With GCC, this generates a random return value (whatever happens
  // to be on the stack / in those registers at the time) on most platforms,
  // and generates an illegal instruction trap on SPARC.  With LLVM it corrupts
  // the stack.  
  bool isPointerSizedReturn = (ResultType->isAnyPointerType() ||
      ResultType->isIntegralOrEnumerationType() || ResultType->isVoidType());

  llvm::BasicBlock *startBB = nullptr;
  llvm::BasicBlock *messageBB = nullptr;
  llvm::BasicBlock *continueBB = nullptr;

  if (!isPointerSizedReturn) {
    startBB = Builder.GetInsertBlock();
    messageBB = CGF.createBasicBlock("msgSend");
    continueBB = CGF.createBasicBlock("continue");

    llvm::Value *isNil = Builder.CreateICmpEQ(Receiver, 
            llvm::Constant::getNullValue(Receiver->getType()));
    Builder.CreateCondBr(isNil, continueBB, messageBB);
    CGF.EmitBlock(messageBB);
  }

  IdTy = cast<llvm::PointerType>(CGM.getTypes().ConvertType(ASTIdTy));
  llvm::Value *cmd;
  if (Method)
    cmd = GetSelector(CGF, Method);
  else
    cmd = GetSelector(CGF, Sel);
  cmd = EnforceType(Builder, cmd, SelectorTy);
  Receiver = EnforceType(Builder, Receiver, IdTy);

  llvm::Metadata *impMD[] = {
      llvm::MDString::get(VMContext, Sel.getAsString()),
      llvm::MDString::get(VMContext, Class ? Class->getNameAsString() : ""),
      llvm::ConstantAsMetadata::get(llvm::ConstantInt::get(
          llvm::Type::getInt1Ty(VMContext), Class != nullptr))};
  llvm::MDNode *node = llvm::MDNode::get(VMContext, impMD);

  CallArgList ActualArgs;
  ActualArgs.add(RValue::get(Receiver), ASTIdTy);
  ActualArgs.add(RValue::get(cmd), CGF.getContext().getObjCSelType());
  ActualArgs.addFrom(CallArgs);

  MessageSendInfo MSI = getMessageSendInfo(Method, ResultType, ActualArgs);

  // Get the IMP to call
  llvm::Value *imp;

  // If we have non-legacy dispatch specified, we try using the objc_msgSend()
  // functions.  These are not supported on all platforms (or all runtimes on a
  // given platform), so we 
  switch (CGM.getCodeGenOpts().getObjCDispatchMethod()) {
    case CodeGenOptions::Legacy:
      imp = LookupIMP(CGF, Receiver, cmd, node, MSI);
      break;
    case CodeGenOptions::Mixed:
    case CodeGenOptions::NonLegacy:
      if (CGM.ReturnTypeUsesFPRet(ResultType)) {
        imp = CGM.CreateRuntimeFunction(llvm::FunctionType::get(IdTy, IdTy, true),
                                  "objc_msgSend_fpret");
      } else if (CGM.ReturnTypeUsesSRet(MSI.CallInfo)) {
        // The actual types here don't matter - we're going to bitcast the
        // function anyway
        imp = CGM.CreateRuntimeFunction(llvm::FunctionType::get(IdTy, IdTy, true),
                                  "objc_msgSend_stret");
      } else {
        imp = CGM.CreateRuntimeFunction(llvm::FunctionType::get(IdTy, IdTy, true),
                                  "objc_msgSend");
      }
  }

  // Reset the receiver in case the lookup modified it
  ActualArgs[0] = CallArg(RValue::get(Receiver), ASTIdTy, false);

  imp = EnforceType(Builder, imp, MSI.MessengerType);

  llvm::Instruction *call;
  CGCallee callee(CGCalleeInfo(), imp);
  RValue msgRet = CGF.EmitCall(MSI.CallInfo, callee, Return, ActualArgs, &call);
  call->setMetadata(msgSendMDKind, node);


  if (!isPointerSizedReturn) {
    messageBB = CGF.Builder.GetInsertBlock();
    CGF.Builder.CreateBr(continueBB);
    CGF.EmitBlock(continueBB);
    if (msgRet.isScalar()) {
      llvm::Value *v = msgRet.getScalarVal();
      llvm::PHINode *phi = Builder.CreatePHI(v->getType(), 2);
      phi->addIncoming(v, messageBB);
      phi->addIncoming(llvm::Constant::getNullValue(v->getType()), startBB);
      msgRet = RValue::get(phi);
    } else if (msgRet.isAggregate()) {
      Address v = msgRet.getAggregateAddress();
      llvm::PHINode *phi = Builder.CreatePHI(v.getType(), 2);
      llvm::Type *RetTy = v.getElementType();
      Address NullVal = CGF.CreateTempAlloca(RetTy, v.getAlignment(), "null");
      CGF.InitTempAlloca(NullVal, llvm::Constant::getNullValue(RetTy));
      phi->addIncoming(v.getPointer(), messageBB);
      phi->addIncoming(NullVal.getPointer(), startBB);
      msgRet = RValue::getAggregate(Address(phi, v.getAlignment()));
    } else /* isComplex() */ {
      std::pair<llvm::Value*,llvm::Value*> v = msgRet.getComplexVal();
      llvm::PHINode *phi = Builder.CreatePHI(v.first->getType(), 2);
      phi->addIncoming(v.first, messageBB);
      phi->addIncoming(llvm::Constant::getNullValue(v.first->getType()),
          startBB);
      llvm::PHINode *phi2 = Builder.CreatePHI(v.second->getType(), 2);
      phi2->addIncoming(v.second, messageBB);
      phi2->addIncoming(llvm::Constant::getNullValue(v.second->getType()),
          startBB);
      msgRet = RValue::getComplex(phi, phi2);
    }
  }
  return msgRet;
}

/// Generates a MethodList.  Used in construction of a objc_class and
/// objc_category structures.
llvm::Constant *CGObjCGNU::
GenerateMethodList(StringRef ClassName,
                   StringRef CategoryName,
                   ArrayRef<Selector> MethodSels,
                   ArrayRef<llvm::Constant *> MethodTypes,
                   bool isClassMethodList) {
  if (MethodSels.empty())
    return NULLPtr;

  ConstantInitBuilder Builder(CGM);

  auto MethodList = Builder.beginStruct();
  MethodList.addNullPointer(CGM.Int8PtrTy);
  MethodList.addInt(Int32Ty, MethodTypes.size());

  // Get the method structure type.
  llvm::StructType *ObjCMethodTy =
    llvm::StructType::get(CGM.getLLVMContext(), {
      PtrToInt8Ty, // Really a selector, but the runtime creates it us.
      PtrToInt8Ty, // Method types
      IMPTy        // Method pointer
    });
  auto Methods = MethodList.beginArray();
  for (unsigned int i = 0, e = MethodTypes.size(); i < e; ++i) {
    llvm::Constant *FnPtr =
      TheModule.getFunction(SymbolNameForMethod(ClassName, CategoryName,
                                                MethodSels[i],
                                                isClassMethodList));
    assert(FnPtr && "Can't generate metadata for method that doesn't exist");
    auto Method = Methods.beginStruct(ObjCMethodTy);
    Method.add(MakeConstantString(MethodSels[i].getAsString()));
    Method.add(MethodTypes[i]);
    Method.addBitCast(FnPtr, IMPTy);
    Method.finishAndAddTo(Methods);
  }
  Methods.finishAndAddTo(MethodList);

  // Create an instance of the structure
  return MethodList.finishAndCreateGlobal(".objc_method_list",
                                          CGM.getPointerAlign());
}

/// Generates an IvarList.  Used in construction of a objc_class.
llvm::Constant *CGObjCGNU::
GenerateIvarList(ArrayRef<llvm::Constant *> IvarNames,
                 ArrayRef<llvm::Constant *> IvarTypes,
                 ArrayRef<llvm::Constant *> IvarOffsets) {
  if (IvarNames.empty())
    return NULLPtr;

  ConstantInitBuilder Builder(CGM);

  // Structure containing array count followed by array.
  auto IvarList = Builder.beginStruct();
  IvarList.addInt(IntTy, (int)IvarNames.size());

  // Get the ivar structure type.
  llvm::StructType *ObjCIvarTy =
      llvm::StructType::get(PtrToInt8Ty, PtrToInt8Ty, IntTy);

  // Array of ivar structures.
  auto Ivars = IvarList.beginArray(ObjCIvarTy);
  for (unsigned int i = 0, e = IvarNames.size() ; i < e ; i++) {
    auto Ivar = Ivars.beginStruct(ObjCIvarTy);
    Ivar.add(IvarNames[i]);
    Ivar.add(IvarTypes[i]);
    Ivar.add(IvarOffsets[i]);
    Ivar.finishAndAddTo(Ivars);
  }
  Ivars.finishAndAddTo(IvarList);

  // Create an instance of the structure
  return IvarList.finishAndCreateGlobal(".objc_ivar_list",
                                        CGM.getPointerAlign());
}

/// Generate a class structure
llvm::Constant *CGObjCGNU::GenerateClassStructure(
    llvm::Constant *MetaClass,
    llvm::Constant *SuperClass,
    unsigned info,
    const char *Name,
    llvm::Constant *Version,
    llvm::Constant *InstanceSize,
    llvm::Constant *IVars,
    llvm::Constant *Methods,
    llvm::Constant *Protocols,
    llvm::Constant *IvarOffsets,
    llvm::Constant *Properties,
    llvm::Constant *StrongIvarBitmap,
    llvm::Constant *WeakIvarBitmap,
    bool isMeta) {
  // Set up the class structure
  // Note:  Several of these are char*s when they should be ids.  This is
  // because the runtime performs this translation on load.
  //
  // Fields marked New ABI are part of the GNUstep runtime.  We emit them
  // anyway; the classes will still work with the GNU runtime, they will just
  // be ignored.
  llvm::StructType *ClassTy = llvm::StructType::get(
      PtrToInt8Ty,        // isa
      PtrToInt8Ty,        // super_class
      PtrToInt8Ty,        // name
      LongTy,             // version
      LongTy,             // info
      LongTy,             // instance_size
      IVars->getType(),   // ivars
      Methods->getType(), // methods
      // These are all filled in by the runtime, so we pretend
      PtrTy, // dtable
      PtrTy, // subclass_list
      PtrTy, // sibling_class
      PtrTy, // protocols
      PtrTy, // gc_object_type
      // New ABI:
      LongTy,                 // abi_version
      IvarOffsets->getType(), // ivar_offsets
      Properties->getType(),  // properties
      IntPtrTy,               // strong_pointers
      IntPtrTy                // weak_pointers
      );

  ConstantInitBuilder Builder(CGM);
  auto Elements = Builder.beginStruct(ClassTy);

  // Fill in the structure

  // isa 
  Elements.addBitCast(MetaClass, PtrToInt8Ty);
  // super_class
  Elements.add(SuperClass);
  // name
  Elements.add(MakeConstantString(Name, ".class_name"));
  // version
  Elements.addInt(LongTy, 0);
  // info
  Elements.addInt(LongTy, info);
  // instance_size
  if (isMeta) {
    llvm::DataLayout td(&TheModule);
    Elements.addInt(LongTy,
                    td.getTypeSizeInBits(ClassTy) /
                      CGM.getContext().getCharWidth());
  } else
    Elements.add(InstanceSize);
  // ivars
  Elements.add(IVars);
  // methods
  Elements.add(Methods);
  // These are all filled in by the runtime, so we pretend
  // dtable
  Elements.add(NULLPtr);
  // subclass_list
  Elements.add(NULLPtr);
  // sibling_class
  Elements.add(NULLPtr);
  // protocols
  Elements.addBitCast(Protocols, PtrTy);
  // gc_object_type
  Elements.add(NULLPtr);
  // abi_version
  Elements.addInt(LongTy, 1);
  // ivar_offsets
  Elements.add(IvarOffsets);
  // properties
  Elements.add(Properties);
  // strong_pointers
  Elements.add(StrongIvarBitmap);
  // weak_pointers
  Elements.add(WeakIvarBitmap);
  // Create an instance of the structure
  // This is now an externally visible symbol, so that we can speed up class
  // messages in the next ABI.  We may already have some weak references to
  // this, so check and fix them properly.
  std::string ClassSym((isMeta ? "_OBJC_METACLASS_": "_OBJC_CLASS_") +
          std::string(Name));
  llvm::GlobalVariable *ClassRef = TheModule.getNamedGlobal(ClassSym);
  llvm::Constant *Class =
    Elements.finishAndCreateGlobal(ClassSym, CGM.getPointerAlign(), false,
                                   llvm::GlobalValue::ExternalLinkage);
  if (ClassRef) {
    ClassRef->replaceAllUsesWith(llvm::ConstantExpr::getBitCast(Class,
                  ClassRef->getType()));
    ClassRef->removeFromParent();
    Class->setName(ClassSym);
  }
  return Class;
}

llvm::Constant *CGObjCGNU::
GenerateProtocolMethodList(ArrayRef<llvm::Constant *> MethodNames,
                           ArrayRef<llvm::Constant *> MethodTypes) {
  // Get the method structure type.
  llvm::StructType *ObjCMethodDescTy =
    llvm::StructType::get(CGM.getLLVMContext(), { PtrToInt8Ty, PtrToInt8Ty });
  ConstantInitBuilder Builder(CGM);
  auto MethodList = Builder.beginStruct();
  MethodList.addInt(IntTy, MethodNames.size());
  auto Methods = MethodList.beginArray(ObjCMethodDescTy);
  for (unsigned int i = 0, e = MethodTypes.size() ; i < e ; i++) {
    auto Method = Methods.beginStruct(ObjCMethodDescTy);
    Method.add(MethodNames[i]);
    Method.add(MethodTypes[i]);
    Method.finishAndAddTo(Methods);
  }
  Methods.finishAndAddTo(MethodList);
  return MethodList.finishAndCreateGlobal(".objc_method_list",
                                          CGM.getPointerAlign());
}

// Create the protocol list structure used in classes, categories and so on
llvm::Constant *
CGObjCGNU::GenerateProtocolList(ArrayRef<std::string> Protocols) {

  ConstantInitBuilder Builder(CGM);
  auto ProtocolList = Builder.beginStruct();
  ProtocolList.add(NULLPtr);
  ProtocolList.addInt(LongTy, Protocols.size());

  auto Elements = ProtocolList.beginArray(PtrToInt8Ty);
  for (const std::string *iter = Protocols.begin(), *endIter = Protocols.end();
      iter != endIter ; iter++) {
    llvm::Constant *protocol = nullptr;
    llvm::StringMap<llvm::Constant*>::iterator value =
      ExistingProtocols.find(*iter);
    if (value == ExistingProtocols.end()) {
      protocol = GenerateEmptyProtocol(*iter);
    } else {
      protocol = value->getValue();
    }
    Elements.addBitCast(protocol, PtrToInt8Ty);
  }
  Elements.finishAndAddTo(ProtocolList);
  return ProtocolList.finishAndCreateGlobal(".objc_protocol_list",
                                            CGM.getPointerAlign());
}

llvm::Value *CGObjCGNU::GenerateProtocolRef(CodeGenFunction &CGF,
                                            const ObjCProtocolDecl *PD) {
  llvm::Value *protocol = ExistingProtocols[PD->getNameAsString()];
  llvm::Type *T =
    CGM.getTypes().ConvertType(CGM.getContext().getObjCProtoType());
  return CGF.Builder.CreateBitCast(protocol, llvm::PointerType::getUnqual(T));
}

llvm::Constant *
CGObjCGNU::GenerateEmptyProtocol(const std::string &ProtocolName) {
  llvm::Constant *ProtocolList = GenerateProtocolList({});
  llvm::Constant *MethodList = GenerateProtocolMethodList({}, {});
  // Protocols are objects containing lists of the methods implemented and
  // protocols adopted.
  ConstantInitBuilder Builder(CGM);
  auto Elements = Builder.beginStruct();

  // The isa pointer must be set to a magic number so the runtime knows it's
  // the correct layout.
  Elements.add(llvm::ConstantExpr::getIntToPtr(
          llvm::ConstantInt::get(Int32Ty, ProtocolVersion), IdTy));

  Elements.add(MakeConstantString(ProtocolName, ".objc_protocol_name"));
  Elements.add(ProtocolList);
  Elements.add(MethodList);
  Elements.add(MethodList);
  Elements.add(MethodList);
  Elements.add(MethodList);
  return Elements.finishAndCreateGlobal(".objc_protocol",
                                        CGM.getPointerAlign());
}

void CGObjCGNU::GenerateProtocol(const ObjCProtocolDecl *PD) {
  ASTContext &Context = CGM.getContext();
  std::string ProtocolName = PD->getNameAsString();
  
  // Use the protocol definition, if there is one.
  if (const ObjCProtocolDecl *Def = PD->getDefinition())
    PD = Def;

  SmallVector<std::string, 16> Protocols;
  for (const auto *PI : PD->protocols())
    Protocols.push_back(PI->getNameAsString());
  SmallVector<llvm::Constant*, 16> InstanceMethodNames;
  SmallVector<llvm::Constant*, 16> InstanceMethodTypes;
  SmallVector<llvm::Constant*, 16> OptionalInstanceMethodNames;
  SmallVector<llvm::Constant*, 16> OptionalInstanceMethodTypes;
  for (const auto *I : PD->instance_methods()) {
    std::string TypeStr = Context.getObjCEncodingForMethodDecl(I);
    if (I->getImplementationControl() == ObjCMethodDecl::Optional) {
      OptionalInstanceMethodNames.push_back(
          MakeConstantString(I->getSelector().getAsString()));
      OptionalInstanceMethodTypes.push_back(MakeConstantString(TypeStr));
    } else {
      InstanceMethodNames.push_back(
          MakeConstantString(I->getSelector().getAsString()));
      InstanceMethodTypes.push_back(MakeConstantString(TypeStr));
    }
  }
  // Collect information about class methods:
  SmallVector<llvm::Constant*, 16> ClassMethodNames;
  SmallVector<llvm::Constant*, 16> ClassMethodTypes;
  SmallVector<llvm::Constant*, 16> OptionalClassMethodNames;
  SmallVector<llvm::Constant*, 16> OptionalClassMethodTypes;
  for (const auto *I : PD->class_methods()) {
    std::string TypeStr = Context.getObjCEncodingForMethodDecl(I);
    if (I->getImplementationControl() == ObjCMethodDecl::Optional) {
      OptionalClassMethodNames.push_back(
          MakeConstantString(I->getSelector().getAsString()));
      OptionalClassMethodTypes.push_back(MakeConstantString(TypeStr));
    } else {
      ClassMethodNames.push_back(
          MakeConstantString(I->getSelector().getAsString()));
      ClassMethodTypes.push_back(MakeConstantString(TypeStr));
    }
  }

  llvm::Constant *ProtocolList = GenerateProtocolList(Protocols);
  llvm::Constant *InstanceMethodList =
    GenerateProtocolMethodList(InstanceMethodNames, InstanceMethodTypes);
  llvm::Constant *ClassMethodList =
    GenerateProtocolMethodList(ClassMethodNames, ClassMethodTypes);
  llvm::Constant *OptionalInstanceMethodList =
    GenerateProtocolMethodList(OptionalInstanceMethodNames,
            OptionalInstanceMethodTypes);
  llvm::Constant *OptionalClassMethodList =
    GenerateProtocolMethodList(OptionalClassMethodNames,
            OptionalClassMethodTypes);

  // Property metadata: name, attributes, isSynthesized, setter name, setter
  // types, getter name, getter types.
  // The isSynthesized value is always set to 0 in a protocol.  It exists to
  // simplify the runtime library by allowing it to use the same data
  // structures for protocol metadata everywhere.

  llvm::Constant *PropertyList;
  llvm::Constant *OptionalPropertyList;
  {
    llvm::StructType *propertyMetadataTy =
      llvm::StructType::get(CGM.getLLVMContext(),
        { PtrToInt8Ty, Int8Ty, Int8Ty, Int8Ty, Int8Ty, PtrToInt8Ty,
          PtrToInt8Ty, PtrToInt8Ty, PtrToInt8Ty });

    unsigned numReqProperties = 0, numOptProperties = 0;
    for (auto property : PD->instance_properties()) {
      if (property->isOptional())
        numOptProperties++;
      else
        numReqProperties++;
    }

    ConstantInitBuilder reqPropertyListBuilder(CGM);
    auto reqPropertiesList = reqPropertyListBuilder.beginStruct();
    reqPropertiesList.addInt(IntTy, numReqProperties);
    reqPropertiesList.add(NULLPtr);
    auto reqPropertiesArray = reqPropertiesList.beginArray(propertyMetadataTy);

    ConstantInitBuilder optPropertyListBuilder(CGM);
    auto optPropertiesList = optPropertyListBuilder.beginStruct();
    optPropertiesList.addInt(IntTy, numOptProperties);
    optPropertiesList.add(NULLPtr);
    auto optPropertiesArray = optPropertiesList.beginArray(propertyMetadataTy);

    // Add all of the property methods need adding to the method list and to the
    // property metadata list.
    for (auto *property : PD->instance_properties()) {
      auto &propertiesArray =
        (property->isOptional() ? optPropertiesArray : reqPropertiesArray);
      auto fields = propertiesArray.beginStruct(propertyMetadataTy);

      fields.add(MakePropertyEncodingString(property, nullptr));
      PushPropertyAttributes(fields, property);

      if (ObjCMethodDecl *getter = property->getGetterMethodDecl()) {
        std::string typeStr = Context.getObjCEncodingForMethodDecl(getter);
        llvm::Constant *typeEncoding = MakeConstantString(typeStr);
        InstanceMethodTypes.push_back(typeEncoding);
        fields.add(MakeConstantString(getter->getSelector().getAsString()));
        fields.add(typeEncoding);
      } else {
        fields.add(NULLPtr);
        fields.add(NULLPtr);
      }
      if (ObjCMethodDecl *setter = property->getSetterMethodDecl()) {
        std::string typeStr = Context.getObjCEncodingForMethodDecl(setter);
        llvm::Constant *typeEncoding = MakeConstantString(typeStr);
        InstanceMethodTypes.push_back(typeEncoding);
        fields.add(MakeConstantString(setter->getSelector().getAsString()));
        fields.add(typeEncoding);
      } else {
        fields.add(NULLPtr);
        fields.add(NULLPtr);
      }

      fields.finishAndAddTo(propertiesArray);
    }

    reqPropertiesArray.finishAndAddTo(reqPropertiesList);
    PropertyList =
      reqPropertiesList.finishAndCreateGlobal(".objc_property_list",
                                              CGM.getPointerAlign());

    optPropertiesArray.finishAndAddTo(optPropertiesList);
    OptionalPropertyList =
      optPropertiesList.finishAndCreateGlobal(".objc_property_list",
                                              CGM.getPointerAlign());
  }

  // Protocols are objects containing lists of the methods implemented and
  // protocols adopted.
  // The isa pointer must be set to a magic number so the runtime knows it's
  // the correct layout.
  ConstantInitBuilder Builder(CGM);
  auto Elements = Builder.beginStruct();
  Elements.add(
      llvm::ConstantExpr::getIntToPtr(
          llvm::ConstantInt::get(Int32Ty, ProtocolVersion), IdTy));
  Elements.add(
      MakeConstantString(ProtocolName, ".objc_protocol_name"));
  Elements.add(ProtocolList);
  Elements.add(InstanceMethodList);
  Elements.add(ClassMethodList);
  Elements.add(OptionalInstanceMethodList);
  Elements.add(OptionalClassMethodList);
  Elements.add(PropertyList);
  Elements.add(OptionalPropertyList);
  ExistingProtocols[ProtocolName] =
    llvm::ConstantExpr::getBitCast(
      Elements.finishAndCreateGlobal(".objc_protocol", CGM.getPointerAlign()),
      IdTy);
}
void CGObjCGNU::GenerateProtocolHolderCategory() {
  // Collect information about instance methods
  SmallVector<Selector, 1> MethodSels;
  SmallVector<llvm::Constant*, 1> MethodTypes;

  ConstantInitBuilder Builder(CGM);
  auto Elements = Builder.beginStruct();

  const std::string ClassName = "__ObjC_Protocol_Holder_Ugly_Hack";
  const std::string CategoryName = "AnotherHack";
  Elements.add(MakeConstantString(CategoryName));
  Elements.add(MakeConstantString(ClassName));
  // Instance method list
  Elements.addBitCast(GenerateMethodList(
          ClassName, CategoryName, MethodSels, MethodTypes, false), PtrTy);
  // Class method list
  Elements.addBitCast(GenerateMethodList(
          ClassName, CategoryName, MethodSels, MethodTypes, true), PtrTy);

  // Protocol list
  ConstantInitBuilder ProtocolListBuilder(CGM);
  auto ProtocolList = ProtocolListBuilder.beginStruct();
  ProtocolList.add(NULLPtr);
  ProtocolList.addInt(LongTy, ExistingProtocols.size());
  auto ProtocolElements = ProtocolList.beginArray(PtrTy);
  for (auto iter = ExistingProtocols.begin(), endIter = ExistingProtocols.end();
       iter != endIter ; iter++) {
    ProtocolElements.addBitCast(iter->getValue(), PtrTy);
  }
  ProtocolElements.finishAndAddTo(ProtocolList);
  Elements.addBitCast(
                   ProtocolList.finishAndCreateGlobal(".objc_protocol_list",
                                                      CGM.getPointerAlign()),
                   PtrTy);
  Categories.push_back(llvm::ConstantExpr::getBitCast(
        Elements.finishAndCreateGlobal("", CGM.getPointerAlign()),
        PtrTy));
}

/// Libobjc2 uses a bitfield representation where small(ish) bitfields are
/// stored in a 64-bit value with the low bit set to 1 and the remaining 63
/// bits set to their values, LSB first, while larger ones are stored in a
/// structure of this / form:
/// 
/// struct { int32_t length; int32_t values[length]; };
///
/// The values in the array are stored in host-endian format, with the least
/// significant bit being assumed to come first in the bitfield.  Therefore, a
/// bitfield with the 64th bit set will be (int64_t)&{ 2, [0, 1<<31] }, while a
/// bitfield / with the 63rd bit set will be 1<<64.
llvm::Constant *CGObjCGNU::MakeBitField(ArrayRef<bool> bits) {
  int bitCount = bits.size();
  int ptrBits = CGM.getDataLayout().getPointerSizeInBits();
  if (bitCount < ptrBits) {
    uint64_t val = 1;
    for (int i=0 ; i<bitCount ; ++i) {
      if (bits[i]) val |= 1ULL<<(i+1);
    }
    return llvm::ConstantInt::get(IntPtrTy, val);
  }
  SmallVector<llvm::Constant *, 8> values;
  int v=0;
  while (v < bitCount) {
    int32_t word = 0;
    for (int i=0 ; (i<32) && (v<bitCount)  ; ++i) {
      if (bits[v]) word |= 1<<i;
      v++;
    }
    values.push_back(llvm::ConstantInt::get(Int32Ty, word));
  }

  ConstantInitBuilder builder(CGM);
  auto fields = builder.beginStruct();
  fields.addInt(Int32Ty, values.size());
  auto array = fields.beginArray();
  for (auto v : values) array.add(v);
  array.finishAndAddTo(fields);

  llvm::Constant *GS =
    fields.finishAndCreateGlobal("", CharUnits::fromQuantity(4));
  llvm::Constant *ptr = llvm::ConstantExpr::getPtrToInt(GS, IntPtrTy);
  return ptr;
}

void CGObjCGNU::GenerateCategory(const ObjCCategoryImplDecl *OCD) {
  std::string ClassName = OCD->getClassInterface()->getNameAsString();
  std::string CategoryName = OCD->getNameAsString();
  // Collect information about instance methods
  SmallVector<Selector, 16> InstanceMethodSels;
  SmallVector<llvm::Constant*, 16> InstanceMethodTypes;
  for (const auto *I : OCD->instance_methods()) {
    InstanceMethodSels.push_back(I->getSelector());
    std::string TypeStr = CGM.getContext().getObjCEncodingForMethodDecl(I);
    InstanceMethodTypes.push_back(MakeConstantString(TypeStr));
  }

  // Collect information about class methods
  SmallVector<Selector, 16> ClassMethodSels;
  SmallVector<llvm::Constant*, 16> ClassMethodTypes;
  for (const auto *I : OCD->class_methods()) {
    ClassMethodSels.push_back(I->getSelector());
    std::string TypeStr = CGM.getContext().getObjCEncodingForMethodDecl(I);
    ClassMethodTypes.push_back(MakeConstantString(TypeStr));
  }

  // Collect the names of referenced protocols
  SmallVector<std::string, 16> Protocols;
  const ObjCCategoryDecl *CatDecl = OCD->getCategoryDecl();
  const ObjCList<ObjCProtocolDecl> &Protos = CatDecl->getReferencedProtocols();
  for (ObjCList<ObjCProtocolDecl>::iterator I = Protos.begin(),
       E = Protos.end(); I != E; ++I)
    Protocols.push_back((*I)->getNameAsString());

  ConstantInitBuilder Builder(CGM);
  auto Elements = Builder.beginStruct();
  Elements.add(MakeConstantString(CategoryName));
  Elements.add(MakeConstantString(ClassName));
  // Instance method list
  Elements.addBitCast(
          GenerateMethodList(ClassName, CategoryName, InstanceMethodSels,
                             InstanceMethodTypes, false),
          PtrTy);
  // Class method list
  Elements.addBitCast(
          GenerateMethodList(ClassName, CategoryName, ClassMethodSels,
                             ClassMethodTypes, true),
          PtrTy);
  // Protocol list
  Elements.addBitCast(GenerateProtocolList(Protocols), PtrTy);
  Categories.push_back(llvm::ConstantExpr::getBitCast(
        Elements.finishAndCreateGlobal("", CGM.getPointerAlign()),
        PtrTy));
}

llvm::Constant *CGObjCGNU::GeneratePropertyList(const ObjCImplementationDecl *OID,
        SmallVectorImpl<Selector> &InstanceMethodSels,
        SmallVectorImpl<llvm::Constant*> &InstanceMethodTypes) {
  ASTContext &Context = CGM.getContext();
  // Property metadata: name, attributes, attributes2, padding1, padding2,
  // setter name, setter types, getter name, getter types.
  llvm::StructType *propertyMetadataTy =
    llvm::StructType::get(CGM.getLLVMContext(),
        { PtrToInt8Ty, Int8Ty, Int8Ty, Int8Ty, Int8Ty, PtrToInt8Ty,
          PtrToInt8Ty, PtrToInt8Ty, PtrToInt8Ty });

  unsigned numProperties = 0;
  for (auto *propertyImpl : OID->property_impls()) {
    (void) propertyImpl;
    numProperties++;
  }

  ConstantInitBuilder builder(CGM);
  auto propertyList = builder.beginStruct();
  propertyList.addInt(IntTy, numProperties);
  propertyList.add(NULLPtr);
  auto properties = propertyList.beginArray(propertyMetadataTy);

  // Add all of the property methods need adding to the method list and to the
  // property metadata list.
  for (auto *propertyImpl : OID->property_impls()) {
    auto fields = properties.beginStruct(propertyMetadataTy);
    ObjCPropertyDecl *property = propertyImpl->getPropertyDecl();
    bool isSynthesized = (propertyImpl->getPropertyImplementation() == 
        ObjCPropertyImplDecl::Synthesize);
    bool isDynamic = (propertyImpl->getPropertyImplementation() == 
        ObjCPropertyImplDecl::Dynamic);

    fields.add(MakePropertyEncodingString(property, OID));
    PushPropertyAttributes(fields, property, isSynthesized, isDynamic);
    if (ObjCMethodDecl *getter = property->getGetterMethodDecl()) {
      std::string TypeStr = Context.getObjCEncodingForMethodDecl(getter);
      llvm::Constant *TypeEncoding = MakeConstantString(TypeStr);
      if (isSynthesized) {
        InstanceMethodTypes.push_back(TypeEncoding);
        InstanceMethodSels.push_back(getter->getSelector());
      }
      fields.add(MakeConstantString(getter->getSelector().getAsString()));
      fields.add(TypeEncoding);
    } else {
      fields.add(NULLPtr);
      fields.add(NULLPtr);
    }
    if (ObjCMethodDecl *setter = property->getSetterMethodDecl()) {
      std::string TypeStr = Context.getObjCEncodingForMethodDecl(setter);
      llvm::Constant *TypeEncoding = MakeConstantString(TypeStr);
      if (isSynthesized) {
        InstanceMethodTypes.push_back(TypeEncoding);
        InstanceMethodSels.push_back(setter->getSelector());
      }
      fields.add(MakeConstantString(setter->getSelector().getAsString()));
      fields.add(TypeEncoding);
    } else {
      fields.add(NULLPtr);
      fields.add(NULLPtr);
    }
    fields.finishAndAddTo(properties);
  }
  properties.finishAndAddTo(propertyList);

  return propertyList.finishAndCreateGlobal(".objc_property_list",
                                            CGM.getPointerAlign());
}

void CGObjCGNU::RegisterAlias(const ObjCCompatibleAliasDecl *OAD) {
  // Get the class declaration for which the alias is specified.
  ObjCInterfaceDecl *ClassDecl =
    const_cast<ObjCInterfaceDecl *>(OAD->getClassInterface());
  ClassAliases.emplace_back(ClassDecl->getNameAsString(),
                            OAD->getNameAsString());
}

void CGObjCGNU::GenerateClass(const ObjCImplementationDecl *OID) {
  ASTContext &Context = CGM.getContext();

  // Get the superclass name.
  const ObjCInterfaceDecl * SuperClassDecl =
    OID->getClassInterface()->getSuperClass();
  std::string SuperClassName;
  if (SuperClassDecl) {
    SuperClassName = SuperClassDecl->getNameAsString();
    EmitClassRef(SuperClassName);
  }

  // Get the class name
  ObjCInterfaceDecl *ClassDecl =
      const_cast<ObjCInterfaceDecl *>(OID->getClassInterface());
  std::string ClassName = ClassDecl->getNameAsString();

  // Emit the symbol that is used to generate linker errors if this class is
  // referenced in other modules but not declared.
  std::string classSymbolName = "__objc_class_name_" + ClassName;
  if (auto *symbol = TheModule.getGlobalVariable(classSymbolName)) {
    symbol->setInitializer(llvm::ConstantInt::get(LongTy, 0));
  } else {
    new llvm::GlobalVariable(TheModule, LongTy, false,
                             llvm::GlobalValue::ExternalLinkage,
                             llvm::ConstantInt::get(LongTy, 0),
                             classSymbolName);
  }

  // Get the size of instances.
  int instanceSize = 
    Context.getASTObjCImplementationLayout(OID).getSize().getQuantity();

  // Collect information about instance variables.
  SmallVector<llvm::Constant*, 16> IvarNames;
  SmallVector<llvm::Constant*, 16> IvarTypes;
  SmallVector<llvm::Constant*, 16> IvarOffsets;

  ConstantInitBuilder IvarOffsetBuilder(CGM);
  auto IvarOffsetValues = IvarOffsetBuilder.beginArray(PtrToIntTy);
  SmallVector<bool, 16> WeakIvars;
  SmallVector<bool, 16> StrongIvars;

  int superInstanceSize = !SuperClassDecl ? 0 :
    Context.getASTObjCInterfaceLayout(SuperClassDecl).getSize().getQuantity();
  // For non-fragile ivars, set the instance size to 0 - {the size of just this
  // class}.  The runtime will then set this to the correct value on load.
  if (CGM.getLangOpts().ObjCRuntime.isNonFragile()) {
    instanceSize = 0 - (instanceSize - superInstanceSize);
  }

  for (const ObjCIvarDecl *IVD = ClassDecl->all_declared_ivar_begin(); IVD;
       IVD = IVD->getNextIvar()) {
      // Store the name
      IvarNames.push_back(MakeConstantString(IVD->getNameAsString()));
      // Get the type encoding for this ivar
      std::string TypeStr;
      Context.getObjCEncodingForType(IVD->getType(), TypeStr, IVD);
      IvarTypes.push_back(MakeConstantString(TypeStr));
      // Get the offset
      uint64_t BaseOffset = ComputeIvarBaseOffset(CGM, OID, IVD);
      uint64_t Offset = BaseOffset;
      if (CGM.getLangOpts().ObjCRuntime.isNonFragile()) {
        Offset = BaseOffset - superInstanceSize;
      }
      llvm::Constant *OffsetValue = llvm::ConstantInt::get(IntTy, Offset);
      // Create the direct offset value
      std::string OffsetName = "__objc_ivar_offset_value_" + ClassName +"." +
          IVD->getNameAsString();
      llvm::GlobalVariable *OffsetVar = TheModule.getGlobalVariable(OffsetName);
      if (OffsetVar) {
        OffsetVar->setInitializer(OffsetValue);
        // If this is the real definition, change its linkage type so that
        // different modules will use this one, rather than their private
        // copy.
        OffsetVar->setLinkage(llvm::GlobalValue::ExternalLinkage);
      } else
        OffsetVar = new llvm::GlobalVariable(TheModule, IntTy,
          false, llvm::GlobalValue::ExternalLinkage,
          OffsetValue,
          "__objc_ivar_offset_value_" + ClassName +"." +
          IVD->getNameAsString());
      IvarOffsets.push_back(OffsetValue);
      IvarOffsetValues.add(OffsetVar);
      Qualifiers::ObjCLifetime lt = IVD->getType().getQualifiers().getObjCLifetime();
      switch (lt) {
        case Qualifiers::OCL_Strong:
          StrongIvars.push_back(true);
          WeakIvars.push_back(false);
          break;
        case Qualifiers::OCL_Weak:
          StrongIvars.push_back(false);
          WeakIvars.push_back(true);
          break;
        default:
          StrongIvars.push_back(false);
          WeakIvars.push_back(false);
      }
  }
  llvm::Constant *StrongIvarBitmap = MakeBitField(StrongIvars);
  llvm::Constant *WeakIvarBitmap = MakeBitField(WeakIvars);
  llvm::GlobalVariable *IvarOffsetArray =
    IvarOffsetValues.finishAndCreateGlobal(".ivar.offsets",
                                           CGM.getPointerAlign());

  // Collect information about instance methods
  SmallVector<Selector, 16> InstanceMethodSels;
  SmallVector<llvm::Constant*, 16> InstanceMethodTypes;
  for (const auto *I : OID->instance_methods()) {
    InstanceMethodSels.push_back(I->getSelector());
    std::string TypeStr = Context.getObjCEncodingForMethodDecl(I);
    InstanceMethodTypes.push_back(MakeConstantString(TypeStr));
  }

  llvm::Constant *Properties = GeneratePropertyList(OID, InstanceMethodSels,
          InstanceMethodTypes);

  // Collect information about class methods
  SmallVector<Selector, 16> ClassMethodSels;
  SmallVector<llvm::Constant*, 16> ClassMethodTypes;
  for (const auto *I : OID->class_methods()) {
    ClassMethodSels.push_back(I->getSelector());
    std::string TypeStr = Context.getObjCEncodingForMethodDecl(I);
    ClassMethodTypes.push_back(MakeConstantString(TypeStr));
  }
  // Collect the names of referenced protocols
  SmallVector<std::string, 16> Protocols;
  for (const auto *I : ClassDecl->protocols())
    Protocols.push_back(I->getNameAsString());

  // Get the superclass pointer.
  llvm::Constant *SuperClass;
  if (!SuperClassName.empty()) {
    SuperClass = MakeConstantString(SuperClassName, ".super_class_name");
  } else {
    SuperClass = llvm::ConstantPointerNull::get(PtrToInt8Ty);
  }
  // Empty vector used to construct empty method lists
  SmallVector<llvm::Constant*, 1>  empty;
  // Generate the method and instance variable lists
  llvm::Constant *MethodList = GenerateMethodList(ClassName, "",
      InstanceMethodSels, InstanceMethodTypes, false);
  llvm::Constant *ClassMethodList = GenerateMethodList(ClassName, "",
      ClassMethodSels, ClassMethodTypes, true);
  llvm::Constant *IvarList = GenerateIvarList(IvarNames, IvarTypes,
      IvarOffsets);
  // Irrespective of whether we are compiling for a fragile or non-fragile ABI,
  // we emit a symbol containing the offset for each ivar in the class.  This
  // allows code compiled for the non-Fragile ABI to inherit from code compiled
  // for the legacy ABI, without causing problems.  The converse is also
  // possible, but causes all ivar accesses to be fragile.

  // Offset pointer for getting at the correct field in the ivar list when
  // setting up the alias.  These are: The base address for the global, the
  // ivar array (second field), the ivar in this list (set for each ivar), and
  // the offset (third field in ivar structure)
  llvm::Type *IndexTy = Int32Ty;
  llvm::Constant *offsetPointerIndexes[] = {Zeros[0],
      llvm::ConstantInt::get(IndexTy, 1), nullptr,
      llvm::ConstantInt::get(IndexTy, 2) };

  unsigned ivarIndex = 0;
  for (const ObjCIvarDecl *IVD = ClassDecl->all_declared_ivar_begin(); IVD;
       IVD = IVD->getNextIvar()) {
      const std::string Name = "__objc_ivar_offset_" + ClassName + '.'
          + IVD->getNameAsString();
      offsetPointerIndexes[2] = llvm::ConstantInt::get(IndexTy, ivarIndex);
      // Get the correct ivar field
      llvm::Constant *offsetValue = llvm::ConstantExpr::getGetElementPtr(
          cast<llvm::GlobalVariable>(IvarList)->getValueType(), IvarList,
          offsetPointerIndexes);
      // Get the existing variable, if one exists.
      llvm::GlobalVariable *offset = TheModule.getNamedGlobal(Name);
      if (offset) {
        offset->setInitializer(offsetValue);
        // If this is the real definition, change its linkage type so that
        // different modules will use this one, rather than their private
        // copy.
        offset->setLinkage(llvm::GlobalValue::ExternalLinkage);
      } else {
        // Add a new alias if there isn't one already.
        offset = new llvm::GlobalVariable(TheModule, offsetValue->getType(),
                false, llvm::GlobalValue::ExternalLinkage, offsetValue, Name);
        (void) offset; // Silence dead store warning.
      }
      ++ivarIndex;
  }
  llvm::Constant *ZeroPtr = llvm::ConstantInt::get(IntPtrTy, 0);

  //Generate metaclass for class methods
  llvm::Constant *MetaClassStruct = GenerateClassStructure(
      NULLPtr, NULLPtr, 0x12L, ClassName.c_str(), nullptr, Zeros[0],
      GenerateIvarList(empty, empty, empty), ClassMethodList, NULLPtr, NULLPtr,
      NULLPtr, ZeroPtr, ZeroPtr, true);
  if (CGM.getTriple().isOSBinFormatCOFF()) {
    auto Storage = llvm::GlobalValue::DefaultStorageClass;
    if (OID->getClassInterface()->hasAttr<DLLImportAttr>())
      Storage = llvm::GlobalValue::DLLImportStorageClass;
    else if (OID->getClassInterface()->hasAttr<DLLExportAttr>())
      Storage = llvm::GlobalValue::DLLExportStorageClass;
    cast<llvm::GlobalValue>(MetaClassStruct)->setDLLStorageClass(Storage);
  }

  // Generate the class structure
  llvm::Constant *ClassStruct = GenerateClassStructure(
      MetaClassStruct, SuperClass, 0x11L, ClassName.c_str(), nullptr,
      llvm::ConstantInt::get(LongTy, instanceSize), IvarList, MethodList,
      GenerateProtocolList(Protocols), IvarOffsetArray, Properties,
      StrongIvarBitmap, WeakIvarBitmap);
  if (CGM.getTriple().isOSBinFormatCOFF()) {
    auto Storage = llvm::GlobalValue::DefaultStorageClass;
    if (OID->getClassInterface()->hasAttr<DLLImportAttr>())
      Storage = llvm::GlobalValue::DLLImportStorageClass;
    else if (OID->getClassInterface()->hasAttr<DLLExportAttr>())
      Storage = llvm::GlobalValue::DLLExportStorageClass;
    cast<llvm::GlobalValue>(ClassStruct)->setDLLStorageClass(Storage);
  }

  // Resolve the class aliases, if they exist.
  if (ClassPtrAlias) {
    ClassPtrAlias->replaceAllUsesWith(
        llvm::ConstantExpr::getBitCast(ClassStruct, IdTy));
    ClassPtrAlias->eraseFromParent();
    ClassPtrAlias = nullptr;
  }
  if (MetaClassPtrAlias) {
    MetaClassPtrAlias->replaceAllUsesWith(
        llvm::ConstantExpr::getBitCast(MetaClassStruct, IdTy));
    MetaClassPtrAlias->eraseFromParent();
    MetaClassPtrAlias = nullptr;
  }

  // Add class structure to list to be added to the symtab later
  ClassStruct = llvm::ConstantExpr::getBitCast(ClassStruct, PtrToInt8Ty);
  Classes.push_back(ClassStruct);
}

llvm::Function *CGObjCGNU::ModuleInitFunction() {
  // Only emit an ObjC load function if no Objective-C stuff has been called
  if (Classes.empty() && Categories.empty() && ConstantStrings.empty() &&
      ExistingProtocols.empty() && SelectorTable.empty())
    return nullptr;

  // Add all referenced protocols to a category.
  GenerateProtocolHolderCategory();

  llvm::StructType *selStructTy =
    dyn_cast<llvm::StructType>(SelectorTy->getElementType());
  llvm::Type *selStructPtrTy = SelectorTy;
  if (!selStructTy) {
    selStructTy = llvm::StructType::get(CGM.getLLVMContext(),
                                        { PtrToInt8Ty, PtrToInt8Ty });
    selStructPtrTy = llvm::PointerType::getUnqual(selStructTy);
  }

  // Generate statics list:
  llvm::Constant *statics = NULLPtr;
  if (!ConstantStrings.empty()) {
    llvm::GlobalVariable *fileStatics = [&] {
      ConstantInitBuilder builder(CGM);
      auto staticsStruct = builder.beginStruct();

      StringRef stringClass = CGM.getLangOpts().ObjCConstantStringClass;
      if (stringClass.empty()) stringClass = "NXConstantString";
      staticsStruct.add(MakeConstantString(stringClass,
                                           ".objc_static_class_name"));

      auto array = staticsStruct.beginArray();
      array.addAll(ConstantStrings);
      array.add(NULLPtr);
      array.finishAndAddTo(staticsStruct);

      return staticsStruct.finishAndCreateGlobal(".objc_statics",
                                                 CGM.getPointerAlign());
    }();

    ConstantInitBuilder builder(CGM);
    auto allStaticsArray = builder.beginArray(fileStatics->getType());
    allStaticsArray.add(fileStatics);
    allStaticsArray.addNullPointer(fileStatics->getType());

    statics = allStaticsArray.finishAndCreateGlobal(".objc_statics_ptr",
                                                    CGM.getPointerAlign());
    statics = llvm::ConstantExpr::getBitCast(statics, PtrTy);
  }

  // Array of classes, categories, and constant objects.

  SmallVector<llvm::GlobalAlias*, 16> selectorAliases;
  unsigned selectorCount;

  // Pointer to an array of selectors used in this module.
  llvm::GlobalVariable *selectorList = [&] {
    ConstantInitBuilder builder(CGM);
    auto selectors = builder.beginArray(selStructTy);
    auto &table = SelectorTable; // MSVC workaround
    for (auto &entry : table) {

      std::string selNameStr = entry.first.getAsString();
      llvm::Constant *selName = ExportUniqueString(selNameStr, ".objc_sel_name");

      for (TypedSelector &sel : entry.second) {
        llvm::Constant *selectorTypeEncoding = NULLPtr;
        if (!sel.first.empty())
          selectorTypeEncoding =
            MakeConstantString(sel.first, ".objc_sel_types");

        auto selStruct = selectors.beginStruct(selStructTy);
        selStruct.add(selName);
        selStruct.add(selectorTypeEncoding);
        selStruct.finishAndAddTo(selectors);

        // Store the selector alias for later replacement
        selectorAliases.push_back(sel.second);
      }
    }

    // Remember the number of entries in the selector table.
    selectorCount = selectors.size();

    // NULL-terminate the selector list.  This should not actually be required,
    // because the selector list has a length field.  Unfortunately, the GCC
    // runtime decides to ignore the length field and expects a NULL terminator,
    // and GCC cooperates with this by always setting the length to 0.
    auto selStruct = selectors.beginStruct(selStructTy);
    selStruct.add(NULLPtr);
    selStruct.add(NULLPtr);
    selStruct.finishAndAddTo(selectors);

    return selectors.finishAndCreateGlobal(".objc_selector_list",
                                           CGM.getPointerAlign());
  }();

  // Now that all of the static selectors exist, create pointers to them.
  for (unsigned i = 0; i < selectorCount; ++i) {
    llvm::Constant *idxs[] = {
      Zeros[0],
      llvm::ConstantInt::get(Int32Ty, i)
    };
    // FIXME: We're generating redundant loads and stores here!
    llvm::Constant *selPtr = llvm::ConstantExpr::getGetElementPtr(
        selectorList->getValueType(), selectorList, idxs);
    // If selectors are defined as an opaque type, cast the pointer to this
    // type.
    selPtr = llvm::ConstantExpr::getBitCast(selPtr, SelectorTy);
    selectorAliases[i]->replaceAllUsesWith(selPtr);
    selectorAliases[i]->eraseFromParent();
  }

  llvm::GlobalVariable *symtab = [&] {
    ConstantInitBuilder builder(CGM);
    auto symtab = builder.beginStruct();

    // Number of static selectors
    symtab.addInt(LongTy, selectorCount);

    symtab.addBitCast(selectorList, selStructPtrTy);

    // Number of classes defined.
    symtab.addInt(CGM.Int16Ty, Classes.size());
    // Number of categories defined
    symtab.addInt(CGM.Int16Ty, Categories.size());

    // Create an array of classes, then categories, then static object instances
    auto classList = symtab.beginArray(PtrToInt8Ty);
    classList.addAll(Classes);
    classList.addAll(Categories);
    //  NULL-terminated list of static object instances (mainly constant strings)
    classList.add(statics);
    classList.add(NULLPtr);
    classList.finishAndAddTo(symtab);

    // Construct the symbol table.
    return symtab.finishAndCreateGlobal("", CGM.getPointerAlign());
  }();

  // The symbol table is contained in a module which has some version-checking
  // constants
  llvm::Constant *module = [&] {
    llvm::Type *moduleEltTys[] = {
      LongTy, LongTy, PtrToInt8Ty, symtab->getType(), IntTy
    };
    llvm::StructType *moduleTy =
      llvm::StructType::get(CGM.getLLVMContext(),
         makeArrayRef(moduleEltTys).drop_back(unsigned(RuntimeVersion < 10)));

    ConstantInitBuilder builder(CGM);
    auto module = builder.beginStruct(moduleTy);
    // Runtime version, used for ABI compatibility checking.
    module.addInt(LongTy, RuntimeVersion);
    // sizeof(ModuleTy)
    module.addInt(LongTy, CGM.getDataLayout().getTypeStoreSize(moduleTy));

    // The path to the source file where this module was declared
    SourceManager &SM = CGM.getContext().getSourceManager();
    const FileEntry *mainFile = SM.getFileEntryForID(SM.getMainFileID());
    std::string path =
      (Twine(mainFile->getDir()->getName()) + "/" + mainFile->getName()).str();
    module.add(MakeConstantString(path, ".objc_source_file_name"));
    module.add(symtab);

    if (RuntimeVersion >= 10) {
      switch (CGM.getLangOpts().getGC()) {
      case LangOptions::GCOnly:
        module.addInt(IntTy, 2);
        break;
      case LangOptions::NonGC:
        if (CGM.getLangOpts().ObjCAutoRefCount)
          module.addInt(IntTy, 1);
        else
          module.addInt(IntTy, 0);
        break;
      case LangOptions::HybridGC:
        module.addInt(IntTy, 1);
        break;
      }
    }

    return module.finishAndCreateGlobal("", CGM.getPointerAlign());
  }();

  // Create the load function calling the runtime entry point with the module
  // structure
  llvm::Function * LoadFunction = llvm::Function::Create(
      llvm::FunctionType::get(llvm::Type::getVoidTy(VMContext), false),
      llvm::GlobalValue::InternalLinkage, ".objc_load_function",
      &TheModule);
  llvm::BasicBlock *EntryBB =
      llvm::BasicBlock::Create(VMContext, "entry", LoadFunction);
  CGBuilderTy Builder(CGM, VMContext);
  Builder.SetInsertPoint(EntryBB);

  llvm::FunctionType *FT =
    llvm::FunctionType::get(Builder.getVoidTy(), module->getType(), true);
  llvm::Value *Register = CGM.CreateRuntimeFunction(FT, "__objc_exec_class");
  Builder.CreateCall(Register, module);

  if (!ClassAliases.empty()) {
    llvm::Type *ArgTypes[2] = {PtrTy, PtrToInt8Ty};
    llvm::FunctionType *RegisterAliasTy =
      llvm::FunctionType::get(Builder.getVoidTy(),
                              ArgTypes, false);
    llvm::Function *RegisterAlias = llvm::Function::Create(
      RegisterAliasTy,
      llvm::GlobalValue::ExternalWeakLinkage, "class_registerAlias_np",
      &TheModule);
    llvm::BasicBlock *AliasBB =
      llvm::BasicBlock::Create(VMContext, "alias", LoadFunction);
    llvm::BasicBlock *NoAliasBB =
      llvm::BasicBlock::Create(VMContext, "no_alias", LoadFunction);

    // Branch based on whether the runtime provided class_registerAlias_np()
    llvm::Value *HasRegisterAlias = Builder.CreateICmpNE(RegisterAlias,
            llvm::Constant::getNullValue(RegisterAlias->getType()));
    Builder.CreateCondBr(HasRegisterAlias, AliasBB, NoAliasBB);

    // The true branch (has alias registration function):
    Builder.SetInsertPoint(AliasBB);
    // Emit alias registration calls:
    for (std::vector<ClassAliasPair>::iterator iter = ClassAliases.begin();
       iter != ClassAliases.end(); ++iter) {
       llvm::Constant *TheClass =
          TheModule.getGlobalVariable("_OBJC_CLASS_" + iter->first, true);
       if (TheClass) {
         TheClass = llvm::ConstantExpr::getBitCast(TheClass, PtrTy);
         Builder.CreateCall(RegisterAlias,
                            {TheClass, MakeConstantString(iter->second)});
       }
    }
    // Jump to end:
    Builder.CreateBr(NoAliasBB);

    // Missing alias registration function, just return from the function:
    Builder.SetInsertPoint(NoAliasBB);
  }
  Builder.CreateRetVoid();

  return LoadFunction;
}

llvm::Function *CGObjCGNU::GenerateMethod(const ObjCMethodDecl *OMD,
                                          const ObjCContainerDecl *CD) {
  const ObjCCategoryImplDecl *OCD =
    dyn_cast<ObjCCategoryImplDecl>(OMD->getDeclContext());
  StringRef CategoryName = OCD ? OCD->getName() : "";
  StringRef ClassName = CD->getName();
  Selector MethodName = OMD->getSelector();
  bool isClassMethod = !OMD->isInstanceMethod();

  CodeGenTypes &Types = CGM.getTypes();
  llvm::FunctionType *MethodTy =
    Types.GetFunctionType(Types.arrangeObjCMethodDeclaration(OMD));
  std::string FunctionName = SymbolNameForMethod(ClassName, CategoryName,
      MethodName, isClassMethod);

  llvm::Function *Method
    = llvm::Function::Create(MethodTy,
                             llvm::GlobalValue::InternalLinkage,
                             FunctionName,
                             &TheModule);
  return Method;
}

llvm::Constant *CGObjCGNU::GetPropertyGetFunction() {
  return GetPropertyFn;
}

llvm::Constant *CGObjCGNU::GetPropertySetFunction() {
  return SetPropertyFn;
}

llvm::Constant *CGObjCGNU::GetOptimizedPropertySetFunction(bool atomic,
                                                           bool copy) {
  return nullptr;
}

llvm::Constant *CGObjCGNU::GetGetStructFunction() {
  return GetStructPropertyFn;
}

llvm::Constant *CGObjCGNU::GetSetStructFunction() {
  return SetStructPropertyFn;
}

llvm::Constant *CGObjCGNU::GetCppAtomicObjectGetFunction() {
  return nullptr;
}

llvm::Constant *CGObjCGNU::GetCppAtomicObjectSetFunction() {
  return nullptr;
}

llvm::Constant *CGObjCGNU::EnumerationMutationFunction() {
  return EnumerationMutationFn;
}

void CGObjCGNU::EmitSynchronizedStmt(CodeGenFunction &CGF,
                                     const ObjCAtSynchronizedStmt &S) {
  EmitAtSynchronizedStmt(CGF, S, SyncEnterFn, SyncExitFn);
}


void CGObjCGNU::EmitTryStmt(CodeGenFunction &CGF,
                            const ObjCAtTryStmt &S) {
  // Unlike the Apple non-fragile runtimes, which also uses
  // unwind-based zero cost exceptions, the GNU Objective C runtime's
  // EH support isn't a veneer over C++ EH.  Instead, exception
  // objects are created by objc_exception_throw and destroyed by
  // the personality function; this avoids the need for bracketing
  // catch handlers with calls to __blah_begin_catch/__blah_end_catch
  // (or even _Unwind_DeleteException), but probably doesn't
  // interoperate very well with foreign exceptions.
  //
  // In Objective-C++ mode, we actually emit something equivalent to the C++
  // exception handler. 
  EmitTryCatchStmt(CGF, S, EnterCatchFn, ExitCatchFn, ExceptionReThrowFn);
}

void CGObjCGNU::EmitThrowStmt(CodeGenFunction &CGF,
                              const ObjCAtThrowStmt &S,
                              bool ClearInsertionPoint) {
  llvm::Value *ExceptionAsObject;

  if (const Expr *ThrowExpr = S.getThrowExpr()) {
    llvm::Value *Exception = CGF.EmitObjCThrowOperand(ThrowExpr);
    ExceptionAsObject = Exception;
  } else {
    assert((!CGF.ObjCEHValueStack.empty() && CGF.ObjCEHValueStack.back()) &&
           "Unexpected rethrow outside @catch block.");
    ExceptionAsObject = CGF.ObjCEHValueStack.back();
  }
  ExceptionAsObject = CGF.Builder.CreateBitCast(ExceptionAsObject, IdTy);
  llvm::CallSite Throw =
      CGF.EmitRuntimeCallOrInvoke(ExceptionThrowFn, ExceptionAsObject);
  Throw.setDoesNotReturn();
  CGF.Builder.CreateUnreachable();
  if (ClearInsertionPoint)
    CGF.Builder.ClearInsertionPoint();
}

llvm::Value * CGObjCGNU::EmitObjCWeakRead(CodeGenFunction &CGF,
                                          Address AddrWeakObj) {
  CGBuilderTy &B = CGF.Builder;
  AddrWeakObj = EnforceType(B, AddrWeakObj, PtrToIdTy);
  return B.CreateCall(WeakReadFn.getType(), WeakReadFn,
                      AddrWeakObj.getPointer());
}

void CGObjCGNU::EmitObjCWeakAssign(CodeGenFunction &CGF,
                                   llvm::Value *src, Address dst) {
  CGBuilderTy &B = CGF.Builder;
  src = EnforceType(B, src, IdTy);
  dst = EnforceType(B, dst, PtrToIdTy);
  B.CreateCall(WeakAssignFn.getType(), WeakAssignFn,
               {src, dst.getPointer()});
}

void CGObjCGNU::EmitObjCGlobalAssign(CodeGenFunction &CGF,
                                     llvm::Value *src, Address dst,
                                     bool threadlocal) {
  CGBuilderTy &B = CGF.Builder;
  src = EnforceType(B, src, IdTy);
  dst = EnforceType(B, dst, PtrToIdTy);
  // FIXME. Add threadloca assign API
  assert(!threadlocal && "EmitObjCGlobalAssign - Threal Local API NYI");
  B.CreateCall(GlobalAssignFn.getType(), GlobalAssignFn,
               {src, dst.getPointer()});
}

void CGObjCGNU::EmitObjCIvarAssign(CodeGenFunction &CGF,
                                   llvm::Value *src, Address dst,
                                   llvm::Value *ivarOffset) {
  CGBuilderTy &B = CGF.Builder;
  src = EnforceType(B, src, IdTy);
  dst = EnforceType(B, dst, IdTy);
  B.CreateCall(IvarAssignFn.getType(), IvarAssignFn,
               {src, dst.getPointer(), ivarOffset});
}

void CGObjCGNU::EmitObjCStrongCastAssign(CodeGenFunction &CGF,
                                         llvm::Value *src, Address dst) {
  CGBuilderTy &B = CGF.Builder;
  src = EnforceType(B, src, IdTy);
  dst = EnforceType(B, dst, PtrToIdTy);
  B.CreateCall(StrongCastAssignFn.getType(), StrongCastAssignFn,
               {src, dst.getPointer()});
}

void CGObjCGNU::EmitGCMemmoveCollectable(CodeGenFunction &CGF,
                                         Address DestPtr,
                                         Address SrcPtr,
                                         llvm::Value *Size) {
  CGBuilderTy &B = CGF.Builder;
  DestPtr = EnforceType(B, DestPtr, PtrTy);
  SrcPtr = EnforceType(B, SrcPtr, PtrTy);

  B.CreateCall(MemMoveFn.getType(), MemMoveFn,
               {DestPtr.getPointer(), SrcPtr.getPointer(), Size});
}

llvm::GlobalVariable *CGObjCGNU::ObjCIvarOffsetVariable(
                              const ObjCInterfaceDecl *ID,
                              const ObjCIvarDecl *Ivar) {
  const std::string Name = "__objc_ivar_offset_" + ID->getNameAsString()
    + '.' + Ivar->getNameAsString();
  // Emit the variable and initialize it with what we think the correct value
  // is.  This allows code compiled with non-fragile ivars to work correctly
  // when linked against code which isn't (most of the time).
  llvm::GlobalVariable *IvarOffsetPointer = TheModule.getNamedGlobal(Name);
  if (!IvarOffsetPointer) {
    // This will cause a run-time crash if we accidentally use it.  A value of
    // 0 would seem more sensible, but will silently overwrite the isa pointer
    // causing a great deal of confusion.
    uint64_t Offset = -1;
    // We can't call ComputeIvarBaseOffset() here if we have the
    // implementation, because it will create an invalid ASTRecordLayout object
    // that we are then stuck with forever, so we only initialize the ivar
    // offset variable with a guess if we only have the interface.  The
    // initializer will be reset later anyway, when we are generating the class
    // description.
    if (!CGM.getContext().getObjCImplementation(
              const_cast<ObjCInterfaceDecl *>(ID)))
      Offset = ComputeIvarBaseOffset(CGM, ID, Ivar);

    llvm::ConstantInt *OffsetGuess = llvm::ConstantInt::get(Int32Ty, Offset,
                             /*isSigned*/true);
    // Don't emit the guess in non-PIC code because the linker will not be able
    // to replace it with the real version for a library.  In non-PIC code you
    // must compile with the fragile ABI if you want to use ivars from a
    // GCC-compiled class.
    if (CGM.getLangOpts().PICLevel) {
      llvm::GlobalVariable *IvarOffsetGV = new llvm::GlobalVariable(TheModule,
            Int32Ty, false,
            llvm::GlobalValue::PrivateLinkage, OffsetGuess, Name+".guess");
      IvarOffsetPointer = new llvm::GlobalVariable(TheModule,
            IvarOffsetGV->getType(), false, llvm::GlobalValue::LinkOnceAnyLinkage,
            IvarOffsetGV, Name);
    } else {
      IvarOffsetPointer = new llvm::GlobalVariable(TheModule,
              llvm::Type::getInt32PtrTy(VMContext), false,
              llvm::GlobalValue::ExternalLinkage, nullptr, Name);
    }
  }
  return IvarOffsetPointer;
}

LValue CGObjCGNU::EmitObjCValueForIvar(CodeGenFunction &CGF,
                                       QualType ObjectTy,
                                       llvm::Value *BaseValue,
                                       const ObjCIvarDecl *Ivar,
                                       unsigned CVRQualifiers) {
  const ObjCInterfaceDecl *ID =
    ObjectTy->getAs<ObjCObjectType>()->getInterface();
  return EmitValueForIvarAtOffset(CGF, ID, BaseValue, Ivar, CVRQualifiers,
                                  EmitIvarOffset(CGF, ID, Ivar));
}

static const ObjCInterfaceDecl *FindIvarInterface(ASTContext &Context,
                                                  const ObjCInterfaceDecl *OID,
                                                  const ObjCIvarDecl *OIVD) {
  for (const ObjCIvarDecl *next = OID->all_declared_ivar_begin(); next;
       next = next->getNextIvar()) {
    if (OIVD == next)
      return OID;
  }

  // Otherwise check in the super class.
  if (const ObjCInterfaceDecl *Super = OID->getSuperClass())
    return FindIvarInterface(Context, Super, OIVD);

  return nullptr;
}

llvm::Value *CGObjCGNU::EmitIvarOffset(CodeGenFunction &CGF,
                         const ObjCInterfaceDecl *Interface,
                         const ObjCIvarDecl *Ivar) {
  if (CGM.getLangOpts().ObjCRuntime.isNonFragile()) {
    Interface = FindIvarInterface(CGM.getContext(), Interface, Ivar);

    // The MSVC linker cannot have a single global defined as LinkOnceAnyLinkage
    // and ExternalLinkage, so create a reference to the ivar global and rely on
    // the definition being created as part of GenerateClass.
    if (RuntimeVersion < 10 ||
        CGF.CGM.getTarget().getTriple().isKnownWindowsMSVCEnvironment())
      return CGF.Builder.CreateZExtOrBitCast(
          CGF.Builder.CreateAlignedLoad(
              Int32Ty, CGF.Builder.CreateAlignedLoad(
                           ObjCIvarOffsetVariable(Interface, Ivar),
                           CGF.getPointerAlign(), "ivar"),
              CharUnits::fromQuantity(4)),
          PtrDiffTy);
    std::string name = "__objc_ivar_offset_value_" +
      Interface->getNameAsString() +"." + Ivar->getNameAsString();
    CharUnits Align = CGM.getIntAlign();
    llvm::Value *Offset = TheModule.getGlobalVariable(name);
    if (!Offset) {
      auto GV = new llvm::GlobalVariable(TheModule, IntTy,
          false, llvm::GlobalValue::LinkOnceAnyLinkage,
          llvm::Constant::getNullValue(IntTy), name);
      GV->setAlignment(Align.getQuantity());
      Offset = GV;
    }
    Offset = CGF.Builder.CreateAlignedLoad(Offset, Align);
    if (Offset->getType() != PtrDiffTy)
      Offset = CGF.Builder.CreateZExtOrBitCast(Offset, PtrDiffTy);
    return Offset;
  }
  uint64_t Offset = ComputeIvarBaseOffset(CGF.CGM, Interface, Ivar);
  return llvm::ConstantInt::get(PtrDiffTy, Offset, /*isSigned*/true);
}

CGObjCRuntime *
clang::CodeGen::CreateGNUObjCRuntime(CodeGenModule &CGM) {
  switch (CGM.getLangOpts().ObjCRuntime.getKind()) {
  case ObjCRuntime::GNUstep:
    return new CGObjCGNUstep(CGM);

  case ObjCRuntime::GCC:
    return new CGObjCGCC(CGM);

  case ObjCRuntime::ObjFW:
    return new CGObjCObjFW(CGM);

  case ObjCRuntime::FragileMacOSX:
  case ObjCRuntime::MacOSX:
  case ObjCRuntime::iOS:
  case ObjCRuntime::WatchOS:
    llvm_unreachable("these runtimes are not GNU runtimes");
  }
  llvm_unreachable("bad runtime");
}
