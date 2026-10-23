# Phase 3 handoff — where the speed is, and what to do next

Written for whoever picks this up. `docs/PLAN.md` is the project;
`docs/PHASE3-FINDINGS.md` is the evidence for everything claimed here, with
the command behind each number. This document is the plan that follows
from it: the state, the one measurement to take first, the ranked work,
the kill gates, and the traps that have already cost time.

Everything below was true on 2026-10-23 at commit `bbcaa3d` (#85), all four
recompiler differentials passing (`harness/wasm-core/run-jitdiff.sh`, every
hand-written and randomised case) and the native trace differential
matching — **except where the update below supersedes it.**

### Update, the same day: the video config

The first step of §4.1, counting the texture hashes, found the harness
itself was the cost. `bootgame.cpp` set up video with `ActivateBackend`
alone where `Core.cpp` calls `VideoBackendBase::PopulateBackendInfo`, so
`g_Config.Refresh()` and the backend's `InitBackendInfo` never ran and
every video setting was a C++ member default. Among them
`iSafeTextureCache_ColorSamples` = 0: every texture lookup hashed the
whole texture, 528 hashes and 40.8 MB a frame in gameplay. Video is now
set up as `Core.cpp` does it, plus the one line of this game's
`GameSettings/SSP.ini`; `--no-dolphin-video-config` restores the old
setup for A/B runs.

Same frames (23,249), same draws (28,874 at 390 s), and in single core
the same 280,060,345,494 executed instructions either way — the game does
identical work, only the GPU thread's cost changed. The hash went from
0.53 s per emulated second on the GPU thread to 0.01 s. §1's table is
updated; §2's profiles and §4.1 are the old setup and kept as the
record; §3's ranking is revised.

---

## 1. Where it stands

CPU-side, Null video backend, nothing drawn. Chrome 155, Apple M4.

| | at `bbcaa3d` | video config (#87) | + madd inline (§4.2) |
|---|---|---|---|
| boot, first 2.0 emulated seconds | 139–145% | 141% | 141–142% |
| 390 emulated s, title to tutorial level, pressing A, single core | 74% | 97% | **105%** |
| the gameplay window (300–390 s, character on screen, 3D scene), single core | 59% | 80% | **88%** |
| the same 390 s with the GPU on its own thread (`--dual-core`) | 88% | 109% | **119%** |
| the gameplay window, dual core | 70% | 89–90% | **99%** |

The game runs its frame loop at 59.6 frames per emulated second
throughout, so 99% is about 59 frames a wall second in the level. Single
and dual core render the same 23,249 frames and the same 28,874 draws in
the GX capture at 390 s, so the game takes the same path either way.

**The Phase 2 gate is at the line, not past it.** `docs/PLAN.md` asks for
100% with the Null backend before graphics are added. Dual core, the
gameplay window measured 99.4% — inside the ±3% runs vary by, so neither
met nor missed on one run. Single core is 88%. The CPU thread still binds
(§3).

What the recompiler does today: compiled blocks are 99.9% of executed
instructions; the hatch (an untranslated instruction calling the
interpreter's handler in place) carries the rest, 1.4% over the 390-s
dual-core run; nothing is interpreted straight-line any more (3.5 million
instructions in 390 s, down from 462 million); no block is retired.
Inline: the integer ISA including the carry forms, divides and rotates;
`lwarx`/`stwcx.`; FP loads and stores with Dolphin's exact conversions;
`fadd`/`fsub`/`fmul`/`fdiv` and their single and paired forms, `ps_muls0/1`,
`frsp`, `fcmpu`/`fcmpo` for finite operands; `mfcr`/`mtcrf`; the
quantized `psq_l`/`psq_st` family specialised on the GQR with a guard;
gather-pipe stores; the locked L1 cache at `0xE0000000`; a second tier
for code that runs with translation off. The dispatcher follows Dolphin's
slice model, skips idle loops, keeps `mtmsr` in the chain, and takes the
FP program exception on an `mtmsr` exit.

---

## 2. Where the time goes

Two profiles, both of the gameplay window, both taken with
`PROFILE=<dir> harness/wasm-core/run-jit.sh …` (see §6).

**Single core, the emulation thread, 159.8 s sampled:**

| share | what |
|---|---|
| 44% | compiled blocks — 12,385 distinct, top 10 = 6.5% of block time, top 500 = 47%: flat |
| 42% | VideoCommon running inline on the CPU thread: `GetMurmurHash3` alone 28% (the texture cache hashing every texture a draw uses), vertex loading 6%, XFB decode 2% |
| 9% | interpreter: `fma` 3.5%, `NI_madd_msub` 1.5%, `ppc_interp_op`, `RunTable4`, `ps_madds0/1` — one family, the fused multiply-adds |
| 3% | dispatcher and CoreTiming |

Half of the single-core run's guest instructions (267 billion against 127
billion in dual core, same frames) were the game spinning in its GPU-wait
loop, so the 44% overstates the recompiler's own cost.

**Dual core, both threads**, the same window, the profiler started at
the 300-s mark and sampling the last 122 s of the run (82 emulated
seconds at the window's 67% under the profiler; unprofiled it is 70%):

| | GPU thread | CPU thread |
|---|---|---|
| busy, share of the sampled wall clock | **74%** | **73%** |
| waiting for the other thread | 26% | 27% |
| busy time per emulated second | 1.11 s | 1.09 s |
| of which | VideoCommon 0.92 s: `GetMurmurHash3` **0.49 s**, `SetCPStatusFromGPU` 0.13 s, vertex loading ~0.15 s, XFB decode 0.03 s; JS glue (`js-to-wasm`, the wait path) 0.16 s | compiled blocks 0.81 s (12,451 blocks, top 500 = 46%); interpreter 0.18 s, all of it the fused multiply-adds (`fma` 0.06, `ppc_interp_op` 0.05, `NI_madd_msub` 0.03, `ps_madds0/1`); gather-pipe flush 0.04 s; dispatcher 0.04 s |

Neither thread is saturated and neither is cheap. Each needs about 1.1
seconds of work per emulated second, so even with perfect overlap the
window would run at ~90%; and a quarter of the wall clock on each side
is spent waiting for the other, which is the rest of the gap to 70%.
The hottest compiled blocks are real work, not spinning — `0x8037ab50`..
is a single-precision `lfs`/`fmuls`/`fadds`/`stfs` loop under `bdnz`,
`0x80138160` a matrix routine of `psq_l`/`ps_muls0`/`ps_madds1`,
`0x8004e45c` and `0x800570b8` function prologues — and the top ten are
6% of block time. In dual core the game's GPU waits are caught by the
idle rule and the CPU thread blocks in `SyncOnSkipIdle` rather than
spinning; the 127 billion instructions it executes over 390 s are the
game's work.

Both profiles above are the old video setup, whole-texture hashing.
**With Dolphin's video config, dual core**, the same window (profiler
from 15 s into it; the window at 80.6% under the profiler, 88–90%
without):

| | GPU thread | CPU thread |
|---|---|---|
| busy, share of the sampled wall clock | 57% | **91%** |
| waiting for the other thread | 43% | 9% |
| busy time per emulated second | 0.71 s | **1.13 s** |
| of which | VideoCommon 0.50 s: `SetCPStatusFromGPU` 0.15 s, vertex loading ~0.11 s, `RefreshPeekCache` 0.04 s, XFB decode 0.03 s, `GetMurmurHash3` ~0.01 s; JS glue (`js-to-wasm`) 0.17 s | compiled blocks 0.84 s (12,748 blocks, top 500 = 47%); interpreter 0.18 s, the fused multiply-adds as before; gather-pipe flush 0.05 s; dispatcher 0.04 s |

The CPU thread binds. It needs 1.13 s of work per emulated second, so it
cannot reach real time however fast the GPU thread gets.

---

## 3. What that decides

*Revised for the video config.* The GPU thread's cost was mostly the
harness's whole-texture hashing and is gone (§4.1). What remains, in
order:

1. **The CPU thread's emitted code and its interpreter remainder**, 0.84 +
   0.18 s per emulated second, and now the binding thread. The
   interpreter part is one instruction family with an exact inline form
   (§4.2), about 0.15 s; the compiled part is flat and needs the general
   shape of the emitted code to improve (§4.3). Getting to 1.0 s means
   taking about 0.13 s off this thread.

   *After §4.2:* 1.01 s per emulated second — compiled blocks 0.87 s,
   interpreter 0.03 s, VideoCommon's gather-pipe flush 0.05 s, dispatcher
   0.04 s; 90% busy against the GPU thread's 0.64 s. The compiled part is
   now nearly all of it, and §4.3 is the next work.
2. **The GPU thread's remaining VideoCommon work**, 0.50 s per emulated
   second, with 43% of its time spent waiting. Not on the critical path
   until the CPU thread drops below it; `SetCPStatusFromGPU` (§4.1 item 4)
   is its largest line.
3. **The hand-off** (§4.4): the CPU thread waits 9% of the time now, so
   this is small until item 1 moves.

Reproduce the measurement before acting on it — it is one profile of one
window on one machine, and the profiler costs about 10% of speed (70.4%
→ 63.0% and 88.8% → 79.0% in the window). Set `PROF_DELAY_MS` so that
sampling starts inside the window: with the video config the window
opens about 265 s into the run in dual core, not 330 s.

```sh
PROFILE=/tmp/prof-dual PROF_DELAY_MS=280000 harness/wasm-core/run-jit.sh 300000000000 \
  --jit --jit-stats --jit-names --dual-core --input=A --skylander \
  --ticks=291600000000 --speed-from=218700000000
cat /tmp/prof-dual/threads.txt
```

`threads.txt` prints each busy thread's share of the sampled wall time by
category; the `window` line in `run.txt` gives the emulated seconds. The
number to track per item is that thread's busy seconds per emulated
second: the dual-core wall number only moves when the slower thread, or
the waiting, does. Measuring a recompiler change on the dual-core speed
alone will under-report it while the GPU thread is this heavy.

---

## 4. The work, ranked

### 4.1 VideoCommon's cost per frame — items 1–3 done or moot

*Outcome.* Item 1 was done and found the premise below wrong: the
harness was not running at 128 sample points but at 0, because it never
loaded Dolphin's video config (see the update at the top). Measured with
`--jit-stats`, gameplay window: 528 hashes a frame, not ~3,000 — 430
texture lookups, 98 re-checks of bound textures, no palettes, and 112
binds reused from TMEM with no hash. At 128 samples they read 0.85 MB a
frame, most of it the one XFB a frame Dolphin always hashes in full
(`should_force_safe_hashing`), and the hash is ~1% of the GPU thread.
Items 2 (a cheaper hash) and 3 (hash less) are not worth doing at that
size. Item 4 is still open and is the GPU thread's largest line. The
original text follows as the record.

Texture hashing is half of it. Every texture a draw binds is hashed
on every use (`TextureCacheBase::LoadImpl`, `TCacheEntry::CalculateHash`,
both through `Common::GetHash64`), and the hash runs over
`GFX_SAFE_TEXTURE_CACHE_COLOR_SAMPLES` = 128 sample points, which is
already Dolphin's fastest setting. Native Dolphin uses the CPU's CRC32
instruction for this (`GetHash64_SSE42_CRC32`, `GetHash64_ARMv8_CRC32`,
`Hash.cpp:423`); the wasm build is `_M_GENERIC` and falls back to a scalar
`GetMurmurHash3`, two 64-bit multiplies and rotates per 16 bytes. That is
part of why a hash that is noise on native is a quarter of a thread
here; the count of hashes is the other part, and is not yet measured.

In order:

1. **Count it.** Hashes per frame, bytes hashed per frame, and how many are
   `LoadImpl` (texture binds) against `CalculateHash` (EFB copies). A
   counter in `GetHash64` printed with `--jit-stats` is a twenty-line
   change and decides everything below. ~1,900 draws a frame with one or
   two textures each puts the bind hashes near 3,000 a frame, 180,000 an
   emulated second.
2. **A cheaper hash.** The wasm build does not enable SIMD
   (`CMAKE_CXX_FLAGS=-pthread` in `dolphin-web/build-wasm`); wasm SIMD128
   has been in Chrome since 91. Dolphin already builds and links `xxHash`
   (`Externals/xxhash`), whose XXH3 is cheaper per byte than this
   MurmurHash in scalar code and vectorises for wasm SIMD128 through
   SIMDe (`xxhash.h:3850`). Replacing the generic `s_texture_hash_func`
   with XXH3 over the same sample pattern is a change to `Hash.cpp` alone;
   the hash value need not match native, only be stable within a run.
   Rebuilding the Dolphin libraries with `-msimd128` also speeds vertex
   loading and texture decode, and is hours of build; try the hash alone
   first.
3. **Hash less.** The texture cache's safety check exists because games
   modify textures in RAM; this game's textures are read from the disc
   into RAM once. Dolphin has no per-game "trust textures" knob, and
   inventing one changes correctness semantics — a last resort, and only
   with the GX capture's draw count proving the frames are unchanged.
4. **`SetCPStatusFromGPU`**, 0.13 s per emulated second: the command
   processor's status and interrupt bookkeeping, run by `RunGpuLoop`
   after every 32-byte gather-pipe burst it consumes (`Fifo.cpp:350`–
   `361`): some three million calls per emulated second at this game's
   FIFO rate. It is the second line on the GPU thread after the hash,
   and the kind of per-burst cost that amortises if the loop decodes
   several bursts between status updates; whether that keeps the CPU's
   view of the FIFO correct is the question to answer first.
5. **The rest of VideoCommon** — vertex loading (`VertexLoader::RunVertices`
   and the `*_ReadIndex` templates, ~0.15 s), `TexDecoder_DecodeXFB`
   (the XFB read back out of RAM on every swap) — belongs to the WebGPU
   backend's profile when there is one. The plan's backend does texture
   decode in compute shaders; vertex loading stays on the CPU.

**Kill gate:** the GPU thread's busy time per emulated second, from
`threads.txt`, before and after: 1.11 s today, 0.49 s of it the hash. If
it does not fall by most of the hash's share, the work is wrong
somewhere. If it falls and the dual-core speed does not rise, the CPU
thread (1.09 s) or the hand-off (§4.4) is now the limit, which the same
`threads.txt` shows.

*Met by the video config:* 1.17 s → 0.71 s re-measured, the hash 0.53 s
→ 0.01 s; the dual-core window rose 70% → 89–90%, and the CPU thread, at
1.13 s, is now the limit.

### 4.2 The fused multiply-adds inline

After the quantized forms and the gather pipe went inline, `ps_madds0`,
`ps_madds1`, `ps_madd` and `ps_msub` are 57% of what still goes through the
hatch (op 4, xo5 14/15/28/29 in the `--jit-stats` histogram; the histogram
keys on the ten-bit xo so each shows under several entries), 990 million
of 1.73 billion hatched instructions over 390 s, almost all in the
character skinning loop at `0x8018acc0`. The interpreter's whole 9% in the
single-core profile is this family: `fma` is musl's *software* fused
multiply-add, since wasm has no fma instruction.

The inline version does not need one. `Interpreter_FPUtils.h:285`
(`NI_madd_msub`) computes `std::fma(a, Force25Bit(c), b)` in double; when
`a` has at most 28 significant mantissa bits (always, when it holds a
single — every paired-single result is `ForceSingle`d), the product
`a × c_round` has at most 53 bits and is exact in double, so `f64.mul`
then `f64.add` rounds exactly once, identically to the fma. The tie
correction that follows (`(bits & 0x1FFFFFFF) == 0x10000000`, then an
error-free 2Sum whose one fma is `p + a'` with `p` exact, then nudge the
result by one ulp in the direction of the error) is a dozen `f64`
operations that translate one for one. So the common case is: finite
operands, `a` float-exact (`(bits_a & 0x1FFFFFFF) == 0` after the existing
`is_special` tests) → inline multiply, add, tie check, `ForceSingle`,
FPRF from ps0; anything else → `EmitHatchCall`, exactly as `EmitFpBinary`
(`block.h:2628`) already does for `fmul`/`fadd`. The wrapper to match is
`Interpreter::ps_madds0` (`Interpreter_Paired.cpp:418`): both halves
through `NI_madd<true>`, `ForceSingle` each, `SetBoth`, `UpdateFPRFSingle(ps0)`.
The double-precision forms (op 63 `fmadd` family) need `bits(a) + bits(c)
≤ 53` for the same trick and are not in the histogram's top twelve; leave
them on the hatch.

Add the six paired forms to `jitdiff.cpp`'s template pools with FP state
seeded both float-exact and not, so the guard's fallback is exercised;
the per-instruction, block and chain differentials then cover it. Then
`fctiwz` (op 63 xo 15, 3.5% of the hatch) and the `stw` that still hatch
(op 36, 2.8% — find out where with the retirement-style diagnostic before
assuming they are gather-pipe fills).

**Kill gate:** the hatch share of executed instructions in the 390-s
dual-core run, 1.37% today, and the interpreter category of the CPU
thread in `threads.txt`, 0.18 s per emulated second today. Expect the
hatch under 0.5% and the category under 0.03 s. Measure on the CPU
thread's busy time per emulated second, not on the dual-core wall number
(§3).

*Outcome.* Done as described, `EmitPsMadd` in `block.h`, off with
`--jit-no-madd`; one refinement to the argument above — the product is
exact only if it also stays clear of overflow and the subnormal range,
so the guard bounds the exponents of `a` and `c` to 2^±200 as well. The
hatch went 1.37% → 0.35% of executed instructions (met); the interpreter
category 0.186 s → 0.034 s per emulated second (just over the 0.03 s
expected; what remains is `fctiwz`, `stw` and `psq_st`, 14%, 11% and 8%
of a hatch a quarter of its old size). CPU thread 1.12 s → 1.01 s per
emulated second; the gameplay window 88.9% → 99.4% dual core, 80.3% →
88.3% single core, which executed the same 280,060,345,494 instructions
with and without it.

The differential needed three repairs to test it, all now in: op 4's
xo 12, 14 and 15 are A-forms (they were generated with frC = 0, so
`ps_muls0` and `ps_madds0` had never seen another frC); the "singles" in
the FP seed were not singles; and directed cases now put a product
exactly on a float rounding tie, which random operands reach about once
in 2^25. Swapping the tie correction's direction, or removing it, each
fails the differential at once.

### 4.3 Code quality in the emitted blocks

Compiled blocks are 0.81 s per emulated second on the CPU thread for
about 330 million guest instructions — 2.4 ns each, four times the
0.59 ns the boot's hot loop measured in isolation. The profile is flat
(the top 500 of 12,451 blocks are 46% of block time), so the gain has to
come from the general shape of every block, not a hot-loop special case.
The hand-written variant of the boot's hot loop ran 2.8× faster than
`block.h`'s output for the same guest code (`docs/PHASE3-FINDINGS.md`
§1); real blocks are heavier in loads, stores and branches, so expect
less — but the CPU thread cannot reach real time without some of it.

Three changes, in the order of least risk to the differential:

1. **Compare and CR0 as Dolphin's JIT does it.** `EmitCompareToCR`
   (`block.h:840`) is ~30 wasm instructions: three compares, XER[SO],
   and a load from `s_crTable`. Dolphin's own representation
   (`ConditionRegister.h:30`) is designed so that a compare stores the
   sign-extended 64-bit difference directly — `Jit64::cmpXX`
   (`Jit_Integer.cpp:525`) is `MOVSX; SUB; store`, four instructions,
   and the SO bit (bit 59) comes along from XER. The randomised
   differential compares the raw 64-bit field words today, so it will
   flag a different-but-equivalent encoding as a mismatch; change it to
   compare the *decoded* four bits (`GetCRField`) first, which is the
   right invariant anyway and also lets `mtcrf` stop re-encoding the
   fields its mask leaves alone. Compare-and-branch pairs are a third of
   the instruction stream; this is the cheapest of the three.
2. **Guest registers in wasm locals across a block.** Every GPR access is
   an `i32.load`/`i32.store` against the state struct. Caching the GPRs a
   block touches in locals and writing back at every exit — the epilogue,
   every bail, and *before every hatch call*, since the interpreter reads
   the struct (and reloading after, since it may write it) — is the
   register-allocation step every JIT takes. The hatch makes it less of a
   win in FP-heavy code than in integer code, which is one more reason
   §4.2 comes first.
3. **A real `loop` for a self-targeting back edge.** A conditional branch
   back to the block's own start is today a direct self tail-call: a call,
   a prologue, the downcount check. A wasm `loop` with the downcount
   decrement on the back edge keeps the registers in locals across
   iterations and is where the hand-written variant's 2.8× came from.

**Kill gate for each:** the boot window (`--ticks=1458000000`, 141–145%
today) and the CPU thread's compiled-blocks seconds per emulated second
in the dual-core gameplay window (0.81 s today). Under 5% on both is not
a result (runs vary ±3%); keep the flag that turns it off
(`--jit-no-…`), as every other change has.

### 4.4 The CPU–GPU hand-off

Each thread spends about a quarter of the wall clock waiting for the
other: 0.4 s per emulated second each side. The CPU waits in Dolphin's
`SyncOnSkipIdle` — the CPU thread blocks on the GPU thread at every
skipped idle loop; `--no-sync-idle` turns it off and the game drops
frames, so it is load-bearing — and the GPU waits when the FIFO runs
dry. The two threads take turns more than they overlap.

Dolphin's knobs for this are the `SyncGPU` family (`MAIN_SYNC_GPU`,
`MAIN_SYNC_GPU_MAX_DISTANCE` = 200,000 ticks, `MIN_DISTANCE`,
`OVERCLOCK`; `Fifo.cpp:329`–`385`), off by default, which bound how far
the CPU may run ahead of the GPU in emulated ticks instead of stopping
it at every idle loop. Worth measuring on, with the frame count and the
GX capture's draw count checked against the default run. The timing
question underneath is that the Null backend is instant in emulated
time and a real one is not; a WebGPU backend will change this balance
again.

