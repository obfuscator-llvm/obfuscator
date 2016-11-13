//===- Substitution.cpp - Substitution Obfuscation
// pass-------------------------===//
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
#include "llvm/Transforms/Obfuscation/Utils.h"
#include "llvm/IR/Intrinsics.h"

#include <stdio.h>
#include <random>

#define DEBUG_TYPE "substitution"

#define NUMBER_ADD_SUBST 5
#define NUMBER_SUB_SUBST 4
#define NUMBER_AND_SUBST 3
#define NUMBER_OR_SUBST 2
#define NUMBER_XOR_SUBST 4

static cl::opt<int>
ObfTimes("sub-loop",
         cl::desc("Choose how many time the -sub pass loops on a function"),
         cl::value_desc("number of times"), cl::init(1), cl::Optional);

/*
static cl::opt<string> FunctionName(
    "funcSUB", cl::init(""),
    cl::desc(
        "Substitute only certain functions: -mllvm -funcSUB=\"func1,func2\""));
*/

/*
static cl::opt<int>
Percentage("perSUB", cl::init(100),
           cl::desc("Substitute only a certain percentage [%] "
                    "of functions: -mllvm -perSUB=10"));
*/

// Stats
STATISTIC(Add, "Add substitued");
STATISTIC(Sub, "Sub substitued");
// STATISTIC(Mul,  "Mul substitued");
// STATISTIC(Div,  "Div substitued");
// STATISTIC(Rem,  "Rem substitued");
// STATISTIC(Shi,  "Shift substitued");
STATISTIC(And, "And substitued");
STATISTIC(Or, "Or substitued");
STATISTIC(Xor, "Xor substitued");

namespace {

struct Substitution : public FunctionPass {
  static char ID; // Pass identification, replacement for typeid
  void (Substitution::*funcAdd[NUMBER_ADD_SUBST])(BinaryOperator *bo);
  void (Substitution::*funcSub[NUMBER_SUB_SUBST])(BinaryOperator *bo);
  void (Substitution::*funcAnd[NUMBER_AND_SUBST])(BinaryOperator *bo);
  void (Substitution::*funcOr[NUMBER_OR_SUBST])(BinaryOperator *bo);
  void (Substitution::*funcXor[NUMBER_XOR_SUBST])(BinaryOperator *bo);
  bool flag;

  Substitution() : FunctionPass(ID) {}

  Substitution(bool flag) : FunctionPass(ID) {
    // Check if the percentage is correct
    if (ObfTimes <= 0) {
      LLVMContext &ctx = llvm::getGlobalContext();
      ctx.emitError(
          Twine("Substitution application number -sub-loop=x must be x > 0"));
    }

    this->flag = flag;
    funcAdd[0] = &Substitution::addNeg;
    funcAdd[1] = &Substitution::addDoubleNeg;
    funcAdd[2] = &Substitution::addRand;
    funcAdd[3] = &Substitution::addRand2;
    funcAdd[4] = &Substitution::addAha;

    funcSub[0] = &Substitution::subNeg;
    funcSub[1] = &Substitution::subRand;
    funcSub[2] = &Substitution::subRand2;
    funcSub[3] = &Substitution::subAha;

    funcAnd[0] = &Substitution::andSubstitution;
    funcAnd[1] = &Substitution::andSubstitutionRand;
    funcAnd[2] = &Substitution::andSubstitutionRandExp;

    funcOr[0] = &Substitution::orSubstitution;
    funcOr[1] = &Substitution::orSubstitutionRand;

    funcXor[0] = &Substitution::xorSubstitution;
    funcXor[1] = &Substitution::xorSubstitutionRand;
    funcXor[2] = &Substitution::xorSubstitutionAha;
    funcXor[3] = &Substitution::xorSubstitutionAhaOpt;
  }

  bool runOnFunction(Function &F);
  bool substitute(Function *f);

  void addNeg(BinaryOperator *bo);
  void addDoubleNeg(BinaryOperator *bo);
  void addRand(BinaryOperator *bo);
  void addRand2(BinaryOperator *bo);
  void addAha(BinaryOperator *bo);

  void subNeg(BinaryOperator *bo);
  void subRand(BinaryOperator *bo);
  void subRand2(BinaryOperator *bo);
  void subAha(BinaryOperator *bo);

