# Handoff: fights at 100%

Written 2026-10-30, after #105 merged (main 49bd02a). For the next agent
picking up Skylanders speed. Read `docs/PHASE3-WEBGPU.md` (the backend,
every milestone's evidence) and `docs/PHASE3-HANDOFF.md` §5–§6 (landmines,
how to measure) as well; this file is only what is new.

## 1. The goal

The player's fights run 60–70% on the real page, with drops to 0–20%.
The goal is 100% in fights on their laptop (macOS, Chrome 156, the page
from `web/build.sh && node web/serve.mjs /tmp/ssa.iso 8080`).

The player's rule for every change: **one at a time, its own PR, not
merged until it is right, passes CI and has a measured positive
effect.** Ask before merging.

## 2. What a play session showed

The page now sends its log and a line per second to the server
(this branch: `web/shell.js`, `harness_compiles` / `harness_compile_ms`
in `bootgame.cpp`). Lines look like:

```
2026-10-30T06:40:44.091Z emulated 2491.4 s readout 0% compiled 21 in 1058 ms
2026-10-30T06:39:11.014Z log   video     backend up in 1845 ms
```

The server writes them to `$READOUT_LOG` (default
`$TMPDIR/skylanders-readout.log`). A 17-minute session of the player's
(sheep level, Level_021) had **two separate problems**:

