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

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/mman.h>

// 8 GiB by default: x86_64 images sit above 0x100000000, so the first 4 GiB of
// guest space is mostly the guest's own __PAGEZERO.
#define DEFAULT_SIZE (8ull << 30)

int rsd_as_init(rsd_as *as, uint64_t size) {
    memset(as, 0, sizeof *as);
    if (!size)
        size = DEFAULT_SIZE;
    size = (size + RSD_GUEST_PAGE - 1) & ~(uint64_t)(RSD_GUEST_PAGE - 1);

    // Reserve the whole range with no access. Let the kernel pick the host
    // address: nothing in the guest may depend on where rashid put it.
    void *p = mmap(NULL, size, PROT_NONE, MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
    if (p == MAP_FAILED) {
        perror("rashid: reserve guest address space");
        return -1;
    }
    as->base = p;
    as->size = size;
    as->npages = size / RSD_GUEST_PAGE;
    as->perm = calloc(as->npages, 1);
    if (!as->perm) {
        munmap(p, size);
        return -1;
    }
    return 0;
}

void rsd_as_free(rsd_as *as) {
    if (as->base) munmap(as->base, as->size);
    free(as->perm);
    memset(as, 0, sizeof *as);
}

int rsd_as_map(rsd_as *as, uint64_t gaddr, uint64_t len, int prot) {
    if (!len)
        return 0;
    if (!rsd_as_in(as, gaddr, len)) {
        fprintf(stderr, "rashid: map 0x%llx+0x%llx outside guest space (0x%llx)\n",
                gaddr, len, as->size);
        return -1;
    }
    // The host page size (16K on Apple Silicon) is coarser than the guest's
    // 4K, so round the host mapping outward and track permissions per guest
    // page. Sub-page protection differences are lost to the MMU but still
    // enforced by rsd_as_ok().
    long hp = sysconf(_SC_PAGESIZE);
    uint64_t lo = gaddr & ~(uint64_t)(hp - 1);
    uint64_t hi = (gaddr + len + hp - 1) & ~(uint64_t)(hp - 1);
    if (mprotect(as->base + lo, hi - lo, PROT_READ | PROT_WRITE) < 0) {
        perror("rashid: mprotect guest page");
        return -1;
    }
    for (uint64_t p = gaddr / RSD_GUEST_PAGE;
         p <= (gaddr + len - 1) / RSD_GUEST_PAGE; p++)
        as->perm[p] |= (uint8_t)prot;
    return 0;
}

uint64_t rsd_as_find(const rsd_as *as, uint64_t len, uint64_t hint) {
    uint64_t need = (len + RSD_GUEST_PAGE - 1) / RSD_GUEST_PAGE;
    uint64_t start = hint / RSD_GUEST_PAGE, run = 0;
    for (uint64_t p = start; p < as->npages; p++) {
        run = as->perm[p] ? 0 : run + 1;
        if (run == need)
            return (p + 1 - need) * RSD_GUEST_PAGE;
    }
    return 0;
}

void rsd_as_dump(const rsd_as *as) {
    printf("guest space: 0x%llx bytes @ host %p\n", as->size, (void *)as->base);
    uint64_t p = 0;
    while (p < as->npages) {
        if (!as->perm[p]) { p++; continue; }
        uint64_t s = p, prot = as->perm[p];
        while (p < as->npages && as->perm[p] == prot) p++;
        printf("  0x%011llx-0x%011llx  %c%c%c\n",
               s * RSD_GUEST_PAGE, p * RSD_GUEST_PAGE,
               (prot & RSD_PROT_R) ? 'r' : '-',
               (prot & RSD_PROT_W) ? 'w' : '-',
               (prot & RSD_PROT_X) ? 'x' : '-');
    }
}
