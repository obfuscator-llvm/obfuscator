; RUN: opt < %s -mtriple=x86_64-unknown-linux-gnu -mcpu=pentium4 -cost-model -analyze | FileCheck %s -check-prefix=CHECK -check-prefix=SSE -check-prefix=SSE2 -check-prefix=NOPOPCNT
; RUN: opt < %s -mtriple=x86_64-unknown-linux-gnu -mcpu=corei7 -cost-model -analyze | FileCheck %s -check-prefix=CHECK -check-prefix=SSE -check-prefix=SSE42 -check-prefix=POPCNT
; RUN: opt < %s -mtriple=x86_64-unknown-linux-gnu -mcpu=corei7-avx -cost-model -analyze | FileCheck %s -check-prefix=CHECK -check-prefix=AVX -check-prefix=AVX1 -check-prefix=POPCNT
; RUN: opt < %s -mtriple=x86_64-unknown-linux-gnu -mcpu=core-avx2 -cost-model -analyze | FileCheck %s -check-prefix=CHECK -check-prefix=AVX -check-prefix=AVX2 -check-prefix=POPCNT
; RUN: opt < %s -mtriple=x86_64-unknown-linux-gnu -mcpu=bdver2 -cost-model -analyze | FileCheck %s -check-prefix=CHECK -check-prefix=AVX -check-prefix=AVX1 -check-prefix=POPCNT
; RUN: opt < %s -mtriple=x86_64-unknown-linux-gnu -mcpu=bdver4 -cost-model -analyze | FileCheck %s -check-prefix=CHECK -check-prefix=AVX -check-prefix=AVX2 -check-prefix=POPCNT
; RUN: opt < %s -mtriple=x86_64-unknown-linux-gnu -mcpu=knl -cost-model -analyze | FileCheck %s -check-prefix=CHECK -check-prefix=AVX512 -check-prefix=AVX512F -check-prefix=POPCNT
; RUN: opt < %s -mtriple=x86_64-unknown-linux-gnu -mcpu=skx -cost-model -analyze | FileCheck %s -check-prefix=CHECK -check-prefix=AVX512 -check-prefix=AVX512BW -check-prefix=POPCNT

; Verify the cost of scalar population count instructions.

declare i64 @llvm.ctpop.i64(i64)
declare i32 @llvm.ctpop.i32(i32)
declare i16 @llvm.ctpop.i16(i16)
declare  i8 @llvm.ctpop.i8(i8)

define i64 @var_ctpop_i64(i64 %a) {
; CHECK: 'Cost Model Analysis' for function 'var_ctpop_i64':
; NOPOPCNT: Found an estimated cost of 4 for instruction:   %ctpop
; POPCNT: Found an estimated cost of 1 for instruction:   %ctpop
  %ctpop = call i64 @llvm.ctpop.i64(i64 %a)
  ret i64 %ctpop
}

define i32 @var_ctpop_i32(i32 %a) {
; CHECK: 'Cost Model Analysis' for function 'var_ctpop_i32':
; NOPOPCNT: Found an estimated cost of 4 for instruction:   %ctpop
; POPCNT: Found an estimated cost of 1 for instruction:   %ctpop
  %ctpop = call i32 @llvm.ctpop.i32(i32 %a)
  ret i32 %ctpop
}

define i16 @var_ctpop_i16(i16 %a) {
; CHECK: 'Cost Model Analysis' for function 'var_ctpop_i16':
; NOPOPCNT: Found an estimated cost of 4 for instruction:   %ctpop
; POPCNT: Found an estimated cost of 1 for instruction:   %ctpop
  %ctpop = call i16 @llvm.ctpop.i16(i16 %a)
  ret i16 %ctpop
}

define i8 @var_ctpop_i8(i8 %a) {
; CHECK: 'Cost Model Analysis' for function 'var_ctpop_i8':
; NOPOPCNT: Found an estimated cost of 4 for instruction:   %ctpop
; POPCNT: Found an estimated cost of 1 for instruction:   %ctpop
  %ctpop = call i8 @llvm.ctpop.i8(i8 %a)
  ret i8 %ctpop
}

; Verify the cost of vector population count instructions.

declare <2 x i64> @llvm.ctpop.v2i64(<2 x i64>)
declare <4 x i32> @llvm.ctpop.v4i32(<4 x i32>)
declare <8 x i16> @llvm.ctpop.v8i16(<8 x i16>)
declare <16 x i8> @llvm.ctpop.v16i8(<16 x i8>)

