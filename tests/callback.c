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
// Native code calling back into guest code.
//
// The mirror image of a thunk. qsort is native arm64, its comparator is
// translated x86_64, and the two meet at a trampoline: a native entry point
// that captures the AAPCS64 registers, runs the guest function in the
// interpreter, and puts the result back where the caller expects it.
//
// This is what makes application-defined behaviour possible at all -
// comparators, delegates, method implementations, blocks. Without it a
// framework can only ever call other framework code.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int cmp_int(const void *a, const void *b) {
    int x = *(const int *)a, y = *(const int *)b;
    return (x > y) - (x < y);
}

static int cmp_str(const void *a, const void *b) {
    return strcmp(*(const char *const *)a, *(const char *const *)b);
}

int main(void) {
    int v[] = { 42, 7, 19, 3, 88, 1, 55 };
    int n = (int)(sizeof v / sizeof *v);
    qsort(v, (size_t)n, sizeof *v, cmp_int);
    for (int i = 0; i < n; i++) printf("%d%s", v[i], i + 1 < n ? " " : "\n");

    const char *s[] = { "pear", "apple", "fig", "cherry" };
    int m = (int)(sizeof s / sizeof *s);
    qsort(s, (size_t)m, sizeof *s, cmp_str);
    for (int i = 0; i < m; i++) printf("%s%s", s[i], i + 1 < m ? " " : "\n");

    int key = 19;
    int *hit = bsearch(&key, v, (size_t)n, sizeof *v, cmp_int);
    printf("found=%d\n", hit ? *hit : -1);
    return 0;
}
