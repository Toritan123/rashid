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
// rashid - AArch64 instruction encoding
//
// Just enough of the encoding to write the block compiler, kept separate so
// it can be checked against the assembler rather than trusted. Every encoder
// here has a case in tests/emit_test.c that assembles the same instruction
// with clang and compares the bytes.
#ifndef RASHID_EMIT_H
#define RASHID_EMIT_H

#include <stdint.h>

enum { A64_X0 = 0, A64_X1, A64_X2, A64_X3, A64_X4, A64_X5, A64_X6, A64_X7,
       A64_X8, A64_X9, A64_X10, A64_X11, A64_X12, A64_X13, A64_X14, A64_X15,
       A64_X16, A64_X17, A64_X18, A64_X19, A64_X20, A64_X21, A64_X22, A64_X23,
       A64_X24, A64_X25, A64_X26, A64_X27, A64_X28, A64_X29, A64_X30,
       A64_SP = 31, A64_ZR = 31 };

// ldr/str Xt, [Xn, #off]   off must be a multiple of 8, 0..32760
static inline uint32_t a64_ldr(int t, int n, uint32_t off) {
    return 0xF9400000u | ((off / 8) << 10) | ((uint32_t)n << 5) | (uint32_t)t;
}
static inline uint32_t a64_str(int t, int n, uint32_t off) {
    return 0xF9000000u | ((off / 8) << 10) | ((uint32_t)n << 5) | (uint32_t)t;
}

// str Xt, [sp, #-16]!   and   ldr Xt, [sp], #16
static inline uint32_t a64_push(int t) {
    return 0xF8000C00u | ((0x1F0u & 0x1FF) << 12) | (31u << 5) | (uint32_t)t;
}
static inline uint32_t a64_pop(int t) {
    return 0xF8400400u | ((16u & 0x1FF) << 12) | (31u << 5) | (uint32_t)t;
}

// movz/movk Xd, #imm16, lsl #(16*hw)
static inline uint32_t a64_movz(int d, uint16_t imm, int hw) {
    return 0xD2800000u | ((uint32_t)hw << 21) | ((uint32_t)imm << 5) | (uint32_t)d;
}
static inline uint32_t a64_movk(int d, uint16_t imm, int hw) {
    return 0xF2800000u | ((uint32_t)hw << 21) | ((uint32_t)imm << 5) | (uint32_t)d;
}

// mov Xd, Xm  (orr Xd, xzr, Xm)
static inline uint32_t a64_mov(int d, int m) {
    return 0xAA0003E0u | ((uint32_t)m << 16) | (uint32_t)d;
}

// Three-register arithmetic, 64-bit.
static inline uint32_t a64_add(int d, int n, int m) {
    return 0x8B000000u | ((uint32_t)m << 16) | ((uint32_t)n << 5) | (uint32_t)d;
}
static inline uint32_t a64_sub(int d, int n, int m) {
    return 0xCB000000u | ((uint32_t)m << 16) | ((uint32_t)n << 5) | (uint32_t)d;
}
static inline uint32_t a64_and(int d, int n, int m) {
    return 0x8A000000u | ((uint32_t)m << 16) | ((uint32_t)n << 5) | (uint32_t)d;
}
static inline uint32_t a64_orr(int d, int n, int m) {
    return 0xAA000000u | ((uint32_t)m << 16) | ((uint32_t)n << 5) | (uint32_t)d;
}
static inline uint32_t a64_eor(int d, int n, int m) {
    return 0xCA000000u | ((uint32_t)m << 16) | ((uint32_t)n << 5) | (uint32_t)d;
}

// The same operations on 32-bit registers, which is the whole reason they
// are here: a W-register result is zero-extended into the X register, which
// is exactly what a 32-bit x86 operation does to the upper half.
static inline uint32_t a64_add_w(int d, int n, int m) {
    return 0x0B000000u | ((uint32_t)m << 16) | ((uint32_t)n << 5) | (uint32_t)d;
}
static inline uint32_t a64_sub_w(int d, int n, int m) {
    return 0x4B000000u | ((uint32_t)m << 16) | ((uint32_t)n << 5) | (uint32_t)d;
}
static inline uint32_t a64_and_w(int d, int n, int m) {
    return 0x0A000000u | ((uint32_t)m << 16) | ((uint32_t)n << 5) | (uint32_t)d;
}
static inline uint32_t a64_orr_w(int d, int n, int m) {
    return 0x2A000000u | ((uint32_t)m << 16) | ((uint32_t)n << 5) | (uint32_t)d;
}
static inline uint32_t a64_eor_w(int d, int n, int m) {
    return 0x4A000000u | ((uint32_t)m << 16) | ((uint32_t)n << 5) | (uint32_t)d;
}
static inline uint32_t a64_mov_w(int d, int m) {
    return 0x2A0003E0u | ((uint32_t)m << 16) | (uint32_t)d;
}

