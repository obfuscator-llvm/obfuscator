#include <stdio.h>
#include <iostream>
#include <sstream>
#include "llvm/Pass.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include "llvm/Support/raw_ostream.h"

#include "AbstractStringEncryptionPass.h"

using namespace llvm;

AbstractStringEncryptionPass::AbstractStringEncryptionPass(char ID) : ModulePass(ID) {
    
}

bool AbstractStringEncryptionPass::runOnModule(Module &M) {
    bool changed = false;

    uint64_t encryptedStringCounter = 0;
    
    StringMapGlobalVars.clear();
    std::vector<GlobalVariable*> StringGlobalVars;
    std::vector<GlobalVariable*> StringGlobalVarsToDelete;
    
    //-----------------
    //get strings
    for (Module::global_iterator I = M.global_begin(), E = M.global_end(); I != E; ++I) {
        GlobalVariable* GV = I;
        if(GV->isConstant()){
            Constant* c = GV->getInitializer();
            ConstantDataSequential *cds = dyn_cast<ConstantDataSequential>(c);
            if(cds){
                StringGlobalVars.push_back(I);
            }
        }
    }
    
    //-----------------
    //encrypt strings
    for(std::vector<GlobalVariable*>::iterator it = StringGlobalVars.begin(); it != StringGlobalVars.end(); ++it){
        GlobalVariable* GV = *it;
        ConstantDataSequential *cds = dyn_cast<ConstantDataSequential>(GV->getInitializer());
        std::string clearstr = "";
        if(cds->isString()){
            clearstr = cds->getAsString();
        }else{
            if(cds->isCString ()){
                clearstr = cds->getAsCString();
            }else{
                errs() << "Can't get string value from " << GV->getName() << " SKIP ENCRYPTION!\n";
                continue;
            }
        }
        
        //encrypt current string
        std::string encryptedString = stringEncryption(clearstr);
        //std::string encryptedString = clearstr;
        
        //create new global string with the encrypted string
        //@todo check if name does not exist in module
        std::ostringstream oss;
        oss << ".encstr" << encryptedStringCounter;
        encryptedStringCounter++;
        Constant *cryptedStr = ConstantDataArray::getString(M.getContext(), encryptedString, true);
        GlobalVariable* gCryptedStr = new GlobalVariable(M, cryptedStr->getType(), true, GlobalValue::ExternalLinkage, cryptedStr, oss.str());
        StringMapGlobalVars[oss.str()] = gCryptedStr;
        
        //replace use of clear string with encrypted string
        GV->replaceAllUsesWith(gCryptedStr);
        //need to remove clear text global
        StringGlobalVarsToDelete.push_back(GV);
        changed = true;
    }
    
    //----------------------------------------------
    //insert decryption code where string is used
    
    //iterate every instruction of the module
    for (Module::iterator I = M.begin(), E = M.end(); I != E; ++I) {
        for (Function::iterator bb = I->begin(), e = I->end(); bb != e; ++bb) {
            for (BasicBlock::iterator inst = bb->begin(); inst != bb->end(); ++inst) {
                //check if instruction is call
                CallInst *call = dyn_cast<CallInst>(inst);
                if (call != 0) {
                    handleCall(M, call);
                    continue;
                }
                
                //check if instruction is load
                LoadInst *load = dyn_cast<LoadInst>(inst);
                if(load != 0){
                    handleLoad(M, load);
                    continue;
                }
                
            }
        }
    }
    
    //remove all clear text global variable
    for(std::vector<GlobalVariable*>::iterator it = StringGlobalVarsToDelete.begin(); it != StringGlobalVarsToDelete.end(); ++it){
        GlobalVariable* GV = *it;
        GV->eraseFromParent();
    }
    
    //M.dump();
    return changed;
}

void AbstractStringEncryptionPass::handleLoad(Module &M, LoadInst* Load) {
   //check if loaded pointer is global
    Value* ptrOp = Load->getPointerOperand();
    GlobalVariable *GV = dyn_cast<GlobalVariable>(ptrOp);
    if (GV == 0)
        return;
    
    //check if loaded pointer is constant
    Constant* c = GV->getInitializer();
    if(c == 0)
        return;
    ConstantExpr *constExpr = dyn_cast<ConstantExpr>(c);
    if(constExpr == 0)
        return;
    
    if (constExpr->getOpcode() == Instruction::GetElementPtr){
        //get GEP instruction
        GetElementPtrInst* gepInst = dyn_cast<GetElementPtrInst>(constExpr->getAsInstruction());
        if(gepInst == 0)
            return;
                
        //check if the string is encrypted
        StringRef gepOpName = gepInst->getPointerOperand()->getName();
        std::map<std::string, GlobalVariable*>::iterator it = StringMapGlobalVars.find(gepOpName.str());
        if(it != StringMapGlobalVars.end()){
            //get size of string
            ConstantDataSequential *cds = dyn_cast<ConstantDataSequential>(it->second->getInitializer());
            uint64_t size = cds->getNumElements();
            //generate IR to decrypt string
            LoadInst* newload = new LoadInst(Load->getPointerOperand(), "", false, 8, Load);
            Value* decryptedStr = stringDecryption(M, newload, size, Load);
            //replace current load with the decryption code
            Load->replaceAllUsesWith(decryptedStr);
        }
    } 
}

void AbstractStringEncryptionPass::handleCall(llvm::Module &M, llvm::CallInst* Call) {
    for(unsigned i = 0; i < Call->getNumArgOperands(); i++){
                                
        llvm::ConstantExpr *constExpr = llvm::dyn_cast<llvm::ConstantExpr>(Call->getArgOperand(i));
        //not a constant expr
        if (constExpr == 0)
            continue;
        //not a gep
        if (constExpr->getOpcode() != llvm::Instruction::GetElementPtr)
            continue;

        llvm::GetElementPtrInst* gepInst = dyn_cast<llvm::GetElementPtrInst>(constExpr->getAsInstruction());
        if(gepInst == 0)
            continue;
        
        //load encrypted string
        StringRef gepOpName = gepInst->getPointerOperand()->getName();
        std::map<std::string, GlobalVariable*>::iterator it = StringMapGlobalVars.find(gepOpName.str());
        if(it != StringMapGlobalVars.end()){
            //get size of string
            ConstantDataSequential *cds = dyn_cast<ConstantDataSequential>(it->second->getInitializer());
            uint64_t size = cds->getNumElements();
             //generate IR to decrypt string
            llvm::Value* decryptedStr = stringDecryption(M, it->second, size, Call);
            Call->setArgOperand(i, decryptedStr);
        }
    }    
}
