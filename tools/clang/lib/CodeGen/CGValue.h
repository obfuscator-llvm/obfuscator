//===-- CGValue.h - LLVM CodeGen wrappers for llvm::Value* ------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// These classes implement wrappers around llvm::Value in order to
// fully represent the range of values for C L- and R- values.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_LIB_CODEGEN_CGVALUE_H
#define LLVM_CLANG_LIB_CODEGEN_CGVALUE_H

#include "clang/AST/ASTContext.h"
#include "clang/AST/Type.h"
#include "llvm/IR/Value.h"
#include "llvm/IR/Type.h"
#include "Address.h"

namespace llvm {
  class Constant;
  class MDNode;
}

namespace clang {
namespace CodeGen {
  class AggValueSlot;
  struct CGBitFieldInfo;

/// RValue - This trivial value class is used to represent the result of an
/// expression that is evaluated.  It can be one of three things: either a
/// simple LLVM SSA value, a pair of SSA values for complex numbers, or the
/// address of an aggregate value in memory.
class RValue {
  enum Flavor { Scalar, Complex, Aggregate };

  // The shift to make to an aggregate's alignment to make it look
  // like a pointer.
  enum { AggAlignShift = 4 };

  // Stores first value and flavor.
  llvm::PointerIntPair<llvm::Value *, 2, Flavor> V1;
  // Stores second value and volatility.
  llvm::PointerIntPair<llvm::Value *, 1, bool> V2;

public:
  bool isScalar() const { return V1.getInt() == Scalar; }
  bool isComplex() const { return V1.getInt() == Complex; }
  bool isAggregate() const { return V1.getInt() == Aggregate; }

  bool isVolatileQualified() const { return V2.getInt(); }

  /// getScalarVal() - Return the Value* of this scalar value.
  llvm::Value *getScalarVal() const {
    assert(isScalar() && "Not a scalar!");
    return V1.getPointer();
  }

  /// getComplexVal - Return the real/imag components of this complex value.
  ///
  std::pair<llvm::Value *, llvm::Value *> getComplexVal() const {
    return std::make_pair(V1.getPointer(), V2.getPointer());
  }

  /// getAggregateAddr() - Return the Value* of the address of the aggregate.
  Address getAggregateAddress() const {
    assert(isAggregate() && "Not an aggregate!");
    auto align = reinterpret_cast<uintptr_t>(V2.getPointer()) >> AggAlignShift;
    return Address(V1.getPointer(), CharUnits::fromQuantity(align));
  }
  llvm::Value *getAggregatePointer() const {
    assert(isAggregate() && "Not an aggregate!");
    return V1.getPointer();
  }

  static RValue getIgnored() {
    // FIXME: should we make this a more explicit state?
    return get(nullptr);
  }

  static RValue get(llvm::Value *V) {
    RValue ER;
    ER.V1.setPointer(V);
    ER.V1.setInt(Scalar);
    ER.V2.setInt(false);
    return ER;
  }
  static RValue getComplex(llvm::Value *V1, llvm::Value *V2) {
    RValue ER;
    ER.V1.setPointer(V1);
    ER.V2.setPointer(V2);
    ER.V1.setInt(Complex);
    ER.V2.setInt(false);
    return ER;
  }
  static RValue getComplex(const std::pair<llvm::Value *, llvm::Value *> &C) {
    return getComplex(C.first, C.second);
  }
  // FIXME: Aggregate rvalues need to retain information about whether they are
  // volatile or not.  Remove default to find all places that probably get this
  // wrong.
  static RValue getAggregate(Address addr, bool isVolatile = false) {
    RValue ER;
    ER.V1.setPointer(addr.getPointer());
    ER.V1.setInt(Aggregate);

    auto align = static_cast<uintptr_t>(addr.getAlignment().getQuantity());
    ER.V2.setPointer(reinterpret_cast<llvm::Value*>(align << AggAlignShift));
    ER.V2.setInt(isVolatile);
    return ER;
  }
};

/// Does an ARC strong l-value have precise lifetime?
enum ARCPreciseLifetime_t {
  ARCImpreciseLifetime, ARCPreciseLifetime
};

/// The source of the alignment of an l-value; an expression of
/// confidence in the alignment actually matching the estimate.
enum class AlignmentSource {
  /// The l-value was an access to a declared entity or something
  /// equivalently strong, like the address of an array allocated by a
  /// language runtime.
  Decl,

