// REQUIRES: x86
// RUN: llvm-mc -filetype=obj -triple=x86_64-pc-linux %s -o %t.o
// RUN: ld.lld %t.o -shared -o %t.so
// RUN: llvm-readelf -S %t.so | FileCheck %s -check-prefix=SECTION
// RUN: llvm-objdump -d %t.so | FileCheck %s

// SECTION: .got.plt PROGBITS 0000000000003000 003000 000018

// 0x3000 (.got.plt) - 0x1000 = 8192
// CHECK: gotpc64:
// CHECK-NEXT: 1000: {{.*}} movabsq $8192, %r11
.global gotpc64
gotpc64:
  movabsq $_GLOBAL_OFFSET_TABLE_-., %r11
