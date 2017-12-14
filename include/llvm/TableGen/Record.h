//===- llvm/TableGen/Record.h - Classes for Table Records -------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file defines the main TableGen data structures, including the TableGen
// types, values, and high-level data structures.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TABLEGEN_RECORD_H
#define LLVM_TABLEGEN_RECORD_H

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/FoldingSet.h"
#include "llvm/ADT/PointerIntPair.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/SMLoc.h"
#include "llvm/Support/TrailingObjects.h"
#include "llvm/Support/raw_ostream.h"
#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace llvm {

class ListRecTy;
struct MultiClass;
class Record;
class RecordKeeper;
class RecordVal;
class StringInit;

//===----------------------------------------------------------------------===//
//  Type Classes
//===----------------------------------------------------------------------===//

class RecTy {
public:
  /// \brief Subclass discriminator (for dyn_cast<> et al.)
  enum RecTyKind {
    BitRecTyKind,
    BitsRecTyKind,
    CodeRecTyKind,
    IntRecTyKind,
    StringRecTyKind,
    ListRecTyKind,
    DagRecTyKind,
    RecordRecTyKind
  };

private:
  RecTyKind Kind;
  ListRecTy *ListTy = nullptr;

public:
  RecTy(RecTyKind K) : Kind(K) {}
  virtual ~RecTy() = default;

  RecTyKind getRecTyKind() const { return Kind; }

  virtual std::string getAsString() const = 0;
  void print(raw_ostream &OS) const { OS << getAsString(); }
  void dump() const;

  /// Return true if all values of 'this' type can be converted to the specified
  /// type.
  virtual bool typeIsConvertibleTo(const RecTy *RHS) const;

  /// Returns the type representing list<this>.
  ListRecTy *getListTy();
};

inline raw_ostream &operator<<(raw_ostream &OS, const RecTy &Ty) {
  Ty.print(OS);
  return OS;
}

/// 'bit' - Represent a single bit
class BitRecTy : public RecTy {
  static BitRecTy Shared;

  BitRecTy() : RecTy(BitRecTyKind) {}

public:
  static bool classof(const RecTy *RT) {
    return RT->getRecTyKind() == BitRecTyKind;
  }

  static BitRecTy *get() { return &Shared; }

  std::string getAsString() const override { return "bit"; }

  bool typeIsConvertibleTo(const RecTy *RHS) const override;
};

/// 'bits<n>' - Represent a fixed number of bits
class BitsRecTy : public RecTy {
  unsigned Size;

  explicit BitsRecTy(unsigned Sz) : RecTy(BitsRecTyKind), Size(Sz) {}

public:
  static bool classof(const RecTy *RT) {
    return RT->getRecTyKind() == BitsRecTyKind;
  }

  static BitsRecTy *get(unsigned Sz);

  unsigned getNumBits() const { return Size; }

  std::string getAsString() const override;

  bool typeIsConvertibleTo(const RecTy *RHS) const override;
};

/// 'code' - Represent a code fragment
class CodeRecTy : public RecTy {
  static CodeRecTy Shared;

  CodeRecTy() : RecTy(CodeRecTyKind) {}

public:
  static bool classof(const RecTy *RT) {
    return RT->getRecTyKind() == CodeRecTyKind;
  }

  static CodeRecTy *get() { return &Shared; }

  std::string getAsString() const override { return "code"; }
};

/// 'int' - Represent an integer value of no particular size
class IntRecTy : public RecTy {
  static IntRecTy Shared;

  IntRecTy() : RecTy(IntRecTyKind) {}

public:
  static bool classof(const RecTy *RT) {
    return RT->getRecTyKind() == IntRecTyKind;
  }

  static IntRecTy *get() { return &Shared; }

  std::string getAsString() const override { return "int"; }

  bool typeIsConvertibleTo(const RecTy *RHS) const override;
};

/// 'string' - Represent an string value
class StringRecTy : public RecTy {
  static StringRecTy Shared;

  StringRecTy() : RecTy(StringRecTyKind) {}

public:
  static bool classof(const RecTy *RT) {
    return RT->getRecTyKind() == StringRecTyKind ||
           RT->getRecTyKind() == CodeRecTyKind;
  }

  static StringRecTy *get() { return &Shared; }

  std::string getAsString() const override;
};

/// 'list<Ty>' - Represent a list of values, all of which must be of
/// the specified type.
class ListRecTy : public RecTy {
  friend ListRecTy *RecTy::getListTy();

  RecTy *Ty;

  explicit ListRecTy(RecTy *T) : RecTy(ListRecTyKind), Ty(T) {}

public:
  static bool classof(const RecTy *RT) {
    return RT->getRecTyKind() == ListRecTyKind;
  }

  static ListRecTy *get(RecTy *T) { return T->getListTy(); }
  RecTy *getElementType() const { return Ty; }

  std::string getAsString() const override;

  bool typeIsConvertibleTo(const RecTy *RHS) const override;
};

/// 'dag' - Represent a dag fragment
class DagRecTy : public RecTy {
  static DagRecTy Shared;

  DagRecTy() : RecTy(DagRecTyKind) {}

public:
  static bool classof(const RecTy *RT) {
    return RT->getRecTyKind() == DagRecTyKind;
  }

  static DagRecTy *get() { return &Shared; }

  std::string getAsString() const override;
};

/// '[classname]' - Represent an instance of a class, such as:
/// (R32 X = EAX).
class RecordRecTy : public RecTy {
  friend class Record;

  Record *Rec;

  explicit RecordRecTy(Record *R) : RecTy(RecordRecTyKind), Rec(R) {}

public:
  static bool classof(const RecTy *RT) {
    return RT->getRecTyKind() == RecordRecTyKind;
  }

  static RecordRecTy *get(Record *R);

  Record *getRecord() const { return Rec; }

  std::string getAsString() const override;

  bool typeIsConvertibleTo(const RecTy *RHS) const override;
};

/// Find a common type that T1 and T2 convert to.
/// Return 0 if no such type exists.
RecTy *resolveTypes(RecTy *T1, RecTy *T2);

//===----------------------------------------------------------------------===//
//  Initializer Classes
//===----------------------------------------------------------------------===//

