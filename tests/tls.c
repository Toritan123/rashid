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
// Thread-local storage. macOS x86_64 keeps the thread's TSD block at %gs and
// installs it with the machine-dependent syscall 0x3000003
// (thread_fast_set_cthread_self); pthread_getspecific is then a single
// instruction, movq %gs:(,%rdi,8), %rax. Nothing linked against libSystem
// gets far without this.
//
// Exits with a bitmask; 15 means every check passed. The same binary runs
// natively on an Intel Mac (or under Rosetta), so the expected value is
// checkable against real hardware.
static long sys3(long n, long a, long b, long c) {
    long ret;
    __asm__ volatile("syscall" : "=a"(ret)
                     : "a"(n), "D"(a), "S"(b), "d"(c) : "rcx", "r11", "memory");
    return ret;
}

static long tsd[64];

void start(void) {
    long acc = 0, v, ea;

    sys3(0x3000003, (long)tsd, 0, 0);          // install the TSD base

    tsd[7] = 55;                                // 1. scaled-index read,
    __asm__ volatile("movq %%gs:(,%1,8), %0"    //    i.e. pthread_getspecific
                     : "=r"(v) : "r"(7L));
    if (v == 55) acc |= 1;

    tsd[0] = 11;                                // 2. absolute displacement
    __asm__ volatile("movq %%gs:0x0, %0" : "=r"(v));
    if (v == 11) acc |= 2;

    __asm__ volatile("movq %0, %%gs:0x10"       // 3. write through %gs
                     :: "r"(22L) : "memory");
    if (tsd[2] == 22) acc |= 4;

    // 4. lea must ignore the segment override: 65 48 8d 04 25 40 00 00 00
    //    is "lea %gs:0x40, %rax", and the result is 0x40, not gs_base+0x40.
    __asm__ volatile(".byte 0x65, 0x48, 0x8d, 0x04, 0x25, 0x40, 0x00, 0x00, 0x00"
                     : "=a"(ea));
    if (ea == 0x40) acc |= 8;

    sys3(0x2000001, acc, 0, 0);
    __builtin_unreachable();
}
