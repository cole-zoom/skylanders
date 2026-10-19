#!/usr/bin/env python3
"""Find the first step where two CPU traces diverge.

The point of this tool is the word *first*. A boot sequence executes millions
of instructions; once two runs disagree, every subsequent line disagrees too,
and a plain `diff` reports all of them. The only useful piece of information
is the earliest divergence and the state around it -- everything after is
consequence, not cause.

Usage:
    difftrace.py <native.txt> <wasm.txt>

Exit status is 0 when the traces agree and 1 when they do not, so this can gate
a build.
"""
import sys

FIELDS = ["step", "pc", "gpr_hash", "cr", "msr", "lr", "ctr"]
CONTEXT = 6


def load(path):
    """Trace lines start with 'T '. Everything else is ordinary program output
    and is ignored, which is what lets both streams share stdout."""
    rows = []
    with open(path) as f:
        for line in f:
            if line.startswith("T "):
                parts = line.split()[1:]
                if len(parts) == len(FIELDS):
                    rows.append(parts)
    return rows


def describe(row):
    return "  ".join(f"{name}={value}" for name, value in zip(FIELDS, row))


def main(native_path, wasm_path):
    native, wasm = load(native_path), load(wasm_path)
    print(f"native: {len(native)} steps    wasm32: {len(wasm)} steps")

    if not native or not wasm:
        print("ERROR: one trace is empty -- did the program run?")
        return 1

    limit = min(len(native), len(wasm))
    for i in range(limit):
        if native[i] == wasm[i]:
            continue

        # Found it. Everything from here is about making the cause legible.
        print(f"\nFIRST DIVERGENCE at trace line {i} (step {native[i][0]})\n")
        lo = max(0, i - CONTEXT)
        for j in range(lo, i):
            print(f"  ok   {describe(native[j])}")
        print(f"  NAT  {describe(native[i])}")
        print(f"  WASM {describe(wasm[i])}")

        differing = [n for n, a, b in zip(FIELDS, native[i], wasm[i]) if a != b]
        print(f"\n  fields that differ: {', '.join(differing)}")
        if "pc" in differing:
            print("  pc differs -- control flow diverged, so a branch or an")
            print("  exception was resolved differently.")
        elif "gpr_hash" in differing:
            print(f"  only register contents differ. Re-run both with")
            print(f"  --dump-at={int(native[i][0], 16)} to see which register.")
        return 1

    if len(native) != len(wasm):
        print(f"\nTraces agree for {limit} steps, then differ in LENGTH.")
        longer = "native" if len(native) > len(wasm) else "wasm32"
        print(f"  {longer} kept executing -- the other stopped early, which")
        print("  usually means a trap rather than a wrong value.")
        return 1

    print(f"MATCH -- {len(native)} steps identical")
    return 0


if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    sys.exit(main(sys.argv[1], sys.argv[2]))
