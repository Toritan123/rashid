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
#include "cpu.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <math.h>
#include <unistd.h>
#include <sys/mman.h>

#define GUEST_STACK_SIZE (1u << 20)

static const char *rname[16] = {
    "rax","rcx","rdx","rbx","rsp","rbp","rsi","rdi",
    "r8","r9","r10","r11","r12","r13","r14","r15"
};

// ---------------------------------------------------------------- memory
// Guest addresses are offsets into the guest address space, never host
// pointers. Every access is bounds- and permission-checked, so a wild guest
// pointer stops the guest instead of corrupting rashid.

static void memfault(rsd_cpu *c, uint64_t a, const char *what) {
    if (!c->fault) {
        c->fault = what;
        c->fault_rip = c->cur_rip;
        c->fault_addr = a;
        c->fault_has_addr = true;
    }
    c->running = false;
}

// Distinguish "nothing is there" from "there, but not allowed" - the two mean
// very different things when debugging a guest.
static const char *why(const rsd_cpu *c, uint64_t a, uint64_t n, const char *act) {
    (void)act;
    if (!rsd_as_mapped(c->as, a, n)) return "not guest memory";
    return "guest memory protection";
}

static uint64_t ld(rsd_cpu *c, uint64_t a, int size) {
    if (!rsd_as_ok(c->as, a, (uint64_t)size, RSD_PROT_R)) {
        memfault(c, a, why(c, a, (uint64_t)size, "read"));
        return 0;
    }
    const void *h = rsd_g2h(c->as, a);
    switch (size) {
    case 1: return *(const uint8_t  *)h;
    case 2: return *(const uint16_t *)h;
    case 4: return *(const uint32_t *)h;
    default:return *(const uint64_t *)h;
    }
}

static void st(rsd_cpu *c, uint64_t a, uint64_t v, int size) {
    if (!rsd_as_ok(c->as, a, (uint64_t)size, RSD_PROT_W)) {
        memfault(c, a, why(c, a, (uint64_t)size, "write"));
        return;
    }
    void *h = rsd_g2h(c->as, a);
    switch (size) {
    case 1: *(uint8_t  *)h = (uint8_t)v;  break;
    case 2: *(uint16_t *)h = (uint16_t)v; break;
    case 4: *(uint32_t *)h = (uint32_t)v; break;
    default:*(uint64_t *)h = v;           break;
    }
}

static inline uint64_t trunc_sz(uint64_t v, int size) {
    switch (size) {
    case 1: return v & 0xff;
    case 2: return v & 0xffff;
    case 4: return v & 0xffffffffu;
    default:return v;
    }
}
static inline int64_t sext(uint64_t v, int size) {
    switch (size) {
    case 1: return (int8_t)v;
    case 2: return (int16_t)v;
    case 4: return (int32_t)v;
    default:return (int64_t)v;
    }
}

// ------------------------------------------------------------- registers
// Writes to a 32-bit register zero-extend; 8/16-bit writes preserve the rest.
static void setreg(rsd_cpu *c, int r, uint64_t v, int size) {
    switch (size) {
    case 1: c->r[r] = (c->r[r] & ~0xffull)   | (v & 0xff);   break;
    case 2: c->r[r] = (c->r[r] & ~0xffffull) | (v & 0xffff); break;
    case 4: c->r[r] = v & 0xffffffffu;                       break;
    default:c->r[r] = v;                                     break;
    }
}
static uint64_t getreg(const rsd_cpu *c, int r, int size) {
    return trunc_sz(c->r[r], size);
}

// 8-bit register file differs depending on whether a REX prefix is present:
// without REX, indices 4..7 name ah/ch/dh/bh.
static uint64_t getreg8(const rsd_cpu *c, int r, bool rex) {
    if (!rex && r >= 4 && r < 8) return (c->r[r - 4] >> 8) & 0xff;
    return c->r[r] & 0xff;
}
static void setreg8(rsd_cpu *c, int r, uint64_t v, bool rex) {
    if (!rex && r >= 4 && r < 8) {
        c->r[r - 4] = (c->r[r - 4] & ~0xff00ull) | ((v & 0xff) << 8);
        return;
    }
    c->r[r] = (c->r[r] & ~0xffull) | (v & 0xff);
}

// ----------------------------------------------------------------- flags
static void set_pzs(rsd_cpu *c, uint64_t res, int size) {
    res = trunc_sz(res, size);
    c->flags &= ~(F_ZF | F_SF | F_PF);
    if (!res) c->flags |= F_ZF;
    if (res >> (size * 8 - 1)) c->flags |= F_SF;
    if (!(__builtin_popcount(res & 0xff) & 1)) c->flags |= F_PF;
}
static void flags_logic(rsd_cpu *c, uint64_t res, int size) {
    c->flags &= ~(F_CF | F_OF);
    set_pzs(c, res, size);
}
static void flags_add(rsd_cpu *c, uint64_t a, uint64_t b, uint64_t res, int size) {
    a = trunc_sz(a, size); b = trunc_sz(b, size);
    uint64_t r = trunc_sz(res, size);
    uint64_t sign = 1ull << (size * 8 - 1);
    c->flags &= ~(F_CF | F_OF | F_AF);
    if (r < a) c->flags |= F_CF;
    if (~(a ^ b) & (a ^ r) & sign) c->flags |= F_OF;
    if ((a ^ b ^ r) & 0x10) c->flags |= F_AF;
    set_pzs(c, r, size);
}
static void flags_sub(rsd_cpu *c, uint64_t a, uint64_t b, uint64_t res, int size) {
    a = trunc_sz(a, size); b = trunc_sz(b, size);
    uint64_t r = trunc_sz(res, size);
    uint64_t sign = 1ull << (size * 8 - 1);
    c->flags &= ~(F_CF | F_OF | F_AF);
    if (a < b) c->flags |= F_CF;
    if ((a ^ b) & (a ^ r) & sign) c->flags |= F_OF;
    if ((a ^ b ^ r) & 0x10) c->flags |= F_AF;
    set_pzs(c, r, size);
}

// ADC and SBB fold the carry in, which also changes how CF comes out: with a
// carry in, a result equal to the first operand still means it wrapped.
static void flags_adc(rsd_cpu *c, uint64_t a, uint64_t b, uint64_t cf,
                      uint64_t res, int size) {
    a = trunc_sz(a, size); b = trunc_sz(b, size);
    uint64_t r = trunc_sz(res, size);
    uint64_t sign = 1ull << (size * 8 - 1);
    c->flags &= ~(F_CF | F_OF | F_AF);
    if (r < a || (cf && r == a)) c->flags |= F_CF;
    if (~(a ^ b) & (a ^ r) & sign) c->flags |= F_OF;
    if ((a ^ b ^ r) & 0x10) c->flags |= F_AF;
    set_pzs(c, r, size);
}

static void flags_sbb(rsd_cpu *c, uint64_t a, uint64_t b, uint64_t cf,
                      uint64_t res, int size) {
    a = trunc_sz(a, size); b = trunc_sz(b, size);
    uint64_t r = trunc_sz(res, size);
    uint64_t sign = 1ull << (size * 8 - 1);
    c->flags &= ~(F_CF | F_OF | F_AF);
    if (a < b || (cf && a == b)) c->flags |= F_CF;
    if ((a ^ b) & (a ^ r) & sign) c->flags |= F_OF;
    if ((a ^ b ^ r) & 0x10) c->flags |= F_AF;
    set_pzs(c, r, size);
}

