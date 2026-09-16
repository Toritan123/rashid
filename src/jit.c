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
#include "jit.h"
#include "emit.h"

#include <libkern/OSCacheControl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#define ARENA (4u << 20)
#define CACHE_BITS 12          // small enough to stay in cache
#define MAX_BLOCK 2048         // emitted words before a block is closed

// The interpreter's own routines, so the two engines share one definition of
// what an instruction means.
uint64_t rsd_i_ld(rsd_cpu *, uint64_t, uint64_t);
void     rsd_i_st(rsd_cpu *, uint64_t, uint64_t, uint64_t);
void     rsd_i_flags_add(rsd_cpu *, uint64_t, uint64_t, uint64_t, uint64_t);
void     rsd_i_flags_sub(rsd_cpu *, uint64_t, uint64_t, uint64_t, uint64_t);
void     rsd_i_flags_logic(rsd_cpu *, uint64_t, uint64_t);
uint64_t rsd_i_cond(rsd_cpu *, uint64_t);
void     rsd_i_ld128(rsd_cpu *, uint64_t, uint64_t *);
void     rsd_i_st128(rsd_cpu *, uint64_t, const uint64_t *);
void     rsd_i_comisd(rsd_cpu *, uint64_t, uint64_t);
void     rsd_i_comiss(rsd_cpu *, uint64_t, uint64_t);
uint64_t rsd_i_cvt2si(rsd_cpu *, uint64_t, uint64_t, uint64_t, uint64_t);
uint64_t rsd_i_nan_d(uint64_t, uint64_t, uint64_t, uint64_t);
uint64_t rsd_i_nan_s(uint64_t, uint64_t, uint64_t, uint64_t);
void     rsd_i_flags_inc(rsd_cpu *, uint64_t, uint64_t, uint64_t);
void     rsd_i_flags_dec(rsd_cpu *, uint64_t, uint64_t, uint64_t);

#define CPU A64_X19            // compiled code's only base register
#define MAX_INSNS 48           // guest instructions per block

typedef struct {
    uint32_t *out;
    int       n, max;
    bool      full;
    int       fix[256];        // cbz words to point at the fault exit
    int       nfix;
} buf;

static void put(buf *b, uint32_t insn) {
    if (b->n >= b->max) { b->full = true; return; }
    b->out[b->n++] = insn;
}

static void put_imm(buf *b, int reg, uint64_t v) {
    put(b, a64_movz(reg, (uint16_t)v, 0));
    if (v >> 16)  put(b, a64_movk(reg, (uint16_t)(v >> 16), 1));
    if (v >> 32)  put(b, a64_movk(reg, (uint16_t)(v >> 32), 2));
    if (v >> 48)  put(b, a64_movk(reg, (uint16_t)(v >> 48), 3));
}

static void put_call(buf *b, void *fn) {
    put_imm(b, A64_X9, (uint64_t)(uintptr_t)fn);
    put(b, a64_blr(A64_X9));
}

// Offsets into rsd_cpu, checked once in rsd_jit_init.
static uint32_t off_r(int r)   { return (uint32_t)(offsetof(rsd_cpu, r) + (size_t)r * 8); }
static uint32_t off_tmp(int i) { return (uint32_t)(offsetof(rsd_cpu, jit_tmp) + (size_t)i * 8); }
static uint32_t off_xmm(int i) { return (uint32_t)(offsetof(rsd_cpu, xmm) + (size_t)i * 16); }
#define OFF_CURRIP  ((uint32_t)offsetof(rsd_cpu, cur_rip))
#define OFF_RUNNING ((uint32_t)offsetof(rsd_cpu, running))

// Which instruction is executing, so a fault inside a helper is reported
// against it rather than against wherever the block began.
static void put_set_rip(buf *b, uint64_t rip) {
    put_imm(b, A64_X9, rip);
    put(b, a64_str(A64_X9, CPU, OFF_CURRIP));
}

// A helper that faults stops the cpu but returns normally. Leave the block
// at once: carrying on would, for instance, store a value that a failed load
// never produced.
static void put_fault_check(buf *b) {
    put(b, a64_ldrb(A64_X9, CPU, OFF_RUNNING));
    if (b->nfix < 256 && !b->full) b->fix[b->nfix++] = b->n;
    else b->full = true;
    put(b, a64_cbz_w(A64_X9, 0));
}

static void put_leave(buf *b) {
    put(b, a64_pop(A64_X30));
    put(b, a64_ret());
}

// --- the compilable subset -------------------------------------------
// Integer operations on 32- and 64-bit operands, with register, immediate
// and memory operands; lea, push, pop; and the control transfers that end a
// block. Byte and 16-bit forms, SSE, segment overrides and anything else end
// the block and the interpreter carries on.

// Pseudo-opcodes for operations that have no ALU row of their own.
enum { OP_INC = 0x100, OP_DEC = 0x101 };

enum { K_RR = 1, K_RI, K_RM, K_MR, K_MI, K_LEA, K_PUSH, K_POP, K_NOP, K_SSE,
       K_JCC, K_JMP, K_CALL, K_RET, K_CALLM, K_JMPM };

typedef struct {
    int      kind, op, size, len;
    int      reg, src;
    bool     mem, has_base, has_index, riprel;
    int      base, index, scale;
    int64_t  disp;
    uint64_t imm;
    bool     no_write;
    int      cc;
    uint64_t next, target;
    // SSE: mandatory prefix, second opcode byte, xmm destination, xmm or
    // general register source, integer width for conversions, cmp predicate
    int      mand, op2, xreg, xsrc, isize, pred;
} insn;

