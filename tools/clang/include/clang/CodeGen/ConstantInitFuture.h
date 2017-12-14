//===- ConstantInitFuture.h - "Future" constant initializers ----*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This class defines the ConstantInitFuture class.  This is split out
// from ConstantInitBuilder.h in order to allow APIs to work with it
// without having to include that entire header.  This is particularly
// important because it is often useful to be able to default-construct
// a future in, say, a default argument.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_CODEGEN_CONSTANTINITFUTURE_H
#define LLVM_CLANG_CODEGEN_CONSTANTINITFUTURE_H

#include "llvm/ADT/PointerUnion.h"
#include "llvm/IR/Constant.h"

// Forward-declare ConstantInitBuilderBase and give it a
// PointerLikeTypeTraits specialization so that we can safely use it
// in a PointerUnion below.
namespace clang {
namespace CodeGen {
class ConstantInitBuilderBase;
}
}
namespace llvm {
template <>
class PointerLikeTypeTraits< ::clang::CodeGen::ConstantInitBuilderBase*> {
public:
  using T = ::clang::CodeGen::ConstantInitBuilderBase*;

  static inline void *getAsVoidPointer(T p) { return p; }
  static inline T getFromVoidPointer(void *p) {return static_cast<T>(p);}
  enum { NumLowBitsAvailable = 2 };
};
}

namespace clang {
namespace CodeGen {

/// A "future" for a completed constant initializer, which can be passed
/// around independently of any sub-builders (but not the original parent).
class ConstantInitFuture {
  using PairTy = llvm::PointerUnion<ConstantInitBuilderBase*, llvm::Constant*>;

  PairTy Data;

  friend class ConstantInitBuilderBase;
  explicit ConstantInitFuture(ConstantInitBuilderBase *builder);

public:
  ConstantInitFuture() {}

  /// A future can be explicitly created from a fixed initializer.
  explicit ConstantInitFuture(llvm::Constant *initializer) : Data(initializer) {
    assert(initializer && "creating null future");
  }

  /// Is this future non-null?
  explicit operator bool() const { return bool(Data); }

  /// Return the type of the initializer.
  llvm::Type *getType() const;

  /// Abandon this initializer.
  void abandon();

  /// Install the initializer into a global variable.  This cannot
  /// be called multiple times.
  void installInGlobal(llvm::GlobalVariable *global);

  void *getOpaqueValue() const { return Data.getOpaqueValue(); }
  static ConstantInitFuture getFromOpaqueValue(void *value) {
    ConstantInitFuture result;
    result.Data = PairTy::getFromOpaqueValue(value);
    return result;
  }
  enum {
    NumLowBitsAvailable =
      llvm::PointerLikeTypeTraits<PairTy>::NumLowBitsAvailable
  };
};

}  // end namespace CodeGen
}  // end namespace clang

namespace llvm {

template <>
class PointerLikeTypeTraits< ::clang::CodeGen::ConstantInitFuture> {
public:
  using T = ::clang::CodeGen::ConstantInitFuture;

  static inline void *getAsVoidPointer(T future) {
    return future.getOpaqueValue();
  }
  static inline T getFromVoidPointer(void *p) {
    return T::getFromOpaqueValue(p);
  }
  enum { NumLowBitsAvailable = T::NumLowBitsAvailable };
};

} // end namespace llvm

#endif