// add Xd, Xn, #imm12
static inline uint32_t a64_add_imm(int d, int n, uint32_t imm) {
    return 0x91000000u | (imm << 10) | ((uint32_t)n << 5) | (uint32_t)d;
}
static inline uint32_t a64_sub_imm(int d, int n, uint32_t imm) {
    return 0xD1000000u | (imm << 10) | ((uint32_t)n << 5) | (uint32_t)d;
}

// add Xd, Xn, Xm, lsl #shift   - an index register times its scale
static inline uint32_t a64_add_shift(int d, int n, int m, int shift) {
    return 0x8B000000u | ((uint32_t)m << 16) | ((uint32_t)shift << 10) |
           ((uint32_t)n << 5) | (uint32_t)d;
}

// ldrb Wt, [Xn, #off]
static inline uint32_t a64_ldrb(int t, int n, uint32_t off) {
    return 0x39400000u | (off << 10) | ((uint32_t)n << 5) | (uint32_t)t;
}

// cbz Wt, <pc + 4*imm19>   - imm19 is patched once the target is known
static inline uint32_t a64_cbz_w(int t, int32_t imm19) {
    return 0x34000000u | (((uint32_t)imm19 & 0x7FFFF) << 5) | (uint32_t)t;
}

// cmp Xn, #0
static inline uint32_t a64_cmp0(int n) {
    return 0xF100001Fu | ((uint32_t)n << 5);
}

// csel Xd, Xn, Xm, <cond>
enum { A64_EQ = 0, A64_NE = 1 };
static inline uint32_t a64_csel(int d, int n, int m, int cond) {
    return 0x9A800000u | ((uint32_t)m << 16) | ((uint32_t)cond << 12) |
           ((uint32_t)n << 5) | (uint32_t)d;
}

// bic Xd, Xn, Xm   (Xn AND NOT Xm)
static inline uint32_t a64_bic(int d, int n, int m) {
    return 0x8A200000u | ((uint32_t)m << 16) | ((uint32_t)n << 5) | (uint32_t)d;
}

// csetm Xd/Wd, cond: all ones if cond holds, else zero (csinv with the
// inverted condition)
static inline uint32_t a64_csetm(int d, int cond) {
    return 0xDA9F03E0u | ((uint32_t)(cond ^ 1) << 12) | (uint32_t)d;
}
static inline uint32_t a64_csetm_w(int d, int cond) {
    return 0x5A9F03E0u | ((uint32_t)(cond ^ 1) << 12) | (uint32_t)d;
}

// --- floating point -----------------------------------------------------
// Guest XMM lanes live in rsd_cpu, so these load and store them directly.

// ldr/str Dt, [Xn, #off]   (off multiple of 8)   and   St / Wt (multiple of 4)
static inline uint32_t a64_ldr_d(int t, int n, uint32_t off) {
    return 0xFD400000u | ((off / 8) << 10) | ((uint32_t)n << 5) | (uint32_t)t;
}
static inline uint32_t a64_str_d(int t, int n, uint32_t off) {
    return 0xFD000000u | ((off / 8) << 10) | ((uint32_t)n << 5) | (uint32_t)t;
}
static inline uint32_t a64_ldr_s(int t, int n, uint32_t off) {
    return 0xBD400000u | ((off / 4) << 10) | ((uint32_t)n << 5) | (uint32_t)t;
}
static inline uint32_t a64_str_s(int t, int n, uint32_t off) {
    return 0xBD000000u | ((off / 4) << 10) | ((uint32_t)n << 5) | (uint32_t)t;
}
static inline uint32_t a64_ldr_w(int t, int n, uint32_t off) {
    return 0xB9400000u | ((off / 4) << 10) | ((uint32_t)n << 5) | (uint32_t)t;
}
static inline uint32_t a64_str_w(int t, int n, uint32_t off) {
    return 0xB9000000u | ((off / 4) << 10) | ((uint32_t)n << 5) | (uint32_t)t;
}

