# Spike A — PowerPC → WebAssembly throughput

**Status: GATE MET.** Chrome sustains **863.9 MIPS** on the realistic variant,
against a 600 MIPS gate.

## Question

Can recompiled Broadway code sustain ≥ 600 MIPS in a browser? Broadway is
729 MHz at close to 1 IPC on game code, so ~600 MIPS is roughly 100% speed with
headroom.

## Result

30M iterations × 11 trials, median (min–max), Apple M4:

| Variant | Native aarch64 | Chrome 155 | Chrome / native |
|---|---|---|---|
| `naive` — eager CR, bounds-checked loads | 842.9 (714–892) | 722.8 (518–734) | 0.86 |
| `tuned` — lazy CR, masked loads | 844.2 (685–861) | 609.3 (518–705) | 0.72 |
| **`dispatch` — state in memory, indirect call** | **596.9** (496–626) | **863.9** (793–887) | **1.45** |

`dispatch` is the number that matters. The other two inline everything into one
hot loop with guest state in host registers, which no real recompiler can do.

**Chrome's slowest dispatch trial (793.4 MIPS) still clears the gate.**

The surprise is that WebAssembly *beats* native on this variant, by 1.45×. Two
plausible contributors, neither verified: V8's `call_indirect` against a bounded
table may be cheaper than a native load-from-global plus unpredictable indirect
branch; and the eight small block functions likely give V8's register allocator
an easier problem than one large loop body. This is the opposite of the
`2–4× slower` penalty `docs/PLAN.md` assumes, and that text now needs revising.

## How this was made trustworthy

Two earlier versions of this spike produced plausible numbers from broken
measurements. Every defence below exists because something actually went wrong.

**Structural verification — `./verify.sh`.** Disassembles the emitted module and
asserts the dispatch loop really dispatches. A MIPS number cannot tell you
whether the compiler quietly removed the thing you were measuring.

```
PASS  call_indirect present in module              25
PASS  call_indirect inside run_dispatched          3
PASS  distinct blocks as indirect targets          8
PASS  run_dispatched not inlined-fat               70 lines
```

**Semantic verification — block invocation counter.** `spike_block_calls()`
counts real block entries and the harness asserts it equals
`iters/10 + iters × trials`. Confirmed **333,000,000 = 333,000,000** on both
targets. Structure alone cannot prove the loop ran the expected number of times.

**Three defences against devirtualization**, all required:

1. Eight genuinely distinct block functions, each folding a different constant
   into the accumulator, so identical-code-folding cannot merge them.
2. The block table is filled at runtime through an exported
   `spike_install_blocks(seed)`, never a static initialiser — LLVM cannot know
   which function occupies which slot.
3. The table index is derived from guest state, so it is data-dependent rather
   than a predictable loop counter.

## What went wrong on the way here

Each of these produced a believable number that was wrong.

1. **Measured `Vec`, not the MMU.** `self.mem[off..off+4]` carries its own bounds
   check and panic path, while the naive path's explicit early-return let LLVM
   prove safety and drop its check — so `tuned` came out *slower* than `naive`,
   inverting the native ordering. Fixed by moving both paths to raw pointer
   reads, leaving address translation as the only difference.

2. **Devirtualization.** The block table held one repeated function and
   `core::hint::black_box` is weak on wasm32, so LLVM inlined the indirect call.
   Result: 1518 MIPS, faster than native and faster than the inlined variant.
   Fixed by the three defences above.

3. **A non-bijective permutation.** `(i × seed) ^ i` is not a bijection over
   0..8, so only four blocks were ever installed and the other four were
   dead-stripped — four indirect targets instead of eight. Caught by `verify.sh`,
   not by reading the code. Fixed to `(i × (seed|1)) & 7`, where an odd
   multiplier is invertible mod 8.

4. **Different address trajectories.** `block_impl` folded its per-block constant
   into the *index* computation, so every block walked different memory than
   `run_tuned` did. The comparison was measuring cache behaviour, not dispatch
   cost. Fixed so the index advance is byte-identical to `run_tuned` and the
   constant only perturbs the accumulator.

5. **Chrome silently produced nothing.** The extraction used `sed`, which cannot
   match across newlines, so a working benchmark looked like a failed one. Fixed
   with a Python extractor.

## Open issue: float contraction diverges across targets

`naive` and `tuned` produce identical checksums on both targets (`3493a94a`).
`dispatch` does not — native `8641789f` vs Chrome `27617d8c`. They agree at 10
iterations and diverge by 10M, which is characteristic of rounding, not logic.

Cause: aarch64 contracts `fa*fc + fb*fd` into a fused multiply-add with a single
rounding step. WebAssembly has no scalar FMA instruction, so it rounds twice. The
two targets compute genuinely different floats, and the error accumulates.

This does not undermine the timing result — the operation count is identical and
independently verified — but **it is a real finding for the emulator**. Broadway's
paired-single operations have defined rounding behaviour, and a recompiler that
lets the host fuse will drift from hardware. `ppc2wasm` must control contraction
explicitly, and the differential harness must compare floats bit-exactly or it
will not catch this class of bug.

## What this still does not measure

- **A synthetic kernel, not profiled game code.** The 18-instruction mix is
  plausible for a vertex transform but no Skylanders profile informs it. That
  needs the oracle traces.
- **Eight blocks, all the same shape.** Real code has thousands of blocks with
  varied sizes and register pressure, and far worse icache behaviour.
- **No MMU slow paths**, no unaligned access, no MMIO, no interrupts.
- **No cross-block guest state spilling** beyond the four registers touched here.

So 863.9 MIPS is an upper bound on a realistic shape, not a prediction. The
honest reading: **the CPU approach is not disqualified, and the headroom is
larger than the plan assumed.** Phase 2 remains the risk.

## Reproduce

```sh
cd spikes/jit
cargo build --release && ./target/release/native 30000000 11   # native
cargo build --release --target wasm32-unknown-unknown
cp target/wasm32-unknown-unknown/release/spike_a_throughput.wasm spike.wasm
./verify.sh                 # structural checks — run this first
node run-wasm.mjs 30000000 11   # Node / V8
./run-chrome.sh 30000000 11     # real Chrome, headless
```

`verify.sh` exits non-zero if the module structure is wrong. If it fails, every
timing from that module is meaningless.
