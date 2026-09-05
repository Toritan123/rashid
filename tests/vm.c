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
// Guest memory management. The guest and rashid share one address space, so
// an address the guest gets back from mmap is directly usable by native code
// too - which is the whole point, since translated code passes pointers into
// native frameworks.
//
// Exits with the number of checks that passed.
#define SYS_MUNMAP   0x2000049
#define SYS_MPROTECT 0x200004a
#define SYS_MMAP     0x20000c5
#define SYS_EXIT     0x2000001

#define PROT_NONE  0
#define PROT_READ  1
#define PROT_WRITE 2
#define MAP_PRIVATE 0x0002
#define MAP_ANON    0x1000

static long sys6(long n, long a, long b, long c, long d, long e, long f) {
    register long r10 __asm__("r10") = d;
    register long r8  __asm__("r8")  = e;
    register long r9  __asm__("r9")  = f;
    long ret;
    __asm__ volatile("syscall" : "=a"(ret)
                     : "a"(n), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8), "r"(r9)
                     : "rcx", "r11", "memory");
    return ret;
}

void start(void) {
    long ok = 0;

    long p = sys6(SYS_MMAP, 0, 0x4000, PROT_READ | PROT_WRITE,
                  MAP_ANON | MAP_PRIVATE, -1, 0);
    if (p > 0) ok++;

    *(volatile long *)p = 0x1122334455667788L;              // it is writable
    if (*(volatile long *)p == 0x1122334455667788L) ok++;

    ((volatile char *)p)[0x3fff] = 0x5a;                     // and all of it
    if (((volatile char *)p)[0x3fff] == 0x5a) ok++;

    long q = sys6(SYS_MMAP, 0, 0x4000, PROT_READ | PROT_WRITE,
                  MAP_ANON | MAP_PRIVATE, -1, 0);
    if (q > 0 && q != p) ok++;                               // distinct regions

    *(volatile long *)q = 0x99;
    if (*(volatile long *)p == 0x1122334455667788L) ok++;    // no aliasing

    if (sys6(SYS_MPROTECT, p, 0x4000, PROT_READ, 0, 0, 0) == 0) ok++;
    if (*(volatile long *)p == 0x1122334455667788L) ok++;    // still readable

    if (sys6(SYS_MUNMAP, q, 0x4000, 0, 0, 0, 0) == 0) ok++;
    if (sys6(SYS_MUNMAP, p, 0x4000, 0, 0, 0, 0) == 0) ok++;

    sys6(SYS_EXIT, ok, 0, 0, 0, 0, 0);
    __builtin_unreachable();
}
