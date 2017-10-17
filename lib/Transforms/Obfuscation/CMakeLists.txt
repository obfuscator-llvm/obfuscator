add_llvm_library(LLVMObfuscation
  CryptoUtils.cpp
  Substitution.cpp
  BogusControlFlow.cpp
  Utils.cpp
  SplitBasicBlocks.cpp
  Flattening.cpp
  AbstractStringEncryptionPass.cpp
  XorStringEncryption.cpp
  )

add_dependencies(LLVMObfuscation intrinsics_gen)
