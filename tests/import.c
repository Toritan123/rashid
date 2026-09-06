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
// A normally linked binary - not freestanding. It exists to check that
// LC_DYLD_CHAINED_FIXUPS is walked correctly: the image is rebased, every
// import is bound, real application code runs, and execution stops at the
// first imported symbol actually called, naming it.
//
// rashid is the dynamic linker for this image; the guest's own dyld never
// runs.
#include <stdio.h>

int main(int argc, char **argv) {
    // A format argument keeps this as printf; clang rewrites a bare
    // printf("...\n") into puts.
    printf("this line needs a thunk for printf: %d\n", argc);
    (void)argv;
    return 0;
}