static bool cond(const rsd_cpu *c, int cc) {
    bool cf = c->flags & F_CF, zf = c->flags & F_ZF;
    bool sf = !!(c->flags & F_SF), of = !!(c->flags & F_OF), pf = c->flags & F_PF;
    switch (cc >> 1) {
    case 0: return (cc & 1) ? !of : of;                       // o / no
    case 1: return (cc & 1) ? !cf : cf;                       // b / ae
    case 2: return (cc & 1) ? !zf : zf;                       // e / ne
    case 3: return (cc & 1) ? !(cf || zf) : (cf || zf);       // be / a
    case 4: return (cc & 1) ? !sf : sf;                       // s / ns
    case 5: return (cc & 1) ? !pf : pf;                       // p / np
    case 6: return (cc & 1) ? (sf == of) : (sf != of);        // l / ge
    default:return (cc & 1) ? (!zf && sf == of) : (zf || sf != of); // le / g
    }
}

// ---------------------------------------------------------------- decode
typedef struct {
    bool     is_reg;
    int      reg;    // register index when is_reg
    uint64_t addr;   // effective address otherwise
    uint64_t seg;    // segment base to add on access (LEA ignores it)
} opnd;

typedef struct {
    rsd_cpu *c;
    uint8_t *p;      // host cursor into the guest's code page
    uint8_t *p0;     // where this instruction started (host)
    uint64_t rip0;   // where this instruction started (guest)
    uint8_t  rex;
    bool     has_rex;
    int      opsize; // 1/2/4/8
    bool     addr32;
    uint64_t seg_base;
    uint8_t  mand;   // mandatory SSE prefix: 0, 0x66, 0xF2 or 0xF3
} dec;

static uint8_t  fetch8 (dec *d) { return *d->p++; }
static uint16_t fetch16(dec *d) { uint16_t v; memcpy(&v, d->p, 2); d->p += 2; return v; }
static uint32_t fetch32(dec *d) { uint32_t v; memcpy(&v, d->p, 4); d->p += 4; return v; }
static uint64_t fetch64(dec *d) { uint64_t v; memcpy(&v, d->p, 8); d->p += 8; return v; }

// Guest address just past the bytes consumed so far.
static uint64_t dec_rip(const dec *d) { return d->rip0 + (uint64_t)(d->p - d->p0); }

// Decodes ModRM (+SIB, +displacement). Returns the reg field; fills *rm.
static int modrm(dec *d, opnd *rm) {
    uint8_t m   = fetch8(d);
    int mod     = m >> 6;
    int reg     = ((m >> 3) & 7) | ((d->rex & 4) ? 8 : 0);
    int rm_bits = m & 7;

    if (mod == 3) {
        rm->is_reg = true;
        rm->reg    = rm_bits | ((d->rex & 1) ? 8 : 0);
        rm->seg    = 0;
        return reg;
    }

    rm->is_reg = false;
    rm->seg    = d->seg_base;
    uint64_t addr = 0;

    if (rm_bits == 4) { // SIB
        uint8_t sib = fetch8(d);
        int scale = sib >> 6;
        int index = ((sib >> 3) & 7) | ((d->rex & 2) ? 8 : 0);
        int base  = (sib & 7) | ((d->rex & 1) ? 8 : 0);

        if (index != RSP) addr += d->c->r[index] << scale; // index==4 means none
        if ((sib & 7) == 5 && mod == 0) addr += (int64_t)(int32_t)fetch32(d);
        else                            addr += d->c->r[base];
    } else if (rm_bits == 5 && mod == 0) {
        // RIP-relative: displacement is from the end of the instruction, so
        // it is resolved by the caller after all immediates are consumed.
        int32_t disp = (int32_t)fetch32(d);
        rm->addr = (uint64_t)disp;   // marker; fixed up below
        rm->reg  = -1;               // signals "rip-relative pending"
        return reg;
    } else {
        addr = d->c->r[rm_bits | ((d->rex & 1) ? 8 : 0)];
    }

    if (mod == 1)      addr += (int64_t)(int8_t)fetch8(d);
    else if (mod == 2) addr += (int64_t)(int32_t)fetch32(d);

    rm->addr = addr;
    rm->reg  = -2; // ordinary memory operand
    return reg;
}

// Resolve a pending RIP-relative operand once the instruction length is known.
// Call this only after every immediate has been fetched: the displacement is
// measured from the end of the entire instruction, immediates included.
static void fixup_rip(dec *d, opnd *rm) {
    if (!rm->is_reg && rm->reg == -1)
        rm->addr = dec_rip(d) + (int64_t)(int32_t)rm->addr;
}

static uint64_t rd(rsd_cpu *c, const opnd *o, int size, bool has_rex) {
    if (o->is_reg) return size == 1 ? getreg8(c, o->reg, has_rex)
                                    : getreg(c, o->reg, size);
    return ld(c, o->addr + o->seg, size);
}
static void wr(rsd_cpu *c, const opnd *o, uint64_t v, int size, bool has_rex) {
    if (o->is_reg) { if (size == 1) setreg8(c, o->reg, v, has_rex);
                     else           setreg(c, o->reg, v, size); return; }
    st(c, o->addr + o->seg, v, size);
}

static void push(rsd_cpu *c, uint64_t v) { c->r[RSP] -= 8; st(c, c->r[RSP], v, 8); }
static uint64_t pop(rsd_cpu *c) { uint64_t v = ld(c, c->r[RSP], 8); c->r[RSP] += 8; return v; }

// --------------------------------------------------------------- syscall
// macOS encodes the syscall class in the high byte of the number: 1 = Mach
// trap, 2 = BSD, 3 = machine-dependent. Args are in rdi/rsi/rdx/r10/r8/r9,
// and errors come back as CF set with errno in rax.
#define SYSCALL_CLASS(n)  (((n) >> 24) & 0xff)
#define SYSCALL_NUMBER(n) ((n) & 0xffffff)
#define CLASS_MACH 1
#define CLASS_BSD  2
#define CLASS_MDEP 3

static void syscall_ret(rsd_cpu *c, int64_t ret) {
    if (ret < 0) { c->flags |= F_CF;  c->r[RAX] = (uint64_t)(-ret); }
    else         { c->flags &= ~F_CF; c->r[RAX] = (uint64_t)ret; }
}

static void unimplemented_syscall(rsd_cpu *c, uint64_t klass, uint64_t n) {
    static const char *names[] = { "?", "mach", "bsd", "mdep" };
    c->fault = "unimplemented syscall";
    c->fault_rip = c->rip;
    c->running = false;
    fprintf(stderr, "rashid: unimplemented %s syscall %llu (rax=0x%llx) at 0x%llx\n",
            klass < 4 ? names[klass] : "?", n, c->r[RAX], c->rip);
}

