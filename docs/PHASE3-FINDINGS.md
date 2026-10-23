# Phase 3 — what was actually slow, and what moved it

An independent investigation of why the browser build did not run at full
speed, compared against the plan in `docs/PHASE3-DENSITY.md`, and the work
that followed. Written so the next reader can check every claim against a
command.

Headline, same harness, same machine (Chrome 155, Apple M4):

| | wall time | real-time speed |
|---|---|---|
| start of this work (#82): 800M instructions, 193.8 MIPS | 4127 ms for 1.10 emulated s | 26.6% |
| #83: the first 2.0 emulated seconds of the boot | 1391–1427 ms | **140–144%** |
| #83, idle loops not skipped (796 MIPS) | 1847 ms | 108% |
| #83: 20 emulated seconds, input and a figure on the portal | 6865 ms | 291% |
| now: the first 2.0 emulated seconds of the boot | 1380–1417 ms | **141–145%** |
| now, single core: 390 emulated seconds, title to tutorial level, pressing A | 527 s | 74% |
| now, single core: the gameplay window, 300–390 emulated s, character on screen, 3D scene | 152.6 s for 90 s | 59% |
| now, dual core (`--dual-core`, the GPU on its own thread as in Dolphin): 390 emulated seconds | 444 s | **88%** |
| now, dual core: the gameplay window | 127.8 s for 90 s | **70%** |

The gameplay rows are the facts about gameplay speed, measured CPU-side
with the Null video backend (nothing is drawn): the game runs its 60 Hz
frame loop at 59.6 frames per emulated second throughout, so 70% of real
time is about 42 frames a wall second in the tutorial level. Single core
and dual core produce the same frame count and the same 28,874 draws in
the GX capture at 390 s -- the game takes the same path. Over the same
390 seconds at the start of the gameplay work the single-core numbers
were 63% and 50%: the hatch share fell from 2.17% of executed
instructions to 0.62%, the instructions interpreted one at a time from
462 million to 3.5 million, and the blocks retired from 47 to none.

The dual-core run executes HALF the guest instructions of the single-core
one for the same 390 emulated seconds and the same frames (127 billion
against 267 billion). The difference is the game spinning in its
GPU-wait loops -- `bl OSDisableInterrupts; lbz; bl OSEnableInterrupts;
cmpwi; bne`, which the busy-wait rule cannot see through -- while the
single-core run did the GPU's work inline between those spins. The CPU
work the game actually does in gameplay is about 360 million instructions
per emulated second, not the billion the single-core count suggested.

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

The gameplay work that followed, measured over the same 390 emulated
seconds (title to tutorial level, pressing A) and its last 90 seconds:

| step | whole run | gameplay window | notes |
|---|---|---|---|
| a Nunchuk on the Wii Remote, presses by emulated tick, the disc-file timeline, a GX inventory at 390 s; `fadd`/`fsub`/`fmul`/`fdiv`, their single and paired forms, `ps_muls0/1`, `frsp` inline for the common case | 63.0% | 50.2% | 5.8B instructions through the hatch (2.17%); `psq_l` 29%, `psq_st` 14%, `stb`/`stw` 14% of them |
| `psq_l`/`psq_st` and their six variants specialised on the GQR and guarded; gather-pipe stores inline when they do not fill the buffer | 64.9% | 52.8% | both gone from the histogram; the gain was small, which said the hatch was not the cost |
| the retirement list: the skinning loop's loads and stores at `0xE0000000` bailed on every iteration; the locked L1 cache translated; the `0xCC00`/`0xCD00` MMIO mask fixed; `mfcr`/`mtcrf`/`fcmpu`/`fcmpo` inline; the FP program exception after an `mtmsr` exit | **74.0%** | **59.0%** | interpreted singly 462M → 3.5M, bails 226M → 1.2M, retired 47 → 0; hatch 1.7B (0.62%), 55% of it the fused multiply-adds |
| a CPU profile of the window: 44% compiled blocks, 42% VideoCommon on the emulation thread (texture hashing 28%), 9% interpreter; `--dual-core` runs the GPU on its own thread as Core.cpp does (`EmulatorState(true)` is the line without which the loop sleeps) | **87.9%** | **70.4%** | same frames and draws; guest instructions 267B → 127B, the GPU-wait spinning gone |
| `--dual-core --no-sync-idle`: the CPU not waiting for the GPU thread at each skipped idle loop (Dolphin's "Sync on skip idle" off) | 251% | 275% | **not comparable**: 11,754 frames instead of 23,249 and 15.9 per emulated second in the window -- the GPU falls behind and the game drops frames. Different work, so not a speed |
| texture-hash counters by call site (`texhash` with `--jit-stats`), which found every hash unsampled: video set up as `Core.cpp` does it (`PopulateBackendInfo`: `g_Config.Refresh()`, `InitBackendInfo`, `VerifyValidity`) plus `SSP.ini`'s line, instead of `ActivateBackend` alone; dual core | **108.8%** | **89.9%** | hashes 528 a frame, 40.8 MB → 0.85 MB read; GPU thread 1.17 → 0.71 s and the hash 0.53 → 0.01 s per emulated second; same frames and draws. `--tex-samples=128` alone: 108.4% / 88.8% |
| the same, single core | **97.2%** | **80.3%** | against 73.5% / 58.5% with `--no-dolphin-video-config` in the same session; **280,060,345,494 executed instructions both ways**, same hatch count, frames and draws: identical guest work |

The video config, in more detail. The harness never called
`g_Config.Refresh()`, so every `VideoConfig` field was its C++ member
default; about thirty differ from Dolphin's (among them hash samples 0
against 128, skip EFB/XFB copy to RAM, deferred EFB copies, fast depth,
EFB scale) and `g_backend_info` was empty of what Null reports (geometry
shaders, bounding box, dual-source blend). Of those only the hash samples
changed the cost: `--tex-samples=128` alone measured the same as the
whole setup. The skip-copy settings change nothing on Null, which has no
copy to VRAM, so every EFB and XFB copy goes to RAM in native Dolphin too.
The hashes that remain at 128 samples are mostly the one XFB a frame that
Dolphin always hashes in full (`should_force_safe_hashing`).

The GX capture of the full-setup dual-core run in the A/B session began
mid-draw (`9f 00 04` and its vertices), and the decoder lost sync at byte
0. Decoded from its first register write it matched the default run's
register writes exactly, 28,873 draws plus the one cut off: a
capture-timing artifact. The profiler cost about 10% of speed in these
runs (70.4% → 63.0% in the window), more than the 5% assumed before.

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
else is zero and takes the miss path, which in order tries the locked L1
cache (its own compare: 256 KB is too small for a 32 MB granule), the
gather pipe (for a store), the hardware registers (through the hatch) and
finally bails to the interpreter. One load replaced one compare on the
hot path; the cold path grew, and only runs for addresses outside RAM.

**The tier-up threshold** interprets a straight-line run from a cold PC up
to the first taken branch, and only the start PC gets a cache entry.
Installing a module costs ~24 µs; interpreting a cold block costs a few
hundred nanoseconds.

**FP correctness is the interpreter's.** `lfs`/`stfs` use Dolphin's
`ConvertToDouble`/`ConvertToSingle` reimplemented bit for bit (including
the denormal and NaN branches, which `f64.promote_f32` would get wrong).
`fadd`/`fsub`/`fmul`/`fdiv`, their single and paired forms, `ps_muls0/1`
and `frsp` are inline for the common case -- finite operands, a finite
result -- with the interpreter's own rounding rules (`Force25Bit` on the
multiplicand, `ForceSingle`'s NI flush, the FPRF class codes); anything
that would set an exception bit is detected before any write and the
whole instruction runs through the hatch instead. `fcmpu`/`fcmpo` are
inline except with a NaN operand. Everything else -- the fused
multiply-adds above all, whose single-precision rounding is a software
routine in Dolphin -- still goes through the hatch, so its semantics are
exactly `Interpreter_FloatingPoint.cpp`'s. Two interpreter quirks are
matched on purpose: the compares OR their result into FPRF rather than
replacing the FPCC bits (`fpscr.FPRF & ~FPCC_MASK` clears nothing, FPRF
being a five-bit field), and `mtcrf` re-encodes every CR field through
`s_crTable`, not only the ones its mask selects.

**Quantized loads and stores are specialised on the GQR.** `psq_l`/
`psq_st` and their indexed and update forms are compiled for the GQR
value seen at compile time -- the type decides the load width and sign,
the scale becomes one `f32.const` -- and guarded: the block compares the
live register with that value and runs the instruction through the hatch
when they differ. Dolphin's own JIT does the same. Over 390 emulated
seconds of gameplay the guard never fell back enough to register in the
hatch histogram.

**The gather pipe is written inline** when the store does not fill the
32-byte buffer: the bytes go to `gather_pipe_ptr` in guest order and the
pointer advances. The store that would fill it takes the hatch, so the
flush (`UpdateGatherPipe`, the FIFO, the command processor) stays the
interpreter's and both paths meet at the same buffer state. The guest
address is compared, so this is enabled only when the DBATs map
`0xCC008000` to the pipe (`MMU::IsOptimizableGatherPipeWrite`), which the
Wii's standard BATs do.

**The locked L1 cache has a translation.** The character skinning loop
(`0x8018acc0`..) streams its vertices through the locked cache at
`0xE0000000`, and every one of its quantized loads and stores bailed:
450 million instructions interpreted one at a time over 390 emulated
seconds, 99% of everything still interpreted, found through a retirement
list that prints the refusing instruction and its address. Addresses in
`[0xE0000000, 0xE0040000)` now translate to Dolphin's locked-cache buffer,
enabled per compile while the DBATs map the range
(`MMU::IsOptimizableRAMAddress`), since the game's OS init sets that BAT
long after the harness starts.

**An `mtmsr` exit takes the FP program exception.** `Interpreter::mtmsr`
follows the write with `CheckFPExceptions`: an exception already
summarised in FPSCR[FEX] is raised the moment FE0 or FE1 enables it. The
dispatcher's fix-up after a marked exit did `MSRUpdated` and
`CheckExceptions` and skipped that step. The chain differential found it
once FPSCR became part of its seeded state.

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
and the randomised differential found both. (The mask that merged
`0xCC00` and `0xCD00` cleared the wrong bit until the retirement list
showed `lwzx`/`stwx` at `0xCD00002E` interpreted a million times; the
randomised differential keeps its addresses out of that range on purpose
and could not have seen it.)

**Translation off has its own tier.** The block cache, link table and
region table come in two sets, chosen by MSR[DR] at every dispatch. The
physical tier's region table maps `0x00000000` and `0x10000000`; a chain
cannot change DR without leaving, so a chain is one tier or the other.

The last profile, over the first 2 emulated seconds: compiled blocks 55%
of the run, the dispatcher loop 18%, module compilation 5%, disc reads
over XHR 4%, the interpreter under 3%. What is left is the emitted code.

**How the gameplay facts were gathered.** The harness attaches a Nunchuk
(the game will not leave its first screen without one) and schedules
presses by emulated tick; pressing A alone takes the game from the title
through the tutorial level with the character on screen. Every disc file
is printed as it is first read, with the emulated time: `level/
Level_027_Tutorial.arc` opens at 104 s and `character/000_AirDragon.arc`
at 171 s, so the window from 300 s is gameplay, not a menu. The GX
command stream captured at 390 s decodes to ~1,900 draws a frame with
normals and texture coordinates, 415k vertices per 3 MB -- a 3D scene,
while the menus before it are triangle fans with colour and one texture
coordinate. Frames are counted at XFB flips: 59.6 per emulated second
throughout. All speeds are CPU-side with the Null video backend; nothing
is drawn.

---

## 5. Known gaps, ranked by what they cost now

A CPU profile of the gameplay window (profiler started after 300
emulated seconds, 159 s of samples on the emulation thread) splits the
time as follows:

| share | what |
|---|---|
| 44% | compiled blocks -- 12,385 distinct blocks sampled, the top 10 only 6.5% of block time, the top 500 47%: a flat profile with no single hot loop |
| 42% | **VideoCommon on the CPU thread**: `GetMurmurHash3` alone 28% (the texture cache hashing every texture a draw uses, from `TextureCacheBase::LoadImpl` and `TCacheEntry::CalculateHash`), vertex loading 6%, XFB decode and the rest |
| 9% | interpreter: the fused multiply-adds through the hatch (`fma`, `NI_madd_msub`, `ps_madds0/1`) |
| 3% | the dispatcher and CoreTiming |

What held the single-core number at 59% was the Null backend's command
processing on the emulation thread, there because the harness was
configured single-core for determinism against the native oracle.
Dolphin's own answer is its dual-core mode, a GPU thread; `--dual-core`
in the harness runs it the way Core.cpp does, and the window went to
70%. The compiled-block share above is not all useful work either: half
of the single-core run's guest instructions were the game's GPU-wait
loops spinning (see the headline), and in dual core those spins are how
the CPU thread waits for the GPU thread. The recompiler's own cost in the
window is therefore well under half the wall time; the GPU thread's
work (0.76 s per emulated second, texture hashing above all) and the
frame-by-frame hand-off between the two threads are what the 70% is
made of.

The same window profiled under `--dual-core`, both threads, sampling the
last 122 s of the run (82 emulated seconds; the profiler costs ~5%, the
window ran at 67% instead of 70%):

| | GPU thread | CPU thread |
|---|---|---|
| busy, share of the sampled wall clock | 74% | 73% |
| waiting for the other thread | 26% | 27% |
| busy time per emulated second | 1.11 s | 1.09 s |
| the lines | VideoCommon 0.92 s (`GetMurmurHash3` 0.49, `SetCPStatusFromGPU` 0.13, vertex loading ~0.15, XFB decode 0.03), JS glue on the wait path 0.16 | compiled blocks 0.81 s (12,451 sampled, top 500 = 46%), interpreter 0.18 s (all the fused multiply-adds), gather-pipe flush 0.04, dispatcher 0.04 |

Neither thread is saturated and neither is under real time: each needs
~1.1 s of work per emulated second, which caps a perfectly overlapped
run near 90%, and a quarter of the wall clock on each side is spent
waiting for the other. The hottest compiled blocks are real work (a
single-precision `lfs`/`fmuls`/`fadds`/`stfs` loop at `0x8037ab50`, a
`psq_l`/`ps_muls0`/`ps_madds1` matrix routine at `0x80138160`), not the
GPU-wait spin, which the idle rule catches in dual core. The plan that
follows from this is `docs/PHASE3-HANDOFF.md`; the ranking below is the
one it refines.

1. **VideoCommon's CPU-side cost**, 42% of the emulation thread in
   single-core gameplay and 0.92 s per emulated second of the GPU thread
   in dual core, texture hashing above all. A GPU thread takes it off the
   CPU's critical path but does not make it cheaper, and a WebGPU backend
   will carry the same vertex loading and texture cache. Worth its own
   profile once there is a backend to profile it against.
2. **Code quality in the emitted blocks** -- registers in locals across a
   block, real `loop` constructs for self-targeting back edges, the
   compare-and-branch pairs that are a third of the stream. The
   hand-written hot-loop variant showed 2.8× headroom on that shape. The
   gameplay profile is flat (the top 500 of 12,385 blocks are 47% of
   block time), so the gain has to come from the general shape of the
   emitted code, not from a hot-loop special case.
3. **The fused multiply-adds through the hatch.** After the quantized
   forms and the gather pipe went inline, `ps_madds0`/`ps_madds1`/
   `ps_madd` are 40% of what remains in the histogram, in the skinning
   loop above all. Dolphin's `NI_madd_msub` is a software routine for the
   single-precision rounding; an inline common case needs care and the
   differential is ready for it.
4. **Compare and CR0 encoding.** `cmpwi` is ~30 wasm instructions through
   `s_crTable`; Dolphin's own JIT stores the sign-extended difference.
   Adopting that needs the differential to compare decoded CR fields
   rather than raw words, which is the right invariant anyway -- and would
   also let `mtcrf` stop re-encoding the fields its mask leaves alone.
5. **The GPU-wait loop that calls a function.** The game waits for the
   GPU in `bl OSDisableInterrupts; lbz; bl OSEnableInterrupts; cmpwi;
   bne`, which the busy-wait rule cannot see through, and that loop was
   HALF the guest instructions of the single-core run (267 billion
   against 127 billion dual-core, same frames). Dolphin cannot see
   through it either. In dual core it is the CPU thread's way of waiting
   for the GPU thread, so it costs host CPU rather than wall time; in
   single core it was a third of the wall time spent running a loop
   whose outcome the inline GPU work had already decided. With calls
   followed (`trace_uncond`) and `mfmsr`/`mtmsr` admitted when only EE
   changes, the rule could recognise it; what the dispatcher should do
   with a recognised GPU wait is a timing-model question (the Null
   backend is instant in emulated time, a real one is not).
6. **No block invalidation**, as before. `icbi` is interpreted and
   invalidates nothing of ours. `mtspr` to a BAT goes through the hatch
   and the region tables, the gather-pipe address and the locked-cache
   mapping do not follow it; the game sets its BATs once, at OS init.
7. **The hatch's inline footprint.** Every load and store carries a cold
   path for the hardware registers, the gather pipe and now the locked
   cache; it runs only for addresses outside RAM, but it is code the
   browser compiles. The boot window is unchanged by it within noise.
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
# gameplay: 400 emulated s pressing A, speed measured from 300 s, the GX
# stream captured at 390 s, every disc file read printed as it is opened
harness/wasm-core/run-jit.sh 300000000000 --jit --jit-stats --input=A --skylander \
  --ticks=291600000000 --speed-from=218700000000 --capture-gx=3000000 --gx-from=284310000000
python3 harness/src/decode_capture.py <that run's output>              # what the GX stream draws
```

Flags that turn each change off for an A/B: `--jit-no-idle`,
`--jit-no-mtmsr`, `--jit-no-hatch`, `--jit-no-phys`, `--jit-no-gp`,
`--jit-no-psq`, `--jit-no-l1`, `--jit-threshold=1`, `--jit-no-link`.
`--dual-core` runs the GPU on its own thread (Dolphin's dual-core mode);
the run is then not deterministic, so it is for speed, not for a
differential against the oracle. Check `frames` and the GX capture's
draw count against the single-core run before comparing speeds: a
variant that changes what the game does (`--no-sync-idle` halves the
frame count) is not faster, it is different. The first argument
is a cap on instructions; `--ticks=N` stops at N emulated ticks (729
million per second), which is the comparison to use. `--speed-from=T`
opens a second speed window at tick T, so a gameplay phase can be timed
on its own; `--xfb-every=T` prints a thumbnail of the frame every T ticks;
`--pointer=X,Y`, `--period-ms`, `--press-ms` shape the input; a run
without `--no-nunchuk` has a Nunchuk on the Wii Remote, which the game
insists on before it leaves its first screen.

Measurements vary by about ±3% run to run; a difference inside that is
not a result. Do not run the differential (node) on the same machine
while measuring: it inflated runs by 15% and read as a regression. Runs
past the boot with input are not comparable across builds at a fixed
instruction count: presses are scheduled by instruction count, so timing
changes what the game does.

A CPU profile attributes samples to `b_<pc>` functions with `--jit-names`.
`PROFILE=<dir> harness/wasm-core/run-jit.sh …` runs the same command under
V8's sampling profiler, attached to every worker over the DevTools
protocol (`profile.mjs`), and writes the run's output, the raw profiles,
a self-time summary and `threads.txt` -- each busy thread by category,
which under `--dual-core` is the CPU thread and the GPU thread side by
side (`profile-threads.mjs`). `PROF_DELAY_MS=N` starts sampling N ms
after stepping begins, so a late phase can be profiled on its own.