Single core has a related cost of its own: the game's GPU-wait loop —
`bl OSDisableInterrupts; lbz; bl OSEnableInterrupts; cmpwi; bne` — calls
a function, so the busy-wait rule cannot see through it and it spins:
half of the single-core run's guest instructions. Recognising it needs
the idle rule to follow `bl` into a leaf (`trace_uncond` exists for the
tracer) and to admit the `mfmsr`/`rlwinm`/`mtmsr` shape when only EE
changes. In dual core the idle rule already catches the waits that
matter, so this is a single-core item.

### 4.5 Then Phase 3 proper: the WebGPU backend

Once the GPU thread's CPU-side cost is understood, the backend is the
project's next phase as planned (`docs/PLAN.md` Phase 3). What exists for
it: `spikes/webgpu/` (Spike B: every WebGPU feature the game needs
present in Chrome 155, the title screen's geometry replayed, the three
missing pieces named — XF transform, textures, TEV), the GX decoder and
its tests (`harness/src/decode_capture.py`, `test_decode.py`), captures
of the title and gameplay with their reference renders
(`harness/reference/`), `--capture-gx` in the harness, and the knowledge
from the 390-s run that gameplay is ~1,900 draws a frame, 1–2 TEV
stages, no logic ops. `VideoBackends/Metal` is the structural template
(`docs/PLAN.md` Bet 03). The `web/` directory is empty: there is no
browser shell yet, and the harness's `bootgame.cpp` is the only thing
that runs the game in a browser.

