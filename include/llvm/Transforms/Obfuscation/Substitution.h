//===- SubstitutionIncludes.h - Substitution Obfuscation ------------------===//
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

#ifndef _OBFUSCATION_SUBSTITUTIONS_H_
#define _OBFUSCATION_SUBSTITUTIONS_H_

#include "llvm/ADT/Statistic.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/Pass.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Transforms/IPO.h"
#include "llvm/Transforms/Obfuscation/CryptoUtils.h"

namespace llvm {
Pass *createSubstitution(bool flag);
} // namespace llvm

#endif