class Init {
protected:
  /// \brief Discriminator enum (for isa<>, dyn_cast<>, et al.)
  ///
  /// This enum is laid out by a preorder traversal of the inheritance
  /// hierarchy, and does not contain an entry for abstract classes, as per
  /// the recommendation in docs/HowToSetUpLLVMStyleRTTI.rst.
  ///
  /// We also explicitly include "first" and "last" values for each
  /// interior node of the inheritance tree, to make it easier to read the
  /// corresponding classof().
  ///
  /// We could pack these a bit tighter by not having the IK_FirstXXXInit
  /// and IK_LastXXXInit be their own values, but that would degrade
  /// readability for really no benefit.
  enum InitKind : uint8_t {
    IK_BitInit,
    IK_FirstTypedInit,
    IK_BitsInit,
    IK_CodeInit,
    IK_DagInit,
    IK_DefInit,
    IK_FieldInit,
    IK_IntInit,
    IK_ListInit,
    IK_FirstOpInit,
    IK_BinOpInit,
    IK_TernOpInit,
    IK_UnOpInit,
    IK_LastOpInit,
    IK_StringInit,
    IK_VarInit,
    IK_VarListElementInit,
    IK_LastTypedInit,
    IK_UnsetInit,
    IK_VarBitInit
  };

private:
  const InitKind Kind;

protected:
  uint8_t Opc; // Used by UnOpInit, BinOpInit, and TernOpInit

private:
  virtual void anchor();

public:
  InitKind getKind() const { return Kind; }

protected:
  explicit Init(InitKind K, uint8_t Opc = 0) : Kind(K), Opc(Opc) {}

public:
  Init(const Init &) = delete;
  Init &operator=(const Init &) = delete;
  virtual ~Init() = default;

  /// This virtual method should be overridden by values that may
  /// not be completely specified yet.
  virtual bool isComplete() const { return true; }

  /// Print out this value.
  void print(raw_ostream &OS) const { OS << getAsString(); }

  /// Convert this value to a string form.
  virtual std::string getAsString() const = 0;
  /// Convert this value to a string form,
  /// without adding quote markers.  This primaruly affects
  /// StringInits where we will not surround the string value with
  /// quotes.
  virtual std::string getAsUnquotedString() const { return getAsString(); }

  /// Debugging method that may be called through a debugger, just
  /// invokes print on stderr.
  void dump() const;

  /// This virtual function converts to the appropriate
  /// Init based on the passed in type.
  virtual Init *convertInitializerTo(RecTy *Ty) const = 0;

  /// This method is used to implement the bitrange
  /// selection operator.  Given an initializer, it selects the specified bits
  /// out, returning them as a new init of bits type.  If it is not legal to use
  /// the bit subscript operator on this initializer, return null.
  virtual Init *convertInitializerBitRange(ArrayRef<unsigned> Bits) const {
    return nullptr;
  }

  /// This method is used to implement the list slice
  /// selection operator.  Given an initializer, it selects the specified list
  /// elements, returning them as a new init of list type.  If it is not legal
  /// to take a slice of this, return null.
  virtual Init *convertInitListSlice(ArrayRef<unsigned> Elements) const {
    return nullptr;
  }

  /// This method is used to implement the FieldInit class.
  /// Implementors of this method should return the type of the named field if
  /// they are of record type.
  virtual RecTy *getFieldType(StringInit *FieldName) const {
    return nullptr;
  }

  /// This method complements getFieldType to return the
  /// initializer for the specified field.  If getFieldType returns non-null
  /// this method should return non-null, otherwise it returns null.
  virtual Init *getFieldInit(Record &R, const RecordVal *RV,
                             StringInit *FieldName) const {
    return nullptr;
  }

  /// This method is used by classes that refer to other
  /// variables which may not be defined at the time the expression is formed.
  /// If a value is set for the variable later, this method will be called on
  /// users of the value to allow the value to propagate out.
  virtual Init *resolveReferences(Record &R, const RecordVal *RV) const {
    return const_cast<Init *>(this);
  }

  /// This method is used to return the initializer for the specified
  /// bit.
  virtual Init *getBit(unsigned Bit) const = 0;

  /// This method is used to retrieve the initializer for bit
  /// reference. For non-VarBitInit, it simply returns itself.
  virtual Init *getBitVar() const { return const_cast<Init*>(this); }

  /// This method is used to retrieve the bit number of a bit
  /// reference. For non-VarBitInit, it simply returns 0.
  virtual unsigned getBitNum() const { return 0; }
};

inline raw_ostream &operator<<(raw_ostream &OS, const Init &I) {
  I.print(OS); return OS;
}

/// This is the common super-class of types that have a specific,
/// explicit, type.
class TypedInit : public Init {
  RecTy *Ty;

protected:
  explicit TypedInit(InitKind K, RecTy *T, uint8_t Opc = 0)
    : Init(K, Opc), Ty(T) {}

public:
  TypedInit(const TypedInit &) = delete;
  TypedInit &operator=(const TypedInit &) = delete;

  static bool classof(const Init *I) {
    return I->getKind() >= IK_FirstTypedInit &&
           I->getKind() <= IK_LastTypedInit;
  }

  RecTy *getType() const { return Ty; }

  Init *convertInitializerTo(RecTy *Ty) const override;

  Init *convertInitializerBitRange(ArrayRef<unsigned> Bits) const override;
  Init *convertInitListSlice(ArrayRef<unsigned> Elements) const override;

  /// This method is used to implement the FieldInit class.
  /// Implementors of this method should return the type of the named field if
  /// they are of record type.
  ///
  RecTy *getFieldType(StringInit *FieldName) const override;

  /// This method is used to implement
  /// VarListElementInit::resolveReferences.  If the list element is resolvable
  /// now, we return the resolved value, otherwise we return null.
  virtual Init *resolveListElementReference(Record &R, const RecordVal *RV,
                                            unsigned Elt) const = 0;
};

/// '?' - Represents an uninitialized value
class UnsetInit : public Init {
  UnsetInit() : Init(IK_UnsetInit) {}

public:
  UnsetInit(const UnsetInit &) = delete;
  UnsetInit &operator=(const UnsetInit &) = delete;

  static bool classof(const Init *I) {
    return I->getKind() == IK_UnsetInit;
  }

  static UnsetInit *get();

  Init *convertInitializerTo(RecTy *Ty) const override;

  Init *getBit(unsigned Bit) const override {
    return const_cast<UnsetInit*>(this);
  }

  bool isComplete() const override { return false; }
  std::string getAsString() const override { return "?"; }
};

/// 'true'/'false' - Represent a concrete initializer for a bit.
class BitInit : public Init {
  bool Value;

  explicit BitInit(bool V) : Init(IK_BitInit), Value(V) {}

public:
  BitInit(const BitInit &) = delete;
  BitInit &operator=(BitInit &) = delete;

  static bool classof(const Init *I) {
    return I->getKind() == IK_BitInit;
  }

  static BitInit *get(bool V);

  bool getValue() const { return Value; }

  Init *convertInitializerTo(RecTy *Ty) const override;

  Init *getBit(unsigned Bit) const override {
    assert(Bit < 1 && "Bit index out of range!");
    return const_cast<BitInit*>(this);
  }

  std::string getAsString() const override { return Value ? "1" : "0"; }
};