static void do_syscall(rsd_cpu *c) {
    uint64_t klass = SYSCALL_CLASS(c->r[RAX]);
    uint64_t n     = SYSCALL_NUMBER(c->r[RAX]);
    uint64_t a0 = c->r[RDI], a1 = c->r[RSI], a2 = c->r[RDX];

    if (klass == CLASS_MDEP) {
        switch (n) {
        case 3:  // thread_fast_set_cthread_self: rdi is the new %gs base.
                 // This is how a thread's TSD block is installed; everything
                 // from pthread_getspecific to malloc's per-thread caches
                 // reads through it.
            c->gs_base = a0;
            syscall_ret(c, 0);
            return;
        default:
            unimplemented_syscall(c, klass, n);
            return;
        }
    }

    if (klass != CLASS_BSD) {
        unimplemented_syscall(c, klass, n);
        return;
    }

    int64_t ret;
    switch (n) {
    case 1: // exit
        c->running = false;
        c->exit_code = (int)a0;
        return;
    case 3:
        if (!rsd_as_ok(c->as, a1, a2, RSD_PROT_W)) {
            memfault(c, a1, "read() buffer not writable in guest space"); return;
        }
        ret = read((int)a0, rsd_g2h(c->as, a1), (size_t)a2);
        break;
    case 4:
        if (!rsd_as_ok(c->as, a1, a2, RSD_PROT_R)) {
            memfault(c, a1, "write() buffer not readable in guest space"); return;
        }
        ret = write((int)a0, rsd_g2h(c->as, a1), (size_t)a2);
        break;
    case 6: ret = close((int)a0); break;

    case 73:  // munmap
        ret = rsd_as_unmap(c->as, a0, a1);
        break;
    case 74:  // mprotect
        ret = rsd_as_protect(c->as, a0, a1, (int)(a2 & 7));
        break;
    case 75:  // madvise - advisory only, nothing to do
        ret = 0;
        break;
    case 197: {  // mmap
        uint64_t flags = c->r[R10];
        int fd = (int)c->r[R8];
        if (!(flags & 0x1000) && fd >= 0) {   // MAP_ANON
            // File-backed guest mappings are not needed yet: rashid loads
            // images itself rather than running the guest's dyld.
            unimplemented_syscall(c, klass, n);
            return;
        }
        uint64_t got = rsd_as_map(c->as, a0, a1, (int)(a2 & 7), (flags & 0x10) != 0);
        if (!got) { syscall_ret(c, -ENOMEM); return; }
        c->flags &= ~F_CF;
        c->r[RAX] = got;
        return;
    }
    default:
        unimplemented_syscall(c, klass, n);
        return;
    }
    syscall_ret(c, ret);
}


// ------------------------------------------------------------------- SSE
// Measured against compiler-generated x86_64 code, SIMD use is overwhelmingly
// 16-byte moves and scalar double arithmetic - movups and movaps alone are
// three quarters of it, and packed integer SSE barely appears. XMM support is
// unavoidable in any case: the x86_64 ABI passes floating-point arguments in
// xmm0-7 and returns in xmm0.
//
// The mandatory prefix selects the variant of a 0F opcode:
//   none = packed single, 66 = packed double / integer,
//   F3   = scalar single, F2 = scalar double.

static void ld128(rsd_cpu *c, uint64_t a, rsd_xmm *o) {
    if (!rsd_as_ok(c->as, a, 16, RSD_PROT_R)) {
        memfault(c, a, why(c, a, 16, "read"));
        memset(o, 0, sizeof *o);
        return;
    }
    memcpy(o, rsd_g2h(c->as, a), 16);
}

static void st128(rsd_cpu *c, uint64_t a, const rsd_xmm *v) {
    if (!rsd_as_ok(c->as, a, 16, RSD_PROT_W)) {
        memfault(c, a, why(c, a, 16, "write"));
        return;
    }
    memcpy(rsd_g2h(c->as, a), v, 16);
}

static void xrd(rsd_cpu *c, const opnd *o, rsd_xmm *out) {
    if (o->is_reg) *out = c->xmm[o->reg];
    else           ld128(c, o->addr + o->seg, out);
}

static void xwr(rsd_cpu *c, const opnd *o, const rsd_xmm *v) {
    if (o->is_reg) c->xmm[o->reg] = *v;
    else           st128(c, o->addr + o->seg, v);
}

// Scalar operand: `bytes` from a register's low lane or from memory.
static uint64_t xrd_lo(rsd_cpu *c, const opnd *o, int bytes) {
    if (o->is_reg) return bytes == 4 ? c->xmm[o->reg].d[0] : c->xmm[o->reg].q[0];
    return ld(c, o->addr + o->seg, bytes);
}

// COMISD/UCOMISD: unordered sets ZF, PF and CF together; OF, AF and SF clear.
static void comis(rsd_cpu *c, double a, double b) {
    c->flags &= ~(F_OF | F_AF | F_SF | F_ZF | F_PF | F_CF);
    if (isnan(a) || isnan(b)) c->flags |= F_ZF | F_PF | F_CF;
    else if (a < b)           c->flags |= F_CF;
    else if (a == b)          c->flags |= F_ZF;
}

// CMPPS/CMPPD/CMPSS/CMPSD predicates. C's == on floating point compiles to
// these rather than to ucomis, so they turn up in ordinary code constantly.
static bool fcmp(uint8_t pred, double x, double y) {
    bool un = isnan(x) || isnan(y);
    switch (pred & 7) {
    case 0: return !un && x == y;      // EQ  (ordered)
    case 1: return !un && x <  y;      // LT
    case 2: return !un && x <= y;      // LE
    case 3: return un;                 // UNORD
    case 4: return un || x != y;       // NEQ (unordered)
    case 5: return un || !(x <  y);    // NLT
    case 6: return un || !(x <= y);    // NLE
    default: return !un;               // ORD
    }
}

static double fbin(uint8_t op2, double x, double y) {
    switch (op2) {
    case 0x58: return x + y;
    case 0x59: return x * y;
    case 0x5C: return x - y;
    case 0x5E: return x / y;
    // MIN/MAX return the second operand when the inputs are unordered or
    // equal, which is why these are not written as fmin/fmax.
    case 0x5D: return x < y ? x : y;
    default:   return x > y ? x : y;
    }
}