  void andSubstitution(BinaryOperator *bo);
  void andSubstitutionRand(BinaryOperator *bo);
  void andSubstitutionRandExp(BinaryOperator *bo);

  void orSubstitution(BinaryOperator *bo);
  void orSubstitutionRand(BinaryOperator *bo);

  void xorSubstitution(BinaryOperator *bo);
  void xorSubstitutionRand(BinaryOperator *bo);
  void xorSubstitutionAha(BinaryOperator *bo);
  void xorSubstitutionAhaOpt(BinaryOperator *bo);
};
}

char Substitution::ID = 0;
static RegisterPass<Substitution> X("substitution", "operators substitution");
Pass *llvm::createSubstitution(bool flag) { return new Substitution(flag); }

bool Substitution::runOnFunction(Function &F) {
  Function *tmp = &F;

  // Do we obfuscate
  if (toObfuscate(flag, tmp, "sub")) {
    substitute(tmp);
  }

  return false;
}

bool Substitution::substitute(Function *f) {
  Function *tmp = f;

  // Loop for the number of time we run the pass on the function
  int times = ObfTimes;
  do {
    for (Function::iterator bb = tmp->begin(); bb != tmp->end(); ++bb) {
      for (BasicBlock::iterator inst = bb->begin(); inst != bb->end(); ++inst) {
        if (inst->isBinaryOp()) {
          switch (inst->getOpcode()) {
          case BinaryOperator::Add:
            // case BinaryOperator::FAdd:
            // Substitute with random add operation
            (this->*funcAdd[/*llvm::cryptoutils->get_range(NUMBER_ADD_SUBST)*/4])(
                cast<BinaryOperator>(inst));
            ++Add;
            break;
          case BinaryOperator::Sub:
            // case BinaryOperator::FSub:
            // Substitute with random sub operation
            (this->*funcSub[/*llvm::cryptoutils->get_range(NUMBER_SUB_SUBST)*/3])(
                cast<BinaryOperator>(inst));
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
            (this->*
             funcAnd[/*llvm::cryptoutils->get_range(NUMBER_AND_SUBST)*/2])(cast<BinaryOperator>(inst));
            ++And;
            break;
          case Instruction::Or:
            (this->*
             funcOr[llvm::cryptoutils->get_range(NUMBER_OR_SUBST)])(cast<BinaryOperator>(inst));
            ++Or;
            break;
          case Instruction::Xor:
            (this->*
             funcXor[/*llvm::cryptoutils->get_range(NUMBER_XOR_SUBST)*/3])(cast<BinaryOperator>(inst));
            ++Xor;
            break;
          default:
            break;
          }              // End switch
        }                // End isBinaryOp
      }                  // End for basickblock
    }                    // End for Function
  } while (--times > 0); // for times
  return false;
}

// Implementation of a = b - (-c)
void Substitution::addNeg(BinaryOperator *bo) {
  BinaryOperator *op = NULL;

  // Create sub
  if (bo->getOpcode() == Instruction::Add) {
    op = BinaryOperator::CreateNeg(bo->getOperand(1), "", bo);
    op =
        BinaryOperator::Create(Instruction::Sub, bo->getOperand(0), op, "", bo);

    // Check signed wrap
    op->setHasNoSignedWrap(bo->hasNoSignedWrap());
    op->setHasNoUnsignedWrap(bo->hasNoUnsignedWrap());
  } else {
    op = BinaryOperator::CreateFNeg(bo->getOperand(1), "", bo);
    op = BinaryOperator::Create(Instruction::FSub, bo->getOperand(0), op, "",
                                bo);
  }

  bo->replaceAllUsesWith(op);
}

// Implementation of a = -(-b + (-c))
void Substitution::addDoubleNeg(BinaryOperator *bo) {
  BinaryOperator *op, *op2 = NULL;

  if (bo->getOpcode() == Instruction::Add) {
    op = BinaryOperator::CreateNeg(bo->getOperand(0), "", bo);
    op2 = BinaryOperator::CreateNeg(bo->getOperand(1), "", bo);
    op = BinaryOperator::Create(Instruction::Add, op, op2, "", bo);
    op = BinaryOperator::CreateNeg(op, "", bo);

    // Check signed wrap
    op->setHasNoSignedWrap(bo->hasNoSignedWrap());
    op->setHasNoUnsignedWrap(bo->hasNoUnsignedWrap());
  } else {
    op = BinaryOperator::CreateFNeg(bo->getOperand(0), "", bo);
    op2 = BinaryOperator::CreateFNeg(bo->getOperand(1), "", bo);
    op = BinaryOperator::Create(Instruction::FAdd, op, op2, "", bo);
    op = BinaryOperator::CreateFNeg(op, "", bo);
  }

  bo->replaceAllUsesWith(op);
}