declare <4 x i64> @llvm.ctpop.v4i64(<4 x i64>)
declare <8 x i32> @llvm.ctpop.v8i32(<8 x i32>)
declare <16 x i16> @llvm.ctpop.v16i16(<16 x i16>)
declare <32 x i8> @llvm.ctpop.v32i8(<32 x i8>)

declare <8 x i64> @llvm.ctpop.v8i64(<8 x i64>)
declare <16 x i32> @llvm.ctpop.v16i32(<16 x i32>)
declare <32 x i16> @llvm.ctpop.v32i16(<32 x i16>)
declare <64 x i8> @llvm.ctpop.v64i8(<64 x i8>)

define <2 x i64> @var_ctpop_v2i64(<2 x i64> %a) {
; CHECK: 'Cost Model Analysis' for function 'var_ctpop_v2i64':
; SSE2: Found an estimated cost of 12 for instruction:   %ctpop
; SSE42: Found an estimated cost of 7 for instruction:   %ctpop
; AVX: Found an estimated cost of 7 for instruction:   %ctpop
; AVX512: Found an estimated cost of 7 for instruction:   %ctpop
  %ctpop = call <2 x i64> @llvm.ctpop.v2i64(<2 x i64> %a)
  ret <2 x i64> %ctpop
}

define <4 x i64> @var_ctpop_v4i64(<4 x i64> %a) {
; CHECK: 'Cost Model Analysis' for function 'var_ctpop_v4i64':
; SSE2: Found an estimated cost of 24 for instruction:   %ctpop
; SSE42: Found an estimated cost of 14 for instruction:   %ctpop
; AVX1: Found an estimated cost of 16 for instruction:   %ctpop
; AVX2: Found an estimated cost of 7 for instruction:   %ctpop
; AVX512: Found an estimated cost of 7 for instruction:   %ctpop
  %ctpop = call <4 x i64> @llvm.ctpop.v4i64(<4 x i64> %a)
  ret <4 x i64> %ctpop
}

define <8 x i64> @var_ctpop_v8i64(<8 x i64> %a) {
; CHECK: 'Cost Model Analysis' for function 'var_ctpop_v8i64':
; SSE2: Found an estimated cost of 48 for instruction:   %ctpop
; SSE42: Found an estimated cost of 28 for instruction:   %ctpop
; AVX1: Found an estimated cost of 32 for instruction:   %ctpop
; AVX2: Found an estimated cost of 14 for instruction:   %ctpop
; AVX512F: Found an estimated cost of 16 for instruction:   %ctpop
; AVX512BW: Found an estimated cost of 7 for instruction:   %ctpop
  %ctpop = call <8 x i64> @llvm.ctpop.v8i64(<8 x i64> %a)
  ret <8 x i64> %ctpop
}

define <4 x i32> @var_ctpop_v4i32(<4 x i32> %a) {
; CHECK: 'Cost Model Analysis' for function 'var_ctpop_v4i32':
; SSE2: Found an estimated cost of 15 for instruction:   %ctpop
; SSE42: Found an estimated cost of 11 for instruction:   %ctpop
; AVX: Found an estimated cost of 11 for instruction:   %ctpop
; AVX512: Found an estimated cost of 11 for instruction:   %ctpop
  %ctpop = call <4 x i32> @llvm.ctpop.v4i32(<4 x i32> %a)
  ret <4 x i32> %ctpop
}

define <8 x i32> @var_ctpop_v8i32(<8 x i32> %a) {
; CHECK: 'Cost Model Analysis' for function 'var_ctpop_v8i32':
; SSE2: Found an estimated cost of 30 for instruction:   %ctpop
; SSE42: Found an estimated cost of 22 for instruction:   %ctpop
; AVX1: Found an estimated cost of 24 for instruction:   %ctpop
; AVX2: Found an estimated cost of 11 for instruction:   %ctpop
; AVX512: Found an estimated cost of 11 for instruction:   %ctpop
  %ctpop = call <8 x i32> @llvm.ctpop.v8i32(<8 x i32> %a)
  ret <8 x i32> %ctpop
}

define <16 x i32> @var_ctpop_v16i32(<16 x i32> %a) {
; CHECK: 'Cost Model Analysis' for function 'var_ctpop_v16i32':
; SSE2: Found an estimated cost of 60 for instruction:   %ctpop
; SSE42: Found an estimated cost of 44 for instruction:   %ctpop
; AVX1: Found an estimated cost of 48 for instruction:   %ctpop
; AVX2: Found an estimated cost of 22 for instruction:   %ctpop
; AVX512F: Found an estimated cost of 24 for instruction:   %ctpop
; AVX512BW: Found an estimated cost of 11 for instruction:   %ctpop
  %ctpop = call <16 x i32> @llvm.ctpop.v16i32(<16 x i32> %a)
  ret <16 x i32> %ctpop
}

