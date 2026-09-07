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

// Native entry points that lead back into guest code.
//
// A framework calling an application's method, a comparator, a delegate - all
// of them enter arm64-shaped code and must end up in the x86_64 interpreter.
// A block of identical stubs provides the addresses to hand out: each loads
// its own index and jumps to one dispatcher, which captures the argument
// registers and the caller's stack, runs the guest function, and puts the
// result back where AAPCS64 expects it.
//
// The stubs are assembled rather than generated so that no runtime code
// generation - and so no MAP_JIT, no W^X dance - is needed for this.

#define RSD_TRAMP_COUNT 512

        .text
        .globl  _rsd_tramp_table
        .p2align 4
_rsd_tramp_table:
        .set    idx, 0
        .rept   RSD_TRAMP_COUNT
        movz    x16, #idx
        b       _rsd_tramp_common
        .set    idx, idx + 1
        .endr

        .globl  _rsd_tramp_stride
_rsd_tramp_stride:
        .quad   8

// The frame handed to the dispatcher mirrors rsd_callframe in callback.h.
        .p2align 2
_rsd_tramp_common:
        stp     x29, x30, [sp, #-16]!
        mov     x29, sp
        sub     sp, sp, #144
        stp     x0, x1, [sp, #0]
        stp     x2, x3, [sp, #16]
        stp     x4, x5, [sp, #32]
        stp     x6, x7, [sp, #48]
        stp     d0, d1, [sp, #64]
        stp     d2, d3, [sp, #80]
        stp     d4, d5, [sp, #96]
        stp     d6, d7, [sp, #112]
        add     x2, x29, #16            // where the caller's stack args begin
        str     x2, [sp, #128]

        mov     x0, x16                 // which trampoline was entered
        mov     x1, sp
        bl      _rsd_guest_call

        ldp     x0, x1, [sp, #0]        // results written back into the frame
        ldp     d0, d1, [sp, #64]
        mov     sp, x29
        ldp     x29, x30, [sp], #16
        ret