// Implementation of  r = rand (); a = b + r; a = a + c; a = a - r
void Substitution::addRand(BinaryOperator *bo) {
  BinaryOperator *op = NULL;

  if (bo->getOpcode() == Instruction::Add) {
    Type *ty = bo->getType();
    ConstantInt *co =
        (ConstantInt *)ConstantInt::get(ty, llvm::cryptoutils->get_uint64_t());
    op =
        BinaryOperator::Create(Instruction::Add, bo->getOperand(0), co, "", bo);
    op =
        BinaryOperator::Create(Instruction::Add, op, bo->getOperand(1), "", bo);
    op = BinaryOperator::Create(Instruction::Sub, op, co, "", bo);

    // Check signed wrap
    op->setHasNoSignedWrap(bo->hasNoSignedWrap());
    op->setHasNoUnsignedWrap(bo->hasNoUnsignedWrap());

    bo->replaceAllUsesWith(op);
  }
  /* else {
      Type *ty = bo->getType();
      ConstantFP *co =
  (ConstantFP*)ConstantFP::get(ty,(float)llvm::cryptoutils->get_uint64_t());
      op = BinaryOperator::Create(Instruction::FAdd,bo->getOperand(0),co,"",bo);
      op = BinaryOperator::Create(Instruction::FAdd,op,bo->getOperand(1),"",bo);
      op = BinaryOperator::Create(Instruction::FSub,op,co,"",bo);
  } */
}

// Implementation of r = rand (); a = b - r; a = a + b; a = a + r
void Substitution::addRand2(BinaryOperator *bo) {
  BinaryOperator *op = NULL;

  if (bo->getOpcode() == Instruction::Add) {
    Type *ty = bo->getType();
    ConstantInt *co =
        (ConstantInt *)ConstantInt::get(ty, llvm::cryptoutils->get_uint64_t());
    op =
        BinaryOperator::Create(Instruction::Sub, bo->getOperand(0), co, "", bo);
    op =
        BinaryOperator::Create(Instruction::Add, op, bo->getOperand(1), "", bo);
    op = BinaryOperator::Create(Instruction::Add, op, co, "", bo);

    // Check signed wrap
    op->setHasNoSignedWrap(bo->hasNoSignedWrap());
    op->setHasNoUnsignedWrap(bo->hasNoUnsignedWrap());

    bo->replaceAllUsesWith(op);
  }
  /* else {
      Type *ty = bo->getType();
      ConstantFP *co =
  (ConstantFP*)ConstantFP::get(ty,(float)llvm::cryptoutils->get_uint64_t());
      op = BinaryOperator::Create(Instruction::FAdd,bo->getOperand(0),co,"",bo);
      op = BinaryOperator::Create(Instruction::FAdd,op,bo->getOperand(1),"",bo);
      op = BinaryOperator::Create(Instruction::FSub,op,co,"",bo);
  } */
}

// using the aha! method to do add
// since we get b XOR c = ((b AND c) * -2) + (b + c)
// we convert to a = b + c = (b XOR c) - ((b AND c) * -2)
//
// a = b XOR c
// x = b AND c
// x = x + x
// a = a + x
void Substitution::addAha(BinaryOperator *bo) {
	BinaryOperator *a, *x = NULL;
	
	if (bo->getOpcode() == Instruction::Add) {
		Value *b = bo->getOperand(0);
		Value *c = bo->getOperand(1);
		
		a = BinaryOperator::Create(Instruction::Xor, b, c, "", bo);
		x = BinaryOperator::Create(Instruction::And, b, c, "", bo);
		x = BinaryOperator::Create(Instruction::Add, x, x, "", bo);
		a = BinaryOperator::Create(Instruction::Add, a, x, "", bo);

		// Check signed wrap
		a->setHasNoSignedWrap(bo->hasNoSignedWrap());
		a->setHasNoUnsignedWrap(bo->hasNoUnsignedWrap());
	}
	
	bo->replaceAllUsesWith(a);
}

