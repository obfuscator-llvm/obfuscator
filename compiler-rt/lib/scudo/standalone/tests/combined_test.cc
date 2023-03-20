//===-- combined_test.cc ----------------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "allocator_config.h"
#include "combined.h"

#include "gtest/gtest.h"

#include <condition_variable>
#include <mutex>
#include <thread>

static std::mutex Mutex;
static std::condition_variable Cv;
static bool Ready = false;

static constexpr scudo::Chunk::Origin Origin = scudo::Chunk::Origin::Malloc;

// This allows us to turn on the Quarantine for specific tests. The Quarantine
// parameters are on the low end, to avoid having to loop excessively in some
// tests.
static bool UseQuarantine = false;
extern "C" const char *__scudo_default_options() {
  if (!UseQuarantine)
    return "";
  return "quarantine_size_kb=256:thread_local_quarantine_size_kb=128:"
         "quarantine_max_chunk_size=1024";
}

template <class Config> static void testAllocator() {
  using AllocatorT = scudo::Allocator<Config>;
  auto Deleter = [](AllocatorT *A) {
    A->unmapTestOnly();
    delete A;
  };
  std::unique_ptr<AllocatorT, decltype(Deleter)> Allocator(new AllocatorT,
                                                           Deleter);
  Allocator->reset();

  constexpr scudo::uptr MinAlignLog = FIRST_32_SECOND_64(3U, 4U);

  // This allocates and deallocates a bunch of chunks, with a wide range of
  // sizes and alignments, with a focus on sizes that could trigger weird
  // behaviors (plus or minus a small delta of a power of two for example).
  for (scudo::uptr SizeLog = 0U; SizeLog <= 20U; SizeLog++) {
    for (scudo::uptr AlignLog = MinAlignLog; AlignLog <= 16U; AlignLog++) {
      const scudo::uptr Align = 1U << AlignLog;
      for (scudo::sptr Delta = -32; Delta <= 32; Delta++) {
        if (static_cast<scudo::sptr>(1U << SizeLog) + Delta <= 0)
          continue;
        const scudo::uptr Size = (1U << SizeLog) + Delta;
        void *P = Allocator->allocate(Size, Origin, Align);
        EXPECT_NE(P, nullptr);
        EXPECT_TRUE(scudo::isAligned(reinterpret_cast<scudo::uptr>(P), Align));
        EXPECT_LE(Size, Allocator->getUsableSize(P));
        memset(P, 0xaa, Size);
        Allocator->deallocate(P, Origin, Size);
      }
    }
  }
  Allocator->releaseToOS();

  // Verify that a chunk will end up being reused, at some point.
  const scudo::uptr NeedleSize = 1024U;
  void *NeedleP = Allocator->allocate(NeedleSize, Origin);
  Allocator->deallocate(NeedleP, Origin);
  bool Found = false;
  for (scudo::uptr I = 0; I < 1024U && !Found; I++) {
    void *P = Allocator->allocate(NeedleSize, Origin);
    if (P == NeedleP)
      Found = true;
    Allocator->deallocate(P, Origin);
  }
  EXPECT_TRUE(Found);

  constexpr scudo::uptr MaxSize = Config::Primary::SizeClassMap::MaxSize;

  // Reallocate a large chunk all the way down to a byte, verifying that we
  // preserve the data in the process.
  scudo::uptr Size = MaxSize * 2;
  const scudo::uptr DataSize = 2048U;
  void *P = Allocator->allocate(Size, Origin);
  const char Marker = 0xab;
  memset(P, Marker, scudo::Min(Size, DataSize));
  while (Size > 1U) {
    Size /= 2U;
    void *NewP = Allocator->reallocate(P, Size);
    EXPECT_NE(NewP, nullptr);
    for (scudo::uptr J = 0; J < scudo::Min(Size, DataSize); J++)
      EXPECT_EQ((reinterpret_cast<char *>(NewP))[J], Marker);
    P = NewP;
  }
  Allocator->deallocate(P, Origin);

  // Allocates a bunch of chunks, then iterate over all the chunks, ensuring
  // they are the ones we allocated. This requires the allocator to not have any
  // other allocated chunk at this point (eg: won't work with the Quarantine).
  if (!UseQuarantine) {
    std::vector<void *> V;
    for (scudo::uptr I = 0; I < 64U; I++)
      V.push_back(Allocator->allocate(rand() % (MaxSize / 2U), Origin));
    Allocator->disable();
    Allocator->iterateOverChunks(
        0U, static_cast<scudo::uptr>(SCUDO_MMAP_RANGE_SIZE - 1),
        [](uintptr_t Base, size_t Size, void *Arg) {
          std::vector<void *> *V = reinterpret_cast<std::vector<void *> *>(Arg);
          void *P = reinterpret_cast<void *>(Base);
          EXPECT_NE(std::find(V->begin(), V->end(), P), V->end());
        },
        reinterpret_cast<void *>(&V));
    Allocator->enable();
    while (!V.empty()) {
      Allocator->deallocate(V.back(), Origin);
      V.pop_back();
    }
  }

  Allocator->releaseToOS();
  Allocator->printStats();
}