static bool terminates(int kind) { return kind >= K_JCC; }

static void modrm_mem(const uint8_t *p, int *n, uint8_t rex, uint8_t m, insn *o) {
    int mod = m >> 6, rmb = m & 7;
    int32_t d32;
    o->mem = true;
    if (rmb == 4) {
        uint8_t sib = p[(*n)++];
        int index = ((sib >> 3) & 7) | ((rex & 2) ? 8 : 0);
        int base  = (sib & 7) | ((rex & 1) ? 8 : 0);
        if (index != 4) { o->has_index = true; o->index = index; o->scale = sib >> 6; }
        if ((sib & 7) == 5 && mod == 0) { memcpy(&d32, p + *n, 4); *n += 4; o->disp = d32; }
        else { o->has_base = true; o->base = base; }
    } else if (rmb == 5 && mod == 0) {
        memcpy(&d32, p + *n, 4); *n += 4;
        o->disp = d32;
        o->riprel = true;
    } else {
        o->has_base = true;
        o->base = rmb | ((rex & 1) ? 8 : 0);
    }
    if (mod == 1)      o->disp += (int8_t)p[(*n)++];
    else if (mod == 2) { memcpy(&d32, p + *n, 4); *n += 4; o->disp += d32; }
}

// The SSE subset: what compiled x86_64 code actually uses in scalar
// floating-point work. Each accepts only the prefixes the interpreter treats
// the same way; anything ambiguous is left to the interpreter.
static bool decode_sse(const uint8_t *p, int *n, uint8_t rex, uint8_t mand,
                       uint8_t op2, insn *o) {
    bool scalar = (mand == 0xF2 || mand == 0xF3);
    bool packed = (mand == 0x00 || mand == 0x66);
    switch (op2) {
    case 0x10: case 0x11:                        // movs[sd] / movup[sd]
        break;
    case 0x28: case 0x29:                        // movap[sd]
    case 0x54: case 0x55: case 0x56: case 0x57:  // and / andn / or / xor
    case 0x2E: case 0x2F:                        // ucomis / comis
        if (!packed) return false;
        break;
    case 0xDB: case 0xEB: case 0xEF:             // pand / por / pxor
        if (mand != 0x66) return false;
        break;
    case 0x51: case 0x58: case 0x59: case 0x5C:
    case 0x5D: case 0x5E: case 0x5F:             // scalar arithmetic
    case 0xC2:                                   // cmps[sd]
    case 0x2A: case 0x2C: case 0x2D: case 0x5A:  // conversions
        if (!scalar) return false;
        break;
    default:
        return false;
    }

    uint8_t m = p[(*n)++];
    o->kind = K_SSE;
    o->mand = mand;
    o->op2 = op2;
    o->isize = (rex & 8) ? 8 : 4;
    o->xreg = ((m >> 3) & 7) | ((rex & 4) ? 8 : 0);
    if ((m >> 6) == 3) o->xsrc = (m & 7) | ((rex & 1) ? 8 : 0);
    else               modrm_mem(p, n, rex, m, o);
    if (op2 == 0xC2) o->pred = p[(*n)++] & 7;
    return true;
}

