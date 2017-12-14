/*===----------------------- clzerointrin.h - CLZERO ----------------------===
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
#ifndef __X86INTRIN_H
#error "Never use <clzerointrin.h> directly; include <x86intrin.h> instead."
#endif

#ifndef _CLZEROINTRIN_H
#define _CLZEROINTRIN_H

/* Define the default attributes for the functions in this file. */
#define __DEFAULT_FN_ATTRS \
  __attribute__((__always_inline__, __nodebug__,  __target__("clzero")))

/// \brief Loads the cache line address and zero's out the cacheline
///
/// \headerfile <clzerointrin.h>
///
/// This intrinsic corresponds to the <c> CLZERO </c> instruction.
///
/// \param __line
///    A pointer to a cacheline which needs to be zeroed out.
static __inline__ void __DEFAULT_FN_ATTRS
_mm_clzero (void * __line)
{
  __builtin_ia32_clzero ((void *)__line);
}

#undef __DEFAULT_FN_ATTRS 

#endif /* _CLZEROINTRIN_H */
