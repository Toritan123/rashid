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
// rashid - guest address space
//
// The guest shares rashid's address space: a guest pointer *is* a host
// pointer. That is forced by the thunk design rather than chosen. When
// translated code calls into a native arm64 framework it hands over pointers,
// and native code stores pointers into memory the guest later reads - into
// structs, into ObjC objects, into buffers passed to callbacks. Translating
// arguments at the boundary would mean chasing whole pointer graphs, so the
// two sides have to agree on what an address means. Wine and box64 share an
// address space for the same reason.
//
// What this module tracks is therefore not a separate space but the set of
// regions rashid has handed to the guest, with their permissions. The
// interpreter checks accesses against it, which turns a wild guest pointer
// into a precise report instead of a crash somewhere later. It is a debugging
// aid, not a sandbox; the JIT will let the host MMU do this work.
#ifndef RASHID_AS_H
#define RASHID_AS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define RSD_GUEST_PAGE 4096      // x86_64 page granularity
#define RSD_PROT_R     1         // matches PROT_READ/WRITE/EXEC
#define RSD_PROT_W     2
#define RSD_PROT_X     4

typedef struct {
    uint64_t start, end;
    int      prot;
} rsd_region;

typedef struct {
    rsd_region *reg;
    int         n, cap;
} rsd_as;

int  rsd_as_init(rsd_as *as);
void rsd_as_free(rsd_as *as);

// Map len bytes for the guest. `at` is a preferred address; with `fixed` it
// is required. Returns the address, or 0 on failure.
uint64_t rsd_as_map(rsd_as *as, uint64_t at, uint64_t len, int prot, bool fixed);
int      rsd_as_protect(rsd_as *as, uint64_t at, uint64_t len, int prot);
int      rsd_as_unmap(rsd_as *as, uint64_t at, uint64_t len);

// Is [a, a+n) guest memory, and does it allow `prot`?
bool rsd_as_ok(const rsd_as *as, uint64_t a, uint64_t n, int prot);
bool rsd_as_mapped(const rsd_as *as, uint64_t a, uint64_t n);

void rsd_as_dump(const rsd_as *as);

// Guest and host addresses are the same thing; this exists to mark the places
// where a guest address crosses into host code.
static inline void *rsd_g2h(const rsd_as *as, uint64_t g) {
    (void)as;
    return (void *)(uintptr_t)g;
}

#endif
