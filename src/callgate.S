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

// The call gate: enter a native arm64 function with a register and stack
// state we choose.
//
// This is written in assembly for two reasons C cannot cover. A returned
// double lands in d0 and a returned integer in x0, and which one matters
// depends on a signature the caller may not know, so both are captured. And
// variadic arguments on macOS arm64 all go on the stack, so the gate has to
// be able to place an arbitrary block there.
//
// A 16-byte struct comes back in x0:x1 (or d0:d1), so all four are captured;
// the caller decides which ones the signature meant.
//
//   void rsd_call_native(void *fn,
//                        const uint64_t x[8],   // -> x0..x7
//                        const double   d[8],   // -> d0..d7
//                        const void    *stack,  // copied above sp
//                        uint64_t       stackbytes,
//                        uint64_t       ret[4]);// x0, x1, d0 bits, d1 bits
        .text
        .globl  _rsd_call_native
        .p2align 2
_rsd_call_native:
        stp     x29, x30, [sp, #-48]!
        mov     x29, sp
        stp     x19, x20, [sp, #16]
        stp     x21, x22, [sp, #32]

        mov     x19, x0                 // fn
        mov     x20, x5                 // &ret_d
        mov     x21, x1                 // x[]
        mov     x22, x2                 // d[]

        // Reserve the stack block, keeping sp 16-byte aligned.
        add     x4, x4, #15
        and     x4, x4, #-16
        sub     sp, sp, x4
        mov     x9, sp
        mov     x10, x3
        mov     x11, x4
1:      cbz     x11, 2f
        ldr     x12, [x10], #8
        str     x12, [x9], #8
        sub     x11, x11, #8
        b       1b
2:
        ldp     d0, d1, [x22, #0]
        ldp     d2, d3, [x22, #16]
        ldp     d4, d5, [x22, #32]
        ldp     d6, d7, [x22, #48]
        ldp     x0, x1, [x21, #0]
        ldp     x2, x3, [x21, #16]
        ldp     x4, x5, [x21, #32]
        ldp     x6, x7, [x21, #48]

        blr     x19

        stp     x0, x1, [x20, #0]
        stp     d0, d1, [x20, #16]
        mov     sp, x29
        ldp     x19, x20, [sp, #16]
        ldp     x21, x22, [sp, #32]
        ldp     x29, x30, [sp], #48
        ret
