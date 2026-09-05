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
// The guest gets its own address space, disjoint from rashid's. A guest address
// is an offset into one big host reservation, so the guest can use its
// preferred vmaddr (x86_64 images want 0x100000000, which is exactly where
// rashid's own arm64 image sits) and a wild guest pointer faults cleanly instead
// of corrupting the translator.
//
// In the interpreter this costs a bounds check per access. In the JIT (M5) the
// base becomes a reserved register, which is what FEX and box64 do.
#ifndef RSD_AS_H
#define RSD_AS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define RSD_GUEST_PAGE 4096          // x86_64 page granularity
#define RSD_PROT_R     1
#define RSD_PROT_W     2
#define RSD_PROT_X     4

typedef struct {
    uint8_t *base;      // host address of guest 0
    uint64_t size;      // guest address space size
    uint8_t *perm;      // one byte per guest page: RSD_PROT_*
    uint64_t npages;
} rsd_as;

int  rsd_as_init(rsd_as *as, uint64_t size);
void rsd_as_free(rsd_as *as);

// Commit [gaddr, gaddr+len) with the given permissions. Returns 0 on success.
int  rsd_as_map(rsd_as *as, uint64_t gaddr, uint64_t len, int prot);

// Find `len` bytes of free guest space; returns 0 if none.
uint64_t rsd_as_find(const rsd_as *as, uint64_t len, uint64_t hint);

// Human-readable dump of the committed regions.
void rsd_as_dump(const rsd_as *as);

static inline bool rsd_as_in(const rsd_as *as, uint64_t g, uint64_t n) {
    return g < as->size && n <= as->size - g;
}

// Guest -> host. Caller must have checked rsd_as_in().
static inline void *rsd_g2h(const rsd_as *as, uint64_t g) {
    return as->base + g;
}

// True if [g, g+n) is committed at all, whatever its permissions.
static inline bool rsd_as_mapped(const rsd_as *as, uint64_t g, uint64_t n) {
    if (!rsd_as_in(as, g, n))
        return false;
    for (uint64_t p = g / RSD_GUEST_PAGE; p <= (g + n - 1) / RSD_GUEST_PAGE; p++)
        if (!as->perm[p])
            return false;
    return true;
}

// Permission check for [g, g+n). n is small (<= 8) in practice.
static inline bool rsd_as_ok(const rsd_as *as, uint64_t g, uint64_t n, int prot) {
    if (!rsd_as_in(as, g, n))
        return false;
    uint64_t p0 = g / RSD_GUEST_PAGE, p1 = (g + n - 1) / RSD_GUEST_PAGE;
    for (uint64_t p = p0; p <= p1; p++)
        if ((as->perm[p] & prot) != prot)
            return false;
    return true;
}

#endif
