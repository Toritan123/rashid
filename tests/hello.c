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
// Freestanding x86_64 guest: no libSystem, so rashid M0 can run it without a
// dynamic loader or thunk layer. macOS BSD syscalls carry class 2 (0x2000000).
static long sys3(long n, long a, long b, long c) {
    long ret;
    __asm__ volatile("syscall"
                     : "=a"(ret)
                     : "a"(n), "D"(a), "S"(b), "d"(c)
                     : "rcx", "r11", "memory");
    return ret;
}

static const char msg[] = "hello from a translated x86_64 guest\n";

void start(void) {
    sys3(0x2000004, 1, (long)msg, sizeof msg - 1);   // write
    sys3(0x2000001, 0, 0, 0);                        // exit
    __builtin_unreachable();
}
