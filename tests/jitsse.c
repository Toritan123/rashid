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
// Scalar floating point through the block compiler.
//
// The values are chosen for the places where x86 and a naive translation part
// company: NaN, both zeros, infinities, and integers that do not fit. min and
// max return their second operand when the inputs are unordered; comparisons
// have eight predicates that treat NaN differently; a conversion that does
// not fit produces the "integer indefinite" value rather than saturating.
//
// Everything is folded into a checksum, compared against native execution,
// and replayed block by block under -J.
#include <emmintrin.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static double dv[] = { 1.5, -2.25, 0.0, -0.0, 1e300, -1e300, 3e9, -3e9,
                       2147483647.5, NAN, INFINITY, -INFINITY, 0.1, 7.0 };
static float  fv[] = { 1.5f, -2.25f, 0.0f, -0.0f, 3e38f, -3e38f, 3e9f,
                       NAN, INFINITY, 0.1f, 7.0f };
static long   iv[] = { 0, 1, -1, 2147483647L, -2147483648L,
                       9223372036854775807L, (long)0x8000000000000000UL, 123456789 };
#define N(a) (sizeof a / sizeof *a)

// The shift matters. Without it a difference confined to the top bit - the
// sign of a double - stays in the top bit forever, and an even number of
// them cancel out. That hid a real NaN sign bug in the first version of
// this test.
static inline unsigned long mix(unsigned long h, unsigned long v) {
    h = (h ^ v) * 0x100000001b3UL;
    return h ^ (h >> 29);
}
static inline unsigned long bd(double x) { unsigned long u; memcpy(&u, &x, 8); return u; }
static inline unsigned long bf(float x)  { unsigned u; memcpy(&u, &x, 4); return u; }

__attribute__((noinline)) static unsigned long doubles(unsigned long h) {
    for (int k = 0; k < 8; k++)
    for (unsigned i = 0; i < N(dv); i++)
    for (unsigned j = 0; j < N(dv); j++) {
        double a = dv[i], b = dv[j];
        h = mix(h, bd(a + b)); h = mix(h, bd(a - b));
        h = mix(h, bd(a * b)); h = mix(h, bd(a / b));
        h = mix(h, bd(a < b ? a : b));                       // minsd
        h = mix(h, bd(a > b ? a : b));                       // maxsd
        h = mix(h, bd(__builtin_sqrt(a)));                   // sqrtsd
        h = mix(h, (unsigned long)((a < b) | (a <= b) << 1 | (a == b) << 2 |
                                   (a != b) << 3 | (a > b) << 4 | (a >= b) << 5));
        h = mix(h, bd(a < b ? 1.0 : 2.0));                   // cmpltsd mask use
    }
    return h;
}

__attribute__((noinline)) static unsigned long floats(unsigned long h) {
    for (int k = 0; k < 8; k++)
    for (unsigned i = 0; i < N(fv); i++)
    for (unsigned j = 0; j < N(fv); j++) {
        float a = fv[i], b = fv[j];
        h = mix(h, bf(a + b)); h = mix(h, bf(a - b));
        h = mix(h, bf(a * b)); h = mix(h, bf(a / b));
        h = mix(h, bf(a < b ? a : b));
        h = mix(h, bf(a > b ? a : b));
        h = mix(h, bf(__builtin_sqrtf(a)));
        h = mix(h, (unsigned long)((a < b) | (a <= b) << 1 | (a == b) << 2 |
                                   (a != b) << 3 | (a > b) << 4 | (a >= b) << 5));
    }
    return h;
}

