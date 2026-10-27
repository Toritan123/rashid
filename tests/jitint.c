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
// Narrow operands, shifts, multiplies and setcc through the block compiler.
//
// A byte write leaves the other seven bytes of its register alone, which is
// the opposite of what a 32-bit write does, and without a REX prefix the
// register numbers 4..7 name ah, ch, dh and bh - bits 8..15 of rax, rcx, rdx
// and rbx - rather than the low byte of rsp, rbp, rsi and rdi. Neither rule
// has anywhere to show itself in ordinary C, so the high-byte registers and
// the merging writes are written out in assembly here.
//
// A shift takes its count modulo the operand width, and a count that comes
// out zero leaves even the flags alone - which is the one case where doing
// the shift anyway and discarding the result is wrong.
//
// The flags are checked as well as the results: a byte compare sets SF from
// bit 7 and ZF from the low eight bits, so an operation done at the wrong
// width is usually visible only in a condition code.
#include <stdio.h>
#include <string.h>

static inline unsigned long mix(unsigned long h, unsigned long v) {
    h = (h ^ v) * 0x100000001b3UL;
    return h ^ (h >> 29);
}

static unsigned char  ub[64];
static signed char    sb[64];
static short          sh[32];
static unsigned short uh[32];

// Byte arithmetic in memory and in registers, with every ALU row and both
// directions of the modrm byte.
__attribute__((noinline)) static unsigned long bytes(unsigned long h) {
    for (int i = 0; i < 64; i++) { ub[i] = (unsigned char)(i * 37 + 11); sb[i] = (signed char)(i * 53 - 97); }
    for (int k = 0; k < 3; k++) {
        for (int i = 0; i < 64; i++) {
            unsigned char a = ub[i], b = ub[(i * 7 + 1) & 63];
            ub[i] = (unsigned char)(a + b);
            ub[(i + 1) & 63] ^= a;
            ub[(i + 2) & 63] -= b;
            ub[(i + 3) & 63] |= (unsigned char)(a & 0x3c);
            ub[(i + 4) & 63] &= (unsigned char)(b | 0x81);
            ub[(i + 5) & 63]++;
            ub[(i + 6) & 63]--;
            // Byte compares, whose only observable effect is a flag.
            h = mix(h, (unsigned long)(a > b) | ((unsigned long)(a == b) << 1) |
                       ((unsigned long)((signed char)a < (signed char)b) << 2) |
                       ((unsigned long)((a & 0x80) != 0) << 3));
            h = mix(h, ub[i]);
        }
        for (int i = 0; i < 64; i++) {
            signed char a = sb[i];
            sb[i] = (signed char)(a * 3 + 1);
            h = mix(h, (unsigned long)(long)sb[i]);
            h = mix(h, (unsigned long)(a < 0) | ((unsigned long)(a == 0) << 1));
        }
    }
    return h;
}

// movzx and movsx, from a register and from memory, at both source widths
// and both destination widths.
__attribute__((noinline)) static unsigned long widen(unsigned long h) {
    for (int i = 0; i < 32; i++) { sh[i] = (short)(i * 4099 - 30000); uh[i] = (unsigned short)(i * 2053 + 7); }
    for (int i = 0; i < 64; i++) {
        unsigned char  u8 = ub[i];
        signed char    s8 = sb[i];
        unsigned short u16 = uh[i & 31];
        short          s16 = sh[i & 31];
        h = mix(h, (unsigned)u8);          h = mix(h, (unsigned long)u8);
        h = mix(h, (unsigned)(int)s8);     h = mix(h, (unsigned long)(long)s8);
        h = mix(h, (unsigned)u16);         h = mix(h, (unsigned long)u16);
        h = mix(h, (unsigned)(int)s16);    h = mix(h, (unsigned long)(long)s16);
        // Through a register rather than straight from memory.
        unsigned long r = (unsigned long)(unsigned char)(u8 ^ 0x5a);
        h = mix(h, r + (unsigned long)(long)(signed char)(s8 ^ 0x5a));
    }
    return h;
}

