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
#define MAX_BLOCK 256          // emitted words before a block is closed

// The interpreter's own routines, so the two engines share one definition of
// what an instruction means.
uint64_t rsd_i_ld(rsd_cpu *, uint64_t, uint64_t);
void     rsd_i_st(rsd_cpu *, uint64_t, uint64_t, uint64_t);
void     rsd_i_flags_add(rsd_cpu *, uint64_t, uint64_t, uint64_t, uint64_t);
void     rsd_i_flags_sub(rsd_cpu *, uint64_t, uint64_t, uint64_t, uint64_t);
void     rsd_i_flags_logic(rsd_cpu *, uint64_t, uint64_t);
uint64_t rsd_i_cond(rsd_cpu *, uint64_t);

#define CPU A64_X19            // compiled code's only base register

typedef struct {
    uint32_t *out;
    int       n, max;
    bool      full;
} buf;

static void put(buf *b, uint32_t insn) {
    if (b->n >= b->max) { b->full = true; return; }
    b->out[b->n++] = insn;
}

// A 64-bit constant, in as few movz/movk as it needs.
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

static uint32_t reg_off(int r) { return (uint32_t)(r * 8); }

// --- the compilable subset -------------------------------------------
// Only 64-bit register-to-register forms. Everything else ends the block and
// the interpreter continues, which keeps the compiler small enough to trust
// and lets it grow one measurement at a time.

typedef struct { int op; int dst; int src; uint64_t imm;
                 bool has_imm; bool no_write; int size; } insn;

// Decodes one instruction. Returns its length, or 0 if it is not one of ours.
static int decode(const uint8_t *p, insn *o) {
    memset(o, 0, sizeof *o);
    int n = 0;
    uint8_t rex = 0;
    if ((p[0] & 0xF0) == 0x40) { rex = p[n++]; }
    o->size = (rex & 8) ? 8 : 4;               // 32- and 64-bit forms

    uint8_t op = p[n++];
    uint8_t modrm;

    switch (op) {
    case 0x01: case 0x03: case 0x09: case 0x0B:
    case 0x21: case 0x23: case 0x29: case 0x2B:
    case 0x31: case 0x33: case 0x39: case 0x3B:
    case 0x85: case 0x89: case 0x8B: {
        modrm = p[n++];
        if ((modrm >> 6) != 3) return 0;       // register operands only
        int reg = ((modrm >> 3) & 7) | ((rex & 4) ? 8 : 0);
        int rm  = (modrm & 7) | ((rex & 1) ? 8 : 0);
        bool to_reg = (op & 2) && op != 0x85;
        o->dst = to_reg ? reg : rm;
        o->src = to_reg ? rm : reg;
        o->op = (op == 0x85) ? 0x85 : (op == 0x89 || op == 0x8B) ? 0x88 : (op & ~3);
        o->no_write = (o->op == 0x38 || o->op == 0x85);
        return n;
    }
    case 0x81: case 0x83: {
        modrm = p[n++];
        if ((modrm >> 6) != 3) return 0;
        int ext = (modrm >> 3) & 7;
        if (ext == 2 || ext == 3) return 0;    // adc/sbb need the carry in
        o->dst = (modrm & 7) | ((rex & 1) ? 8 : 0);
        if (op == 0x83) { o->imm = (uint64_t)(int64_t)(int8_t)p[n]; n += 1; }
        else { int32_t v; memcpy(&v, p + n, 4); o->imm = (uint64_t)(int64_t)v; n += 4; }
        if (o->size == 4) o->imm &= 0xffffffffu;
        o->has_imm = true;
        static const int map[8] = { 0x00, 0x08, 0, 0, 0x20, 0x28, 0x30, 0x38 };
        o->op = map[ext];
        o->no_write = (ext == 7);
        return n;
    }
    case 0xB8: case 0xB9: case 0xBA: case 0xBB:
    case 0xBC: case 0xBD: case 0xBE: case 0xBF: {
        o->dst = (op & 7) | ((rex & 1) ? 8 : 0);
        if (o->size == 8) { memcpy(&o->imm, p + n, 8); n += 8; }
        else { uint32_t v; memcpy(&v, p + n, 4); o->imm = v; n += 4; }
        o->has_imm = true;
        o->op = 0x88;                          // mov
        return n;
    }
    default:
        return 0;
    }
}

