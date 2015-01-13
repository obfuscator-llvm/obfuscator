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
	class StoreInst;
	class CallInst;
	class InvokeInst;
	class ReturnInst;
	class GetElementPtrInst;
	class Instruction;
}

namespace llvm {
	class AbstractStringEncryptionPass : public ModulePass {
		public:
			AbstractStringEncryptionPass(char ID);

			virtual bool runOnModule(Module &M);

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
			virtual Value* stringDecryption(Module &M, Value* EncryptedString, const uint64_t Size, Instruction* Parent) = 0;

		private:
			void handleLoad(Module &M, LoadInst* Load);
			void handleStore(Module &M, StoreInst* store);
			void handleCall(Module &M, CallInst* Call);
			void handleInvoke(Module &M, InvokeInst* Invoke);
			void handleReturn(Module &M, ReturnInst* Ret);
			void handleGEP(Module &M, GetElementPtrInst* Gep);
			
			void checkStringsCanBeEncrypted(Module &M, std::vector<GlobalVariable*>& StringGlobalVars);
			
			bool encryptString(Module &M, std::vector<GlobalVariable*>& StringGlobalVars, std::vector<GlobalVariable*>& StringGlobalVarsToDelete);
			void insertDecryptionCode(Module &M);
			
			std::string getGlobalStringValue(GlobalVariable* GV);
			
		private:
			std::map<std::string, GlobalVariable*> StringMapGlobalVars;
			std::vector<Instruction*> InstructionToDel;
			
			uint64_t encryptedStringCounter;
	};
}

#endif
