// REQUIRES: aarch64
// RUN: llvm-mc -filetype=obj -triple=aarch64-linux-gnu %s -o %t
// RUN: ld.lld %t -o %t2 2>&1
// RUN: llvm-objdump -d  -start-address=136118280 -stop-address=136118292 -triple=aarch64-linux-gnu %t2 | FileCheck %s
// Check that the range extension thunks are dumped close to the aarch64 branch
// range of 128 MiB
 .section .text.1, "ax", %progbits
 .balign 0x1000
 .globl _start
_start:
 bl high_target
 ret

 .section .text.2, "ax", %progbits
 .space 0x2000000

 .section .text.2, "ax", %progbits
 .space 0x2000000

 .section .text.3, "ax", %progbits
 .space 0x2000000

 .section .text.4, "ax", %progbits
 .space 0x2000000 - 0x40000

 .section .text.5, "ax", %progbits
 .space 0x40000

 .section .text.6, "ax", %progbits
 .balign 0x1000

 .globl high_target
 .type high_target, %function
high_target:
 ret

// CHECK: __AArch64AbsLongThunk_high_target:
// CHECK-NEXT:  81d0008:        50 00 00 58     ldr     x16, #8
// CHECK-NEXT:  81d000c:        00 02 1f d6     br      x16
// CHECK: $d:
// CHECK-NEXT:  81d0010:        00 10 21 08     .word   0x08211000
