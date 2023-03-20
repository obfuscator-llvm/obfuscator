add_llvm_library( LLVMEncodeFunctionName
        EncodeFunctionName.cpp

        ADDITIONAL_HEADER_DIRS
        ${LLVM_MAIN_INCLUDE_DIR}/llvm/Transforms
        ${LLVM_MAIN_INCLUDE_DIR}/llvm/Transforms/EncodeFunctionName

        DEPENDS
        intrinsics_gen
        )