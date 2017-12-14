//===-- X86TargetMachine.h - Define TargetMachine for the X86 ---*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file declares the X86 specific subclass of TargetMachine.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_X86_X86TARGETMACHINE_H
#define LLVM_LIB_TARGET_X86_X86TARGETMACHINE_H

#include "X86Subtarget.h"
#include "llvm/ADT/Optional.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/Analysis/TargetTransformInfo.h"
#include "llvm/Support/CodeGen.h"
#include "llvm/Target/TargetMachine.h"
#include <memory>

namespace llvm {

class StringRef;
class X86Subtarget;
class X86RegisterBankInfo;

class X86TargetMachine final : public LLVMTargetMachine {
  std::unique_ptr<TargetLoweringObjectFile> TLOF;
  mutable StringMap<std::unique_ptr<X86Subtarget>> SubtargetMap;

public:
  X86TargetMachine(const Target &T, const Triple &TT, StringRef CPU,
                   StringRef FS, const TargetOptions &Options,
                   Optional<Reloc::Model> RM, CodeModel::Model CM,
                   CodeGenOpt::Level OL);
  ~X86TargetMachine() override;

  const X86Subtarget *getSubtargetImpl(const Function &F) const override;
  // The no argument getSubtargetImpl, while it exists on some targets, is
  // deprecated and should not be used.
  const X86Subtarget *getSubtargetImpl() const = delete;

  TargetIRAnalysis getTargetIRAnalysis() override;

  // Set up the pass pipeline.
  TargetPassConfig *createPassConfig(PassManagerBase &PM) override;

  TargetLoweringObjectFile *getObjFileLowering() const override {
    return TLOF.get();
  }

  bool isMachineVerifierClean() const override {
    return false;
  }
};

} // end namespace llvm

#endif // LLVM_LIB_TARGET_X86_X86TARGETMACHINE_H
