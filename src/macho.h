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
// rashid - x86_64 Mach-O loader for Apple Silicon
#ifndef RSD_MACHO_H
#define RSD_MACHO_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "as.h"

#define RSD_MAX_SEGS   16
#define RSD_MAX_DYLIBS 128

typedef struct {
    char     name[17];
    uint64_t vmaddr, vmsize;
    uint64_t fileoff, filesize;
    uint32_t initprot, maxprot;
} rsd_seg;

typedef struct {
    const char *path;
    uint8_t    *file;       // mmap'd file image
    size_t      filesz;
    uint64_t    slice_off;  // offset of the x86_64 slice within a fat file

    rsd_seg     segs[RSD_MAX_SEGS];
    int         nsegs;

    const char *dylibs[RSD_MAX_DYLIBS];
    int         ndylibs;

    uint64_t    pref_base;  // __TEXT vmaddr as linked
    uint64_t    slide;      // actual placement - pref_base
    uint64_t    entry;      // guest address of entry point
    bool        pie;
    bool        has_main;   // LC_MAIN vs LC_UNIXTHREAD
} rsd_image;

// Parse an x86_64 Mach-O and map it. A PIE image is slid wherever there is
// room; a non-PIE image must land on its linked address or the load fails,
// since its code contains absolute references.
// Returns 0 on success, -1 on failure (message printed to stderr).
int  rsd_load(rsd_image *img, rsd_as *as, const char *path);
void rsd_unload(rsd_image *img);
void rsd_dump(const rsd_image *img);

#endif
