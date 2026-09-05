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
// rashid - x86-64 interpreter core (integer subset)
#ifndef RSD_CPU_H
#define RSD_CPU_H

#include <stdint.h>
#include <stdbool.h>

#include "as.h"

enum { RAX, RCX, RDX, RBX, RSP, RBP, RSI, RDI,
       R8,  R9,  R10, R11, R12, R13, R14, R15 };

enum { F_CF = 1u << 0, F_PF = 1u << 2, F_AF = 1u << 4,
       F_ZF = 1u << 6, F_SF = 1u << 7, F_OF = 1u << 11 };

typedef struct {
    uint64_t r[16];
    uint64_t rip;
    uint64_t cur_rip;     // start of the instruction being executed
    uint32_t flags;

    // Segment bases. macOS x86_64 keeps thread-local storage at %gs and never
    // uses %fs; pthread_getspecific is literally movq %gs:(,%rdi,8), %rax.
    uint64_t gs_base;
    uint64_t fs_base;

    rsd_as  *as;          // guest address space
    uint64_t stack_base;  // guest address
    uint64_t stack_size;

    bool     running;
    int      exit_code;
    uint64_t icount;
    bool     trace;

    const char *fault;    // set when execution aborts
    uint64_t    fault_rip;
    uint64_t    fault_addr;
    bool        fault_has_addr;
} rsd_cpu;

int  rsd_cpu_init(rsd_cpu *c, rsd_as *as, uint64_t entry, int argc, char **argv);
void rsd_cpu_free(rsd_cpu *c);

// Execute up to `budget` instructions (0 = unlimited). Returns when the
// guest exits or faults.
void rsd_cpu_run(rsd_cpu *c, uint64_t budget);
void rsd_cpu_dump(const rsd_cpu *c);

#endif
