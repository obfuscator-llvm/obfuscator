
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/JSON.h"

#include "llvm/Transforms/Obfuscation/Obfuscation.h"
#include "llvm/Transforms/Obfuscation/BogusControlFlow.h"
#include "llvm/Transforms/Obfuscation/Flattening.h"
#include "llvm/Transforms/Obfuscation/Split.h"
#include "llvm/Transforms/Obfuscation/Substitution.h"
#include "llvm/Transforms/Obfuscation/StringObfuscation.h"
#include "llvm/Transforms/Obfuscation/CryptoUtils.h"

#include <iostream>
#include <fstream>
#include <sstream>

using namespace llvm;
using namespace std;

// Flags for Obfuscation
static cl::opt<bool> Flattening(
  "fla", cl::init(false),
  cl::desc("Enable the flattening pass"));

static cl::opt<bool> BogusControlFlow(
  "bcf", cl::init(false),
  cl::desc("Enable bogus control flow"));

static cl::opt<bool> Substitution(
  "sub", cl::init(false),
  cl::desc("Enable instruction substitutions"));

static cl::opt<std::string> AesSeed(
  "aesSeed", cl::init(""),
  cl::desc("seed for the AES-CTR PRNG"));

static cl::opt<bool> Split(
  "spli", cl::init(false),
  cl::desc("Enable basic block splitting"));

static cl::opt<std::string> Seed("seed", cl::init(""),
                           cl::desc("seed for the random"));

static cl::opt<bool> StringObf("sobf", cl::init(false),
                           cl::desc("Enable the string obfuscation"));

namespace llvm {

struct ObfuscationPass {
	bool enabled ;

	ObfuscationPass() :
		enabled(false)
	{
	}
};

struct SplittingPass : ObfuscationPass {
	int num ;

	SplittingPass() :
		ObfuscationPass(), num(0)
	{
	}
};

struct BogusControlFlowPass : ObfuscationPass {
	int probability ;
	int loop ;

	BogusControlFlowPass() :
		ObfuscationPass(), probability(0), loop(0)
	{
	}
};

struct ObfuscationConfig {
	ObfuscationPass sub ;
	ObfuscationPass fla ;
	SplittingPass spli ;
	BogusControlFlowPass bcf ;
	ObfuscationPass sobf ;

	ObfuscationConfig() :
		sub(), fla(), spli(), bcf(), sobf() {
	}
};

#define O_LLVM_CONFIG_FILENAME "o-llvm.json"
#define O_LLVM "O-LLVM"

struct Obfuscation : public ModulePass {
  static char ID;
  Obfuscation() : ModulePass(ID) {
  }

  StringRef getPassName() const override {
    return StringRef("Obfuscation");
  }