// ah, ch, dh, bh, and the low bytes that the same register numbers name once
// a REX prefix is present. Written out because a C compiler will not produce
// them on purpose.
__attribute__((noinline)) static unsigned long highbytes(unsigned long h) {
    for (int i = 0; i < 64; i++) {
        unsigned long a = 0x1122334455660000UL | (unsigned long)(i * 2731);
        unsigned long c = 0x7fff0000ffff0000UL | (unsigned long)(i * 601);
        unsigned long out1, out2, out3, fl;
        __asm__ volatile(
            // Read a high byte, work on it, write it back: the rest of rax
            // has to survive, and so does the low byte.
            "movq  %4, %%rax\n\t"
            "movq  %5, %%rcx\n\t"
            "movq  %6, %%rbx\n\t"
            "addb  %%ch, %%ah\n\t"
            "xorb  %%al, %%ah\n\t"
            "subb  $0x25, %%ah\n\t"
            "movb  %%ah, %%dh\n\t"
            "andb  %%dh, %%bh\n\t"
            "orb   %%bh, %%ch\n\t"
            "incb  %%ah\n\t"
            "decb  %%cl\n\t"
            "movq  %%rax, %0\n\t"
            "movq  %%rcx, %1\n\t"
            // A high byte as the source of a comparison, and as a value that
            // crosses into a full-width register through movzx and movsx.
            "cmpb  %%ch, %%ah\n\t"
            "movzbl %%ah, %%edx\n\t"
            "movsbl %%ch, %%esi\n\t"
            "addq  %%rsi, %%rdx\n\t"
            "movq  %%rdx, %2\n\t"
            "pushfq\n\t"
            "popq  %3\n\t"
            : "=&r"(out1), "=&r"(out2), "=&r"(out3), "=&r"(fl)
            : "r"(a), "r"(c), "r"(0x66778899aabbccddUL ^ (unsigned long)i)
            : "rax", "rbx", "rcx", "rdx", "rsi", "cc");
        h = mix(h, out1); h = mix(h, out2); h = mix(h, out3);
        h = mix(h, fl & 0x8d5);            // the arithmetic flags only
    }
    for (int i = 0; i < 64; i++) {
        unsigned long si = 0x4040404040400000UL | (unsigned long)(i * 3307);
        unsigned long di = 0x0101010101010000UL | (unsigned long)(i * 97);
        unsigned long o1, o2;
        __asm__ volatile(
            // With REX these same numbers are sil and dil, and a byte write
            // must leave the upper seven bytes of each untouched.
            "movq  %2, %%rsi\n\t"
            "movq  %3, %%rdi\n\t"
            "addb  %%sil, %%dil\n\t"
            "xorb  $0x7e, %%sil\n\t"
            "movb  %%dil, %%sil\n\t"
            "notb  %%dil\n\t"
            "movq  %%rsi, %0\n\t"
            "movq  %%rdi, %1\n\t"
            : "=&r"(o1), "=&r"(o2)
            : "r"(si), "r"(di)
            : "rsi", "rdi", "cc");
        h = mix(h, o1); h = mix(h, o2);
    }
    return h;
}

// The accumulator and immediate forms: op al, imm8; mov r8, imm8; and a byte
// immediate compared against memory.
__attribute__((noinline)) static unsigned long immediates(unsigned long h) {
    for (int i = 0; i < 64; i++) {
        unsigned long o1, o2;
        __asm__ volatile(
            "movq  %2, %%rax\n\t"
            "addb  $0x31, %%al\n\t"
            "andb  $0x7f, %%al\n\t"
            "orb   $0x08, %%al\n\t"
            "xorb  $0x44, %%al\n\t"
            "subb  $0x13, %%al\n\t"
            "cmpb  $0x40, %%al\n\t"
            "movb  $0x5c, %%bl\n\t"
            "testb $0x10, %%al\n\t"
            "movq  %%rax, %0\n\t"
            "pushfq\n\t"
            "popq  %1\n\t"
            : "=&r"(o1), "=&r"(o2)
            : "r"(0xaaaabbbbccccddddUL + (unsigned long)i)
            : "rax", "rbx", "cc");
        h = mix(h, o1); h = mix(h, o2 & 0x8d5);
    }
    for (int i = 0; i < 64; i++) {
        ub[i] = (unsigned char)(ub[i] + 0x11);
        if (ub[i] > 0x80) ub[i] = (unsigned char)(ub[i] - 0x40);
        if (sb[i] < -3)   sb[i] = (signed char)(sb[i] + 7);
        h = mix(h, (unsigned long)ub[i] | ((unsigned long)(unsigned char)sb[i] << 8));
    }
    return h;
}

