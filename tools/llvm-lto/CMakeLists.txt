set(LLVM_LINK_COMPONENTS ${LLVM_TARGETS_TO_BUILD} lto support obfuscation)

add_llvm_tool(llvm-lto
  llvm-lto.cpp
  )

