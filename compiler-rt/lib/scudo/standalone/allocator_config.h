//===-- allocator_config.h --------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef SCUDO_ALLOCATOR_CONFIG_H_
#define SCUDO_ALLOCATOR_CONFIG_H_

#include "combined.h"
#include "common.h"
#include "flags.h"
#include "primary32.h"
#include "primary64.h"
#include "size_class_map.h"
#include "tsd_exclusive.h"
#include "tsd_shared.h"

namespace scudo {

// Default configurations for various platforms.

struct DefaultConfig {
  using SizeClassMap = DefaultSizeClassMap;
#if SCUDO_CAN_USE_PRIMARY64
  // 1GB Regions
  typedef SizeClassAllocator64<SizeClassMap, 30U> Primary;
#else
  // 512KB regions
  typedef SizeClassAllocator32<SizeClassMap, 19U> Primary;
#endif
  template <class A> using TSDRegistryT = TSDRegistryExT<A>; // Exclusive
};

struct AndroidConfig {
  using SizeClassMap = AndroidSizeClassMap;
#if SCUDO_CAN_USE_PRIMARY64
  // 1GB regions
  typedef SizeClassAllocator64<SizeClassMap, 30U> Primary;
#else
  // 512KB regions
  typedef SizeClassAllocator32<SizeClassMap, 19U> Primary;
#endif
  template <class A>
  using TSDRegistryT = TSDRegistrySharedT<A, 2U>; // Shared, max 2 TSDs.
};

struct AndroidSvelteConfig {
  using SizeClassMap = SvelteSizeClassMap;
#if SCUDO_CAN_USE_PRIMARY64
  // 512MB regions
  typedef SizeClassAllocator64<SizeClassMap, 29U> Primary;
#else
  // 256KB regions
  typedef SizeClassAllocator32<SizeClassMap, 18U> Primary;
#endif
  template <class A>
  using TSDRegistryT = TSDRegistrySharedT<A, 1U>; // Shared, only 1 TSD.
};

struct FuchsiaConfig {
  // 1GB Regions
  typedef SizeClassAllocator64<DefaultSizeClassMap, 30U> Primary;
  template <class A>
  using TSDRegistryT = TSDRegistrySharedT<A, 8U>; // Shared, max 8 TSDs.
};

#if SCUDO_ANDROID
typedef AndroidConfig Config;
#elif SCUDO_FUCHSIA
typedef FuchsiaConfig Config;
#else
typedef DefaultConfig Config;
#endif

} // namespace scudo

#endif // SCUDO_ALLOCATOR_CONFIG_H_
