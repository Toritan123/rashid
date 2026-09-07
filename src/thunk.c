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
// rashid - resolving imports to native arm64 code
//
// The frameworks an x86_64 application wants are already loaded in this
// process, as arm64. Resolving an import is therefore a dlsym away; the work
// is in deciding what to do with the answer.
//
// A function must not be bound directly, because the guest would try to
// execute arm64 instructions as x86. It is bound to a stub instead, and the
// interpreter converts the calling convention when execution lands there. A
// data symbol - an Objective-C class object, a CoreFoundation constant - is
// the opposite: it is never called, only dereferenced, so it is bound
// straight to the native address.
//
// Telling the two apart by name would be guesswork. Instead the symbol's
// address is looked up in its own image and the containing segment's
// protection decides: executable means code.
#include "macho.h"

#include <dlfcn.h>
#include <stdio.h>
#include <mach-o/loader.h>
#include <string.h>

static bool addr_is_code(const void *p) {
    Dl_info info;
    if (!dladdr(p, &info) || !info.dli_fbase) return true;
    const struct mach_header_64 *mh = info.dli_fbase;
    if (mh->magic != MH_MAGIC_64) return true;

    uint64_t target = (uint64_t)(uintptr_t)p;
    uint64_t text_vm = 0;
    const struct load_command *lc = (const void *)(mh + 1);
    for (uint32_t i = 0; i < mh->ncmds; i++) {
        if (lc->cmd == LC_SEGMENT_64) {
            const struct segment_command_64 *sc = (const void *)lc;
            if (!strcmp(sc->segname, "__TEXT")) { text_vm = sc->vmaddr; break; }
        }
        lc = (const void *)((const uint8_t *)lc + lc->cmdsize);
    }
    uint64_t slide = (uint64_t)(uintptr_t)mh - text_vm;

    lc = (const void *)(mh + 1);
    for (uint32_t i = 0; i < mh->ncmds; i++) {
        if (lc->cmd == LC_SEGMENT_64) {
            const struct segment_command_64 *sc = (const void *)lc;
            uint64_t s = sc->vmaddr + slide;
            if (target >= s && target < s + sc->vmsize)
                return (sc->initprot & VM_PROT_EXECUTE) != 0;
        }
        lc = (const void *)((const uint8_t *)lc + lc->cmdsize);
    }
    return true;
}

// Variadic functions rashid can forward. There is no general answer without
// signatures for every import, so this covers the formatted-IO family, which
// is what applications actually reach for. Anything else variadic will be
// handed its arguments in registers and misbehave; that is a known gap.
static const struct { const char *name; rsd_vaspec spec; } variadics[] = {
    { "printf",    { 1, 0, false, false, false } },
    { "fprintf",   { 2, 1, false, false, false } },
    { "sprintf",   { 2, 1, false, false, false } },
    { "snprintf",  { 3, 2, false, false, false } },
    { "dprintf",   { 2, 1, false, false, false } },
    { "asprintf",  { 2, 1, false, false, false } },
    { "syslog",    { 2, 1, false, false, false } },
    { "scanf",     { 1, 0, true, false, false } },
    { "fscanf",    { 2, 1, true, false, false } },
    { "sscanf",    { 2, 1, true, false, false } },
    { "NSLog",     { 1, 0, false, true,  false } },
    { "objc_msgSend", { 2, -1, false, true, true } },
};

const rsd_vaspec *rsd_variadic_spec(const char *symbol) {
    if (!symbol) return NULL;
    const char *n = symbol[0] == '_' ? symbol + 1 : symbol;
    for (size_t i = 0; i < sizeof variadics / sizeof *variadics; i++)
        if (!strcmp(n, variadics[i].name)) return &variadics[i].spec;
    return NULL;
}

// Bring in the libraries the image asks for. They exist on this machine as
// arm64, at the very paths the x86_64 image names, so a dlopen is all it
// takes - the frameworks do not have to be reimplemented or translated,
// which is the premise the whole design rests on.
static void load_dependencies(const rsd_image *img) {
    for (int i = 0; i < img->ndylibs; i++) {
        if (!dlopen(img->dylibs[i], RTLD_LAZY | RTLD_GLOBAL))
            fprintf(stderr, "rashid: cannot load %s: %s\n",
                    img->dylibs[i], dlerror());
    }
}

void rsd_resolve_imports(rsd_image *img) {
    load_dependencies(img);
    for (int i = 0; i < img->nimports; i++) {
        rsd_import *im = &img->imports[i];
        const char *n = im->name;
        im->native  = dlsym(RTLD_DEFAULT, n[0] == '_' ? n + 1 : n);
        im->is_code = im->native ? addr_is_code(im->native) : true;
    }
}