__attribute__((noinline)) static unsigned long conversions(unsigned long h) {
    for (int k = 0; k < 8; k++) {
        for (unsigned i = 0; i < N(dv); i++) {
            double a = dv[i];
            h = mix(h, (unsigned long)(long)a);              // cvttsd2si 64
            h = mix(h, (unsigned)(int)a);                    // cvttsd2si 32
            h = mix(h, (unsigned long)_mm_cvtsd_si64(_mm_set_sd(a)));  // cvtsd2si
            h = mix(h, bf((float)a));                        // cvtsd2ss
        }
        for (unsigned i = 0; i < N(fv); i++) {
            float f = fv[i];
            h = mix(h, (unsigned long)(long)f);              // cvttss2si
            h = mix(h, (unsigned)(int)f);
            h = mix(h, bd((double)f));                       // cvtss2sd
        }
        for (unsigned i = 0; i < N(iv); i++) {
            h = mix(h, bd((double)iv[i]));                   // cvtsi2sd 64
            h = mix(h, bd((double)(int)iv[i]));              // cvtsi2sd 32
            h = mix(h, bf((float)iv[i]));                    // cvtsi2ss 64
        }
    }
    return h;
}

__attribute__((noinline)) static unsigned long packed(unsigned long h) {
    _Alignas(16) double buf[4] = { 1.0, -2.0, 3.5, -0.0 };
    _Alignas(16) double mask[2] = { -1.25, 0.5 };
    for (int k = 0; k < 64; k++) {
        __m128d x = _mm_load_pd(buf);                       // movapd load
        __m128d m = _mm_load_pd(mask);
        __m128d r = _mm_or_pd(_mm_and_pd(x, m), _mm_andnot_pd(m, x));
        r = _mm_xor_pd(r, _mm_load_pd(buf + 2));
        _mm_store_pd(buf, r);                                // movapd store
        __m128i xi = _mm_castpd_si128(r);
        xi = _mm_xor_si128(xi, _mm_castpd_si128(m));         // pxor
        h = mix(h, (unsigned long)_mm_cvtsi128_si64(xi));
        double lo = buf[0];                                  // movsd load
        buf[1] = lo * 0.5;                                   // movsd store
        h = mix(h, bd(buf[1]));
    }
    return h;
}

// All eight comparison predicates, at both widths. Ordinary C comparisons
// only ever produced eq and neq here, so predicate 2 (le) went untested until
// a mutation turning it into lt went unnoticed.
__attribute__((noinline)) static unsigned long predicates(unsigned long h) {
    for (int k = 0; k < 4; k++)
    for (unsigned i = 0; i < N(dv); i++)
    for (unsigned j = 0; j < N(dv); j++) {
        __m128d a = _mm_set_sd(dv[i]), b = _mm_set_sd(dv[j]);
        __m128d r[8] = { _mm_cmpeq_sd(a, b),    _mm_cmplt_sd(a, b),
                         _mm_cmple_sd(a, b),    _mm_cmpunord_sd(a, b),
                         _mm_cmpneq_sd(a, b),   _mm_cmpnlt_sd(a, b),
                         _mm_cmpnle_sd(a, b),   _mm_cmpord_sd(a, b) };
        for (int p = 0; p < 8; p++)
            h = mix(h, (unsigned long)_mm_cvtsi128_si64(_mm_castpd_si128(r[p])));
    }
    for (int k = 0; k < 4; k++)
    for (unsigned i = 0; i < N(fv); i++)
    for (unsigned j = 0; j < N(fv); j++) {
        __m128 a = _mm_set_ss(fv[i]), b = _mm_set_ss(fv[j]);
        __m128 r[8] = { _mm_cmpeq_ss(a, b),    _mm_cmplt_ss(a, b),
                        _mm_cmple_ss(a, b),    _mm_cmpunord_ss(a, b),
                        _mm_cmpneq_ss(a, b),   _mm_cmpnlt_ss(a, b),
                        _mm_cmpnle_ss(a, b),   _mm_cmpord_ss(a, b) };
        for (int p = 0; p < 8; p++)
            h = mix(h, (unsigned)_mm_cvtsi128_si32(_mm_castps_si128(r[p])));
    }
    return h;
}

int main(void) {
    unsigned long h = 0xcbf29ce484222325UL;
    h = doubles(h);
    h = floats(h);
    h = conversions(h);
    h = packed(h);
    h = predicates(h);
    printf("checksum=%016lx\n", h);
    return (int)(h & 0x7f);
}
