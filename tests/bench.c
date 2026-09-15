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
// Four shapes of work, each isolating a different cost:
//   int    pure interpretation - decode, dispatch, flags
//   mem    loads and stores, so the permission checks on every access
//   flt    scalar double, where one guest instruction does more work
//   thunk  crossing into native libSystem, where rashid barely interprets
//
// Usage: bench <int|mem|flt|thunk> <iterations>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static long integer_work(long n) {
    long a = 1, b = 0;
    for (long i = 0; i < n; i++) { long t = a + b; b = a; a = t & 0xffffff; }
    return a + b;
}

static long memory_work(long n) {
    static long buf[4096];
    long s = 0;
    for (long i = 0; i < n; i++) { buf[i & 4095] = i; s += buf[(i * 7) & 4095]; }
    return s;
}

static double float_work(long n) {
    double x = 1.0, s = 0;
    for (long i = 0; i < n; i++) {
        x = x * 1.0000001 + 0.5; s += x * 0.5; if (x > 1e6) x = 1.0;
    }
    return s;
}

static long thunk_work(long n) {
    static char buf[64];
    long s = 0;
    // The string has to change or the compiler folds strlen away.
    for (long i = 0; i < n; i++) { buf[i & 31] = (char)(97 + (i & 7)); s += (long)strlen(buf); }
    return s;
}

int main(int argc, char **argv) {
    long n = argc > 2 ? atol(argv[2]) : 1000000;
    const char *w = argc > 1 ? argv[1] : "int";
    if (!strcmp(w, "int"))        printf("%ld\n", integer_work(n));
    else if (!strcmp(w, "mem"))   printf("%ld\n", memory_work(n));
    else if (!strcmp(w, "flt"))   printf("%.3f\n", float_work(n));
    else if (!strcmp(w, "thunk")) printf("%ld\n", thunk_work(n));
    else { fprintf(stderr, "usage: bench <int|mem|flt|thunk> <iterations>\n"); return 2; }
    return 0;
}
