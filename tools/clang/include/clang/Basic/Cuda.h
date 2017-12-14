//===--- Cuda.h - Utilities for compiling CUDA code  ------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_BASIC_CUDA_H
#define LLVM_CLANG_BASIC_CUDA_H

namespace llvm {
class StringRef;
} // namespace llvm

namespace clang {

enum class CudaVersion {
  UNKNOWN,
  CUDA_70,
  CUDA_75,
  CUDA_80,
};
const char *CudaVersionToString(CudaVersion V);

// No string -> CudaVersion conversion function because there's no canonical
// spelling of the various CUDA versions.

enum class CudaArch {
  UNKNOWN,
  SM_20,
  SM_21,
  SM_30,
  SM_32,
  SM_35,
  SM_37,
  SM_50,
  SM_52,
  SM_53,
  SM_60,
  SM_61,
  SM_62,
};
const char *CudaArchToString(CudaArch A);

// The input should have the form "sm_20".
CudaArch StringToCudaArch(llvm::StringRef S);

enum class CudaVirtualArch {
  UNKNOWN,
  COMPUTE_20,
  COMPUTE_30,
  COMPUTE_32,
  COMPUTE_35,
  COMPUTE_37,
  COMPUTE_50,
  COMPUTE_52,
  COMPUTE_53,
  COMPUTE_60,
  COMPUTE_61,
  COMPUTE_62,
};
const char *CudaVirtualArchToString(CudaVirtualArch A);

// The input should have the form "compute_20".
CudaVirtualArch StringToCudaVirtualArch(llvm::StringRef S);

/// Get the compute_xx corresponding to an sm_yy.
CudaVirtualArch VirtualArchForCudaArch(CudaArch A);

/// Get the earliest CudaVersion that supports the given CudaArch.
CudaVersion MinVersionForCudaArch(CudaArch A);

} // namespace clang

#endif
