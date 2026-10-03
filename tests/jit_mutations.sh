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
# A mutation counts as caught if -J objects on any of these.
GUESTS=("tests/jit.x86" "tests/jitsse.x86" "tests/jitint.x86" "tests/bench.x86 flt 2000")
for g in "${GUESTS[@]}"; do
    f=${g%% *}
    [ -x "$f" ] || { echo "build $f first"; exit 1; }
done

orig=$(mktemp)
cp "$SRC" "$orig"
trap 'cp "$orig" "$SRC"; rm -f rashid; make -s >/dev/null 2>&1; rm -f "$orig"' EXIT

rebuild() { rm -f rashid; make -s >/dev/null 2>&1; }

check() {   # prints "agree", "disagree" or "crashed"
    local out g crashed=no
    for g in "${GUESTS[@]}"; do
        # shellcheck disable=SC2086
        out=$(./rashid -J -n 0 $g 2>&1 >/dev/null)
        if printf '%s' "$out" | grep -q "disagree"; then echo disagree; return; fi
        # Judged by the crash reporter, not the exit status: a guest may
        # legitimately exit with 255.
        printf '%s' "$out" | grep -q "rashid: host SIG" && crashed=yes
    done
    [ $crashed = yes ] && echo crashed || echo agree
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

# SSE
mutate "min returns first operand on NaN" \
  's{a64_fcsel_d\(D0, D0, D1, A64_MI\)}{a64_fcsel_d(D0, D0, D1, 11)}'
mutate "cmp predicate le compiled as lt" \
  's{    A64_LS,  // 2 le}{    A64_MI,  // 2 le}'
mutate "generated NaN keeps arm64 sign" \
  's{b->out\[skip\] = a64_bcond\(A64_VC, b->n - skip\)}{b->out[skip] = a64_bcond(14, b->n - skip)}'
mutate "32-bit int converted as 64-bit" \
  's{o->isize == 8 \? a64_scvtf_dx\(D0, A64_X1\) : a64_scvtf_dw\(D0, A64_X1\)}{a64_scvtf_dx(D0, A64_X1)}'
mutate "andnot operands swapped" \
  's{a64_bic\(A64_X1, A64_X4, A64_X1\)}{a64_bic(A64_X1, A64_X1, A64_X4)}'
mutate "sqrt of destination, not source" \
  's{dbl \? a64_fsqrt_d\(D0, D1\) : a64_fsqrt_s\(D0, D1\)}{dbl ? a64_fsqrt_d(D0, D2) : a64_fsqrt_s(D0, D2)}'

# narrow operands, shifts, multiplies, setcc
mutate "byte write clears the register" \
  's{put\(b, a64_strb\(src, CPU, off_r\(reg\)\)\);}{put(b, a64_str(src, CPU, off_r(reg)));}'
mutate "ah read as al" \
  's{put\(b, a64_ubfx\(dst, dst, 8, 8\)\);}{}'
mutate "ah written as al" \
  's{put\(b, a64_bfi\(A64_X9, src, 8, 8\)\);}{put(b, a64_bfi(A64_X9, src, 0, 8));}'
mutate "movsx widened as movzx" \
  's{if \(!o->sign\)}{if (1)}'
mutate "movzx from a byte takes a word" \
  's{o->ssize = \(op2 & 1\) \? 2 : 1;}{o->ssize = 2;}'
mutate "shift count not masked" \
  's{int mask = \(o->size == 8\) \? 63 : 31;}{int mask = 63;}'
mutate "shift by zero shifts anyway" \
  's{if \(!o->by_cl && !\(o->imm & \(uint64_t\)mask\)\) return;}{}'
mutate "shift by cl takes rdx" \
  's{put\(b, a64_ldr\(A64_X5, CPU, off_r\(1\)\)\);}{put(b, a64_ldr(A64_X5, CPU, off_r(2)));}'
mutate "32-bit multiply done at 64 bits" \
  's{o->size == 8 \? a64_mul\(A64_X3, A64_X1, A64_X2\)\n                            : a64_mul_w\(A64_X3, A64_X1, A64_X2\)}{a64_mul(A64_X3, A64_X1, A64_X2)}s'
mutate "setcc inverted" \
  's{o->cc = op2 & 0xF;}{o->cc = (op2 & 0xF) ^ 1;}'

cp "$orig" "$SRC"
rebuild
after=$(check)
printf '  %-34s %s\n' "restored compiler" "$after"
[ "$after" = agree ] || fail=1

[ $fail = 0 ] && echo "every mutation was caught" || echo "FAILURES"
exit $fail
