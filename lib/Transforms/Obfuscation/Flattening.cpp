//===- Flattening.cpp - Flattening Obfuscation pass------------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implements the flattening pass
//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/Obfuscation/Flattening.h"
#include "llvm/PrngAESCtr.h"

#define DEBUG_TYPE "flattening"

using namespace llvm;

// Stats
STATISTIC(Flattened,  "Functions flattened");

static cl::opt<string>
FunctionName("funcFLA",cl::init(""),cl::desc("Flatten only certain functions: -mllvm -funcFLA=\"func1,func2\""));

static cl::opt<int>
Percentage("perFLA",cl::init(100),cl::desc("Flatten only a certain percentage of functions"));

namespace {
    struct Flattening : public FunctionPass {
        static char ID; // Pass identification, replacement for typeid
        Flattening(): FunctionPass(ID){}
        
        virtual bool runOnFunction(Function &F) {
            Function *tmp = &F;
            string func = FunctionName;
            
            // Check if declaration or variadic
            if(tmp->isDeclaration()) {
                return false;
            }
            
            // Check if the number of applications is correct
            if ( !((Percentage > 0) && (Percentage <= 100)) ) {
                LLVMContext &ctx = llvm::getGlobalContext();
                ctx.emitError(Twine ("Flattening application function percentage -perFLA=x must be 0 < x <= 100"));
            }

            
            // Check name
            else if(func.size() != 0 && func.find(tmp->getName()) != string::npos) {
                if(flatten(tmp)) {
                    ++Flattened;
                    return false;
                }
            }
            
            if((((int)llvm::cprng->get_range(100))) < Percentage && flatten(tmp)) {
                ++Flattened;
            }
            return false;
        }
    };
}

char Flattening::ID = 0;
static RegisterPass<Flattening> X("flattening", "Call graph flattening");

Pass *llvm::createFlattening() {
    return new Flattening();
}
