#include <stdio.h>
#include <iostream>
#include <fstream>
#include <sstream>
#include "llvm/Pass.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/GlobalVariable.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/Process.h"
#include "llvm/Support/Path.h"

#include "AbstractStringEncryptionPass.h"

using namespace llvm;

AbstractStringEncryptionPass::AbstractStringEncryptionPass(char ID) : ModulePass(ID), encryptedStringCounter(0) {

}

bool AbstractStringEncryptionPass::runOnModule(Module &M) {
	bool changed = false;

	StringMapGlobalVars.clear();
	std::vector<GlobalVariable*> StringGlobalVars;
	std::vector<GlobalVariable*> StringGlobalVarsToDelete;

	//-----------------
	//get list of strings
	for (Module::global_iterator I = M.global_begin(), E = M.global_end(); I != E; ++I) {
		GlobalVariable* GV = I;
		if(GV->isConstant()){
			Constant* c = GV->getInitializer();
			if(c){
				ConstantDataSequential *cds = dyn_cast<ConstantDataSequential>(c);
				if(cds){
					if(cds->isString()){
						StringGlobalVars.push_back(I);
					}else{
						if(cds->isCString()){
							StringGlobalVars.push_back(I);
						}else{
							//not a string skip it
							//errs() << "WARNING : Can't get string value from " << GV->getName() << " SKIP ENCRYPTION!\n";
						}
					}
				}
			}
		}
	}

	
	//-----------------
	//remove all strings that cannot be encrypted
	checkStringsCanBeEncrypted(M, StringGlobalVars);

	//-----------------
	//encrypt strings
	changed = encryptString(M, StringGlobalVars, StringGlobalVarsToDelete);
	if(changed == false)
		return changed;
	
	//----------------------------------------------
	//insert decryption code where string is used
	insertDecryptionCode(M);

	
	//remove all clear text global variable
	for(std::vector<GlobalVariable*>::iterator it = StringGlobalVarsToDelete.begin(); it != StringGlobalVarsToDelete.end(); ++it){
		GlobalVariable* GV = *it;
		GV->eraseFromParent();
	}
	//remove all dead instructions
	for(std::vector<Instruction*>::iterator it = InstructionToDel.begin(); it != InstructionToDel.end(); ++it){
		Instruction* inst = *it;
		inst->eraseFromParent();
	}
	
	//M.dump();
	return changed;
}

std::string AbstractStringEncryptionPass::getGlobalStringValue(GlobalVariable* GV) {
	std::string str = "";
	ConstantDataSequential *cds = dyn_cast<ConstantDataSequential>(GV->getInitializer());
	if(cds){
		if(cds->isString()){
			str = cds->getAsString();
		}else{
			if(cds->isCString ()){
				str = cds->getAsCString();
			}
		}
	}
	return str;
}

