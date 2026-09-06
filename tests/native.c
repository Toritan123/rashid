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
// A normally linked binary whose work is done by native arm64 libSystem.
//
// Every call here crosses the thunk boundary: translated x86_64 code sets up
// System V arguments, rashid converts them to AAPCS64 and enters the real
// arm64 implementation, and the result comes back in rax. The frameworks are
// never translated - they are already on the machine in the right
// architecture, which is the whole premise.
//
// Deliberately avoids variadic functions, which need stack marshalling that
// is not implemented yet.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <math.h>

int main(int argc, char **argv) {
    int ok = 0;

    puts("running through native arm64 libSystem");

    if (strlen("translated") == 10) ok++;
    if (strcmp("abc", "abc") == 0) ok++;
    if (strcmp("abc", "abd") < 0) ok++;
    if (strchr("hello", 'l') != NULL) ok++;

    char buf[32];
    memset(buf, 'x', sizeof buf);
    if (buf[0] == 'x' && buf[31] == 'x') ok++;
    if (memcmp(buf, buf, sizeof buf) == 0) ok++;

    char *p = malloc(64);
    if (p) { memcpy(p, "heap", 5); if (strcmp(p, "heap") == 0) ok++; free(p); }

    if (atoi("1234") == 1234) ok++;
    if (toupper('q') == 'Q') ok++;
    if (abs(-7) == 7) ok++;

    // double arguments and a double return, so xmm0-7 and v0-v7 both matter
    if (sqrt(144.0) == 12.0) ok++;
    if (fabs(-2.5) == 2.5) ok++;
    if (fmax(3.0, 4.0) == 4.0) ok++;

    if (argc >= 1 && argv[0] != NULL) ok++;

    puts(ok == 14 ? "all native calls returned correctly"
                  : "some native call went wrong");
    return ok;
}
