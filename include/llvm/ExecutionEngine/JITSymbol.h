//===- JITSymbol.h - JIT symbol abstraction ---------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// Abstraction for target process addresses.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_EXECUTIONENGINE_JITSYMBOL_H
#define LLVM_EXECUTIONENGINE_JITSYMBOL_H

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

#include "llvm/Support/Error.h"

namespace llvm {

class GlobalValue;

namespace object {

class BasicSymbolRef;

} // end namespace object

/// @brief Represents an address in the target process's address space.
using JITTargetAddress = uint64_t;

/// @brief Flags for symbols in the JIT.
class JITSymbolFlags {
public:
  using UnderlyingType = uint8_t;

  enum FlagNames : UnderlyingType {
    None = 0,
    HasError = 1U << 0,
    Weak = 1U << 1,
    Common = 1U << 2,
    Absolute = 1U << 3,
    Exported = 1U << 4
  };

  /// @brief Default-construct a JITSymbolFlags instance.
  JITSymbolFlags() = default;

  /// @brief Construct a JITSymbolFlags instance from the given flags.
  JITSymbolFlags(FlagNames Flags) : Flags(Flags) {}

  /// @brief Return true if there was an error retrieving this symbol.
  bool hasError() const {
    return (Flags & HasError) == HasError;
  }

  /// @brief Returns true is the Weak flag is set.
  bool isWeak() const {
    return (Flags & Weak) == Weak;
  }

  /// @brief Returns true is the Weak flag is set.
  bool isCommon() const {
    return (Flags & Common) == Common;
  }

  bool isStrongDefinition() const {
    return !isWeak() && !isCommon();
  }

  /// @brief Returns true is the Weak flag is set.
  bool isExported() const {
    return (Flags & Exported) == Exported;
  }

  operator UnderlyingType&() { return Flags; }

  /// Construct a JITSymbolFlags value based on the flags of the given global
  /// value.
  static JITSymbolFlags fromGlobalValue(const GlobalValue &GV);

  /// Construct a JITSymbolFlags value based on the flags of the given libobject
  /// symbol.
  static JITSymbolFlags fromObjectSymbol(const object::BasicSymbolRef &Symbol);

private:
  UnderlyingType Flags = None;
};

/// @brief Represents a symbol that has been evaluated to an address already.
class JITEvaluatedSymbol {
public:
  /// @brief Create a 'null' symbol.
  JITEvaluatedSymbol(std::nullptr_t) {}

  /// @brief Create a symbol for the given address and flags.
  JITEvaluatedSymbol(JITTargetAddress Address, JITSymbolFlags Flags)
      : Address(Address), Flags(Flags) {}

  /// @brief An evaluated symbol converts to 'true' if its address is non-zero.
  explicit operator bool() const { return Address != 0; }

  /// @brief Return the address of this symbol.
  JITTargetAddress getAddress() const { return Address; }

  /// @brief Return the flags for this symbol.
  JITSymbolFlags getFlags() const { return Flags; }

private:
  JITTargetAddress Address = 0;
  JITSymbolFlags Flags;
};

/// @brief Represents a symbol in the JIT.
class JITSymbol {
public:
  using GetAddressFtor = std::function<Expected<JITTargetAddress>()>;

  /// @brief Create a 'null' symbol, used to represent a "symbol not found"
  ///        result from a successful (non-erroneous) lookup.
  JITSymbol(std::nullptr_t)
      : CachedAddr(0) {}

  /// @brief Create a JITSymbol representing an error in the symbol lookup
  ///        process (e.g. a network failure during a remote lookup).
  JITSymbol(Error Err)
    : Err(std::move(Err)), Flags(JITSymbolFlags::HasError) {}

  /// @brief Create a symbol for a definition with a known address.
  JITSymbol(JITTargetAddress Addr, JITSymbolFlags Flags)
      : CachedAddr(Addr), Flags(Flags) {}

