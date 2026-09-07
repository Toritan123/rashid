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
// rashid - handing an image's Objective-C metadata to the native runtime
//
// When a normal application loads, dyld tells the Objective-C runtime about
// the new image and the runtime fixes it up: selectors are made canonical,
// class references resolved, categories attached. rashid is the loader here,
// so it has to do the parts the image depends on.
//
// The one that matters immediately is selectors. Each entry in
// __objc_selrefs starts out pointing at a string inside the image itself,
// and objc_msgSend compares selectors by pointer, not by text. Left alone,
// every message send would look up a selector the runtime has never heard of.
#include "macho.h"

#include "callback.h"

#include <objc/runtime.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "stubs.h"
#include <objc/message.h>

const char *rsd_objc_cstring(uint64_t nsstring) {
    if (!nsstring) return NULL;
    // NSLog and friends take an NSString format. The object came from the
    // guest but is a native one, so asking it for its bytes is just a send.
    typedef const char *(*msg_t)(id, SEL);
    return ((msg_t)objc_msgSend)((id)(uintptr_t)nsstring,
                                 sel_registerName("UTF8String"));
}

// Objective-C methods that take a format string and an ellipsis. Like the C
// side, there is no way to discover this from metadata - a method's type
// encoding describes only its declared arguments - so the ones applications
// actually use are listed. The count includes self and _cmd, and the format
// string is the last fixed argument.
static const struct { const char *sel; int nfixed; } objc_variadic[] = {
    { "stringWithFormat:",            3 },
    { "initWithFormat:",              3 },
    { "localizedStringWithFormat:",   3 },
    { "stringByAppendingFormat:",     3 },
    { "appendFormat:",                3 },
    { "predicateWithFormat:",         3 },
    { "raise:format:",                4 },
};

int rsd_objc_variadic_sel(uint64_t sel) {
    if (!sel) return 0;
    const char *name = sel_getName((SEL)(uintptr_t)sel);
    if (!name) return 0;
    for (size_t i = 0; i < sizeof objc_variadic / sizeof *objc_variadic; i++)
        if (!strcmp(name, objc_variadic[i].sel)) return objc_variadic[i].nfixed;
    return 0;
}

// --- classes the application defines itself ---------------------------
//
// An image references its own classes by address: clang emits a direct
// leaq to _OBJC_CLASS_$_Foo, with no indirection to redirect. So the
// structure sitting at that address has to become a class the runtime
// recognises.
//
// The class itself is built through the public runtime API - allocate,
// add methods, register - with every implementation replaced by a
// trampoline, since the implementations are x86_64 code the runtime must
// not enter directly.
//
// Copying the finished class object over the image's own does not work:
// libobjc is arm64e and signs pointers with address diversity, so a class
// object moved to another address is no longer valid. Instead the image's
// structure is treated as a handle. Rashid remembers which native class it
// stands for and substitutes the real one whenever the guest passes it
// across the boundary, which is the only place it can be observed.

// Layouts are the published 64-bit Objective-C ABI, the same on x86_64 and
// arm64, so the image's metadata can be read where it lies.
#define CLS_SUPER      8
#define CLS_DATA       32
#define RO_INSTSIZE    8
#define RO_NAME        24
#define RO_METHODS     32
#define RO_IVARS       48

static bool objc_verbose(void) {
    static int v = -1;
    if (v < 0) v = getenv("RASHID_OBJC_DEBUG") != NULL;
    return v != 0;
}

static uint64_t rd64(uint64_t a) { return *(const uint64_t *)(uintptr_t)a; }
static uint32_t rd32(uint64_t a) { return *(const uint32_t *)(uintptr_t)a; }

// Walk a method list, handing each entry to `emit`.
static void each_method(uint64_t list,
                        void (*emit)(void *ctx, SEL sel, const char *types, uint64_t imp),
                        void *ctx) {
    if (!list) return;
    uint32_t ef = rd32(list), count = rd32(list + 4);
    uint32_t entsize = ef & 0xfffc;
    bool small = (ef & 0x80000000) != 0;
    if (objc_verbose())
        fprintf(stderr, "rashid:   method_list @0x%llx ef=0x%08x entsize=%u "
                        "small=%d count=%u\n", list, ef, entsize, small, count);
    if (!entsize || count > 4096) return;

    for (uint32_t i = 0; i < count; i++) {
        uint64_t e = list + 8 + (uint64_t)i * entsize;
        SEL sel; const char *types; uint64_t imp;
        if (small) {
            int32_t n_off = (int32_t)rd32(e);
            int32_t t_off = (int32_t)rd32(e + 4);
            int32_t i_off = (int32_t)rd32(e + 8);
            // The name field is a relative pointer to a selector reference,
            // and those were rewritten to canonical selectors at load time.
            sel   = (SEL)(uintptr_t)rd64(e + n_off);
            types = (const char *)(uintptr_t)(e + 4 + t_off);
            imp   = e + 8 + i_off;
        } else {
            // In a big method list the name field holds the selector string
            // itself; the runtime is what makes it canonical.
            const char *nm = (const char *)(uintptr_t)rd64(e);
            sel   = nm ? sel_registerName(nm) : NULL;
            types = (const char *)(uintptr_t)rd64(e + 8);
            imp   = rd64(e + 16);
        }
        if (objc_verbose())
            fprintf(stderr, "rashid:     sel=%p(%s) types=%s imp=0x%llx\n",
                    (void *)sel, sel ? sel_getName(sel) : "?",
                    types ? types : "?", imp);
        if (sel && imp) emit(ctx, sel, types, imp);
    }
}

