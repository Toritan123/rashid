# Rashid

**Run Intel (x86_64) macOS applications on Apple Silicon after Rosetta is gone.**

Apple ships Rosetta 2 through macOS 27. From macOS 28 it is cut back to a
compatibility shim for a handful of old games, and the roughly 18,000
Intel-only Mac applications that still exist stop launching.

Rashid is a clean-room userspace translator built to keep them working. It is
not derived from Rosetta and does not require it.

> **Status: early, but it works.** Rashid runs real x86_64 Mach-O
> executables, including Objective-C ones, sending messages into the native
> arm64 Foundation on the machine. Coverage is far from complete — see
> [Roadmap](#roadmap).

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
- A shared address space with per-region permission tracking, so a wild guest
  pointer is reported precisely instead of crashing the translator.
- An integer instruction subset: ALU with carry (`adc`/`sbb`), flags, `jcc`,
  `call`/`ret`, `mul`/`div`, `movzx`/`movsx`, `cmovcc`, `setcc`, `shld`/`shrd`
- **Thread-local storage.** macOS x86_64 keeps a thread's TSD block at `%gs`
  and installs it with the machine-dependent syscall `0x3000003`;
  `pthread_getspecific` is then one instruction, `movq %gs:(,%rdi,8), %rax`.
  (`%fs` is never used on macOS — zero occurrences across all of libsystem.)
- **SSE**, covering 99.3% of the SIMD instructions compiler-generated x86_64
  code actually executes (see below): 16-byte and scalar moves, bitwise ops,
  scalar and packed float arithmetic, comparisons, conversions.
- Guest memory management: `mmap` (anonymous), `mprotect`, `munmap`
- **Dynamic loading.** `LC_DYLD_CHAINED_FIXUPS` is walked, the image rebased,
  and every import resolved against the native arm64 frameworks already
  loaded in the process.
- **Thunks for C functions.** Translated code calls into native arm64
  libSystem and gets correct answers back, including variadic calls.
- **Objective-C.** Selectors are registered with the native runtime, classes
  and constant strings bind to the real objects, and message sends run in the
  arm64 Foundation already on the machine.
- A few macOS syscalls, dispatched by class: `read`, `write`, `close`,
  `exit`, `thread_fast_set_cthread_self`

Not yet: AppKit and anything with a user interface, classes defined by the
application itself, AVX, x87, signals, threads, file-backed guest `mmap`.

An Objective-C binary runs to completion with output identical to the same
binary run natively:

```
len=23              range=6,9           double=2.50
prefix=1            count=3 first=x     dict=v
format=built/7      NSLog reached with hello objective-c world (23)
```

### Objective-C needed less than expected, and more

`objc_msgSend` needs no special handling at all. It looks variadic, but it
is not: the compiler knows each call site's real signature and emits an
ordinary call, so System V's and AAPCS64's register assignments line up and
the generic thunk carries it unchanged.

What does need doing is what dyld would have told the runtime. Selector
references in an image start out pointing at strings inside that image, and
`objc_msgSend` compares selectors by pointer rather than by text, so every
one is re-registered with the native runtime at load time. Class references
and constant strings are already handled by binding data symbols directly.

Three things did need real work, and each was found by running something and
watching it be wrong:

- A 16-byte struct comes back in `rax:rdx` on one side and `x0:x1` on the
  other. Capturing only `x0` made `[s rangeOfString:]` return a plausible
  location and a garbage length.
- `NSLog` is variadic with an *NSString* format, not a C string. Reading it
  means asking the object for its bytes — an ordinary message send, since the
  object is native.
- A few Objective-C methods take an ellipsis, `stringWithFormat:` among them.
  A method's type encoding describes only its declared arguments, so
  variadicity cannot be discovered from metadata; the selector is looked up
  in a list instead.

### Crossing the ABI boundary### Crossing the ABI boundary

The two integer conventions line up almost exactly — System V's `rdi, rsi,
rdx, rcx, r8, r9` become AAPCS64's `x0..x5`, and floating-point arguments sit
in `xmm0-7` on one side and `v0-v7` on the other. A small assembly call gate
enters the native function with a chosen register and stack state and
captures both `x0` and `d0`, since which return register matters depends on a
signature rashid may not know.

Whether an imported symbol is code or data decides how it is bound. A
function has to go through a stub, because binding it directly would leave
the guest trying to execute arm64 instructions as x86. A data symbol — an
Objective-C class object, a CoreFoundation constant — is never called, only
dereferenced, so it binds straight to the native address. Rather than guess
from the name, rashid looks the symbol up in its own image and asks whether
the containing segment is executable.

Variadic calls need real work. **macOS arm64 passes every variadic argument
on the stack**, while System V passes the first few in registers, so
forwarding one means knowing how many arguments there are and what they are.
For the formatted-IO family that comes from the format string, which rashid
parses to rebuild the call. Other variadic functions are a known gap: without
signatures they would be handed their arguments in registers and misbehave.

### How much is left for the first application### How much is left for the first application

A Cocoa "hello world" — `NSString`, `NSLog`, an autorelease pool — imports
eight symbols:

```
_NSLog                                Foundation
_OBJC_CLASS_$_NSString                Foundation
_objc_autoreleasePoolPop              libobjc
_objc_autoreleasePoolPush             libobjc
_objc_msgSend                         libobjc
_objc_release                         libobjc
_objc_retainAutoreleasedReturnValue   libobjc
___CFConstantStringClassReference     CoreFoundation
```

That is the real size of the first milestone. The 32,444 C entry points
counted earlier are the eventual surface, not the entry price: an application
touches a small, and largely shared, subset of it. Six of these eight are
Objective-C runtime entry points, and everything reached *through*
`objc_msgSend` is covered by generated thunks rather than hand-written ones.

### Address space

**The guest shares rashid's address space: a guest pointer is a host pointer.**

This is forced rather than chosen. When translated code calls into a native
arm64 framework it hands over pointers, and native code stores pointers into
memory the guest later reads — into structs, into ObjC objects, into buffers
passed to callbacks. Converting at the boundary would mean chasing whole
pointer graphs, so both sides have to agree on what an address means. Wine
and box64 share an address space for the same reason.

The consequence is that an image cannot always get the address it was linked
for. x86_64 images want `0x100000000`, which is exactly where an arm64
executable — rashid itself — is placed, and the linker will not move it:
`-no_pie` and `-image_base` are ignored on arm64, and `-pagezero_size` caps
at 4 GB. So PIE images are slid, and a non-PIE image is refused with a clear
message rather than silently misplaced. Every x86_64 macOS application built
since roughly 2011 is PIE.

What rashid tracks is the set of regions it has handed to the guest, with
their permissions. Every access is checked against that list, which turns a
wild guest pointer into a precise report instead of a crash somewhere later.
It is a debugging aid, not a sandbox, and it will have to relax once thunks
exist — at that point the guest legitimately reaches native memory.

Permissions are recorded per region and enforced in software rather than with
`mprotect`. Apple Silicon uses 16K host pages while x86_64 images are laid
out on 4K boundaries, so a single host page routinely spans segments with
different permissions; rounding a protection outward silently makes the
neighbouring segment unwritable. The JIT will need a real answer here, since
it cannot afford a check on every access.

### Which SSE instructions matter

Chosen by measurement rather than by reading the manual front to back.
Counting what a framework's compiled code actually contains — Foundation,
3.2M instructions, as a stand-in for application code:

| | count | |
|---|---|---|
| `movups` / `movaps` | 63,923 | struct copies and spilling xmm |
| `movsd` / `movapd` / `movupd` | 9,614 | moving doubles |
| `xorps` / `xorpd` | 5,107 | zeroing |
| `ucomisd` `addsd` `mulsd` `subsd` | 2,432 | scalar double arithmetic |

Almost all of it is 16-byte moves and scalar double math; packed integer SSE
barely appears. That is a very different profile from hand-written assembly
in libSystem, where `memcpy` and `strlen` dominate — and libSystem is exactly
what Rashid does *not* translate.

XMM support is unavoidable regardless: the x86_64 ABI passes floating-point
arguments in xmm0-7 and returns in xmm0, so even calling a native function
that takes a `double` goes through it.

Of the 86,615 SIMD instructions in Foundation, 86,013 are implemented. The
remaining 0.7% is a long tail of 57 SSE3/SSSE3/SSE4.1 opcodes — `pinsrb`,
`blendvpd`, `movddup`, `punpcklqdq` and friends. Hitting one stops the guest
with the opcode and its mandatory prefix printed, since for `0F` opcodes the
prefix is what selects the instruction.

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
  sse      rashid=21   native=21   ok
  vm       rashid=9    native=9    ok

== native thunks (real binaries running through arm64 libSystem) ==
  native   output and exit status match native (14)
  varargs  output and exit status match native (0)

== Objective-C (message sends into native arm64 Foundation) ==
  objc     output and exit status match native (0)
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
| M1a | address space and permission tracking | done |
| M1b | TLS (`%gs`), guest `mmap` — done; signals remain | partial |
| M2 | SSE — done; x87 remains | partial |
| M3 | chained fixups, binding, C thunks — done | done |
| **M4** | **Objective-C: selectors registered, message sends into native Foundation** | **done** |
| M5 | basic-block JIT (`MAP_JIT` + `pthread_jit_write_protect_np`) | |
| M6 | AOT cache, trace JIT | |

**M4 was the goal line, and a Foundation application now runs.** What remains
is coverage — AppKit, application-defined classes, the long tail of the
instruction set — and then speed. M5 and M6 make it fast; they do not change
what runs.

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