### Smaller, known, not urgent

- **No block invalidation.** `icbi` invalidates nothing; a BAT write goes
  through the hatch and the region tables, gather-pipe address and
  locked-cache mapping do not follow. The game sets its BATs once, at OS
  init. Any game that loads code into reused RAM would break this.
- **The hatch's inline footprint**: every load and store carries cold
  paths for the hardware registers, the gather pipe and the locked cache.
  Measured as noise in the boot window.
- **No bounds check on the interpreter's MMIO table**: a guest store to
  `0xCC80xxxx` would trap the emulator. The hatch takes only the three
  blocks the interpreter decodes, so it cannot happen from compiled code
  today; the randomised differential keeps its addresses out of that
  range on purpose.
- **The nightly workflow** (`.github/workflows/nightly.yml`) builds
  Dolphin's libraries and runs the differentials on a hosted runner. It
  has passed twice; it does not run bootgame, so every speed number in
  the repo is measured by hand on one machine.

---

## 5. Landmines

Each of these cost at least an hour this phase.

- **Measure at a fixed emulated time** (`--ticks=N`, 729 million per
  second), never a fixed instruction count. Idle skipping removes
  instructions; a lower count is not slower. Compare "% of real time".
- **Runs vary ±3%.** A difference inside that is not a result. Nothing
  else may run while measuring: the node differential on the same machine
  inflated runs by 15% and read as a regression.
