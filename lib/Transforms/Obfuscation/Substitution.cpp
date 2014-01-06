//===- Substitution.cpp - Substitution Obfuscation pass-------------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implements operators substitution's pass
//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/Obfuscation/Substitution.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/Support/raw_ostream.h"

#define DEBUG_TYPE "substitution"

#define NUMBER_ADD_SUBST   4
#define NUMBER_SUB_SUBST   3
#define NUMBER_AND_SUBST   2
#define NUMBER_OR_SUBST    2
#define NUMBER_XOR_SUBST   2

static cl::opt<int>
ObfTimes("sub-loop", cl::desc("Choose how many time the -sub pass loops on a function"), cl::value_desc("number of times"), cl::init(1), cl::Optional);

static cl::opt<string>
FunctionName("funcSUB",cl::init(""),cl::desc("Substitute only certain functions: -mllvm -funcSUB=\"func1,func2\""));

static cl::opt<int>
Percentage("perSUB",cl::init(100),cl::desc("Substitute only a certain percentage [%] of functions: -mllvm -perSUB=10"));

// Stats
STATISTIC(Add,  "Add substitued");
STATISTIC(Sub,  "Sub substitued");
//STATISTIC(Mul,  "Mul substitued");
//STATISTIC(Div,  "Div substitued");
//STATISTIC(Rem,  "Rem substitued");
//STATISTIC(Shi,  "Shift substitued");
STATISTIC(And,  "And substitued");
STATISTIC(Or,   "Or substitued");
STATISTIC(Xor,  "Xor substitued");

namespace {

    struct Substitution : public FunctionPass {
        static char ID; // Pass identification, replacement for typeid
        void (*funcAdd[NUMBER_ADD_SUBST])(BinaryOperator *bo);
        void (*funcSub[NUMBER_SUB_SUBST])(BinaryOperator *bo);
        void (*funcAnd[NUMBER_AND_SUBST])(BinaryOperator *bo);
        void (*funcOr[NUMBER_OR_SUBST])(BinaryOperator *bo);
        void (*funcXor[NUMBER_XOR_SUBST])(BinaryOperator *bo);

        Substitution() : FunctionPass(ID) {

            funcAdd[0] = addNeg;
            funcAdd[1] = addDoubleNeg;
            funcAdd[2] = addRand;
            funcAdd[3] = addRand2;

            funcSub[0] = subNeg;
            funcSub[1] = subRand;
            funcSub[2] = subRand2;

            funcAnd[0] = andSubstitution;
            funcAnd[1] = andSubstitutionRand;

            funcOr[0] = orSubstitution;
            funcOr[1] = orSubstitutionRand;

            funcXor[0] = xorSubstitution;
            funcXor[1] = xorSubstitutionRand;
        }

        virtual bool runOnFunction(Function &F) {
            Function *tmp = &F;
            string func = FunctionName;

            // Check if declaration or variadic
            if(tmp->isDeclaration()) {
                return false;
            }

            // Check if the percentage is correct
            if ( !((Percentage > 0) && (Percentage <= 100)) ) {
                LLVMContext &ctx = llvm::getGlobalContext();
                ctx.emitError(Twine ("Substitution percentage -perSUB=x must be 0 < x <= 100"));
            }

            // Check if the percentage is correct
            if (ObfTimes <= 0) {
                LLVMContext &ctx = llvm::getGlobalContext();
                ctx.emitError(Twine ("Substitution application number -sub-loop=x must be x > 0"));
            }


            if( (func.size() != 0 && func.find(tmp->getName()) != string::npos) ||
               ((int) llvm::cprng->get_range(100) < Percentage) ) {
                // Loop for the number of time we run the pass on the function
                int times = ObfTimes;
                do{

                    for(Function::iterator bb=tmp->begin();bb!=tmp->end();++bb) {
                        for(BasicBlock::iterator inst=bb->begin();inst!=bb->end();++inst) {
                            if(inst->isBinaryOp()) {
                                switch(inst->getOpcode()) {
                                    case BinaryOperator::Add:
                                        //case BinaryOperator::FAdd:
                                        // Substitute with random add operation
                                        funcAdd[llvm::cprng->get_range(NUMBER_ADD_SUBST)](cast<BinaryOperator>(inst));
                                        ++Add;
                                        break;
                                    case BinaryOperator::Sub:
                                        //case BinaryOperator::FSub:
                                        // Substitute with random sub operation
                                        funcSub[llvm::cprng->get_range(NUMBER_SUB_SUBST)](cast<BinaryOperator>(inst));
                                        ++Sub;
                                        break;
                                    case BinaryOperator::Mul:
                                    case BinaryOperator::FMul:
                                        //++Mul;
                                        break;
                                    case BinaryOperator::UDiv:
                                    case BinaryOperator::SDiv:
                                    case BinaryOperator::FDiv:
                                        //++Div;
                                        break;
                                    case BinaryOperator::URem:
                                    case BinaryOperator::SRem:
                                    case BinaryOperator::FRem:
                                        //++Rem;
                                        break;
                                    case Instruction::Shl:
                                        //++Shi;
                                    break;
                                    case Instruction::LShr:
                                        //++Shi;
                                        break;
                                    case Instruction::AShr:
                                        //++Shi;
                                        break;
                                    case Instruction::And:
                                        funcAnd[llvm::cprng->get_range(2)](cast<BinaryOperator>(inst));
                                        ++And;
                                        break;
                                    case Instruction::Or:
                                        funcOr[llvm::cprng->get_range(2)](cast<BinaryOperator>(inst));
                                        ++Or;
                                        break;
                                    case Instruction::Xor:
                                        funcXor[llvm::cprng->get_range(2)](cast<BinaryOperator>(inst));
                                        ++Xor;
                                        break;
                                    default:
                                        break;
                                } // End switch
                            } // End isBinaryOp
                        } // End for basickblock
                    } // End for Function
                } while (--times > 0); // for times
            } // End if func name && percentage

            return false;
        }
    };
}

char Substitution::ID = 0;
static RegisterPass<Substitution> X("substitution", "operators substitution");
Pass *llvm::createSubstitution() {
    return new Substitution();
}