// x1 = first operand, x2 = second, x3 = result - which is also where the
// flag helpers want them, so no shuffling is needed before the call.
static void emit_one(buf *b, const insn *o) {
    put(b, a64_ldr(A64_X1, CPU, reg_off(o->dst)));
    if (o->has_imm) put_imm(b, A64_X2, o->imm);
    else            put(b, a64_ldr(A64_X2, CPU, reg_off(o->src)));

    bool w = (o->size == 4);
    void *flags = NULL;
    switch (o->op) {
    case 0x88: put(b, w ? a64_mov_w(A64_X3, A64_X2) : a64_mov(A64_X3, A64_X2)); break;
    case 0x00: put(b, w ? a64_add_w(A64_X3, A64_X1, A64_X2)
                        : a64_add(A64_X3, A64_X1, A64_X2));
               flags = rsd_i_flags_add; break;
    case 0x28: case 0x38:
               put(b, w ? a64_sub_w(A64_X3, A64_X1, A64_X2)
                        : a64_sub(A64_X3, A64_X1, A64_X2));
               flags = rsd_i_flags_sub; break;
    case 0x20: case 0x85:
               put(b, w ? a64_and_w(A64_X3, A64_X1, A64_X2)
                        : a64_and(A64_X3, A64_X1, A64_X2));
               flags = rsd_i_flags_logic; break;
    case 0x08: put(b, w ? a64_orr_w(A64_X3, A64_X1, A64_X2)
                        : a64_orr(A64_X3, A64_X1, A64_X2));
               flags = rsd_i_flags_logic; break;
    case 0x30: put(b, w ? a64_eor_w(A64_X3, A64_X1, A64_X2)
                        : a64_eor(A64_X3, A64_X1, A64_X2));
               flags = rsd_i_flags_logic; break;
    default:   b->full = true; return;
    }

    if (!o->no_write) put(b, a64_str(A64_X3, CPU, reg_off(o->dst)));

    if (flags == (void *)rsd_i_flags_logic) {
        put(b, a64_mov(A64_X0, CPU));
        put(b, a64_mov(A64_X1, A64_X3));
        put_imm(b, A64_X2, (uint64_t)o->size);
        put_call(b, flags);
    } else if (flags) {
        put(b, a64_mov(A64_X0, CPU));
        put_imm(b, A64_X4, (uint64_t)o->size);
        put_call(b, flags);
    }
}

// --- arena and cache ---------------------------------------------------

int rsd_jit_init(rsd_jit *j) {
    memset(j, 0, sizeof *j);
    j->arena = mmap(NULL, ARENA, PROT_READ | PROT_WRITE | PROT_EXEC,
                    MAP_PRIVATE | MAP_ANON | MAP_JIT, -1, 0);
    if (j->arena == MAP_FAILED) { perror("rashid: MAP_JIT"); return -1; }
    j->size = ARENA;
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

    // Compile. The guest's code is directly readable, addresses being shared.
    if (!rsd_as_ok(c->as, rip, 16, RSD_PROT_X)) return NULL;
    const uint8_t *p = rsd_g2h(c->as, rip);

    uint32_t words[MAX_BLOCK];
    buf b = { words, 0, MAX_BLOCK, false };
    put(&b, a64_push(A64_X30));

    uint64_t pc = rip;
    int count = 0;
    for (;;) {
        insn o;
        if (!rsd_as_ok(c->as, pc, 16, RSD_PROT_X)) break;
        int len = decode(rsd_g2h(c->as, pc), &o);
        if (!len) {
            if (getenv("RASHID_JIT_DEBUG")) {
                const uint8_t *q = rsd_g2h(c->as, pc);
                fprintf(stderr, "rashid: jit stops at 0x%llx on %02x %02x %02x\n",
                        pc, q[0], q[1], q[2]);
            }
            break;
        }
        int before = b.n;
        emit_one(&b, &o);
        if (b.full) { b.n = before; break; }
        pc += (uint64_t)len;
        count++;
    }
    (void)p;

    // A block of one is not worth the call overhead.
    // Remember the refusal too, so the next thousand visits are one compare.
    if (count < 2) {
        j->cache[s].tag = rip; j->cache[s].code = NULL; j->bailed++; return NULL;
    }

    put_imm(&b, A64_X0, pc);
    put(&b, a64_pop(A64_X30));
    put(&b, a64_ret());
    if (b.full) {
        j->cache[s].tag = rip; j->cache[s].code = NULL; j->bailed++; return NULL;
    }

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