static int decode(const uint8_t *p, uint64_t rip, insn *o) {
    memset(o, 0, sizeof *o);
    int n = 0;
    uint8_t rex = 0, mand = 0;
    // A 66/F2/F3 prefix selects an SSE instruction; on anything else it is an
    // operand-size or rep prefix, which the compiler does not handle.
    if (p[n] == 0x66 || p[n] == 0xF2 || p[n] == 0xF3) mand = p[n++];
    if ((p[n] & 0xF0) == 0x40) rex = p[n++];
    o->size = (rex & 8) ? 8 : 4;
    uint8_t op = p[n++], m;
    int32_t d32;
    if (mand && op != 0x0F) return 0;

    switch (op) {
    case 0x01: case 0x03: case 0x09: case 0x0B:
    case 0x21: case 0x23: case 0x29: case 0x2B:
    case 0x31: case 0x33: case 0x39: case 0x3B:
    case 0x85: case 0x89: case 0x8B: {
        m = p[n++];
        int regf = ((m >> 3) & 7) | ((rex & 4) ? 8 : 0);
        bool to_reg = (op & 2) && op != 0x85;
        if ((m >> 6) == 3) {
            int rm = (m & 7) | ((rex & 1) ? 8 : 0);
            o->kind = K_RR;
            o->reg = to_reg ? regf : rm;
            o->src = to_reg ? rm : regf;
        } else {
            modrm_mem(p, &n, rex, m, o);
            o->kind = to_reg ? K_RM : K_MR;
            o->reg = regf;
        }
        o->op = (op == 0x85) ? 0x85 : (op == 0x89 || op == 0x8B) ? 0x88 : (op & ~3);
        o->no_write = (o->op == 0x38 || o->op == 0x85);
        break;
    }
    case 0x81: case 0x83: case 0xC7: {
        m = p[n++];
        int ext = (m >> 3) & 7;
        if (op == 0xC7 && ext != 0) return 0;
        if (op != 0xC7 && (ext == 2 || ext == 3)) return 0;   // adc/sbb
        if ((m >> 6) == 3) { o->kind = K_RI; o->reg = (m & 7) | ((rex & 1) ? 8 : 0); }
        else               { modrm_mem(p, &n, rex, m, o); o->kind = K_MI; }
        if (op == 0x83) o->imm = (uint64_t)(int64_t)(int8_t)p[n++];
        else { memcpy(&d32, p + n, 4); n += 4; o->imm = (uint64_t)(int64_t)d32; }
        if (o->size == 4) o->imm &= 0xffffffffu;
        if (op == 0xC7) { o->op = 0x88; break; }
        static const int map[8] = { 0x00, 0x08, 0, 0, 0x20, 0x28, 0x30, 0x38 };
        o->op = map[ext];
        o->no_write = (ext == 7);
        break;
    }
    case 0xA9:                                  // test eax/rax, imm32
        o->kind = K_RI;
        o->reg = 0;
        memcpy(&d32, p + n, 4); n += 4;
        o->imm = (uint64_t)(int64_t)d32;
        if (o->size == 4) o->imm &= 0xffffffffu;
        o->op = 0x85;
        o->no_write = true;
        break;
    case 0xB8: case 0xB9: case 0xBA: case 0xBB:
    case 0xBC: case 0xBD: case 0xBE: case 0xBF:
        o->kind = K_RI;
        o->reg = (op & 7) | ((rex & 1) ? 8 : 0);
        if (o->size == 8) { memcpy(&o->imm, p + n, 8); n += 8; }
        else { uint32_t v; memcpy(&v, p + n, 4); o->imm = v; n += 4; }
        o->op = 0x88;
        break;
    case 0x8D:
        m = p[n++];
        if ((m >> 6) == 3) return 0;
        modrm_mem(p, &n, rex, m, o);
        o->kind = K_LEA;
        o->reg = ((m >> 3) & 7) | ((rex & 4) ? 8 : 0);
        break;
    case 0x50: case 0x51: case 0x52: case 0x53:
    case 0x54: case 0x55: case 0x56: case 0x57:
    case 0x58: case 0x59: case 0x5A: case 0x5B:
    case 0x5C: case 0x5D: case 0x5E: case 0x5F:
        o->kind = (op < 0x58) ? K_PUSH : K_POP;
        o->reg = (op & 7) | ((rex & 1) ? 8 : 0);
        if (o->reg == 4) return 0;             // push/pop rsp: rare, subtle
        break;
    case 0x90:
        if (rex) return 0;                     // with REX.B this is xchg
        o->kind = K_NOP;
        break;
    case 0x70: case 0x71: case 0x72: case 0x73: case 0x74: case 0x75:
    case 0x76: case 0x77: case 0x78: case 0x79: case 0x7A: case 0x7B:
    case 0x7C: case 0x7D: case 0x7E: case 0x7F:
        o->kind = K_JCC; o->cc = op & 0xF;
        o->imm = (uint64_t)(int64_t)(int8_t)p[n++];
        break;
    case 0x0F: {
        uint8_t op2 = p[n++];
        if (op2 >= 0x80 && op2 <= 0x8F) {
            if (mand) return 0;
            o->kind = K_JCC; o->cc = op2 & 0xF;
            memcpy(&d32, p + n, 4); n += 4; o->imm = (uint64_t)(int64_t)d32;
        } else if (op2 == 0x1F) {                   // multi-byte nop
            if (mand && mand != 0x66) return 0;
            m = p[n++];
            if ((m >> 6) != 3) modrm_mem(p, &n, rex, m, o);
            o->mem = false;
            o->kind = K_NOP;
        } else if (!decode_sse(p, &n, rex, mand, op2, o)) {
            return 0;
        }
        break;
    }
    case 0xEB: o->kind = K_JMP;  o->imm = (uint64_t)(int64_t)(int8_t)p[n++]; break;
    case 0xE9: o->kind = K_JMP;  memcpy(&d32, p + n, 4); n += 4; o->imm = (uint64_t)(int64_t)d32; break;
    case 0xE8: o->kind = K_CALL; memcpy(&d32, p + n, 4); n += 4; o->imm = (uint64_t)(int64_t)d32; break;
    case 0xC3: o->kind = K_RET; break;
    case 0xFF: {
        m = p[n++];
        int ext = (m >> 3) & 7;
        if (ext == 0 || ext == 1) {                 // inc / dec
            if ((m >> 6) == 3) { o->kind = K_RI; o->reg = (m & 7) | ((rex & 1) ? 8 : 0); }
            else               { modrm_mem(p, &n, rex, m, o); o->kind = K_MI; }
            o->imm = 1;
            o->op = ext == 0 ? OP_INC : OP_DEC;
            break;
        }
        if (ext != 2 && ext != 4) return 0;
        if ((m >> 6) == 3) {
            o->reg = (m & 7) | ((rex & 1) ? 8 : 0);
            if (o->reg == 4) return 0;
        } else {
            modrm_mem(p, &n, rex, m, o);
        }
        o->kind = (ext == 2) ? K_CALLM : K_JMPM;
        break;
    }
    default:
        return 0;
    }

    o->len = n;
    o->next = rip + (uint64_t)n;
    if (o->kind == K_JCC || o->kind == K_JMP || o->kind == K_CALL)
        o->target = o->next + o->imm;
    return n;
}

// Effective address into x1. A RIP-relative one is a constant here, because
// the block is compiled for one address.
static void put_ea(buf *b, const insn *o) {
    if (o->riprel) { put_imm(b, A64_X1, o->next + (uint64_t)o->disp); return; }
    if (!o->has_base && !o->has_index) { put_imm(b, A64_X1, (uint64_t)o->disp); return; }
    if (o->has_base) put(b, a64_ldr(A64_X1, CPU, off_r(o->base)));
    else             put(b, a64_movz(A64_X1, 0, 0));
    if (o->has_index) {
        put(b, a64_ldr(A64_X6, CPU, off_r(o->index)));
        put(b, a64_add_shift(A64_X1, A64_X1, A64_X6, o->scale));
    }
    if (o->disp > 0 && o->disp < 4096)       put(b, a64_add_imm(A64_X1, A64_X1, (uint32_t)o->disp));
    else if (o->disp < 0 && o->disp > -4096) put(b, a64_sub_imm(A64_X1, A64_X1, (uint32_t)-o->disp));
    else if (o->disp) { put_imm(b, A64_X6, (uint64_t)o->disp); put(b, a64_add(A64_X1, A64_X1, A64_X6)); }
}