// using the aha! method to do sub
// since we get b XOR c = ((b AND c) * -2) + (b + c)
// we convert to a = b + -c = (b XOR -c) - ((b AND -c) * -2)
//
// c = -c
// a = b XOR c
// x = b AND c
// x = x + x
// a = a + x
void Substitution::subAha(BinaryOperator *bo) {
	BinaryOperator *a, *x, *c = NULL;
	
	if (bo->getOpcode() == Instruction::Sub) {
		Type *ty = bo->getType();
		
		ConstantInt *negTwo = (ConstantInt *)ConstantInt::get(ty, -2, true);
		Value *b = bo->getOperand(0);
		c = BinaryOperator::CreateNeg(bo->getOperand(1), "", bo);
		
		
		a = BinaryOperator::Create(Instruction::Xor, b, c, "", bo);
		x = BinaryOperator::Create(Instruction::And, b, c, "", bo);
		x = BinaryOperator::Create(Instruction::Add, x, x, "", bo);
		a = BinaryOperator::Create(Instruction::Add, a, x, "", bo);

		// Check signed wrap
		a->setHasNoSignedWrap(bo->hasNoSignedWrap());
		a->setHasNoUnsignedWrap(bo->hasNoUnsignedWrap());
	}
	
	bo->replaceAllUsesWith(a);
}

// Implementation of a = b + (-c)
void Substitution::subNeg(BinaryOperator *bo) {
  BinaryOperator *op = NULL;

  if (bo->getOpcode() == Instruction::Sub) {
    op = BinaryOperator::CreateNeg(bo->getOperand(1), "", bo);
    op =
        BinaryOperator::Create(Instruction::Add, bo->getOperand(0), op, "", bo);

    // Check signed wrap
    op->setHasNoSignedWrap(bo->hasNoSignedWrap());
    op->setHasNoUnsignedWrap(bo->hasNoUnsignedWrap());
  } else {
    op = BinaryOperator::CreateFNeg(bo->getOperand(1), "", bo);
    op = BinaryOperator::Create(Instruction::FAdd, bo->getOperand(0), op, "",
                                bo);
  }

  bo->replaceAllUsesWith(op);
}

// Implementation of  r = rand (); a = b + r; a = a - c; a = a - r
void Substitution::subRand(BinaryOperator *bo) {
  BinaryOperator *op = NULL;

  if (bo->getOpcode() == Instruction::Sub) {
    Type *ty = bo->getType();
    ConstantInt *co =
        (ConstantInt *)ConstantInt::get(ty, llvm::cryptoutils->get_uint64_t());
    op =
        BinaryOperator::Create(Instruction::Add, bo->getOperand(0), co, "", bo);
    op =
        BinaryOperator::Create(Instruction::Sub, op, bo->getOperand(1), "", bo);
    op = BinaryOperator::Create(Instruction::Sub, op, co, "", bo);

    // Check signed wrap
    op->setHasNoSignedWrap(bo->hasNoSignedWrap());
    op->setHasNoUnsignedWrap(bo->hasNoUnsignedWrap());

    bo->replaceAllUsesWith(op);
  }
  /* else {
      Type *ty = bo->getType();
      ConstantFP *co =
  (ConstantFP*)ConstantFP::get(ty,(float)llvm::cryptoutils->get_uint64_t());
      op = BinaryOperator::Create(Instruction::FAdd,bo->getOperand(0),co,"",bo);
      op = BinaryOperator::Create(Instruction::FSub,op,bo->getOperand(1),"",bo);
      op = BinaryOperator::Create(Instruction::FSub,op,co,"",bo);
  } */
}

