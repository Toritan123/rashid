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
// Exercises the ALU, flags, conditional branches, calls and multiply/divide
// paths of the interpreter. Exits with 55 (= 1+2+...+10).
static long sys3(long n, long a, long b, long c) {
    long ret;
    __asm__ volatile("syscall"
                     : "=a"(ret)
                     : "a"(n), "D"(a), "S"(b), "d"(c)
                     : "rcx", "r11", "memory");
    return ret;
}

__attribute__((noinline))
static long triangle(long n) {
    long s = 0;
    for (long i = 1; i <= n; i++) s += i;
    return s;
}

__attribute__((noinline))
static long checks(void) {
    long acc = 0;
    if (triangle(4) == 10)        acc |= 1;
    if ((7 * 6) / 3 == 14)        acc |= 2;
    if ((-9 / 2) == -4)           acc |= 4;   // signed division
    if ((1u << 5) == 32)          acc |= 8;
    if (((unsigned char)-1) == 255) acc |= 16;
    return acc;
}

void start(void) {
    if (checks() != 31)
        sys3(0x2000001, 1, 0, 0);            // exit(1) on any check failure
    sys3(0x2000001, triangle(10), 0, 0);     // exit(55)
    __builtin_unreachable();
}