// Returns true if the opcode was handled. Must not consume any bytes when it
// returns false, so unknown opcodes fall through to the caller untouched.
static bool sse_exec(rsd_cpu *c, dec *d, uint8_t op2, uint64_t start) {
    opnd rm;
    int reg;
    rsd_xmm a, b;

    switch (op2) {

    // ---- 16-byte and scalar moves ----
    case 0x10: case 0x11:            // movups/movss/movupd/movsd
    case 0x28: case 0x29: {          // movaps/movapd
        bool store = (op2 & 1);      // .11 and .29 write the r/m operand
        reg = modrm(d, &rm);
        fixup_rip(d, &rm);
        c->rip = dec_rip(d);
        opnd ro = { .is_reg = true, .reg = reg };
        opnd *dst = store ? &rm : &ro, *src = store ? &ro : &rm;

        if (op2 <= 0x11 && (d->mand == 0xF3 || d->mand == 0xF2)) {
            int n = (d->mand == 0xF3) ? 4 : 8;   // movss / movsd
            uint64_t v = xrd_lo(c, src, n);
            if (dst->is_reg) {
                if (src->is_reg) {
                    // register to register keeps the rest of the destination
                    if (n == 4) c->xmm[dst->reg].d[0] = (uint32_t)v;
                    else        c->xmm[dst->reg].q[0] = v;
                } else {
                    memset(&c->xmm[dst->reg], 0, sizeof(rsd_xmm));
                    if (n == 4) c->xmm[dst->reg].d[0] = (uint32_t)v;
                    else        c->xmm[dst->reg].q[0] = v;
                }
            } else {
                st(c, dst->addr + dst->seg, v, n);
            }
            return true;
        }
        xrd(c, src, &a);
        xwr(c, dst, &a);
        return true;
    }

    case 0x6F: case 0x7F: {          // movdqa (66) / movdqu (F3)
        if (d->mand != 0x66 && d->mand != 0xF3) return false;
        bool store = (op2 == 0x7F);
        reg = modrm(d, &rm);
        fixup_rip(d, &rm);
        c->rip = dec_rip(d);
        opnd ro = { .is_reg = true, .reg = reg };
        xrd(c, store ? &ro : &rm, &a);
        xwr(c, store ? &rm : &ro, &a);
        return true;
    }

    case 0x6E: {                     // movd/movq xmm, r/m
        if (d->mand != 0x66) return false;
        reg = modrm(d, &rm);
        fixup_rip(d, &rm);
        c->rip = dec_rip(d);
        int n = (d->rex & 8) ? 8 : 4;
        uint64_t v = rm.is_reg ? getreg(c, rm.reg, n) : ld(c, rm.addr + rm.seg, n);
        memset(&c->xmm[reg], 0, sizeof(rsd_xmm));
        c->xmm[reg].q[0] = v;
        return true;
    }

    case 0x7E: {                     // movd/movq r/m, xmm (66) or movq xmm (F3)
        reg = modrm(d, &rm);
        fixup_rip(d, &rm);
        c->rip = dec_rip(d);
        if (d->mand == 0xF3) {       // movq xmm, xmm/m64 - zeroes the top half
            uint64_t v = xrd_lo(c, &rm, 8);
            memset(&c->xmm[reg], 0, sizeof(rsd_xmm));
            c->xmm[reg].q[0] = v;
            return true;
        }
        if (d->mand != 0x66) return false;
        int n = (d->rex & 8) ? 8 : 4;
        uint64_t v = c->xmm[reg].q[0];
        if (rm.is_reg) setreg(c, rm.reg, v, n);
        else           st(c, rm.addr + rm.seg, v, n);
        return true;
    }

    case 0xD6: {                     // movq xmm/m64, xmm
        if (d->mand != 0x66) return false;
        reg = modrm(d, &rm);
        fixup_rip(d, &rm);
        c->rip = dec_rip(d);
        if (rm.is_reg) {
            memset(&c->xmm[rm.reg], 0, sizeof(rsd_xmm));
            c->xmm[rm.reg].q[0] = c->xmm[reg].q[0];
        } else {
            st(c, rm.addr + rm.seg, c->xmm[reg].q[0], 8);
        }
        return true;
    }

    // ---- bitwise: 128 bits regardless of the prefix ----
    case 0x54: case 0x55: case 0x56: case 0x57:   // and/andn/or/xor ps,pd
    case 0xDB: case 0xEB: case 0xEF: {            // pand/por/pxor
        if (op2 >= 0xDB && d->mand != 0x66) return false;
        reg = modrm(d, &rm);
        fixup_rip(d, &rm);
        c->rip = dec_rip(d);
        xrd(c, &rm, &b);
        a = c->xmm[reg];
        for (int i = 0; i < 2; i++) {
            switch (op2) {
            case 0x54: case 0xDB: a.q[i] &= b.q[i]; break;
            case 0x55:            a.q[i] = ~a.q[i] & b.q[i]; break;
            case 0x56: case 0xEB: a.q[i] |= b.q[i]; break;
            default:              a.q[i] ^= b.q[i]; break;
            }
        }
        c->xmm[reg] = a;
        return true;
    }

    // ---- arithmetic ----
    case 0x51: case 0x58: case 0x59:
    case 0x5C: case 0x5D: case 0x5E: case 0x5F: {
        reg = modrm(d, &rm);
        fixup_rip(d, &rm);
        c->rip = dec_rip(d);
        a = c->xmm[reg];
        if (d->mand == 0xF2) {                      // scalar double
            union { uint64_t u; double f; } s;
            s.u = xrd_lo(c, &rm, 8);
            a.lf[0] = (op2 == 0x51) ? sqrt(s.f) : fbin(op2, a.lf[0], s.f);
        } else if (d->mand == 0xF3) {               // scalar single
            union { uint32_t u; float f; } s;
            s.u = (uint32_t)xrd_lo(c, &rm, 4);
            a.f[0] = (op2 == 0x51) ? sqrtf(s.f)
                                   : (float)fbin(op2, a.f[0], s.f);
        } else if (d->mand == 0x66) {               // packed double
            xrd(c, &rm, &b);
            for (int i = 0; i < 2; i++)
                a.lf[i] = (op2 == 0x51) ? sqrt(b.lf[i]) : fbin(op2, a.lf[i], b.lf[i]);
        } else {                                    // packed single
            xrd(c, &rm, &b);
            for (int i = 0; i < 4; i++)
                a.f[i] = (op2 == 0x51) ? sqrtf(b.f[i])
                                       : (float)fbin(op2, a.f[i], b.f[i]);
        }
        c->xmm[reg] = a;
        return true;
    }

    // ---- compare ----
    case 0x2E: case 0x2F: {          // ucomiss/ucomisd, comiss/comisd
        reg = modrm(d, &rm);
        fixup_rip(d, &rm);
        c->rip = dec_rip(d);
        if (d->mand == 0x66) {
            union { uint64_t u; double f; } s;
            s.u = xrd_lo(c, &rm, 8);
            comis(c, c->xmm[reg].lf[0], s.f);
        } else {
            union { uint32_t u; float f; } s;
            s.u = (uint32_t)xrd_lo(c, &rm, 4);
            comis(c, (double)c->xmm[reg].f[0], (double)s.f);
        }
        return true;
    }

    // ---- conversions ----
    case 0x2A: {                     // cvtsi2ss / cvtsi2sd
        if (d->mand != 0xF2 && d->mand != 0xF3) return false;
        reg = modrm(d, &rm);
        fixup_rip(d, &rm);
        c->rip = dec_rip(d);
        int n = (d->rex & 8) ? 8 : 4;
        int64_t v = (int64_t)sext(rm.is_reg ? getreg(c, rm.reg, n)
                                            : ld(c, rm.addr + rm.seg, n), n);
        if (d->mand == 0xF2) c->xmm[reg].lf[0] = (double)v;
        else                 c->xmm[reg].f[0]  = (float)v;
        return true;
    }

    case 0x2C: case 0x2D: {          // cvt(t)ss2si / cvt(t)sd2si
        if (d->mand != 0xF2 && d->mand != 0xF3) return false;
        reg = modrm(d, &rm);
        fixup_rip(d, &rm);
        c->rip = dec_rip(d);
        double v;
        if (d->mand == 0xF2) { union { uint64_t u; double f; } s;
                               s.u = xrd_lo(c, &rm, 8); v = s.f; }
        else                 { union { uint32_t u; float f; } s;
                               s.u = (uint32_t)xrd_lo(c, &rm, 4); v = (double)s.f; }
        // 2C truncates, 2D rounds to nearest even (the default rounding mode).
        double r = (op2 == 0x2C) ? trunc(v) : nearbyint(v);
        int n = (d->rex & 8) ? 8 : 4;
        setreg(c, reg, (uint64_t)(int64_t)r, n);
        return true;
    }

    case 0x5A: {                     // cvtss2sd / cvtsd2ss
        if (d->mand != 0xF2 && d->mand != 0xF3) return false;
        reg = modrm(d, &rm);
        fixup_rip(d, &rm);
        c->rip = dec_rip(d);
        if (d->mand == 0xF3) {       // single -> double
            union { uint32_t u; float f; } s;
            s.u = (uint32_t)xrd_lo(c, &rm, 4);
            c->xmm[reg].lf[0] = (double)s.f;
        } else {                     // double -> single
            union { uint64_t u; double f; } s;
            s.u = xrd_lo(c, &rm, 8);
            c->xmm[reg].f[0] = (float)s.f;
        }
        return true;
    }

    // ---- packed integer compare and mask ----
    case 0x74: case 0x76: {          // pcmpeqb / pcmpeqd
        if (d->mand != 0x66) return false;
        reg = modrm(d, &rm);
        fixup_rip(d, &rm);
        c->rip = dec_rip(d);
        xrd(c, &rm, &b);
        a = c->xmm[reg];
        if (op2 == 0x74) for (int i = 0; i < 16; i++) a.b[i] = a.b[i] == b.b[i] ? 0xff : 0;
        else             for (int i = 0; i < 4;  i++) a.d[i] = a.d[i] == b.d[i] ? 0xffffffffu : 0;
        c->xmm[reg] = a;
        return true;
    }

    case 0xD7: {                     // pmovmskb
        if (d->mand != 0x66) return false;
        reg = modrm(d, &rm);
        fixup_rip(d, &rm);
        c->rip = dec_rip(d);
        if (!rm.is_reg) return false;
        uint64_t m = 0;
        for (int i = 0; i < 16; i++)
            if (c->xmm[rm.reg].b[i] & 0x80) m |= 1ull << i;
        setreg(c, reg, m, 8);
        return true;
    }

    case 0x70: {                     // pshufd
        if (d->mand != 0x66) return false;
        reg = modrm(d, &rm);
        uint8_t imm = fetch8(d);
        fixup_rip(d, &rm);
        c->rip = dec_rip(d);
        xrd(c, &rm, &b);
        for (int i = 0; i < 4; i++) a.d[i] = b.d[(imm >> (2 * i)) & 3];
        c->xmm[reg] = a;
        return true;
    }

    case 0xC2: {                     // cmpps / cmppd / cmpss / cmpsd
        reg = modrm(d, &rm);
        uint8_t imm = fetch8(d);
        fixup_rip(d, &rm);
        c->rip = dec_rip(d);
        a = c->xmm[reg];
        if (d->mand == 0xF2) {                      // scalar double
            union { uint64_t u; double f; } t;
            t.u = xrd_lo(c, &rm, 8);
            a.q[0] = fcmp(imm, a.lf[0], t.f) ? ~0ull : 0;
        } else if (d->mand == 0xF3) {               // scalar single
            union { uint32_t u; float f; } t;
            t.u = (uint32_t)xrd_lo(c, &rm, 4);
            a.d[0] = fcmp(imm, (double)a.f[0], (double)t.f) ? 0xffffffffu : 0;
        } else if (d->mand == 0x66) {               // packed double
            xrd(c, &rm, &b);
            for (int i = 0; i < 2; i++)
                a.q[i] = fcmp(imm, a.lf[i], b.lf[i]) ? ~0ull : 0;
        } else {                                    // packed single
            xrd(c, &rm, &b);
            for (int i = 0; i < 4; i++)
                a.d[i] = fcmp(imm, (double)a.f[i], (double)b.f[i]) ? 0xffffffffu : 0;
        }
        c->xmm[reg] = a;
        return true;
    }

    case 0xC5: {                     // pextrw
        if (d->mand != 0x66) return false;
        reg = modrm(d, &rm);
        uint8_t imm = fetch8(d);
        fixup_rip(d, &rm);
        c->rip = dec_rip(d);
        if (!rm.is_reg) return false;
        setreg(c, reg, c->xmm[rm.reg].w[imm & 7], 8);
        return true;
    }

    default:
        return false;
    }
    (void)start;
}

