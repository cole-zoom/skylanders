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

**Done — see `docs/PHASE2-PROFILE.md`, and read it before trusting §2-§6
below, which it contradicts in three places.**

The short version: 54 MIPS was measuring compilation. The same build does
117.0 MIPS over 800M instructions, because module installation is a one-off
~22 µs per block that a 200M run never amortises. Of an 800M run, compiled
execution is 46%, installation 25%, the interpreter fallback 13% and the whole
dispatcher round-trip 13% — of which the `unordered_map` is 3.4%.

The constraint is that **blocks are 2.82 instructions long**. Chains average
16.69 instructions but cross 5.9 blocks, and every block entry pays a
prologue, the `MSR[DR]` guard and the entire link epilogue.

And the first thing to do about it is smaller than any item below: **`bdnz`
and the CTR-decrementing branch forms are not translated**, so a counted loop
leaves compiled code, interprets one instruction and re-enters on every
iteration — 3.68M forced chain exits, 7.8% of all dispatches, and a hard
ceiling on chain length that no link tuning can lift. The profile document
ranks the rest.

### 2. Make the dispatcher cheap

`jit_cache` is a `std::unordered_map<u32, JitEntry>` consulted on every chain
return. A direct-mapped array keyed by PC — the same shape as the link table —
would remove a hash and a pointer chase from the hottest path in the emulator.

Low risk and mechanical — but **measured at 3.4% of the run**, not the largest
single win. The hypothesis in §1 did not hold. Do it when it is convenient,
not first.

### 3. Fix the link table's collisions

**39% of remaining link misses have a compiled successor.** That number is
already measured (PR #52) and it means the direct-mapped table is now the
limit: two hot PCs sharing a slot evict each other forever.

Two-way or four-way associativity, or simply a better hash than
`(pc >> 2) & mask`. Guest code is not uniformly distributed over PCs and the
low bits are not the interesting ones.

The invariant this must not break is that a slot's stored PC is **checked**,
not assumed. The chain differential poisons the slot a chain reaches by
running off the end, with an entry belonging to a different PC; accepting it
costs not a slow chain but a jump into another block's code. Dropping the
check leaves both the per-instruction and block differentials green.

### 4. Support MEM2

The guard accepts only MEM1 (`0x80000000`, mask `0xFE000000`). **MEM2 lives at
`0x90000000` and the boot uses it constantly** — the XFB sits at `0x10299d80`.
Every MEM2 access bails, which ends a chain *and* forces a dispatcher
round-trip *and* interprets an instruction.

It needs a second region in the guard and a second host base in
`StateLayout`.

This was guessed to be "probably the biggest remaining source of short
chains". **Measured: 251,914 bails out of 277,355,832 block entries**, under
one in a thousand — so not that.

Do not read the bail count as MEM2 being free. A block that refuses its first
instruction is retired and interpreted from then on, so a MEM2 access bails
once and is interpreted forever. Sized separately: 9,835 retired blocks and
4,455,074 instructions through them, ~3.5% of the run.

### 5. Keep guest registers in wasm locals within a block

Every instruction currently loads its operands from the state struct in memory
and stores the result back. Within a block, a value produced by one
instruction and consumed by the next could stay in a local.

Spike A's `dispatch` variant shows memory-resident state still reaches 863
MIPS, so this looked like an optimisation rather than a necessity.

**Promoted.** In situ, compiled code runs at about 250 MIPS, not the 1538–2811
a microbenchmark reported — the microbenchmark measured long blocks and real
guest code does not come in long blocks. Compiled execution is 46% of the run.
This is now part of the top item, alongside direct linking and longer blocks.

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

- "The dispatcher's hash lookup is probably the largest single win" — wrong,
  3.4%. "MEM2 is probably the biggest source of short chains" — wrong, one
  bail in a thousand entries. Both were written in this document, in a
  section that said they were guesses, and both were settled by one profile.

- "Linking is gated on coverage" (PR #48) — wrong. Coverage reached 98.7% and
  linking still lost.
- "The link table is too small" — enlarged it 8×, changed nothing. The counters
  then showed 99.7% of misses were successors that were never compiled.
- "MSR will lengthen blocks" — wrong. Blocks are limited by branch density
  (~22% of the instruction stream), which no amount of instruction support
  changes.

**Use the differentials.** `harness/wasm-core/run-jitdiff.sh` builds and runs
all four in about five seconds: hand-written cases, a randomised
per-instruction differential, a randomised **block** differential that
compares registers, condition fields and a window of guest memory, and a
randomised **chain** differential that publishes a link table and runs blocks
linked together.

They need Emscripten and the prebuilt libraries in `dolphin-web/build-wasm`,
which is why CI does not run them. They do not need a browser — node has
WebAssembly, a shared memory and a growable table, which is the whole of what
this test uses.

Read the counters the chain differential prints, not just its verdict. They
say how many chains linked, how long the longest was, and how many took each
of the four exits. A chain differential whose lookups all missed would pass in
exactly the same silence as one that worked — see below.

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

- ~~**Linking.**~~ Closed. `jitdiff.cpp` now runs a randomised chain
  differential: blocks laid end to end, published in a link table the way the
  dispatcher publishes them, entered at the first and compared against the
  interpreter over registers, condition fields, LR, CTR, PC and a window of
  guest memory. A typical run links 255 of 300 chains, 1840 tail calls, with
  the longest chain 32 blocks.

  Two things about it are worth knowing before changing it.

  How far a chain got is read from the **downcount**, not the returned PC.
  Every exit accounts for itself there — the epilogue once per block, a bail
  for the part of a block that ran, `mtmsr` for itself — so the budget delta
  is the one number covering all four. The returned PC says where a chain
  stopped, not how many blocks it crossed. The budget is a *soft* limit: the
  epilogue subtracts and only then tests, so a chain overshoots by up to one
  block, and that is the emulator's behaviour too.

  It asserts on its own **coverage**, not only on agreement. Breaking the
  epilogue's hash so no chain ever forms leaves every state comparison green —
  measured, not assumed — and only `chains actually tail-called` catches it.
  Keep those assertions.

- **Dispatcher-side link management.** The chain differential publishes the
  table itself. What it does not exercise is the dispatcher's own logic in
  `bootgame.cpp`: when a block is published, and **retirement** — a block that
  bails on its first instruction is dropped and its slot cleared, permanently
  and with no re-entry. That path has no test.
- Timing quantisation. Chains are 10.73 instructions, so a scheduled event can
  fire that late. Nothing checks it.
- Anything disc-dependent in CI. CI has no disc and never will; the boot
  differential, the FIFO milestone and the capture comparison are all manual.