1. **Drops to 0–40%: shaders built mid-fight.** All 24 seconds under 50%
   were seconds spent compiling: up to 24 new shaders/pipelines a second,
   ~40 ms each, the emulator waiting for the GPU thread. They are fight
   effects the shipped UID list (`web/make-uidcache.sh`: walking + figure
   swaps) does not cover. The game appends them to the UID list kept in
   IndexedDB (the player's had grown 1,293 → 2,223), so a fight hitches
   once per browser, not every time.
2. **A sustained 55–75% for ~50 s with nothing compiling.** 250 draws and
   4.8 MB of vertex/uniform data a frame. This is the emulated CPU.

Noise in that log, worth silencing someday: `/dev/net/ip/top` is opened
and refused ~100 times a second (the game polls for a network the build
does not have; it scales with frame rate, not the cause of anything), and
Dolphin's audio mixer reports a full granule queue in bursts.

## 3. The fight, replayed in the harness

The player's save state: `~/Downloads/skylanders-state-2026-10-30T0715.sav.gz`
(33 MB gzipped, 110 MB raw). **It is guest RAM, i.e. disc content: never
commit it.** To replay it:

```sh
# the runner's page server serves /tmp/bootweb on :8102; macOS purges /tmp,
# so check these are still there
mkdir -p /tmp/bootweb
gunzip -c ~/Downloads/skylanders-state-2026-10-30T0715.sav.gz > /tmp/bootweb/fight.sav
cp web/dist/SSPE52.uidcache /tmp/bootweb/

WEBGPU=1 caffeinate -dims harness/wasm-core/run-jit.sh 900000000000 \
  --jit --dual-core --webgpu --no-osd --frame-times --idle-stats \
  --skylander --load-state=http://127.0.0.1:8102/fight.sav \
  --ticks=14580000000 --speed-from=2187000000 \
  --uidcache-url=http://127.0.0.1:8102/SSPE52.uidcache > fight.log 2>&1
grep -E 'window    from|idle      [0-9]|seconds   window|hitches' fight.log
```

About a minute each. The disc must be at `/tmp/ssa.iso` (it gets purged;
regenerate it from the RVZ with dolphin-tool). Nothing else running: the
player's page on :8080 competes for the CPU.

| run | window (3–20 emulated s) | CPU thread waited |
|---|---|---|
| first | 72.0% | 1,969 ms (1,848 of it compile hitches) |
| again | 77.4% | 382 ms |
| `JS_FLAGS=--no-liftoff` | 83.0% | 427 ms |
| `JS_FLAGS=--wasm-tiering-budget=100000` | **89.2%** | 420 ms |

One run each; alternate A/B and repeat before believing a difference
under ~3 points (see `docs/PHASE3-HANDOFF.md` §6 on drift).

The profile (`PROFILE=<dir> PROF_DELAY_MS=8000 … --jit-names`, then
`<dir>/threads.txt`):

- CPU thread busy 99%: **85.6% compiled blocks**, 6.5% dispatcher
  (`_main_thread`), 1.8% interpreter/hatch, 1.5% VideoCommon (gather
  pipe), 1.0% MMU slow paths, 0.8% DSP HLE.
- **Flat:** 11,462 blocks sampled; the top 10 are 5.3% of block time,
  the top 100 18.7%, the top 500 38.5%. No hotspot to hand-tune.
- GPU thread busy 32%. Drawing is not the limit.
- The game runs ~518M guest instructions per emulated second here (walking:
  ~390M); the CPU thread manages ~406 MIPS while busy. **It needs ~28%
  more throughput for 100%.**

## 4. The plan

Ranked; each a separate PR, measured on the fight state *and* the
regression scenes, alternating old and new builds: the vortex
(`/tmp/bootweb/s941.sav`, `--ticks=21870000000`), the town gate
(`/tmp/bootweb/gate.sav`, `--ticks=14580000000`), both with `--speed-from=2187000000`
and the same other flags as the fight (`tornado.sav` is there too); the
walking window (`--input=A --skylander --ticks=291600000000
--speed-from=218700000000`, see PHASE3-HANDOFF.md §6). Then the
differentials (`run-jitdiff.sh`) and M3 (`harness/wasm-core/run-m3.sh`).

1. **No freezes on new shaders.** *Done, #107: `docs/PHASE3-WEBGPU.md`
   "A draw waits 4 ms for its pipeline, then goes without".* When a
   pipeline is not ready, skip the
   draw instead of waiting (Dolphin's "asynchronous, skip drawing" idea).
   Pieces already there: `Pipeline::Request` / `BuildInBackground`
   (async `CreateRenderPipelineAsync`, `dolphin-web/VideoBackends/WebGPU/WGPUPipeline.cpp`);
   `PrepareDraw` already returns false for a null pipeline
   (`WGPUGfx.cpp`). Shaders themselves (glslang + Tint, ~7 ms) are still
   compiled synchronously because the shader cache runs with 0 worker
   threads (parallel compile was tried and rejected: dlmalloc contention,
   mimalloc +350 MB heap; PHASE3-WEBGPU.md "Tried and not kept"). Check
   M3 still matches (it waits for everything) and what a skipped draw
   looks like in the fight.
2. **Get hot blocks to TurboFan sooner, without flags.** *Done: the warm
   entry, `docs/PHASE3-WEBGPU.md` "Hot blocks optimised sooner".* Players cannot
   pass `--js-flags`; the +12 points above have to come from how code is
   packaged. V8 tiers up per function when its budget (decremented by
   code size executed, at returns and loop back-edges) runs out; a fight
   runs thousands of blocks a few thousand times each, so many stay in
   Liftoff. Ideas, unmeasured: re-emit blocks that turn hot into fewer,
   larger functions/modules; a budget-burning warm-up for hot blocks;
   compilation hints (check what Chrome 156 ships). Earlier finding to
   respect: the cost of TurboFan code was code size (inlined cold paths,
   `slowpaths.h`), PHASE3-FINDINGS.md.
3. **Memory access hot path.** ~14 wasm instructions per guest access
   with a dependent table load (PHASE3-WEBGPU.md, "Left"). A
   fastmem-style direct mapping for the common BAT case.
4. **Guest registers in wasm locals** across a block/region instead of
   loads and stores to `PowerPCState` per instruction.
5. **Dispatcher** (6.5%): `_main_thread`, link table, budget exits.
6. Small: hatch instructions (`cror`, `ps_sum0`, hardware stores; ~2%),
   `GPFifo::UpdateGatherPipe` (1.2%), DSP HLE on the CPU thread.

## 5. Landmines (short; the full list is PHASE3-HANDOFF.md §5)

- `run-jit.sh` relinks bootgame every run: never edit sources during a
  series. Put each variant behind a flag and alternate.
- Commits need the five numbered sections (WHAT THIS IS, CAVEATS AND
  RISKS, DESIGN CHOICES, WHY, WHAT MIGHT GO WRONG) or CI rejects them.
- `harness/reference/*.png`, `harness/traces/*`, save states: never commit.
- Dolphin changes go in `dolphin-web/patches/`.
- `web/dist` must be rebuilt (`web/build.sh`, ~30 s) for the page to
  run a change; the page server sends `no-store`, the player hard-reloads.
- A first visit's Metal shader compiles: test with Chrome for Testing
  (its own Metal cache), never by touching the player's Chrome cache.
- zsh: an unquoted `$FLAGS` is one argument; use `${=FLAGS}`.
