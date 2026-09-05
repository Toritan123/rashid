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
#include "as.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static uint64_t host_page(void) {
    static uint64_t p;
    if (!p) p = (uint64_t)sysconf(_SC_PAGESIZE);
    return p;
}

int rsd_as_init(rsd_as *as) {
    memset(as, 0, sizeof *as);
    return 0;
}

void rsd_as_free(rsd_as *as) {
    for (int i = 0; i < as->n; i++)
        munmap((void *)(uintptr_t)as->reg[i].start,
               (size_t)(as->reg[i].end - as->reg[i].start));
    free(as->reg);
    memset(as, 0, sizeof *as);
}

static int find(const rsd_as *as, uint64_t a) {
    for (int i = 0; i < as->n; i++)
        if (a >= as->reg[i].start && a < as->reg[i].end)
            return i;
    return -1;
}

static int insert(rsd_as *as, uint64_t start, uint64_t end, int prot) {
    if (as->n == as->cap) {
        int cap = as->cap ? as->cap * 2 : 16;
        rsd_region *r = realloc(as->reg, (size_t)cap * sizeof *r);
        if (!r) return -1;
        as->reg = r;
        as->cap = cap;
    }
    int i = 0;
    while (i < as->n && as->reg[i].start < start) i++;
    memmove(&as->reg[i + 1], &as->reg[i], (size_t)(as->n - i) * sizeof *as->reg);
    as->reg[i] = (rsd_region){ start, end, prot };
    as->n++;
    return 0;
}

uint64_t rsd_as_map(rsd_as *as, uint64_t at, uint64_t len, int prot, bool fixed) {
    if (!len) return 0;
    uint64_t hp = host_page();
    uint64_t lo = at & ~(hp - 1);
    uint64_t hi = (at + len + hp - 1) & ~(hp - 1);
    if (!at) hi = (len + hp - 1) & ~(hp - 1);

    // Everything is mapped read/write first so the loader can fill it in;
    // rsd_as_protect() then applies the final permissions.
    int hostprot = PROT_READ | PROT_WRITE;
    int flags = MAP_PRIVATE | MAP_ANON;
    void *want = at ? (void *)(uintptr_t)lo : NULL;
    if (at && fixed) flags |= MAP_FIXED;

    void *p = mmap(want, (size_t)(at ? hi - lo : hi), hostprot, flags, -1, 0);
    if (p == MAP_FAILED && at && !fixed)
        p = mmap(NULL, (size_t)hi, hostprot, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (p == MAP_FAILED) return 0;

    uint64_t base = (uint64_t)(uintptr_t)p;
    uint64_t size = at ? hi - lo : hi;
    if (insert(as, base, base + size, prot) < 0) {
        munmap(p, (size_t)size);
        return 0;
    }
    // Callers that asked for a specific address get back the offset they
    // wanted within the rounded-out host page.
    return (at && p == want) ? at : base;
}

int rsd_as_protect(rsd_as *as, uint64_t at, uint64_t len, int prot) {
    if (!len) return 0;
    uint64_t end = at + len;

    // Host memory deliberately stays read/write and mprotect is not called.
    // Apple Silicon uses 16K host pages while x86_64 images are laid out on
    // 4K boundaries, so one host page routinely spans segments with different
    // permissions - rounding a protection outward would silently make the
    // neighbouring segment unwritable. Permissions are recorded per guest
    // region instead and enforced by rsd_as_ok() on every access. The JIT
    // will need a real answer here, since it cannot afford a software check.
    rsd_region *out = malloc((size_t)(as->n * 3 + 1) * sizeof *out);
    if (!out) return -1;
    int m = 0;
    for (int i = 0; i < as->n; i++) {
        rsd_region r = as->reg[i];
        if (r.end <= at || r.start >= end) { out[m++] = r; continue; }
        if (r.start < at) out[m++] = (rsd_region){ r.start, at, r.prot };
        uint64_t s = r.start > at ? r.start : at;
        uint64_t e = r.end   < end ? r.end   : end;
        out[m++] = (rsd_region){ s, e, prot };
        if (r.end > end) out[m++] = (rsd_region){ end, r.end, r.prot };
    }
    free(as->reg);
    as->reg = out;
    as->cap = as->n * 3 + 1;
    as->n = m;
    return 0;
}

int rsd_as_unmap(rsd_as *as, uint64_t at, uint64_t len) {
    uint64_t hp = host_page();
    uint64_t lo = at & ~(hp - 1);
    uint64_t hi = (at + len + hp - 1) & ~(hp - 1);
    if (munmap((void *)(uintptr_t)lo, (size_t)(hi - lo)) < 0)
        return -1;
    for (int i = 0; i < as->n; i++) {
        if (as->reg[i].start >= lo && as->reg[i].end <= hi) {
            memmove(&as->reg[i], &as->reg[i + 1],
                    (size_t)(as->n - i - 1) * sizeof *as->reg);
            as->n--; i--;
        }
    }
    return 0;
}

bool rsd_as_mapped(const rsd_as *as, uint64_t a, uint64_t n) {
    if (!n) return true;
    if (a + n < a) return false;                 // wrapped
    while (n) {
        int i = find(as, a);
        if (i < 0) return false;
        uint64_t avail = as->reg[i].end - a;
        if (avail >= n) return true;
        a += avail; n -= avail;
    }
    return true;
}

bool rsd_as_ok(const rsd_as *as, uint64_t a, uint64_t n, int prot) {
    if (!n) return true;
    if (a + n < a) return false;
    while (n) {
        int i = find(as, a);
        if (i < 0 || (as->reg[i].prot & prot) != prot) return false;
        uint64_t avail = as->reg[i].end - a;
        if (avail >= n) return true;
        a += avail; n -= avail;
    }
    return true;
}

void rsd_as_dump(const rsd_as *as) {
    printf("guest regions (%d):\n", as->n);
    for (int i = 0; i < as->n; i++)
        printf("  0x%011llx-0x%011llx  %c%c%c\n",
               as->reg[i].start, as->reg[i].end,
               (as->reg[i].prot & RSD_PROT_R) ? 'r' : '-',
               (as->reg[i].prot & RSD_PROT_W) ? 'w' : '-',
               (as->reg[i].prot & RSD_PROT_X) ? 'x' : '-');
}