// With the operands in x1 and x2, the result into x3. Returns the flag helper
// the operation needs, if any.
static void *put_op(buf *b, const insn *o) {
    bool w = o->size == 4;
    switch (o->op) {
    case 0x88: put(b, w ? a64_mov_w(A64_X3, A64_X2) : a64_mov(A64_X3, A64_X2)); return NULL;
    case 0x00: put(b, w ? a64_add_w(A64_X3, A64_X1, A64_X2) : a64_add(A64_X3, A64_X1, A64_X2));
               return rsd_i_flags_add;
    case 0x28: case 0x38:
               put(b, w ? a64_sub_w(A64_X3, A64_X1, A64_X2) : a64_sub(A64_X3, A64_X1, A64_X2));
               return rsd_i_flags_sub;
    case 0x20: case 0x85:
               put(b, w ? a64_and_w(A64_X3, A64_X1, A64_X2) : a64_and(A64_X3, A64_X1, A64_X2));
               return rsd_i_flags_logic;
    case 0x08: put(b, w ? a64_orr_w(A64_X3, A64_X1, A64_X2) : a64_orr(A64_X3, A64_X1, A64_X2));
               return rsd_i_flags_logic;
    case OP_INC: put(b, w ? a64_add_w(A64_X3, A64_X1, A64_X2) : a64_add(A64_X3, A64_X1, A64_X2));
               return rsd_i_flags_inc;
    case OP_DEC: put(b, w ? a64_sub_w(A64_X3, A64_X1, A64_X2) : a64_sub(A64_X3, A64_X1, A64_X2));
               return rsd_i_flags_dec;
    case 0x30: put(b, w ? a64_eor_w(A64_X3, A64_X1, A64_X2) : a64_eor(A64_X3, A64_X1, A64_X2));
               return rsd_i_flags_logic;
    }
    b->full = true;
    return NULL;
}

// Operands are still in x1, x2 and x3, which is where the helpers want them.
static void put_flags(buf *b, void *fn, int size) {
    if (!fn) return;
    put(b, a64_mov(A64_X0, CPU));
    if (fn == (void *)rsd_i_flags_logic) {
        put(b, a64_mov(A64_X1, A64_X3));
        put_imm(b, A64_X2, (uint64_t)size);
    } else if (fn == (void *)rsd_i_flags_inc || fn == (void *)rsd_i_flags_dec) {
        put(b, a64_mov(A64_X2, A64_X3));            // (cpu, a, r, size)
        put_imm(b, A64_X3, (uint64_t)size);
    } else {
        put_imm(b, A64_X4, (uint64_t)size);
    }
    put_call(b, fn);
}

static void put_load(buf *b, int size) {        // x1 = address -> x0 = value
    put(b, a64_mov(A64_X0, CPU));
    put_imm(b, A64_X2, (uint64_t)size);
    put_call(b, rsd_i_ld);
    put_fault_check(b);
}

static void put_store(buf *b, int size) {       // x1 = address, x2 = value
    put(b, a64_mov(A64_X0, CPU));
    put_imm(b, A64_X3, (uint64_t)size);
    put_call(b, rsd_i_st);
    put_fault_check(b);
}

// --- SSE code generation ---------------------------------------------
// Guest XMM registers live in rsd_cpu; d0/s0 hold the destination lane and
// d1/s1 the source while an instruction is computed. Helper calls clobber
// every low FP register, so a memory source is fetched before the
// destination is loaded.

#define D0 0
#define D1 1
#define D2 2

// Which AArch64 condition gives each CMPSD/CMPSS predicate after fcmp. An
// unordered comparison sets C and V, which is what makes these line up.
static const int cmp_cond[8] = {
    A64_EQ,  // 0 eq, ordered
    A64_MI,  // 1 lt
    A64_LS,  // 2 le
    A64_VS,  // 3 unord
    A64_NE,  // 4 neq, unordered
    A64_PL,  // 5 nlt
    A64_HI,  // 6 nle
    A64_VC,  // 7 ord
};

static void put_fsrc(buf *b, const insn *o, uint64_t rip, bool dbl) {
    if (!o->mem) {
        put(b, dbl ? a64_ldr_d(D1, CPU, off_xmm(o->xsrc)) : a64_ldr_s(D1, CPU, off_xmm(o->xsrc)));
        return;
    }
    put_set_rip(b, rip);
    put_ea(b, o);
    put_load(b, dbl ? 8 : 4);
    put(b, dbl ? a64_fmov_dx(D1, A64_X0) : a64_fmov_sw(D1, A64_X0));
}

