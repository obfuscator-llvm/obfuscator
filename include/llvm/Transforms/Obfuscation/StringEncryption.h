#ifndef __STRING_ENCRYPTION_H__
#define __STRING_ENCRYPTION_H__

// LLVM include
#include "llvm/Pass.h"

using namespace llvm;

namespace llvm {
	Pass* createXorStringEncryption();
}

#endif