static void add_method(void *ctx, SEL sel, const char *types, uint64_t imp) {
    uint64_t tramp = rsd_callback_for(imp);
    if (!tramp) return;
    class_addMethod((Class)ctx, sel, (IMP)(uintptr_t)tramp, types);
}

// Ivars: declare them so the runtime lays the class out, then write the
// offsets it chose back into the image, which is where compiled accesses
// read them from.
static void add_ivars(Class cls, uint64_t ro) {
    uint64_t list = rd64(ro + RO_IVARS);
    if (!list) return;
    uint32_t entsize = rd32(list), count = rd32(list + 4);
    if (!entsize || count > 1024) return;
    for (uint32_t i = 0; i < count; i++) {
        uint64_t iv = list + 8 + (uint64_t)i * entsize;
        const char *name = (const char *)(uintptr_t)rd64(iv + 8);
        const char *type = (const char *)(uintptr_t)rd64(iv + 16);
        uint32_t align = rd32(iv + 24), size = rd32(iv + 28);
        if (name) class_addIvar(cls, name, size, (uint8_t)align, type);
    }
}

static void fix_ivar_offsets(Class cls, uint64_t ro) {
    uint64_t list = rd64(ro + RO_IVARS);
    if (!list) return;
    uint32_t entsize = rd32(list), count = rd32(list + 4);
    if (!entsize || count > 1024) return;
    for (uint32_t i = 0; i < count; i++) {
        uint64_t iv = list + 8 + (uint64_t)i * entsize;
        uint64_t off_ptr = rd64(iv);
        const char *name = (const char *)(uintptr_t)rd64(iv + 8);
        if (!off_ptr || !name) continue;
        Ivar real = class_getInstanceVariable(cls, name);
        if (real) *(int32_t *)(uintptr_t)off_ptr = (int32_t)ivar_getOffset(real);
    }
}

// The image's class structures stand in for real ones; this is the map.
#define RSD_MAX_CLASSES 256
static struct { uint64_t guest; Class native; } class_map[RSD_MAX_CLASSES];
static int nclasses;

static void remember(uint64_t guest, Class native) {
    if (nclasses < RSD_MAX_CLASSES) {
        class_map[nclasses].guest  = guest;
        class_map[nclasses].native = native;
        nclasses++;
    }
}

uint64_t rsd_objc_real_class(uint64_t maybe_guest_class) {
    for (int i = 0; i < nclasses; i++)
        if (class_map[i].guest == maybe_guest_class)
            return (uint64_t)(uintptr_t)class_map[i].native;
    return maybe_guest_class;
}

static int register_class(uint64_t gcls) {
    uint64_t ro = rd64(gcls + CLS_DATA) & ~7ull;
    if (!ro) return 0;
    const char *name = (const char *)(uintptr_t)rd64(ro + RO_NAME);
    Class super = (Class)(uintptr_t)rd64(gcls + CLS_SUPER);
    if (objc_verbose())
        fprintf(stderr, "rashid: class @0x%llx ro=0x%llx name=%s super=%p\n",
                gcls, ro, name ? name : "(null)", (void *)super);
    if (!name || !super) return 0;
    if (objc_getClass(name)) return 0;          // already there

    Class cls = objc_allocateClassPair(super, name, 0);
    if (!cls) return 0;

    add_ivars(cls, ro);
    each_method(rd64(ro + RO_METHODS), add_method, cls);

    uint64_t gmeta = rd64(gcls);                 // the image's metaclass
    uint64_t mro = gmeta ? rd64(gmeta + CLS_DATA) & ~7ull : 0;
    if (mro) each_method(rd64(mro + RO_METHODS), add_method, (Class)object_getClass((id)cls));

    objc_registerClassPair(cls);
    fix_ivar_offsets(cls, ro);
    if (objc_verbose())
        fprintf(stderr, "rashid:   registered %s as %p, copying over 0x%llx\n",
                name, (void *)cls, gcls);

    remember(gcls, cls);
    if (gmeta) remember(gmeta, (Class)object_getClass((id)cls));
    return 1;
}

static int register_classes(rsd_image *img, rsd_as *as) {
    const rsd_sect *cl = rsd_find_sect(img, "__objc_classlist");
    if (!cl) return 0;
    uint64_t base = cl->addr + img->slide;
    int n = 0;
    if (objc_verbose())
        fprintf(stderr, "rashid: __objc_classlist at 0x%llx size %llu\n",
                base, cl->size);
    for (uint64_t off = 0; off + 8 <= cl->size; off += 8) {
        if (!rsd_as_mapped(as, base + off, 8)) continue;
        uint64_t gcls = rd64(base + off);
        if (gcls) n += register_class(gcls);
    }
    return n;
}

int rsd_objc_prepare(rsd_image *img, rsd_as *as) {
    const rsd_sect *sr = rsd_find_sect(img, "__objc_selrefs");
    if (!sr) return register_classes(img, as);

    uint64_t base = sr->addr + img->slide;
    int n = 0;
    for (uint64_t off = 0; off + 8 <= sr->size; off += 8) {
        uint64_t slot_addr = base + off;
        if (!rsd_as_mapped(as, slot_addr, 8)) continue;
        uint64_t *slot = rsd_g2h(as, slot_addr);
        const char *name = (const char *)(uintptr_t)*slot;
        if (!name) continue;
        *slot = (uint64_t)(uintptr_t)sel_registerName(name);
        n++;
    }
    return n + register_classes(img, as);
}
