#ifndef __XOR_STRING_ENCRYPTION_H__
#define __XOR_STRING_ENCRYPTION_H__

#include <stdint.h>
#include "AbstractStringEncryptionPass.h"

namespace llvm {
	class Module;
	class LoadInst;
}

namespace llvm {
	class XorStringEncryption : public AbstractStringEncryptionPass {
		public:
			static char ID;
		
			XorStringEncryption(uint32_t KeySize = 80);
			XorStringEncryption(const std::string& Key);

		protected:
			/** encryption method
			 * \param ClearString string to encrypt
			 * \return encrypted string */
			virtual std::string stringEncryption(const std::string& ClearString);
			/** Decryption method, called every time a encrypted string is used.
			 * Should generate the llvm IR to decrypt the string
			 * \param M module
			 * \param LoadEncryptedString load instruction (load pointer to the encrypted string)
			 * \return value that will replace the load to the encrypted string */		
			virtual llvm::Value* stringDecryption(llvm::Module &M, llvm::LoadInst* LoadEncryptedString, const uint64_t Size);
		
		private:
			/** generate random key */
			std::string generateRandomKey(uint32_t Size);
		
		private:
			std::string _key;			
	};
}

#endif