- **Check `frames` and the GX capture's draw count before comparing
  speeds.** A variant that changes what the game does (`--no-sync-idle`:
  half the frames) is not faster, it is different. `--dual-core` is not
  deterministic, so it is for speed, never for a differential.
- **Single-core instruction counts are not the game's CPU work.** Half of
  them were GPU-wait spinning. The game does about 360 million
  instructions per emulated second in gameplay.
- **The runner's `t=` counter is a poll count, not a clock.** The harness
  buffers output until the end; trust its `speed` line.
- **`/tmp/ssa.iso` gets purged.** Regenerate from the RVZ in
  `skylanders-rom/` (never committed) in eight seconds:
  `dolphin-web/build-native/Binaries/dolphin-tool convert -u /tmp/dolphin-convert -i "<rvz>" -o /tmp/ssa.iso -f iso`.
  A run that stalls at `DiscIO::CreateDisc` with `http: status=0` is this.
- **The speed throttle.** `MAIN_EMULATION_SPEED` must be 0 or the harness
  sits at exactly 100% with `CoreTiming::Throttle` sleeping in the
  profile. `bootgame.cpp` sets it.
- **`Fifo::EmulatorState(true)` after `Prepare()`** or the GPU thread
  sleeps forever: 0 frames, FIFO stuck at 2,336 bytes, emulated time
  racing.
