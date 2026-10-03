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
#include "jit.h"

#include <dlfcn.h>
#include <signal.h>
#include <stdio.h>
#include <sys/ucontext.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>

// When a guest mistake - or a rashid one - takes the host process down,
// report what was being executed rather than dying silently. Thunks call
// native code with guest-supplied pointers, and compiled blocks run as native
// code, so a host fault is a real possibility.
static rsd_cpu *crash_cpu;
static rsd_jit *crash_jit;

static void on_crash(int sig, siginfo_t *si, void *ctx) {
    ucontext_t *uc = ctx;
    uint64_t pc = (uint64_t)uc->uc_mcontext->__ss.__pc;
    char buf[320];
    int n = snprintf(buf, sizeof buf,
                     "\nrashid: host %s at %p (host pc 0x%llx)\n",
                     sig == SIGSEGV ? "SIGSEGV" : "SIGBUS", si->si_addr, pc);
    if (n > 0) write(2, buf, (size_t)n);
    Dl_info info;
    if (dladdr((void *)(uintptr_t)pc, &info) && info.dli_sname) {
        n = snprintf(buf, sizeof buf, "        in %s+0x%llx\n", info.dli_sname,
                     pc - (uint64_t)(uintptr_t)info.dli_saddr);
        if (n > 0) write(2, buf, (size_t)n);
    }
    if (crash_jit && pc >= (uint64_t)(uintptr_t)crash_jit->arena &&
        pc < (uint64_t)(uintptr_t)crash_jit->arena + crash_jit->used) {
        n = snprintf(buf, sizeof buf, "        inside compiled code\n");
        if (n > 0) write(2, buf, (size_t)n);
    }
    if (crash_cpu) {
        n = snprintf(buf, sizeof buf,
                     "        guest rip 0x%llx, last instruction 0x%llx\n",
                     crash_cpu->rip, crash_cpu->cur_rip);
        if (n > 0) write(2, buf, (size_t)n);
    }
    signal(sig, SIG_DFL);
    raise(sig);
}

static void usage(void) {
    fprintf(stderr,
        "usage: rashid [-t] [-n N] <x86_64-mach-o> [args...]\n"
        "  -t     trace every instruction\n"
        "  -n N   stop after N instructions (default 10000000)\n"
        "  -l     load and dump the image only, do not execute\n"
        "  -m     dump the guest address space map after loading\n"
        "  -j     compile basic blocks instead of interpreting them\n"
        "  -J     compile, and check every block against the interpreter\n");
}

int main(int argc, char **argv) {
    setvbuf(stdout, NULL, _IONBF, 0);   // keep host and guest writes interleaved

    bool trace = false, load_only = false, show_map = false;
    bool jit = false, jit_check = false;
    uint64_t budget = 10000000;
    int i = 1;

    for (; i < argc && argv[i][0] == '-' && argv[i][1]; i++) {
        if (!strcmp(argv[i], "-t")) trace = true;
        else if (!strcmp(argv[i], "-l")) load_only = true;
        else if (!strcmp(argv[i], "-m")) show_map = true;
        else if (!strcmp(argv[i], "-j")) jit = true;
        else if (!strcmp(argv[i], "-J")) { jit = true; jit_check = true; }
        else if (!strcmp(argv[i], "-n") && i + 1 < argc) budget = strtoull(argv[++i], NULL, 0);
        else { usage(); return 2; }
    }
    if (i >= argc) { usage(); return 2; }

    rsd_as as;
    if (rsd_as_init(&as) < 0) return 1;

    rsd_image img;
    if (rsd_load(&img, &as, argv[i]) < 0) return 1;
    if (rsd_fixups(&img, &as) < 0) return 1;
    rsd_objc_prepare(&img, &as);
    rsd_dump(&img);

    rsd_stubs stubs;
    rsd_stubs_of(&img, &stubs);
    if (show_map) { fprintf(stderr, "\n"); rsd_as_dump(&as); }

    if (!load_only) {
        int unresolved = 0;
        for (int k = 0; k < img.nimports; k++)
            if (!img.imports[k].native) unresolved++;
        if (unresolved)
            fprintf(stderr,
                "rashid: %d of %d import(s) could not be resolved; execution "
                "stops if one is called.\n", unresolved, img.nimports);
    }

    if (load_only) return 0;

    fprintf(stderr, "-- executing --\n");
    rsd_cpu cpu;
    if (rsd_cpu_init(&cpu, &as, img.entry, img.has_main, argc - i, argv + i) < 0)
        return 1;
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_crash;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    crash_cpu = &cpu;

    rsd_jit jitstate;
    if (jit) {
        if (rsd_jit_init(&jitstate) < 0) return 1;
        cpu.jit = &jitstate;
        crash_jit = &jitstate;
        cpu.jit_check = jit_check;
    }
    cpu.stubs = &stubs;
    cpu.trace = trace;
    rsd_cpu_run(&cpu, budget);

    if (cpu.fault) { rsd_cpu_dump(&cpu); rsd_cpu_free(&cpu); return 1; }

    if (jit) rsd_jit_report(&jitstate, cpu.icount);
    fprintf(stderr, "-- guest exited with status %d after %llu instructions --\n",
           cpu.exit_code, cpu.icount);
    rsd_cpu_free(&cpu);
    return cpu.exit_code;
}