static void put_sse(buf *b, const insn *o, uint64_t rip) {
    bool dbl = (o->mand == 0xF2);
    uint32_t dst = off_xmm(o->xreg);

    switch (o->op2) {

    case 0x10: case 0x11:
        if (o->mand == 0xF2 || o->mand == 0xF3) {       // movsd / movss
            int nb = (o->mand == 0xF2) ? 8 : 4;
            bool store = (o->op2 == 0x11);
            if (!o->mem) {
                // Register to register replaces only the low lane.
                uint32_t from = store ? dst : off_xmm(o->xsrc);
                uint32_t to   = store ? off_xmm(o->xsrc) : dst;
                if (nb == 8) { put(b, a64_ldr(A64_X1, CPU, from)); put(b, a64_str(A64_X1, CPU, to)); }
                else         { put(b, a64_ldr_w(A64_X1, CPU, from)); put(b, a64_str_w(A64_X1, CPU, to)); }
                return;
            }
            put_set_rip(b, rip);
            put_ea(b, o);
            if (store) {
                put(b, a64_ldr(A64_X2, CPU, dst));
                put_store(b, nb);
                return;
            }
            put_load(b, nb);                            // from memory: rest zeroed
            put(b, a64_str(A64_X0, CPU, dst));
            put(b, a64_str(A64_ZR, CPU, dst + 8));
            return;
        }
        /* fall through: movups / movupd */
    case 0x28: case 0x29: {                             // 128-bit copies
        bool store = (o->op2 & 1);
        if (!o->mem) {
            uint32_t from = store ? dst : off_xmm(o->xsrc);
            uint32_t to   = store ? off_xmm(o->xsrc) : dst;
            put(b, a64_ldr(A64_X1, CPU, from));
            put(b, a64_ldr(A64_X2, CPU, from + 8));
            put(b, a64_str(A64_X1, CPU, to));
            put(b, a64_str(A64_X2, CPU, to + 8));
            return;
        }
        put_set_rip(b, rip);
        put_ea(b, o);
        put(b, a64_add_imm(A64_X2, CPU, dst));
        put(b, a64_mov(A64_X0, CPU));
        put_call(b, store ? (void *)rsd_i_st128 : (void *)rsd_i_ld128);
        put_fault_check(b);
        return;
    }

    case 0x54: case 0x55: case 0x56: case 0x57:
    case 0xDB: case 0xEB: case 0xEF: {                  // 128-bit bitwise
        if (o->mem) {
            put_set_rip(b, rip);
            put_ea(b, o);
            put(b, a64_add_imm(A64_X2, CPU, off_tmp(0)));
            put(b, a64_mov(A64_X0, CPU));
            put_call(b, rsd_i_ld128);
            put_fault_check(b);
            put(b, a64_ldr(A64_X4, CPU, off_tmp(0)));
            put(b, a64_ldr(A64_X5, CPU, off_tmp(1)));
        } else {
            put(b, a64_ldr(A64_X4, CPU, off_xmm(o->xsrc)));
            put(b, a64_ldr(A64_X5, CPU, off_xmm(o->xsrc) + 8));
        }
        put(b, a64_ldr(A64_X1, CPU, dst));
        put(b, a64_ldr(A64_X2, CPU, dst + 8));
        switch (o->op2) {
        case 0x54: case 0xDB: put(b, a64_and(A64_X1, A64_X1, A64_X4)); put(b, a64_and(A64_X2, A64_X2, A64_X5)); break;
        case 0x55:            put(b, a64_bic(A64_X1, A64_X4, A64_X1)); put(b, a64_bic(A64_X2, A64_X5, A64_X2)); break;
        case 0x56: case 0xEB: put(b, a64_orr(A64_X1, A64_X1, A64_X4)); put(b, a64_orr(A64_X2, A64_X2, A64_X5)); break;
        default:              put(b, a64_eor(A64_X1, A64_X1, A64_X4)); put(b, a64_eor(A64_X2, A64_X2, A64_X5)); break;
        }
        put(b, a64_str(A64_X1, CPU, dst));
        put(b, a64_str(A64_X2, CPU, dst + 8));
        return;
    }

    case 0x51: case 0x58: case 0x59: case 0x5C:
    case 0x5D: case 0x5E: case 0x5F: {                  // scalar arithmetic
        put_fsrc(b, o, rip, dbl);
        if (o->op2 != 0x51) {
            put(b, dbl ? a64_ldr_d(D0, CPU, dst) : a64_ldr_s(D0, CPU, dst));
            put(b, dbl ? a64_ldr_d(D2, CPU, dst) : a64_ldr_s(D2, CPU, dst));  // first operand, kept
        }
        switch (o->op2) {
        case 0x58: put(b, dbl ? a64_fop_d(FOP_ADD, D0, D0, D1) : a64_fop_s(FOP_ADD, D0, D0, D1)); break;
        case 0x59: put(b, dbl ? a64_fop_d(FOP_MUL, D0, D0, D1) : a64_fop_s(FOP_MUL, D0, D0, D1)); break;
        case 0x5C: put(b, dbl ? a64_fop_d(FOP_SUB, D0, D0, D1) : a64_fop_s(FOP_SUB, D0, D0, D1)); break;
        case 0x5E: put(b, dbl ? a64_fop_d(FOP_DIV, D0, D0, D1) : a64_fop_s(FOP_DIV, D0, D0, D1)); break;
        case 0x51: put(b, dbl ? a64_fsqrt_d(D0, D1) : a64_fsqrt_s(D0, D1)); break;
        // min and max return the second operand when the inputs are
        // unordered or equal; mi and gt are both false for unordered.
        case 0x5D: put(b, dbl ? a64_fcmp_d(D0, D1) : a64_fcmp_s(D0, D1));
                   put(b, dbl ? a64_fcsel_d(D0, D0, D1, A64_MI) : a64_fcsel_s(D0, D0, D1, A64_MI)); break;
        case 0x5F: put(b, dbl ? a64_fcmp_d(D0, D1) : a64_fcmp_s(D0, D1));
                   put(b, dbl ? a64_fcsel_d(D0, D0, D1, A64_GT) : a64_fcsel_s(D0, D0, D1, A64_GT)); break;
        }
        if (o->op2 != 0x5D && o->op2 != 0x5F) {
            // Only a NaN result can differ from x86, so the common path is
            // one compare and a branch not taken.
            put(b, dbl ? a64_fcmp_d(D0, D0) : a64_fcmp_s(D0, D0));
            int skip = b->n;
            put(b, a64_bcond(A64_VC, 0));
            bool unary = (o->op2 == 0x51);
            // rsd_i_nan_* take no cpu, so their arguments start at x0 -
            // unlike every other helper here, which is exactly the mistake
            // the first version of this made.
            if (dbl) {
                put(b, a64_fmov_xd(A64_X0, D0));
                put(b, a64_fmov_xd(A64_X1, unary ? D1 : D2));
                put(b, a64_fmov_xd(A64_X2, D1));
            } else {
                put(b, a64_fmov_ws(A64_X0, D0));
                put(b, a64_fmov_ws(A64_X1, unary ? D1 : D2));
                put(b, a64_fmov_ws(A64_X2, D1));
            }
            put_imm(b, A64_X3, unary ? 1 : 0);
            put_call(b, dbl ? (void *)rsd_i_nan_d : (void *)rsd_i_nan_s);
            put(b, dbl ? a64_fmov_dx(D0, A64_X0) : a64_fmov_sw(D0, A64_X0));
            if (!b->full) b->out[skip] = a64_bcond(A64_VC, b->n - skip);
        }
        put(b, dbl ? a64_str_d(D0, CPU, dst) : a64_str_s(D0, CPU, dst));
        return;
    }

    case 0xC2:                                          // cmpsd / cmpss
        put_fsrc(b, o, rip, dbl);
        put(b, dbl ? a64_ldr_d(D0, CPU, dst) : a64_ldr_s(D0, CPU, dst));
        put(b, dbl ? a64_fcmp_d(D0, D1) : a64_fcmp_s(D0, D1));
        if (dbl) { put(b, a64_csetm(A64_X3, cmp_cond[o->pred]));   put(b, a64_str(A64_X3, CPU, dst)); }
        else     { put(b, a64_csetm_w(A64_X3, cmp_cond[o->pred])); put(b, a64_str_w(A64_X3, CPU, dst)); }
        return;

    case 0x2E: case 0x2F: {                             // ucomis / comis
        bool d64 = (o->mand == 0x66);
        if (o->mem) {
            put_set_rip(b, rip);
            put_ea(b, o);
            put_load(b, d64 ? 8 : 4);
            put(b, a64_mov(A64_X2, A64_X0));
        } else {
            put(b, d64 ? a64_ldr(A64_X2, CPU, off_xmm(o->xsrc)) : a64_ldr_w(A64_X2, CPU, off_xmm(o->xsrc)));
        }
        put(b, d64 ? a64_ldr(A64_X1, CPU, dst) : a64_ldr_w(A64_X1, CPU, dst));
        put(b, a64_mov(A64_X0, CPU));
        put_call(b, d64 ? (void *)rsd_i_comisd : (void *)rsd_i_comiss);
        return;
    }

    case 0x2A: {                                        // cvtsi2sd / cvtsi2ss
        int src_gp = o->xsrc;                           // a general register here
        if (o->mem) {
            put_set_rip(b, rip);
            put_ea(b, o);
            put_load(b, o->isize);
            put(b, a64_mov(A64_X1, A64_X0));
        } else {
            put(b, a64_ldr(A64_X1, CPU, off_r(src_gp)));
        }
        if (dbl) put(b, o->isize == 8 ? a64_scvtf_dx(D0, A64_X1) : a64_scvtf_dw(D0, A64_X1));
        else     put(b, o->isize == 8 ? a64_scvtf_sx(D0, A64_X1) : a64_scvtf_sw(D0, A64_X1));
        put(b, dbl ? a64_str_d(D0, CPU, dst) : a64_str_s(D0, CPU, dst));
        return;
    }

    case 0x2C: case 0x2D: {                             // cvt(t)s[sd]2si
        // Out-of-range results follow x86, not a C cast, so this goes through
        // the same routine the interpreter uses.
        int fsize = dbl ? 8 : 4;
        if (o->mem) {
            put_set_rip(b, rip);
            put_ea(b, o);
            put_load(b, fsize);
            put(b, a64_mov(A64_X1, A64_X0));
        } else {
            put(b, a64_ldr(A64_X1, CPU, off_xmm(o->xsrc)));
        }
        put(b, a64_mov(A64_X0, CPU));
        put_imm(b, A64_X2, (uint64_t)fsize);
        put_imm(b, A64_X3, o->op2 == 0x2D ? 1 : 0);
        put_imm(b, A64_X4, (uint64_t)o->isize);
        put_call(b, rsd_i_cvt2si);
        put(b, a64_str(A64_X0, CPU, off_r(o->xreg)));   // xreg is the integer destination
        return;
    }

    case 0x5A:                                          // cvtss2sd / cvtsd2ss
        if (dbl) {                                      // double -> single
            put_fsrc(b, o, rip, true);
            put(b, a64_fcvt_sd(D0, D1));
            put(b, a64_str_s(D0, CPU, dst));
        } else {                                        // single -> double
            put_fsrc(b, o, rip, false);
            put(b, a64_fcvt_ds(D0, D1));
            put(b, a64_str_d(D0, CPU, dst));
        }
        return;
    }
    b->full = true;
}