- **Use the retirement list before blaming the hatch.** `--jit-stats`
  prints `retire pc … ins … ea …` for every block the dispatcher gave up
  on. The hatch cost about 5 ns a call; what cost 450 million interpreted
  instructions was a bail per iteration of the skinning loop, visible
  only there. The randomised differential cannot find a wrong MMIO mask
  because it keeps its addresses away from MMIO.
- **An `mtmsr` exit must take the FP program exception**
  (`CheckFPExceptions` after `MSRUpdated`), in the dispatcher and in
  every test fix-up that mirrors it. The chain differential found this
  only once FPSCR became seeded state; keep FPSCR in the seed.
- **The compares OR into FPRF, and `mtcrf` re-encodes every field.** Both
  are interpreter quirks matched on purpose; "fixing" either breaks the
  differential.
- **Build with `harness/wasm-core/link.sh`**, not `link-jit.sh` (no Dolphin
  includes). `compile_region` must stay inside the Emscripten guard or the
  native build breaks.
- **Check the video config before profiling VideoCommon.** The harness
  sets Dolphin's subsystems up by hand, and a step `Core.cpp` takes that
  the harness skips leaves its settings at C++ member defaults without a
  word of warning: the missing `PopulateBackendInfo` was 0.53 s of the GPU
  thread per emulated second. The `video` line at startup prints the
  settings that matter; anything else Dolphin sets up from its config is
  worth the same suspicion.
