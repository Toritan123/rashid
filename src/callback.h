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
// rashid - native entry points that lead back into guest code
//
// Frameworks call back: a method implementation, a comparator, a delegate, a
// block. Those all arrive as arm64 calls and have to end up in the x86_64
// interpreter, which is the mirror image of a thunk and needs the same ABI
// conversion in the other direction.
#ifndef RASHID_CALLBACK_H
#define RASHID_CALLBACK_H

#include <stdint.h>

// Mirrors the layout that trampoline.S builds on the stack.
typedef struct {
    uint64_t x[8];    // x0..x7 on entry, x0 and x1 on return
    uint64_t d[8];    // raw bits of d0..d7, likewise
    uint64_t stack;   // where the caller's stack arguments begin
    uint64_t pad;
} rsd_callframe;

// Native address to hand to a framework so that calling it runs `guest_fn`.
// Returns 0 if the pool is exhausted.
uint64_t rsd_callback_for(uint64_t guest_fn);

// Guest function behind a trampoline index, or 0.
uint64_t rsd_callback_target(uint64_t index);

// Called from trampoline.S.
void rsd_guest_call(uint64_t index, rsd_callframe *f);

#endif
