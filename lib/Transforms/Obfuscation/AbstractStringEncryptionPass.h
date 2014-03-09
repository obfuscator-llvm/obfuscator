#ifndef __ABSTRACT_STRING_ENCRYPTION_PASS_H__
#define __ABSTRACT_STRING_ENCRYPTION_PASS_H__

#include <stdint.h>
#include <llvm/Pass.h>

namespace llvm {
	class Value;
	class Module;
	class LoadInst;
}

namespace llvm {
	class AbstractStringEncryptionPass : public llvm::ModulePass {
		public:
			AbstractStringEncryptionPass(char ID);

			virtual bool runOnModule(llvm::Module &M);

		protected:
			/** encryption method
			 * \param ClearString string to encrypt
			 * \return encrypted string */
			virtual std::string stringEncryption(const std::string& ClearString) = 0;
			/** Decryption method, called every time a encrypted string is used.
			 * Should generate the llvm IR to decrypt the string
			 * \param M module
			 * \param LoadEncryptedString load instruction (load pointer to the encrypted string)
			 * \return value that will replace the load to the encrypted string */		
			virtual llvm::Value* stringDecryption(llvm::Module &M, llvm::LoadInst* LoadEncryptedString, const uint64_t Size) = 0;			
	};
}

#endif