// Two-operand arithmetic, double (d) and single (s).
enum { FOP_MUL = 0x0800, FOP_DIV = 0x1800, FOP_ADD = 0x2800, FOP_SUB = 0x3800 };
static inline uint32_t a64_fop_d(int op, int d, int n, int m) {
    return 0x1E600000u | (uint32_t)op | ((uint32_t)m << 16) | ((uint32_t)n << 5) | (uint32_t)d;
}
static inline uint32_t a64_fop_s(int op, int d, int n, int m) {
    return 0x1E200000u | (uint32_t)op | ((uint32_t)m << 16) | ((uint32_t)n << 5) | (uint32_t)d;
}
static inline uint32_t a64_fsqrt_d(int d, int n) { return 0x1E61C000u | ((uint32_t)n << 5) | (uint32_t)d; }
static inline uint32_t a64_fsqrt_s(int d, int n) { return 0x1E21C000u | ((uint32_t)n << 5) | (uint32_t)d; }

static inline uint32_t a64_fcmp_d(int n, int m) { return 0x1E602000u | ((uint32_t)m << 16) | ((uint32_t)n << 5); }
static inline uint32_t a64_fcmp_s(int n, int m) { return 0x1E202000u | ((uint32_t)m << 16) | ((uint32_t)n << 5); }

enum { A64_MI = 4, A64_PL = 5, A64_VS = 6, A64_VC = 7, A64_HI = 8, A64_LS = 9, A64_GT = 12 };
static inline uint32_t a64_fcsel_d(int d, int n, int m, int cond) {
    return 0x1E600C00u | ((uint32_t)m << 16) | ((uint32_t)cond << 12) | ((uint32_t)n << 5) | (uint32_t)d;
}
static inline uint32_t a64_fcsel_s(int d, int n, int m, int cond) {
    return 0x1E200C00u | ((uint32_t)m << 16) | ((uint32_t)cond << 12) | ((uint32_t)n << 5) | (uint32_t)d;
}

// Bit copies between integer and floating-point registers.
static inline uint32_t a64_fmov_dx(int d, int n) { return 0x9E670000u | ((uint32_t)n << 5) | (uint32_t)d; }
static inline uint32_t a64_fmov_xd(int d, int n) { return 0x9E660000u | ((uint32_t)n << 5) | (uint32_t)d; }
static inline uint32_t a64_fmov_sw(int d, int n) { return 0x1E270000u | ((uint32_t)n << 5) | (uint32_t)d; }
static inline uint32_t a64_fmov_ws(int d, int n) { return 0x1E260000u | ((uint32_t)n << 5) | (uint32_t)d; }

// Signed integer to floating point, and between the two widths.
static inline uint32_t a64_scvtf_dx(int d, int n) { return 0x9E620000u | ((uint32_t)n << 5) | (uint32_t)d; }
static inline uint32_t a64_scvtf_dw(int d, int n) { return 0x1E620000u | ((uint32_t)n << 5) | (uint32_t)d; }
static inline uint32_t a64_scvtf_sx(int d, int n) { return 0x9E220000u | ((uint32_t)n << 5) | (uint32_t)d; }
static inline uint32_t a64_scvtf_sw(int d, int n) { return 0x1E220000u | ((uint32_t)n << 5) | (uint32_t)d; }
static inline uint32_t a64_fcvt_sd(int d, int n)  { return 0x1E624000u | ((uint32_t)n << 5) | (uint32_t)d; } // s <- d
static inline uint32_t a64_fcvt_ds(int d, int n)  { return 0x1E22C000u | ((uint32_t)n << 5) | (uint32_t)d; } // d <- s

// b.<cond> <pc + 4*imm19>
static inline uint32_t a64_bcond(int cond, int32_t imm19) {
    return 0x54000000u | (((uint32_t)imm19 & 0x7FFFF) << 5) | (uint32_t)cond;
}

static inline uint32_t a64_blr(int n) { return 0xD63F0000u | ((uint32_t)n << 5); }
static inline uint32_t a64_ret(void)  { return 0xD65F03C0u; }

#endif