void AbstractStringEncryptionPass::checkStringsCanBeEncrypted(Module &M, std::vector<GlobalVariable*>& StringGlobalVars) {
	//do not encrypt string that are directly in return instruction
	// example : const char* fun(){ return "clear-text"; }
	// this can't be encrypted since we have to do some allocation to decrypt the string ...
	for (Module::iterator I = M.begin(), E = M.end(); I != E; ++I) {
		for (Function::iterator bb = I->begin(), e = I->end(); bb != e; ++bb) {
			for (BasicBlock::iterator inst = bb->begin(); inst != bb->end(); ++inst) {
				//check every return instruction
				ReturnInst *ret = dyn_cast<ReturnInst>(inst);
				if (ret == 0)
					continue;
				
				//get the return value
				Value* retval = ret->getReturnValue();
				if(retval == 0)
					continue;
				
				//check if the return value is a load instruction
				LoadInst* loadInst = dyn_cast<LoadInst>(retval);
				if(loadInst){
					Value* ptrOp = loadInst->getPointerOperand();
					GlobalVariable *GV = dyn_cast<GlobalVariable>(ptrOp);
					if (GV == 0){
						ConstantExpr *constExpr = dyn_cast<ConstantExpr>(ptrOp);
						if(constExpr == 0)
							continue;
						
						GetElementPtrInst* gepInst = dyn_cast<GetElementPtrInst>(constExpr->getAsInstruction());
						if (gepInst == 0)
							continue;
							
						GlobalVariable* GV = dyn_cast<GlobalVariable>(gepInst->getPointerOperand());
						if(GV){
							// handle load i8* getelementptr inbounds ([X x i8]* @string, i32 0, i64 x), align 1
							ConstantDataSequential *cds = dyn_cast<ConstantDataSequential>(GV->getInitializer());
							if(cds){
								errs() << "WARNING : " << getGlobalStringValue(GV) << " cant't be ecnrypted (const char* directly used in return instruction)!\n";
								std::vector<GlobalVariable*>::iterator it = std::find(StringGlobalVars.begin(), StringGlobalVars.end(), GV);
								if(it != StringGlobalVars.end()){
									StringGlobalVars.erase(it);
								}
							}else{
								// handle load i8** getelementptr inbounds ([X x i8*]* @string_array, i32 0, i64 x), align 8					
								if (ConstantArray* array = dyn_cast<ConstantArray>(GV->getInitializer())) {
									Constant* c = array->getAggregateElement(dyn_cast<ConstantInt>(gepInst->getOperand(2))->getZExtValue());
									ConstantExpr* ce = dyn_cast<ConstantExpr>(c);
									if(ce){
										GetElementPtrInst *gepElementFromArray = dyn_cast<GetElementPtrInst>(ce->getAsInstruction());
										if(gepElementFromArray){
											GlobalVariable* GV = dyn_cast<GlobalVariable>(gepElementFromArray->getPointerOperand());
											if(GV){
												ConstantDataSequential *cds = dyn_cast<ConstantDataSequential>(GV->getInitializer());
												if(cds){
													errs() << "WARNING : " << getGlobalStringValue(GV) << " cant't be ecnrypted (const char* directly used in return instruction)!\n";
													std::vector<GlobalVariable*>::iterator it = std::find(StringGlobalVars.begin(), StringGlobalVars.end(), GV);
													if(it != StringGlobalVars.end()){
														StringGlobalVars.erase(it);
													}
												}
											}
										}
									}
								}
							}
						}
						continue;
					}else{
						//check if loaded pointer is constant
						Constant* c = GV->getInitializer();
						if(c == 0)
							continue;

						ConstantExpr *constExpr = dyn_cast<ConstantExpr>(c);
						if(constExpr == 0)
							continue;

						if (constExpr->getOpcode() == Instruction::GetElementPtr){
							//get GEP instruction
							GetElementPtrInst* gepInst = dyn_cast<GetElementPtrInst>(constExpr->getAsInstruction());
							if(gepInst == 0)
								continue;

							GlobalVariable* GV = dyn_cast<GlobalVariable>(gepInst->getPointerOperand());
							if(GV){
								ConstantDataSequential *cds = dyn_cast<ConstantDataSequential>(GV->getInitializer());
								if(cds){
									errs() << "WARNING : " << getGlobalStringValue(GV) << " cant't be ecnrypted (const char* directly used in return instruction)!\n";
									std::vector<GlobalVariable*>::iterator it = std::find(StringGlobalVars.begin(), StringGlobalVars.end(), GV);
									if(it != StringGlobalVars.end()){
										StringGlobalVars.erase(it);
									}
								}
							}
						}	
					}
				}else{
					//instruction is not a load, check for global variable...
					ConstantExpr *constExpr = dyn_cast<ConstantExpr>(retval);
					if (constExpr == 0)
						continue;
					if (constExpr->getOpcode() != Instruction::GetElementPtr)
						continue;
					GetElementPtrInst* gepInst = dyn_cast<GetElementPtrInst>(constExpr->getAsInstruction());
					if(gepInst == 0)
						continue;
					
					GlobalVariable* GV = dyn_cast<GlobalVariable>(gepInst->getPointerOperand());
					if(GV){
						ConstantDataSequential *cds = dyn_cast<ConstantDataSequential>(GV->getInitializer());
						if(cds){
							errs() << "WARNING : " << getGlobalStringValue(GV) << " cant't be ecnrypted (const char* directly used in return instruction)!\n";
							std::vector<GlobalVariable*>::iterator it = std::find(StringGlobalVars.begin(), StringGlobalVars.end(), GV);
							if(it != StringGlobalVars.end()){
								StringGlobalVars.erase(it);
							}
						}
					}
				}
			}
		}
	}

	// I don't know how to handle this case :	 
	//	int main(){
	//		const char *test[] = { "item0", "item1", "item2", "item3", "item4"};
	//		printf("%s\n", test[3]);
	//		return 0;
	//	}
	// do not encrypt those strings ...
	// @TODO : find a way to handle this case and remove this code
	for (Module::iterator I = M.begin(), E = M.end(); I != E; ++I) {
		for (Function::iterator bb = I->begin(), e = I->end(); bb != e; ++bb) {
			for (BasicBlock::iterator inst = bb->begin(); inst != bb->end(); ++inst) {
				if(llvm::MemCpyInst* memcpyInst = dyn_cast<llvm::MemCpyInst>(inst)){
					if (llvm::ConstantExpr *constExpr = llvm::dyn_cast<llvm::ConstantExpr>(memcpyInst->getArgOperand(1))){
						if(llvm::CastInst* castInst = dyn_cast<llvm::CastInst>(constExpr->getAsInstruction())){		
							if (GlobalVariable* global = dyn_cast<GlobalVariable>(castInst->getOperand(0))) {
								if (ConstantArray* array = dyn_cast<ConstantArray>(global->getInitializer())) {
									for(unsigned int i = 0; i < array->getNumOperands(); i++){
										if(ConstantExpr* ce = dyn_cast<ConstantExpr>(array->getOperand(i))){
											if(GetElementPtrInst *gep = dyn_cast<GetElementPtrInst>(ce->getAsInstruction())){
												if(GlobalVariable* gv = dyn_cast<GlobalVariable>(gep->getPointerOperand())){
													if(dyn_cast<ConstantDataSequential>(gv->getInitializer())){
														errs() << "WARNING : " << getGlobalStringValue(gv) << " won't be ecnrypted (char** encryption not fully supported!)!\n";
														std::vector<GlobalVariable*>::iterator it = std::find(StringGlobalVars.begin(), StringGlobalVars.end(), gv);
														if(it != StringGlobalVars.end()){
															StringGlobalVars.erase(it);
														}
													}
												}
											}
										}
									}
								}
							}
						}
					}
				}
			
			}
		}
	}
}

