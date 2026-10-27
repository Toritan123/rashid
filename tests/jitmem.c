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
// The edges of compiled code's in-line memory accesses.
//
// A compiled load or store is made in line when its address is in a region
// a compiled access of the same kind recently succeeded in, and otherwise
// through the interpreter's routine. Each mode here is an access that the
// in-line path must refuse and leave to that routine, which faults: run
// under -j, every mode must end in a "guest memory protection" fault.
//
//   e  8 bytes read across the end of the region most recently read
//   s  the same, for the region read before that
//   p  a write to a region since made read-only
//
// The regions are made by mmap and split by mprotect. rashid does not
// change host protection - it records it - so the bytes beyond each edge
// are really there, and only the check stands between the access and them.
#define SYS_EXIT     0x2000001
#define SYS_MPROTECT 0x200004a
#define SYS_MMAP     0x20000c5

#define PROT_NONE   0
#define PROT_READ   1
#define PROT_WRITE  2
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

// The order of accesses is the whole test - which region is first and which
// second in the cache depends on it - so each sequence is one asm statement,
// where the compiler cannot slip a spill to the stack in between. The first
// access to a region in a sequence goes through the interpreter's routine
// and notes the region; the ones after can be in line.
static void read2(long a, long b) {
    __asm__ volatile("movq (%0), %%rax\n\t movq (%1), %%rax"
                     : : "r"(a), "r"(b) : "rax", "memory");
}
static void read3(long a, long b, long c) {
    __asm__ volatile("movq (%0), %%rax\n\t movq (%1), %%rax\n\t movq (%2), %%rax"
                     : : "r"(a), "r"(b), "r"(c) : "rax", "memory");
}
static void write1(long a, long v) {
    __asm__ volatile("movq %1, (%0)" : : "r"(a), "r"(v) : "memory");
}

int start(int argc, char **argv) {
    char mode = (argc > 1) ? argv[1][0] : 0;
    long p = sys6(SYS_MMAP, 0, 0x8000, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    long q = sys6(SYS_MMAP, 0, 0x8000, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    // Each becomes a 16K region followed by 16K the guest may not touch.
    sys6(SYS_MPROTECT, p + 0x4000, 0x4000, PROT_NONE, 0, 0, 0);
    sys6(SYS_MPROTECT, q + 0x4000, 0x4000, PROT_NONE, 0, 0, 0);

    if (mode == 'e') {
        read2(p, p + 0x4000 - 7);       // p first; its last byte is past the end
    } else if (mode == 's') {
        read3(p, q, p + 0x4000 - 7);    // q first, p second
    } else if (mode == 'p') {
        write1(p, 1);
        sys6(SYS_MPROTECT, p, 0x4000, PROT_READ, 0, 0, 0);
        write1(p + 16, 3);
    } else {
        // No mode: the accesses just inside every edge, which must not fault.
        read2(p, p + 0x4000 - 8);
        read3(p, q, p + 0x4000 - 8);
        write1(p, 0);
        write1(p + 0x4000 - 8, 4);
        sys6(SYS_EXIT, *(volatile long *)(p + 0x4000 - 8), 0, 0, 0, 0, 0);
    }
    sys6(SYS_EXIT, 0, 0, 0, 0, 0, 0);
    __builtin_unreachable();
}
