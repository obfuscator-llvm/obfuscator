; NOTE: Assertions have been autogenerated by utils/update_llc_test_checks.py
; RUN: llc < %s -mtriple=x86_64-unknown-linux-gnu -mattr=sse4.1 | FileCheck %s --check-prefix=SSE
; RUN: llc < %s -mtriple=x86_64-unknown-linux-gnu -mattr=avx2 | FileCheck %s --check-prefix=AVX2
; RUN: llc < %s -mtriple=x86_64-unknown-linux-gnu -mattr=avx512f | FileCheck %s --check-prefix=AVX512

; Verify that we don't scalarize a packed vector shift left of 16-bit
; signed integers if the amount is a constant build_vector.
; Check that we produce a SSE2 packed integer multiply (pmullw) instead.

define <8 x i16> @test1(<8 x i16> %a) {
; SSE-LABEL: test1:
; SSE:       # BB#0:
; SSE-NEXT:    pmullw {{.*}}(%rip), %xmm0
; SSE-NEXT:    retq
;
; AVX2-LABEL: test1:
; AVX2:       # BB#0:
; AVX2-NEXT:    vpmullw {{.*}}(%rip), %xmm0, %xmm0
; AVX2-NEXT:    retq
;
; AVX512-LABEL: test1:
; AVX512:       # BB#0:
; AVX512-NEXT:    vpmullw {{.*}}(%rip), %xmm0, %xmm0
; AVX512-NEXT:    retq
  %shl = shl <8 x i16> %a, <i16 1, i16 1, i16 2, i16 3, i16 7, i16 0, i16 9, i16 11>
  ret <8 x i16> %shl
}

define <8 x i16> @test2(<8 x i16> %a) {
; SSE-LABEL: test2:
; SSE:       # BB#0:
; SSE-NEXT:    pmullw {{.*}}(%rip), %xmm0
; SSE-NEXT:    retq
;
; AVX2-LABEL: test2:
; AVX2:       # BB#0:
; AVX2-NEXT:    vpmullw {{.*}}(%rip), %xmm0, %xmm0
; AVX2-NEXT:    retq
;
; AVX512-LABEL: test2:
; AVX512:       # BB#0:
; AVX512-NEXT:    vpmullw {{.*}}(%rip), %xmm0, %xmm0
; AVX512-NEXT:    retq
  %shl = shl <8 x i16> %a, <i16 0, i16 undef, i16 0, i16 0, i16 1, i16 undef, i16 -1, i16 1>
  ret <8 x i16> %shl
}

; Verify that a vector shift left of 32-bit signed integers is simply expanded
; into a SSE4.1 pmulld (instead of cvttps2dq + pmulld) if the vector of shift
; counts is a constant build_vector.

define <4 x i32> @test3(<4 x i32> %a) {
; SSE-LABEL: test3:
; SSE:       # BB#0:
; SSE-NEXT:    pmulld {{.*}}(%rip), %xmm0
; SSE-NEXT:    retq
;
; AVX2-LABEL: test3:
; AVX2:       # BB#0:
; AVX2-NEXT:    vpsllvd {{.*}}(%rip), %xmm0, %xmm0
; AVX2-NEXT:    retq
;
; AVX512-LABEL: test3:
; AVX512:       # BB#0:
; AVX512-NEXT:    vpsllvd {{.*}}(%rip), %xmm0, %xmm0
; AVX512-NEXT:    retq
  %shl = shl <4 x i32> %a, <i32 1, i32 -1, i32 2, i32 -3>
  ret <4 x i32> %shl
}

define <4 x i32> @test4(<4 x i32> %a) {
; SSE-LABEL: test4:
; SSE:       # BB#0:
; SSE-NEXT:    pmulld {{.*}}(%rip), %xmm0
; SSE-NEXT:    retq
;
; AVX2-LABEL: test4:
; AVX2:       # BB#0:
; AVX2-NEXT:    vpsllvd {{.*}}(%rip), %xmm0, %xmm0
; AVX2-NEXT:    retq
;
; AVX512-LABEL: test4:
; AVX512:       # BB#0:
; AVX512-NEXT:    vpsllvd {{.*}}(%rip), %xmm0, %xmm0
; AVX512-NEXT:    retq
  %shl = shl <4 x i32> %a, <i32 0, i32 0, i32 1, i32 1>
  ret <4 x i32> %shl
}

