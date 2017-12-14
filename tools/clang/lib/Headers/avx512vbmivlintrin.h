/*===------------- avx512vbmivlintrin.h - VBMI intrinsics ------------------===
 *
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 *
 *===-----------------------------------------------------------------------===
 */
#ifndef __IMMINTRIN_H
#error "Never use <avx512vbmivlintrin.h> directly; include <immintrin.h> instead."
#endif

#ifndef __VBMIVLINTRIN_H
#define __VBMIVLINTRIN_H

/* Define the default attributes for the functions in this file. */
#define __DEFAULT_FN_ATTRS __attribute__((__always_inline__, __nodebug__, __target__("avx512vbmi,avx512vl")))


static __inline__ __m128i __DEFAULT_FN_ATTRS
_mm_mask2_permutex2var_epi8 (__m128i __A, __m128i __I, __mmask16 __U,
            __m128i __B)
{
  return (__m128i) __builtin_ia32_vpermi2varqi128_mask ((__v16qi) __A,
              (__v16qi) __I
              /* idx */ ,
              (__v16qi) __B,
              (__mmask16)
              __U);
}

static __inline__ __m256i __DEFAULT_FN_ATTRS
_mm256_mask2_permutex2var_epi8 (__m256i __A, __m256i __I,
         __mmask32 __U, __m256i __B)
{
  return (__m256i) __builtin_ia32_vpermi2varqi256_mask ((__v32qi) __A,
              (__v32qi) __I
              /* idx */ ,
              (__v32qi) __B,
              (__mmask32)
              __U);
}

static __inline__ __m128i __DEFAULT_FN_ATTRS
_mm_permutex2var_epi8 (__m128i __A, __m128i __I, __m128i __B)
{
  return (__m128i) __builtin_ia32_vpermt2varqi128_mask ((__v16qi) __I
              /* idx */ ,
              (__v16qi) __A,
              (__v16qi) __B,
              (__mmask16) -
              1);
}

static __inline__ __m128i __DEFAULT_FN_ATTRS
_mm_mask_permutex2var_epi8 (__m128i __A, __mmask16 __U, __m128i __I,
           __m128i __B)
{
  return (__m128i) __builtin_ia32_vpermt2varqi128_mask ((__v16qi) __I
              /* idx */ ,
              (__v16qi) __A,
              (__v16qi) __B,
              (__mmask16)
              __U);
}

static __inline__ __m128i __DEFAULT_FN_ATTRS
_mm_maskz_permutex2var_epi8 (__mmask16 __U, __m128i __A, __m128i __I,
            __m128i __B)
{
  return (__m128i) __builtin_ia32_vpermt2varqi128_maskz ((__v16qi) __I
               /* idx */ ,
               (__v16qi) __A,
               (__v16qi) __B,
               (__mmask16)
               __U);
}

static __inline__ __m256i __DEFAULT_FN_ATTRS
_mm256_permutex2var_epi8 (__m256i __A, __m256i __I, __m256i __B)
{
  return (__m256i) __builtin_ia32_vpermt2varqi256_mask ((__v32qi) __I
              /* idx */ ,
              (__v32qi) __A,
              (__v32qi) __B,
              (__mmask32) -
              1);
}

static __inline__ __m256i __DEFAULT_FN_ATTRS
_mm256_mask_permutex2var_epi8 (__m256i __A, __mmask32 __U,
        __m256i __I, __m256i __B)
{
  return (__m256i) __builtin_ia32_vpermt2varqi256_mask ((__v32qi) __I
              /* idx */ ,
              (__v32qi) __A,
              (__v32qi) __B,
              (__mmask32)
              __U);
}

static __inline__ __m256i __DEFAULT_FN_ATTRS
_mm256_maskz_permutex2var_epi8 (__mmask32 __U, __m256i __A,
         __m256i __I, __m256i __B)
{
  return (__m256i) __builtin_ia32_vpermt2varqi256_maskz ((__v32qi) __I
               /* idx */ ,
               (__v32qi) __A,
               (__v32qi) __B,
               (__mmask32)
               __U);
}

static __inline__ __m128i __DEFAULT_FN_ATTRS
_mm_permutexvar_epi8 (__m128i __A, __m128i __B)
{
  return (__m128i) __builtin_ia32_permvarqi128_mask ((__v16qi) __B,
                 (__v16qi) __A,
                 (__v16qi) _mm_undefined_si128 (),
                 (__mmask16) -1);
}

