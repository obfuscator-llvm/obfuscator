#define DEBUG_TYPE "objdiv"
#include <string>
#include <strstream>

#include "llvm/ADT/Statistic.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Value.h"
#include "llvm/IR/CallSite.h"
#include "llvm/Pass.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/CryptoUtils.h"
#include "llvm/Transforms/Obfuscation/StringObfuscation.h"

using namespace llvm;

STATISTIC(GlobalsEncoded, "Counts number of global variables encoded");

namespace llvm {
    
    struct encVar {
    public:
        GlobalVariable *var;
        char key;
    };
    
    class StringObfuscationPass: public llvm::ModulePass {
    public:
        static char ID; // pass identification
        bool is_flag = false;
        StringObfuscationPass() : ModulePass(ID) {}
        StringObfuscationPass(bool flag) : ModulePass(ID)
        {
            is_flag = flag;
        }
        
        virtual bool runOnModule(Module &M) {
            if(!is_flag)
                return false;
            std::vector<GlobalVariable*> toDelConstGlob;
            //std::vector<GlobalVariable*> encGlob;
            std::vector<encVar*> encGlob;
            for (Module::global_iterator gi = M.global_begin(), ge = M.global_end();
                 gi != ge; ++gi) {
                // Loop over all global variables
                GlobalVariable* gv = &(*gi);
                //errs() << "Global var " << gv->getName();
                std::string::size_type str_idx = gv->getName().str().find(".str.");
                std::string section(gv->getSection());
                
                // Let's encode the static ones
                if (gv->isConstant() && gv->hasInitializer() && isa<ConstantDataSequential>(gv->getInitializer()) && section != "llvm.metadata" && section.find("__objc_methname") == std::string::npos) {
                    ++GlobalsEncoded;
                    //errs() << " is constant";
                    
                    // Duplicate global variable
                    GlobalVariable *dynGV = new GlobalVariable(M,
                                                               gv->getType()->getElementType(),
                                                               !(gv->isConstant()), gv->getLinkage(),
                                                               (Constant*) 0, gv->getName(),
                                                               (GlobalVariable*) 0,
                                                               gv->getThreadLocalMode(),
                                                               gv->getType()->getAddressSpace());
                    // dynGV->copyAttributesFrom(gv);
                    dynGV->setInitializer(gv->getInitializer());
                    
                    Constant *initializer = gv->getInitializer();
                    ConstantDataSequential *cdata = dyn_cast<ConstantDataSequential>(initializer);
                    if (cdata) {
                        const char *orig = cdata->getRawDataValues().data();
                        unsigned int len = cdata->getNumElements()*cdata->getElementByteSize();
                        
                        encVar *cur = new encVar();
                        cur->var = dynGV;
                        cur->key = llvm::cryptoutils->get_uint8_t();
                        
                        // casting away const is undef. behavior in C++
                        // TODO a clean implementation would retrieve the data, generate a new constant
                        // set the correct type, and copy the data over.
                        //char *encr = new char[len];
                        //Constant *initnew = ConstantDataArray::getString(M.getContext(), encr, true);
                        char *encr = (char*)orig; // ugly but works for now
                        
                        // Simple xor encoding
                        for (unsigned i = 0; i != len; ++i) {
                            encr[i] = orig[i]^cur->key;
                        }
                        
                        // FIXME Second part of the unclean hack.
                        dynGV->setInitializer(initializer);
                        
                        // Prepare to add decode function for this variable
                        encGlob.push_back(cur);
                    } else {
                        // errs() << " undhandled!";
                        // just copying default initializer for now
                        dynGV->setInitializer(initializer);
                    }
                    
                    // redirect references to new GV and remove old one
                    gv->replaceAllUsesWith(dynGV);
                    toDelConstGlob.push_back(gv);
                    
                }
            }
            
            // actuallte delete marked globals
            for (unsigned i = 0, e = toDelConstGlob.size(); i != e; ++i)
                toDelConstGlob[i]->eraseFromParent();
            
            
            // create code to initialize global variables at runtime
            addDecodeFunction(&M, &encGlob);
            
            return true;
        }
        