// Shifts at every width, by a constant and by cl, including the counts that
// the mask turns into zero and the ones that reach the top bit.
__attribute__((noinline)) static unsigned long shifts(unsigned long h) {
    static const unsigned char counts[] = { 0, 1, 7, 8, 15, 31, 32, 33, 63, 64, 65, 255 };
    static const unsigned long vals[] = {
        0, 1, 0x80, 0xff, 0x8000000000000000UL, 0x7fffffffffffffffUL,
        0xdeadbeefcafebabeUL, 0x00000000ffffffffUL, 0xffffffff00000000UL, 123456789UL };
    for (unsigned v = 0; v < sizeof vals / sizeof *vals; v++)
    for (unsigned k = 0; k < sizeof counts / sizeof *counts; k++) {
        unsigned long a = vals[v];
        unsigned cl = counts[k];
        unsigned long o[6]; unsigned long fl[6];
        // By cl, so the count is not known when the block is compiled.
        __asm__ volatile("movq %8, %%rax\n\t movb %9, %%cl\n\t shlq %%cl, %%rax\n\t"
                         "movq %%rax, %0\n\t pushfq\n\t popq %1\n\t"
                         "movq %8, %%rax\n\t shrq %%cl, %%rax\n\t"
                         "movq %%rax, %2\n\t pushfq\n\t popq %3\n\t"
                         "movq %8, %%rax\n\t sarq %%cl, %%rax\n\t"
                         "movq %%rax, %4\n\t pushfq\n\t popq %5\n\t"
                         "movl %10, %%eax\n\t sarl %%cl, %%eax\n\t"
                         "movq %%rax, %6\n\t pushfq\n\t popq %7\n\t"
                         : "=&r"(o[0]), "=&r"(fl[0]), "=&r"(o[1]), "=&r"(fl[1]),
                           "=&r"(o[2]), "=&r"(fl[2]), "=&r"(o[3]), "=&r"(fl[3])
                         : "r"(a), "r"((unsigned char)cl), "r"((unsigned)a)
                         : "rax", "rcx", "cc");
        // AF is undefined after a shift, and so is OF unless the count was
        // one, so neither is compared where the manual does not define it.
        unsigned long m = (cl == 1) ? 0x8c5 : 0x0c5;
        for (int i = 0; i < 4; i++) { h = mix(h, o[i]); h = mix(h, fl[i] & m); }
        // A shift by cl that comes out zero leaves the flags of whatever came
        // before it, so it must not be taken as overwriting them.
        {
            unsigned long r, f;
            __asm__ volatile("movq %2, %%rax\n\t movb %3, %%cl\n\t cmpq $0x5a5a, %%rax\n\t"
                             "shlq %%cl, %%rax\n\t movq %%rax, %0\n\t pushfq\n\t popq %1\n\t"
                             : "=&r"(r), "=&r"(f) : "r"(a), "r"((unsigned char)cl)
                             : "rax", "rcx", "cc");
            h = mix(h, r);
            h = mix(h, f & ((cl & 63) ? m : 0x8d5));
        }
        // By a constant, which the compiler resolves while compiling.
        // The three shifts by zero come after a compare and before the
        // flags are read, because a shift by zero is only observable in the
        // flags it must not touch.
        __asm__ volatile("movq %4, %%rax\n\t shlq $13, %%rax\n\t sarq $5, %%rax\n\t"
                         "movq %%rax, %0\n\t pushfq\n\t popq %1\n\t"
                         "movl %5, %%eax\n\t shll $17, %%eax\n\t"
                         "shrb $3, %%al\n\t salb $1, %%al\n\t sarb $2, %%al\n\t"
                         "cmpl $0x1234, %%eax\n\t"
                         "shrl $0, %%eax\n\t shlq $0, %%rax\n\t sarb $0, %%al\n\t"
                         "movq %%rax, %2\n\t pushfq\n\t popq %3\n\t"
                         : "=&r"(o[4]), "=&r"(fl[4]), "=&r"(o[5]), "=&r"(fl[5])
                         : "r"(a), "r"((unsigned)a)
                         : "rax", "cc");
        h = mix(h, o[4]); h = mix(h, fl[4] & 0x0c5);
        h = mix(h, o[5]); h = mix(h, fl[5] & 0x8d5);
        // Shifts in memory, where the operand is read and written back.
        static unsigned long m64; static unsigned m32; static unsigned char m8;
        m64 = a; m32 = (unsigned)a; m8 = (unsigned char)a;
        m64 >>= (cl & 63); m32 <<= (cl & 31); m8 = (unsigned char)(m8 >> (cl & 7));
        h = mix(h, m64); h = mix(h, m32); h = mix(h, m8);
    }
    return h;
}

