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

// Entering compiled code.
//
// Written here rather than emitted so the block compiler never has to get a
// prologue right. It parks the cpu pointer in x19, which compiled blocks use
// as their only base register, and hands back whatever rip the block decided
// to continue at.
//
//   uint64_t rsd_jit_enter(rsd_cpu *c, void *body);
        .text
        .globl  _rsd_jit_enter
        .p2align 2
_rsd_jit_enter:
        stp     x29, x30, [sp, #-32]!
        mov     x29, sp
        str     x19, [sp, #16]
        mov     x19, x0
        blr     x1
        ldr     x19, [sp, #16]
        ldp     x29, x30, [sp], #32
        ret
