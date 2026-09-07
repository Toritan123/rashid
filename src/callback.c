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
#include "callback.h"

#include <stdio.h>

#define RSD_TRAMP_COUNT 512

extern char     rsd_tramp_table[];
extern uint64_t rsd_tramp_stride;

static uint64_t targets[RSD_TRAMP_COUNT];
static int      used;

uint64_t rsd_callback_for(uint64_t guest_fn) {
    if (!guest_fn) return 0;
    for (int i = 0; i < used; i++)
        if (targets[i] == guest_fn)
            return (uint64_t)(uintptr_t)rsd_tramp_table + (uint64_t)i * rsd_tramp_stride;
    if (used == RSD_TRAMP_COUNT) {
        fprintf(stderr, "rashid: out of callback trampolines (%d)\n", RSD_TRAMP_COUNT);
        return 0;
    }
    targets[used] = guest_fn;
    return (uint64_t)(uintptr_t)rsd_tramp_table + (uint64_t)used++ * rsd_tramp_stride;
}

uint64_t rsd_callback_target(uint64_t index) {
    return index < (uint64_t)used ? targets[index] : 0;
}
