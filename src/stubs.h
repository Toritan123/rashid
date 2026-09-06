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
// rashid - stubs for imports that have no thunk yet
//
// Every imported symbol is bound to a distinct address in a small executable
// region. Jumping to one is not a crash but a diagnostic: rashid reports the
// symbol and the library it came from. That turns "this application does not
// run" into a worklist.
#ifndef RASHID_STUBS_H
#define RASHID_STUBS_H

#include <stdint.h>

#define RSD_STUB_STRIDE 16

typedef struct {
    uint64_t     base;      // guest address of stub 0
    int          n;
    const char **names;     // symbol name per stub
    const char **libs;      // library it was imported from
} rsd_stubs;

// Which import does this address belong to, or -1.
static inline int rsd_stub_index(const rsd_stubs *s, uint64_t addr) {
    if (!s || !s->base || addr < s->base) return -1;
    uint64_t off = addr - s->base;
    if (off >= (uint64_t)s->n * RSD_STUB_STRIDE) return -1;
    return (int)(off / RSD_STUB_STRIDE);
}

#endif