// imul in all three shapes, and setcc into a register and into memory.
__attribute__((noinline)) static unsigned long muls(unsigned long h) {
    static const long v[] = { 0, 1, -1, 3, -7, 65537, -65537, 0x7fffffff,
                              -0x80000000L, 0x100000001L, 0x7fffffffffffffffL,
                              (long)0x8000000000000000UL };
    static unsigned char flagbytes[16];
    for (unsigned i = 0; i < sizeof v / sizeof *v; i++)
    for (unsigned j = 0; j < sizeof v / sizeof *v; j++) {
        h = mix(h, (unsigned long)(v[i] * v[j]));                   // imul r64, r/m64
        h = mix(h, (unsigned)((int)v[i] * (int)v[j]));              // imul r32, r/m32
        h = mix(h, (unsigned long)(v[i] * 1337L));                  // imul r64, r/m64, imm32
        h = mix(h, (unsigned long)(v[i] * -5L));                    // imul r64, r/m64, imm8
        h = mix(h, (unsigned)((int)v[j] * 1000003));
        // setcc from a comparison, written to memory and to a register.
        for (int k = 0; k < 16; k++) flagbytes[k] = 0;
        flagbytes[0] = v[i] <  v[j];   flagbytes[1] = v[i] <= v[j];
        flagbytes[2] = v[i] == v[j];   flagbytes[3] = v[i] >  v[j];
        flagbytes[4] = (unsigned long)v[i] <  (unsigned long)v[j];
        flagbytes[5] = (unsigned long)v[i] >= (unsigned long)v[j];
        flagbytes[6] = (int)v[i] != (int)v[j];
        flagbytes[7] = ((v[i] ^ v[j]) & 1) != 0;
        unsigned long packed = 0;
        for (int k = 0; k < 8; k++) packed |= (unsigned long)flagbytes[k] << (k * 8);
        h = mix(h, packed);
    }
    return h;
}

// A store writes its own width and no more, whatever is in the rest of the
// register it comes from. Every neighbour starts nonzero, so a store that
// is too wide changes something; and the stores repeat, so all but the first
// are made in line by compiled code.
__attribute__((noinline)) static unsigned long narrow_stores(unsigned long h) {
    static unsigned words[9] = { 11, 22, 33, 44, 55, 66, 77, 88, 99 };
    static unsigned char bytes8[9] = { 1, 2, 3, 4, 5, 6, 7, 8, 9 };
    for (int k = 0; k < 8; k++) {
        __asm__ volatile("movabsq $0xa5a5a5a5c3c3c3c3, %%rax\n\t"
                         "addq %2, %%rax\n\t"
                         "movl %%eax, (%0)\n\t"
                         "movb %%al, (%1)"
                         : : "r"(&words[k]), "r"(&bytes8[k]), "r"((unsigned long)k)
                         : "rax", "memory");
    }
    for (int k = 0; k < 9; k++) { h = mix(h, words[k]); h = mix(h, bytes8[k]); }
    return h;
}

int main(void) {
    unsigned long h = 0xcbf29ce484222325UL;
    h = bytes(h);
    h = widen(h);
    h = highbytes(h);
    h = immediates(h);
    h = shifts(h);
    h = muls(h);
    h = narrow_stores(h);
    for (int i = 0; i < 64; i++) h = mix(h, ub[i] ^ ((unsigned long)(unsigned char)sb[i] << 8));
    printf("checksum=%016lx\n", h);
    return (int)(h & 0x7f);
}