static void put_insn(buf *b, const insn *o, uint64_t rip) {
    void *fl;
    switch (o->kind) {
    case K_NOP:
        return;

    case K_SSE:
        put_sse(b, o, rip);
        return;

    case K_RR:
    case K_RI:
        put(b, a64_ldr(A64_X1, CPU, off_r(o->reg)));
        if (o->kind == K_RI) put_imm(b, A64_X2, o->imm);
        else                 put(b, a64_ldr(A64_X2, CPU, off_r(o->src)));
        fl = put_op(b, o);
        if (!o->no_write) put(b, a64_str(A64_X3, CPU, off_r(o->reg)));
        put_flags(b, fl, o->size);
        return;

    case K_RM:                                  // reg = reg op [mem]
        put_set_rip(b, rip);
        put_ea(b, o);
        put_load(b, o->size);
        put(b, a64_mov(A64_X2, A64_X0));
        put(b, a64_ldr(A64_X1, CPU, off_r(o->reg)));
        fl = put_op(b, o);
        if (!o->no_write) put(b, a64_str(A64_X3, CPU, off_r(o->reg)));
        put_flags(b, fl, o->size);
        return;

    case K_MR:                                  // [mem] = [mem] op reg
    case K_MI:
        put_set_rip(b, rip);
        put_ea(b, o);
        if (o->op == 0x88) {                    // a plain store
            if (o->kind == K_MI) put_imm(b, A64_X2, o->imm);
            else                 put(b, a64_ldr(A64_X2, CPU, off_r(o->reg)));
            put_store(b, o->size);
            return;
        }
        put(b, a64_str(A64_X1, CPU, off_tmp(0)));
        put_load(b, o->size);
        put(b, a64_mov(A64_X1, A64_X0));
        if (o->kind == K_MI) put_imm(b, A64_X2, o->imm);
        else                 put(b, a64_ldr(A64_X2, CPU, off_r(o->reg)));
        fl = put_op(b, o);
        if (!o->no_write) {
            put(b, a64_str(A64_X1, CPU, off_tmp(1)));
            put(b, a64_str(A64_X2, CPU, off_tmp(2)));
            put(b, a64_str(A64_X3, CPU, off_tmp(3)));
            put(b, a64_mov(A64_X2, A64_X3));
            put(b, a64_ldr(A64_X1, CPU, off_tmp(0)));
            put_store(b, o->size);
            put(b, a64_ldr(A64_X1, CPU, off_tmp(1)));
            put(b, a64_ldr(A64_X2, CPU, off_tmp(2)));
            put(b, a64_ldr(A64_X3, CPU, off_tmp(3)));
        }
        put_flags(b, fl, o->size);
        return;

    case K_LEA:
        put_ea(b, o);
        if (o->size == 4) { put(b, a64_mov_w(A64_X3, A64_X1)); put(b, a64_str(A64_X3, CPU, off_r(o->reg))); }
        else                put(b, a64_str(A64_X1, CPU, off_r(o->reg)));
        return;

    case K_PUSH:
        put_set_rip(b, rip);
        put(b, a64_ldr(A64_X1, CPU, off_r(4)));
        put(b, a64_sub_imm(A64_X1, A64_X1, 8));
        put(b, a64_str(A64_X1, CPU, off_r(4)));
        put(b, a64_ldr(A64_X2, CPU, off_r(o->reg)));
        put_store(b, 8);
        return;

    case K_POP:
        put_set_rip(b, rip);
        put(b, a64_ldr(A64_X1, CPU, off_r(4)));
        put_load(b, 8);
        put(b, a64_str(A64_X0, CPU, off_r(o->reg)));
        put(b, a64_ldr(A64_X1, CPU, off_r(4)));
        put(b, a64_add_imm(A64_X1, A64_X1, 8));
        put(b, a64_str(A64_X1, CPU, off_r(4)));
        return;

    // --- block terminators: x0 = where to continue ---------------------
    case K_JMP:
        put_imm(b, A64_X0, o->target);
        put_leave(b);
        return;

    case K_JCC:
        put(b, a64_mov(A64_X0, CPU));
        put_imm(b, A64_X1, (uint64_t)o->cc);
        put_call(b, rsd_i_cond);
        put_imm(b, A64_X2, o->target);
        put_imm(b, A64_X3, o->next);
        put(b, a64_cmp0(A64_X0));
        put(b, a64_csel(A64_X0, A64_X2, A64_X3, A64_NE));
        put_leave(b);
        return;

    case K_CALL:
        put_set_rip(b, rip);
        put(b, a64_ldr(A64_X1, CPU, off_r(4)));
        put(b, a64_sub_imm(A64_X1, A64_X1, 8));
        put(b, a64_str(A64_X1, CPU, off_r(4)));
        put_imm(b, A64_X2, o->next);
        put_store(b, 8);
        put_imm(b, A64_X0, o->target);
        put_leave(b);
        return;

    case K_RET:
        put_set_rip(b, rip);
        put(b, a64_ldr(A64_X1, CPU, off_r(4)));
        put_load(b, 8);
        put(b, a64_str(A64_X0, CPU, off_tmp(0)));
        put(b, a64_ldr(A64_X1, CPU, off_r(4)));
        put(b, a64_add_imm(A64_X1, A64_X1, 8));
        put(b, a64_str(A64_X1, CPU, off_r(4)));
        put(b, a64_ldr(A64_X0, CPU, off_tmp(0)));
        put_leave(b);
        return;

    case K_CALLM:
        // Matches the interpreter's order: the address is computed first, the
        // return address pushed, then the target read.
        put_set_rip(b, rip);
        if (o->mem) { put_ea(b, o); put(b, a64_str(A64_X1, CPU, off_tmp(0))); }
        put(b, a64_ldr(A64_X1, CPU, off_r(4)));
        put(b, a64_sub_imm(A64_X1, A64_X1, 8));
        put(b, a64_str(A64_X1, CPU, off_r(4)));
        put_imm(b, A64_X2, o->next);
        put_store(b, 8);
        if (o->mem) { put(b, a64_ldr(A64_X1, CPU, off_tmp(0))); put_load(b, 8); }
        else          put(b, a64_ldr(A64_X0, CPU, off_r(o->reg)));
        put_leave(b);
        return;

    case K_JMPM:
        if (o->mem) { put_set_rip(b, rip); put_ea(b, o); put_load(b, 8); }
        else          put(b, a64_ldr(A64_X0, CPU, off_r(o->reg)));
        put_leave(b);
        return;
    }
    b->full = true;
}

