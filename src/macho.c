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
#include "macho.h"

#include <mach-o/loader.h>
#include <mach-o/fat.h>
#include <mach/machine.h>

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

static uint32_t bswap32(uint32_t v) { return __builtin_bswap32(v); }

// Locate the x86_64 slice. Handles thin and fat (both endiannesses).
static int find_slice(const uint8_t *f, size_t sz, uint64_t *out_off) {
    if (sz < 4) return -1;
    uint32_t magic = *(const uint32_t *)f;

    if (magic == MH_MAGIC_64 || magic == MH_CIGAM_64) {
        const struct mach_header_64 *mh = (const void *)f;
        if (mh->cputype != CPU_TYPE_X86_64) {
            fprintf(stderr, "rashid: not an x86_64 image (cputype=%d)\n", mh->cputype);
            return -1;
        }
        *out_off = 0;
        return 0;
    }

    if (magic == FAT_MAGIC || magic == FAT_CIGAM) {
        const struct fat_header *fh = (const void *)f;
        uint32_t n = bswap32(fh->nfat_arch); // fat headers are always big-endian
        const struct fat_arch *fa = (const void *)(f + sizeof *fh);
        for (uint32_t i = 0; i < n; i++) {
            if ((int32_t)bswap32((uint32_t)fa[i].cputype) == CPU_TYPE_X86_64) {
                *out_off = bswap32(fa[i].offset);
                return 0;
            }
        }
        fprintf(stderr, "rashid: fat binary has no x86_64 slice\n");
        return -1;
    }

    fprintf(stderr, "rashid: unrecognized magic 0x%08x\n", magic);
    return -1;
}

