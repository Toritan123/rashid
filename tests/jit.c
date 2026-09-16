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
// Exercise for the block compiler.
//
// Each function leans on a different part of it: scaled index addressing at
// both widths, negative displacements, pointer chasing, RIP-relative
// read-modify-write, call and return through recursion, and loops whose
// branches go both ways. The result is a checksum, compared against native
// execution, and the harness also runs this with -J so that every compiled
// block is replayed through the interpreter.
//
// It exists because the rest of the suite turned out not to be enough. With
// the conditional branch in compiled code deliberately inverted, tests/arith
// still passed - none of its compiled blocks happened to end in a jcc that was
// taken. This test was accepted only after catching that sabotage and two
// others.
#include <stdio.h>

struct node { long key; struct node *next; long pad[3]; };

__attribute__((noinline)) static long sum_array(const long *a, int n) {
    long s = 0;
    for (int i = 0; i < n; i++) s += a[i];
    return s;
}

__attribute__((noinline)) static int sum_ints(const int *a, int n) {
    int s = 0;
    for (int i = n - 1; i >= 0; i--) s += a[i] * 3;
    return s;
}

__attribute__((noinline)) static long walk(struct node *p) {
    long s = 0;
    while (p) { s ^= p->key; p = p->next; }
    return s;
}

__attribute__((noinline)) static long fib(long n) {
    return n < 2 ? n : fib(n - 1) + fib(n - 2);
}

static long counter;
static long table[16];
static int  gcount;

// Globals are addressed RIP-relative, and here they sit inside loops the
// compiler can take whole: read-modify-write with an immediate, an indexed
// load through a RIP-relative base, inc and dec.
__attribute__((noinline)) static long globals(int n) {
    long s = 0;
    for (int i = 0; i < n; i++) {
        table[i & 15] += i;
        gcount += 2;
        s += table[(i * 5) & 15];
        if (s & 1) gcount--; else gcount++;
    }
    return s + gcount;
}

__attribute__((noinline)) static void bump(long by) {
    counter += by;
    counter -= 1;
}

__attribute__((noinline)) static long frame(long a, long b, long c) {
    long local[8];
    for (int i = 0; i < 8; i++) local[i] = a * i - b + c;
    long s = 0;
    for (int i = 7; i >= 0; i--) s = s * 31 + local[i];
    return s;
}

int main(void) {
    long arr[64];
    int ints[64];
    for (int i = 0; i < 64; i++) { arr[i] = i * 1000003L; ints[i] = i - 30; }

    struct node nodes[32];
    for (int i = 0; i < 32; i++) {
        nodes[i].key = (long)i * 7919;
        nodes[i].next = i + 1 < 32 ? &nodes[i + 1] : 0;
    }

    long acc = 0;
    for (int round = 0; round < 50; round++) {
        acc += sum_array(arr, 64);
        acc ^= sum_ints(ints, 64);
        acc += walk(nodes);
        bump(round);
    }
    acc ^= globals(200);
    acc += fib(18);
    acc ^= frame(12345, -678, 9);

    printf("acc=%ld counter=%ld\n", acc, counter);
    return (int)(acc & 0x7f);
}
