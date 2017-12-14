//===--- NamespaceEndCommentsFixer.h ----------------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
///
/// \file
/// \brief This file declares NamespaceEndCommentsFixer, a TokenAnalyzer that
/// fixes namespace end comments.
///
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_LIB_FORMAT_NAMESPACEENDCOMMENTSFIXER_H
#define LLVM_CLANG_LIB_FORMAT_NAMESPACEENDCOMMENTSFIXER_H

#include "TokenAnalyzer.h"

namespace clang {
namespace format {

class NamespaceEndCommentsFixer : public TokenAnalyzer {
public:
  NamespaceEndCommentsFixer(const Environment &Env, const FormatStyle &Style);

  tooling::Replacements
  analyze(TokenAnnotator &Annotator,
          SmallVectorImpl<AnnotatedLine *> &AnnotatedLines,
          FormatTokenLexer &Tokens) override;
};

} // end namespace format
} // end namespace clang

#endif
