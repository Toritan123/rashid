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
// rashid - compiling basic blocks to AArch64
//
// The interpreter costs about forty-five cycles per guest instruction, most
// of it decoding the same bytes over and over. A block compiler decodes once.
//
// All guest state stays in the rsd_cpu struct and the compiler calls back
// into the interpreter's own routines for flags and memory, so the two
// engines cannot disagree about semantics - only about speed. Holding guest
// registers in host registers, which would remove a load and a store per
// operand, is the obvious next step and a separate one; lazy flags, which
// would remove the helper call from most arithmetic, is the one after.
//
// Anything not understood simply ends the block, and the interpreter takes
// over from there, so the compiler can stay small and grow by measurement.
#ifndef RASHID_JIT_H
#define RASHID_JIT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cpu.h"

typedef struct rsd_jit {
    uint8_t *arena;       // MAP_JIT memory
    size_t   size, used;
    // Tag, code and instruction count share a cache line, because the miss
    // is the cost of the lookup and one is cheaper than three.
    struct rsd_jit_entry {
        uint64_t tag;
        void    *code;        // NULL means "nothing compilable here"
        uint32_t count;       // guest instructions in the block
        uint32_t pad;
    } *cache;
    int      cache_bits;
    uint64_t compiled, executed, bailed, interp;
    // What the interpreter was asked to run, by opcode. The next thing
    // worth compiling is whatever is at the top of this list.
    struct rsd_jit_prof { uint8_t b[4]; uint64_t n; } *prof;
    int      nprof;
} rsd_jit;

int  rsd_jit_init(rsd_jit *j);
void rsd_jit_free(rsd_jit *j);

// Compiled entry point for this address, compiling it if worthwhile, or NULL
// if there is nothing here the compiler understands.
void *rsd_jit_lookup(rsd_jit *j, rsd_cpu *c, uint64_t rip);
uint32_t rsd_jit_count(const rsd_jit *j, uint64_t rip);

// One guest instruction the compiler declined, counted and - with
// RASHID_JIT_PROFILE set - recorded by its first four bytes.
void rsd_jit_note_interp(rsd_jit *j, const uint8_t *code);
void rsd_jit_report(const rsd_jit *j, uint64_t icount);

// Implemented in jitenter.S.
uint64_t rsd_jit_enter(rsd_cpu *c, void *body);

#endif
