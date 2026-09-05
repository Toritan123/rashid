# Contributing to Rashid

## The one rule that matters: stay clean-room

Rashid exists so that Intel macOS applications keep working after Apple
removes Rosetta. That only helps anyone if Rashid can actually be
distributed, which means it must not contain, embed, or derive from Apple's
code.

**Allowed** — this is ordinary interoperability work:

- Implementing from published specifications: the Intel SDM, the ARM ARM,
  `<mach-o/loader.h>`, the System V and AAPCS64 ABI documents.
- Observing *data*: parsing the dyld shared cache, reading ObjC type
  encodings, dumping Mach-O structures, comparing your own translator's
  output against what a program is supposed to compute.
- Running Apple's tools on your own machine to understand behaviour.
- Testing against Rosetta while it still exists, to check that a program
  produces the same result.

**Not allowed** — these would make Rashid undistributable:

- Disassembling `libRosettaRuntime`, `runtime`, or `translate_tool` and
  porting their algorithms, structure, or constants into Rashid.
- Committing any Apple binary: shared caches, AOT caches, extracted system
  dylibs, SDKs, framework binaries, or fragments of them.
- Copying code from any source whose license does not permit it.

If you have disassembled Rosetta, please do not contribute to the
translation core. Work on tooling, tests, or documentation instead. This is
not a judgement — it is how clean-room provenance is kept defensible.

By opening a pull request you confirm that your contribution is your own
work or is compatibly licensed, that it is not derived from Apple's
implementation, and that you agree to it being distributed under
the Apache License, Version 2.0.

New source files should carry the standard header; copy it from any existing
file in `src/`.

## Practical notes

- Build and test: `make && make test`
- The test suite must stay green, including the fault-containment cases: a
  misbehaving guest must never take down the translator.
- New instructions need a test that exercises them. `tests/arith.c` is the
  pattern: compute something whose answer is known, exit with it.
- Keep the interpreter authoritative. When the JIT lands, it is validated
  against the interpreter, not the other way round.

## Where help is most useful right now

See the roadmap in the README. The near-term gaps are TLS (`%fs` segment
handling), SSE2, and the dyld-equivalent loader — in that order, because
nothing that links against libSystem runs without them.
