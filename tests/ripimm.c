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
// RIP-relative addressing combined with an immediate operand.
//
// The displacement is measured from the end of the *whole* instruction, so
// the immediate must be consumed before the displacement is resolved. Getting
// this wrong shifts every such access by the immediate's width - silently, to
// a valid-looking nearby address. Regression test for exactly that.
//
// Exits with a bitmask; the expected value is whatever real x86_64 hardware
// produces, which the test harness checks by running this binary natively.
static long sys3(long n, long a, long b, long c) {
    long ret;
    __asm__ volatile("syscall" : "=a"(ret)
                     : "a"(n), "D"(a), "S"(b), "d"(c) : "rcx", "r11", "memory");
    return ret;
}

static volatile long v_mov, v_add, v_sub, v_and, v_shl, v_mul, v_cmp, v_byte;

void start(void) {
    long acc = 0;

    v_mov = 0x11223344;                       // C7 /0  imm32
    if (v_mov == 0x11223344) acc |= 1;

    v_add = 5;   v_add = v_add + 0x1000;      // 81 /0  imm32
    if (v_add == 0x1005) acc |= 2;

    v_sub = 5;   v_sub = v_sub - 3;           // 83 /5  imm8
    if (v_sub == 2) acc |= 4;

    v_and = 0xff; v_and = v_and & 0x0f;       // 83 /4  imm8
    if (v_and == 0x0f) acc |= 8;

    v_shl = 1;   v_shl = v_shl << 5;          // C1 /4  imm8
    if (v_shl == 32) acc |= 16;

    v_mul = 3;   v_mul = v_mul * 100000;      // 69     imm32
    if (v_mul == 300000) acc |= 32;

    v_cmp = 0x1234;                           // 81 /7  imm32 (cmp)
    if (v_cmp == 0x1234) acc |= 64;

    *(volatile char *)&v_byte = 0x5a;         // C6 /0  imm8
    if ((*(volatile char *)&v_byte) == 0x5a) acc |= 128;

    sys3(0x2000001, acc, 0, 0);
    __builtin_unreachable();
}