/// '{ a, b, c }' - Represents an initializer for a BitsRecTy value.
/// It contains a vector of bits, whose size is determined by the type.
class BitsInit final : public TypedInit, public FoldingSetNode,
                       public TrailingObjects<BitsInit, Init *> {
  unsigned NumBits;

  BitsInit(unsigned N)
    : TypedInit(IK_BitsInit, BitsRecTy::get(N)), NumBits(N) {}

public:
  BitsInit(const BitsInit &) = delete;
  BitsInit &operator=(const BitsInit &) = delete;

  // Do not use sized deallocation due to trailing objects.
  void operator delete(void *p) { ::operator delete(p); }

  static bool classof(const Init *I) {
    return I->getKind() == IK_BitsInit;
  }

  static BitsInit *get(ArrayRef<Init *> Range);

  void Profile(FoldingSetNodeID &ID) const;

  unsigned getNumBits() const { return NumBits; }

  Init *convertInitializerTo(RecTy *Ty) const override;
  Init *convertInitializerBitRange(ArrayRef<unsigned> Bits) const override;

  bool isComplete() const override {
    for (unsigned i = 0; i != getNumBits(); ++i)
      if (!getBit(i)->isComplete()) return false;
    return true;
  }

  bool allInComplete() const {
    for (unsigned i = 0; i != getNumBits(); ++i)
      if (getBit(i)->isComplete()) return false;
    return true;
  }

  std::string getAsString() const override;

  /// This method is used to implement
  /// VarListElementInit::resolveReferences.  If the list element is resolvable
  /// now, we return the resolved value, otherwise we return null.
  Init *resolveListElementReference(Record &R, const RecordVal *RV,
                                    unsigned Elt) const override {
    llvm_unreachable("Illegal element reference off bits<n>");
  }

  Init *resolveReferences(Record &R, const RecordVal *RV) const override;

  Init *getBit(unsigned Bit) const override {
    assert(Bit < NumBits && "Bit index out of range!");
    return getTrailingObjects<Init *>()[Bit];
  }
};

/// '7' - Represent an initialization by a literal integer value.
class IntInit : public TypedInit {
  int64_t Value;

  explicit IntInit(int64_t V)
    : TypedInit(IK_IntInit, IntRecTy::get()), Value(V) {}

public:
  IntInit(const IntInit &) = delete;
  IntInit &operator=(const IntInit &) = delete;

  static bool classof(const Init *I) {
    return I->getKind() == IK_IntInit;
  }

  static IntInit *get(int64_t V);

  int64_t getValue() const { return Value; }

  Init *convertInitializerTo(RecTy *Ty) const override;
  Init *convertInitializerBitRange(ArrayRef<unsigned> Bits) const override;

  std::string getAsString() const override;

  /// This method is used to implement
  /// VarListElementInit::resolveReferences.  If the list element is resolvable
  /// now, we return the resolved value, otherwise we return null.
  Init *resolveListElementReference(Record &R, const RecordVal *RV,
                                    unsigned Elt) const override {
    llvm_unreachable("Illegal element reference off int");
  }

  Init *getBit(unsigned Bit) const override {
    return BitInit::get((Value & (1ULL << Bit)) != 0);
  }
};

/// "foo" - Represent an initialization by a string value.
class StringInit : public TypedInit {
  StringRef Value;

  explicit StringInit(StringRef V)
      : TypedInit(IK_StringInit, StringRecTy::get()), Value(V) {}

public:
  StringInit(const StringInit &) = delete;
  StringInit &operator=(const StringInit &) = delete;

  static bool classof(const Init *I) {
    return I->getKind() == IK_StringInit;
  }

  static StringInit *get(StringRef);

  StringRef getValue() const { return Value; }

  Init *convertInitializerTo(RecTy *Ty) const override;

  std::string getAsString() const override { return "\"" + Value.str() + "\""; }

  std::string getAsUnquotedString() const override { return Value; }

  /// resolveListElementReference - This method is used to implement
  /// VarListElementInit::resolveReferences.  If the list element is resolvable
  /// now, we return the resolved value, otherwise we return null.
  Init *resolveListElementReference(Record &R, const RecordVal *RV,
                                    unsigned Elt) const override {
    llvm_unreachable("Illegal element reference off string");
  }

  Init *getBit(unsigned Bit) const override {
    llvm_unreachable("Illegal bit reference off string");
  }
};

class CodeInit : public TypedInit {
  StringRef Value;

  explicit CodeInit(StringRef V)
      : TypedInit(IK_CodeInit, static_cast<RecTy *>(CodeRecTy::get())),
        Value(V) {}

public:
  CodeInit(const StringInit &) = delete;
  CodeInit &operator=(const StringInit &) = delete;

  static bool classof(const Init *I) {
    return I->getKind() == IK_CodeInit;
  }

  static CodeInit *get(StringRef);

  StringRef getValue() const { return Value; }

  Init *convertInitializerTo(RecTy *Ty) const override;

  std::string getAsString() const override {
    return "[{" + Value.str() + "}]";
  }

  std::string getAsUnquotedString() const override { return Value; }

  /// This method is used to implement
  /// VarListElementInit::resolveReferences.  If the list element is resolvable
  /// now, we return the resolved value, otherwise we return null.
  Init *resolveListElementReference(Record &R, const RecordVal *RV,
                                    unsigned Elt) const override {
    llvm_unreachable("Illegal element reference off string");
  }

  Init *getBit(unsigned Bit) const override {
    llvm_unreachable("Illegal bit reference off string");
  }
};

/// [AL, AH, CL] - Represent a list of defs
///
class ListInit final : public TypedInit, public FoldingSetNode,
                       public TrailingObjects<ListInit, Init *> {
  unsigned NumValues;

public:
  using const_iterator = Init *const *;

private:
  explicit ListInit(unsigned N, RecTy *EltTy)
    : TypedInit(IK_ListInit, ListRecTy::get(EltTy)), NumValues(N) {}

public:
  ListInit(const ListInit &) = delete;
  ListInit &operator=(const ListInit &) = delete;

  // Do not use sized deallocation due to trailing objects.
  void operator delete(void *p) { ::operator delete(p); }

  static bool classof(const Init *I) {
    return I->getKind() == IK_ListInit;
  }
  static ListInit *get(ArrayRef<Init *> Range, RecTy *EltTy);

  void Profile(FoldingSetNodeID &ID) const;

  Init *getElement(unsigned i) const {
    assert(i < NumValues && "List element index out of range!");
    return getTrailingObjects<Init *>()[i];
  }

  Record *getElementAsRecord(unsigned i) const;

  Init *convertInitListSlice(ArrayRef<unsigned> Elements) const override;

  Init *convertInitializerTo(RecTy *Ty) const override;

  /// This method is used by classes that refer to other
  /// variables which may not be defined at the time they expression is formed.
  /// If a value is set for the variable later, this method will be called on
  /// users of the value to allow the value to propagate out.
  ///
  Init *resolveReferences(Record &R, const RecordVal *RV) const override;

  std::string getAsString() const override;

  ArrayRef<Init*> getValues() const {
    return makeArrayRef(getTrailingObjects<Init *>(), NumValues);
  }

  const_iterator begin() const { return getTrailingObjects<Init *>(); }
  const_iterator end  () const { return begin() + NumValues; }

  size_t         size () const { return NumValues;  }
  bool           empty() const { return NumValues == 0; }

  /// This method is used to implement
  /// VarListElementInit::resolveReferences.  If the list element is resolvable
  /// now, we return the resolved value, otherwise we return null.
  Init *resolveListElementReference(Record &R, const RecordVal *RV,
                                    unsigned Elt) const override;

  Init *getBit(unsigned Bit) const override {
    llvm_unreachable("Illegal bit reference off list");
  }
};

