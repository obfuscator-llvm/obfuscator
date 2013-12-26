//===- SubstitutionIncludes.h - Substitution Obfuscation pass-------------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file contains includes and defines for the substitution pass
//
//===----------------------------------------------------------------------===//

#ifndef _SUBSTITUTIONS_H_
#define _SUBSTITUTIONS_H_


// LLVM include
#include "llvm/Pass.h"
#include "llvm/Function.h"
#include "llvm/Instructions.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/Transforms/IPO.h"
#include "llvm/Transforms/IPO.h"
#include "llvm/Module.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/PrngAESCtr.h"

// Namespace
using namespace llvm;
using namespace std;

void addNeg(BinaryOperator *bo);
void addDoubleNeg(BinaryOperator *bo);
void addRand(BinaryOperator *bo);
void addRand2(BinaryOperator *bo);

void subNeg(BinaryOperator *bo);
void subRand(BinaryOperator *bo);
void subRand2(BinaryOperator *bo);

void andSubstitution(BinaryOperator *bo);
void andSubstitutionRand(BinaryOperator *bo);
void orSubstitution(BinaryOperator *bo);
void orSubstitutionRand(BinaryOperator *bo);
void xorSubstitution(BinaryOperator *bo);
void xorSubstitutionRand(BinaryOperator *bo);

namespace llvm {
	Pass *createSubstitution ();
}

#endif