bool AbstractStringEncryptionPass::encryptString(Module &M, std::vector<GlobalVariable*>& StringGlobalVars, std::vector<GlobalVariable*>& StringGlobalVarsToDelete) {
	bool changed = false;
	for(std::vector<GlobalVariable*>::iterator it = StringGlobalVars.begin(); it != StringGlobalVars.end(); ++it){
		GlobalVariable* GV = *it;
		//get clear text string
		std::string clearstr = getGlobalStringValue(GV);

		GlobalVariable::LinkageTypes lt = GV->getLinkage();
		switch(lt){
			default:
				//not supported
				//errs() << "WARNING : " << GV->getName() << "use unsupported linkage type (" << lt << ") SKIP ENCRYPTION!\n";
				//GV->dump();
				break;
			case GlobalVariable::ExternalLinkage:
			case GlobalVariable::InternalLinkage:
			case GlobalVariable::PrivateLinkage:
				//linkage supported

				//encrypt current string
				std::string encryptedString = stringEncryption(clearstr);

				//create new global string with the encrypted string
				//@todo check if name does not exist in module
				std::ostringstream oss;
				oss << ".encstr" << encryptedStringCounter << "_" << sys::Process::GetRandomNumber();
				encryptedStringCounter++;
				Constant *cryptedStr = ConstantDataArray::getString(M.getContext(), encryptedString, true);
				GlobalVariable* gCryptedStr = new GlobalVariable(M, cryptedStr->getType(), true, GV->getLinkage(), cryptedStr, oss.str());
				StringMapGlobalVars[oss.str()] = gCryptedStr;
				//replace use of clear string with encrypted string
				GV->replaceAllUsesWith(gCryptedStr);
				//remove clear text global
				StringGlobalVarsToDelete.push_back(GV);
				changed = true;
				break;
		}
	}
	return changed;
}

