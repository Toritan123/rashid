#!/bin/bash
# Is -J actually checking anything?
#
# A verifier that never fails proves nothing, so this breaks the block compiler
# on purpose, in ways a real compiler bug would, and requires -J to notice each
# time. Every mutation is built from scratch: the binary is deleted first,
# because HFS+ records modification times to the second, and an edit followed
# by make within that second leaves a stale binary that make believes is up to
# date. That exact thing produced a misleading result while this was written.
set -u
cd "$(dirname "$0")/.."
SRC=src/jit.c
GUEST=tests/jit.x86
[ -x "$GUEST" ] || { echo "build $GUEST first"; exit 1; }

orig=$(mktemp)
cp "$SRC" "$orig"
trap 'cp "$orig" "$SRC"; rm -f rashid; make -s >/dev/null 2>&1; rm -f "$orig"' EXIT

rebuild() { rm -f rashid; make -s >/dev/null 2>&1; }

check() {   # prints "agree", "disagree" or "crashed"
    local out st
    out=$(./rashid -J -n 0 "$GUEST" 2>&1 >/dev/null); st=$?
    if printf '%s' "$out" | grep -q "disagree"; then echo disagree
    elif [ $st -ge 128 ]; then echo crashed       # a signal took rashid down
    else echo agree
    fi
}

fail=0
rebuild
base=$(check)
printf '  %-34s %s\n' "unmodified compiler" "$base"
[ "$base" = agree ] || { echo "  the unmodified compiler must agree"; exit 1; }

mutate() {  # name, perl substitution
    local name=$1 expr=$2
    cp "$orig" "$SRC"
    perl -0777 -pi -e "$expr" "$SRC"
    if cmp -s "$orig" "$SRC"; then
        printf '  %-34s MUTATION DID NOT APPLY\n' "$name"; fail=1; return
    fi
    rebuild
    local r
    r=$(check)
    case $r in
        disagree) printf '  %-34s caught\n' "$name" ;;
        # Not silent, but not caught by -J either: rashid itself died, which
        # is its own bug to find.
        crashed)  printf '  %-34s CRASHED RASHID\n' "$name"; fail=1 ;;
        *)        printf '  %-34s MISSED\n' "$name"; fail=1 ;;
    esac
}

mutate "index scale off by one" \
  's{a64_add_shift\(A64_X1, A64_X1, A64_X6, o->scale\)}{a64_add_shift(A64_X1, A64_X1, A64_X6, o->scale ? o->scale - 1 : 0)}'
mutate "rip-relative measured from start" \
  's{put_imm\(b, A64_X1, o->next \+ \(uint64_t\)o->disp\)}{put_imm(b, A64_X1, o->next - 4 + (uint64_t)o->disp)}'
mutate "conditional branch inverted" \
  's{a64_csel\(A64_X0, A64_X2, A64_X3, A64_NE\)}{a64_csel(A64_X0, A64_X2, A64_X3, A64_EQ)}'
mutate "32-bit add done at 64 bits" \
  's{w \? a64_add_w\(A64_X3, A64_X1, A64_X2\) : a64_add}{w ? a64_add(A64_X3, A64_X1, A64_X2) : a64_add}'
mutate "push forgets to move rsp" \
  's{put\(b, a64_sub_imm\(A64_X1, A64_X1, 8\)\);\n        put\(b, a64_str\(A64_X1, CPU, off_r\(4\)\)\);\n        put\(b, a64_ldr\(A64_X2}{put(b, a64_ldr(A64_X2}s'
mutate "cmp writes its result back" \
  's{if \(o->no_write\) return 0;}{}; s{o->no_write = \(o->op == 0x38 \|\| o->op == 0x85\);}{o->no_write = (o->op == 0x85);}'

cp "$orig" "$SRC"
rebuild
after=$(check)
printf '  %-34s %s\n' "restored compiler" "$after"
[ "$after" = agree ] || fail=1

[ $fail = 0 ] && echo "every mutation was caught" || echo "FAILURES"
exit $fail