/// Base class for operators
///
class OpInit : public TypedInit {
protected:
  explicit OpInit(InitKind K, RecTy *Type, uint8_t Opc)
    : TypedInit(K, Type, Opc) {}

public:
  OpInit(const OpInit &) = delete;
  OpInit &operator=(OpInit &) = delete;

  static bool classof(const Init *I) {
    return I->getKind() >= IK_FirstOpInit &&
           I->getKind() <= IK_LastOpInit;
  }

  // Clone - Clone this operator, replacing arguments with the new list
  virtual OpInit *clone(ArrayRef<Init *> Operands) const = 0;

  virtual unsigned getNumOperands() const = 0;
  virtual Init *getOperand(unsigned i) const = 0;

  // Fold - If possible, fold this to a simpler init.  Return this if not
  // possible to fold.
  virtual Init *Fold(Record *CurRec, MultiClass *CurMultiClass) const = 0;

  Init *resolveListElementReference(Record &R, const RecordVal *RV,
                                    unsigned Elt) const override;

  Init *getBit(unsigned Bit) const override;
};

/// !op (X) - Transform an init.
///
class UnOpInit : public OpInit, public FoldingSetNode {
public:
  enum UnaryOp : uint8_t { CAST, HEAD, TAIL, EMPTY };

private:
  Init *LHS;

  UnOpInit(UnaryOp opc, Init *lhs, RecTy *Type)
    : OpInit(IK_UnOpInit, Type, opc), LHS(lhs) {}

public:
  UnOpInit(const UnOpInit &) = delete;
  UnOpInit &operator=(const UnOpInit &) = delete;

  static bool classof(const Init *I) {
    return I->getKind() == IK_UnOpInit;
  }

  static UnOpInit *get(UnaryOp opc, Init *lhs, RecTy *Type);

  void Profile(FoldingSetNodeID &ID) const;

  // Clone - Clone this operator, replacing arguments with the new list
  OpInit *clone(ArrayRef<Init *> Operands) const override {
    assert(Operands.size() == 1 &&
           "Wrong number of operands for unary operation");
    return UnOpInit::get(getOpcode(), *Operands.begin(), getType());
  }

  unsigned getNumOperands() const override { return 1; }

  Init *getOperand(unsigned i) const override {
    assert(i == 0 && "Invalid operand id for unary operator");
    return getOperand();
  }

  UnaryOp getOpcode() const { return (UnaryOp)Opc; }
  Init *getOperand() const { return LHS; }

  // Fold - If possible, fold this to a simpler init.  Return this if not
  // possible to fold.
  Init *Fold(Record *CurRec, MultiClass *CurMultiClass) const override;

  Init *resolveReferences(Record &R, const RecordVal *RV) const override;

  std::string getAsString() const override;
};

/// !op (X, Y) - Combine two inits.
class BinOpInit : public OpInit, public FoldingSetNode {
public:
  enum BinaryOp : uint8_t { ADD, AND, OR, SHL, SRA, SRL, LISTCONCAT,
                            STRCONCAT, CONCAT, EQ };

private:
  Init *LHS, *RHS;

  BinOpInit(BinaryOp opc, Init *lhs, Init *rhs, RecTy *Type) :
      OpInit(IK_BinOpInit, Type, opc), LHS(lhs), RHS(rhs) {}

public:
  BinOpInit(const BinOpInit &) = delete;
  BinOpInit &operator=(const BinOpInit &) = delete;

  static bool classof(const Init *I) {
    return I->getKind() == IK_BinOpInit;
  }

  static BinOpInit *get(BinaryOp opc, Init *lhs, Init *rhs,
                        RecTy *Type);

  void Profile(FoldingSetNodeID &ID) const;

  // Clone - Clone this operator, replacing arguments with the new list
  OpInit *clone(ArrayRef<Init *> Operands) const override {
    assert(Operands.size() == 2 &&
           "Wrong number of operands for binary operation");
    return BinOpInit::get(getOpcode(), Operands[0], Operands[1], getType());
  }

  unsigned getNumOperands() const override { return 2; }
  Init *getOperand(unsigned i) const override {
    switch (i) {
    default: llvm_unreachable("Invalid operand id for binary operator");
    case 0: return getLHS();
    case 1: return getRHS();
    }
  }

  BinaryOp getOpcode() const { return (BinaryOp)Opc; }
  Init *getLHS() const { return LHS; }
  Init *getRHS() const { return RHS; }

  // Fold - If possible, fold this to a simpler init.  Return this if not
  // possible to fold.
  Init *Fold(Record *CurRec, MultiClass *CurMultiClass) const override;

  Init *resolveReferences(Record &R, const RecordVal *RV) const override;

  std::string getAsString() const override;
};

/// !op (X, Y, Z) - Combine two inits.
class TernOpInit : public OpInit, public FoldingSetNode {
public:
  enum TernaryOp : uint8_t { SUBST, FOREACH, IF };

private:
  Init *LHS, *MHS, *RHS;

  TernOpInit(TernaryOp opc, Init *lhs, Init *mhs, Init *rhs,
             RecTy *Type) :
      OpInit(IK_TernOpInit, Type, opc), LHS(lhs), MHS(mhs), RHS(rhs) {}

public:
  TernOpInit(const TernOpInit &) = delete;
  TernOpInit &operator=(const TernOpInit &) = delete;

  static bool classof(const Init *I) {
    return I->getKind() == IK_TernOpInit;
  }

  static TernOpInit *get(TernaryOp opc, Init *lhs,
                         Init *mhs, Init *rhs,
                         RecTy *Type);

  void Profile(FoldingSetNodeID &ID) const;

  // Clone - Clone this operator, replacing arguments with the new list
  OpInit *clone(ArrayRef<Init *> Operands) const override {
    assert(Operands.size() == 3 &&
           "Wrong number of operands for ternary operation");
    return TernOpInit::get(getOpcode(), Operands[0], Operands[1], Operands[2],
                           getType());
  }

