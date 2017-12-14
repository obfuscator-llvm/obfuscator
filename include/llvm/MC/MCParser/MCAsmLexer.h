//===- llvm/MC/MCAsmLexer.h - Abstract Asm Lexer Interface ------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_MC_MCPARSER_MCASMLEXER_H
#define LLVM_MC_MCPARSER_MCASMLEXER_H

#include "llvm/ADT/APInt.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/SMLoc.h"
#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <string>

namespace llvm {

/// Target independent representation for an assembler token.
class AsmToken {
public:
  enum TokenKind {
    // Markers
    Eof, Error,

    // String values.
    Identifier,
    String,

    // Integer values.
    Integer,
    BigNum, // larger than 64 bits

    // Real values.
    Real,

    // Comments
    Comment,
    HashDirective,
    // No-value.
    EndOfStatement,
    Colon,
    Space,
    Plus, Minus, Tilde,
    Slash,     // '/'
    BackSlash, // '\'
    LParen, RParen, LBrac, RBrac, LCurly, RCurly,
    Star, Dot, Comma, Dollar, Equal, EqualEqual,

    Pipe, PipePipe, Caret,
    Amp, AmpAmp, Exclaim, ExclaimEqual, Percent, Hash,
    Less, LessEqual, LessLess, LessGreater,
    Greater, GreaterEqual, GreaterGreater, At,

    // MIPS unary expression operators such as %neg.
    PercentCall16, PercentCall_Hi, PercentCall_Lo, PercentDtprel_Hi,
    PercentDtprel_Lo, PercentGot, PercentGot_Disp, PercentGot_Hi, PercentGot_Lo,
    PercentGot_Ofst, PercentGot_Page, PercentGottprel, PercentGp_Rel, PercentHi,
    PercentHigher, PercentHighest, PercentLo, PercentNeg, PercentPcrel_Hi,
    PercentPcrel_Lo, PercentTlsgd, PercentTlsldm, PercentTprel_Hi,
    PercentTprel_Lo
  };

private:
  TokenKind Kind;

  /// A reference to the entire token contents; this is always a pointer into
  /// a memory buffer owned by the source manager.
  StringRef Str;

  APInt IntVal;

public:
  AsmToken() = default;
  AsmToken(TokenKind Kind, StringRef Str, APInt IntVal)
      : Kind(Kind), Str(Str), IntVal(std::move(IntVal)) {}
  AsmToken(TokenKind Kind, StringRef Str, int64_t IntVal = 0)
      : Kind(Kind), Str(Str), IntVal(64, IntVal, true) {}

  TokenKind getKind() const { return Kind; }
  bool is(TokenKind K) const { return Kind == K; }
  bool isNot(TokenKind K) const { return Kind != K; }

  SMLoc getLoc() const;
  SMLoc getEndLoc() const;
  SMRange getLocRange() const;

  /// Get the contents of a string token (without quotes).
  StringRef getStringContents() const {
    assert(Kind == String && "This token isn't a string!");
    return Str.slice(1, Str.size() - 1);
  }

  /// Get the identifier string for the current token, which should be an
  /// identifier or a string. This gets the portion of the string which should
  /// be used as the identifier, e.g., it does not include the quotes on
  /// strings.
  StringRef getIdentifier() const {
    if (Kind == Identifier)
      return getString();
    return getStringContents();
  }

  /// Get the string for the current token, this includes all characters (for
  /// example, the quotes on strings) in the token.
  ///
  /// The returned StringRef points into the source manager's memory buffer, and
  /// is safe to store across calls to Lex().
  StringRef getString() const { return Str; }

  // FIXME: Don't compute this in advance, it makes every token larger, and is
  // also not generally what we want (it is nicer for recovery etc. to lex 123br
  // as a single token, then diagnose as an invalid number).
  int64_t getIntVal() const {
    assert(Kind == Integer && "This token isn't an integer!");
    return IntVal.getZExtValue();
  }

  APInt getAPIntVal() const {
    assert((Kind == Integer || Kind == BigNum) &&
           "This token isn't an integer!");
    return IntVal;
  }
};

/// A callback class which is notified of each comment in an assembly file as
/// it is lexed.
class AsmCommentConsumer {
public:
  virtual ~AsmCommentConsumer() = default;

