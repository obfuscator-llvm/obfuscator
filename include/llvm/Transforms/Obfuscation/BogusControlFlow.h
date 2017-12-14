//===- BogusControlFlow.h - bogus control flow obfuscation pass -----------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file contains includes and defines for the bogus control flow pass
//
//===----------------------------------------------------------------------===//
#ifndef _OBFUSCATION_BOGUSCONTROLFLOW_H_
#define _OBFUSCATION_BOGUSCONTROLFLOW_H_

#include "llvm/ADT/Statistic.h"
#include "llvm/CodeGen/ISDOpcodes.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/GlobalValue.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Type.h"
#include "llvm/Pass.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/IPO.h"
#include "llvm/Transforms/Obfuscation/CryptoUtils.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include "llvm/Transforms/Utils/Cloning.h"

namespace llvm {
Pass *createBogus();
Pass *createBogus(bool flag);
} // namespace llvm

#endif