  unsigned getNumOperands() const override { return 3; }
  Init *getOperand(unsigned i) const override {
    switch (i) {
    default: llvm_unreachable("Invalid operand id for ternary operator");
    case 0: return getLHS();
    case 1: return getMHS();
    case 2: return getRHS();
    }
  }

  TernaryOp getOpcode() const { return (TernaryOp)Opc; }
  Init *getLHS() const { return LHS; }
  Init *getMHS() const { return MHS; }
  Init *getRHS() const { return RHS; }

  // Fold - If possible, fold this to a simpler init.  Return this if not
  // possible to fold.
  Init *Fold(Record *CurRec, MultiClass *CurMultiClass) const override;

  bool isComplete() const override { return false; }

  Init *resolveReferences(Record &R, const RecordVal *RV) const override;

  std::string getAsString() const override;
};

/// 'Opcode' - Represent a reference to an entire variable object.
class VarInit : public TypedInit {
  Init *VarName;

  explicit VarInit(Init *VN, RecTy *T)
      : TypedInit(IK_VarInit, T), VarName(VN) {}

public:
  VarInit(const VarInit &) = delete;
  VarInit &operator=(const VarInit &) = delete;

  static bool classof(const Init *I) {
    return I->getKind() == IK_VarInit;
  }

  static VarInit *get(StringRef VN, RecTy *T);
  static VarInit *get(Init *VN, RecTy *T);

  StringRef getName() const;
  Init *getNameInit() const { return VarName; }

  std::string getNameInitAsString() const {
    return getNameInit()->getAsUnquotedString();
  }

  Init *resolveListElementReference(Record &R, const RecordVal *RV,
                                    unsigned Elt) const override;

  RecTy *getFieldType(StringInit *FieldName) const override;
  Init *getFieldInit(Record &R, const RecordVal *RV,
                     StringInit *FieldName) const override;

  /// This method is used by classes that refer to other
  /// variables which may not be defined at the time they expression is formed.
  /// If a value is set for the variable later, this method will be called on
  /// users of the value to allow the value to propagate out.
  ///
  Init *resolveReferences(Record &R, const RecordVal *RV) const override;

  Init *getBit(unsigned Bit) const override;

  std::string getAsString() const override { return getName(); }
};

/// Opcode{0} - Represent access to one bit of a variable or field.
class VarBitInit : public Init {
  TypedInit *TI;
  unsigned Bit;

  VarBitInit(TypedInit *T, unsigned B) : Init(IK_VarBitInit), TI(T), Bit(B) {
    assert(T->getType() &&
           (isa<IntRecTy>(T->getType()) ||
            (isa<BitsRecTy>(T->getType()) &&
             cast<BitsRecTy>(T->getType())->getNumBits() > B)) &&
           "Illegal VarBitInit expression!");
  }

public:
  VarBitInit(const VarBitInit &) = delete;
  VarBitInit &operator=(const VarBitInit &) = delete;

  static bool classof(const Init *I) {
    return I->getKind() == IK_VarBitInit;
  }

  static VarBitInit *get(TypedInit *T, unsigned B);

  Init *convertInitializerTo(RecTy *Ty) const override;

  Init *getBitVar() const override { return TI; }
  unsigned getBitNum() const override { return Bit; }

  std::string getAsString() const override;
  Init *resolveReferences(Record &R, const RecordVal *RV) const override;

  Init *getBit(unsigned B) const override {
    assert(B < 1 && "Bit index out of range!");
    return const_cast<VarBitInit*>(this);
  }
};

/// List[4] - Represent access to one element of a var or
/// field.
class VarListElementInit : public TypedInit {
  TypedInit *TI;
  unsigned Element;

  VarListElementInit(TypedInit *T, unsigned E)
      : TypedInit(IK_VarListElementInit,
                  cast<ListRecTy>(T->getType())->getElementType()),
        TI(T), Element(E) {
    assert(T->getType() && isa<ListRecTy>(T->getType()) &&
           "Illegal VarBitInit expression!");
  }

public:
  VarListElementInit(const VarListElementInit &) = delete;
  VarListElementInit &operator=(const VarListElementInit &) = delete;

  static bool classof(const Init *I) {
    return I->getKind() == IK_VarListElementInit;
  }

  static VarListElementInit *get(TypedInit *T, unsigned E);

  TypedInit *getVariable() const { return TI; }
  unsigned getElementNum() const { return Element; }

  /// This method is used to implement
  /// VarListElementInit::resolveReferences.  If the list element is resolvable
  /// now, we return the resolved value, otherwise we return null.
  Init *resolveListElementReference(Record &R, const RecordVal *RV,
                                    unsigned Elt) const override;

  std::string getAsString() const override;
  Init *resolveReferences(Record &R, const RecordVal *RV) const override;

  Init *getBit(unsigned Bit) const override;
};

/// AL - Represent a reference to a 'def' in the description
class DefInit : public TypedInit {
  friend class Record;

  Record *Def;

  DefInit(Record *D, RecordRecTy *T) : TypedInit(IK_DefInit, T), Def(D) {}

public:
  DefInit(const DefInit &) = delete;
  DefInit &operator=(const DefInit &) = delete;

  static bool classof(const Init *I) {
    return I->getKind() == IK_DefInit;
  }

  static DefInit *get(Record*);

  Init *convertInitializerTo(RecTy *Ty) const override;

  Record *getDef() const { return Def; }

  //virtual Init *convertInitializerBitRange(ArrayRef<unsigned> Bits);

  RecTy *getFieldType(StringInit *FieldName) const override;
  Init *getFieldInit(Record &R, const RecordVal *RV,
                     StringInit *FieldName) const override;

  std::string getAsString() const override;

  Init *getBit(unsigned Bit) const override {
    llvm_unreachable("Illegal bit reference off def");
  }

  /// This method is used to implement
  /// VarListElementInit::resolveReferences.  If the list element is resolvable
  /// now, we return the resolved value, otherwise we return null.
  Init *resolveListElementReference(Record &R, const RecordVal *RV,
                                    unsigned Elt) const override {
    llvm_unreachable("Illegal element reference off def");
  }
};

/// X.Y - Represent a reference to a subfield of a variable
class FieldInit : public TypedInit {
  Init *Rec;                // Record we are referring to
  StringInit *FieldName;    // Field we are accessing

  FieldInit(Init *R, StringInit *FN)
      : TypedInit(IK_FieldInit, R->getFieldType(FN)), Rec(R), FieldName(FN) {
    assert(getType() && "FieldInit with non-record type!");
  }

public:
  FieldInit(const FieldInit &) = delete;
  FieldInit &operator=(const FieldInit &) = delete;