  /// Callback function for when a comment is lexed. Loc is the start of the
  /// comment text (excluding the comment-start marker). CommentText is the text
  /// of the comment, excluding the comment start and end markers, and the
  /// newline for single-line comments.
  virtual void HandleComment(SMLoc Loc, StringRef CommentText) = 0;
};


/// Generic assembler lexer interface, for use by target specific assembly
/// lexers.
class MCAsmLexer {
  /// The current token, stored in the base class for faster access.
  SmallVector<AsmToken, 1> CurTok;

  /// The location and description of the current error
  SMLoc ErrLoc;
  std::string Err;

protected: // Can only create subclasses.
  const char *TokStart = nullptr;
  bool SkipSpace = true;
  bool AllowAtInIdentifier;
  bool IsAtStartOfStatement = true;
  AsmCommentConsumer *CommentConsumer = nullptr;

  bool AltMacroMode;
  MCAsmLexer();

  virtual AsmToken LexToken() = 0;

  void SetError(SMLoc errLoc, const std::string &err) {
    ErrLoc = errLoc;
    Err = err;
  }

public:
  MCAsmLexer(const MCAsmLexer &) = delete;
  MCAsmLexer &operator=(const MCAsmLexer &) = delete;
  virtual ~MCAsmLexer();

  bool IsaAltMacroMode() {
    return AltMacroMode;
  }

  void SetAltMacroMode(bool AltMacroSet) {
    AltMacroMode = AltMacroSet;
  }

  /// Consume the next token from the input stream and return it.
  ///
  /// The lexer will continuosly return the end-of-file token once the end of
  /// the main input file has been reached.
  const AsmToken &Lex() {
    assert(!CurTok.empty());
    // Mark if we parsing out a EndOfStatement.
    IsAtStartOfStatement = CurTok.front().getKind() == AsmToken::EndOfStatement;
    CurTok.erase(CurTok.begin());
    // LexToken may generate multiple tokens via UnLex but will always return
    // the first one. Place returned value at head of CurTok vector.
    if (CurTok.empty()) {
      AsmToken T = LexToken();
      CurTok.insert(CurTok.begin(), T);
    }
    return CurTok.front();
  }

  void UnLex(AsmToken const &Token) {
    IsAtStartOfStatement = false;
    CurTok.insert(CurTok.begin(), Token);
  }

  bool isAtStartOfStatement() { return IsAtStartOfStatement; }

  virtual StringRef LexUntilEndOfStatement() = 0;

  /// Get the current source location.
  SMLoc getLoc() const;

  /// Get the current (last) lexed token.
  const AsmToken &getTok() const {
    return CurTok[0];
  }

  /// Look ahead at the next token to be lexed.
  const AsmToken peekTok(bool ShouldSkipSpace = true) {
    AsmToken Tok;

    MutableArrayRef<AsmToken> Buf(Tok);
    size_t ReadCount = peekTokens(Buf, ShouldSkipSpace);

    assert(ReadCount == 1);
    (void)ReadCount;

    return Tok;
  }

  /// Look ahead an arbitrary number of tokens.
  virtual size_t peekTokens(MutableArrayRef<AsmToken> Buf,
                            bool ShouldSkipSpace = true) = 0;

  /// Get the current error location
  SMLoc getErrLoc() {
    return ErrLoc;
  }

  /// Get the current error string
  const std::string &getErr() {
    return Err;
  }

  /// Get the kind of current token.
  AsmToken::TokenKind getKind() const { return getTok().getKind(); }

  /// Check if the current token has kind \p K.
  bool is(AsmToken::TokenKind K) const { return getTok().is(K); }

  /// Check if the current token has kind \p K.
  bool isNot(AsmToken::TokenKind K) const { return getTok().isNot(K); }

  /// Set whether spaces should be ignored by the lexer
  void setSkipSpace(bool val) { SkipSpace = val; }

  bool getAllowAtInIdentifier() { return AllowAtInIdentifier; }
  void setAllowAtInIdentifier(bool v) { AllowAtInIdentifier = v; }

  void setCommentConsumer(AsmCommentConsumer *CommentConsumer) {
    this->CommentConsumer = CommentConsumer;
  }
};

} // end namespace llvm

#endif // LLVM_MC_MCPARSER_MCASMLEXER_H
