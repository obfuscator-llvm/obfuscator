add_llvm_library(LLVMObfuscation
  CryptoUtils.cpp
  Substitution.cpp
  BogusControlFlow.cpp
  AbstractStringEncryptionPass.cpp
  XorStringEncryption.cpp
  Flattening.cpp
  )

add_dependencies(LLVMObfuscation intrinsics_gen)