  /// The l-value was considered opaque, so the alignment was
  /// determined from a type, but that type was an explicitly-aligned
  /// typedef.
  AttributedType,

  /// The l-value was considered opaque, so the alignment was
  /// determined from a type.
  Type
};

/// Given that the base address has the given alignment source, what's
/// our confidence in the alignment of the field?
static inline AlignmentSource getFieldAlignmentSource(AlignmentSource Source) {
  // For now, we don't distinguish fields of opaque pointers from
  // top-level declarations, but maybe we should.
  return AlignmentSource::Decl;
}

class LValueBaseInfo {
  AlignmentSource AlignSource;
  bool MayAlias;

public:
  explicit LValueBaseInfo(AlignmentSource Source = AlignmentSource::Type,
                 bool Alias = false)
    : AlignSource(Source), MayAlias(Alias) {}
  AlignmentSource getAlignmentSource() const { return AlignSource; }
  void setAlignmentSource(AlignmentSource Source) { AlignSource = Source; }
  bool getMayAlias() const { return MayAlias; }
  void setMayAlias(bool Alias) { MayAlias = Alias; }

  void mergeForCast(const LValueBaseInfo &Info) {
    setAlignmentSource(Info.getAlignmentSource());
    setMayAlias(getMayAlias() || Info.getMayAlias());
  }
};

/// LValue - This represents an lvalue references.  Because C/C++ allow
/// bitfields, this is not a simple LLVM pointer, it may be a pointer plus a
/// bitrange.
class LValue {
  enum {
    Simple,       // This is a normal l-value, use getAddress().
    VectorElt,    // This is a vector element l-value (V[i]), use getVector*
    BitField,     // This is a bitfield l-value, use getBitfield*.
    ExtVectorElt, // This is an extended vector subset, use getExtVectorComp
    GlobalReg     // This is a register l-value, use getGlobalReg()
  } LVType;

  llvm::Value *V;

  union {
    // Index into a vector subscript: V[i]
    llvm::Value *VectorIdx;

    // ExtVector element subset: V.xyx
    llvm::Constant *VectorElts;

    // BitField start bit and size
    const CGBitFieldInfo *BitFieldInfo;
  };

  QualType Type;

  // 'const' is unused here
  Qualifiers Quals;

  // The alignment to use when accessing this lvalue.  (For vector elements,
  // this is the alignment of the whole vector.)
  int64_t Alignment;

  // objective-c's ivar
  bool Ivar:1;
  
  // objective-c's ivar is an array
  bool ObjIsArray:1;

  // LValue is non-gc'able for any reason, including being a parameter or local
  // variable.
  bool NonGC: 1;

  // Lvalue is a global reference of an objective-c object
  bool GlobalObjCRef : 1;
  
  // Lvalue is a thread local reference
  bool ThreadLocalRef : 1;

  // Lvalue has ARC imprecise lifetime.  We store this inverted to try
  // to make the default bitfield pattern all-zeroes.
  bool ImpreciseLifetime : 1;

  LValueBaseInfo BaseInfo;

  // This flag shows if a nontemporal load/stores should be used when accessing
  // this lvalue.
  bool Nontemporal : 1;

  Expr *BaseIvarExp;

  /// Used by struct-path-aware TBAA.
  QualType TBAABaseType;
  /// Offset relative to the base type.
  uint64_t TBAAOffset;

  /// TBAAInfo - TBAA information to attach to dereferences of this LValue.
  llvm::MDNode *TBAAInfo;

private:
  void Initialize(QualType Type, Qualifiers Quals,
                  CharUnits Alignment, LValueBaseInfo BaseInfo,
                  llvm::MDNode *TBAAInfo = nullptr) {
    assert((!Alignment.isZero() || Type->isIncompleteType()) &&
           "initializing l-value with zero alignment!");
    this->Type = Type;
    this->Quals = Quals;
    this->Alignment = Alignment.getQuantity();
    assert(this->Alignment == Alignment.getQuantity() &&
           "Alignment exceeds allowed max!");
    this->BaseInfo = BaseInfo;

    // Initialize Objective-C flags.
    this->Ivar = this->ObjIsArray = this->NonGC = this->GlobalObjCRef = false;
    this->ImpreciseLifetime = false;
    this->Nontemporal = false;
    this->ThreadLocalRef = false;
    this->BaseIvarExp = nullptr;

    // Initialize fields for TBAA.
    this->TBAABaseType = Type;
    this->TBAAOffset = 0;
    this->TBAAInfo = TBAAInfo;
  }

public:
  bool isSimple() const { return LVType == Simple; }
  bool isVectorElt() const { return LVType == VectorElt; }
  bool isBitField() const { return LVType == BitField; }
  bool isExtVectorElt() const { return LVType == ExtVectorElt; }
  bool isGlobalReg() const { return LVType == GlobalReg; }

