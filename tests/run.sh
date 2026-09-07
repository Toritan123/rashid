#!/bin/bash
# Differential test harness.
#
# Every guest is a freestanding x86_64 binary that computes something and
# exits with the answer. That answer is checked two ways: against a value
# recorded from real x86_64 hardware, and - while Rosetta still exists on
# this machine - against a live native run of the very same binary.
#
# The live check is the strong one, because it compares against the actual
# CPU rather than against our own belief about it. It also disappears with
# macOS 28, which is the whole reason this project exists; the recorded
# values are what survive.
set -u
BIN=${BIN:-./rashid}
fail=0
native_seen=0

run_case() {
    name=$1; want=$2
    "$BIN" "tests/$name.x86" >/dev/null 2>&1
    got=$?

    "tests/$name.x86" >/dev/null 2>&1
    nat=$?
    case $nat in
        126|127) nat="n/a" ;;
        *) native_seen=1 ;;
    esac

    status="ok"
    [ "$got" = "$want" ] || { status="FAIL (expected $want)"; fail=1; }
    if [ "$nat" != "n/a" ] && [ "$nat" != "$want" ]; then
        status="FAIL (hardware says $nat, recorded $want)"; fail=1
    fi
    printf '  %-8s rashid=%-4s native=%-4s %s\n' "$name" "$got" "$nat" "$status"
}

echo "== behaviour (exit status must match real x86_64) =="
run_case hello  0
run_case arith  55
run_case tls    15
run_case ripimm 255
run_case sse    21
run_case vm     9

echo
echo "== fault containment (a broken guest must not take down the translator) =="
for mode in null w f; do
    out=$("$BIN" tests/fault.x86 "$mode" 2>&1 | grep -m1 '^fault:')
    if [ -n "$out" ]; then
        printf '  %-8s %s\n' "$mode" "$out"
    else
        printf '  %-8s FAIL: no fault reported\n' "$mode"; fail=1
    fi
done

echo
echo "== native thunks (real binaries running through arm64 libSystem) =="
compare() {
    name=$1
    n_out=$("tests/$name.x86" 2>/dev/null); n=$?
    r_out=$("$BIN" "tests/$name.x86" 2>/dev/null); r=$?
    if [ "$n_out" = "$r_out" ] && [ "$n" = "$r" ]; then
        printf '  %-8s output and exit status match native (%s)\n' "$name" "$n"
    else
        printf '  %-8s FAIL: native=%s rashid=%s\n' "$name" "$n" "$r"
        diff <(printf '%s\n' "$n_out") <(printf '%s\n' "$r_out") | head -6
        fail=1
    fi
}
compare native
compare varargs
compare callback

echo
echo "== Objective-C (message sends into native arm64 Foundation) =="
compare objc

echo
echo "== dynamic loading (chained fixups walked, imports resolved) =="
out=$("$BIN" -l tests/import.x86 2>&1)
case $out in
    *"_printf"*"libSystem"*"thunk"*)
        printf '  %-8s _printf bound and resolved to a native thunk\n' import ;;
    *)  printf '  %-8s FAIL: _printf not reported as a resolved thunk\n' import
        printf '%s\n' "$out" | tail -4
        fail=1 ;;
esac

echo
[ "$native_seen" = 1 ] || echo "note: no native x86_64 execution available; recorded values only"
if [ "$fail" = 0 ]; then echo "all tests passed"; else echo "FAILURES"; fi
exit $fail
