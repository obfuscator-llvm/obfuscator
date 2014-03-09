add_llvm_library(LLVMObfuscation
  CgfFunction.cpp
  Flattening.cpp
  PrngAESCtr.cpp
  Substitution.cpp
  SubstitutionFunction.cpp
  BogusControlFlow.cpp
  AbsractStringEncryptionPass.cpp
  XorStringEncryption.cpp
  )

add_dependencies(LLVMObfuscation intrinsics_gen)
