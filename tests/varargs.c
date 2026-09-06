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
// Variadic calls across the ABI boundary.
//
// System V hands the first few arguments to the callee in registers; macOS
// arm64 puts every variadic argument on the stack. Bridging the two needs the
// argument count and types, which for the formatted-IO family come from the
// format string. This exercises the awkward parts: mixed integer and
// floating-point, more integers than System V has registers for, more doubles
// than it has xmm registers for, a width taken from an argument, and the
// scanf direction where every conversion consumes a pointer instead.
#include <stdio.h>

int main(void) {
    printf("%d %s %.2f %c %x %lld\n",
           42, "str", 3.25, 'Z', 255, 1234567890123LL);

    printf("%d %d %d %d %d %d %d %d\n", 1, 2, 3, 4, 5, 6, 7, 8);

    printf("%.1f %.1f %.1f %.1f %.1f %.1f %.1f %.1f %.1f\n",
           1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0, 9.0);

    printf("[%*d] [%-6s]\n", 5, 42, "pad");

    char buf[64];
    snprintf(buf, sizeof buf, "%s=%d/%.3f", "key", 7, 0.125);
    puts(buf);

    int a = 0, b = 0;
    sscanf("10 20", "%d %d", &a, &b);
    printf("sum=%d\n", a + b);

    return 0;
}