// Implementation of  r = rand (); a = b - r; a = a - c; a = a + r
void Substitution::subRand2(BinaryOperator *bo) {
  BinaryOperator *op = NULL;

  if (bo->getOpcode() == Instruction::Sub) {
    Type *ty = bo->getType();
    ConstantInt *co =
        (ConstantInt *)ConstantInt::get(ty, llvm::cryptoutils->get_uint64_t());
    op =
        BinaryOperator::Create(Instruction::Sub, bo->getOperand(0), co, "", bo);
    op =
        BinaryOperator::Create(Instruction::Sub, op, bo->getOperand(1), "", bo);
    op = BinaryOperator::Create(Instruction::Add, op, co, "", bo);

    // Check signed wrap
    op->setHasNoSignedWrap(bo->hasNoSignedWrap());
    op->setHasNoUnsignedWrap(bo->hasNoUnsignedWrap());

    bo->replaceAllUsesWith(op);
  }
  /* else {
      Type *ty = bo->getType();
      ConstantFP *co =
  (ConstantFP*)ConstantFP::get(ty,(float)llvm::cryptoutils->get_uint64_t());
      op = BinaryOperator::Create(Instruction::FSub,bo->getOperand(0),co,"",bo);
      op = BinaryOperator::Create(Instruction::FSub,op,bo->getOperand(1),"",bo);
      op = BinaryOperator::Create(Instruction::FAdd,op,co,"",bo);
  } */
}

// Implementation of a = b & c => a = (b^~c)& b
void Substitution::andSubstitution(BinaryOperator *bo) {
  BinaryOperator *op = NULL;

  // Create NOT on second operand => ~c
  op = BinaryOperator::CreateNot(bo->getOperand(1), "", bo);

  // Create XOR => (b^~c)
  BinaryOperator *op1 =
      BinaryOperator::Create(Instruction::Xor, bo->getOperand(0), op, "", bo);

  // Create AND => (b^~c) & b
  op = BinaryOperator::Create(Instruction::And, op1, bo->getOperand(0), "", bo);
  bo->replaceAllUsesWith(op);
}

// Implementation of a = a && b <=> !(!a | !b) && (r | !r)
void Substitution::andSubstitutionRand(BinaryOperator *bo) {
  // Copy of the BinaryOperator type to create the random number with the
  // same type of the operands
  Type *ty = bo->getType();

  // r (Random number)
  ConstantInt *co =
      (ConstantInt *)ConstantInt::get(ty, llvm::cryptoutils->get_uint64_t());

  // !a
  BinaryOperator *op = BinaryOperator::CreateNot(bo->getOperand(0), "", bo);

  // !b
  BinaryOperator *op1 = BinaryOperator::CreateNot(bo->getOperand(1), "", bo);

  // !r
  BinaryOperator *opr = BinaryOperator::CreateNot(co, "", bo);

  // (!a | !b)
  BinaryOperator *opa =
      BinaryOperator::Create(Instruction::Or, op, op1, "", bo);

  // (r | !r)
  opr = BinaryOperator::Create(Instruction::Or, co, opr, "", bo);

  // !(!a | !b)
  op = BinaryOperator::CreateNot(opa, "", bo);

  // !(!a | !b) && (r | !r)
  op = BinaryOperator::Create(Instruction::And, op, opr, "", bo);

  // We replace all the old AND operators with the new one transformed
  bo->replaceAllUsesWith(op);
}

// b AND c = (b AND c) OR (r AND !r)
// 		=  (b OR r) AND (c OR r) AND (b OR !r) AND (c or !r)

// r = rand()
// s = !r
// w = b OR r
// x = c OR r
// y = b OR s
// z = c OR s
// a = w AND x
// a = a AND y
// a = a AND z
void Substitution::andSubstitutionRandExp(BinaryOperator *bo) {
	BinaryOperator *a, *s, *w, *x, *y, *z = NULL;
	
	Type *ty = bo->getType();
	auto r_val = llvm::cryptoutils->get_uint64_t();
	ConstantInt *r = (ConstantInt *)ConstantInt::get(ty, r_val);
	s = BinaryOperator::CreateNot(r, "", bo);
	Value *b = bo->getOperand(0);
	Value *c = bo->getOperand(1);
	
	// w = b OR r 
	w = BinaryOperator::Create(Instruction::Or, b, r, "", bo);
	
	// x = c OR r
	x = BinaryOperator::Create(Instruction::Or, c, r, "", bo);
	
	// y = b OR s
	y = BinaryOperator::Create(Instruction::Or, b, s, "", bo);
	
	// z = c OR s
	z = BinaryOperator::Create(Instruction::Or, c, s, "", bo);
	
	// a = w AND x
	a = BinaryOperator::Create(Instruction::And, w, x, "", bo);
	
	// a = a AND y
	a = BinaryOperator::Create(Instruction::And, a, y, "", bo);
	
	// a = a AND z
	a = BinaryOperator::Create(Instruction::And, a, z, "", bo);

	// Check signed wrap
	a->setHasNoSignedWrap(bo->hasNoSignedWrap());
	a->setHasNoUnsignedWrap(bo->hasNoUnsignedWrap());

	bo->replaceAllUsesWith(a);
}