  bool isVolatileQualified() const { return Quals.hasVolatile(); }
  bool isRestrictQualified() const { return Quals.hasRestrict(); }
  unsigned getVRQualifiers() const {
    return Quals.getCVRQualifiers() & ~Qualifiers::Const;
  }

  QualType getType() const { return Type; }

  Qualifiers::ObjCLifetime getObjCLifetime() const {
    return Quals.getObjCLifetime();
  }

  bool isObjCIvar() const { return Ivar; }
  void setObjCIvar(bool Value) { Ivar = Value; }

  bool isObjCArray() const { return ObjIsArray; }
  void setObjCArray(bool Value) { ObjIsArray = Value; }

  bool isNonGC () const { return NonGC; }
  void setNonGC(bool Value) { NonGC = Value; }

  bool isGlobalObjCRef() const { return GlobalObjCRef; }
  void setGlobalObjCRef(bool Value) { GlobalObjCRef = Value; }

  bool isThreadLocalRef() const { return ThreadLocalRef; }
  void setThreadLocalRef(bool Value) { ThreadLocalRef = Value;}

  ARCPreciseLifetime_t isARCPreciseLifetime() const {
    return ARCPreciseLifetime_t(!ImpreciseLifetime);
  }
  void setARCPreciseLifetime(ARCPreciseLifetime_t value) {
    ImpreciseLifetime = (value == ARCImpreciseLifetime);
  }
  bool isNontemporal() const { return Nontemporal; }
  void setNontemporal(bool Value) { Nontemporal = Value; }

  bool isObjCWeak() const {
    return Quals.getObjCGCAttr() == Qualifiers::Weak;
  }
  bool isObjCStrong() const {
    return Quals.getObjCGCAttr() == Qualifiers::Strong;
  }

  bool isVolatile() const {
    return Quals.hasVolatile();
  }
  
  Expr *getBaseIvarExp() const { return BaseIvarExp; }
  void setBaseIvarExp(Expr *V) { BaseIvarExp = V; }

  QualType getTBAABaseType() const { return TBAABaseType; }
  void setTBAABaseType(QualType T) { TBAABaseType = T; }

  uint64_t getTBAAOffset() const { return TBAAOffset; }
  void setTBAAOffset(uint64_t O) { TBAAOffset = O; }

  llvm::MDNode *getTBAAInfo() const { return TBAAInfo; }
  void setTBAAInfo(llvm::MDNode *N) { TBAAInfo = N; }

  const Qualifiers &getQuals() const { return Quals; }
  Qualifiers &getQuals() { return Quals; }

  unsigned getAddressSpace() const { return Quals.getAddressSpace(); }

  CharUnits getAlignment() const { return CharUnits::fromQuantity(Alignment); }
  void setAlignment(CharUnits A) { Alignment = A.getQuantity(); }

  LValueBaseInfo getBaseInfo() const { return BaseInfo; }
  void setBaseInfo(LValueBaseInfo Info) { BaseInfo = Info; }

  // simple lvalue
  llvm::Value *getPointer() const {
    assert(isSimple());
    return V;
  }
  Address getAddress() const { return Address(getPointer(), getAlignment()); }
  void setAddress(Address address) {
    assert(isSimple());
    V = address.getPointer();
    Alignment = address.getAlignment().getQuantity();
  }

  // vector elt lvalue
  Address getVectorAddress() const {
    return Address(getVectorPointer(), getAlignment());
  }
  llvm::Value *getVectorPointer() const { assert(isVectorElt()); return V; }
  llvm::Value *getVectorIdx() const { assert(isVectorElt()); return VectorIdx; }

