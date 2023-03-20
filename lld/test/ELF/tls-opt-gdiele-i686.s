// REQUIRES: x86
// RUN: llvm-mc -filetype=obj -triple=i686-pc-linux %p/Inputs/tls-opt-gdiele-i686.s -o %tso.o
// RUN: llvm-mc -filetype=obj -triple=i686-pc-linux %s -o %t.o
// RUN: ld.lld -shared %tso.o -o %tso
// RUN: ld.lld --hash-style=sysv %t.o %tso -o %tout
// RUN: llvm-readobj -r %tout | FileCheck --check-prefix=NORELOC %s
// RUN: llvm-objdump -d %tout | FileCheck --check-prefix=DISASM %s

// NORELOC:      Relocations [
// NORELOC-NEXT: Section ({{.*}}) .rel.dyn {
// NORELOC-NEXT:   0x402058 R_386_TLS_TPOFF tlsshared0 0x0
// NORELOC-NEXT:   0x40205C R_386_TLS_TPOFF tlsshared1 0x0
// NORELOC-NEXT:   }
// NORELOC-NEXT: ]

// DISASM:      Disassembly of section .text:
// DISASM-EMPTY:
// DISASM-NEXT: _start:
// DISASM-NEXT: 401000: 65 a1 00 00 00 00 movl %gs:0, %eax
// DISASM-NEXT: 401006: 03 83 58 f0 ff ff addl -4008(%ebx), %eax
// DISASM-NEXT: 40100c: 65 a1 00 00 00 00 movl %gs:0, %eax
// DISASM-NEXT: 401012: 03 83 5c f0 ff ff addl -4004(%ebx), %eax
// DISASM-NEXT: 401018: 65 a1 00 00 00 00 movl %gs:0, %eax
// DISASM-NEXT: 40101e: 81 e8 08 00 00 00 subl $8, %eax
// DISASM-NEXT: 401024: 65 a1 00 00 00 00 movl %gs:0, %eax
// DISASM-NEXT: 40102a: 81 e8 04 00 00 00 subl $4, %eax

.type tlsexe1,@object
.section .tbss,"awT",@nobits
.globl tlsexe1
.align 4
tlsexe1:
 .long 0
 .size tlsexe1, 4

.type tlsexe2,@object
.section .tbss,"awT",@nobits
.globl tlsexe2
.align 4
tlsexe2:
 .long 0
 .size tlsexe2, 4

.section .text
.globl ___tls_get_addr
.type ___tls_get_addr,@function
___tls_get_addr:

.section .text
.globl _start
_start:
//GD->IE
leal tlsshared0@tlsgd(,%ebx,1),%eax
call ___tls_get_addr@plt
leal tlsshared1@tlsgd(,%ebx,1),%eax
call ___tls_get_addr@plt
//GD->LE
leal tlsexe1@tlsgd(,%ebx,1),%eax
call ___tls_get_addr@plt
leal tlsexe2@tlsgd(,%ebx,1),%eax
call ___tls_get_addr@plt
