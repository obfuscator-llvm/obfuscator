#include "llvm/Transforms/Obfuscation/Flattening.h"

using namespace llvm;

// Shamefully borrowed from ../Scalar/RegToMem.cpp :(
bool valueEscapes(Instruction *Inst) {
    BasicBlock *BB = Inst->getParent();
    for (Value::use_iterator UI = Inst->use_begin(),E = Inst->use_end();UI != E; ++UI) {
        Instruction *I = cast<Instruction>(*UI);
        if (I->getParent() != BB || isa<PHINode>(I)) {
            return true;
        }
    }
    return false;
}

bool flatten(Function *f) {
    vector<BasicBlock*> origBB;
    BasicBlock *loopEntry;
    BasicBlock *loopEnd;
    LoadInst *load;
    SwitchInst *switchI;
    AllocaInst *switchVar;
    
    // SCRAMBLER
    char scrambling_key[16];
    llvm::cprng->get_bytes (scrambling_key, 16);
    // END OF SCRAMBLER
    
    
    
    // Save all original BB
    for (Function::iterator i=f->begin();i!=f->end();++i) {
        BasicBlock *tmp = i;
        origBB.push_back(tmp);
        
        BasicBlock *bb = i;
        if(isa<InvokeInst>(bb->getTerminator())) {
            return false;
        }
    }
    
    // Nothing to flatten
    if(origBB.size() <= 1) {
        return false;
    }
    
    // Remove first BB
    origBB.erase(origBB.begin());
    
    // Get a pointer on the first BB
    Function::iterator tmp = f->begin(); //++tmp;
    BasicBlock *insert = tmp;
    
    // If main begin with an if
    BranchInst *br = NULL;
    if(isa<BranchInst>(insert->getTerminator())) {
        br = cast<BranchInst>(insert->getTerminator());
    }
    
    if((br != NULL && br->isConditional()) || insert->getTerminator()->getNumSuccessors() > 1) {
        BasicBlock::iterator i = insert->back();
        
        if(insert->size() > 1) {
            i--;
        }
        
        BasicBlock *tmpBB = insert->splitBasicBlock(i,"first");
        origBB.insert(origBB.begin(),tmpBB);
    }
    
    // Remove jump
    insert->getTerminator()->eraseFromParent();
    
    // Create switch variable and set as it
    switchVar = new AllocaInst(Type::getInt32Ty(f->getContext()),0,"switchVar",insert);
    new StoreInst(ConstantInt::get(Type::getInt32Ty(f->getContext()), llvm::cprng->scramble32(0, scrambling_key)),switchVar,insert);
  
    
    // Create main loop
    loopEntry = BasicBlock::Create(f->getContext(),"loopEntry",f,insert);
    loopEnd = BasicBlock::Create(f->getContext(),"loopEnd",f,insert);
    
    load = new LoadInst(switchVar,"switchVar",loopEntry);
    
    // Move first BB on top
    insert->moveBefore(loopEntry);
    BranchInst::Create(loopEntry,insert);
    
    // loopEnd jump to loopEntry
    BranchInst::Create(loopEntry,loopEnd);
    
    BasicBlock *swDefault = BasicBlock::Create(f->getContext(),"switchDefault",f,loopEnd);
    BranchInst::Create(loopEnd,swDefault);
    
    // Create switch instruction itself and set condition
    switchI = SwitchInst::Create(f->begin(),swDefault,0,loopEntry);
    switchI->setCondition(load);
    
    // Remove branch jump from 1st BB and make a jump to the while
    f->begin()->getTerminator()->eraseFromParent();
    
    BranchInst::Create(loopEntry,f->begin());
    
    // Put all BB in the switch
    for(vector<BasicBlock*>::iterator b=origBB.begin();b!=origBB.end();++b) {
        BasicBlock *i = *b;
        ConstantInt *numCase = NULL;
        
        // Move the BB inside the switch (only visual, no code logic)
        i->moveBefore(loopEnd);
                
        // Add case to switch
        numCase = cast<ConstantInt>(ConstantInt::get(switchI->getCondition()->getType(),llvm::cprng->scramble32(switchI->getNumCases(), scrambling_key)));
        switchI->addCase(numCase,i);

    }
    
    // Recalculate switchVar
    for(vector<BasicBlock*>::iterator b=origBB.begin();b!=origBB.end();++b) {
        BasicBlock *i = *b;
        ConstantInt *numCase = NULL;
        
        // Ret BB
        if(i->getTerminator()->getNumSuccessors() == 0) {
            continue;
        }
        
        // If it's a non-conditional jump
        if(i->getTerminator()->getNumSuccessors() == 1) {
            // Get successor and delete terminator
            BasicBlock *succ = i->getTerminator()->getSuccessor(0);
            i->getTerminator()->eraseFromParent();
            
            // Get next case
            numCase = switchI->findCaseDest(succ);
            
            // If next case == default case (switchDefault)
            if(numCase == NULL) {
                numCase = cast<ConstantInt>(ConstantInt::get(switchI->getCondition()->getType(),llvm::cprng->scramble32(switchI->getNumCases()-1, scrambling_key)));
            }
            
            // Update switchVar and jump to the end of loop
            new StoreInst(numCase,load->getPointerOperand(),i);
            BranchInst::Create(loopEnd,i);
            continue;
        }
        
        // If it's a conditional jump
        if(i->getTerminator()->getNumSuccessors() == 2) {
            // Get next cases
            ConstantInt *numCaseTrue = switchI->findCaseDest(i->getTerminator()->getSuccessor(0));
            ConstantInt *numCaseFalse = switchI->findCaseDest(i->getTerminator()->getSuccessor(1));
            
            // Check if next case == default case (switchDefault)
            if(numCaseTrue == NULL) {
                numCaseTrue = cast<ConstantInt>(ConstantInt::get(switchI->getCondition()->getType(),llvm::cprng->scramble32(switchI->getNumCases()-1, scrambling_key)));
                
            }
            
            if(numCaseFalse == NULL) {
                numCaseFalse = cast<ConstantInt>(ConstantInt::get(switchI->getCondition()->getType(),llvm::cprng->scramble32(switchI->getNumCases()-1, scrambling_key)));
            }
            
            // Create a SelectInst
            BranchInst *br = cast<BranchInst>(i->getTerminator());
            SelectInst *sel = SelectInst::Create(br->getCondition(),numCaseTrue,numCaseFalse,"",i->getTerminator());
            
            // Erase terminator
            i->getTerminator()->eraseFromParent();
            
            // Update switchVar and jump to the end of loop
            new StoreInst(sel,load->getPointerOperand(),i);
            BranchInst::Create(loopEnd,i);
            continue;
        }
    }
    
    // Try to remove phi node and demote reg to stack
    vector<PHINode*> tmpPhi;
    vector<Instruction*> tmpReg;
    BasicBlock *bbEntry = f->begin();
    
    do {
        tmpPhi.clear();
        tmpReg.clear();

        for(Function::iterator i=f->begin();i!=f->end();++i) {

            for(BasicBlock::iterator j=i->begin();j!=i->end();++j) {

                if(isa<PHINode>(j)) {
                    PHINode *phi = cast<PHINode>(j);
                    tmpPhi.push_back(phi);
                    continue;
                }
                if(!(isa<AllocaInst>(j) && j->getParent() == bbEntry) && (valueEscapes(j) || j->isUsedOutsideOfBlock(i))) {
                    tmpReg.push_back(j);
                    continue;
                }
            }
        }
        for(unsigned int i=0;i!=tmpReg.size();++i) {
            DemoteRegToStack(*tmpReg.at(i),f->begin()->getTerminator());
        }

        for(unsigned int i=0;i!=tmpPhi.size();++i) {
            DemotePHIToStack(tmpPhi.at(i),f->begin()->getTerminator());
        }
        
    } while(tmpReg.size() != 0 || tmpPhi.size() != 0);

    
    return true;
}
