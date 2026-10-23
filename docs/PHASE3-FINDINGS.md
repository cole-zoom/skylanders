# Phase 3 — what was actually slow, and what moved it

An independent investigation of why the browser build did not run at full
speed, compared against the plan in `docs/PHASE3-DENSITY.md`, and the work
that followed. Written so the next reader can check every claim against a
command.

Headline, same harness, same machine (Chrome 155, Apple M4):

| | wall time | real-time speed |
|---|---|---|
| start of this work (#82): 800M instructions, 193.8 MIPS | 4127 ms for 1.10 emulated s | 26.6% |
| end: the first 2.0 emulated seconds of the boot | 1391–1427 ms | **140–144%** |
| end, idle loops not skipped (796 MIPS) | 1847 ms | 108% |
| end: 20 emulated seconds, input and a figure on the portal | 6865 ms | **291%** |

"Real-time speed" is emulated seconds per wall second, which is what a
player experiences and what MIPS stops measuring the moment an idle loop
is skipped. The harness prints both now, and `--ticks=N` runs for a fixed
amount of emulated time, which is the only comparison that holds once
timing changes what the game does.

Correctness: all four differentials pass with every new form covered, and
the 20,000-step native trace differential still matches.

---

## 1. Where the time went

The Phase 2 budget attributed the run as compiled execution 50%,
installation 17%, interpreter 15%, dispatcher 15%. Three of those were
timed directly and the largest was a residual. A sampling CPU profile of
the 800M boot, attached to the worker that runs `main()` through the
Chrome DevTools protocol (`Profiler.start` on every worker target), gave
a different picture:

| self time, 800M boot | ms | share |
|---|---|---|
| the dispatcher loop in `main()` (inlined step, lookup, bookkeeping) | 1422 | 35% |
| compiled blocks | 1016 | 25% |
| `new WebAssembly.Module` + `Instance` | 555 | 14% |
| interpreter (`SingleStepInner` and callees) | ~580 | 14% |
| `CoreTiming::Advance` + `MoveEvents` | 295 | 7% |
| `MSRUpdated` + `CheckExceptions` | 125 | 3% |
| translation (the C++ emitter) | ~100 | 2.5% |

Confirmed a second way: with linking off (218M round trips instead of
32.5M), the dispatcher loop rose by 2.3 s and `Advance` by 1.8 s while
compiled-block time fell — about 24 ns per extra round trip, all of it
outside the emitted code.

Two more facts the profile gave that no residual could:

- **The hot set was ten functions.** Of 655 compiled functions sampled,
  the top ten held 73.5% of compiled-block time and the top hundred 84.5%.
- **Half the boot was one three-instruction loop.** The trace of the last
  2000 instructions is `lwz r0,0x248(r13); cmpwi r0,0; beq -8` at
  `0x803873a0`, and the opcode counts (bc 17.9%, lwz 17.4%, cmpi 16.4%)
  say it ran about 130 million times. Another 24.4 million of the 32.5
  million dispatcher round trips were `mtmsr` exits: the OS toggling
  interrupts around every critical section.

### Checking the emitted code itself

`docs/PHASE3-DENSITY.md` rests on one number: 2.6 ns per guest instruction
in the boot, read against a microbenchmark curve where cost rises from 0.44
to 2.25 ns as the working set grows to a megabyte, and concluded that the
emitted code "sits at the bottom of that curve" because there is too much
of it.

The hot loop above, compiled by the same `block.h`, linked to itself with
the same direct tail call, and run in isolation (scratch `hotloop.cpp`,
node, V8):

| variant | ns per iteration | ns per instruction | MIPS |
|---|---|---|---|
| block.h output, linked, direct self tail-call (the boot's shape) | 1.76 | 0.59 | 1707 |
| block.h output, linked via the table lookup | 2.29 | 0.76 | 1309 |
| block.h output, unlinked, one call per iteration | 3.22 | 1.07 | 933 |
| hand-written: registers in locals, one load + byte swap, i64 CR | 0.64 | 0.21 | 4684 |

So the emitted code for the instructions that made up half the run cost
0.59 ns each, not 2.6, and the 2.6 was a residual carrying everything the
probes had not measured. The code could be better — the hand-written
variant is 2.8× faster — but it was not the constraint, and the hot set
was far too small for an instruction-cache explanation.

---

## 2. Where this disagrees with `PHASE3-DENSITY.md`, and why

**The density plan was built on a measurement that could not see the
dispatcher.** The round-trip probes (`--jit-probe=N`) timed three things by
doing each twice — the cache lookup, `Advance` with nothing to advance, one
indirect call — and summed them to 17.7 ns. They did not time the loop
around those things, the per-step harness checks, the indirect-call
misprediction into a varying target, or `Advance` when events actually
fire. Everything they missed landed in the residual called "compiled
execution".

**Its microbenchmark measured a different workload.** `jitspike.cpp` walks
up to 4096 distinct blocks round-robin, which is the worst case for both
instruction cache and indirect-branch prediction, and its own caveat says
it cannot separate the two. The boot does not touch its code uniformly;
it spends half its time in one block. A kill-gate experiment on that spike
would have "passed" or "failed" on a property the boot does not have.

**Its remedy would have cost on the path that mattered.** Out-of-line
helpers reduce bytes by adding calls. In a workload where a handful of
blocks are always hot, the bytes were already in cache and the calls would
have been pure cost. The ideas in its "independent of density" section —
deferring singleton compiles, the interpreter long tail — were right, and
both are done below in a different form.

**It also could not have found the ceiling.** Dolphin throttles emulation
to real time by default. Below ~90% of real time that is invisible; once
the recompiler got close, `CoreTiming::Throttle` began sleeping and the
profile showed 184 ms of clock reads in a 2.2 s run. No amount of code
density would have moved the speed line past 100% while that was on.

What the density document got right and this work kept: anything that
grows code without a measured reason is suspect; the differentials before
every push; the warning that structural changes measured as "fewer
entries" had not paid.

---

## 3. What was built, in order, with what it measured

Same 800M boot throughout. The emulated time covered by those 800M
instructions grows once idle loops are skipped, so the column that
compares across rows is real-time speed.

| step | real-time speed | notes |
|---|---|---|
| baseline | 26.6% | 193.8 MIPS |
| Dolphin's slice model: `Advance` only when the downcount expires, link-table dispatch, `mtmsr` continuing the chain unless a sensitive bit changes or an interrupt is pending | 37.0% | 269.9 MIPS; round trips 32.5M → 7.6M; `Advance` calls 32.5M → 76k |
| idle loops (Dolphin's busy-wait rule) hand the slice to `CoreTiming::Idle` | 38.0% | 699M ticks skipped; the instruction mix changes to real work, which exposes the next line |
| MEM2 and the uncached mirrors through a 128-entry region table; the carry forms, divides, `mulhw`, `rlwimi`/`rlwnm`, `srawi`/`sraw`, `dcbz`, cache no-ops | 72.1% | interpreted instructions 40M → 2.9M |
| tier-up threshold: a block is compiled on its 16th visit, interpreted straight-line before that | 92.8% | modules 11.3k → 3.6k; code that runs once is never compiled |
| one `i32.load` plus a two-rotate byte swap for words | — | no measurable change on its own |
| `mftb` from CoreTiming's globals; `lmw`/`stmw`; SRR0/1, SPRG0-3, GQR0-7, DAR/DSISR, XER; FP loads/stores with Dolphin's exact single/double conversions; `fmr`/`fneg`/`fabs`/`fnabs`/`fsel` and the paired moves and merges | 94.4% | |
| no retirement of a block that refused while MSR[FP] was off (lazy FP context switch); code in MEM2 compiled | 95.1% | |
| throttle off; `lwarx`/`stwcx.`; a peephole for `mfmsr; rlwinm/ori; mtmsr` | **150.3%** | the throttle was the ceiling |
| the interpreter hatch: an untranslated instruction calls the interpreter's handler in place instead of ending the block | 146–148% | −3% in the boot window, where FP is rare; **+21% over 20 emulated seconds past it** (240% → 291%) |
| loads and stores to the hardware registers (`0xCC00xxxx`, the GX gather pipe above all) through the hatch rather than bailing and retiring the block | — | bails 64k → 1.8k over the boot |
| a second tier for code that runs with translation off: the exception handlers at physical `0x500`… | — | interpreted instructions over 3B: 5.1M → 1.7M; no change in the boot window |

The three rows at the end are measured with `--ticks` (fixed emulated
time); the earlier ones at a fixed 800M instructions, which covered the
same 2.056 emulated seconds until the hatch changed the timing path.

The native build of the harness was also repaired (`compile_region` had
been left outside the Emscripten guard), the differential grew checks for
XER, the FP registers, FPSCR and the reservation, a MEM2 scratch window,
the uncached-mirror BATs the boot maps, and a larger node heap; and two
diagnostics were added: `--jit-names` gives every block a wasm name of the
form `b_<pc>` so profiles name guest code, and `--disasm=ADDR,N` prints
what is there.

---

## 4. Design notes worth keeping

**The dispatcher follows Dolphin's model now.** `CoreTiming::Advance` sets
the downcount to the distance to the next event; blocks decrement it; the
clock advances only when it expires. A chain that ends early for any other
reason dispatches again with the budget it has left. The block cache is
consulted through the link table first (one load, one compare), and the
hash map only on a miss.

**`mtmsr` stays in the chain** unless (a) PR, FP, IR, DR, FE0 or FE1
changed, (b) EE was enabled with an exception pending, or (c) a page-table
update is waiting. The OS's `OSDisableInterrupts`/`OSEnableInterrupts`
shape is recognised and emitted as a store plus one or two loads.

**Idle loops** use Dolphin's `IsBusyWaitLoop` rule: a conditional branch
back to the block's own start, a body of loads and integer arithmetic that
never writes a register it read first, no other branch. The taken edge
charges the budget, sets a flag and returns; the dispatcher calls
`CoreTiming::Idle`. The game is still at the same PC when the next event
has fired.

**The region table** maps the top seven bits of a guest address to a host
base: MEM1, MEM2 (two granules) and both uncached mirrors; everything
else — MMIO, the EFB, the locked cache — is zero and bails to the
interpreter. One load replaced one compare.

**The tier-up threshold** interprets a straight-line run from a cold PC up
to the first taken branch, and only the start PC gets a cache entry.
Installing a module costs ~24 µs; interpreting a cold block costs a few
hundred nanoseconds.

**FP correctness is the interpreter's.** `lfs`/`stfs` use Dolphin's
`ConvertToDouble`/`ConvertToSingle` reimplemented bit for bit (including
the denormal and NaN branches, which `f64.promote_f32` would get wrong).
Every FP *arithmetic* instruction currently goes through the hatch, so its
NaN ordering, FPSCR flags and the single-precision rounding quirks are
exactly `Interpreter_FloatingPoint.cpp`'s. Inlining the hottest of them is
the next step and the differential is ready for it: FPSCR and all 32
paired registers are compared.

**The hatch** is one imported function, `env.ppc_interp_op(state, ins,
pc)`, exported by the harness and imported by every block module as
function index 0 — so a direct call between sibling blocks adds one to the
index it patches in. The helper is `Interpreter::SingleStepInner` with the
fetch and the FP-availability test removed (the block supplies the word
and makes the test) and `SingleStep`'s exception check folded in; it
returns non-zero when the PC did not simply advance, and the block leaves
with the PC the interpreter chose. A load or store whose address lands in
`0xCC00xxxx`, `0xCD00xxxx` or `0xCD80xxxx` takes the same path; any other
address outside RAM still bails, because the interpreter has no bounds
check on its MMIO handler table and needs a video backend for the EFB,
and the randomised differential found both.

**Translation off has its own tier.** The block cache, link table and
region table come in two sets, chosen by MSR[DR] at every dispatch. The
physical tier's region table maps `0x00000000` and `0x10000000`; a chain
cannot change DR without leaving, so a chain is one tier or the other.

The last profile, over the first 2 emulated seconds: compiled blocks 55%
of the run, the dispatcher loop 18%, module compilation 5%, disc reads
over XHR 4%, the interpreter under 3%. What is left is the emitted code.

---

## 5. Known gaps, ranked by what they cost now

1. **FP arithmetic through the hatch costs ~40–70 ns an instruction.** Fine
   against the ~150 ns it replaced; not fine for gameplay, where it is a
   quarter of the stream. Inline `fmuls`/`fadds`/`fmadds`/`ps_madd`/
   `psq_l` first, profile-guided (`--jit-names`), exactly, one at a time;
   the differential compares FPSCR and all 32 paired registers already.
2. **Compare and CR0 encoding.** `cmpwi` is ~30 wasm instructions through
   `s_crTable`; Dolphin's own JIT stores the sign-extended difference.
   Adopting that needs the differential to compare decoded CR fields
   rather than raw words, which is the right invariant anyway. Compares
   and conditional branches are a third of the instruction stream.
3. **Code quality in the hot integer loops** — registers in locals across
   a block, real `loop` constructs for self-targeting back edges. The
   hand-written hot-loop variant shows 2.8× headroom on that shape.
4. **The hatch's inline footprint.** Every load and store now carries a
   cold path of about forty bytes for the hardware-register case; that is
   the likely cause of the −3% in the boot window.
5. **Interrupt-polling loops that call a function.** The game's main wait
   past the boot is `bl OSDisableInterrupts; lbz; bl OSEnableInterrupts;
   cmpwi; bne`, which the busy-wait rule cannot see through. Dolphin
   cannot either; with calls followed (the `trace_uncond` flag) and
   `mfmsr`/`mtmsr` admitted when only EE changes, it could be.
6. **No block invalidation**, as before. `icbi` is interpreted and
   invalidates nothing of ours. `mtspr` to a BAT goes through the hatch
   and the region tables do not follow it; the boot sets the BATs once.
7. **Gameplay has not been reached in the harness.** The runs with input
   and a figure sit at a prompt that wants a Nunchuk; attaching one
   (`record-gameplay.sh` shows the configuration) is the precondition for
   a gameplay profile.
8. **The interpreter has no bounds check on its MMIO handler table.** A
   store to `0xCC80xxxx` from guest code would trap the whole emulator.
   No game does it; the randomised differential did, which is why the
   hatch only takes the three blocks the interpreter decodes.

---

## 6. How to measure

```sh
harness/wasm-core/run-jitdiff.sh                                        # the differentials, ~1 min
harness/wasm-core/run-jit.sh 2000000000 --jit --ticks=1458000000        # the number: 2.0 emulated s
harness/wasm-core/run-jit.sh 2000000000 --jit --jit-stats --ticks=1458000000   # the breakdowns
harness/wasm-core/run-jit.sh 20000000000 --jit --input=A --skylander --ticks=14580000000  # 20 s past the boot
harness/wasm-core/run-jit.sh 800000000 --jit --disasm=803a7e00,12       # read a hot block
harness/wasm-core/run-boot.sh /tmp/ssa.iso 20000                        # native trace differential
```

Flags that turn each change off for an A/B: `--jit-no-idle`,
`--jit-no-mtmsr`, `--jit-no-hatch`, `--jit-no-phys`, `--jit-threshold=1`,
`--jit-no-link`. The first argument is a cap on instructions; `--ticks=N`
stops at N emulated ticks (729 million per second), which is the
comparison to use.

Measurements vary by about ±3% run to run; a difference inside that is
not a result. Do not run the differential (node) on the same machine
while measuring: it inflated runs by 15% and read as a regression. Runs
past the boot with input are not comparable across builds at a fixed
instruction count: presses are scheduled by instruction count, so timing
changes what the game does.

A CPU profile attributes samples to `b_<pc>` functions with `--jit-names`;
the profiler runner used for this document attaches `Profiler.start` to
every worker target over the DevTools protocol and aggregates self time
(it lives outside the repo; the approach is a page of node).