// --- arena and cache ---------------------------------------------------

int rsd_jit_init(rsd_jit *j) {
    memset(j, 0, sizeof *j);
    j->arena = mmap(NULL, ARENA, PROT_READ | PROT_WRITE | PROT_EXEC,
                    MAP_PRIVATE | MAP_ANON | MAP_JIT, -1, 0);
    if (j->arena == MAP_FAILED) { perror("rashid: MAP_JIT"); return -1; }
    j->size = ARENA;
    // Compiled code addresses rsd_cpu fields with scaled 12-bit offsets.
    if (off_r(15) >= 32768 || OFF_CURRIP % 8 || OFF_CURRIP >= 32768 ||
        off_tmp(3) >= 4096 || OFF_RUNNING >= 4096 ||
        off_xmm(0) % 8 || off_xmm(15) + 8 >= 4096) {
        fprintf(stderr, "rashid: rsd_cpu layout is out of reach of the jit\n");
        return -1;
    }
    j->cache_bits = CACHE_BITS;
    j->cache = calloc((size_t)1 << CACHE_BITS, sizeof *j->cache);
    if (!j->cache) return -1;
    return 0;
}

void rsd_jit_free(rsd_jit *j) {
    if (j->arena && j->arena != MAP_FAILED) munmap(j->arena, j->size);
    free(j->cache);
    memset(j, 0, sizeof *j);
}

