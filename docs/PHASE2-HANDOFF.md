# Phase 2 handoff — the recompiler, and how to get it to full speed

Written for whoever picks this up next. `docs/PLAN.md` is the project; this is
the state of the recompiler, what the constraint actually is, and the traps
that have already cost time.

---

## Where it stands

Booting *Skylanders: Spyro's Adventure* in Chrome, 200 million instructions:

| | MIPS | compiled |
|---|---|---|
| interpreter | 18.0 | — |
| `--jit` | 42.1 | 94.9% |
| `--jit` (linking, default) | **54.0** | 94.9% |

Full speed is roughly **600 MIPS** — Broadway is 729 MHz at close to 1 IPC. So
there is about **11×** still to find.

What works: 98.7% of executed instructions translate; blocks tail-call their
successors through a link table; the dispatcher falls back to the interpreter
for anything refused; the boot completes and renders correctly.

What does not exist: floating point, paired singles, MEM2, block invalidation,
and any profiling of where the 11× actually went.

---

## The constraint is no longer codegen

This is the most important thing to understand before touching anything.

- Spike A measured **hand-written** wasm at 863.9 MIPS (`spikes/jit/FINDINGS.md`).
- The **emitted** code measured 1538–2811 MIPS in a microbenchmark (PR #42).

The code the compiler produces is already fast enough. At 54 MIPS in the real
boot, the time is going somewhere else — and at 10.73 instructions per chain,
200M instructions means roughly **17.7 million dispatcher round-trips**, each
costing an `unordered_map` lookup, an indirect call, and a `CoreTiming::Advance()`.

**That has never been profiled.** It is the strongest hypothesis, not a
measured fact, and the first job is to turn it into one.

---

## What to do, in order

### 1. Profile a chain return — before changing anything

Everything below is a guess until this exists. Instrument or sample the
dispatcher loop and find the split between: the cache lookup, the call itself,
`CoreTiming::Advance`, and the bookkeeping.

The counters that already exist (`link_stats`, `miss_but_cached`, per-chain
instruction counts) are the model to copy: count the things that distinguish
*different causes*, not just the things that are easy to count.

### 2. Make the dispatcher cheap

`jit_cache` is a `std::unordered_map<u32, JitEntry>` consulted on every chain
return. A direct-mapped array keyed by PC — the same shape as the link table —
would remove a hash and a pointer chase from the hottest path in the emulator.

Low risk, mechanical, and probably the largest single win if the hypothesis in
§1 holds.

### 3. Fix the link table's collisions

**39% of remaining link misses have a compiled successor.** That number is
already measured (PR #52) and it means the direct-mapped table is now the
limit: two hot PCs sharing a slot evict each other forever.

Two-way or four-way associativity, or simply a better hash than
`(pc >> 2) & mask`. Guest code is not uniformly distributed over PCs and the
low bits are not the interesting ones.

### 4. Support MEM2

The guard accepts only MEM1 (`0x80000000`, mask `0xFE000000`). **MEM2 lives at
`0x90000000` and the boot uses it constantly** — the XFB sits at `0x10299d80`.
Every MEM2 access bails, which ends a chain *and* forces a dispatcher
round-trip *and* interprets an instruction.

This is probably the biggest remaining source of short chains. It needs a
second region in the guard and a second host base in `StateLayout`.

### 5. Keep guest registers in wasm locals within a block

Every instruction currently loads its operands from the state struct in memory
and stores the result back. Within a block, a value produced by one
instruction and consumed by the next could stay in a local.

Spike A's `dispatch` variant shows memory-resident state still reaches 863
MIPS, so this is an optimisation rather than a necessity — do it after §1–4.

### 6. Floating point and paired singles

**Not optional for the actual game, and invisible in the current profile.**
`--jit-coverage` is measured over boot, where FP is ~1.3%. A vertex pipeline is
mostly paired singles, so a gameplay profile will rank completely differently.

Re-run `--jit-coverage` against gameplay before deciding how much of this to
build. `docs/PLAN.md` flags float contraction (fused multiply-add on aarch64
vs two roundings on wasm) as a Medium risk; it has been observed once, in the
software rasteriser (PR #37), and will reappear here.

---

## How to work in this repo

The methodology matters more than the code, because it is what made the
measurable progress.

**Measure before changing.** Every performance intuition in this project has
been wrong, and every measurement has been right within one run:

- "Linking is gated on coverage" (PR #48) — wrong. Coverage reached 98.7% and
  linking still lost.
- "The link table is too small" — enlarged it 8×, changed nothing. The counters
  then showed 99.7% of misses were successors that were never compiled.
- "MSR will lengthen blocks" — wrong. Blocks are limited by branch density
  (~22% of the instruction stream), which no amount of instruction support
  changes.

**Use the differentials.** `harness/wasm-core/jitdiff.cpp` runs three:
hand-written cases, a randomised per-instruction differential, and a
randomised **block** differential that compares registers, condition fields and
a window of guest memory. Build it with `harness/wasm-core/link.sh` and run it
in a browser.

The block differential has caught a real translation bug and, three times, its
own harness. Expect that: most early failures of a randomised test are the
test. Run each one down anyway.

**Verify a test can fail.** Before trusting a new check, break the thing it
guards and confirm it goes red. The `slw` bug (PR #49) passed every
hand-written case because they shifted by 4.

**Small PRs, five sections each.** `docs/COMMIT_CONVENTION.md`. CI gates every
push and requires them.

---

## Landmines

Things that have already cost hours here.

**The oracle can be wrong.** Memory differential cases once showed the
recompiler returning correct values and the *interpreter* returning zero — the
interpreter needs BAT setup for data access. It reads exactly like a compiler
bug.

**Do not perturb guest state from the dispatcher.** Writing `downcount` before
every block — linked or not — silently changed what the game did and dropped
compiled coverage from 38% to 15.6%, for two measurements, before it was
caught.

**`bclrl` reads the old LR then writes it.** Wrong order makes a call return
to itself, and no register comparison distinguishes that from a correct
return.

**`stwu` writes back the guest address, not the host one.** The host address
is a plausible-looking number that corrupts the stack pointer.

**The SPR field is split and swapped**:
`((ins >> 16) & 0x1F) | (((ins >> 11) & 0x1F) << 5)`.

**`slw`/`srw` take a six-bit shift count** and yield zero at ≥ 32; wasm masks
to five bits.

**PowerPC's `MB..ME` mask wraps** when `MB > ME` — it selects the complement of
the gap, not an empty set.

**Compiled blocks assume MSR[DR] is set.** There is a per-block entry guard;
if you ever let a block continue past an `mtmsr`, that guard stops being
sufficient.

**Block retirement is permanent.** A block that bails on its first instruction
is dropped forever, even if every later execution would have succeeded.

**There is no block invalidation at all.** Self-modifying code or DMA into code
memory would execute stale translations silently. Nothing in this boot appears
to do that, which is not the same as it being safe.

---

## What is not covered by any test

Stated plainly, because it is the largest risk:

- **Linking.** The randomised differentials compile with linking *off*. The
  only evidence that chains execute correctly is that the boot completes and
  the PC range looks sane — and linking is now the default, doing 95% of the
  work. **Closing this is worth doing before optimising further.**
- Timing quantisation. Chains are 10.73 instructions, so a scheduled event can
  fire that late. Nothing checks it.
- Anything disc-dependent in CI. CI has no disc and never will; the boot
  differential, the FIFO milestone and the capture comparison are all manual.