void AbstractStringEncryptionPass::insertDecryptionCode(Module &M){
	//iterate every instruction of the module and get the list of every gep instruction
	std::vector<GetElementPtrInst*> geps;
	for (Module::iterator I = M.begin(), E = M.end(); I != E; ++I) {
		for (Function::iterator bb = I->begin(), e = I->end(); bb != e; ++bb) {
			for (BasicBlock::iterator inst = bb->begin(); inst != bb->end(); ++inst) {
				//check if instruction is a get element pointer
				GetElementPtrInst* gepInst = dyn_cast<GetElementPtrInst>(inst);
				if (gepInst != 0) {
					//store it for later, decoding string with other instruction may add other gep
					geps.push_back(gepInst);
					continue;
				}
			}
		}
	}
	
	//iterate every instruction of the module and insert decryption code 
	//each time an encrypted string is used.
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

				//check if instruction is invoke
				InvokeInst *invoke = dyn_cast<InvokeInst>(inst);
				if(invoke != 0){
					handleInvoke(M, invoke);
					continue;
				}

				//check if instruction is getelementptr
				ReturnInst *ret = dyn_cast<ReturnInst>(inst);
				if (ret != 0) {
					handleReturn(M, ret);
					continue;
				}
				
				//check if instruction is store
				StoreInst *store = dyn_cast<StoreInst>(inst);
				if (store != 0) {
					handleStore(M, store);
					continue;
				}
			}
		}
	}
	
	//handle original gep instruction
	for(std::vector<GetElementPtrInst*>::iterator it = geps.begin(); it != geps.end(); ++it){
		GetElementPtrInst* gep = *it;
		handleGEP(M, gep);
	}
}

void AbstractStringEncryptionPass::handleLoad(Module &M, LoadInst* Load) {
	//check if loaded pointer is global
	Value* ptrOp = Load->getPointerOperand();
	GlobalVariable *GV = dyn_cast<GlobalVariable>(ptrOp);
	if (GV == 0){
		// handle load i8* getelementptr inbounds ([X x i8]* @string, i32 0, i64 x), align 1
		ConstantExpr *constExpr = dyn_cast<ConstantExpr>(ptrOp);
		if(constExpr != 0){
			GetElementPtrInst* gepInst = dyn_cast<GetElementPtrInst>(constExpr->getAsInstruction());
			if (gepInst != 0) {
				//check if the string is encrypted
				StringRef gepOpName = gepInst->getPointerOperand()->getName();
				std::map<std::string, GlobalVariable*>::iterator it = StringMapGlobalVars.find(gepOpName.str());
				if(it != StringMapGlobalVars.end()){
					//get size of string
					ConstantDataSequential *cds = dyn_cast<ConstantDataSequential>(it->second->getInitializer());
					uint64_t size = cds->getNumElements();
					//generate IR to decrypt string
					Value* decryptedStr = stringDecryption(M, it->second, size, Load);
					std::vector<Value*> idxlist;
					idxlist.push_back(gepInst->getOperand(gepInst->getNumOperands() - 1));
					GetElementPtrInst* newGep = GetElementPtrInst::Create(decryptedStr, ArrayRef<Value*>(idxlist), "", Load);
					LoadInst* newload = new LoadInst(newGep, "", false, 8, Load);
					//replace current load with the decryption code
					Load->replaceAllUsesWith(newload);
					InstructionToDel.push_back(Load);
				}else{
					// handle load i8** getelementptr inbounds ([X x i8*]* @string_array, i32 0, i64 x), align 8					
					if (GlobalVariable* global = dyn_cast<GlobalVariable>(gepInst->getPointerOperand())) {
						if (ConstantArray* array = dyn_cast<ConstantArray>(global->getInitializer())) {
							Constant* c = array->getAggregateElement(dyn_cast<ConstantInt>(gepInst->getOperand(2))->getZExtValue());
							ConstantExpr* ce = dyn_cast<ConstantExpr>(c);
							if(ce){
								GetElementPtrInst *gepElementFromArray = dyn_cast<GetElementPtrInst>(ce->getAsInstruction());
								if(gepElementFromArray){
									StringRef gepOpName = gepElementFromArray->getPointerOperand()->getName();
									std::map<std::string, GlobalVariable*>::iterator it = StringMapGlobalVars.find(gepOpName.str());
									if(it != StringMapGlobalVars.end()){
										//get size of string
										ConstantDataSequential *cds = dyn_cast<ConstantDataSequential>(it->second->getInitializer());
										uint64_t size = cds->getNumElements();
										//generate IR to decrypt string
										Value* decryptedStr = stringDecryption(M, it->second, size, Load);
										std::vector<Value*> idxlist;
										idxlist.push_back(gepElementFromArray->getOperand(gepElementFromArray->getNumOperands() - 1));
										GetElementPtrInst* newGep = GetElementPtrInst::Create(decryptedStr, ArrayRef<Value*>(idxlist), "", Load);
										//replace current load with the decryption code
										Load->replaceAllUsesWith(newGep);
										InstructionToDel.push_back(Load);
									}
								}
							}
						}
					}
				}
			}
		}
		return;
	}

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
			InstructionToDel.push_back(Load);
		}
	}
}

