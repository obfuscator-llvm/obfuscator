add_llvm_library(LLVMObfuscation
  CgfFunction.cpp
  Flattening.cpp
  PrngAESCtr.cpp
  Substitution.cpp
  SubstitutionFunction.cpp
  BogusControlFlow.cpp
  )

add_dependencies(LLVMObfuscation intrinsics_gen)
