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
#include "as.h"
#include "macho.h"
#include "cpu.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void usage(void) {
    fprintf(stderr,
        "usage: rashid [-t] [-n N] <x86_64-mach-o> [args...]\n"
        "  -t     trace every instruction\n"
        "  -n N   stop after N instructions (default 10000000)\n"
        "  -l     load and dump the image only, do not execute\n"
        "  -m     dump the guest address space map after loading\n");
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);   // keep host and guest writes interleaved

    bool trace = false, load_only = false, show_map = false;
    uint64_t budget = 10000000;
    int i = 1;

    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        if (!strcmp(argv[i], "-t")) trace = true;
        else if (!strcmp(argv[i], "-l")) load_only = true;
        else if (!strcmp(argv[i], "-m")) show_map = true;
        else if (!strcmp(argv[i], "-n") && i + 1 < argc) budget = strtoull(argv[++i], NULL, 0);
        else { usage(); return 2; }
    }
    if (i >= argc) { usage(); return 2; }

    rsd_as as;
    if (rsd_as_init(&as) < 0) return 1;

    rsd_image img;
    if (rsd_load(&img, &as, argv[i]) < 0) return 1;
    if (rsd_fixups(&img, &as) < 0) return 1;
    rsd_dump(&img);

    rsd_stubs stubs;
    rsd_stubs_of(&img, &stubs);
    if (show_map) { printf("\n"); rsd_as_dump(&as); }

    if (img.nimports && !load_only)
        fprintf(stderr,
            "rashid: %d import(s) have no thunk yet; execution will stop at the "
            "first one called.\n", img.nimports);

    if (load_only) return 0;

    printf("\n-- executing --\n");
    rsd_cpu cpu;
    if (rsd_cpu_init(&cpu, &as, img.entry, argc - i, argv + i) < 0) return 1;
    cpu.stubs = &stubs;
    cpu.trace = trace;
    rsd_cpu_run(&cpu, budget);

    if (cpu.fault) { rsd_cpu_dump(&cpu); rsd_cpu_free(&cpu); return 1; }

    printf("-- guest exited with status %d after %llu instructions --\n",
           cpu.exit_code, cpu.icount);
    rsd_cpu_free(&cpu);
    return cpu.exit_code;
}