  static bool classof(const Init *I) {
    return I->getKind() == IK_FieldInit;
  }

  static FieldInit *get(Init *R, StringInit *FN);

  Init *getBit(unsigned Bit) const override;

  Init *resolveListElementReference(Record &R, const RecordVal *RV,
                                    unsigned Elt) const override;

  Init *resolveReferences(Record &R, const RecordVal *RV) const override;

  std::string getAsString() const override {
    return Rec->getAsString() + "." + FieldName->getValue().str();
  }
};

/// (v a, b) - Represent a DAG tree value.  DAG inits are required
/// to have at least one value then a (possibly empty) list of arguments.  Each
/// argument can have a name associated with it.
class DagInit final : public TypedInit, public FoldingSetNode,
                      public TrailingObjects<DagInit, Init *, StringInit *> {
  friend TrailingObjects;

  Init *Val;
  StringInit *ValName;
  unsigned NumArgs;
  unsigned NumArgNames;

  DagInit(Init *V, StringInit *VN, unsigned NumArgs, unsigned NumArgNames)
      : TypedInit(IK_DagInit, DagRecTy::get()), Val(V), ValName(VN),
        NumArgs(NumArgs), NumArgNames(NumArgNames) {}

  size_t numTrailingObjects(OverloadToken<Init *>) const { return NumArgs; }

public:
  DagInit(const DagInit &) = delete;
  DagInit &operator=(const DagInit &) = delete;

  static bool classof(const Init *I) {
    return I->getKind() == IK_DagInit;
  }

  static DagInit *get(Init *V, StringInit *VN, ArrayRef<Init *> ArgRange,
                      ArrayRef<StringInit*> NameRange);
  static DagInit *get(Init *V, StringInit *VN,
                      ArrayRef<std::pair<Init*, StringInit*>> Args);

  void Profile(FoldingSetNodeID &ID) const;

  Init *convertInitializerTo(RecTy *Ty) const override;

  Init *getOperator() const { return Val; }

  StringInit *getName() const { return ValName; }

  StringRef getNameStr() const {
    return ValName ? ValName->getValue() : StringRef();
  }

  unsigned getNumArgs() const { return NumArgs; }

  Init *getArg(unsigned Num) const {
    assert(Num < NumArgs && "Arg number out of range!");
    return getTrailingObjects<Init *>()[Num];
  }

  StringInit *getArgName(unsigned Num) const {
    assert(Num < NumArgNames && "Arg number out of range!");
    return getTrailingObjects<StringInit *>()[Num];
  }

  StringRef getArgNameStr(unsigned Num) const {
    StringInit *Init = getArgName(Num);
    return Init ? Init->getValue() : StringRef();
  }

  ArrayRef<Init *> getArgs() const {
    return makeArrayRef(getTrailingObjects<Init *>(), NumArgs);
  }

  ArrayRef<StringInit *> getArgNames() const {
    return makeArrayRef(getTrailingObjects<StringInit *>(), NumArgNames);
  }

  Init *resolveReferences(Record &R, const RecordVal *RV) const override;

  std::string getAsString() const override;

  using const_arg_iterator = SmallVectorImpl<Init*>::const_iterator;
  using const_name_iterator = SmallVectorImpl<StringInit*>::const_iterator;

  inline const_arg_iterator  arg_begin() const { return getArgs().begin(); }
  inline const_arg_iterator  arg_end  () const { return getArgs().end(); }

  inline size_t              arg_size () const { return NumArgs; }
  inline bool                arg_empty() const { return NumArgs == 0; }

  inline const_name_iterator name_begin() const { return getArgNames().begin();}
  inline const_name_iterator name_end  () const { return getArgNames().end(); }

  inline size_t              name_size () const { return NumArgNames; }
  inline bool                name_empty() const { return NumArgNames == 0; }

  Init *getBit(unsigned Bit) const override {
    llvm_unreachable("Illegal bit reference off dag");
  }

  Init *resolveListElementReference(Record &R, const RecordVal *RV,
                                    unsigned Elt) const override {
    llvm_unreachable("Illegal element reference off dag");
  }
};

//===----------------------------------------------------------------------===//
//  High-Level Classes
//===----------------------------------------------------------------------===//

class RecordVal {
  friend class Record;

  Init *Name;
  PointerIntPair<RecTy *, 1, bool> TyAndPrefix;
  Init *Value;

public:
  RecordVal(Init *N, RecTy *T, bool P);

  StringRef getName() const;
  Init *getNameInit() const { return Name; }

  std::string getNameInitAsString() const {
    return getNameInit()->getAsUnquotedString();
  }

  bool getPrefix() const { return TyAndPrefix.getInt(); }
  RecTy *getType() const { return TyAndPrefix.getPointer(); }
  Init *getValue() const { return Value; }

  bool setValue(Init *V) {
    if (V) {
      Value = V->convertInitializerTo(getType());
      return Value == nullptr;
    }
    Value = nullptr;
    return false;
  }

  void dump() const;
  void print(raw_ostream &OS, bool PrintSem = true) const;
};

inline raw_ostream &operator<<(raw_ostream &OS, const RecordVal &RV) {
  RV.print(OS << "  ");
  return OS;
}

class Record {
  static unsigned LastID;

  Init *Name;
  // Location where record was instantiated, followed by the location of
  // multiclass prototypes used.
  SmallVector<SMLoc, 4> Locs;
  SmallVector<Init *, 0> TemplateArgs;
  SmallVector<RecordVal, 0> Values;
  SmallVector<std::pair<Record *, SMRange>, 0> SuperClasses;

  // Tracks Record instances. Not owned by Record.
  RecordKeeper &TrackedRecords;

  DefInit *TheInit = nullptr;

  // Unique record ID.
  unsigned ID;

  bool IsAnonymous;

  // Class-instance values can be used by other defs.  For example, Struct<i>
  // is used here as a template argument to another class:
  //
  //   multiclass MultiClass<int i> {
  //     def Def : Class<Struct<i>>;
  //
  // These need to get fully resolved before instantiating any other
  // definitions that use them (e.g. Def).  However, inside a multiclass they
  // can't be immediately resolved so we mark them ResolveFirst to fully
  // resolve them later as soon as the multiclass is instantiated.
  bool ResolveFirst = false;

  void init();
  void checkName();

public:
  // Constructs a record.
  explicit Record(Init *N, ArrayRef<SMLoc> locs, RecordKeeper &records,
                  bool Anonymous = false) :
    Name(N), Locs(locs.begin(), locs.end()), TrackedRecords(records),
    ID(LastID++), IsAnonymous(Anonymous) {
    init();
  }

  explicit Record(StringRef N, ArrayRef<SMLoc> locs, RecordKeeper &records,
                  bool Anonymous = false)
    : Record(StringInit::get(N), locs, records, Anonymous) {}