; If we have AVX/SSE2 but not AVX2, verify that the following shift is split
; into two pmullw instructions. With AVX2, the test case below would produce
; a single vpmullw.

define <16 x i16> @test5(<16 x i16> %a) {
; SSE-LABEL: test5:
; SSE:       # BB#0:
; SSE-NEXT:    movdqa {{.*#+}} xmm2 = [2,2,4,8,128,1,512,2048]
; SSE-NEXT:    pmullw %xmm2, %xmm0
; SSE-NEXT:    pmullw %xmm2, %xmm1
; SSE-NEXT:    retq
;
; AVX2-LABEL: test5:
; AVX2:       # BB#0:
; AVX2-NEXT:    vpmullw {{.*}}(%rip), %ymm0, %ymm0
; AVX2-NEXT:    retq
;
; AVX512-LABEL: test5:
; AVX512:       # BB#0:
; AVX512-NEXT:    vpmullw {{.*}}(%rip), %ymm0, %ymm0
; AVX512-NEXT:    retq
  %shl = shl <16 x i16> %a, <i16 1, i16 1, i16 2, i16 3, i16 7, i16 0, i16 9, i16 11, i16 1, i16 1, i16 2, i16 3, i16 7, i16 0, i16 9, i16 11>
  ret <16 x i16> %shl
}

; If we have AVX/SSE4.1 but not AVX2, verify that the following shift is split
; into two pmulld instructions. With AVX2, the test case below would produce
; a single vpsllvd instead.

define <8 x i32> @test6(<8 x i32> %a) {
; SSE-LABEL: test6:
; SSE:       # BB#0:
; SSE-NEXT:    movdqa {{.*#+}} xmm2 = [2,2,4,8]
; SSE-NEXT:    pmulld %xmm2, %xmm0
; SSE-NEXT:    pmulld %xmm2, %xmm1
; SSE-NEXT:    retq
;
; AVX2-LABEL: test6:
; AVX2:       # BB#0:
; AVX2-NEXT:    vpsllvd {{.*}}(%rip), %ymm0, %ymm0
; AVX2-NEXT:    retq
;
; AVX512-LABEL: test6:
; AVX512:       # BB#0:
; AVX512-NEXT:    vpsllvd {{.*}}(%rip), %ymm0, %ymm0
; AVX512-NEXT:    retq
  %shl = shl <8 x i32> %a, <i32 1, i32 1, i32 2, i32 3, i32 1, i32 1, i32 2, i32 3>
  ret <8 x i32> %shl
}

; With AVX2 and AVX512, the test case below should produce a sequence of
; two vpmullw instructions. On SSE2 instead, we split the shift in four
; parts and then we convert each part into a pmullw.

define <32 x i16> @test7(<32 x i16> %a) {
; SSE-LABEL: test7:
; SSE:       # BB#0:
; SSE-NEXT:    movdqa {{.*#+}} xmm4 = [2,2,4,8,128,1,512,2048]
; SSE-NEXT:    pmullw %xmm4, %xmm0
; SSE-NEXT:    pmullw %xmm4, %xmm1
; SSE-NEXT:    pmullw %xmm4, %xmm2
; SSE-NEXT:    pmullw %xmm4, %xmm3
; SSE-NEXT:    retq
;
; AVX2-LABEL: test7:
; AVX2:       # BB#0:
; AVX2-NEXT:    vbroadcasti128 {{.*#+}} ymm2 = [2,2,4,8,128,1,512,2048,2,2,4,8,128,1,512,2048]
; AVX2-NEXT:    # ymm2 = mem[0,1,0,1]
; AVX2-NEXT:    vpmullw %ymm2, %ymm0, %ymm0
; AVX2-NEXT:    vpmullw %ymm2, %ymm1, %ymm1
; AVX2-NEXT:    retq
;
; AVX512-LABEL: test7:
; AVX512:       # BB#0:
; AVX512-NEXT:    vbroadcasti128 {{.*#+}} ymm2 = [2,2,4,8,128,1,512,2048,2,2,4,8,128,1,512,2048]
; AVX512-NEXT:    # ymm2 = mem[0,1,0,1]
; AVX512-NEXT:    vpmullw %ymm2, %ymm0, %ymm0
; AVX512-NEXT:    vpmullw %ymm2, %ymm1, %ymm1
; AVX512-NEXT:    retq
  %shl = shl <32 x i16> %a, <i16 1, i16 1, i16 2, i16 3, i16 7, i16 0, i16 9, i16 11, i16 1, i16 1, i16 2, i16 3, i16 7, i16 0, i16 9, i16 11, i16 1, i16 1, i16 2, i16 3, i16 7, i16 0, i16 9, i16 11, i16 1, i16 1, i16 2, i16 3, i16 7, i16 0, i16 9, i16 11>
  ret <32 x i16> %shl
}

; Similar to test7; the difference is that with AVX512 support
; we only produce a single vpsllvd/vpsllvq instead of a pair of vpsllvd/vpsllvq.

define <16 x i32> @test8(<16 x i32> %a) {
; SSE-LABEL: test8:
; SSE:       # BB#0:
; SSE-NEXT:    movdqa {{.*#+}} xmm4 = [2,2,4,8]
; SSE-NEXT:    pmulld %xmm4, %xmm0
; SSE-NEXT:    pmulld %xmm4, %xmm1
; SSE-NEXT:    pmulld %xmm4, %xmm2
; SSE-NEXT:    pmulld %xmm4, %xmm3
; SSE-NEXT:    retq
;
; AVX2-LABEL: test8:
; AVX2:       # BB#0:
; AVX2-NEXT:    vbroadcasti128 {{.*#+}} ymm2 = [1,1,2,3,1,1,2,3]
; AVX2-NEXT:    # ymm2 = mem[0,1,0,1]
; AVX2-NEXT:    vpsllvd %ymm2, %ymm0, %ymm0
; AVX2-NEXT:    vpsllvd %ymm2, %ymm1, %ymm1
; AVX2-NEXT:    retq
;
; AVX512-LABEL: test8:
; AVX512:       # BB#0:
; AVX512-NEXT:    vpsllvd {{.*}}(%rip), %zmm0, %zmm0
; AVX512-NEXT:    retq
  %shl = shl <16 x i32> %a, <i32 1, i32 1, i32 2, i32 3, i32 1, i32 1, i32 2, i32 3, i32 1, i32 1, i32 2, i32 3, i32 1, i32 1, i32 2, i32 3>
  ret <16 x i32> %shl
}

; The shift from 'test9' gets shifted separately and blended if we don't have AVX2/AVX512f support.

define <8 x i64> @test9(<8 x i64> %a) {
; SSE-LABEL: test9:
; SSE:       # BB#0:
; SSE-NEXT:    movdqa %xmm1, %xmm4
; SSE-NEXT:    psllq $3, %xmm4
; SSE-NEXT:    psllq $2, %xmm1
; SSE-NEXT:    pblendw {{.*#+}} xmm1 = xmm1[0,1,2,3],xmm4[4,5,6,7]
; SSE-NEXT:    movdqa %xmm3, %xmm4
; SSE-NEXT:    psllq $3, %xmm4
; SSE-NEXT:    psllq $2, %xmm3
; SSE-NEXT:    pblendw {{.*#+}} xmm3 = xmm3[0,1,2,3],xmm4[4,5,6,7]
; SSE-NEXT:    paddq %xmm0, %xmm0
; SSE-NEXT:    paddq %xmm2, %xmm2
; SSE-NEXT:    retq
;
; AVX2-LABEL: test9:
; AVX2:       # BB#0:
; AVX2-NEXT:    vmovdqa {{.*#+}} ymm2 = [1,1,2,3]
; AVX2-NEXT:    vpsllvq %ymm2, %ymm0, %ymm0
; AVX2-NEXT:    vpsllvq %ymm2, %ymm1, %ymm1
; AVX2-NEXT:    retq
;
; AVX512-LABEL: test9:
; AVX512:       # BB#0:
; AVX512-NEXT:    vpsllvq {{.*}}(%rip), %zmm0, %zmm0
; AVX512-NEXT:    retq
  %shl = shl <8 x i64> %a, <i64 1, i64 1, i64 2, i64 3, i64 1, i64 1, i64 2, i64 3>
  ret <8 x i64> %shl
}