    private:
        void addDecodeFunction(Module *mod, std::vector<encVar*> *gvars) {
            // Declare and add the function definition
            std::vector<Type*>FuncTy_args;
            FunctionType* FuncTy = FunctionType::get(
                                                     /*Result=*/Type::getVoidTy(mod->getContext()),  // returning void
                                                     /*Params=*/FuncTy_args,  // taking no args
                                                     /*isVarArg=*/false);
            uint64_t StringObfDecodeRandomName = cryptoutils->get_uint64_t();
            std::string  random_str;
            std::strstream random_stream;
            random_stream << StringObfDecodeRandomName;
            random_stream >> random_str;
            StringObfDecodeRandomName++;
            Constant* c = mod->getOrInsertFunction(".datadiv_decode" + random_str, FuncTy);
            //errs() << "Function name is " << ".datadiv_decode" + random_str << "\n";
            Function* fdecode = cast<Function>(c);
            fdecode->setCallingConv(CallingConv::C);
            
            // Declare some constants we'll use in the loop
            ConstantInt* const_0 = ConstantInt::get(mod->getContext(), APInt(32, 0));
            ConstantInt* const_1 = ConstantInt::get(mod->getContext(), APInt(32, 1));
            BasicBlock* label_entry = BasicBlock::Create(mod->getContext(), "entry", fdecode);
            
            
            for (unsigned i = 0, e = gvars->size(); i != e; ++i) {
                GlobalVariable *gvar = (*gvars)[i]->var;
                char key = (*gvars)[i]->key;
                //errs() << "Adding code for " << gvar->getName() << '\n';
                
                Constant *init = gvar->getInitializer();
                ConstantDataSequential *cdata = dyn_cast<ConstantDataSequential>(init);
                
                unsigned len = cdata->getNumElements()*cdata->getElementByteSize();
                
                // Add per-GV local decode code:
                // for len(globVar): globVar[i] = globVar[i]^key
                
                ConstantInt* const_len = ConstantInt::get(mod->getContext(), APInt(32, len));
                BasicBlock* label_for_body = BasicBlock::Create(mod->getContext(), "for.body", fdecode, 0);
                BasicBlock* label_for_end = BasicBlock::Create(mod->getContext(), "for.end", fdecode, 0);
                
                ICmpInst* cmp = new ICmpInst(*label_entry, ICmpInst::ICMP_EQ, const_len, const_0, "cmp");
                BranchInst::Create(label_for_end, label_for_body, cmp, label_entry);
                
                // Block for.body (label_for_body)
                Argument* fwdref_18 = new Argument(IntegerType::get(mod->getContext(), 32));
                PHINode* int32_i = PHINode::Create(IntegerType::get(mod->getContext(), 32), 2, "i.09", label_for_body);
                int32_i->addIncoming(fwdref_18, label_for_body);
                int32_i->addIncoming(const_0, label_entry);
                
                CastInst* int64_idxprom = new ZExtInst(int32_i, IntegerType::get(mod->getContext(), 64), "idxprom", label_for_body);
                LoadInst* ptr_19 = new LoadInst(gvar, "", false, label_for_body);
                ptr_19->setAlignment(8);
                // Array/ptr load (GetElementPtr can be a bitch about this!)
                std::vector<Value*> ptr_32_indices;
                ptr_32_indices.push_back(const_0);
                ptr_32_indices.push_back(int64_idxprom);
                ArrayRef<Value*> ref_ptr_32_indices = ArrayRef<Value*>(ptr_32_indices);
                Instruction* ptr_arrayidx = GetElementPtrInst::Create(NULL, gvar, ref_ptr_32_indices, "arrayidx", label_for_body);
                // Load
                LoadInst* int8_20 = new LoadInst(ptr_arrayidx, "", false, label_for_body);
                int8_20->setAlignment(1);
                // Decode
                ConstantInt* const_key = ConstantInt::get(mod->getContext(), APInt(8, key));
                //BinaryOperator* int8_dec = BinaryOperator::Create(Instruction::Add, int8_20, const_key, "sub", label_for_body);
                BinaryOperator* int8_dec = BinaryOperator::Create(Instruction::Xor, int8_20, const_key, "xor", label_for_body);
                // Store
                StoreInst* void_21 = new StoreInst(int8_dec, ptr_arrayidx, false, label_for_body);
                void_21->setAlignment(1);
                
                // Adjust loop counter
                BinaryOperator* int32_inc = BinaryOperator::Create(Instruction::Add, int32_i, const_1, "inc", label_for_body);
                
                ICmpInst* int1_cmp = new ICmpInst(*label_for_body, ICmpInst::ICMP_EQ, int32_inc, const_len, "cmp");
                BranchInst::Create(label_for_end, label_for_body, int1_cmp, label_for_body);
                
                // Resolve Forward References
                fwdref_18->replaceAllUsesWith(int32_inc); delete fwdref_18;
                
                // adjust for next iteration
                label_entry = label_for_end;
            }
            
            // Block for.end (label_for_end)
            ReturnInst::Create(mod->getContext(), label_entry);
            
            
            // Lets add our function to the constructors
            std::vector<Type*>StructTy_4_fields;
            StructTy_4_fields.push_back(IntegerType::get(mod->getContext(), 32));
            std::vector<Type*>FuncTy_6_args;
            FunctionType* FuncTy_6 = FunctionType::get(
                                                       /*Result=*/Type::getVoidTy(mod->getContext()),
                                                       /*Params=*/FuncTy_6_args,
                                                       /*isVarArg=*/false);
            
            PointerType* PointerTy_5 = PointerType::get(FuncTy_6, 0);
            StructTy_4_fields.push_back(PointerTy_5);
            StructType *StructTy_4 = StructType::get(mod->getContext(), StructTy_4_fields, /*isPacked=*/false);
            ArrayType* ArrayTy_3 = ArrayType::get(StructTy_4, 1);
            
            GlobalVariable* gvar_array_llvm_global_ctors = NULL;
            for (Module::global_iterator gi = mod->global_begin(); gi != mod->global_end(); ++gi)
            {
                if(gi->getName() == "llvm.global_ctors")
                    gvar_array_llvm_global_ctors = &*gi;
            }
            
            std::vector<Constant*> const_array_10_elems;
            std::vector<Constant*> const_struct_11_fields;
            ConstantInt* const_int32_16max = ConstantInt::get(mod->getContext(), APInt(32, StringRef("65535"), 10));
            const_struct_11_fields.push_back(const_int32_16max);
            const_struct_11_fields.push_back(fdecode);
            Constant* const_struct_11 = ConstantStruct::get(StructTy_4, const_struct_11_fields);
            const_array_10_elems.push_back(const_struct_11);
            Constant* const_array_10 = ConstantArray::get(ArrayTy_3, const_array_10_elems);
            // Global Variable Definitions
            if(!gvar_array_llvm_global_ctors)
            {
                gvar_array_llvm_global_ctors = new GlobalVariable(/*Module=*/*mod,
                                                                  /*Type=*/ArrayTy_3,
                                                                  /*isConstant=*/false,
                                                                  /*Linkage=*/GlobalValue::AppendingLinkage,
                                                                  /*Initializer=*/0, // has initializer, specified below
                                                                  /*Name=*/"llvm.global_ctors");
                gvar_array_llvm_global_ctors->setInitializer(const_array_10);
            }
            else
            {
                ConstantArray* initer =  dyn_cast<ConstantArray>(gvar_array_llvm_global_ctors->getInitializer());
                for (Use &f : initer->operands())
                    const_array_10_elems.push_back(dyn_cast<Constant>(&*f));
                const_array_10 = ConstantArray::get(ArrayTy_3, const_array_10_elems);
                gvar_array_llvm_global_ctors->setInitializer(const_array_10);
            }
        }
        
    };
    
}

char StringObfuscationPass::ID = 0;
static RegisterPass<StringObfuscationPass> X("GVDiv", "Global variable (i.e., const char*) diversification pass", false, true);

Pass * llvm::createStringObfuscation(bool flag) {
    return new StringObfuscationPass(flag);
}
