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
// rashid - LC_DYLD_CHAINED_FIXUPS
//
// Modern Mach-O images do not carry a relocation table. Instead each place
// needing a fixup holds a packed word that also says how far away the next
// one is, so the whole set forms chains threaded through the data segments.
// Walking them is what a dynamic linker does at load time, and rashid has to
// do it because it *is* the dynamic linker here: the guest's own dyld never
// runs.
#include "macho.h"
#include "stubs.h"

#include <mach-o/fixup-chains.h>
#include <mach-o/loader.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *ordinal_lib(const rsd_image *img, int ord) {
    // Ordinals are 1-based into the image's dylib list; the negative values
    // (self, main executable, flat lookup) are rare in application binaries.
    if (ord >= 1 && ord <= img->ndylibs) {
        const char *p = img->dylibs[ord - 1];
        const char *slash = strrchr(p, '/');
        return slash ? slash + 1 : p;
    }
    return "?";
}

static int parse_imports(rsd_image *img, const uint8_t *chain,
                         const struct dyld_chained_fixups_header *h) {
    if (h->imports_format != DYLD_CHAINED_IMPORT) {
        fprintf(stderr, "rashid: unsupported imports format %u\n", h->imports_format);
        return -1;
    }
    const struct dyld_chained_import *imp = (const void *)(chain + h->imports_offset);
    const char *syms = (const char *)(chain + h->symbols_offset);

    img->nimports = (int)h->imports_count;
    if (!img->nimports) return 0;
    img->imports = calloc((size_t)img->nimports, sizeof *img->imports);
    if (!img->imports) return -1;
    for (int i = 0; i < img->nimports; i++) {
        img->imports[i].name = syms + imp[i].name_offset;
        img->imports[i].lib  = ordinal_lib(img, (int)imp[i].lib_ordinal);
        img->imports[i].weak = imp[i].weak_import != 0;
    }
    return 0;
}

// Walk one chain, applying every fixup on it.
static void walk_chain(rsd_image *img, rsd_as *as, uint64_t addr,
                       uint16_t format, uint64_t stub_base) {
    for (;;) {
        if (!rsd_as_mapped(as, addr, 8)) {
            fprintf(stderr, "rashid: fixup chain leaves the image at 0x%llx\n", addr);
            return;
        }
        uint64_t *loc = rsd_g2h(as, addr);
        uint64_t raw = *loc;
        uint32_t next;

        if (raw >> 63) {                                  // bind
            struct dyld_chained_ptr_64_bind b;
            memcpy(&b, &raw, sizeof b);
            next = (uint32_t)b.next;
            if ((int)b.ordinal < img->nimports) {
                rsd_import *im = &img->imports[b.ordinal];
                im->used = true;
                // Data goes straight to the native address; code has to go
                // through a stub so the calling convention can be converted.
                *loc = (im->native && !im->is_code)
                     ? (uint64_t)(uintptr_t)im->native + b.addend
                     : stub_base + (uint64_t)b.ordinal * RSD_STUB_STRIDE + b.addend;
            } else {
                *loc = 0;
            }
        } else {                                          // rebase
            struct dyld_chained_ptr_64_rebase r;
            memcpy(&r, &raw, sizeof r);
            next = (uint32_t)r.next;
            uint64_t t = (format == DYLD_CHAINED_PTR_64_OFFSET)
                       ? img->pref_base + img->slide + r.target
                       : r.target + img->slide;
            *loc = t | ((uint64_t)r.high8 << 56);
        }

        if (!next) return;
        addr += (uint64_t)next * 4;      // both 64-bit formats use a 4-byte stride
    }
}

int rsd_fixups(rsd_image *img, rsd_as *as) {
    if (!img->fixups_size) {
        // Nothing to do: either a static image or one still using the old
        // dyld info opcodes, which application binaries no longer emit.
        return 0;
    }
    const uint8_t *chain = img->file + img->slice_off + img->fixups_off;
    const struct dyld_chained_fixups_header *h = (const void *)chain;
    if (h->fixups_version != 0) {
        fprintf(stderr, "rashid: unsupported fixups version %u\n", h->fixups_version);
        return -1;
    }
    if (parse_imports(img, chain, h) < 0) return -1;
    rsd_resolve_imports(img);

    // One executable stub per import, so an unresolved call lands somewhere
    // rashid can name rather than at address zero.
    if (img->nimports) {
        uint64_t size = (uint64_t)img->nimports * RSD_STUB_STRIDE;
        img->stub_base = rsd_as_map(as, 0, size, RSD_PROT_R | RSD_PROT_X, false);
        if (!img->stub_base) {
            fprintf(stderr, "rashid: cannot allocate import stubs\n");
            return -1;
        }
    }

    const struct dyld_chained_starts_in_image *sii =
        (const void *)(chain + h->starts_offset);
    for (uint32_t i = 0; i < sii->seg_count; i++) {
        if (!sii->seg_info_offset[i]) continue;
        const struct dyld_chained_starts_in_segment *sis =
            (const void *)(chain + h->starts_offset + sii->seg_info_offset[i]);
        if (sis->pointer_format != DYLD_CHAINED_PTR_64 &&
            sis->pointer_format != DYLD_CHAINED_PTR_64_OFFSET) {
            fprintf(stderr, "rashid: unsupported pointer format %u\n",
                    sis->pointer_format);
            return -1;
        }
        uint64_t seg = img->pref_base + img->slide + sis->segment_offset;
        for (uint16_t p = 0; p < sis->page_count; p++) {
            uint16_t start = sis->page_start[p];
            if (start == DYLD_CHAINED_PTR_START_NONE) continue;
            walk_chain(img, as, seg + (uint64_t)p * sis->page_size + start,
                       sis->pointer_format, img->stub_base);
        }
    }
    return 0;
}

void rsd_stubs_of(const rsd_image *img, rsd_stubs *out) {
    memset(out, 0, sizeof *out);
    if (!img->nimports) return;
    out->base  = img->stub_base;
    out->n     = img->nimports;
    out->names = calloc((size_t)img->nimports, sizeof(char *));
    out->libs  = calloc((size_t)img->nimports, sizeof(char *));
    if (!out->names || !out->libs) { out->n = 0; return; }
    out->fns = calloc((size_t)img->nimports, sizeof(void *));
    out->va  = calloc((size_t)img->nimports, sizeof(rsd_vaspec *));
    signed char *cb = calloc((size_t)img->nimports, 1);
    out->cbarg = cb;
    if (!out->fns || !out->va || !cb) { out->n = 0; return; }
    for (int i = 0; i < img->nimports; i++) {
        out->names[i] = img->imports[i].name;
        out->libs[i]  = img->imports[i].lib;
        out->fns[i]   = img->imports[i].is_code ? img->imports[i].native : NULL;
        out->va[i]    = rsd_variadic_spec(img->imports[i].name);
        cb[i]         = (signed char)rsd_callback_arg(img->imports[i].name);
    }
}
