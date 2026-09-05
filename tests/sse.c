/*
 * Rashid - run Intel (x86_64) macOS applications on Apple Silicon
 * Copyright 2026 Toritan123
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
// SSE. Measured against compiler-generated x86_64 code, SIMD is dominated by
// 16-byte moves and scalar double arithmetic; XMM is also unavoidable simply
// because the x86_64 ABI passes floating-point arguments in xmm0-7.
//
// Exits with the number of checks that passed, so the value is meaningful on
// its own and still comparable against a native run.
#include <emmintrin.h>

static long sys3(long n, long a, long b, long c) {
    long ret;
    __asm__ volatile("syscall" : "=a"(ret)
                     : "a"(n), "D"(a), "S"(b), "d"(c) : "rcx", "r11", "memory");
    return ret;
}

static volatile double da, db;
static volatile float  fa, fb;
static volatile long   ia;

typedef struct { long x, y; } pair16;
static pair16 src16 = { 0x1111, 0x2222 }, dst16;

void start(void) {
    long ok = 0;

    da = 3.5; db = 2.0;
    if (da + db == 5.5)  ok++;            // addsd
    if (da * db == 7.0)  ok++;            // mulsd
    if (da - db == 1.5)  ok++;            // subsd
    if (da / db == 1.75) ok++;            // divsd
    if (da > db)         ok++;            // ucomisd, above
    if (!(da < db))      ok++;            // ucomisd, below
    if (da != db)        ok++;            // ucomisd, equality

    ia = 42;  da = (double)ia;            // cvtsi2sd
    if (da == 42.0) ok++;
    db = 3.9;
    if ((long)db == 3) ok++;              // cvttsd2si truncates toward zero
    db = -3.9;
    if ((long)db == -3) ok++;

    fa = 1.5f; fb = 2.5f;
    if (fa + fb == 4.0f) ok++;            // addss
    if (fa * fb == 3.75f) ok++;           // mulss
    da = (double)fa;                       // cvtss2sd
    if (da == 1.5) ok++;
    fb = (float)3.5;                       // cvtsd2ss
    if (fb == 3.5f) ok++;

    da = 16.0;
    if (__builtin_sqrt(da) == 4.0) ok++;   // sqrtsd

    dst16 = src16;                         // 16-byte copy: movups/movaps
    if (dst16.x == 0x1111 && dst16.y == 0x2222) ok++;

    __m128i v = _mm_set1_epi8(0x41);
    __m128i w = _mm_set1_epi8(0x41);
    if (_mm_movemask_epi8(_mm_cmpeq_epi8(v, w)) == 0xffff) ok++;   // pcmpeqb+pmovmskb
    w = _mm_set1_epi8(0x42);
    if (_mm_movemask_epi8(_mm_cmpeq_epi8(v, w)) == 0x0000) ok++;
    if (_mm_movemask_epi8(_mm_xor_si128(v, v)) == 0x0000) ok++;    // pxor

    __m128i z = _mm_setzero_si128();                                // xorps/pxor
    if (_mm_movemask_epi8(_mm_cmpeq_epi8(z, _mm_set1_epi8(0))) == 0xffff) ok++;

    long moved = _mm_cvtsi128_si64(_mm_cvtsi64_si128(0x123456789abcdefL));
    if (moved == 0x123456789abcdefL) ok++;                          // movq to/from xmm

    sys3(0x2000001, ok, 0, 0);
    __builtin_unreachable();
}
