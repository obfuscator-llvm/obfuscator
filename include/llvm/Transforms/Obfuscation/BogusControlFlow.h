//===- BogusControlFlow.h - BogusControlFlow Obfuscation pass-------------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===--------------------------------------------------------------------------------===//
//
// This file contains includes and defines for the bogusControlFlow pass
//
//===--------------------------------------------------------------------------------===//

#ifndef _BOGUSCONTROLFLOW_H_
#define _BOGUSCONTROLFLOW_H_


// LLVM include
#include "llvm/Pass.h"
#include "llvm/Module.h"
#include "llvm/Function.h"
#include "llvm/BasicBlock.h"
#include "llvm/Instructions.h"
#include "llvm/InstrTypes.h"
#include "llvm/Constants.h"
#include "llvm/Type.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/GlobalValue.h"
#include "llvm/LLVMContext.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include "llvm/CodeGen/ISDOpcodes.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/CFG.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Transforms/IPO.h"
#include "llvm/PrngAESCtr.h"
#include <list>

// Stats
STATISTIC(NumFunction,  "a. Number of functions in this module");
STATISTIC(NumTimesOnFunctions, "b. Number of times we run on each function");
STATISTIC(InitNumBasicBlocks,  "c. Initial number of basic blocks in this module");
STATISTIC(NumModifiedBasicBlocks,  "d. Number of modified basic blocks");
STATISTIC(NumAddedBasicBlocks,  "e. Number of added basic blocks in this module");
STATISTIC(FinalNumBasicBlocks,  "f. Final number of basic blocks in this module");

// Namespace
namespace llvm {
	Pass *createBogus ();
}
using namespace std;
#endif

