add_llvm_library(LLVMObfuscation
  Obfuscation.cpp
  Substitution.cpp
  BogusControlFlow.cpp
  Utils.cpp
  SplitBasicBlocks.cpp
  Flattening.cpp
  StringObfuscation.cpp
  CryptoUtils.cpp
  )

add_dependencies(LLVMObfuscation intrinsics_gen)
