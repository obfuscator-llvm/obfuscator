//===--- Stencil.cpp - Stencil implementation -------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "clang/Tooling/Refactoring/Stencil.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/ASTTypeTraits.h"
#include "clang/AST/Expr.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "clang/ASTMatchers/ASTMatchers.h"
#include "clang/Lex/Lexer.h"
#include "clang/Tooling/Refactoring/SourceCode.h"
#include "llvm/Support/Errc.h"
#include <atomic>
#include <memory>
#include <string>

using namespace clang;
using namespace tooling;

using ast_matchers::MatchFinder;
using llvm::Error;

// A down_cast function to safely down cast a StencilPartInterface to a subclass
// D. Returns nullptr if P is not an instance of D.
template <typename D> const D *down_cast(const StencilPartInterface *P) {
  if (P == nullptr || D::typeId() != P->typeId())
    return nullptr;
  return static_cast<const D *>(P);
}

static llvm::Expected<ast_type_traits::DynTypedNode>
getNode(const ast_matchers::BoundNodes &Nodes, StringRef Id) {
  auto &NodesMap = Nodes.getMap();
  auto It = NodesMap.find(Id);
  if (It == NodesMap.end())
    return llvm::make_error<llvm::StringError>(llvm::errc::invalid_argument,
                                               "Id not bound: " + Id);
  return It->second;
}

namespace {
// An arbitrary fragment of code within a stencil.
struct RawTextData {
  explicit RawTextData(std::string T) : Text(std::move(T)) {}
  std::string Text;
};

// A debugging operation to dump the AST for a particular (bound) AST node.
struct DebugPrintNodeOpData {
  explicit DebugPrintNodeOpData(std::string S) : Id(std::move(S)) {}
  std::string Id;
};

// The fragment of code corresponding to the selected range.
struct SelectorOpData {
  explicit SelectorOpData(RangeSelector S) : Selector(std::move(S)) {}
  RangeSelector Selector;
};
} // namespace

bool isEqualData(const RawTextData &A, const RawTextData &B) {
  return A.Text == B.Text;
}

bool isEqualData(const DebugPrintNodeOpData &A, const DebugPrintNodeOpData &B) {
  return A.Id == B.Id;
}

// Equality is not (yet) defined for \c RangeSelector.
bool isEqualData(const SelectorOpData &, const SelectorOpData &) { return false; }

// The `evalData()` overloads evaluate the given stencil data to a string, given
// the match result, and append it to `Result`. We define an overload for each
// type of stencil data.

Error evalData(const RawTextData &Data, const MatchFinder::MatchResult &,
               std::string *Result) {
  Result->append(Data.Text);
  return Error::success();
}

Error evalData(const DebugPrintNodeOpData &Data,
               const MatchFinder::MatchResult &Match, std::string *Result) {
  std::string Output;
  llvm::raw_string_ostream Os(Output);
  auto NodeOrErr = getNode(Match.Nodes, Data.Id);
  if (auto Err = NodeOrErr.takeError())
    return Err;
  NodeOrErr->print(Os, PrintingPolicy(Match.Context->getLangOpts()));
  *Result += Os.str();
  return Error::success();
}

Error evalData(const SelectorOpData &Data, const MatchFinder::MatchResult &Match,
               std::string *Result) {
  auto Range = Data.Selector(Match);
  if (!Range)
    return Range.takeError();
  *Result += getText(*Range, *Match.Context);
  return Error::success();
}

template <typename T>
class StencilPartImpl : public StencilPartInterface {
  T Data;

public:
  template <typename... Ps>
  explicit StencilPartImpl(Ps &&... Args)
      : StencilPartInterface(StencilPartImpl::typeId()),
        Data(std::forward<Ps>(Args)...) {}

  // Generates a unique identifier for this class (specifically, one per
  // instantiation of the template).
  static const void* typeId() {
    static bool b;
    return &b;
  }

  Error eval(const MatchFinder::MatchResult &Match,
             std::string *Result) const override {
    return evalData(Data, Match, Result);
  }

  bool isEqual(const StencilPartInterface &Other) const override {
    if (const auto *OtherPtr = down_cast<StencilPartImpl>(&Other))
      return isEqualData(Data, OtherPtr->Data);
    return false;
  }
};

namespace {
using RawText = StencilPartImpl<RawTextData>;
using DebugPrintNodeOp = StencilPartImpl<DebugPrintNodeOpData>;
using SelectorOp = StencilPartImpl<SelectorOpData>;
} // namespace

StencilPart Stencil::wrap(StringRef Text) {
  return stencil::text(Text);
}

StencilPart Stencil::wrap(RangeSelector Selector) {
  return stencil::selection(std::move(Selector));
}

void Stencil::append(Stencil OtherStencil) {
  for (auto &Part : OtherStencil.Parts)
    Parts.push_back(std::move(Part));
}

llvm::Expected<std::string>
Stencil::eval(const MatchFinder::MatchResult &Match) const {
  std::string Result;
  for (const auto &Part : Parts)
    if (auto Err = Part.eval(Match, &Result))
      return std::move(Err);
  return Result;
}

StencilPart stencil::text(StringRef Text) {
  return StencilPart(std::make_shared<RawText>(Text));
}

StencilPart stencil::selection(RangeSelector Selector) {
  return StencilPart(std::make_shared<SelectorOp>(std::move(Selector)));
}

StencilPart stencil::dPrint(StringRef Id) {
  return StencilPart(std::make_shared<DebugPrintNodeOp>(Id));
}