// ------------------------------------------------------------------ core
static void step(rsd_cpu *c) {
    // 15 bytes is the longest possible x86 instruction.
    if (!rsd_as_ok(c->as, c->rip, 1, RSD_PROT_X)) {
        memfault(c, c->rip, why(c, c->rip, 1, "execute"));
        return;
    }
    int imp = rsd_stub_index(c->stubs, c->rip);
    if (imp >= 0) {
        c->fault = "call into an import that has no thunk yet";
        c->fault_rip = c->rip;
        c->fault_import = imp;
        c->running = false;
        return;
    }

    uint8_t *host = rsd_g2h(c->as, c->rip);
    dec d = { .c = c, .p = host, .p0 = host, .rip0 = c->rip, .opsize = 4 };
    uint64_t start = c->rip;
    c->cur_rip = start;   // ADVANCE() moves c->rip early, so faults use this

    // --- prefixes ---
    for (;;) {
        uint8_t b = *d.p;
        // 66/F2/F3 are operand-size or rep prefixes for general-purpose
        // instructions and variant selectors for SSE ones; record both roles.
        if (b == 0x66)      { d.opsize = 2; d.mand = 0x66; d.p++; }
        else if (b == 0x67) { d.addr32 = true; d.p++; }
        else if (b == 0xF2 || b == 0xF3) { d.mand = b; d.p++; }
        // In 64-bit mode cs/ds/es/ss overrides are ignored.
        else if (b == 0x2E || b == 0x36 || b == 0x3E || b == 0x26) { d.p++; }
        else if (b == 0x64) { d.seg_base = c->fs_base; d.p++; }
        else if (b == 0x65) { d.seg_base = c->gs_base; d.p++; }
        else break;
    }
    if ((*d.p & 0xF0) == 0x40) {
        d.rex = *d.p++; d.has_rex = true;
        if (d.rex & 8) d.opsize = 8;
    }

    uint8_t op = fetch8(&d);
    opnd rm; int reg;
    int sz = d.opsize;

#define ADVANCE() do { c->rip = dec_rip(&d); } while (0)
#define ALU(name, calc, flagfn)                                              \
    do {                                                                     \
        uint64_t a = rd(c, &rm, sz, d.has_rex);                              \
        uint64_t b = src;                                                    \
        uint64_t r = (calc);                                                 \
        flagfn;                                                              \
        if (!store_none) wr(c, &rm, r, sz, d.has_rex);                       \
    } while (0)

    switch (op) {

    // ---- ALU: op r/m, r  and  op r, r/m ----
    case 0x00: case 0x01: case 0x08: case 0x09: case 0x20: case 0x21:
    case 0x28: case 0x29: case 0x30: case 0x31: case 0x38: case 0x39:
    case 0x02: case 0x03: case 0x0A: case 0x0B: case 0x22: case 0x23:
    case 0x2A: case 0x2B: case 0x32: case 0x33: case 0x3A: case 0x3B:
    case 0x10: case 0x11: case 0x12: case 0x13:
    case 0x18: case 0x19: case 0x1A: case 0x1B:
    case 0x84: case 0x85: case 0x88: case 0x89: case 0x8A: case 0x8B: {
        bool byte = !(op & 1);
        if (op == 0x84) byte = true;
        if (op == 0x88 || op == 0x8A) byte = true;
        if (byte) sz = 1;
        bool to_reg = (op & 2) && op != 0x84 && op != 0x85;
        reg = modrm(&d, &rm);
        fixup_rip(&d, &rm);
        ADVANCE();

        opnd ro = { .is_reg = true, .reg = reg };
        opnd *dst = to_reg ? &ro : &rm;
        opnd *src_o = to_reg ? &rm : &ro;
        uint64_t a = rd(c, dst, sz, d.has_rex);
        uint64_t b = rd(c, src_o, sz, d.has_rex);
        uint64_t r;

        switch (op & ~3) {
        case 0x00: r = a + b; flags_add(c, a, b, r, sz); wr(c, dst, r, sz, d.has_rex); break;
        case 0x10: { uint64_t cf = (c->flags & F_CF) ? 1 : 0;
                     r = a + b + cf; flags_adc(c, a, b, cf, r, sz);
                     wr(c, dst, r, sz, d.has_rex); break; }
        case 0x18: { uint64_t cf = (c->flags & F_CF) ? 1 : 0;
                     r = a - b - cf; flags_sbb(c, a, b, cf, r, sz);
                     wr(c, dst, r, sz, d.has_rex); break; }
        case 0x08: r = a | b; flags_logic(c, r, sz);     wr(c, dst, r, sz, d.has_rex); break;
        case 0x20: r = a & b; flags_logic(c, r, sz);     wr(c, dst, r, sz, d.has_rex); break;
        case 0x28: r = a - b; flags_sub(c, a, b, r, sz); wr(c, dst, r, sz, d.has_rex); break;
        case 0x30: r = a ^ b; flags_logic(c, r, sz);     wr(c, dst, r, sz, d.has_rex); break;
        case 0x38: r = a - b; flags_sub(c, a, b, r, sz); break;   // cmp
        default:
            if (op == 0x84 || op == 0x85) { r = a & b; flags_logic(c, r, sz); }
            else /* mov */                { wr(c, dst, b, sz, d.has_rex); }
            break;
        }
        return;
    }

    // ---- ALU: op eAX, imm ----
    case 0x04: case 0x05: case 0x0C: case 0x0D: case 0x24: case 0x25:
    case 0x2C: case 0x2D: case 0x34: case 0x35: case 0x3C: case 0x3D:
    case 0x14: case 0x15: case 0x1C: case 0x1D: {
        bool byte = !(op & 1);
        if (byte) sz = 1;
        uint64_t b = byte ? fetch8(&d) : (uint64_t)(int64_t)(int32_t)fetch32(&d);
        ADVANCE();
        uint64_t a = getreg(c, RAX, sz), r;
        switch (op & ~7) {
        case 0x00: r = a + b; flags_add(c, a, b, r, sz); setreg(c, RAX, r, sz); break;
        case 0x10: { uint64_t cf = (c->flags & F_CF) ? 1 : 0;
                     r = a + b + cf; flags_adc(c, a, b, cf, r, sz);
                     setreg(c, RAX, r, sz); break; }
        case 0x18: { uint64_t cf = (c->flags & F_CF) ? 1 : 0;
                     r = a - b - cf; flags_sbb(c, a, b, cf, r, sz);
                     setreg(c, RAX, r, sz); break; }
        case 0x08: r = a | b; flags_logic(c, r, sz);     setreg(c, RAX, r, sz); break;
        case 0x20: r = a & b; flags_logic(c, r, sz);     setreg(c, RAX, r, sz); break;
        case 0x28: r = a - b; flags_sub(c, a, b, r, sz); setreg(c, RAX, r, sz); break;
        case 0x30: r = a ^ b; flags_logic(c, r, sz);     setreg(c, RAX, r, sz); break;
        default:   r = a - b; flags_sub(c, a, b, r, sz); break; // cmp
        }
        return;
    }

    // ---- push/pop reg ----
    case 0x50: case 0x51: case 0x52: case 0x53:
    case 0x54: case 0x55: case 0x56: case 0x57:
        ADVANCE();
        push(c, c->r[(op & 7) | ((d.rex & 1) ? 8 : 0)]);
        return;
    case 0x58: case 0x59: case 0x5A: case 0x5B:
    case 0x5C: case 0x5D: case 0x5E: case 0x5F:
        ADVANCE();
        c->r[(op & 7) | ((d.rex & 1) ? 8 : 0)] = pop(c);
        return;

    case 0x63: { // movsxd r64, r/m32
        reg = modrm(&d, &rm); fixup_rip(&d, &rm); ADVANCE();
        setreg(c, reg, (uint64_t)(int64_t)(int32_t)rd(c, &rm, 4, d.has_rex), sz);
        return;
    }

    case 0x68: { uint64_t v = (uint64_t)(int64_t)(int32_t)fetch32(&d); ADVANCE(); push(c, v); return; }
    case 0x6A: { uint64_t v = (uint64_t)(int64_t)(int8_t)fetch8(&d);   ADVANCE(); push(c, v); return; }

    case 0x69: case 0x6B: { // imul r, r/m, imm
        reg = modrm(&d, &rm);
        uint64_t imm = (op == 0x6B) ? (uint64_t)(int64_t)(int8_t)fetch8(&d)
                                    : (uint64_t)(int64_t)(int32_t)fetch32(&d);
        fixup_rip(&d, &rm);
        ADVANCE();
        int64_t r = sext(rd(c, &rm, sz, d.has_rex), sz) * (int64_t)imm;
        setreg(c, reg, (uint64_t)r, sz);
        flags_logic(c, (uint64_t)r, sz);
        return;
    }

    // ---- jcc rel8 ----
    case 0x70: case 0x71: case 0x72: case 0x73: case 0x74: case 0x75:
    case 0x76: case 0x77: case 0x78: case 0x79: case 0x7A: case 0x7B:
    case 0x7C: case 0x7D: case 0x7E: case 0x7F: {
        int8_t rel = (int8_t)fetch8(&d);
        ADVANCE();
        if (cond(c, op & 0xF)) c->rip += (int64_t)rel;
        return;
    }

    // ---- grp1: op r/m, imm ----
    case 0x80: case 0x81: case 0x83: {
        if (op == 0x80) sz = 1;
        int ext = modrm(&d, &rm);
        ext &= 7;
        uint64_t b = (op == 0x81) ? (uint64_t)(int64_t)(int32_t)fetch32(&d)
                                  : (uint64_t)(int64_t)(int8_t)fetch8(&d);
        fixup_rip(&d, &rm);
        ADVANCE();
        uint64_t a = rd(c, &rm, sz, d.has_rex), r;
        switch (ext) {
        case 0: r = a + b; flags_add(c, a, b, r, sz); wr(c, &rm, r, sz, d.has_rex); break;
        case 1: r = a | b; flags_logic(c, r, sz);     wr(c, &rm, r, sz, d.has_rex); break;
        case 4: r = a & b; flags_logic(c, r, sz);     wr(c, &rm, r, sz, d.has_rex); break;
        case 5: r = a - b; flags_sub(c, a, b, r, sz); wr(c, &rm, r, sz, d.has_rex); break;
        case 6: r = a ^ b; flags_logic(c, r, sz);     wr(c, &rm, r, sz, d.has_rex); break;
        case 7: r = a - b; flags_sub(c, a, b, r, sz); break;    // cmp
        case 2: { uint64_t cf = (c->flags & F_CF) ? 1 : 0;
                  r = a + b + cf; flags_adc(c, a, b, cf, r, sz);
                  wr(c, &rm, r, sz, d.has_rex); break; }
        default: { uint64_t cf = (c->flags & F_CF) ? 1 : 0;   // 3 = sbb
                  r = a - b - cf; flags_sbb(c, a, b, cf, r, sz);
                  wr(c, &rm, r, sz, d.has_rex); break; }
        }
        return;
    }

    case 0x87: { // xchg r/m, r
        reg = modrm(&d, &rm); fixup_rip(&d, &rm); ADVANCE();
        opnd ro = { .is_reg = true, .reg = reg };
        uint64_t a = rd(c, &rm, sz, d.has_rex), b = rd(c, &ro, sz, d.has_rex);
        wr(c, &rm, b, sz, d.has_rex); wr(c, &ro, a, sz, d.has_rex);
        return;
    }

    case 0x8D: { // lea
        reg = modrm(&d, &rm); fixup_rip(&d, &rm); ADVANCE();
        if (rm.is_reg) { c->fault = "lea with register operand"; c->fault_rip = start;
                         c->running = false; return; }
        setreg(c, reg, rm.addr, sz);
        return;
    }

    case 0x8F: { modrm(&d, &rm); fixup_rip(&d, &rm); ADVANCE();
                 wr(c, &rm, pop(c), 8, d.has_rex); return; }

    case 0x90: ADVANCE(); return; // nop

    case 0x98: // cwde / cdqe
        ADVANCE();
        setreg(c, RAX, (uint64_t)sext(getreg(c, RAX, sz / 2), sz / 2), sz);
        return;
    case 0x99: // cdq / cqo
        ADVANCE();
        setreg(c, RDX, sext(getreg(c, RAX, sz), sz) < 0 ? ~0ull : 0, sz);
        return;

    // ---- mov r, imm ----
    case 0xB0: case 0xB1: case 0xB2: case 0xB3:
    case 0xB4: case 0xB5: case 0xB6: case 0xB7: {
        uint64_t v = fetch8(&d); ADVANCE();
        setreg8(c, (op & 7) | ((d.rex & 1) ? 8 : 0), v, d.has_rex);
        return;
    }
    case 0xB8: case 0xB9: case 0xBA: case 0xBB:
    case 0xBC: case 0xBD: case 0xBE: case 0xBF: {
        uint64_t v = (sz == 8) ? fetch64(&d) : (sz == 2 ? fetch16(&d) : fetch32(&d));
        ADVANCE();
        setreg(c, (op & 7) | ((d.rex & 1) ? 8 : 0), v, sz);
        return;
    }

    // ---- shifts ----
    case 0xC0: case 0xC1: case 0xD0: case 0xD1: case 0xD2: case 0xD3: {
        if (op == 0xC0 || op == 0xD0 || op == 0xD2) sz = 1;
        int ext = modrm(&d, &rm) & 7;
        uint64_t cnt;
        if (op == 0xC0 || op == 0xC1)      cnt = fetch8(&d);
        else if (op == 0xD0 || op == 0xD1) cnt = 1;
        else                               cnt = c->r[RCX] & 0xff;
        fixup_rip(&d, &rm);
        ADVANCE();
        cnt &= (sz == 8) ? 63 : 31;
        if (!cnt) return;
        uint64_t a = rd(c, &rm, sz, d.has_rex), r;
        switch (ext) {
        case 4: case 6: r = a << cnt; break;                 // shl/sal
        case 5: r = trunc_sz(a, sz) >> cnt; break;           // shr
        case 7: r = (uint64_t)(sext(a, sz) >> cnt); break;   // sar
        default: c->fault = "rotate not implemented"; c->fault_rip = start;
                 c->running = false; return;
        }
        flags_logic(c, r, sz);
        wr(c, &rm, r, sz, d.has_rex);
        return;
    }

    case 0xC2: { uint16_t n = fetch16(&d); (void)n; c->rip = pop(c); c->r[RSP] += n; return; }
    case 0xC3: c->rip = pop(c); return;

    case 0xC6: case 0xC7: { // mov r/m, imm
        if (op == 0xC6) sz = 1;
        modrm(&d, &rm);
        uint64_t v = (op == 0xC6) ? (uint64_t)fetch8(&d)
                                  : (uint64_t)(int64_t)(int32_t)fetch32(&d);
        fixup_rip(&d, &rm);
        ADVANCE();
        wr(c, &rm, v, sz, d.has_rex);
        return;
    }

    case 0xC9: // leave
        ADVANCE();
        c->r[RSP] = c->r[RBP];
        c->r[RBP] = pop(c);
        return;

    case 0xE8: { int32_t rel = (int32_t)fetch32(&d); ADVANCE();
                 push(c, c->rip); c->rip += (int64_t)rel; return; }
    case 0xE9: { int32_t rel = (int32_t)fetch32(&d); ADVANCE(); c->rip += (int64_t)rel; return; }
    case 0xEB: { int8_t  rel = (int8_t)fetch8(&d);   ADVANCE(); c->rip += (int64_t)rel; return; }

    // ---- grp3 ----
    case 0xF6: case 0xF7: {
        if (op == 0xF6) sz = 1;
        int ext = modrm(&d, &rm) & 7;
        uint64_t imm = 0;
        if (ext == 0 || ext == 1) imm = (op == 0xF6) ? fetch8(&d)
                                                     : (uint64_t)(int64_t)(int32_t)fetch32(&d);
        fixup_rip(&d, &rm);
        ADVANCE();
        uint64_t a = rd(c, &rm, sz, d.has_rex);
        switch (ext) {
        case 0: case 1: flags_logic(c, a & imm, sz); return;              // test
        case 2: wr(c, &rm, ~a, sz, d.has_rex); return;                    // not
        case 3: { uint64_t r = 0 - a; flags_sub(c, 0, a, r, sz);
                  wr(c, &rm, r, sz, d.has_rex); return; }                 // neg
        case 4: { __uint128_t p = (__uint128_t)trunc_sz(c->r[RAX], sz) * trunc_sz(a, sz);
                  setreg(c, RAX, (uint64_t)p, sz);
                  if (sz > 1) setreg(c, RDX, (uint64_t)(p >> (sz * 8)), sz);
                  return; }                                               // mul
        case 5: { __int128_t p = (__int128_t)sext(c->r[RAX], sz) * sext(a, sz);
                  setreg(c, RAX, (uint64_t)p, sz);
                  if (sz > 1) setreg(c, RDX, (uint64_t)((__uint128_t)p >> (sz * 8)), sz);
                  return; }                                               // imul
        case 6: { if (!a) { c->fault = "divide by zero"; c->fault_rip = start;
                            c->running = false; return; }
                  __uint128_t num = ((__uint128_t)trunc_sz(c->r[RDX], sz) << (sz * 8))
                                  | trunc_sz(c->r[RAX], sz);
                  setreg(c, RAX, (uint64_t)(num / trunc_sz(a, sz)), sz);
                  setreg(c, RDX, (uint64_t)(num % trunc_sz(a, sz)), sz);
                  return; }                                               // div
        case 7: { if (!a) { c->fault = "divide by zero"; c->fault_rip = start;
                            c->running = false; return; }
                  __int128_t num = ((__int128_t)sext(c->r[RDX], sz) << (sz * 8))
                                 | trunc_sz(c->r[RAX], sz);
                  setreg(c, RAX, (uint64_t)(num / sext(a, sz)), sz);
                  setreg(c, RDX, (uint64_t)(num % sext(a, sz)), sz);
                  return; }                                               // idiv
        }
        return;
    }

    // ---- grp5 ----
    case 0xFE: case 0xFF: {
        if (op == 0xFE) sz = 1;
        int ext = modrm(&d, &rm) & 7;
        fixup_rip(&d, &rm);
        ADVANCE();
        uint64_t a = rd(c, &rm, sz, d.has_rex);
        switch (ext) {
        case 0: { uint64_t r = a + 1; uint32_t cf = c->flags & F_CF;
                  flags_add(c, a, 1, r, sz); c->flags = (c->flags & ~F_CF) | cf;
                  wr(c, &rm, r, sz, d.has_rex); return; }
        case 1: { uint64_t r = a - 1; uint32_t cf = c->flags & F_CF;
                  flags_sub(c, a, 1, r, sz); c->flags = (c->flags & ~F_CF) | cf;
                  wr(c, &rm, r, sz, d.has_rex); return; }
        case 2: push(c, c->rip); c->rip = rd(c, &rm, 8, d.has_rex); return; // call
        case 4: c->rip = rd(c, &rm, 8, d.has_rex); return;                  // jmp
        case 6: push(c, rd(c, &rm, 8, d.has_rex)); return;                  // push
        default: c->fault = "grp5 far call/jmp not supported"; c->fault_rip = start;
                 c->running = false; return;
        }
    }

    // ---- two-byte opcodes ----
    case 0x0F: {
        uint8_t op2 = fetch8(&d);

        if (op2 == 0x05) { ADVANCE(); do_syscall(c); return; }
        if (sse_exec(c, &d, op2, start)) return;             // syscall

        if (op2 == 0x1E || op2 == 0x1F) { modrm(&d, &rm); fixup_rip(&d, &rm);
                                          ADVANCE(); return; }             // multi-byte nop

        if (op2 >= 0x80 && op2 <= 0x8F) {                                  // jcc rel32
            int32_t rel = (int32_t)fetch32(&d); ADVANCE();
            if (cond(c, op2 & 0xF)) c->rip += (int64_t)rel;
            return;
        }
        if (op2 >= 0x90 && op2 <= 0x9F) {                                  // setcc
            modrm(&d, &rm); fixup_rip(&d, &rm); ADVANCE();
            wr(c, &rm, cond(c, op2 & 0xF) ? 1 : 0, 1, d.has_rex);
            return;
        }
        if (op2 >= 0x40 && op2 <= 0x4F) {                                  // cmovcc
            reg = modrm(&d, &rm); fixup_rip(&d, &rm); ADVANCE();
            if (cond(c, op2 & 0xF)) setreg(c, reg, rd(c, &rm, sz, d.has_rex), sz);
            return;
        }
        if (op2 == 0xA4 || op2 == 0xA5 || op2 == 0xAC || op2 == 0xAD) {    // shld/shrd
            bool left = (op2 < 0xAC);
            reg = modrm(&d, &rm);
            uint64_t cnt;
            if (op2 == 0xA4 || op2 == 0xAC) cnt = fetch8(&d);
            else                            cnt = c->r[RCX] & 0xff;
            fixup_rip(&d, &rm);
            ADVANCE();
            int w = sz * 8;
            cnt &= (sz == 8) ? 63 : 31;
            if (!cnt) return;
            uint64_t dst = rd(c, &rm, sz, d.has_rex);
            uint64_t src = getreg(c, reg, sz);
            uint64_t r = left ? ((dst << cnt) | (src >> (w - cnt)))
                              : ((dst >> cnt) | (src << (w - cnt)));
            flags_logic(c, r, sz);
            wr(c, &rm, r, sz, d.has_rex);
            return;
        }
        if (op2 == 0xAF) {                                                 // imul r, r/m
            reg = modrm(&d, &rm); fixup_rip(&d, &rm); ADVANCE();
            int64_t r = sext(getreg(c, reg, sz), sz) * sext(rd(c, &rm, sz, d.has_rex), sz);
            setreg(c, reg, (uint64_t)r, sz);
            flags_logic(c, (uint64_t)r, sz);
            return;
        }
        if (op2 == 0xB6 || op2 == 0xB7 || op2 == 0xBE || op2 == 0xBF) {    // movzx/movsx
            int ssz = (op2 & 1) ? 2 : 1;
            reg = modrm(&d, &rm); fixup_rip(&d, &rm); ADVANCE();
            uint64_t v = (ssz == 1 && rm.is_reg) ? getreg8(c, rm.reg, d.has_rex)
                                                 : rd(c, &rm, ssz, d.has_rex);
            setreg(c, reg, (op2 & 8) ? (uint64_t)sext(v, ssz) : v, sz);
            return;
        }

        c->fault = "unimplemented 0F opcode";
        c->fault_rip = start;
        c->running = false;
        // Print the mandatory prefix too: for 0F opcodes it selects the
        // instruction, so "0f 58" alone does not identify one.
        fprintf(stderr, "rashid: unimplemented opcode %s0f %02x at 0x%llx\n",
                d.mand == 0x66 ? "66 " : d.mand == 0xF2 ? "f2 " :
                d.mand == 0xF3 ? "f3 " : "", op2, start);
        return;
    }

    default:
        c->fault = "unimplemented opcode";
        c->fault_rip = start;
        c->running = false;
        fprintf(stderr, "rashid: unimplemented opcode %02x at 0x%llx\n", op, start);
        return;
    }
#undef ALU
#undef ADVANCE
}

