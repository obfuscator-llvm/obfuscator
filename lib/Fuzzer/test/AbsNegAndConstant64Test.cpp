// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.

// abs(x) < 0 and y == Const puzzle, 64-bit variant.
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *Data, size_t Size) {
  if (Size < 16 || Size > 64) return 0;
  int64_t x;
  uint64_t y;
  memcpy(&x, Data, sizeof(x));
  memcpy(&y, Data + sizeof(x), sizeof(y));
  if (llabs(x) < 0 && y == 0xbaddcafedeadbeefULL) {
    printf("BINGO; Found the target, exiting; x = 0x%lx y 0x%lx\n", x, y);
    exit(1);
  }
  return 0;
}

