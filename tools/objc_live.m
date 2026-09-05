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
// Dump ObjC class/method signatures from the LIVE arm64 runtime.
// Type encodings are LP64-identical between x86_64 and arm64, so this is a
// valid source of thunk signatures - and it is ground truth for validating
// the static x86_64 cache parser.
#import <objc/runtime.h>
#import <dlfcn.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv) {
    const char *want_lib = NULL, *want_cls = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-l") && i + 1 < argc) want_lib = argv[++i];
        else if (!strcmp(argv[i], "-c") && i + 1 < argc) want_cls = argv[++i];
    }
    if (want_lib && !dlopen(want_lib, RTLD_LAZY)) {
        fprintf(stderr, "dlopen: %s\n", dlerror());
        return 1;
    }

    unsigned n = 0;
    Class *all = objc_copyClassList(&n);
    long nc = 0, nm = 0;
    for (unsigned i = 0; i < n; i++) {
        const char *cname = class_getName(all[i]);
        if (want_cls && strcmp(cname, want_cls)) continue;
        if (want_lib) {
            const char *img = class_getImageName(all[i]);
            if (!img || !strstr(img, want_lib)) continue;
        }
        nc++;
        printf("@interface %s\n", cname);
        for (int meta = 0; meta < 2; meta++) {
            Class c = meta ? object_getClass((id)all[i]) : all[i];
            unsigned mc = 0;
            Method *ms = class_copyMethodList(c, &mc);
            for (unsigned j = 0; j < mc; j++) {
                printf("  %c %-52s %s\n", meta ? '+' : '-',
                       sel_getName(method_getName(ms[j])),
                       method_getTypeEncoding(ms[j]) ?: "(null)");
                nm++;
            }
            free(ms);
        }
    }
    free(all);
    fprintf(stderr, "%ld classes, %ld methods\n", nc, nm);
    return 0;
}
