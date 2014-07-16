#ifndef __ABSTRACT_STRING_ENCRYPTION_PASS_H__
#define __ABSTRACT_STRING_ENCRYPTION_PASS_H__

#include <stdint.h>
#include <map>
#include <llvm/Pass.h>

namespace llvm {
	class Value;
	class GlobalVariable;
	class Module;
	class LoadInst;
	class CallInst;
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
			 * \param EncryptedString encrypted string value
			 * \param Size size of encrypted string
			 * \param Parent parent instruction
			 * \return value that will replace the load to the encrypted string */		
			virtual llvm::Value* stringDecryption(llvm::Module &M, llvm::Value* EncryptedString, const uint64_t Size, llvm::Instruction* Parent) = 0;

		private:
			void handleLoad(llvm::Module &M, llvm::LoadInst* Load);
			void handleCall(llvm::Module &M, llvm::CallInst* Call);
			
		private:
			std::map<std::string, GlobalVariable*> StringMapGlobalVars;
	};
}

#endif