  // extended vector elements.
  Address getExtVectorAddress() const {
    return Address(getExtVectorPointer(), getAlignment());
  }
  llvm::Value *getExtVectorPointer() const {
    assert(isExtVectorElt());
    return V;
  }
  llvm::Constant *getExtVectorElts() const {
    assert(isExtVectorElt());
    return VectorElts;
  }

  // bitfield lvalue
  Address getBitFieldAddress() const {
    return Address(getBitFieldPointer(), getAlignment());
  }
  llvm::Value *getBitFieldPointer() const { assert(isBitField()); return V; }
  const CGBitFieldInfo &getBitFieldInfo() const {
    assert(isBitField());
    return *BitFieldInfo;
  }

  // global register lvalue
  llvm::Value *getGlobalReg() const { assert(isGlobalReg()); return V; }

  static LValue MakeAddr(Address address, QualType type,
                         ASTContext &Context,
                         LValueBaseInfo BaseInfo,
                         llvm::MDNode *TBAAInfo = nullptr) {
    Qualifiers qs = type.getQualifiers();
    qs.setObjCGCAttr(Context.getObjCGCAttrKind(type));

    LValue R;
    R.LVType = Simple;
    assert(address.getPointer()->getType()->isPointerTy());
    R.V = address.getPointer();
    R.Initialize(type, qs, address.getAlignment(), BaseInfo, TBAAInfo);
    return R;
  }

  static LValue MakeVectorElt(Address vecAddress, llvm::Value *Idx,
                              QualType type, LValueBaseInfo BaseInfo) {
    LValue R;
    R.LVType = VectorElt;
    R.V = vecAddress.getPointer();
    R.VectorIdx = Idx;
    R.Initialize(type, type.getQualifiers(), vecAddress.getAlignment(),
                 BaseInfo);
    return R;
  }

  static LValue MakeExtVectorElt(Address vecAddress, llvm::Constant *Elts,
                                 QualType type, LValueBaseInfo BaseInfo) {
    LValue R;
    R.LVType = ExtVectorElt;
    R.V = vecAddress.getPointer();
    R.VectorElts = Elts;
    R.Initialize(type, type.getQualifiers(), vecAddress.getAlignment(),
                 BaseInfo);
    return R;
  }

  /// \brief Create a new object to represent a bit-field access.
  ///
  /// \param Addr - The base address of the bit-field sequence this
  /// bit-field refers to.
  /// \param Info - The information describing how to perform the bit-field
  /// access.
  static LValue MakeBitfield(Address Addr,
                             const CGBitFieldInfo &Info,
                             QualType type,
                             LValueBaseInfo BaseInfo) {
    LValue R;
    R.LVType = BitField;
    R.V = Addr.getPointer();
    R.BitFieldInfo = &Info;
    R.Initialize(type, type.getQualifiers(), Addr.getAlignment(), BaseInfo);
    return R;
  }

  static LValue MakeGlobalReg(Address Reg, QualType type) {
    LValue R;
    R.LVType = GlobalReg;
    R.V = Reg.getPointer();
    R.Initialize(type, type.getQualifiers(), Reg.getAlignment(),
                 LValueBaseInfo(AlignmentSource::Decl, false));
    return R;
  }

  RValue asAggregateRValue() const {
    return RValue::getAggregate(getAddress(), isVolatileQualified());
  }
};

/// An aggregate value slot.
class AggValueSlot {
  /// The address.
  llvm::Value *Addr;

  // Qualifiers
  Qualifiers Quals;

  unsigned Alignment;

  /// DestructedFlag - This is set to true if some external code is
  /// responsible for setting up a destructor for the slot.  Otherwise
  /// the code which constructs it should push the appropriate cleanup.
  bool DestructedFlag : 1;

  /// ObjCGCFlag - This is set to true if writing to the memory in the
  /// slot might require calling an appropriate Objective-C GC
  /// barrier.  The exact interaction here is unnecessarily mysterious.
  bool ObjCGCFlag : 1;
  
  /// ZeroedFlag - This is set to true if the memory in the slot is
  /// known to be zero before the assignment into it.  This means that
  /// zero fields don't need to be set.
  bool ZeroedFlag : 1;

