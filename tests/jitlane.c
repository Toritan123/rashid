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
// XMM lanes kept in host registers.
//
// Within a block, compiled code keeps the low 64 bits of recently used XMM
// registers in d8..d15 as well as in memory. Every way an XMM register can
// change has to keep that copy right, and each sequence here changes one in
// a way that leaves the copy stale if it is not, then reads it back as a
// double in the same block. Each is a single asm statement, so the
// instructions are exactly these and in this order.
#include <stdio.h>
#include <string.h>

static unsigned long mix(unsigned long h, unsigned long v) {
    h ^= v;
    h *= 0x100000001b3UL;
    return h ^ (h >> 29);
}
static unsigned long mixd(unsigned long h, double d) {
    unsigned long v;
    memcpy(&v, &d, 8);
    return mix(h, v);
}

__attribute__((aligned(16))) static double in[16] = {
    1.5, -2.25, 3.125, 1e10, -7.0, 0.1, 6.5, 1e-3,
    2.0, 9.75, -0.5, 4.0, 11.0, -13.5, 0.25, 8.0 };
__attribute__((aligned(16))) static double pair[2] = { 123.0, 456.0 };
static double out[16];

int main(void) {
    unsigned long h = 0xcbf29ce484222325UL;
    for (int round = 0; round < 3; round++) {
        // A 128-bit load from memory into a register whose lane is kept.
        __asm__ volatile(
            "movsd 0(%0), %%xmm0\n\t movsd 8(%0), %%xmm1\n\t"
            "addsd %%xmm1, %%xmm0\n\t"          // xmm0's lane now kept
            "movapd (%1), %%xmm0\n\t"           // replaced from memory
            "addsd %%xmm1, %%xmm0\n\t"
            "movsd %%xmm0, 0(%2)"
            : : "r"(in), "r"(pair), "r"(out) : "xmm0", "xmm1", "memory");
        h = mixd(h, out[0]);

        // andnpd with both lanes kept: ~dst & src, not the other way round.
        __asm__ volatile(
            "movsd 16(%0), %%xmm2\n\t movsd 24(%0), %%xmm3\n\t movsd 32(%0), %%xmm4\n\t"
            "addsd %%xmm4, %%xmm2\n\t addsd %%xmm4, %%xmm3\n\t"
            "andnpd %%xmm2, %%xmm3\n\t"
            "addsd %%xmm3, %%xmm4\n\t"
            "movsd %%xmm3, 8(%1)\n\t movsd %%xmm4, 16(%1)"
            : : "r"(in), "r"(out) : "xmm2", "xmm3", "xmm4", "memory");
        h = mixd(h, out[1]);
        h = mixd(h, out[2]);

        // More registers than there are places to keep them, so some are
        // given up and must then be read from memory.
        __asm__ volatile(
            "movsd 0(%0), %%xmm0\n\t movsd 8(%0), %%xmm1\n\t movsd 16(%0), %%xmm2\n\t"
            "movsd 24(%0), %%xmm3\n\t movsd 32(%0), %%xmm4\n\t movsd 40(%0), %%xmm5\n\t"
            "movsd 48(%0), %%xmm6\n\t movsd 56(%0), %%xmm7\n\t movsd 64(%0), %%xmm8\n\t"
            "movsd 72(%0), %%xmm9\n\t movsd 80(%0), %%xmm10\n\t movsd 88(%0), %%xmm11\n\t"
            "addsd %%xmm1, %%xmm0\n\t addsd %%xmm2, %%xmm1\n\t addsd %%xmm3, %%xmm2\n\t"
            "addsd %%xmm4, %%xmm3\n\t addsd %%xmm5, %%xmm4\n\t addsd %%xmm6, %%xmm5\n\t"
            "addsd %%xmm7, %%xmm6\n\t addsd %%xmm8, %%xmm7\n\t addsd %%xmm9, %%xmm8\n\t"
            "addsd %%xmm10, %%xmm9\n\t addsd %%xmm11, %%xmm10\n\t addsd %%xmm0, %%xmm11\n\t"
            "mulsd %%xmm11, %%xmm0\n\t mulsd %%xmm10, %%xmm1\n\t mulsd %%xmm9, %%xmm2\n\t"
            "mulsd %%xmm8, %%xmm3\n\t mulsd %%xmm7, %%xmm4\n\t mulsd %%xmm6, %%xmm5\n\t"
            "movsd %%xmm0, 24(%1)\n\t movsd %%xmm1, 32(%1)\n\t movsd %%xmm2, 40(%1)\n\t"
            "movsd %%xmm3, 48(%1)\n\t movsd %%xmm4, 56(%1)\n\t movsd %%xmm5, 64(%1)"
            : : "r"(in), "r"(out)
            : "xmm0", "xmm1", "xmm2", "xmm3", "xmm4", "xmm5", "xmm6", "xmm7",
              "xmm8", "xmm9", "xmm10", "xmm11", "memory");
        for (int k = 3; k < 9; k++) h = mixd(h, out[k]);

        // A single-precision result replaces only the low 32 bits of a lane
        // that held a double; the other 32 are still the double's.
        __asm__ volatile(
            "movsd 0(%0), %%xmm0\n\t movsd 8(%0), %%xmm1\n\t movss 40(%0), %%xmm2\n\t"
            "addsd %%xmm1, %%xmm0\n\t"
            "addss %%xmm2, %%xmm0\n\t"
            "movsd %%xmm0, %%xmm3\n\t"
            "addsd %%xmm1, %%xmm3\n\t"
            "movsd %%xmm3, 72(%1)"
            : : "r"(in), "r"(out) : "xmm0", "xmm1", "xmm2", "xmm3", "memory");
        h = mixd(h, out[9]);

        for (int k = 0; k < 16; k++) in[k] = in[k] * 1.5 - 0.25;
    }
    printf("checksum=%016lx\n", h);
    return (int)(h % 100);
}