int rsd_load(rsd_image *img, rsd_as *as, const char *path) {
    memset(img, 0, sizeof *img);
    img->path = path;

    int fd = open(path, O_RDONLY);
    if (fd < 0) { perror("rashid: open"); return -1; }

    struct stat st;
    if (fstat(fd, &st) < 0) { perror("rashid: fstat"); close(fd); return -1; }
    img->filesz = (size_t)st.st_size;

    img->file = mmap(NULL, img->filesz, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (img->file == MAP_FAILED) { perror("rashid: mmap file"); return -1; }

    if (find_slice(img->file, img->filesz, &img->slice_off) < 0) goto fail;

    const uint8_t *slice = img->file + img->slice_off;
    const struct mach_header_64 *mh = (const void *)slice;
    img->pie = (mh->flags & MH_PIE) != 0;

    uint64_t entry_off = 0;
    bool     got_entry = false;

    // --- pass 1: walk load commands -------------------------------------
    const struct load_command *lc = (const void *)(slice + sizeof *mh);
    for (uint32_t i = 0; i < mh->ncmds; i++) {
        switch (lc->cmd) {
        case LC_SEGMENT_64: {
            const struct segment_command_64 *sc = (const void *)lc;
            if (strcmp(sc->segname, SEG_PAGEZERO) == 0) break;
            if (img->nsegs >= RSD_MAX_SEGS) {
                fprintf(stderr, "rashid: too many segments\n"); goto fail;
            }
            rsd_seg *s = &img->segs[img->nsegs++];
            memcpy(s->name, sc->segname, 16); s->name[16] = 0;
            s->vmaddr   = sc->vmaddr;
            s->vmsize   = sc->vmsize;
            s->fileoff  = sc->fileoff;
            s->filesize = sc->filesize;
            s->initprot = (uint32_t)sc->initprot;
            s->maxprot  = (uint32_t)sc->maxprot;
            if (strcmp(sc->segname, SEG_TEXT) == 0) img->pref_base = sc->vmaddr;
            break;
        }
        case LC_MAIN: {
            const struct entry_point_command *ep = (const void *)lc;
            entry_off = ep->entryoff;
            got_entry = true;
            img->has_main = true;
            break;
        }
        case LC_UNIXTHREAD: {
            // x86_64 thread state: flavor, count, then x86_thread_state64_t.
            // rip is field index 16 in that struct (rax..r15, rip).
            const uint32_t *w = (const void *)((const uint8_t *)lc + sizeof(struct load_command));
            const uint64_t *regs = (const void *)(w + 2);
            entry_off = regs[16];   // absolute vmaddr, not an offset
            got_entry = true;
            img->has_main = false;
            break;
        }
        case LC_LOAD_DYLIB:
        case LC_LOAD_WEAK_DYLIB:
        case LC_REEXPORT_DYLIB: {
            const struct dylib_command *dc = (const void *)lc;
            if (img->ndylibs < RSD_MAX_DYLIBS)
                img->dylibs[img->ndylibs++] =
                    (const char *)lc + dc->dylib.name.offset;
            break;
        }
        default: break;
        }
        lc = (const void *)((const uint8_t *)lc + lc->cmdsize);
    }

    if (!img->nsegs || !got_entry) {
        fprintf(stderr, "rashid: image has no segments or no entry point\n");
        goto fail;
    }

    // --- pass 2: place the image ---------------------------------------
    // Reserve the whole span in one mapping so the segments keep their
    // relative layout, then apply each segment's own permissions.
    uint64_t lo = UINT64_MAX, hi = 0;
    for (int i = 0; i < img->nsegs; i++) {
        if (img->segs[i].vmaddr < lo) lo = img->segs[i].vmaddr;
        uint64_t end = img->segs[i].vmaddr + img->segs[i].vmsize;
        if (end > hi) hi = end;
    }

    uint64_t base = rsd_as_map(as, lo, hi - lo, RSD_PROT_R | RSD_PROT_W, !img->pie);
    if (!base) {
        if (!img->pie)
            fprintf(stderr,
                "rashid: cannot place non-PIE image at its linked address "
                "0x%llx\n"
                "        (rashid's own arm64 image occupies that range; only "
                "PIE images can be slid)\n", lo);
        else
            fprintf(stderr, "rashid: cannot reserve 0x%llx bytes for the image\n",
                    hi - lo);
        goto fail;
    }
    img->slide = base - lo;

    for (int i = 0; i < img->nsegs; i++) {
        rsd_seg *s = &img->segs[i];
        if (s->filesize)
            memcpy(rsd_g2h(as, s->vmaddr + img->slide),
                   slice + s->fileoff, s->filesize);
    }
    for (int i = 0; i < img->nsegs; i++) {
        rsd_seg *s = &img->segs[i];
        // initprot bits match RSD_PROT_* (VM_PROT_READ/WRITE/EXECUTE = 1/2/4).
        rsd_as_protect(as, s->vmaddr + img->slide, s->vmsize, (int)(s->initprot & 7));
    }

    img->entry = (img->has_main ? img->pref_base + entry_off : entry_off)
               + img->slide;
    return 0;

fail:
    rsd_unload(img);
    return -1;
}

void rsd_unload(rsd_image *img) {
    if (img->file && img->file != MAP_FAILED)
        munmap(img->file, img->filesz);
    img->file = NULL;
}

void rsd_dump(const rsd_image *img) {
    printf("image   : %s\n", img->path);
    printf("slice   : +0x%llx  %s  %s\n", img->slice_off,
           img->pie ? "PIE" : "non-PIE",
           img->has_main ? "LC_MAIN" : "LC_UNIXTHREAD");
    if (img->slide) printf("slide   : 0x%llx\n", img->slide);
    printf("entry   : 0x%llx\n", img->entry);
    printf("segments:\n");
    for (int i = 0; i < img->nsegs; i++) {
        const rsd_seg *s = &img->segs[i];
        printf("  %-12s vm 0x%011llx+0x%-8llx  file 0x%08llx+0x%-8llx  %c%c%c\n",
               s->name, s->vmaddr, s->vmsize, s->fileoff, s->filesize,
               (s->initprot & 1) ? 'r' : '-',
               (s->initprot & 2) ? 'w' : '-',
               (s->initprot & 4) ? 'x' : '-');
    }
    if (img->ndylibs) {
        printf("dylibs  : (%d) -- these are the thunk targets\n", img->ndylibs);
        for (int i = 0; i < img->ndylibs; i++)
            printf("  %s\n", img->dylibs[i]);
    } else {
        printf("dylibs  : none (freestanding)\n");
    }
}