define <8 x i16> @var_ctpop_v8i16(<8 x i16> %a) {
; CHECK: 'Cost Model Analysis' for function 'var_ctpop_v8i16':
; SSE2: Found an estimated cost of 13 for instruction:   %ctpop
; SSE42: Found an estimated cost of 9 for instruction:   %ctpop
; AVX: Found an estimated cost of 9 for instruction:   %ctpop
; AVX512: Found an estimated cost of 9 for instruction:   %ctpop
  %ctpop = call <8 x i16> @llvm.ctpop.v8i16(<8 x i16> %a)
  ret <8 x i16> %ctpop
}

define <16 x i16> @var_ctpop_v16i16(<16 x i16> %a) {
; CHECK: 'Cost Model Analysis' for function 'var_ctpop_v16i16':
; SSE2: Found an estimated cost of 26 for instruction:   %ctpop
; SSE42: Found an estimated cost of 18 for instruction:   %ctpop
; AVX1: Found an estimated cost of 20 for instruction:   %ctpop
; AVX2: Found an estimated cost of 9 for instruction:   %ctpop
; AVX512: Found an estimated cost of 9 for instruction:   %ctpop
  %ctpop = call <16 x i16> @llvm.ctpop.v16i16(<16 x i16> %a)
  ret <16 x i16> %ctpop
}

define <32 x i16> @var_ctpop_v32i16(<32 x i16> %a) {
; CHECK: 'Cost Model Analysis' for function 'var_ctpop_v32i16':
; SSE2: Found an estimated cost of 52 for instruction:   %ctpop
; SSE42: Found an estimated cost of 36 for instruction:   %ctpop
; AVX1: Found an estimated cost of 40 for instruction:   %ctpop
; AVX2: Found an estimated cost of 18 for instruction:   %ctpop
; AVX512F: Found an estimated cost of 18 for instruction:   %ctpop
; AVX512BW: Found an estimated cost of 9 for instruction:   %ctpop
  %ctpop = call <32 x i16> @llvm.ctpop.v32i16(<32 x i16> %a)
  ret <32 x i16> %ctpop
}

define <16 x i8> @var_ctpop_v16i8(<16 x i8> %a) {
; CHECK: 'Cost Model Analysis' for function 'var_ctpop_v16i8':
; SSE2: Found an estimated cost of 10 for instruction:   %ctpop
; SSE42: Found an estimated cost of 6 for instruction:   %ctpop
; AVX: Found an estimated cost of 6 for instruction:   %ctpop
; AVX512: Found an estimated cost of 6 for instruction:   %ctpop
  %ctpop = call <16 x i8> @llvm.ctpop.v16i8(<16 x i8> %a)
  ret <16 x i8> %ctpop
}

define <32 x i8> @var_ctpop_v32i8(<32 x i8> %a) {
; CHECK: 'Cost Model Analysis' for function 'var_ctpop_v32i8':
; SSE2: Found an estimated cost of 20 for instruction:   %ctpop
; SSE42: Found an estimated cost of 12 for instruction:   %ctpop
; AVX1: Found an estimated cost of 14 for instruction:   %ctpop
; AVX2: Found an estimated cost of 6 for instruction:   %ctpop
; AVX512: Found an estimated cost of 6 for instruction:   %ctpop
  %ctpop = call <32 x i8> @llvm.ctpop.v32i8(<32 x i8> %a)
  ret <32 x i8> %ctpop
}

define <64 x i8> @var_ctpop_v64i8(<64 x i8> %a) {
; CHECK: 'Cost Model Analysis' for function 'var_ctpop_v64i8':
; SSE2: Found an estimated cost of 40 for instruction:   %ctpop
; SSE42: Found an estimated cost of 24 for instruction:   %ctpop
; AVX1: Found an estimated cost of 28 for instruction:   %ctpop
; AVX2: Found an estimated cost of 12 for instruction:   %ctpop
; AVX512F: Found an estimated cost of 12 for instruction:   %ctpop
; AVX512BW: Found an estimated cost of 6 for instruction:   %ctpop
  %ctpop = call <64 x i8> @llvm.ctpop.v64i8(<64 x i8> %a)
  ret <64 x i8> %ctpop
}
