#!/bin/sh
# Where rashid stands. Compares the interpreter against the same binary run
# natively, which on a machine that still has Rosetta means against Rosetta.
exec python3 "$(dirname "$0")/bench.py" "$@"
