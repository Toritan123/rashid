# Rashid

**Run Intel (x86_64) macOS applications on Apple Silicon after Rosetta is gone.**

Apple ships Rosetta 2 through macOS 27. From macOS 28 it is cut back to a
compatibility shim for a handful of old games, and the roughly 18,000
Intel-only Mac applications that still exist stop launching.

Rashid is a clean-room userspace translator built to keep them working. It is
not derived from Rosetta and does not require it, which macOS 27 briefly
turned from a claim into an observation.

> **Status: early, but it works — and it has been shown to work without
> Rosetta.** Upgrading the development machine to macOS 27 removed Rosetta,
> and for a while that machine refused to execute an x86_64 binary at all.
> Rashid ran one anyway:
>
> ```
> $ ./objcclass.x86
> bad CPU type in executable
>
> $ rashid objcclass.x86
> hits=5      describe=clicks=5   version=1.0
> isa=Counter responds=1          byname=Counter
> ```
>
> That is an application defining its own Objective-C class, its methods
> running in the interpreter and its messages going into the native arm64
> Foundation.
>
> macOS then reinstalled Rosetta on demand, as it still does through macOS 27,
> so the machine can execute x86_64 again — but the window was real and so was
> the result. Coverage is still far from complete — see [Roadmap](#roadmap).

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
./rashid -j  path/to/binary     # compile basic blocks
./rashid -J  path/to/binary     # compile, and check each block interpreted
```

## What works

- x86_64 Mach-O loading — thin and fat, `LC_MAIN` and `LC_UNIXTHREAD`
- A shared address space with per-region permission tracking, so a wild guest
  pointer is reported precisely instead of crashing the translator.
- An integer instruction subset: ALU with carry (`adc`/`sbb`), flags, `jcc`,
  `call`/`ret`, `mul`/`div`, `movzx`/`movsx`, `cmovcc`, `setcc`, `shld`/`shrd`,
  `pushfq`/`popfq`
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
- **Callbacks into guest code.** Native frameworks can call back into
  translated code — a comparator handed to `qsort`, a method implementation.
- **Classes the application defines itself**, with instance variables, class
  methods and calls to `super`.
- A few macOS syscalls, dispatched by class: `read`, `write`, `close`,
  `exit`, `thread_fast_set_cthread_self`

Not yet: AppKit and anything with a user interface, categories, protocols,
blocks, AVX, x87, signals, threads, file-backed guest `mmap`.

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

### Classes an application defines

The class is built through the public runtime API — allocate, add methods,
register — with every implementation replaced by a trampoline, since the
implementations are x86_64 code the runtime must not enter directly.
Instance variables are declared the same way, and the offsets the runtime
chooses are written back into the image, which is where compiled ivar
accesses read them from.

Then there is the question of identity. An image references its own classes
by address: clang emits a direct `leaq` to `_OBJC_CLASS_$_Foo`, with no
indirection to redirect. The obvious fix — copy the finished class object
over the image's own — **does not work, because libobjc is arm64e and signs
pointers with address diversity**. A class object moved to another address
is no longer valid, and the runtime says so.

So the image's structure is treated as a handle instead. Rashid remembers
which real class it stands for and substitutes the real one wherever the
guest passes it across the boundary, which is the only place its identity can
be observed.

```
hits=5          describe=clicks=5     version=1.0
isa=Counter     responds=1            byname=Counter
```

`RASHID_OBJC_DEBUG=1` prints what was found and registered.

### Calling back the other way

A thunk lets translated code call a framework. Applications need the reverse
as well: `qsort` calls a comparator, a framework calls a delegate, an
Objective-C runtime calls a method implementation. All of those arrive as
arm64 calls that have to end up in the x86_64 interpreter.

Rashid hands out addresses from a block of assembled trampolines. Each one
loads its own index and jumps to a shared dispatcher, which captures the
argument registers and the caller's stack, saves the interpreter's state,
runs the guest function, and puts the result back where AAPCS64 expects it.
The stubs are assembled rather than generated at runtime, so this needs no
`MAP_JIT` and no W^X handling.

Which argument of a native function is a callback cannot be discovered
without signatures, so the common ones — `qsort`, `bsearch`, `atexit` — are
listed, the same gap as with variadic functions.

### Crossing the ABI boundary### Classes an application defines

The class is built through the public runtime API — allocate, add methods,
register — with every implementation replaced by a trampoline, since the
implementations are x86_64 code the runtime must not enter directly.
Instance variables are declared the same way, and the offsets the runtime
chooses are written back into the image, which is where compiled ivar
accesses read them from.

Then there is the question of identity. An image references its own classes
by address: clang emits a direct `leaq` to `_OBJC_CLASS_$_Foo`, with no
indirection to redirect. The obvious fix — copy the finished class object
over the image's own — **does not work, because libobjc is arm64e and signs
pointers with address diversity**. A class object moved to another address
is no longer valid, and the runtime says so.

So the image's structure is treated as a handle instead. Rashid remembers
which real class it stands for and substitutes the real one wherever the
guest passes it across the boundary, which is the only place its identity can
be observed.

```
hits=5          describe=clicks=5     version=1.0
isa=Counter     responds=1            byname=Counter
```

`RASHID_OBJC_DEBUG=1` prints what was found and registered.

### Calling back the other way

A thunk lets translated code call a framework. Applications need the reverse
as well: `qsort` calls a comparator, a framework calls a delegate, an
Objective-C runtime calls a method implementation. All of those arrive as
arm64 calls that have to end up in the x86_64 interpreter.

Rashid hands out addresses from a block of assembled trampolines. Each one
loads its own index and jumps to a shared dispatcher, which captures the
argument registers and the caller's stack, saves the interpreter's state,
runs the guest function, and puts the result back where AAPCS64 expects it.
The stubs are assembled rather than generated at runtime, so this needs no
`MAP_JIT` and no W^X handling.

Which argument of a native function is a callback cannot be discovered
without signatures, so the common ones — `qsort`, `bsearch`, `atexit` — are
listed, the same gap as with variadic functions.

### Crossing the ABI boundary

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
the containing *section* holds instructions. The segment is not enough:
`__TEXT` is executable but carries read-only data too, and
`__objc_empty_cache` — which every class points at — lives in
`(__TEXT,__const)`.

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

Guests are x86_64 binaries that compute something and exit with the answer.
`make test` checks that answer against a value recorded from real x86_64
hardware and, where the machine can still execute x86_64, against a live
native run of the very same binary.

The fallback is no longer hypothetical. When the development machine upgraded
to macOS 27 and lost Rosetta, every comparison fell back to `tests/expected/`
and the suite stayed green. Rosetta came back and the live comparison agreed
with those recordings, which is as good a check of them as there will ever
be.

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
  callback output and exit status match native (0)

== Objective-C (message sends into native arm64 Foundation) ==
  objc      output and exit status match native (0)
  objcclass output and exit status match native (0)
```

The live comparison is the valuable one: it checks against the actual CPU
rather than against our belief about it. It is also the thing that goes away,
which is the reason this project exists — so record expected values while the
hardware can still be asked.

It has already earned its keep. `tests/ripimm.x86` covers a bug where a
RIP-relative displacement combined with an immediate operand resolved four
bytes early, because the displacement is measured from the end of the
*entire* instruction, immediates included. Every affected access landed on a
plausible nearby address instead of faulting.

## Speed

`make bench`. Measured on an M1 against the same binary run through Rosetta.

```
startup: native 11.8ms, rashid 6.1ms

work       guest instr    native    rashid   ratio  compute  M instr/s
int         42,000,052     17.3m    543.2m     31x      98x       78.2
mem         60,000,059     17.4m    972.6m     56x     171x       62.1
flt         78,000,070     39.4m   1093.4m     28x      39x       71.7
hash        90,001,360     24.6m   1396.6m     57x     109x       64.7
thunk       28,000,082     33.3m    452.4m     14x      21x       62.8
```

The interpreter runs at 60–80 million guest instructions per second, which
puts it 20–170x behind Rosetta on code the guest actually executes. That is
an ordinary number for a switch-dispatch interpreter, and closing it is what
the JIT is for; the spread is informative too. Floating point costs only 39x
because one guest instruction does more work, and thunk-heavy code only 21x
because rashid barely interprets — the work happens in native arm64
libSystem either way.

The iteration counts are large because Rosetta is fast: at a tenth of them
the native column was almost entirely process startup, and subtracting the
baseline from it left a compute ratio that moved between 100x and 500x from
run to run. That column is a difference of two measurements, so it needs the
measurements to be much larger than the thing subtracted.

Whole applications tell a different story, because most of what an
application does is not its own code:

```
app            guest instr    native    rashid   ratio
native                  70     10.7m      6.1m    0.6x
objc                   187     14.8m     10.8m    0.7x
objcclass              282     14.5m      9.4m    0.6x
```

Rashid starts in half the time, because it has nothing to translate: the
frameworks are already arm64. So there is a crossover, and it sits at a few
hundred thousand guest instructions:

```
  iterations   guest instr    native    rashid   ratio
           0            49     11.3m      6.5m    0.6x
      10,000        70,052     13.1m      7.8m    0.6x
     100,000       700,052     11.6m     16.1m    1.4x
   1,000,000     7,000,052     12.3m     97.1m    7.9x
```

Below that, rashid wins on startup alone. Above it, interpretation dominates
and Rosetta pulls away. Short-lived tools are already competitive; anything
that computes depends on the block compiler below, which these figures do
not use.

One negative result worth recording: `mem` is the worst case at 171x, which
looked like the per-access permission check scanning the region list. Caching
the last region that answered made it very slightly *slower*. The region list
is short and the scan is not where the time goes, so that idea is not the
one.

### The block compiler

`rashid -j` compiles basic blocks instead of interpreting them; `-J` does the
same and replays every block through the interpreter before believing it.

```
work      native   interp      jit  jit/interp  jit vs native
int        13.2m   190.4m    23.1m       8.22x            1.8x
mem        13.1m   334.2m    26.9m      12.41x            2.1x
flt        21.2m   376.3m    39.2m       9.60x            1.9x
hash       16.0m   476.6m    47.0m      10.15x            2.9x
thunk      16.3m   114.1m    46.7m       2.45x            2.9x
```

It compiles 8-, 32- and 64-bit integer operations with register, immediate
and memory operands (base, scaled index, displacement, RIP-relative), `lea`,
`inc`/`dec`, `push`/`pop`, shifts, `imul`, `movzx`/`movsx`, `setcc`, and ends
blocks at `jcc`, `jmp`, `call`, `ret` and their indirect forms. Guest state
stays in the `rsd_cpu` struct and compiled code calls back into the
interpreter's own routines for memory it has not seen yet and for the less
common flags, so the two engines share one definition of most of what an
instruction *means*; the exceptions are described below.

`hash` was added for the change that taught the compiler shifts, multiplies
and bytes: that is the shape of most real integer code, and the first
compiler could compile none of it. It ran at interpreter speed, 387ms.

`RASHID_JIT_PROFILE=1` reports how much of the run was compiled and lists, by
opcode, what the interpreter was still asked to do; that list is how the
order above was chosen. `int`, `mem`, `flt` and `hash` now compile 100% of
the instructions they execute, `thunk` 86% — the rest being the import stub,
which is the interpreter's job by design.

#### What eight-bit operands cost

Two x86 rules have nowhere to show themselves in C, so neither the compiler
nor the tests stumbled on them by accident:

- **A byte write merges.** Writing `al` leaves the other seven bytes of `rax`
  alone, where writing `eax` clears the upper four. Compiled code stores a
  byte with `strb`, not `str`.
- **`ah`, `ch`, `dh`, `bh` are not registers 4 to 7.** Without a REX prefix
  those numbers name bits 8..15 of `rax`, `rcx`, `rdx` and `rbx`; with any
  REX prefix — even an empty `0x40` — they name the low byte of `rsp`, `rbp`,
  `rsi` and `rdi`. The compiler has to remember that it saw a prefix with no
  bits set.

Reading a byte needs no masking at all, which is worth stating because it
looks like it should: addition, subtraction and the bitwise operations never
carry information downwards, and the flag helpers truncate what they are
given, so the low eight bits of a full-width operation are already the
answer. Only `ah`-style reads need extracting.

#### A shift was losing its carry

Adding shifts to the compiler meant giving the two engines one definition of
a shift, and writing it down found that the *interpreter* had been setting a
shift's flags with the same routine as `and` — clearing CF and OF every time.
On x86, CF is the last bit shifted out. Anything that shifts and then tests
the carry, which is how bignum arithmetic, `__builtin_*` carry chains and
plenty of hash functions are written, was reading a zero. Both engines now
compute CF properly, and OF as the hardware does. (The test masks off the
bits the manual leaves undefined — AF always, OF unless the count was one —
rather than pinning down behaviour that is not promised.)

A shift by zero changes nothing at all, not even a flag, and that case is
easy to get wrong in a way nothing notices: the one mutation that was missed
on the first run put the shift back in for a count of zero, and the test did
not see it because the flags were overwritten two instructions later. The
test now reads them.

Scalar SSE compiles too: moves, arithmetic, `min`/`max`, `sqrt`, all eight
comparison predicates, `comis`, bitwise operations and the conversions.
Before it did, `flt` ran *slower* compiled than interpreted (0.83x) — its
integer parts compiled but every SSE instruction ended a block, so the trips
in and out cost more than they saved. Now it is 9x faster than the
interpreter and within 3x of Rosetta. `-j` is still not the default.

#### What x86 floating point does that arm64 does not

A test built from NaNs, signed zeros, infinities and out-of-range values,
compared against native execution, found two places where the *interpreter*
was wrong — and the compiler, faithfully matching the interpreter, was wrong
the same way:

- **The sign of a generated NaN.** When an operation creates a NaN from
  ordinary inputs — `0/0`, `sqrt(-1)`, `inf-inf` — x86 SSE returns one with
  the sign bit set (`0xFFF8000000000000`); arm64 returns it with the sign bit
  clear. And when an input is already NaN, x86 returns the first operand's,
  quieted. Both engines now follow x86, through one shared routine; compiled
  code pays one compare and an untaken branch unless the result actually is
  NaN.
- **Conversions that do not fit.** `cvttsd2si` of NaN or of a value out of
  range gives x86's "integer indefinite", `0x80000000` or
  `0x8000000000000000`. A C cast on arm64 saturates instead, and turns NaN
  into 0, so `(int)3e9` had been coming out as the low 32 bits of
  3000000000.

One suspicion turned out wrong and is worth recording because it looked
right: single-precision division computed in double and then rounded to float
is rounded twice, which can differ from dividing in single precision. It
cannot, here. A quotient of two floats cannot land within 2⁻⁵³ of a
single-precision rounding boundary without landing exactly on it, and 200
million random pairs agreed.

#### Where the time went: flags

The plan had been to keep guest registers in host registers next. Measuring
first changed it. `sample` cannot walk a stack through compiled code, so the
measurement was done by removal instead: taking out the flag routine for the
two instructions in `int`'s loop whose flags are never read cut the run from
125ms to 76ms. Each call cost about 4ns, and the loop made four of them — the
flags were most of the time, and the loads and stores of guest registers in
memory were not.

Two changes followed, each measured on its own:

```
              before   flags only     flags
              (calls)   when read   in line
int           120.4m       89.1m     57.5m
mem           221.5m      119.5m    118.0m
hash          273.5m      166.6m    152.8m
```

(6 million iterations each, which is why these are larger than the table
above.)

- **Only the flags something reads.** A block is decoded completely before
  any of it is emitted, then walked backwards: for each instruction, which
  of the flags it writes does a later instruction read before something
  overwrites them? Usually none — an add is followed by a `cmp` — and then
  the flag code is skipped. Everything is live where a block ends, because
  the next block or the interpreter may read anything. The analysis has to
  know which flags an instruction is *certain* to write: `inc` and `dec`
  leave CF, logical operations here leave AF, and a shift by `cl` writes
  nothing when `cl` is zero, so it overwrites nothing.
- **The rest in line.** What is still needed is computed from arm64's own
  flags rather than through a call: the operation is repeated as `cmn`,
  `cmp` or `tst` at the right width, CF, ZF, SF and OF are read off NZCV
  (a borrow is arm64's carry *clear*), and PF and AF, which arm64 does not
  have, come from three shifted `eor`s and from `a ^ b ^ r`. Conditions for
  `jcc` and `setcc` are read straight from the flags word.

The second change gives up something the compiler had kept until now: one
definition of what an instruction means, shared with the interpreter. These
flags now have two. The interpreter's is still the reference, -J compares
them after every block, and eight new mutations — readers ignored by the
analysis, `inc` taken as writing CF, a shift by `cl` taken as writing flags,
a borrow taken as arm64's carry, parity inverted, the nibble carry from the
wrong bit, 32-bit flags taken at 64, `le` without ZF — check that it would
notice. Byte operations still go to the interpreter's routine, since arm64
has no 8-bit flag-setting forms.

One of the eight was reported missed on the first run, and the fault was in
the mutation, not the test: a shift by `cl` has no immediate, so the edited
condition still came out zero and nothing had changed. The script's check
that a mutation *applied* only compares the text.

#### Memory in line

`mem` did not move with the flag changes, because what it had left was a
call into the interpreter for every load and store, with the permission
check that decides whether the guest may touch that address. That check is
now made in line when it can be: the region the last compiled loads, and
the last compiled stores, succeeded in is remembered, and an access inside
one is a subtraction, one unsigned compare and the `ldr` or `str` itself.
Anything else — a new region, a region boundary, an address that is not
guest memory at all — still goes to the interpreter's routine, which makes
the full check, faults if it must, and remembers the region for next time.
Any `mmap`, `mprotect` or `munmap` forgets them all.

```
              flags     memory
              in line   in line
mem           104.6m     66.0m
hash          153.6m    129.1m
thunk         192.1m    169.2m
```

It remembers two regions of each kind, not one. With one, `thunk` became 8%
*slower*: its loop writes a byte to its buffer and then calls `strlen`,
which writes the return address to the stack, so the two stores alternated
between two regions, every one missed, and a miss now looked the region up
twice. `flt`, which makes no memory accesses in its loop, ran 2.6% slower
throughout; aligning blocks did not change that, so it is not where the
loop's code landed, and it is not yet explained. The next change below
takes the path it must be on out of the loop.

The edges are what the tests have to reach. `tests/jitmem.c` reads 8 bytes
that overrun a remembered region by one byte, once while that region is the
newer of the two and once while it is the older, and writes to a region
after making it read-only; each must fault exactly as the interpreter
would. The first version of that test checked nothing it claimed to: its
helper functions `pop rbp` before returning, which reads the stack, so the
stack had always become the newer region by the time the edge was read. The
sequences are now single `asm` statements.

Writing it also found a bug in the shift code that had been there since
shifts were compiled. A shift of a memory operand by `cl` kept the count in
`x5` across the call that loads the operand, and `x5` does not survive a
call. It worked because the interpreter's load routine happened not to use
`x5`.

And it found a gap in `-J`. A store wider than it should be changes bytes
that no log entry names; undoing the logged writes leaves them changed, the
interpreter then sees the same changed bytes, and the two engines agree. The
mutation that made 32-bit in-line stores write 8 bytes was missed for that
reason. Every logged write now also keeps the 16 bytes around it, before the
write and as each engine left them, and those are compared too:

```
rashid: jit and interpreter disagree at 0x1041ad000
  bytes around the write to 0x1041af000 differ:
    jit    c3c3c3c3a5a5a5a5210000002c000000
    interp c3c3c3c316000000210000002c000000
```

Even then the mutation was caught only after `tests/jitint.c` gained stores
from registers with something in their upper half into memory whose
neighbours are not zero; until then the extra bytes written had always been
the bytes already there.

#### What would move it further

Going from one block to the next without returning to the dispatcher, which
is what is left in `int` and `flt`; guest registers in host registers; 16-bit
operands and the `0x66` prefix; and the SSE long tail. Each is a separate
change, because each needs its own mutations.

#### Checking the checker

A verifier that never fails proves nothing, so `make jit-mutations` breaks
the compiler thirty-six ways on purpose — an index scale off by one,
RIP-relative addresses measured from the wrong place, an inverted branch, a
32-bit add done at 64 bits, a push that forgets to move `rsp`, a `cmp` that
writes its result, `min` returning the wrong operand on NaN, the `le`
predicate compiled as `lt`, a generated NaN left with arm64's sign, a 32-bit
integer converted as 64-bit, `andnot` with its operands swapped, `sqrt` of
the wrong register, a byte write that clears its register, `ah` read and
written as `al`, `movsx` widened as `movzx`, a shift count left unmasked, a
shift by zero done anyway, a shift taking its count from the wrong register,
a 32-bit multiply done at 64 bits, `setcc` inverted, the eight flag
mutations above, and six for in-line memory — either region bound taken as
inclusive, a region kept after `mprotect`, an in-line store not logged, a
32-bit in-line load or store made at 64 bits — and requires `-J` to catch
every one.

Getting there turned up these, each a way a passing test was not evidence:

- **Memory had to be part of the comparison.** Replaying a block that did
  `add [mem], 1` on memory the compiled block had already modified checks
  nothing. Writes are now logged, undone before the replay, and compared. The
  scale mutation is invisible without this: every register matched, and only
  the address written to differed.
- **The suite was not exercising the compiler.** With branches inverted,
  `tests/arith` still passed — none of its compiled blocks ended in a taken
  `jcc`. `tests/jit.c` exists for this, and was accepted only once it caught
  every mutation.
- **A result was stale.** HFS+ records modification times to the second, so
  an edit followed by `make` within that second leaves an old binary that
  `make` believes is current. A mutation reported as caught had in fact run
  an unmutated binary. The script now deletes the binary before every build.
- **The interpreter had a bug the compiler exposed.** The RIP-relative
  mutation did not produce a disagreement — it crashed rashid. A GOT entry
  read four bytes early gave a non-canonical pointer, and the bound check in
  `access_ok` computed `ceiling - address` before comparing the two, which
  wraps. Any wild pointer above the user range crashed the host instead of
  faulting. Fixed, with a fault test for it, and rashid now reports host
  crashes with the guest instruction that caused them.
- **XMM registers were not compared.** The replay compared general
  registers, flags and memory. The first version of the NaN fix passed its
  helper's arguments starting at `x1`, as every other helper wants — but that
  one takes no cpu, so it read a stray pointer as the result. Comparing XMM
  found it immediately and named the register.
- **16-byte stores bypassed the write log.** With `le` compiled as `lt`, the
  wrong mask went into memory through a `movapd` store, and the register
  holding it was overwritten before the block ended. Only 8-byte-and-smaller
  stores were logged, so nothing compared saw it. Every store goes through the
  log now.
- **The test's own checksum had a blind spot.** Mixing with
  `(h ^ v) * K` leaves a difference in the top bit in the top bit forever, and
  an even number of them cancel. That is exactly the shape of a NaN sign
  error in a double repeated eight times, and it hid one. The mix now shifts
  high bits down as well.

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
| M5 | basic-block JIT — integer, SSE, narrow operands, shifts, control flow | partial |
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