- **The game's GameSettings INI is not loaded.** The browser build has no
  Sys directory; `SSP.ini`'s one line is set by hand in `bootgame.cpp`. A
  different game, or a Dolphin update that changes `SSP.ini`, needs that
  line revisited.
- **`ninja` in `dolphin-web/build-wasm` fails re-running CMake** ("The
  selection for BZip2 changed from 'Bundled' to 'System'") since
  emsdk's sysroot gained a `libbz2`. For a one-file change, take the
  compile line from `ninja -t commands Source/Core/<Lib>/lib<lib>.a`, run
  it under `emsdk_env.sh`, and `emar r` the object into the archive; a
  real reconfigure wants `-DUSE_SYSTEM_BZIP2=OFF`.
- **The differential's memory: two traps.** Node 22 keeps alive every
  wasm instance that imports the function table, with a structure sized
  to the table (6,000 instances, 10,000 entries: 1.38 GB after `gc()`);
  reusing a table slot does not free them. The per-instruction
  differential therefore compiles with linking off, which drops the
  import; add templates there freely, but a section that installs
  thousands of LINKED modules will hit the worker's limit, which reports
  as "Worker terminated due to reaching memory limit" mid-run. And the
  block differential restores and compares a megabyte above each
  scratch window, not just the window: a store outside what is restored
  survives from the compiled run into the interpreter's and reads back as
  a false mismatch.
- **Commits need all five numbered sections** or CI rejects the PR.
  `docs/COMMIT_CONVENTION.md`; the template is `.gitmessage`.

---

## 6. How to measure

```sh
harness/wasm-core/run-jitdiff.sh                                   # the four differentials, ~1 min
harness/wasm-core/run-jit.sh 2000000000 --jit --ticks=1458000000   # the boot number: 2.0 emulated s
harness/wasm-core/run-jit.sh 2000000000 --jit --jit-stats --ticks=1458000000   # with the breakdowns
harness/wasm-core/run-boot.sh /tmp/ssa.iso 20000                   # native trace differential

# gameplay: 390 emulated s pressing A, speed measured again from 300 s,
# the GX stream captured at 390 s; ~7.5 min of wall clock dual-core
harness/wasm-core/run-jit.sh 300000000000 --jit --jit-stats --dual-core --input=A --skylander \
  --ticks=291600000000 --speed-from=218700000000 --capture-gx=3000000 --gx-from=284310000000
python3 -I harness/src/decode_capture.py <that run's output>        # what the GX stream draws

# the same run under the profiler, sampling from inside the window: it
# opens ~260 s into the run in dual core and ~290 s in single core, so
# 280000 and 305000; re-derive them (total ms minus window ms) whenever
# the speed changes, or the profile covers the wrong stretch
PROFILE=/tmp/prof PROF_DELAY_MS=280000 harness/wasm-core/run-jit.sh 300000000000 \
  --jit --jit-stats --jit-names --dual-core --input=A --skylander \
  --ticks=291600000000 --speed-from=218700000000
cat /tmp/prof/threads.txt                                           # each busy thread by category
node harness/wasm-core/profile-threads.mjs /tmp/prof                # re-aggregate without re-running
harness/wasm-core/run-jit.sh 800000000 --jit --disasm=8037ac20,12   # read a hot block from the profile
```

**The recompiler's own number in gameplay** is the CPU thread's busy
seconds per emulated second, by category, from `threads.txt` and the
window's emulated seconds on the `window` line (1.01 s today: compiled
0.87, interpreter 0.03, the rest 0.11). Busy seconds per emulated second
is the thread's busy share divided by the window's speed under the
profiler. Report recompiler changes against
that; the dual-core wall number moves only when the slower thread or the
waiting does.

A/B flags: `--jit-no-idle`, `--jit-no-mtmsr`, `--jit-no-hatch`,
`--jit-no-phys`, `--jit-no-gp`, `--jit-no-psq`, `--jit-no-l1`,
`--jit-threshold=1`, `--jit-no-link`, `--no-sync-idle`, `--dual-core`,
`--jit-no-madd` (the paired multiply-adds through the hatch),
`--no-dolphin-video-config` (video as it was set up before: member
defaults, whole-texture hashing), `--tex-samples=N` (hash samples alone;
0 = every byte).
Input: `--input=A`, `--skylander`, `--pointer=X,Y`, `--period-ms`,
`--press-ms`, `--no-nunchuk` (the game will not leave its first screen
without a Nunchuk). Diagnostics: `--jit-stats` (with the `texhash`
lines: the texture cache's hashes by call site), `--jit-names`,
`--disasm=ADDR,N`, `--xfb-every=T`, `--disc-files`, `--capture-gx=N`,
`--gx-from=T`, `--speed-from=T`.

Working rules that held: a flag to turn every change off; the
differentials before every push; a number for every claim, measured the
same way as the number it is compared with; when a measurement
disagrees with a document, find out which is wrong before building on
either.
