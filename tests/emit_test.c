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
#include <stdio.h>
#include <string.h>
#include "../src/emit.h"
static int fails;
static void check(const char *asmtext, uint32_t got) {
    char cmd[512], path[] = "/tmp/et_XXXXXX";
    FILE *f = fopen("/tmp/et.s", "w");
    fprintf(f, ".text\n%s\n", asmtext); fclose(f);
    snprintf(cmd, sizeof cmd,
             "clang -arch arm64 -c /tmp/et.s -o /tmp/et.o 2>/dev/null && "
             "otool -t /tmp/et.o 2>/dev/null | sed -n '3p' | awk '{print $2}'");
    FILE *p = popen(cmd, "r"); char out[64] = {0};
    if (fgets(out, sizeof out, p)) {}
    pclose(p);
    // otool -t already prints each word in its natural order on arm64.
    unsigned w = 0; sscanf(out, "%x", &w);
    printf("  %-34s emit=%08x asm=%08x %s\n", asmtext, got, w,
           got == w ? "ok" : "MISMATCH");
    if (got != w) fails++;
    (void)path;
}
int main(void) {
    check("ldr x3, [x19, #64]",      a64_ldr(3, 19, 64));
    check("str x5, [x19, #128]",     a64_str(5, 19, 128));
    check("str x30, [sp, #-16]!",    a64_push(30));
    check("ldr x30, [sp], #16",      a64_pop(30));
    check("movz x1, #4660",          a64_movz(1, 4660, 0));
    check("movk x1, #22136, lsl #16",a64_movk(1, 22136, 1));
    check("mov x2, x7",              a64_mov(2, 7));
    check("add x1, x2, x3",          a64_add(1, 2, 3));
    check("sub x1, x2, x3",          a64_sub(1, 2, 3));
    check("and x1, x2, x3",          a64_and(1, 2, 3));
    check("orr x1, x2, x3",          a64_orr(1, 2, 3));
    check("eor x1, x2, x3",          a64_eor(1, 2, 3));
    check("add x1, x2, #17",         a64_add_imm(1, 2, 17));
    check("sub x1, x2, #17",         a64_sub_imm(1, 2, 17));
    check("add w1, w2, w3",          a64_add_w(1, 2, 3));
    check("sub w1, w2, w3",          a64_sub_w(1, 2, 3));
    check("and w1, w2, w3",          a64_and_w(1, 2, 3));
    check("orr w1, w2, w3",          a64_orr_w(1, 2, 3));
    check("eor w1, w2, w3",          a64_eor_w(1, 2, 3));
    check("mov w2, w7",              a64_mov_w(2, 7));
    check("add x5, x5, x6, lsl #3",  a64_add_shift(5, 5, 6, 3));
    check("ldrb w9, [x19, #200]",    a64_ldrb(9, 19, 200));
    check("cbz w9, .+8",             a64_cbz_w(9, 2));
    check("cbz w9, .-8",             a64_cbz_w(9, -2));
    check("cmp x0, #0",              a64_cmp0(0));
    check("csel x0, x2, x3, ne",     a64_csel(0, 2, 3, A64_NE));
    check("bic x3, x2, x1",          a64_bic(3, 2, 1));
    check("csetm x3, mi",            a64_csetm(3, A64_MI));
    check("csetm x3, eq",            a64_csetm(3, A64_EQ));
    check("csetm w3, hi",            a64_csetm_w(3, A64_HI));
    check("ldr d1, [x19, #136]",     a64_ldr_d(1, 19, 136));
    check("str d0, [x19, #152]",     a64_str_d(0, 19, 152));
    check("ldr s1, [x19, #136]",     a64_ldr_s(1, 19, 136));
    check("str s0, [x19, #152]",     a64_str_s(0, 19, 152));
    check("ldr w3, [x19, #140]",     a64_ldr_w(3, 19, 140));
    check("str w3, [x19, #140]",     a64_str_w(3, 19, 140));
    check("fadd d0, d0, d1",         a64_fop_d(FOP_ADD, 0, 0, 1));
    check("fsub d0, d0, d1",         a64_fop_d(FOP_SUB, 0, 0, 1));
    check("fmul d0, d0, d1",         a64_fop_d(FOP_MUL, 0, 0, 1));
    check("fdiv d0, d0, d1",         a64_fop_d(FOP_DIV, 0, 0, 1));
    check("fadd s0, s0, s1",         a64_fop_s(FOP_ADD, 0, 0, 1));
    check("fdiv s0, s0, s1",         a64_fop_s(FOP_DIV, 0, 0, 1));
    check("fsqrt d0, d1",            a64_fsqrt_d(0, 1));
    check("fsqrt s0, s1",            a64_fsqrt_s(0, 1));
    check("fcmp d0, d1",             a64_fcmp_d(0, 1));
    check("fcmp s0, s1",             a64_fcmp_s(0, 1));
    check("fcsel d0, d0, d1, mi",    a64_fcsel_d(0, 0, 1, A64_MI));
    check("fcsel s0, s0, s1, gt",    a64_fcsel_s(0, 0, 1, A64_GT));
    check("fmov d1, x0",             a64_fmov_dx(1, 0));
    check("fmov x2, d0",             a64_fmov_xd(2, 0));
    check("fmov s1, w0",             a64_fmov_sw(1, 0));
    check("fmov w2, s0",             a64_fmov_ws(2, 0));
    check("scvtf d0, x1",            a64_scvtf_dx(0, 1));
    check("scvtf d0, w1",            a64_scvtf_dw(0, 1));
    check("scvtf s0, x1",            a64_scvtf_sx(0, 1));
    check("scvtf s0, w1",            a64_scvtf_sw(0, 1));
    check("fcvt s0, d1",             a64_fcvt_sd(0, 1));
    check("fcvt d0, s1",             a64_fcvt_ds(0, 1));
    check("b.vc .+36",               a64_bcond(A64_VC, 9));
    check("blr x9",                  a64_blr(9));
    check("ret",                     a64_ret());
    printf("%s\n", fails ? "ENCODING ERRORS" : "all encodings match the assembler");
    return fails != 0;
}
