# Where the 11× went

`docs/PHASE2-HANDOFF.md` §1 asked for one thing before anything else: profile a
chain return, because everything ranked after it was a guess. This is that
measurement, and it moves most of the ranking.

Numbers are from Chrome, booting *Skylanders: Spyro's Adventure*, with
`harness/wasm-core/run-jit.sh`. The instrumentation is in `bootgame.cpp` and
prints on every `--jit` run.

---

## The first finding is that the headline number was measuring compilation

The same build, same flags, three run lengths:

| instructions | time | MIPS | blocks compiled |
|---|---|---|---|
| 200M | 3630 ms | **54.7** | 59,916 |
| 400M | 4614 ms | **86.7** | 60,250 |
| 800M | 6835 ms | **117.0** | 91,817 |

Module installation is a one-off cost of ~22 µs per block and it does not grow
with run length. At 200M instructions it is 32% of the run. The 400→800M
segment reached 31,567 blocks of code the shorter runs never saw — so it is
not a spin loop — and ran at **170 MIPS**.

**54 MIPS was never the recompiler's speed.** It is the speed of a 200M-
instruction window in which half the time is spent compiling and interpreting
a boot that only happens once. Against ~600 MIPS, the gap at 800M is 5×, and
the marginal gap is closer to 3.5×.

Measure at several step counts before believing any number from this harness.

---

## Where the time goes, at 800M instructions

```
speed     6835 ms, 117.0 MIPS
executed  800,000,260 instructions
jit       783,170,147 in compiled code (97.9%)
jit       277,355,832 block entries, 2.82 instructions per entry
jit       16,830,113 interpreted singly, 251,914 block bails
jit       link hits 230,438,203, attempts 251,975,037, misses 21,536,834
jit       translate 230 ms over 91,817 blocks
jit       install  1712 ms over 76,556 modules (22.4 us each)
```

| | ms | share | how it was measured |
|---|---|---|---|
| compiled execution | ~3115 | **46%** | residual |
| module installation | 1712 | **25%** | timed directly |
| interpreter fallback | ~919 | 13% | 16.8M insts at the measured 18.3 MIPS |
| dispatcher round-trips | ~859 | 13% | 46.9M at 18.3 ns, below |
| translation (the C++ emitter) | 230 | 3% | timed directly |

### How the round-trip was split

`performance.now()` resolves to 5 µs even in a cross-origin-isolated page and
a round-trip is ~100 ns, so it cannot be timed one at a time. It was timed by
**doing one thing twice** (`--jit-probe=N`) and subtracting a baseline over
the whole run:

| probe | cost per round-trip | of the 800M run |
|---|---|---|
| `jit_cache` lookup (`unordered_map`) | 5.0 ns | 235 ms, 3.4% |
| `CoreTiming::Advance` | 6.4 ns | 300 ms, 4.4% |
| one indirect call through the table | 6.3 ns | 296 ms, 4.3% |
| **total** | **17.7 ns** | |

Checked a second way: linking off turns 17.7M round-trips into 70.1M with the
same instruction count, and costs 958 ms more — **18.3 ns** per extra
round-trip. Two independent methods, 3% apart.

---

## What this does to the ranked plan

### It was wrong about the dispatcher

> "Cheapen the dispatcher … probably the largest single win."

The `unordered_map` lookup is **3.4%** of the run. Replacing it with a
direct-mapped array is worth at most that. It is still worth doing — it is
mechanical and low risk — but it is not the win.

### MEM2 is not where the bails are, which is not the same as it being free

> "This is probably the biggest remaining source of short chains."

**251,914 bails out of 277,355,832 block entries.** Under one in a thousand —
so MEM2 is not ending chains at anything like the rate that line assumed.

But the bail count structurally understates it, and it took a second
measurement to see why. A block that refuses its first instruction is
**retired**: dropped from the cache, its link slot cleared, and interpreted
from then on. A MEM2 access therefore bails *once* and is interpreted
*forever*, so its cost moves out of the bail counter and into the interpreter
fallback where nothing attributed it.

Sized directly, by splitting interpreted instructions by cause: **9,835
blocks retired, 4,455,074 instructions interpreted through them, 26.5% of all
interpretation** — and that list is 98% loads and stores. Call it ~3.5% of
the run, plus 9,835 blocks removed as link targets.

Not the top item. Several times what the bail count suggests.

### The real constraint is that blocks are 2.82 instructions long

This is the number to carry forward. Chains average 16.69 instructions, which
looks healthy — but a chain crosses **5.9 blocks**, and every block entry pays
the same fixed cost: a function prologue, the `MSR[DR]` guard, and the whole
link epilogue (downcount load, subtract, store, test; hash; table load; PC
compare; tail call). Roughly twenty wasm instructions of overhead to run
**2.82** of guest work.

That also explains the gap the handoff said did not exist. Spike A measured
hand-written wasm at 863 MIPS and the emitted code at 1538–2811 MIPS in a
microbenchmark, and concluded codegen was not the constraint. In situ,
compiled code runs at about **250 MIPS**. The microbenchmark was not wrong; it
measured long blocks, and real guest code does not come in long blocks.

Linking was the right fix for chain length and it worked. It does nothing for
this, because linking changes how often the *dispatcher* is reached, not how
often a *block* is entered.

---

## Why blocks are short, and what the interpreter is left holding

Two more counters, because "blocks are 2.82 instructions" does not say what to
*do*. Lengthening them by compiling more instruction forms and lengthening
them by compiling across branches are different projects.

Behind `--jit-stats`, and off by default: they sit on the dispatch and
interpreter-fallback paths and cost about 4% (6883 ms becomes 6975 ms over
800M). The speed line is the number this project steers by, and a
measurement that quietly taxes the thing it measures is worse than none.