static size_t slot_of(const rsd_jit *j, uint64_t rip) {
    uint64_t h = rip * 0x9E3779B97F4A7C15ull;
    return (size_t)(h >> (64 - j->cache_bits));
}

uint32_t rsd_jit_count(const rsd_jit *j, uint64_t rip) {
    size_t s = slot_of(j, rip);
    return j->cache[s].tag == rip ? j->cache[s].count : 0;
}

void *rsd_jit_lookup(rsd_jit *j, rsd_cpu *c, uint64_t rip) {
    size_t s = slot_of(j, rip);
    if (j->cache[s].tag == rip) return j->cache[s].code;
    if (j->cache[s].code) return NULL;         // occupied by another address

    // Import stubs are recognised by the interpreter and must stay that way.
    if (rsd_stub_index(c->stubs, rip) >= 0) return NULL;
    // Compile. The guest's code is directly readable, addresses being shared.
    if (!rsd_as_ok(c->as, rip, 16, RSD_PROT_X)) return NULL;

    uint32_t words[MAX_BLOCK];
    buf b = { .out = words, .n = 0, .max = MAX_BLOCK - 24 };
    put(&b, a64_push(A64_X30));

    uint64_t pc = rip;
    int count = 0;
    bool ended = false;
    for (;;) {
        insn o;
        if (count >= MAX_INSNS || !rsd_as_ok(c->as, pc, 16, RSD_PROT_X)) break;
        if (!decode(rsd_g2h(c->as, pc), pc, &o)) {
            if (getenv("RASHID_JIT_DEBUG")) {
                const uint8_t *q = rsd_g2h(c->as, pc);
                fprintf(stderr, "rashid: jit stops at 0x%llx on %02x %02x %02x\n",
                        pc, q[0], q[1], q[2]);
            }
            break;
        }
        int n_before = b.n, fix_before = b.nfix;
        put_insn(&b, &o, pc);
        if (b.full) { b.n = n_before; b.nfix = fix_before; b.full = false; break; }
        pc = o.next;
        count++;
        if (terminates(o.kind)) { ended = true; break; }
    }

    // A single instruction is not worth the trip through the entry stub.
    if (count < 2) {
        j->cache[s].tag = rip; j->cache[s].code = NULL; j->bailed++; return NULL;
    }

    b.max = MAX_BLOCK;
    if (!ended) {                               // fell off the end: continue there
        put_imm(&b, A64_X0, pc);
        put_leave(&b);
    }
    int exit_at = b.n;                          // a helper faulted
    put(&b, a64_ldr(A64_X0, CPU, OFF_CURRIP));
    put_leave(&b);
    if (b.full) {
        j->cache[s].tag = rip; j->cache[s].code = NULL; j->bailed++; return NULL;
    }
    for (int i = 0; i < b.nfix; i++)
        words[b.fix[i]] = a64_cbz_w(A64_X9, exit_at - b.fix[i]);

    size_t bytes = (size_t)b.n * 4;
    if (j->used + bytes > j->size) return NULL;
    void *code = j->arena + j->used;

    pthread_jit_write_protect_np(0);
    memcpy(code, words, bytes);
    pthread_jit_write_protect_np(1);
    sys_icache_invalidate(code, bytes);

    j->used += bytes;
    j->compiled++;
    j->cache[s].tag = rip;
    j->cache[s].code = code;
    j->cache[s].count = (uint32_t)count;
    if (getenv("RASHID_JIT_DEBUG"))
        fprintf(stderr, "rashid: jit block 0x%llx..0x%llx = %d insn, %d words\n",
                rip, pc, count, b.n);
    return code;
}