  // When copy-constructing a Record, we must still guarantee a globally unique
  // ID number.  Don't copy TheInit either since it's owned by the original
  // record. All other fields can be copied normally.
  Record(const Record &O) :
    Name(O.Name), Locs(O.Locs), TemplateArgs(O.TemplateArgs),
    Values(O.Values), SuperClasses(O.SuperClasses),
    TrackedRecords(O.TrackedRecords), ID(LastID++),
    IsAnonymous(O.IsAnonymous), ResolveFirst(O.ResolveFirst) { }

  static unsigned getNewUID() { return LastID++; }

  unsigned getID() const { return ID; }

  StringRef getName() const;

  Init *getNameInit() const {
    return Name;
  }

  const std::string getNameInitAsString() const {
    return getNameInit()->getAsUnquotedString();
  }

  void setName(Init *Name);      // Also updates RecordKeeper.

  ArrayRef<SMLoc> getLoc() const { return Locs; }

  /// get the corresponding DefInit.
  DefInit *getDefInit();

  ArrayRef<Init *> getTemplateArgs() const {
    return TemplateArgs;
  }

  ArrayRef<RecordVal> getValues() const { return Values; }

  ArrayRef<std::pair<Record *, SMRange>>  getSuperClasses() const {
    return SuperClasses;
  }

  bool isTemplateArg(Init *Name) const {
    for (Init *TA : TemplateArgs)
      if (TA == Name) return true;
    return false;
  }

  const RecordVal *getValue(const Init *Name) const {
    for (const RecordVal &Val : Values)
      if (Val.Name == Name) return &Val;
    return nullptr;
  }

  const RecordVal *getValue(StringRef Name) const {
    return getValue(StringInit::get(Name));
  }

  RecordVal *getValue(const Init *Name) {
    return const_cast<RecordVal *>(static_cast<const Record *>(this)->getValue(Name));
  }

  RecordVal *getValue(StringRef Name) {
    return const_cast<RecordVal *>(static_cast<const Record *>(this)->getValue(Name));
  }

  void addTemplateArg(Init *Name) {
    assert(!isTemplateArg(Name) && "Template arg already defined!");
    TemplateArgs.push_back(Name);
  }

  void addValue(const RecordVal &RV) {
    assert(getValue(RV.getNameInit()) == nullptr && "Value already added!");
    Values.push_back(RV);
    if (Values.size() > 1)
      // Keep NAME at the end of the list.  It makes record dumps a
      // bit prettier and allows TableGen tests to be written more
      // naturally.  Tests can use CHECK-NEXT to look for Record
      // fields they expect to see after a def.  They can't do that if
      // NAME is the first Record field.
      std::swap(Values[Values.size() - 2], Values[Values.size() - 1]);
  }

  void removeValue(Init *Name) {
    for (unsigned i = 0, e = Values.size(); i != e; ++i)
      if (Values[i].getNameInit() == Name) {
        Values.erase(Values.begin()+i);
        return;
      }
    llvm_unreachable("Cannot remove an entry that does not exist!");
  }

  void removeValue(StringRef Name) {
    removeValue(StringInit::get(Name));
  }

  bool isSubClassOf(const Record *R) const {
    for (const auto &SCPair : SuperClasses)
      if (SCPair.first == R)
        return true;
    return false;
  }

  bool isSubClassOf(StringRef Name) const {
    for (const auto &SCPair : SuperClasses) {
      if (const auto *SI = dyn_cast<StringInit>(SCPair.first->getNameInit())) {
        if (SI->getValue() == Name)
          return true;
      } else if (SCPair.first->getNameInitAsString() == Name) {
        return true;
      }
    }
    return false;
  }

  void addSuperClass(Record *R, SMRange Range) {
    assert(!isSubClassOf(R) && "Already subclassing record!");
    SuperClasses.push_back(std::make_pair(R, Range));
  }

  /// If there are any field references that refer to fields
  /// that have been filled in, we can propagate the values now.
  void resolveReferences() { resolveReferencesTo(nullptr); }

  /// If anything in this record refers to RV, replace the
  /// reference to RV with the RHS of RV.  If RV is null, we resolve all
  /// possible references.
  void resolveReferencesTo(const RecordVal *RV);

  RecordKeeper &getRecords() const {
    return TrackedRecords;
  }

  bool isAnonymous() const {
    return IsAnonymous;
  }

  bool isResolveFirst() const {
    return ResolveFirst;
  }

  void setResolveFirst(bool b) {
    ResolveFirst = b;
  }

  void print(raw_ostream &OS) const;
  void dump() const;

  //===--------------------------------------------------------------------===//
  // High-level methods useful to tablegen back-ends
  //

  /// Return the initializer for a value with the specified name,
  /// or throw an exception if the field does not exist.
  Init *getValueInit(StringRef FieldName) const;

  /// Return true if the named field is unset.
  bool isValueUnset(StringRef FieldName) const {
    return isa<UnsetInit>(getValueInit(FieldName));
  }

  /// This method looks up the specified field and returns
  /// its value as a string, throwing an exception if the field does not exist
  /// or if the value is not a string.
  StringRef getValueAsString(StringRef FieldName) const;

  /// This method looks up the specified field and returns
  /// its value as a BitsInit, throwing an exception if the field does not exist
  /// or if the value is not the right type.
  BitsInit *getValueAsBitsInit(StringRef FieldName) const;

  /// This method looks up the specified field and returns
  /// its value as a ListInit, throwing an exception if the field does not exist
  /// or if the value is not the right type.
  ListInit *getValueAsListInit(StringRef FieldName) const;

  /// This method looks up the specified field and
  /// returns its value as a vector of records, throwing an exception if the
  /// field does not exist or if the value is not the right type.
  std::vector<Record*> getValueAsListOfDefs(StringRef FieldName) const;

  /// This method looks up the specified field and
  /// returns its value as a vector of integers, throwing an exception if the
  /// field does not exist or if the value is not the right type.
  std::vector<int64_t> getValueAsListOfInts(StringRef FieldName) const;

  /// This method looks up the specified field and
  /// returns its value as a vector of strings, throwing an exception if the
  /// field does not exist or if the value is not the right type.
  std::vector<StringRef> getValueAsListOfStrings(StringRef FieldName) const;

  /// This method looks up the specified field and returns its
  /// value as a Record, throwing an exception if the field does not exist or if
  /// the value is not the right type.
  Record *getValueAsDef(StringRef FieldName) const;

  /// This method looks up the specified field and returns its
  /// value as a bit, throwing an exception if the field does not exist or if
  /// the value is not the right type.
  bool getValueAsBit(StringRef FieldName) const;

  /// This method looks up the specified field and
  /// returns its value as a bit. If the field is unset, sets Unset to true and
  /// returns false.
  bool getValueAsBitOrUnset(StringRef FieldName, bool &Unset) const;