### Why a block ends

| ends on | distinct blocks | avg instructions | dispatches |
|---|---|---|---|
| a branch | 67,863 | **5.09** | 42,875,744 |
| an unsupported instruction | 23,872 | **1.47** | 4,051,554 |
| the 64-instruction limit | 82 | 64.00 | 166 |

Blocks that end at a branch already average 5.09 instructions, at the ceiling
branch density implies. **Branches are not the problem.** Blocks that end on
an instruction we cannot translate average 1.47 — they stop almost
immediately — and there are 23,872 of them.

Three averages worth holding together:

- 4.20 instructions per **distinct** block
- 4.78 per block the **dispatcher** enters
- 2.82 per block entry **overall**, so blocks reached by a **tail call**
  average **2.43**

Chains are long (16.69) and made of very short blocks. Linking is doing its
job and the thing it links is too small.

### What the interpreter runs

16,830,113 instructions, split by *why* they are there — a distinction that
turned out to matter more than the totals.

**Retired: 9,835 blocks, 4,455,074 instructions, 26.5%.** A block that refuses
its first instruction is dropped and interpreted from then on. The list is
98% loads and stores — `lwz` 28.5%, `stw` 25.4%, `stbu` 19.7%, `lbzu` 10.0%,
`stwu` 8.3%, `lbz` 4.7%. This is MEM2, and it is the reason the bail count
does not show it.

**Never compiled: 73.5%**, and it concentrates:

| form | share of never-compiled | instructions |
|---|---|---|
| `bdnz` / `bdz` (opcode 16, BO bit 2 clear) | **29.7%** | 3,675,627 |
| `dcbz` | 18.9% | 2,336,730 |
| `rlwinm.` (Rc set) | 8.9% | 1,097,934 |
| `addic.` | 6.4% | 789,359 |
| `subfic` | 3.7% | 460,258 |
| `mftb` | 3.6% | 441,690 |
| `adde` | 3.2% | 400,769 |
| `dcbst` | 2.3% | 281,703 |

`bdnz` is the top of that list and it is worse than its share suggests,
because it is **the loop back-edge**. `block.h` refuses the CTR-decrementing
branch forms, so a counted loop leaves compiled code, interprets one
instruction and re-enters *on every iteration*. That is 3.68M forced chain
exits — 7.8% of all 46.9M dispatches — and it caps chain length at the body
of one loop iteration no matter how good linking gets.

The rest of the list is mostly one theme: **the condition register and XER
carry are not modelled for arithmetic**. `rlwinm.`, `addic.`, `subfic` and
`adde` together are 22.2%.

---

### Suggested order now

0. **`bdnz` and the CTR-decrementing branch forms.** Smallest change on this
   list by a wide margin — CTR is already read and written through the SPR
   path, and the only reason these are refused is that the decrement is not
   modelled. It pays three times: 3.68M interpreted instructions, 3.68M
   forced chain exits, and a ceiling on chain length that no amount of link
   tuning can lift while a loop's back-edge is uncompiled. Do this first.

1. **The rest of the unsupported long tail**, in the order measured above:
   `dcbz` and `dcbst`, then CR-and-carry arithmetic (`rlwinm.`, `addic.`,
   `subfic`, `adde`), then `mftb`. These attack the 13% interpreter fallback
   and the 46% per-entry cost at the same time, because the 23,872 blocks
   that end on an unsupported instruction stop after 1.47 of them.

2. **Batch installation — 25% of the run.** 76,556 separate
   `new WebAssembly.Module` calls at 22.4 µs. One module holding many blocks
   amortises nearly all of it, and it is a precondition for the next item:
   two blocks in the same module can `return_call` each other directly.

3. **Cut the per-block-entry cost — 46% of the run.**
   - *Direct linking.* When the successor is known and in the same module,
     the epilogue's hash, table load and PC compare all disappear. Chains
     already hit 91.5% of the time, so the lookup is nearly always answering
     a question we could have answered at compile time.
   - *Registers in wasm locals.* Ranked 5th before this profile. The fixed
     cost of an entry is only worth paying if the block does real work
     between prologue and epilogue.
   - *Traces across branches.* Last, because the measurement says branches
     are not currently the binding constraint: blocks that end at a branch
     already reach 5.09 instructions.

4. **MEM2 — ~3.5% of the run and 9,835 retired blocks.** Now sized. Also a
   correctness gap, which is the better reason to do it.

5. **Link-table collisions — 58.9% of misses have a compiled successor**, up
   from 39.1% at 200M. Real and growing, but it buys chain length, and chain
   length is not what is costing us.

6. **Floating point and paired singles.** Re-rank against a *gameplay*
   profile, not this one.

**Block retirement deserves a second look while doing any of this.** It is
permanent, with no re-entry, and it is currently load-bearing: it is what
keeps a MEM2 access from bailing 4.5 million times. Once MEM2 is supported,
the blocks it retires will be a different and much smaller population, and a
permanent decision made from one observation will look much less safe.

---

## What this measurement does not cover

- Everything here is one game, booting, in Chrome, on one machine. A gameplay
  profile will rank floating point and paired singles completely differently,
  and the handoff already says so.
- Compiled execution at ~250 MIPS is a **residual**, not a direct measurement:
  it is what is left after the four timed terms. If one of those is wrong, it
  absorbs the error.
- The interpreter term assumes the 18.3 MIPS measured on a `--jit`-less run
  also holds for instructions interpreted one at a time between compiled
  blocks. Those are different instructions — they are the ones no block would
  compile — and may well be slower.
- The probes measure `Advance` with no time to advance by, so they catch its
  fixed cost and not the occasional call that fires a scheduled event.