// Implementation of a = b | c => a = (b & c) | (b ^ c)
void Substitution::orSubstitutionRand(BinaryOperator *bo) {

  Type *ty = bo->getType();
  ConstantInt *co =
      (ConstantInt *)ConstantInt::get(ty, llvm::cryptoutils->get_uint64_t());

  // !a
  BinaryOperator *op = BinaryOperator::CreateNot(bo->getOperand(0), "", bo);

  // !b
  BinaryOperator *op1 = BinaryOperator::CreateNot(bo->getOperand(1), "", bo);

  // !r
  BinaryOperator *op2 = BinaryOperator::CreateNot(co, "", bo);

  // !a && r
  BinaryOperator *op3 =
      BinaryOperator::Create(Instruction::And, op, co, "", bo);

  // a && !r
  BinaryOperator *op4 =
      BinaryOperator::Create(Instruction::And, bo->getOperand(0), op2, "", bo);

  // !b && r
  BinaryOperator *op5 =
      BinaryOperator::Create(Instruction::And, op1, co, "", bo);

  // b && !r
  BinaryOperator *op6 =
      BinaryOperator::Create(Instruction::And, bo->getOperand(1), op2, "", bo);

  // (!a && r) || (a && !r)
  op3 = BinaryOperator::Create(Instruction::Or, op3, op4, "", bo);

  // (!b && r) ||(b && !r)
  op4 = BinaryOperator::Create(Instruction::Or, op5, op6, "", bo);

  // (!a && r) || (a && !r) ^ (!b && r) ||(b && !r)
  op5 = BinaryOperator::Create(Instruction::Xor, op3, op4, "", bo);

  // !a || !b
  op3 = BinaryOperator::Create(Instruction::Or, op, op1, "", bo);

  // !(!a || !b)
  op3 = BinaryOperator::CreateNot(op3, "", bo);

  // r || !r
  op4 = BinaryOperator::Create(Instruction::Or, co, op2, "", bo);

  // !(!a || !b) && (r || !r)
  op4 = BinaryOperator::Create(Instruction::And, op3, op4, "", bo);

  // [(!a && r) || (a && !r) ^ (!b && r) ||(b && !r) ] || [!(!a || !b) && (r ||
  // !r)]
  op = BinaryOperator::Create(Instruction::Or, op5, op4, "", bo);
  bo->replaceAllUsesWith(op);
}

void Substitution::orSubstitution(BinaryOperator *bo) {
  BinaryOperator *op = NULL;

  // Creating first operand (b & c)
  op = BinaryOperator::Create(Instruction::And, bo->getOperand(0),
                              bo->getOperand(1), "", bo);

  // Creating second operand (b ^ c)
  BinaryOperator *op1 = BinaryOperator::Create(
      Instruction::Xor, bo->getOperand(0), bo->getOperand(1), "", bo);

  // final op
  op = BinaryOperator::Create(Instruction::Or, op, op1, "", bo);
  bo->replaceAllUsesWith(op);
}

// Implementation of a = a ~ b => a = (!a && b) || (a && !b)
void Substitution::xorSubstitution(BinaryOperator *bo) {
  BinaryOperator *op = NULL;

  // Create NOT on first operand
  op = BinaryOperator::CreateNot(bo->getOperand(0), "", bo); // !a

  // Create AND
  op = BinaryOperator::Create(Instruction::And, bo->getOperand(1), op, "",
                              bo); // !a && b

  // Create NOT on second operand
  BinaryOperator *op1 =
      BinaryOperator::CreateNot(bo->getOperand(1), "", bo); // !b

  // Create AND
  op1 = BinaryOperator::Create(Instruction::And, bo->getOperand(0), op1, "",
                               bo); // a && !b

  // Create OR
  op = BinaryOperator::Create(Instruction::Or, op, op1, "",
                              bo); // (!a && b) || (a && !b)
  bo->replaceAllUsesWith(op);
}