  bool runOnModule(Module &M) override {

	ObfuscationConfig ocfg ;
	ocfg.sub.enabled = Substitution ;
	ocfg.spli.enabled = Split ;
	ocfg.bcf.enabled = BogusControlFlow ;
	ocfg.fla.enabled = Flattening ;
	ocfg.sobf.enabled = StringObf ;

	outs() << O_LLVM << " : Adding Obfuscation passes\n";

	outs() << O_LLVM << " : configuration file" ;
	if( llvm::sys::fs::exists(O_LLVM_CONFIG_FILENAME) ) {
		outs() << " exists\n";

		if(std::ifstream is_cfg {O_LLVM_CONFIG_FILENAME, std::ios::binary | std::ios::ate}) {
			auto size = is_cfg.tellg();
			std::string cfg(size, '\0'); // construct string to stream size
			is_cfg.seekg(0);
			if(is_cfg.read(&cfg[0], size))
				outs() << cfg << "\n" ;

			// Try to parse content
			outs() << "Parsing content...\n" ;
			auto ValueOrError = json::parse(cfg) ;

			handleAllErrors(
				ValueOrError.takeError(),
				[](const json::ParseError &PE) {
					errs() << O_LLVM << " Error: Error when parsing content (" << PE.message() << ").\n"  ;
				}) ;

			if( ValueOrError ) {
				// On success, grab a reference to the Value and continue.
				auto &V = *ValueOrError ;

				json::Array* jsa = V.getAsArray() ;

				for( auto i : *jsa ) {
					json::Object* sub    = i.getAsObject()->getObject("substitution") ;
					if( NULL != sub ) {
						json::fromJSON( sub->getBoolean("enabled"), ocfg.sub.enabled ) ;
					}

					json::Object* fla    = i.getAsObject()->getObject("flattening") ;
					if( NULL != fla ) {
						json::fromJSON( fla->getBoolean("enabled"), ocfg.fla.enabled ) ;
					}

					json::Object* spli   = i.getAsObject()->getObject("splitting") ;
					if( NULL != spli ) {
						json::fromJSON( spli->getBoolean("enabled"), ocfg.spli.enabled ) ;
					}

					json::Object* bcf    = i.getAsObject()->getObject("bogusControlFlow") ;
					if( NULL != bcf ) {
						json::fromJSON( bcf->getBoolean("enabled"), ocfg.bcf.enabled ) ;
					}

					json::Object* strobf = i.getAsObject()->getObject("stringObfuscation") ;
					if( NULL != strobf ) {
						json::fromJSON( strobf->getBoolean("enabled"), ocfg.sobf.enabled ) ;
					}

					json::Object* appy   = i.getAsObject()->getObject("appliesTo") ;
				}
			}
		} else {
			errs() << "ERROR : Unable to open file.\n";
		}
	}
	else
		outs() << " does not exist\n";

	outs() << "Launching Function Passes for Module " << M.getName() << "...\n" ;
    // Function Passes
    for (Module::iterator iter = M.begin(); iter != M.end(); iter++) {
      Function &F = *iter;
      if (!F.isDeclaration()) {

		FunctionPass *P = NULL;

		P =  createSubstitution(ocfg.sub.enabled);
		P->runOnFunction(F);
		delete P;

		P = createSplitBasicBlock(ocfg.spli.enabled);
		P->runOnFunction(F);
		delete P;

		P = createBogus(ocfg.bcf.enabled);
		P->runOnFunction(F);
		delete P;

		P = createFlattening(ocfg.fla.enabled);
		P->runOnFunction(F);
		delete P;
      }
    }

    outs() << "Launching Module Passes...\n" ;
	if (StringObf) {
		ModulePass *P = createStringObfuscation(ocfg.sobf.enabled); ;
		P->doInitialization(M);
		P->runOnModule(M);
		delete P;
	}

    return true;
  }
};

ModulePass *createObfuscationPass() {
	// Initialization of the global cryptographically
	// secure pseudo-random generator
	if (!AesSeed.empty()) {
		llvm::cryptoutils->prng_seed(AesSeed.c_str());
		if (!llvm::cryptoutils->prng_seed(AesSeed.c_str()))
			exit(1);
	}

	if (!Seed.empty()) {
		llvm::cryptoutils->prng_seed(Seed.c_str());
	}
//	if (AesSeed != 0x1337) {
//		cryptoutils->prng_seed(AesSeed);
//	} else {
//		cryptoutils->prng_seed();
//	}
	outs() << "Initialized Obfuscation Pass.\n" ;
	return new Obfuscation();
}

} // namespace llvm

char Obfuscation::ID = 0;

//INITIALIZE_PASS_BEGIN(Obfuscation, "obfus", "Enable Obfuscation", true, true)
//INITIALIZE_PASS_DEPENDENCY(AntiClassDump);
//INITIALIZE_PASS_DEPENDENCY(BogusControlFlow);
//INITIALIZE_PASS_DEPENDENCY(Flattening);
//INITIALIZE_PASS_DEPENDENCY(FunctionCallObfuscate);
//INITIALIZE_PASS_DEPENDENCY(IndirectBranch);
//INITIALIZE_PASS_DEPENDENCY(SplitBasicBlock);
//INITIALIZE_PASS_DEPENDENCY(StringEncryption);
//INITIALIZE_PASS_DEPENDENCY(Substitution);
//INITIALIZE_PASS_END(Obfuscation, "obfus", "Enable Obfuscation", true, true)

static RegisterPass<Obfuscation> X("obfuscation", "Obfuscation Pass");