// ------------------------------------------------------------------- API
int rsd_cpu_init(rsd_cpu *c, rsd_as *as, uint64_t entry, int argc, char **argv) {
    memset(c, 0, sizeof *c);
    c->as = as;
    c->fault_import = -1;
    c->rip = entry;
    c->flags = 0x202;
    c->running = true;

    uint64_t base = rsd_as_map(as, 0, GUEST_STACK_SIZE,
                               RSD_PROT_R | RSD_PROT_W, false);
    if (!base) { fprintf(stderr, "rashid: cannot allocate guest stack\n"); return -1; }
    uint64_t sp_top = base + GUEST_STACK_SIZE;
    c->stack_base = base;
    c->stack_size = GUEST_STACK_SIZE;

    // Minimal macOS start frame: argc, argv[], NULL, envp NULL, apple NULL.
    // argv strings are copied into guest space; the guest cannot see host
    // pointers.
    uint64_t strp = sp_top - RSD_GUEST_PAGE;
    uint64_t sp = base + GUEST_STACK_SIZE / 2;
    uint64_t gargv[64];
    if (argc > 60) argc = 60;
    for (int i = 0; i < argc; i++) {
        size_t n = strlen(argv[i]) + 1;
        strp -= n;
        memcpy(rsd_g2h(as, strp), argv[i], n);
        gargv[i] = strp;
    }
    uint64_t *f = rsd_g2h(as, sp);
    int n = 0;
    f[n++] = (uint64_t)argc;
    for (int i = 0; i < argc; i++) f[n++] = gargv[i];
    f[n++] = 0;   // argv terminator
    f[n++] = 0;   // envp terminator
    f[n++] = 0;   // apple[] terminator
    c->r[RSP] = sp;
    return 0;
}