  /// AliasedFlag - This is set to true if the slot might be aliased
  /// and it's not undefined behavior to access it through such an
  /// alias.  Note that it's always undefined behavior to access a C++
  /// object that's under construction through an alias derived from
  /// outside the construction process.
  ///
  /// This flag controls whether calls that produce the aggregate
  /// value may be evaluated directly into the slot, or whether they
  /// must be evaluated into an unaliased temporary and then memcpy'ed
  /// over.  Since it's invalid in general to memcpy a non-POD C++
  /// object, it's important that this flag never be set when
  /// evaluating an expression which constructs such an object.
  bool AliasedFlag : 1;

public:
  enum IsAliased_t { IsNotAliased, IsAliased };
  enum IsDestructed_t { IsNotDestructed, IsDestructed };
  enum IsZeroed_t { IsNotZeroed, IsZeroed };
  enum NeedsGCBarriers_t { DoesNotNeedGCBarriers, NeedsGCBarriers };

  /// ignored - Returns an aggregate value slot indicating that the
  /// aggregate value is being ignored.
  static AggValueSlot ignored() {
    return forAddr(Address::invalid(), Qualifiers(), IsNotDestructed,
                   DoesNotNeedGCBarriers, IsNotAliased);
  }

  /// forAddr - Make a slot for an aggregate value.
  ///
  /// \param quals - The qualifiers that dictate how the slot should
  /// be initialied. Only 'volatile' and the Objective-C lifetime
  /// qualifiers matter.
  ///
  /// \param isDestructed - true if something else is responsible
  ///   for calling destructors on this object
  /// \param needsGC - true if the slot is potentially located
  ///   somewhere that ObjC GC calls should be emitted for
  static AggValueSlot forAddr(Address addr,
                              Qualifiers quals,
                              IsDestructed_t isDestructed,
                              NeedsGCBarriers_t needsGC,
                              IsAliased_t isAliased,
                              IsZeroed_t isZeroed = IsNotZeroed) {
    AggValueSlot AV;
    if (addr.isValid()) {
      AV.Addr = addr.getPointer();
      AV.Alignment = addr.getAlignment().getQuantity();
    } else {
      AV.Addr = nullptr;
      AV.Alignment = 0;
    }
    AV.Quals = quals;
    AV.DestructedFlag = isDestructed;
    AV.ObjCGCFlag = needsGC;
    AV.ZeroedFlag = isZeroed;
    AV.AliasedFlag = isAliased;
    return AV;
  }

  static AggValueSlot forLValue(const LValue &LV,
                                IsDestructed_t isDestructed,
                                NeedsGCBarriers_t needsGC,
                                IsAliased_t isAliased,
                                IsZeroed_t isZeroed = IsNotZeroed) {
    return forAddr(LV.getAddress(),
                   LV.getQuals(), isDestructed, needsGC, isAliased, isZeroed);
  }

  IsDestructed_t isExternallyDestructed() const {
    return IsDestructed_t(DestructedFlag);
  }
  void setExternallyDestructed(bool destructed = true) {
    DestructedFlag = destructed;
  }

  Qualifiers getQualifiers() const { return Quals; }

  bool isVolatile() const {
    return Quals.hasVolatile();
  }

  void setVolatile(bool flag) {
    Quals.setVolatile(flag);
  }
  
  Qualifiers::ObjCLifetime getObjCLifetime() const {
    return Quals.getObjCLifetime();
  }

  NeedsGCBarriers_t requiresGCollection() const {
    return NeedsGCBarriers_t(ObjCGCFlag);
  }

  llvm::Value *getPointer() const {
    return Addr;
  }

  Address getAddress() const {
    return Address(Addr, getAlignment());
  }

  bool isIgnored() const {
    return Addr == nullptr;
  }

  CharUnits getAlignment() const {
    return CharUnits::fromQuantity(Alignment);
  }

  IsAliased_t isPotentiallyAliased() const {
    return IsAliased_t(AliasedFlag);
  }

  RValue asRValue() const {
    if (isIgnored()) {
      return RValue::getIgnored();
    } else {
      return RValue::getAggregate(getAddress(), isVolatile());
    }
  }

  void setZeroed(bool V = true) { ZeroedFlag = V; }
  IsZeroed_t isZeroed() const {
    return IsZeroed_t(ZeroedFlag);
  }
};

}  // end namespace CodeGen
}  // end namespace clang

#endif
