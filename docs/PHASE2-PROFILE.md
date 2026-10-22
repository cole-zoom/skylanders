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

### Suggested order now

1. **Cut the per-block-entry cost — 46% of the run.** Three independent
   levers, probably in this order:
   - *Direct linking.* When a block's successor is already compiled and the
     target is static, the epilogue's hash, table load and PC compare are all
     avoidable. Chains already hit 91.5% of the time.
   - *Longer blocks.* Blocks end at every branch, and branch density is ~22%,
     so the ceiling on a basic block is about 4.5 instructions and we get
     2.82. Traces or superblocks that continue through a predicted branch
     raise the ceiling instead of approaching it.
   - *Registers in wasm locals.* Ranked 5th before. The fixed cost of an entry
     is only worth paying if the block does real work between prologue and
     epilogue.
2. **Batch installation — 25% of the run, and the reason short runs look
   slow.** 76,556 separate `new WebAssembly.Module` calls at 22.4 µs. One
   module holding many blocks would amortise nearly all of it. This is the
   cheapest large win on the list.
3. **Interpreter fallback — 13%.** 2.1% of instructions taking 13% of the
   time. Worth finding what they are before building anything: a coverage run
   that ranks the *interpreted* instructions, not the executed ones.
4. **Link-table collisions — now 58.9% of misses have a compiled successor**,
   up from 39.1% at 200M. This grew exactly as predicted and is real, but it
   buys chain length, which is worth ~13% in total.
5. **MEM2, floating point, paired singles.** Correctness, not speed. Re-rank
   against a *gameplay* profile, not this one.

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
