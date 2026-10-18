# Spike A — PowerPC → WebAssembly throughput

**Status: INCONCLUSIVE. Does not yet gate Phase 0.**

## Question

Can recompiled Broadway code sustain ≥ 600 MIPS in a browser? (Gate from
`docs/PLAN.md` — Broadway is 729 MHz at close to 1 IPC on game code, so ~600 MIPS
is roughly 100% speed with headroom.)

## What was measured

A vertex-transform-shaped kernel — 18 guest instructions per iteration: 4 loads,
6 paired-single float ops, 2 stores, 4 integer ops, a compare and a branch —
written once in Rust and built twice, native `aarch64` and `wasm32` run under
Node's V8 (same engine as Chrome).

Three variants, 4 trials each, 30M iterations:

| Variant | Native (MIPS) | WASM (MIPS) | WASM/native |
|---|---|---|---|
| `naive` — eager CR, bounds-checked loads | 785–871 | 645–774 | ~0.90 |
| `tuned` — lazy CR, masked loads | 796–916 | 627–771 | ~0.85 |
| `dispatch` — state in memory, indirect call | 663–732 | 1052–1518 | **invalid** |

## The one real finding

**WebAssembly runs this instruction mix at roughly 0.85–0.90× native aarch64.**

That matters, because `docs/PLAN.md` assumed a **2–4× penalty** and derived the
whole "75–250% of full speed" range from it. For straight-line recompiled code,
that assumption is far too pessimistic. The arithmetic, the float work, and the
masked memory accesses all survive the WASM boundary nearly intact.

This is a *ceiling*, not a result. See below.

## Why this is not conclusive

**The `dispatch` row is invalid and the other two are optimistic.**

Both failures have the same cause: LLVM optimizes across the abstraction the
benchmark is trying to price.

1. **First attempt** measured `Vec` indexing, not the MMU. `self.mem[off..off+4]`
   carries its own bounds check and panic path, while the naive path's explicit
   early-return let LLVM prove safety and drop its check — so "tuned" came out
   *slower* than "naive", inverting the native ordering. Fixed by moving both
   paths to raw pointer reads so the only difference is address translation.

2. **Second attempt** failed to measure dispatch. Both entries in the function
   table are the same `block_body`, and `core::hint::black_box` is weaker on
   wasm32 than on native, so LLVM devirtualized the indirect call, inlined it,
   and optimized the loop — producing a number 2× *faster* than native and
   faster than the inlined variant, which is impossible.

So `naive` and `tuned` measure a fully-inlined hot loop with all guest state in
host registers. A real recompiler cannot do that: it emits per-block functions,
keeps guest state in memory between blocks, and pays an indirect call at every
block boundary. **Real throughput will be below these numbers**, and the gap is
exactly what the broken `dispatch` variant was meant to size.

Variance is also high (±20% run to run) — the harness should report a median
over more trials rather than single runs.

## What to do next

1. Defeat devirtualization: populate the block table at runtime with genuinely
   distinct functions, indexed by a value derived from guest state, so the
   indirect call survives optimization. Confirm by reading the emitted `.wat`
   for an actual `call_indirect`.
2. Force guest state to memory across block boundaries and confirm in the `.wat`
   that it is not being promoted to locals.
3. Median-of-N with outlier reporting instead of single trials.
4. Only then compare against the 600 MIPS gate.

Until step 4, **Phase 0 Spike A is open** and the 600 MIPS gate is unmet — not
failed, unmeasured.

## Reproduce

```sh
cd spikes/jit
cargo build --release && ./target/release/native 30000000
cargo build --release --target wasm32-unknown-unknown
cp target/wasm32-unknown-unknown/release/spike_a_throughput.wasm spike.wasm
node run-wasm.mjs 30000000
```

Checksum is identical (`429fe667`) across every variant and both targets, so all
paths compute the same result — the arithmetic is equivalent even where the
timings are not trustworthy.
