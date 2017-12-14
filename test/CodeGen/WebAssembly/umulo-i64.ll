; RUN: llc < %s -asm-verbose=false | FileCheck %s
; Test that UMULO works correctly on 64-bit operands.
target datalayout = "e-m:e-p:32:32-i64:64-n32:64-S128"
target triple = "wasm32-unknown-emscripten"

; CHECK-LABEL: _ZN4core3num21_$LT$impl$u20$u64$GT$15overflowing_mul17h07be88b4cbac028fE:
; CHECK:     __multi3
; Function Attrs: inlinehint
define void @"_ZN4core3num21_$LT$impl$u20$u64$GT$15overflowing_mul17h07be88b4cbac028fE"(i64, i64) unnamed_addr #0 {
start:
  %2 = call { i64, i1 } @llvm.umul.with.overflow.i64(i64 %0, i64 %1)
  %3 = extractvalue { i64, i1 } %2, 0
  store i64 %3, i64* undef
  unreachable
}

; Function Attrs: nounwind readnone speculatable
declare { i64, i1 } @llvm.umul.with.overflow.i64(i64, i64) #1

attributes #0 = { inlinehint }
attributes #1 = { nounwind readnone speculatable }