TEST(ScudoCombinedTest, BasicCombined) {
  testAllocator<scudo::DefaultConfig>();
#if SCUDO_WORDSIZE == 64U
  testAllocator<scudo::FuchsiaConfig>();
#endif
  // The following configs should work on all platforms.
  UseQuarantine = true;
  testAllocator<scudo::AndroidConfig>();
  UseQuarantine = false;
  testAllocator<scudo::AndroidSvelteConfig>();
}

template <typename AllocatorT> static void stressAllocator(AllocatorT *A) {
  {
    std::unique_lock<std::mutex> Lock(Mutex);
    while (!Ready)
      Cv.wait(Lock);
  }
  std::vector<std::pair<void *, scudo::uptr>> V;
  for (scudo::uptr I = 0; I < 256U; I++) {
    const scudo::uptr Size = std::rand() % 4096U;
    void *P = A->allocate(Size, Origin);
    // A region could have ran out of memory, resulting in a null P.
    if (P)
      V.push_back(std::make_pair(P, Size));
  }
  while (!V.empty()) {
    auto Pair = V.back();
    A->deallocate(Pair.first, Origin, Pair.second);
    V.pop_back();
  }
}

template <class Config> static void testAllocatorThreaded() {
  using AllocatorT = scudo::Allocator<Config>;
  auto Deleter = [](AllocatorT *A) {
    A->unmapTestOnly();
    delete A;
  };
  std::unique_ptr<AllocatorT, decltype(Deleter)> Allocator(new AllocatorT,
                                                           Deleter);
  Allocator->reset();
  std::thread Threads[32];
  for (scudo::uptr I = 0; I < ARRAY_SIZE(Threads); I++)
    Threads[I] = std::thread(stressAllocator<AllocatorT>, Allocator.get());
  {
    std::unique_lock<std::mutex> Lock(Mutex);
    Ready = true;
    Cv.notify_all();
  }
  for (auto &T : Threads)
    T.join();
  Allocator->releaseToOS();
}

TEST(ScudoCombinedTest, ThreadedCombined) {
  testAllocatorThreaded<scudo::DefaultConfig>();
#if SCUDO_WORDSIZE == 64U
  testAllocatorThreaded<scudo::FuchsiaConfig>();
#endif
  UseQuarantine = true;
  testAllocatorThreaded<scudo::AndroidConfig>();
  UseQuarantine = false;
  testAllocatorThreaded<scudo::AndroidSvelteConfig>();
}

struct DeathConfig {
  // Tiny allocator, its Primary only serves chunks of 1024 bytes.
  using DeathSizeClassMap = scudo::SizeClassMap<1U, 10U, 10U, 10U, 1U, 10U>;
  typedef scudo::SizeClassAllocator32<DeathSizeClassMap, 18U> Primary;
  template <class A> using TSDRegistryT = scudo::TSDRegistrySharedT<A, 1U>;
};

TEST(ScudoCombinedTest, DeathCombined) {
  using AllocatorT = scudo::Allocator<DeathConfig>;
  auto Deleter = [](AllocatorT *A) {
    A->unmapTestOnly();
    delete A;
  };
  std::unique_ptr<AllocatorT, decltype(Deleter)> Allocator(new AllocatorT,
                                                           Deleter);
  Allocator->reset();

  const scudo::uptr Size = 1000U;
  void *P = Allocator->allocate(Size, Origin);
  EXPECT_NE(P, nullptr);

  // Invalid sized deallocation.
  EXPECT_DEATH(Allocator->deallocate(P, Origin, Size + 8U), "");

  // Misaligned pointer.
  void *MisalignedP =
      reinterpret_cast<void *>(reinterpret_cast<scudo::uptr>(P) | 1U);
  EXPECT_DEATH(Allocator->deallocate(MisalignedP, Origin, Size), "");
  EXPECT_DEATH(Allocator->reallocate(MisalignedP, Size * 2U), "");

  // Header corruption.
  scudo::u64 *H =
      reinterpret_cast<scudo::u64 *>(scudo::Chunk::getAtomicHeader(P));
  *H ^= 0x42U;
  EXPECT_DEATH(Allocator->deallocate(P, Origin, Size), "");
  *H ^= 0x420042U;
  EXPECT_DEATH(Allocator->deallocate(P, Origin, Size), "");
  *H ^= 0x420000U;

  // Invalid chunk state.
  Allocator->deallocate(P, Origin, Size);
  EXPECT_DEATH(Allocator->deallocate(P, Origin, Size), "");
  EXPECT_DEATH(Allocator->reallocate(P, Size * 2U), "");
  EXPECT_DEATH(Allocator->getUsableSize(P), "");
}
