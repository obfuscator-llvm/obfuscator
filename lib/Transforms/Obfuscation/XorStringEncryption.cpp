#include <stdint.h>
#include "llvm/Pass.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/Support/raw_ostream.h"

#include "llvm/Transforms/Obfuscation/StringEncryption.h"
#include "XorStringEncryption.h"

#define DEBUG_TYPE "xorstringencryption"

using namespace llvm;

XorStringEncryption::XorStringEncryption(uint32_t KeySize) : AbstractStringEncryptionPass(ID){
    _key = generateRandomKey(KeySize);
}

XorStringEncryption::XorStringEncryption(const std::string& Key) : AbstractStringEncryptionPass(ID){
    _key = Key;
}

std::string XorStringEncryption::generateRandomKey(uint32_t Size){
    std::string allowedChar = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789,.-#'?!";
    std::string key;
    for(uint32_t i = 0; i < Size; i++){
        int n = rand() % allowedChar.size();
        key += allowedChar[n];
    }
    //errs() << key << "\n";
    return key;
}

std::string XorStringEncryption::stringEncryption(const std::string& str_) {
    std::string encstr = str_;
    for(uint32_t i = 0; i < encstr.size(); i++){
        encstr[i] ^= _key[i%_key.size()];
    }
    return encstr;
}

llvm::Value* XorStringEncryption::stringDecryption(llvm::Module &M, llvm::Value* encryptedString, const uint64_t Size, llvm::Instruction* parent) {
    //allocate a new string
    AllocaInst* alloca = new AllocaInst(IntegerType::getInt8Ty(M.getContext()), ConstantInt::get(IntegerType::getInt64Ty(M.getContext()), Size), "", parent);

    for(uint64_t i = 0; i < Size; i++){                               
        std::vector<Value*> idxlist;        
        idxlist.push_back(ConstantInt::get(IntegerType::getInt64Ty(M.getContext()), i));
        GetElementPtrInst* destPtr = GetElementPtrInst::CreateInBounds(alloca, ArrayRef<Value*>(idxlist), "", parent);
        
        if(not dyn_cast<LoadInst>(encryptedString)){
            idxlist.clear();
            idxlist.push_back(ConstantInt::get(IntegerType::getInt64Ty(M.getContext()), 0));
            idxlist.push_back(ConstantInt::get(IntegerType::getInt64Ty(M.getContext()), i));
            //convert [NB x i8]* to i8  *...
            //%cast = getelementptr [NB x i8]* @.str, i64 0, i64 i
        }
        
        GetElementPtrInst* srcPtr = GetElementPtrInst::Create(encryptedString, ArrayRef<Value*>(idxlist), "", parent);
        LoadInst* srcload = new LoadInst(srcPtr, "", false, 8, parent);

        BinaryOperator* clearChar = BinaryOperator::CreateXor(srcload, ConstantInt::get(IntegerType::getInt8Ty(M.getContext()), _key[i%_key.size()]), "", parent);
        new StoreInst(clearChar, destPtr, false, 8, parent);                                                        
    }
    return alloca; 
}

char XorStringEncryption::ID = 0;
static RegisterPass<XorStringEncryption> X("xorscrypt", "Xor String Encryption Pass");

Pass *llvm::createXorStringEncryption() {
    return new XorStringEncryption();
}