static __inline__ __m128i __DEFAULT_FN_ATTRS
_mm_maskz_permutexvar_epi8 (__mmask16 __M, __m128i __A, __m128i __B)
{
  return (__m128i) __builtin_ia32_permvarqi128_mask ((__v16qi) __B,
                 (__v16qi) __A,
                 (__v16qi) _mm_setzero_si128 (),
                 (__mmask16) __M);
}

static __inline__ __m128i __DEFAULT_FN_ATTRS
_mm_mask_permutexvar_epi8 (__m128i __W, __mmask16 __M, __m128i __A,
          __m128i __B)
{
  return (__m128i) __builtin_ia32_permvarqi128_mask ((__v16qi) __B,
                 (__v16qi) __A,
                 (__v16qi) __W,
                 (__mmask16) __M);
}

static __inline__ __m256i __DEFAULT_FN_ATTRS
_mm256_permutexvar_epi8 (__m256i __A, __m256i __B)
{
  return (__m256i) __builtin_ia32_permvarqi256_mask ((__v32qi) __B,
                 (__v32qi) __A,
                 (__v32qi) _mm256_undefined_si256 (),
                 (__mmask32) -1);
}

static __inline__ __m256i __DEFAULT_FN_ATTRS
_mm256_maskz_permutexvar_epi8 (__mmask32 __M, __m256i __A,
        __m256i __B)
{
  return (__m256i) __builtin_ia32_permvarqi256_mask ((__v32qi) __B,
                 (__v32qi) __A,
                 (__v32qi) _mm256_setzero_si256 (),
                 (__mmask32) __M);
}

static __inline__ __m256i __DEFAULT_FN_ATTRS
_mm256_mask_permutexvar_epi8 (__m256i __W, __mmask32 __M, __m256i __A,
             __m256i __B)
{
  return (__m256i) __builtin_ia32_permvarqi256_mask ((__v32qi) __B,
                 (__v32qi) __A,
                 (__v32qi) __W,
                 (__mmask32) __M);
}

static __inline__ __m128i __DEFAULT_FN_ATTRS
_mm_mask_multishift_epi64_epi8 (__m128i __W, __mmask16 __M, __m128i __X, __m128i __Y)
{
  return (__m128i) __builtin_ia32_vpmultishiftqb128_mask ((__v16qi) __X,
                (__v16qi) __Y,
                (__v16qi) __W,
                (__mmask16) __M);
}

static __inline__ __m128i __DEFAULT_FN_ATTRS
_mm_maskz_multishift_epi64_epi8 (__mmask16 __M, __m128i __X, __m128i __Y)
{
  return (__m128i) __builtin_ia32_vpmultishiftqb128_mask ((__v16qi) __X,
                (__v16qi) __Y,
                (__v16qi)
                _mm_setzero_si128 (),
                (__mmask16) __M);
}

static __inline__ __m128i __DEFAULT_FN_ATTRS
_mm_multishift_epi64_epi8 (__m128i __X, __m128i __Y)
{
  return (__m128i) __builtin_ia32_vpmultishiftqb128_mask ((__v16qi) __X,
                (__v16qi) __Y,
                (__v16qi)
                _mm_undefined_si128 (),
                (__mmask16) -1);
}

static __inline__ __m256i __DEFAULT_FN_ATTRS
_mm256_mask_multishift_epi64_epi8 (__m256i __W, __mmask32 __M, __m256i __X, __m256i __Y)
{
  return (__m256i) __builtin_ia32_vpmultishiftqb256_mask ((__v32qi) __X,
                (__v32qi) __Y,
                (__v32qi) __W,
                (__mmask32) __M);
}

static __inline__ __m256i __DEFAULT_FN_ATTRS
_mm256_maskz_multishift_epi64_epi8 (__mmask32 __M, __m256i __X, __m256i __Y)
{
  return (__m256i) __builtin_ia32_vpmultishiftqb256_mask ((__v32qi) __X,
                (__v32qi) __Y,
                (__v32qi)
                _mm256_setzero_si256 (),
                (__mmask32) __M);
}

static __inline__ __m256i __DEFAULT_FN_ATTRS
_mm256_multishift_epi64_epi8 (__m256i __X, __m256i __Y)
{
  return (__m256i) __builtin_ia32_vpmultishiftqb256_mask ((__v32qi) __X,
                (__v32qi) __Y,
                (__v32qi)
                _mm256_undefined_si256 (),
                (__mmask32) -1);
}


#undef __DEFAULT_FN_ATTRS

#endif