// implementation of a = a ^ b <=> (a ^ r) ^ (b ^ r) <=> (!a && r || a && !r) ^
// (!b && r || b && !r)
// note : r is a random number
void Substitution::xorSubstitutionRand(BinaryOperator *bo) {
  BinaryOperator *op = NULL;

  Type *ty = bo->getType();
  ConstantInt *co =
      (ConstantInt *)ConstantInt::get(ty, llvm::cryptoutils->get_uint64_t());

  // !a
  op = BinaryOperator::CreateNot(bo->getOperand(0), "", bo);

  // !a && r
  op = BinaryOperator::Create(Instruction::And, co, op, "", bo);

  // !r
  BinaryOperator *opr = BinaryOperator::CreateNot(co, "", bo);

  // a && !r
  BinaryOperator *op1 =
      BinaryOperator::Create(Instruction::And, bo->getOperand(0), opr, "", bo);

  // !b
  BinaryOperator *op2 = BinaryOperator::CreateNot(bo->getOperand(1), "", bo);

  // !b && r
  op2 = BinaryOperator::Create(Instruction::And, op2, co, "", bo);

  // b && !r
  BinaryOperator *op3 =
      BinaryOperator::Create(Instruction::And, bo->getOperand(1), opr, "", bo);

  // (!a && r) || (a && !r)
  op = BinaryOperator::Create(Instruction::Or, op, op1, "", bo);

  // (!b && r) || (b && !r)
  op1 = BinaryOperator::Create(Instruction::Or, op2, op3, "", bo);

  // (!a && r) || (a && !r) ^ (!b && r) || (b && !r)
  op = BinaryOperator::Create(Instruction::Xor, op, op1, "", bo);
  bo->replaceAllUsesWith(op);
}

// XOR substitution using the method found in the Aha! superoptimizer
// https://yurichev.com/blog/llvm/ 
// a = b XOR c = ((b AND c) * -2) + b + c
//
// a = b AND c;
// a = a * -2;
// a = a + b;
// a = a + c; 
void Substitution::xorSubstitutionAha(BinaryOperator *bo) {
	BinaryOperator *a = NULL;
	Type *ty = bo->getType();
	
	ConstantInt *negTwo = (ConstantInt *)ConstantInt::get(ty, -2, true);
	Value *b = bo->getOperand(0);
	Value *c = bo->getOperand(1);
	
	// a = b AND c;
	a = BinaryOperator::Create(Instruction::And, b, c, "", bo);
	
	// a = a * -2
	a = BinaryOperator::Create(Instruction::Mul, a, negTwo, "", bo);
	
	// a = a + b
	a = BinaryOperator::Create(Instruction::Add, a, b, "", bo);
	
	// a = a + c
	a = BinaryOperator::Create(Instruction::Add, a, c, "", bo);

	// Check signed wrap
	a->setHasNoSignedWrap(bo->hasNoSignedWrap());
	a->setHasNoUnsignedWrap(bo->hasNoUnsignedWrap());

	bo->replaceAllUsesWith(a);
}

// Optimised aha! substitution.
// Does not use expensive multiply operation
// a = b XOR c = b + c - 2 * (b AND c) = b + c - ((b AND c) + (b AND c))
// However we still keep the non-opt version as the mul might be effective at
// obfuscating as well
//
// x = b AND c
// x = x + x
// a = b + c
// a = a - x
void Substitution::xorSubstitutionAhaOpt(BinaryOperator *bo) {
	BinaryOperator *a, *x = NULL;
	
	Value *b = bo->getOperand(0);
	Value *c = bo->getOperand(1);
	
	// x = b AND c;
	x = BinaryOperator::Create(Instruction::And, b, c, "", bo);
	
	// x = x + x
	x = BinaryOperator::Create(Instruction::Add, x, x, "", bo);
	
	// a = b + c
	a = BinaryOperator::Create(Instruction::Add, b, c, "", bo);
	
	// a = a - x
	a = BinaryOperator::Create(Instruction::Sub, a, x, "", bo);

	// Check signed wrap
	a->setHasNoSignedWrap(bo->hasNoSignedWrap());
	a->setHasNoUnsignedWrap(bo->hasNoUnsignedWrap());

	bo->replaceAllUsesWith(a);
}

