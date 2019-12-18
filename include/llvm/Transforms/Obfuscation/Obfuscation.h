#ifndef _OBFUSCATION_H_
#define _OBFUSCATION_H_

#include "llvm/Transforms/Obfuscation/Flattening.h"
#include "llvm/Transforms/Obfuscation/StringObfuscation.h"
#include "llvm/Transforms/Obfuscation/Substitution.h"
#include "llvm/Transforms/Obfuscation/BogusControlFlow.h"
#include "llvm/Transforms/Obfuscation/Split.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include "llvm/Transforms/Utils/ModuleUtils.h"
#include "llvm/Transforms/Obfuscation/CryptoUtils.h"
#include "llvm/Transforms/Scalar.h"
#include "llvm/Transforms/Utils.h"

using namespace std;
using namespace llvm;

namespace llvm {

ModulePass* createObfuscationPass();
void initializeObfuscationPass(PassRegistry &Registry);

}

#endif /* _OBFUSCATION_H_ */