void AbstractStringEncryptionPass::handleStore(Module &M, StoreInst* Store) {
   //check if loaded pointer is global
	Value* vOp = Store->getValueOperand();
	ConstantExpr *constExpr = dyn_cast<ConstantExpr>(vOp);
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
			Value* decryptedStr = stringDecryption(M, gepInst->getPointerOperand(), size, Store);
			//replace current store with the decryption code
			new StoreInst(decryptedStr, Store->getPointerOperand(), false, 8, Store);
			InstructionToDel.push_back(Store);
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

void AbstractStringEncryptionPass::handleInvoke(llvm::Module &M, llvm::InvokeInst* Invoke){
	for(unsigned i = 0; i < Invoke->getNumArgOperands(); i++){
		llvm::ConstantExpr *constExpr = llvm::dyn_cast<llvm::ConstantExpr>(Invoke->getArgOperand(i));
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
			llvm::Value* decryptedStr = stringDecryption(M, it->second, size, Invoke);
			Invoke->setArgOperand(i, decryptedStr);
		}
	}
}

void AbstractStringEncryptionPass::handleReturn(Module &M, ReturnInst* ret){
	Value* retval = ret->getReturnValue();
	if(retval == 0)
		return;

	ConstantExpr *constExpr = dyn_cast<llvm::ConstantExpr>(retval);
	if (constExpr == 0)
		return;
	//not a gep
	if (constExpr->getOpcode() != Instruction::GetElementPtr)
		return;

	llvm::GetElementPtrInst* gepInst = dyn_cast<GetElementPtrInst>(constExpr->getAsInstruction());
	if(gepInst == 0)
		return;

	//load encrypted string
	StringRef gepOpName = gepInst->getPointerOperand()->getName();
	std::map<std::string, GlobalVariable*>::iterator it = StringMapGlobalVars.find(gepOpName.str());
	if(it != StringMapGlobalVars.end()){
		//get size of string
		ConstantDataSequential *cds = dyn_cast<ConstantDataSequential>(it->second->getInitializer());
		uint64_t size = cds->getNumElements();
		//generate IR to decrypt string
		Value* decryptedStr = stringDecryption(M, it->second, size, ret);
		ReturnInst::Create(M.getContext(), decryptedStr, ret);
		InstructionToDel.push_back(ret);
	}
}

void AbstractStringEncryptionPass::handleGEP(Module &M, GetElementPtrInst* Gep) {
	//load encrypted string
	StringRef gepOpName = Gep->getPointerOperand()->getName();
	std::map<std::string, GlobalVariable*>::iterator it = StringMapGlobalVars.find(gepOpName.str());
	if(it != StringMapGlobalVars.end()){
		//get size of string
		ConstantDataSequential *cds = dyn_cast<ConstantDataSequential>(it->second->getInitializer());
		uint64_t size = cds->getNumElements();
		//generate IR to decrypt string
		llvm::Value* decryptedStr = stringDecryption(M, it->second, size, Gep);
		std::vector<Value*> idxlist;
		idxlist.push_back(Gep->getOperand(Gep->getNumOperands() - 1));
		GetElementPtrInst* newGep = GetElementPtrInst::Create(decryptedStr, ArrayRef<Value*>(idxlist), "", Gep);
		Gep->replaceAllUsesWith(newGep);
		InstructionToDel.push_back(Gep);
		return;
	}
}
