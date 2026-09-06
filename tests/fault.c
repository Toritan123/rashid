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
// A guest that misbehaves. rashid must report a clean fault and survive: the
// whole point of the separate guest address space is that a wild guest
// pointer cannot reach the translator.
//
// Pass an argument to pick which way to misbehave:
//   (none)  read through a NULL guest pointer
//   w       write into the guest's own read-only __TEXT
//   f       jump far outside the guest address space
static long sys3(long n, long a, long b, long c) {
    long ret;
    __asm__ volatile("syscall" : "=a"(ret)
                     : "a"(n), "D"(a), "S"(b), "d"(c) : "rcx", "r11", "memory");
    return ret;
}

int start(int argc, char **argv) {
    char mode = (argc > 1) ? argv[1][0] : 0;

    if (mode == 'w') {
        *(volatile long *)start = 0;              // write to read-only __TEXT
    } else if (mode == 'f') {
        ((void (*)(void))0x7fffffffffff0000UL)();  // jump out of guest space
    } else {
        volatile long v = *(volatile long *)0;     // NULL read
        (void)v;
    }
    sys3(0x2000001, 0, 0, 0);
    __builtin_unreachable();
}