  /// This method looks up the specified field and returns its
  /// value as an int64_t, throwing an exception if the field does not exist or
  /// if the value is not the right type.
  int64_t getValueAsInt(StringRef FieldName) const;

  /// This method looks up the specified field and returns its
  /// value as an Dag, throwing an exception if the field does not exist or if
  /// the value is not the right type.
  DagInit *getValueAsDag(StringRef FieldName) const;
};

raw_ostream &operator<<(raw_ostream &OS, const Record &R);

struct MultiClass {
  Record Rec;  // Placeholder for template args and Name.
  using RecordVector = std::vector<std::unique_ptr<Record>>;
  RecordVector DefPrototypes;

  void dump() const;

  MultiClass(StringRef Name, SMLoc Loc, RecordKeeper &Records) :
    Rec(Name, Loc, Records) {}
};

class RecordKeeper {
  using RecordMap = std::map<std::string, std::unique_ptr<Record>>;
  RecordMap Classes, Defs;

public:
  const RecordMap &getClasses() const { return Classes; }
  const RecordMap &getDefs() const { return Defs; }

  Record *getClass(StringRef Name) const {
    auto I = Classes.find(Name);
    return I == Classes.end() ? nullptr : I->second.get();
  }

  Record *getDef(StringRef Name) const {
    auto I = Defs.find(Name);
    return I == Defs.end() ? nullptr : I->second.get();
  }

  void addClass(std::unique_ptr<Record> R) {
    bool Ins = Classes.insert(std::make_pair(R->getName(),
                                             std::move(R))).second;
    (void)Ins;
    assert(Ins && "Class already exists");
  }

  void addDef(std::unique_ptr<Record> R) {
    bool Ins = Defs.insert(std::make_pair(R->getName(),
                                          std::move(R))).second;
    (void)Ins;
    assert(Ins && "Record already exists");
  }

  //===--------------------------------------------------------------------===//
  // High-level helper methods, useful for tablegen backends...

  /// This method returns all concrete definitions
  /// that derive from the specified class name.  A class with the specified
  /// name must exist.
  std::vector<Record *> getAllDerivedDefinitions(StringRef ClassName) const;

  void dump() const;
};

/// Sorting predicate to sort record pointers by name.
struct LessRecord {
  bool operator()(const Record *Rec1, const Record *Rec2) const {
    return StringRef(Rec1->getName()).compare_numeric(Rec2->getName()) < 0;
  }
};

/// Sorting predicate to sort record pointers by their
/// unique ID. If you just need a deterministic order, use this, since it
/// just compares two `unsigned`; the other sorting predicates require
/// string manipulation.
struct LessRecordByID {
  bool operator()(const Record *LHS, const Record *RHS) const {
    return LHS->getID() < RHS->getID();
  }
};

/// Sorting predicate to sort record pointers by their
/// name field.
struct LessRecordFieldName {
  bool operator()(const Record *Rec1, const Record *Rec2) const {
    return Rec1->getValueAsString("Name") < Rec2->getValueAsString("Name");
  }
};

struct LessRecordRegister {
  static bool ascii_isdigit(char x) { return x >= '0' && x <= '9'; }

  struct RecordParts {
    SmallVector<std::pair< bool, StringRef>, 4> Parts;

    RecordParts(StringRef Rec) {
      if (Rec.empty())
        return;

      size_t Len = 0;
      const char *Start = Rec.data();
      const char *Curr = Start;
      bool isDigitPart = ascii_isdigit(Curr[0]);
      for (size_t I = 0, E = Rec.size(); I != E; ++I, ++Len) {
        bool isDigit = ascii_isdigit(Curr[I]);
        if (isDigit != isDigitPart) {
          Parts.push_back(std::make_pair(isDigitPart, StringRef(Start, Len)));
          Len = 0;
          Start = &Curr[I];
          isDigitPart = ascii_isdigit(Curr[I]);
        }
      }
      // Push the last part.
      Parts.push_back(std::make_pair(isDigitPart, StringRef(Start, Len)));
    }

    size_t size() { return Parts.size(); }

    std::pair<bool, StringRef> getPart(size_t i) {
      assert (i < Parts.size() && "Invalid idx!");
      return Parts[i];
    }
  };

  bool operator()(const Record *Rec1, const Record *Rec2) const {
    RecordParts LHSParts(StringRef(Rec1->getName()));
    RecordParts RHSParts(StringRef(Rec2->getName()));

    size_t LHSNumParts = LHSParts.size();
    size_t RHSNumParts = RHSParts.size();
    assert (LHSNumParts && RHSNumParts && "Expected at least one part!");

    if (LHSNumParts != RHSNumParts)
      return LHSNumParts < RHSNumParts;

    // We expect the registers to be of the form [_a-zA-z]+([0-9]*[_a-zA-Z]*)*.
    for (size_t I = 0, E = LHSNumParts; I < E; I+=2) {
      std::pair<bool, StringRef> LHSPart = LHSParts.getPart(I);
      std::pair<bool, StringRef> RHSPart = RHSParts.getPart(I);
      // Expect even part to always be alpha.
      assert (LHSPart.first == false && RHSPart.first == false &&
              "Expected both parts to be alpha.");
      if (int Res = LHSPart.second.compare(RHSPart.second))
        return Res < 0;
    }
    for (size_t I = 1, E = LHSNumParts; I < E; I+=2) {
      std::pair<bool, StringRef> LHSPart = LHSParts.getPart(I);
      std::pair<bool, StringRef> RHSPart = RHSParts.getPart(I);
      // Expect odd part to always be numeric.
      assert (LHSPart.first == true && RHSPart.first == true &&
              "Expected both parts to be numeric.");
      if (LHSPart.second.size() != RHSPart.second.size())
        return LHSPart.second.size() < RHSPart.second.size();

      unsigned LHSVal, RHSVal;

      bool LHSFailed = LHSPart.second.getAsInteger(10, LHSVal); (void)LHSFailed;
      assert(!LHSFailed && "Unable to convert LHS to integer.");
      bool RHSFailed = RHSPart.second.getAsInteger(10, RHSVal); (void)RHSFailed;
      assert(!RHSFailed && "Unable to convert RHS to integer.");

      if (LHSVal != RHSVal)
        return LHSVal < RHSVal;
    }
    return LHSNumParts < RHSNumParts;
  }
};

raw_ostream &operator<<(raw_ostream &OS, const RecordKeeper &RK);

/// Return an Init with a qualifier prefix referring
/// to CurRec's name.
Init *QualifyName(Record &CurRec, MultiClass *CurMultiClass,
                  Init *Name, StringRef Scoper);

} // end namespace llvm

#endif // LLVM_TABLEGEN_RECORD_H