void rsd_cpu_free(rsd_cpu *c) {
    c->stack_base = 0;
}

void rsd_cpu_run(rsd_cpu *c, uint64_t budget) {
    while (c->running) {
        if (c->trace)
            fprintf(stderr, "  %6llu  rip=0x%llx rax=0x%llx rdi=0x%llx rsp=0x%llx\n",
                    c->icount, c->rip, c->r[RAX], c->r[RDI], c->r[RSP]);
        step(c);
        if (++c->icount == budget && budget) {
            c->fault = "instruction budget exhausted";
            c->fault_rip = c->rip;
            c->running = false;
        }
    }
}

void rsd_cpu_dump(const rsd_cpu *c) {
    fprintf(stderr, "-- guest state (%llu instructions) --\n", c->icount);
    for (int i = 0; i < 16; i += 4) {
        for (int j = 0; j < 4; j++)
            fprintf(stderr, "%-4s=%016llx  ", rname[i + j], c->r[i + j]);
        fprintf(stderr, "\n");
    }
    fprintf(stderr, "rip =%016llx  flags=%08x [%s%s%s%s]\n", c->rip, c->flags,
            (c->flags & F_CF) ? "C" : "-", (c->flags & F_ZF) ? "Z" : "-",
            (c->flags & F_SF) ? "S" : "-", (c->flags & F_OF) ? "O" : "-");
    if (c->fault && c->fault_import >= 0 && c->stubs) {
        fprintf(stderr, "fault: %s\n       %s  (from %s)\n", c->fault,
                c->stubs->names[c->fault_import], c->stubs->libs[c->fault_import]);
        return;
    }
    if (c->fault) {
        fprintf(stderr, "fault: %s @ rip 0x%llx", c->fault, c->fault_rip);
        if (c->fault_has_addr) fprintf(stderr, " (addr 0x%llx)", c->fault_addr);
        fprintf(stderr, "\n");
    }
}
