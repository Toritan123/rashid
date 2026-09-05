# Rashid

**Run Intel (x86_64) macOS applications on Apple Silicon after Rosetta is gone.**

Apple ships Rosetta 2 through macOS 27. From macOS 28 it is cut back to a
compatibility shim for a handful of old games, and the roughly 18,000
Intel-only Mac applications that still exist stop launching.

Rashid is a clean-room userspace translator built to keep them working. It is
not derived from Rosetta and does not require it.

> **Status: early.** Rashid runs freestanding x86_64 Mach-O executables today.
> It cannot yet run applications that link against system libraries — that is
> milestone M4. See [Roadmap](#roadmap).

[日本語版 README](README.ja.md)

## Why this is more tractable than it sounds

Wine has to reimplement Win32, because Windows is not there. Rashid does not
have that problem: **the frameworks are already on the machine, as arm64.**
AppKit, Metal, Foundation, JavaScriptCore — all native, all present.

So Rashid does not emulate the system. It translates the application's own
x86_64 code and, at every call into a system library, converts the calling
convention and jumps into the native arm64 implementation.

```
   x86_64 application code
        │  ← only this is translated
   ┌────┴──────────────────────────────┐
   │ translator (interpreter, then JIT)│
   └────┬──────────────────────────────┘
        │  calls into a dylib
   ┌────┴──────────────────────────────┐
   │ thunk layer (System V → AAPCS64)  │
   └────┬──────────────────────────────┘
        │
   native arm64 AppKit / Metal / libSystem   ← never translated
```

This is the model box64 uses on Linux and the one Wine's WoW64 uses for
32-bit Windows code. What makes it practical on macOS specifically is that
**the Objective-C runtime carries its own type information**: nearly all of
Cocoa goes through `objc_msgSend`, and every method's signature is recorded
as a type encoding. Thunks for the Cocoa surface can be generated rather than
written.

Measured against the real frameworks:

| | classes | methods | with type encoding | C functions |
|---|---|---|---|---|
| AppKit | 2,573 | 44,061 | 44,051 (99.98%) | 7,207 |
| six core frameworks | 4,243 | 65,693 | 65,678 (99.98%) | 32,444 |

The Objective-C side is essentially fully generatable. Hand-written work is
confined to C entry points, and real applications touch only a fraction of
those.

## Try it

```sh
make
make test
```

```sh
./rashid tests/hello.x86        # run a freestanding x86_64 guest
./rashid -l  path/to/binary     # inspect the Mach-O, list thunk targets
./rashid -m  path/to/binary     # dump the guest address space map
./rashid -t  path/to/binary     # trace every instruction
```

## What works

- x86_64 Mach-O loading — thin and fat, `LC_MAIN` and `LC_UNIXTHREAD`
- An isolated guest address space. The guest gets the addresses it was linked
  for, and a wild guest pointer is reported as a fault instead of corrupting
  the translator.
- An integer instruction subset: ALU and flags, `jcc`, `call`/`ret`,
  `mul`/`div`, `movzx`/`movsx`, `cmovcc`, `setcc`, `shld`/`shrd`
- **Thread-local storage.** macOS x86_64 keeps a thread's TSD block at `%gs`
  and installs it with the machine-dependent syscall `0x3000003`;
  `pthread_getspecific` is then one instruction, `movq %gs:(,%rdi,8), %rax`.
  (`%fs` is never used on macOS — zero occurrences across all of libsystem.)
- A few macOS syscalls, dispatched by class: `read`, `write`, `close`,
  `exit`, `thread_fast_set_cthread_self`

Not yet: anything that imports a dylib (so: every real application), SSE/AVX,
x87, signals, threads, guest `mmap`.

### Guest address space

Guest addresses are offsets into one reservation, not host addresses.

```
host space                        guest space (8 GiB reserved)
  0x100000000  rashid's own text    0x100000000  the guest's text
  0xa58000000  the reservation ───→ 0x000000000  guest zero
               (ASLR'd; unobservable from inside the guest)
```

Both can use `0x100000000`, which is what x86_64 images want and also exactly
where an arm64 executable is placed. This is why non-PIE images work. Every
access is bounds- and permission-checked; in the JIT the base becomes a
reserved register, as in FEX and box64.

## Testing

Guests are freestanding x86_64 binaries that compute something and exit with
the answer. `make test` checks that answer twice: against a value recorded
from real x86_64 hardware, and — while Rosetta still exists on this machine —
against a live native run of the very same binary.

```
== behaviour (exit status must match real x86_64) ==
  hello    rashid=0    native=0    ok
  arith    rashid=55   native=55   ok
  tls      rashid=15   native=15   ok
  ripimm   rashid=255  native=255  ok
```

The live comparison is the valuable one: it checks against the actual CPU
rather than against our belief about it. It also disappears in macOS 28,
which is the reason this project exists — so record expected values now.

It has already earned its keep. `tests/ripimm.x86` covers a bug where a
RIP-relative displacement combined with an immediate operand resolved four
bytes early, because the displacement is measured from the end of the
*entire* instruction, immediates included. Every affected access landed on a
plausible nearby address instead of faulting.

## Roadmap

| | | |
|---|---|---|
| M0 | Mach-O loader + integer interpreter | done |
| T0 | shared-cache reader (`tools/dsc.py`) | done |
| M1a | guest address space separation | done |
| M1b | TLS (`%gs`) — done; guest `mmap` and signals remain | partial |
| M2 | SSE2 and x87 | |
| M3 | dyld equivalent: dependency resolution, chained fixups, stub thunking | |
| **M4** | **generated `objc_msgSend` thunks — first Cocoa application launches** | |
| M5 | basic-block JIT (`MAP_JIT` + `pthread_jit_write_protect_np`) | |
| M6 | AOT cache, trace JIT | |

**M4 is the goal line.** M5 and M6 make it fast; they do not change what runs.

## Clean-room

Rashid contains no Apple code and is not derived from Rosetta's
implementation. The decoder comes from the Intel SDM, the loader from
`<mach-o/loader.h>`, the ABI conversion from the published System V and
AAPCS64 documents.

Reading Apple's *data* — parsing the dyld shared cache, extracting ObjC type
encodings — is interoperability work and is how the thunk tables are
generated. Reading Apple's *code* is not. Contributors who have disassembled
Rosetta should not work on the translation core. See
[CONTRIBUTING.md](CONTRIBUTING.md).

For the record, Rosetta cannot be reused even if one wanted to: its entry
point is a kernel exec path, its daemon requires Apple-private entitlements
that AMFI validates, its files live under SIP, and `libRosettaRuntime` is an
`MH_EXECUTE` image exporting two symbols, so it cannot be loaded as a library.

## Tools

`tools/dsc.py` reads the x86_64 dyld shared cache directly. Apple's
`dsc_extractor` can split the cache into individual Mach-O files but cannot
recover selector names, because in a modern cache a method's selector is an
offset into one cache-wide pool that no single dylib contains.

```sh
./tools/dsc.py objc    <cache> AppKit    # classes, methods, type encodings
./tools/dsc.py symbols <cache> libobjc   # exported symbols
./tools/dsc.py plan    <cache>           # thunk workload
```

One finding worth recording: **type encodings are not identical between
x86_64 and arm64.** `BOOL` is `signed char` (`c`) on x86_64 and `bool` (`B`)
on arm64, so `isFromConnection:` is `c24@0:8@16` in the Intel frameworks and
`B24@0:8@16` in the ARM ones. Thunks must be generated from x86_64 metadata;
the live arm64 runtime is not a substitute.

## Related work

- [FEX-Emu](https://github.com/FEX-Emu/FEX) — the reference for x86→AArch64
  JIT quality. Linux ELF only, and it expects a 4K-page kernel.
- [box64](https://box86.org/2021/08/a-deep-dive-into-library-wrapping/) — the
  same native-library-wrapping model, on Linux.
- [Darling](https://github.com/darlinghq/darling) — macOS binaries on Linux.
  Instructive as a contrast: Darling has to reimplement AppKit, which is why
  after many years most GUI applications still do not run. Rashid does not.
- [rozetka2](https://github.com/XS-Corp/rozetka2) — the closest sibling: also
  clean-room, also x86_64→arm64 for Apple Silicon, MIT. MIT code can be
  brought into Rashid directly; going the other way means carrying the
  Apache-2.0 terms along with it.

## License

Apache-2.0. See [LICENSE](LICENSE) and [NOTICE](NOTICE).

Permissive, with an explicit patent grant — which matters in binary
translation, where the patent landscape is crowded.

"Rosetta", "macOS" and "Apple Silicon" are trademarks of Apple Inc. Rashid is
not affiliated with or endorsed by Apple Inc.
