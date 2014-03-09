add_llvm_library(LLVMObfuscation
  CryptoUtils.cpp
  Substitution.cpp
  BogusControlFlow.cpp
  AbsractStringEncryptionPass.cpp
  XorStringEncryption.cpp
  Flattening.cpp
  )

add_dependencies(LLVMObfuscation intrinsics_gen)
