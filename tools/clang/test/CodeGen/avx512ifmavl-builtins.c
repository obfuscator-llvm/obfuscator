// RUN: %clang_cc1 %s -triple=x86_64-apple-darwin -target-feature +avx512ifma -target-feature +avx512vl -emit-llvm -o - -Wall -Werror | FileCheck %s

#define __MM_MALLOC_H

#include <immintrin.h>

__m128i test_mm_madd52hi_epu64(__m128i __X, __m128i __Y, __m128i __Z) {
  // CHECK-LABEL: @test_mm_madd52hi_epu64
  // CHECK: @llvm.x86.avx512.mask.vpmadd52h.uq.128
  return _mm_madd52hi_epu64(__X, __Y, __Z); 
}

__m128i test_mm_mask_madd52hi_epu64(__m128i __W, __mmask8 __M, __m128i __X, __m128i __Y) {
  // CHECK-LABEL: @test_mm_mask_madd52hi_epu64
  // CHECK: @llvm.x86.avx512.mask.vpmadd52h.uq.128
  return _mm_mask_madd52hi_epu64(__W, __M, __X, __Y); 
}

__m128i test_mm_maskz_madd52hi_epu64(__mmask8 __M, __m128i __X, __m128i __Y, __m128i __Z) {
  // CHECK-LABEL: @test_mm_maskz_madd52hi_epu64
  // CHECK: @llvm.x86.avx512.maskz.vpmadd52h.uq.128
  return _mm_maskz_madd52hi_epu64(__M, __X, __Y, __Z); 
}

__m256i test_mm256_madd52hi_epu64(__m256i __X, __m256i __Y, __m256i __Z) {
  // CHECK-LABEL: @test_mm256_madd52hi_epu64
  // CHECK: @llvm.x86.avx512.mask.vpmadd52h.uq.256
  return _mm256_madd52hi_epu64(__X, __Y, __Z); 
}

__m256i test_mm256_mask_madd52hi_epu64(__m256i __W, __mmask8 __M, __m256i __X, __m256i __Y) {
  // CHECK-LABEL: @test_mm256_mask_madd52hi_epu64
  // CHECK: @llvm.x86.avx512.mask.vpmadd52h.uq.256
  return _mm256_mask_madd52hi_epu64(__W, __M, __X, __Y); 
}

__m256i test_mm256_maskz_madd52hi_epu64(__mmask8 __M, __m256i __X, __m256i __Y, __m256i __Z) {
  // CHECK-LABEL: @test_mm256_maskz_madd52hi_epu64
  // CHECK: @llvm.x86.avx512.maskz.vpmadd52h.uq.256
  return _mm256_maskz_madd52hi_epu64(__M, __X, __Y, __Z); 
}

__m128i test_mm_madd52lo_epu64(__m128i __X, __m128i __Y, __m128i __Z) {
  // CHECK-LABEL: @test_mm_madd52lo_epu64
  // CHECK: @llvm.x86.avx512.mask.vpmadd52l.uq.128
  return _mm_madd52lo_epu64(__X, __Y, __Z); 
}

__m128i test_mm_mask_madd52lo_epu64(__m128i __W, __mmask8 __M, __m128i __X, __m128i __Y) {
  // CHECK-LABEL: @test_mm_mask_madd52lo_epu64
  // CHECK: @llvm.x86.avx512.mask.vpmadd52l.uq.128
  return _mm_mask_madd52lo_epu64(__W, __M, __X, __Y); 
}

__m128i test_mm_maskz_madd52lo_epu64(__mmask8 __M, __m128i __X, __m128i __Y, __m128i __Z) {
  // CHECK-LABEL: @test_mm_maskz_madd52lo_epu64
  // CHECK: @llvm.x86.avx512.maskz.vpmadd52l.uq.128
  return _mm_maskz_madd52lo_epu64(__M, __X, __Y, __Z); 
}

__m256i test_mm256_madd52lo_epu64(__m256i __X, __m256i __Y, __m256i __Z) {
  // CHECK-LABEL: @test_mm256_madd52lo_epu64
  // CHECK: @llvm.x86.avx512.mask.vpmadd52l.uq.256
  return _mm256_madd52lo_epu64(__X, __Y, __Z); 
}

__m256i test_mm256_mask_madd52lo_epu64(__m256i __W, __mmask8 __M, __m256i __X, __m256i __Y) {
  // CHECK-LABEL: @test_mm256_mask_madd52lo_epu64
  // CHECK: @llvm.x86.avx512.mask.vpmadd52l.uq.256
  return _mm256_mask_madd52lo_epu64(__W, __M, __X, __Y); 
}

__m256i test_mm256_maskz_madd52lo_epu64(__mmask8 __M, __m256i __X, __m256i __Y, __m256i __Z) {
  // CHECK-LABEL: @test_mm256_maskz_madd52lo_epu64
  // CHECK: @llvm.x86.avx512.mask.vpmadd52l.uq.256
  return _mm256_maskz_madd52lo_epu64(__M, __X, __Y, __Z); 
}
