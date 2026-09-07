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

#include <objc/runtime.h>
#include <stdio.h>
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

int rsd_objc_prepare(rsd_image *img, rsd_as *as) {
    const rsd_sect *sr = rsd_find_sect(img, "__objc_selrefs");
    if (!sr) return 0;

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
    return n;
}
