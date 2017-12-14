//===-- BPFInstPrinter.cpp - Convert BPF MCInst to asm syntax -------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This class prints an BPF MCInst to a .s file.
//
//===----------------------------------------------------------------------===//

#include "BPFInstPrinter.h"
#include "BPF.h"
#include "llvm/MC/MCAsmInfo.h"
#include "llvm/MC/MCExpr.h"
#include "llvm/MC/MCInst.h"
#include "llvm/MC/MCSymbol.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/FormattedStream.h"
using namespace llvm;

#define DEBUG_TYPE "asm-printer"

// Include the auto-generated portion of the assembly writer.
#include "BPFGenAsmWriter.inc"

void BPFInstPrinter::printInst(const MCInst *MI, raw_ostream &O,
                               StringRef Annot, const MCSubtargetInfo &STI) {
  printInstruction(MI, O);
  printAnnotation(O, Annot);
}

static void printExpr(const MCExpr *Expr, raw_ostream &O) {
#ifndef NDEBUG
  const MCSymbolRefExpr *SRE;

  if (const MCBinaryExpr *BE = dyn_cast<MCBinaryExpr>(Expr))
    SRE = dyn_cast<MCSymbolRefExpr>(BE->getLHS());
  else
    SRE = dyn_cast<MCSymbolRefExpr>(Expr);
  assert(SRE && "Unexpected MCExpr type.");

  MCSymbolRefExpr::VariantKind Kind = SRE->getKind();

  assert(Kind == MCSymbolRefExpr::VK_None);
#endif
  O << *Expr;
}

void BPFInstPrinter::printOperand(const MCInst *MI, unsigned OpNo,
                                  raw_ostream &O, const char *Modifier) {
  assert((Modifier == 0 || Modifier[0] == 0) && "No modifiers supported");
  const MCOperand &Op = MI->getOperand(OpNo);
  if (Op.isReg()) {
    O << getRegisterName(Op.getReg());
  } else if (Op.isImm()) {
    O << (int32_t)Op.getImm();
  } else {
    assert(Op.isExpr() && "Expected an expression");
    printExpr(Op.getExpr(), O);
  }
}

void BPFInstPrinter::printMemOperand(const MCInst *MI, int OpNo, raw_ostream &O,
                                     const char *Modifier) {
  const MCOperand &RegOp = MI->getOperand(OpNo);
  const MCOperand &OffsetOp = MI->getOperand(OpNo + 1);

  // register
  assert(RegOp.isReg() && "Register operand not a register");
  O << getRegisterName(RegOp.getReg());

  // offset
  if (OffsetOp.isImm()) {
    auto Imm = OffsetOp.getImm();
    if (Imm >= 0)
      O << " + " << formatDec(Imm);
    else
      O << " - " << formatDec(-Imm);
  } else {
    assert(0 && "Expected an immediate");
  }
}

void BPFInstPrinter::printImm64Operand(const MCInst *MI, unsigned OpNo,
                                       raw_ostream &O) {
  const MCOperand &Op = MI->getOperand(OpNo);
  if (Op.isImm())
    O << (uint64_t)Op.getImm();
  else
    O << Op;
}
