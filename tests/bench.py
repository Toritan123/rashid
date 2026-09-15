#!/usr/bin/env python3
"""Measure the interpreter against native execution of the same binary.

On a machine that still has Rosetta, "native" is Rosetta. Where it does not,
there is nothing to compare against and the ratios are skipped.
"""
import os, re, subprocess, sys, time

BIN = os.environ.get("BIN", "./rashid")
B = "tests/bench.x86"


def best(cmd, reps=4):
    for _ in range(2):
        subprocess.run(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    times = []
    for _ in range(reps):
        t0 = time.perf_counter()
        subprocess.run(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        times.append(time.perf_counter() - t0)
    return min(times)


def guest_instructions(cmd):
    p = subprocess.run([BIN, "-n", "0"] + cmd, stdout=subprocess.DEVNULL,
                       stderr=subprocess.PIPE, text=True)
    m = re.search(r"after (\d+) instructions", p.stderr)
    return int(m.group(1)) if m else 0


def main():
    if not os.access(B, os.X_OK):
        print("build tests/bench.x86 first")
        return 1
    live = subprocess.run([B, "int", "0"], stdout=subprocess.DEVNULL,
                          stderr=subprocess.DEVNULL).returncode not in (126, 127)
    if not live:
        print("this machine cannot execute x86_64; nothing to compare against")

    # Both columns include process startup, which is most of a short run.
    # The crossover table below shows that baseline; subtract it for the
    # interpreter's own cost, which is closer to 50-100x.
    base_n = best([B, "int", "0"]) if live else 0
    base_x = best([BIN, "-n", "0", B, "int", "0"])
    print(f"startup: native {base_n*1000:.1f}ms, rashid {base_x*1000:.1f}ms "
          f"(included in both columns below)\n")
    print(f"{'work':<8}{'guest instr':>14}{'native':>10}{'rashid':>10}"
          f"{'ratio':>8}{'compute':>9}{'M instr/s':>11}")
    for w, n in (("int", 2_000_000), ("mem", 2_000_000),
                 ("flt", 2_000_000), ("thunk", 500_000)):
        x = best([BIN, "-n", "0", B, w, str(n)])
        ic = guest_instructions([B, w, str(n)])
        r = best([B, w, str(n)]) if live else 0
        ratio = f"{x / r:.0f}x" if r else "-"
        cn, cx = r - base_n, x - base_x
        compute = f"{cx / cn:.0f}x" if cn > 0.0005 else "-"
        print(f"{w:<8}{ic:>14,}{r * 1000:>9.1f}m{x * 1000:>9.1f}m{ratio:>8}"
              f"{compute:>9}{ic / cx / 1e6:>11.1f}")

    print("\nwhole applications, where most of the time is in native frameworks:")
    print(f"{'app':<12}{'guest instr':>14}{'native':>10}{'rashid':>10}{'ratio':>8}")
    for t in ("native", "varargs", "callback", "objc", "objcclass"):
        p = f"tests/{t}.x86"
        if not os.access(p, os.X_OK):
            continue
        x = best([BIN, "-n", "0", p])
        ic = guest_instructions([p])
        r = best([p]) if live else 0
        ratio = f"{x / r:.1f}x" if r else "-"
        print(f"{t:<12}{ic:>14,}{r * 1000:>9.1f}m{x * 1000:>9.1f}m{ratio:>8}")

    if live:
        print("\nthe crossover: rashid starts faster and interprets slower")
        print(f"{'iterations':>12}{'guest instr':>14}{'native':>10}{'rashid':>10}{'ratio':>8}")
        for n in (0, 10_000, 100_000, 1_000_000):
            r = best([B, "int", str(n)])
            x = best([BIN, "-n", "0", B, "int", str(n)])
            ic = guest_instructions([B, "int", str(n)])
            print(f"{n:>12,}{ic:>14,}{r * 1000:>9.1f}m{x * 1000:>9.1f}m{x / r:>7.1f}x")
    return 0


if __name__ == "__main__":
    sys.exit(main())