  /// @brief Construct a JITSymbol from a JITEvaluatedSymbol.
  JITSymbol(JITEvaluatedSymbol Sym)
      : CachedAddr(Sym.getAddress()), Flags(Sym.getFlags()) {}

  /// @brief Create a symbol for a definition that doesn't have a known address
  ///        yet.
  /// @param GetAddress A functor to materialize a definition (fixing the
  ///        address) on demand.
  ///
  ///   This constructor allows a JIT layer to provide a reference to a symbol
  /// definition without actually materializing the definition up front. The
  /// user can materialize the definition at any time by calling the getAddress
  /// method.
  JITSymbol(GetAddressFtor GetAddress, JITSymbolFlags Flags)
      : GetAddress(std::move(GetAddress)), CachedAddr(0), Flags(Flags) {}

  JITSymbol(const JITSymbol&) = delete;
  JITSymbol& operator=(const JITSymbol&) = delete;

  JITSymbol(JITSymbol &&Other)
    : GetAddress(std::move(Other.GetAddress)), Flags(std::move(Other.Flags)) {
    if (Flags.hasError())
      Err = std::move(Other.Err);
    else
      CachedAddr = std::move(Other.CachedAddr);
  }

  JITSymbol& operator=(JITSymbol &&Other) {
    GetAddress = std::move(Other.GetAddress);
    Flags = std::move(Other.Flags);
    if (Flags.hasError())
      Err = std::move(Other.Err);
    else
      CachedAddr = std::move(Other.CachedAddr);
    return *this;
  }

  ~JITSymbol() {
    if (Flags.hasError())
      Err.~Error();
    else
      CachedAddr.~JITTargetAddress();
  }

  /// @brief Returns true if the symbol exists, false otherwise.
  explicit operator bool() const {
    return !Flags.hasError() && (CachedAddr || GetAddress);
  }

  /// @brief Move the error field value out of this JITSymbol.
  Error takeError() {
    if (Flags.hasError())
      return std::move(Err);
    return Error::success();
  }

  /// @brief Get the address of the symbol in the target address space. Returns
  ///        '0' if the symbol does not exist.
  Expected<JITTargetAddress> getAddress() {
    assert(!Flags.hasError() && "getAddress called on error value");
    if (GetAddress) {
      if (auto CachedAddrOrErr = GetAddress()) {
        GetAddress = nullptr;
        CachedAddr = *CachedAddrOrErr;
        assert(CachedAddr && "Symbol could not be materialized.");
      } else
        return CachedAddrOrErr.takeError();
    }
    return CachedAddr;
  }

  JITSymbolFlags getFlags() const { return Flags; }

private:
  GetAddressFtor GetAddress;
  union {
    JITTargetAddress CachedAddr;
    Error Err;
  };
  JITSymbolFlags Flags;
};

/// \brief Symbol resolution.
class JITSymbolResolver {
public:
  virtual ~JITSymbolResolver() = default;

  /// This method returns the address of the specified symbol if it exists
  /// within the logical dynamic library represented by this JITSymbolResolver.
  /// Unlike findSymbol, queries through this interface should return addresses
  /// for hidden symbols.
  ///
  /// This is of particular importance for the Orc JIT APIs, which support lazy
  /// compilation by breaking up modules: Each of those broken out modules
  /// must be able to resolve hidden symbols provided by the others. Clients
  /// writing memory managers for MCJIT can usually ignore this method.
  ///
  /// This method will be queried by RuntimeDyld when checking for previous
  /// definitions of common symbols.
  virtual JITSymbol findSymbolInLogicalDylib(const std::string &Name) = 0;

  /// This method returns the address of the specified function or variable.
  /// It is used to resolve symbols during module linking.
  ///
  /// If the returned symbol's address is equal to ~0ULL then RuntimeDyld will
  /// skip all relocations for that symbol, and the client will be responsible
  /// for handling them manually.
  virtual JITSymbol findSymbol(const std::string &Name) = 0;

private:
  virtual void anchor();
};

} // end namespace llvm

#endif // LLVM_EXECUTIONENGINE_JITSYMBOL_H
