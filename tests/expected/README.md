# Recorded output

These files are what real x86_64 hardware produced, captured while Rosetta
still existed on the development machine. Each was verified against a live
native run at the time it was recorded — see the commits that introduced the
corresponding test.

They exist because the oracle is temporary. Rosetta was removed by the
upgrade to macOS 27 (re-installable there, but gone for good in 28), and once
it is unavailable there is no way to ask the hardware again. `tests/run.sh`
prefers a live native run whenever one is possible and falls back to these.

Do not regenerate a file to make a failing test pass. A difference means
either rashid changed behaviour or the recording was wrong, and both are
worth finding out about.
