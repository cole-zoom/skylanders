# Phase 3 — the WebGPU backend: plan

Written 2026-10-23, when the Phase 2 gate was met: the gameplay window at
103–104% of real time with the Null backend, dual core, three runs
(`PHASE3-HANDOFF.md` §1). `PLAN.md` Phase 3 is the goal; this is how to get
there, what is unknown, and what to find out first.

**Gate (from `PLAN.md`):** the game is visually indistinguishable from native
Dolphin across a fixed set of captured scenes. **And a second one this plan
adds:** the gameplay window stays at or above 100% of real time with the
backend drawing. The CPU margin over 100% is about 4 points; a backend that
costs the GPU thread more than the CPU thread's 1.0 s per emulated second
moves the bottleneck to it.

---

## 1. What exists

- **Spike B** (`spikes/webgpu/FINDINGS.md`): every WebGPU feature this game
  needs is present in Chrome 155 on this machine (`texture-compression-bc`
  for CMPR, `dual-source-blending` for the destination-alpha pass,
  `float32-filterable`); the adapter is Metal. A JS replayer drew the title
  screen's geometry from a capture, untransformed, untextured.
- **What the game uses** (`PLAN.md` §3a, the GX inventory at 390 s): 1–2 TEV
  stages in gameplay (9 on the title screen), one texgen, no logic ops,
  texture formats I8, IA8, RGBA8, CMPR, triangle fans and strips, about
  1,900 draws and 15 EFB copies a frame, EFB format changes (`SSP.ini`).
- **The harness**: `bootgame.cpp` runs the game in Chrome with the GPU on its
  own thread (`--dual-core`), the GX capture (`--capture-gx`), the decoder
  (`harness/src/decode_capture.py`), and reference renders from Dolphin's
  software rasteriser (`harness/reference/`).
- **Toolchain**: Emscripten 6.0.12 with the `emdawnwebgpu` port (Dawn's
  `webgpu.h` / `webgpu_cpp.h` over the browser's WebGPU); glslang and
  SPIRV-Cross in Dolphin's Externals.
- **Not there**: `web/` is empty — no browser shell, no canvas. VideoCommon's
  SPIR-V path (`Spirv.cpp`) is not in the wasm build.

## 2. The shape of the backend

`VideoBackends/WebGPU`, beside the others, modelled on `VideoBackends/Metal`
(`PLAN.md` Bet 03): about 4,600 lines there, split the same way —

| Metal | role | WebGPU notes |
|---|---|---|
| `MTLMain` | backend registration, `InitBackendInfo` | report only what WebGPU has: no geometry shaders (VideoCommon expands lines and points in the vertex shader), no logic ops, dual-source blend, BC textures, no primitive restart at first (VideoCommon's `IndexGenerator` already turns fans into lists) |
| `MTLGfx` | `AbstractGfx`: draws, clears, viewport, present | presentation to an `OffscreenCanvas` owned by the GPU thread |
| `MTLStateTracker` | the command encoder, binding, the render pass | one command encoder per frame or per EFB copy; WebGPU has no mid-pass state for render targets, so a target change ends the pass |
| `MTLObjectCache` | samplers, depth-stencil and blend state | WebGPU bakes blend and depth into the pipeline, so most of this becomes pipeline-key fields |
| `MTLPipeline`, `MTLShader` | pipelines, shaders | §3 |
| `MTLTexture` | textures, framebuffers, staging textures | staging = a `MAP_READ` buffer; reading it is §4 |
| `MTLVertexManager` | streaming vertex/index/uniform buffers | `writeBuffer` or a mapped ring; uniforms with dynamic offsets |
| `MTLPerfQuery`, `MTLBoundingBox` | occlusion queries, bbox | stubs first; check whether the game uses either |

The backend runs on the GPU thread — the harness's `--dual-core` thread,
which is how Dolphin runs and how the gate was met. Every WebGPU object it
creates lives in that worker: WebGPU objects cannot move between workers.

## 3. Shaders: SPIR-V to WGSL

VideoCommon generates GLSL; Metal compiles it to SPIR-V with glslang and then
to MSL with SPIRV-Cross. Browsers accept only WGSL, and SPIRV-Cross has no
WGSL output. The step needed is SPIR-V → WGSL, inside the wasm build, at
pipeline-creation time on the GPU thread.

| option | for | against |
|---|---|---|
| **Tint** (Dawn's compiler, C++) built into the wasm | the reference SPIR-V→WGSL translator; Babylon.js ships the same thing as `twgsl` for exactly this GLSL→SPIR-V→WGSL path in browsers | a large dependency (Tint + SPIRV-Tools) to build under Emscripten; size unmeasured |
| **Naga** (wgpu's, Rust) | mature SPIR-V front end and WGSL back end, `wasm32` builds are routine | a Rust toolchain in the build and an FFI across it (`PLAN.md` Bet 02 allows Rust, for `ppc2wasm`) |
| WGSL emitted directly by VideoCommon's shader generators | no translator at all | a new dialect across every generator (pixel, vertex, uber, texture conversion, EFB copy, …) — the largest option and the one most likely to drift |

**Recommendation: Tint**, with Naga as the fallback if Tint will not build
or is too large. The measurement that decides it is §5 spike S2.

Shader compile time matters more here than on native: pipelines are created
on the GPU thread as new UIDs appear, and a translation that takes tens of
milliseconds is a visible hitch. Dolphin's shader cache and its ubershaders
exist for this; both should work unchanged once translation does.

## 4. Synchronous GPU work in a browser

Dolphin's GPU thread sometimes needs an answer from the GPU before it can go
on: an **EFB peek** (the game reads a pixel — `RefreshPeekCache` was 3% of the
GPU thread on Null, so this game does), a bounding-box read, a perf query,
an EFB copy that must reach guest RAM. On native that is a fence wait. In
WebGPU it is `GPUBuffer.mapAsync`, a promise, and a thread blocked in
`RunGpuLoop` never returns to the event loop that would resolve it.

**Plan: JSPI** (JavaScript Promise Integration, `-sJSPI`): wasm suspends on a
promise and resumes when it resolves, in a worker, without returning to the
loop. Chrome ships it; Emscripten supports it, and `emdawnwebgpu` can wait on
its futures through it. Asyncify is the fallback and far costlier.

Two things to measure before building on it: that it works on an Emscripten
pthread (the GPU thread is one), and what one blocking readback costs — a
full GPU round trip, paid every time the game peeks. Device creation is
asynchronous too, and has to finish before the GPU loop starts.

## 5. Order of work

Each step has a gate; a step that fails its gate is a decision, not a delay.

**S1 — a WebGPU device on the GPU thread (spike).** In the harness, on the
`--dual-core` thread: request the adapter and device, clear an
`OffscreenCanvas` the page transferred to that worker, and do one blocking
readback through JSPI. *Gate:* all three work, and the readback's round trip
is measured. *If JSPI cannot block a pthread:* the design of §4 changes
before anything else is written.

*Done, gate met* (`spikes/webgpu-thread/FINDINGS.md`): device in 50 ms on a
pthread, a blocking readback in ~0.4 ms with correct values. One constraint
found: a frame reaches the screen only after the worker's task ends, so the
GPU loop must yield once per presented frame — through a `MessageChannel`
task (21 µs), not a timer (5.3 ms, clamped) or a microtask (does not
present).

**S2 — Dolphin's shaders through Tint (spike).** Build glslang's SPIR-V path
and Tint for wasm; translate the shaders this game actually creates
(collected from a run, by UID) and check that WebGPU accepts every one.
*Gate:* all of them translate and validate; the size added to the wasm and
the time per shader are measured.

*Done, gate met* (`spikes/webgpu-shaders/FINDINGS.md`). The 390-s run
creates 349 shaders. After four changes to what VideoCommon emits for
WebGPU — eight sampler bindings instead of `samp[8]` (every TEV pixel
shader), a bit-test `isnan`, no forced early-Z (`bSupportsEarlyZ = false`),
32-bit palette words — all 346 that reach SPIR-V translate and are valid in
Chrome. Tint in wasm: 5.9 ms a shader, byte-identical to native, +1.15 MB
gzip. The device needs `dual-source-blending`, `clip-distances` and
`texture-compression-bc`. **Tint it is**; Naga is not needed.

**M1 — the backend skeleton, first pixels.** `VideoBackends/WebGPU`
registered, `InitBackendInfo` honest, the EFB as a render target, XFB
presentation, clears. *Gate:* the game's frame appears in a canvas — the
clear colour and the XFB copy — and the GX capture's frames and draws match
the Null run.

*Done, gate met — and most of M2 with it* (2026-10-23). The backend is in
`dolphin-web/VideoBackends/WebGPU/` (~1,500 lines, Metal's layout),
compiled into the harness by `WEBGPU=1` and selected with `--webgpu`. The
game's own draws work too, because S2's shader rewrites are applied in the
backend's compile step until VideoCommon emits them. Over the 390-s run,
dual core: the same 23,249 frames and 28,874 GX draws as the Null run,
**zero WebGPU errors**, and the tutorial level on the canvas, the same scene
as the native `harness/reference/ssa-gameplay.png`. Screenshots are not
committed (`harness/reference/*.png` is ignored: they are frames of the
game); `SCREENSHOT=out.png WEBGPU=1 harness/wasm-core/run-jit.sh 300000000000
--jit --dual-core --webgpu --no-osd --input=A --skylander
--ticks=291600000000` reproduces this one, and `--ticks=29160000000` the
menu at 40 s. Speed: the
gameplay window **82.4%** (Null: 103–104%), the whole run 92.7%.

How it is built, and what it does not do yet:

- Shaders: Metal's GLSL dialect → glslang (VideoCommon's `Spirv.cpp`) →
  Tint, with the split samplers mapped to group 1, binding 32+i → the
  browser. One explicit pipeline layout for every pipeline (group 0: three
  uniform blocks; group 1: eight 2D-array textures and eight samplers):
  "auto" layouts drop what a shader declares but does not use, and Tint
  declares all eight textures in every TEV shader.
- Tint aborts, rather than failing, on SPIR-V it does not implement
  (`OpIsNan`): one such shader took the GPU thread down. The compile step
  refuses any source still calling `isnan`.
- The presenter draws into an offscreen texture that `PresentBackbuffer`
  copies to the canvas: a canvas texture is valid only until the worker
  yields, and any wait yields (a readback, a shader's validation).
- Not yet: depth textures as shader inputs (draws that sample one are
  skipped), storage and texel buffers (GPU texture decoding, palette
  conversion, the dynamic vertex loader, line/point expansion, bounding
  box), occlusion queries, the generator changes as an `APIType::WebGPU`
  in VideoCommon, a pixel comparison against native frames (M3).
- Harness fixes found on the way: the runner refuses to start while an
  earlier run's Chrome or servers hold its ports (a stale Chrome made two
  A/Bs report the old page), survives a timed-out poll, gives headless
  Chrome a window tall enough for the canvas, and `SCREENSHOT=file.png`
  saves the canvas at the end of a run.

**M2 — drawing.** Pipelines from VideoCommon's UIDs, vertex formats, the
streaming buffers, textures (CPU decode first; CMPR as BC1), samplers, EFB
copies to texture, blending including the destination-alpha pass, EFB format
changes. *Gate:* the title screen and the tutorial level render recognisably
in the browser.

**M3 — the regression suite.** Fixed scenes — title, menu, tutorial level,
the pause screen (`SSP.ini`'s blur) — rendered by native Dolphin and by the
browser from the same captures, diffed per pixel with a tolerance. *Gate:*
`PLAN.md`'s — indistinguishable across the set.

*Done* (2026-10-24), with one stated gap. The suite is
`harness/wasm-core/run-m3.sh`: each FIFO capture is replayed in the browser
harness (`bootgame --fifo=`, Dolphin's FifoPlayer) once with the Software
renderer -- Dolphin's reference rasteriser -- and once with the WebGPU
backend writing its XFB to RAM (`--xfb-to-ram`), and the last frames are
compared (`harness/src/compare_frames.py`). Scenes: boot, title, the
save-slot menu (the three captures in `harness/traces`), and the tutorial
level at 390 s, recorded by the harness itself (`--record-fifo=TICKS`).

| scene | per pixel > 8 | edge-tolerant > 32 | 8x8 blocks: p99 / > 8 | verdict |
|---|---|---|---|---|
| boot | 0.00% | 0.000% | 0.00 / 0.00% | PASS |
| title | 7.46% | 0.077% | 4.92 / 0.00% | PASS |
| save-slot menu | 3.63% | 0.036% | 3.45 / 0.02% | PASS |
| tutorial level | 17.56% | 0.276% | 10.28 / 3.20% | PASS |

Every difference is one of two kinds, each looked at, not assumed:

- **Edges**, a pixel either way, between a GPU's coverage rules and the
  Software rasteriser's. The edge-tolerant figures forgive exactly that.
- **Colour depth.** The Software renderer emulates the EFB's 6-bit format
  with dithering -- a visible cross-hatch in its sky -- and truncates on
  every write, so it runs a level or two darker, more where layers blend
  (the clouds). Dolphin's hardware backends default to "Force 24-bit
  color", which does neither; so does this one. Per pixel this is noise of
  sd 3.5-5 everywhere; over 8x8 blocks it cancels to a bias of +1.1.

Nothing else: no missing or wrong geometry, texture, colour or blend in any
scene. The verdict's thresholds sit just above the level's figures, and it
has teeth: the same backend with blending switched off fails the title
(block p99 167).

The gap: `PLAN.md` says *native* Dolphin, and a native hardware backend
could not be rendered headless here (Metal never presents without a
surface, so no screenshot is taken; the native build has no Vulkan
library). The reference is Dolphin's own rasteriser instead, and the
differences above are the ones any hardware backend has from it. A
windowed native Metal render of the same captures would close this.

Open: with "Force 24-bit color" off (`--no-true-color`) the WebGPU frame
runs *brighter* against the Software one (+2.3 rather than +1.1) and fails
the verdict, where a correct 6-bit path should remove the bias. Whether
Dolphin's hardware backends do the same in that mode is the windowed
comparison's question too. Not the default, and not this game's setting.

Found on the way: every non-WebGPU build of the harness had `main()` within
bytes of Emscripten's 64 KB default stack; a few more locals overflowed it
into static data and zeroed zlib-ng's function table, which surfaced as
"null function" in `crc32_z`. Every build now gets a 4 MB main stack and 2 MB
for other threads (`link.sh`).

**M4 — speed.** The GPU thread's busy seconds per emulated second with the
backend drawing, against the CPU thread's. *Gate:* the gameplay window at or
above 100%. Expect work here: per-draw state changes, uniform uploads, and
the readbacks of §4.

*First step* (2026-10-24): **87.0% → 94.3%** in the gameplay window (dual
core, 300-400 emulated s; two runs each, from one build, with
`--webgpu-per-draw` selecting M1's path: 87.0 / 87.0 against 94.2 / 94.5).
The gate is not met; the Null backend does 104.3% on the same build.

What a frame cost, counted by the backend (every 600 presents it logs
`stats/frame`):

| per frame | M1's path | now |
|---|---|---|
| draws | 388 | 388 |
| `writeBuffer` calls | 1,727 | 4 |
| bytes written | 5.4 MB | 5.4 MB |
| bind groups made | 776 | 21 |
| bind groups set | 776 | 714 |
| readbacks (`mapAsync`) | 0 | 0 |
| submits | 1.3 | 1.3 |

- **Buffer writes gathered.** Allocations in a stream buffer only move
  forward between submits, so everything written since the last submit is
  one range: `StreamBuffer::Write` copies into it and `Flush` writes it in
  one call per buffer, before the submit.
- **The uniform group made once**, with each block's offset given per
  draw (dynamic offsets) rather than a group made per draw for the offsets.
- **Texture groups cached**, keyed by each slot's texture (an id, never
  reused, not the address) and sampler state; a texture's destruction
  clears the cache (0.03 times a frame), and it is bounded at 4,096.
- **State a pass already has is not set again**: pipeline, groups, vertex
  and index buffers.

No readbacks at all in gameplay: the EFB peeks and copies to RAM §4 worried
about do not happen in this window, so the blocking wait is not a factor.

Where the rest goes, from runs with parts of the backend switched off
(temporary flags, not kept):

| variant | window |
|---|---|
| now | 94.5% |
| no yield per present | 95.9% |
| draws not recorded | 97.6% |
| draws not recorded and no buffer writes | 101.1% |
| Null backend | 104.3% |

The cost is latency, not throughput: the GPU thread is 59% busy and
Chrome's GPU process about 11% of a core, so nothing is saturated, but the
CPU thread waits for the GPU thread at every skipped idle loop
(`SyncOnSkipIdle`; 95% of its waiting is in `CoreTiming::Idle`). That first
reading put the remainder down to the draws (~3 points), the buffer writes
(~3.5) and the GPU loop's own overhead (~22% of the thread); the
measurements below correct it.

*What the gap is* (2026-10-24, later the same day). Instrumentation added
for this, all in the tree: `--idle-stats` (times every idle sync of the CPU
thread with the GPU thread and reports them for the window, with the FIFO
backlog at the long ones; two `performance.now()` calls per sync, about
half a percent), the `fifo` line (the GPU loop's runs, chunks and the CP's
enables, from `dolphin-web/patches/0018-fifo-loop-hooks.patch`),
`--fifo-batch=N` and `--gpu-spin=N` (the same patch), `--skip-vertex-convert`
and `--vertex-stats` (`0019-vertex-convert-skip-hook.patch`),
`--sync-gpu=TICKS`, `--webgpu-no-upload` and `--webgpu-no-draws`. The
patched objects are rebuilt by hand (`PHASE3-HANDOFF.md` §5, the ninja
regen trap). Gameplay window, dual core, one run each unless stated; runs
vary ±3%, and the `frames` line (23,846 for this run) is the determinism
check -- it held in every run below. "Own time" is the window's wall clock
minus the waiting: the CPU thread's time for the same 36 billion guest
instructions.

| run | window | waited | own time |
|---|---|---|---|
| Null | 106.1% | 10.7% | 84.2 s |
| Null, vertex conversion skipped | **115.0%** | -- | -- |
| Null, GPU loop sleeps after 1,000 empty runs | 102.8% | 10.2% | 87.4 s |
| WebGPU | 93.1% (94.2–94.5 without `--idle-stats`) | 11.8% | 94.7 s |
| WebGPU, vertex conversion skipped | **97.4%** | 5.4% | 97.1 s |
| WebGPU, no buffer uploads | 91.1% | 11.0% | 97.7 s |
| WebGPU, no draws recorded | 90.3% | 11.2% | 98.3 s |
| WebGPU, GPU loop sleeps after 1,000 / 100 empty runs | 92.2% / 82.2% | 11.5% / 10.4% | 96.0 / 109 s |
| WebGPU, FIFO chunks batched 64 / 2,048 per status update | 94.8% / 92.3% | | |
| Null, SyncGPU 200,000 / 2,000,000 ticks, no sync on idle | 97.8% / 101.7% | 1.1% / 1.7% | 101 / 97 s |

**The waiting is one wait per game frame, for a full FIFO to drain.** The
CPU thread makes 37,850 idle syncs an emulated second and 99.8% of them
find the GPU thread already done (≤5 µs). About 30 a second -- one per
30-fps game frame -- wait 2 to 13 ms, and those are all of the 11.8%.
Every one of them begins with the FIFO's read-write distance at 240 KB:
the FIFO is 256 KB and that is GX's default high watermark, 16 KB below
the top. So the game writes a frame's commands (268 KB; 8 MB a second)
faster than the GPU thread consumes them, overflows, and spins in its
overflow handler; the handler's loop is an idle loop, and "sync on skip
idle" holds the CPU thread there until the GPU thread has drained
*everything* -- not to the low watermark the hardware would resume at. The
wait is the GPU thread's time to process 240 KB of commands and the draws
in them: 4.2 ms with this backend, 3.4 ms with Null.

**Most of the drain is vertex conversion.** The wasm build gets Dolphin's
generic `VertexLoader` -- a function-pointer call per attribute per vertex;
the x64 and ARM64 JIT loaders do not exist here -- and the profile had it
at 14% of the thread's samples. `--skip-vertex-convert` (the FIFO is
advanced over the vertices and the draws go out with stale data) measures
what it is worth: under Null the wait all but disappears and the run is
CPU-bound (115%); under this backend 97.4%, with 5.4% still waited -- 1.85
ms a game frame, of which about 1 ms is VideoCommon's other work in the
drain that Null has too (the opcode decoder, index generation, texture
hashing and lookup, pipeline state) and about 0.85 ms this backend's calls.
The vertices: 2.2 billion over the 400-s run, 5.5 million an emulated
second, in **359 distinct formats**; the top twelve cover 44%. The common
shape is position, one colour and one texture coordinate, all 16-bit
indexed, six raw bytes to 24 native, with the VATs differing in fraction
bits. A long tail: a loader per format, as Dolphin's native loaders are,
rather than a few hand-specialised ones.

**The CPU thread itself runs 12% slower with the backend drawing.** 84 s
of own time under Null, 95–98 s under WebGPU, for the same instructions --
10.5 s of the 13 s between the two backends, more than the extra waiting
(2.6 s). It is not the GPU loop's spin (sleeping instead is worse for both
backends: the wake-ups land on the CPU thread's FIFO-write path, and the
loop still runs 9 million times a second), and not the buffer uploads
(91.1% with none, own time 97.7 s), and not the draws (90.3% with none
recorded, own time 98.3 s -- nothing reaching the GPU process but texture
uploads and the present). What remains with `--webgpu` at all: the present
path (the canvas copy, the submit, the yield; Chrome compositing an
animating canvas, which may change the renderer's scheduling or QoS),
texture uploads, the package's power budget, or the machine: the later
runs of this series came out 2–3 points lower on both backends (Null 106.1
early, 102.8 late; WebGPU 93.1 to 90.3), so the no-upload and no-draws
deltas are inside that drift, though the own-time gap is not. To test, in
this order: `--webgpu` with the present skipped (the next bisect step; add
the switch); `sudo powermetrics --samplers cpu_power -i 1000` during a Null
run and a WebGPU run (the P-cluster's frequency); the window in a visible
rather than headless Chrome; and from now on A/B pairs run alternately,
compared to their neighbours.

**What does not help, measured.** The GPU loop's per-chunk overhead
(`SetCPStatusFromGPU` and the loop were 22% of the thread's samples): the
counters show 17 million loop runs an emulated second, 17 million of them
finding nothing -- the loop spins between the CPU thread's syncs until
`SystemTimers` lets it sleep at 1 kHz -- so it is not on the critical path,
and batching up to 64 or 2,048 chunks per status update changes nothing.
The game enables CP breakpoints and both watermark interrupts (the `fifo`
line's enables are 7), so the batching is only taken while no breakpoint
is set; it stays in the patch, at 1, as the measurement. Dolphin's SyncGPU
(`--sync-gpu=TICKS --no-sync-idle`: the CPU may run TICKS ahead of the
GPU's estimated progress instead of stopping at idles): frames and pacing
hold -- plain `--no-sync-idle` drops to 15.9 frames a second -- but it is
slower, and the CPU thread executes 2.6× the guest instructions (927 MIPS
against 381): the overflow wait becomes a spin the idle rule does not
absorb. Open: with that loop recognised (`PHASE3-HANDOFF.md` §4.4 has its
shape, for single core), SyncGPU could let the drain overlap the next
frame's CPU work, which "sync on skip idle" never can. The present's yield:
0.11–0.16 ms a frame, in the noise.

*The plan, ranked by measured payoff.* The gate needs ~6 points. Under
this backend vertex conversion is worth 3.5 (97.4%), the drain's other
work about 2 (the 0.85 ms of calls, if taken off the thread), and the CPU
thread's unexplained 12% up to 10 if its cause can be removed; the
recompiler sets the ceiling (115% at zero waiting).

1. **A wasm vertex-loader JIT on the GPU thread.** One wasm function per
   `VertexLoaderUID`, laid out as `VertexLoaderARM64.cpp` lays its code
   out (per attribute: index fetch, convert, scale by the fraction, write
   at the native offset), installed the way `JitInstall` installs blocks
   (`bootgame.cpp:199`: `WebAssembly.Module` and `Instance` with
   `wasmMemory` and `wasmTable`, synchronous, allowed on a worker; the GPU
   thread installs and calls on itself -- check whether the table is
   shared between threads before growing it from two). It plugs into
   `VertexLoaderBase::CreateVertexLoader` (`VertexLoaderBase.cpp:236`),
   which already has `VertexLoaderType::Compare` wrapping a new loader in
   `VertexLoaderTester` against the software one -- the differential, for
   free: run the 390-s path and the M3 captures under it. Keep the generic
   loader selectable (`VertexLoaderType::Software`). Gate: Null ≥110%,
   WebGPU ≥97%, frames 23,846, M3 PASS; `--vertex-stats` for the formats.
   About a week; the emitter (`ppc2wasm/src/wasm_emit.h`) and the install
   path exist. Payoff measured: Null 106 → 115, WebGPU 94 → 97.
2. **Find what slows the CPU thread** (above). Half a day of measurement
   before anything is built: `powermetrics`, the synthetic-load run, and
   `--idle-stats` own time on each variant. If it is power or core
   placement, it is the machine's and will differ elsewhere -- say so and
   move on; if it is the backend's thread work, it joins item 3.
3. **The backend's work in the drain.** Either take the WebGPU calls off
   the GPU thread -- a submission worker: `Gfx` records draws (buffer
   offsets, bind-group keys, pipeline, pass changes, upload ranges) into a
   queue and a third pthread issues the API calls, so the drain ends when
   the frame is *generated*; Dolphin's Vulkan backend does this for its
   submits, and here every call is a wire serialisation, so all of them
   move; readbacks and the present need ordering; a design change, one to
   two weeks -- or shave in place, each worth ≤2 points and read off
   `stats/frame`: generate vertices straight into the gathered-write
   buffer (`StreamBuffer::Write` copies 3.7 MB a frame), a
   `mappedAtCreation` staging buffer per submit, skip the 80 of 322
   vertex-constant uploads a frame that repeat the last, one uniform
   bind-group set per draw instead of two.
4. **The CPU thread** sets the ceiling: with the waiting gone the window
   tops out near 115%. `PHASE3-HANDOFF.md` §4.3 stands: find where the
   compiled code's 2.67 ns a guest instruction goes before more emitter
   work.
5. **Not worth doing**: the GPU loop's overhead or spin, FIFO batching,
   the present's yield, SyncGPU as it stands.

Order: 1; 2 alongside it (measurement only); then 3's shaves, and 3's
worker if the gate is still unmet.

*Item 1, the wasm vertex-loader JIT* (2026-10-25): **the gate is met --
WebGPU 101.4 / 101.5%**, Null 121.2 / 121.1%. Gameplay window, dual core,
one build, two A/B pairs run back to back, the second in reverse order
(JIT then generic, then generic then JIT), every run under `caffeinate
-dims`; `frames` 23,846 in all eight:

| gameplay window | generic loader | JIT |
|---|---|---|
| WebGPU, pair 1 / pair 2 | 96.2 / 96.1% | **101.4 / 101.5%** |
| Null, pair 1 / pair 2 | 113.8 / 110.8% | **121.2 / 121.1%** |

Both baselines came out above the earlier series' (94% and 104-106%) on
the same code -- a quiet, idle machine -- so the A/B within a pair is the
result: +5.3 points under WebGPU, +7 to +10 under Null. Under Null the
JIT beats the "conversion skipped" bound measured earlier (115%), which
was measured in a slower series; the bounds above are not a ceiling to
compare against across sessions.

`harness/wasm-core/vertexjit.cpp`, through a weak hook in
`VertexLoaderBase::CreateVertexLoader`
(`dolphin-web/patches/0020-vertex-loader-hook.patch`; only on targets
without Dolphin's x64 or ARM64 loader). One module per `VertexLoaderUID`,
one function `(src, dst, count) -> loaded`: the array bases and strides
the format uses loaded once before the loop, then per vertex each
attribute's index read, its address computed, its components converted
and stored at the native offset, the z-freeze caches written for the last
vertices, and a vertex whose position index is 0xFF/0xFFFF skipped. The
output is the **generic loader's, bit for bit** -- including where the
ARM64 loader differs from it (the position scale applies whether or not
ByteDequant is set; texture-matrix indices are masked to six bits) --
because the generic loader is what has drawn every frame until now.
Float components are copied as their bits, so a NaN keeps its payload.

- **Correctness.** `--vertex-loader=compare` runs both loaders on every
  call from the same caches and compares the counts, the output and the
  five caches. The 400-s path: all 359 formats compiled, 157.8 million
  runs compared, **0 differed**. M3 passes (4 scenes) with the JIT as the
  default; the level-390 capture alone makes 140 loaders.
- **Cost.** 199 KB of modules for the 359 formats, 14-18 ms installing
  over the whole run (one module each, ~45 us).
- **Threads.** A function-table entry exists only on the thread that
  added it, so a loader installs on the first thread that runs it and any
  other thread gets the generic loader (counted: 0 calls in this run --
  in dual core every loader runs on the GPU thread).
- **Switches.** `--vertex-loader=jit` (default), `software` (Dolphin's
  generic loader, unchanged), `compare`. The `vtxjit` line at the end of
  a run reports what was compiled.

*A measuring trap found on the way:* long runs started while nobody is at
the machine crawl from their first 30 seconds (415 million instructions
in them against 6,646 million, 16x slower) and time out after an hour -- the generic loader as much as the JIT, so
not the code. A run started at the machine is normal. The cause is not
pinned down (display sleep or idle throttling of the headless Chrome is
the suspect; the MacBook Air is fanless, but heat alone is not 16x).
Start every series under `caffeinate -dims` and check the first `--- t=30s`
line (~6.5 billion instructions for the gameplay command) before
trusting it; kill the headless Chrome (`pkill -f "Chrome --headless"`)
after killing a run, or it stays at 100% of a core.

*What is left.* The margin is 1.5 points, inside the run-to-run variance
of a different session, so the gate is met but not by much.

Item 2's first bisect step, run the same night (JIT on, gameplay window,
frames 23,846 in all four, order no-present, present, present,
no-present): `--webgpu-no-present` (frames drawn and submitted, nothing
copied to the canvas, no yield) 102.9 / 103.0%, presenting 100.9 / 101.4%.
**Presenting costs about 2 points** -- real, but a tenth of the gap to
Null (121%). The rest is in drawing and submitting, which is where item
3 points; `powermetrics` (needs a password) is still unrun. Item 3's shaves
(vertices generated straight into the gathered-write buffer; repeated
vertex-constant uploads skipped) and a SIMD conversion path in the JIT are
the next points to buy headroom.

**M5 — the browser shell.** `web/`: the page, the canvas, input, the disc
streaming that already works in the harness. Overlaps Phase 4.

*Done* (2026-10-25). `web/build.sh && node web/serve.mjs /tmp/ssa.iso`,
then http://127.0.0.1:8080/ in Chrome.

- **The page** (`web/index.html`, `web/shell.js`) boots the same module
  every measurement runs (`bootgame.cpp`, WebGPU, JIT, dual core) with
  `--live-input --speed=1 --skylander`. It shows a boot status, a speed
  readout and a fullscreen button. Before loading anything it checks for
  WebGPU, JSPI and cross-origin isolation, so a missing one is reported in
  words, not as an abort. Query: `speed=`, `figure=ID:VAR`, `log=1`.
- **Input** (`web/input.js`): keyboard, mouse and standard-mapping
  gamepads become one 24-byte struct the page writes straight into wasm
  memory. `--live-input` points the Wiimote's and Nunchuk's input
  overrides at it, the same hook the scripted `--input` uses, so no
  Dolphin patch was needed. The layout is fixed in both files.
- **Real time**: `--speed=1` turns Dolphin's throttle back on (the
  Video Interface calls it every field); measuring runs keep it off.
- **One origin** (`web/serve.mjs`): page and module with COOP/COEP, and the
  disc at `/disc`, ranges only.
- **Smoke test** (`web/smoke.mjs`): headless Chrome, keys sent through
  Chrome's input pipeline. Tapping Space every 2 s takes the game from boot
  through the intro and Flynn's dialogue to Shattered Island (about 4.5
  min of real time); `MOVE_FROM=` then holds W and D, and Whirlwind
  walks.

*Measured with it.* The speed readout holds at about 100% and never above,
so the limit works. It dips to 84–94% in the stretch before gameplay (about
120–260 s in) and **to 87–91% while walking**: the M4 gate's window taps A
in place, and moving through the level costs more. Real play is not yet at
100% everywhere, so the M4 headroom work (item 3) still matters.

*Not done here.* Audio (Null audio; Phase 4's AudioWorklet), the portal
UI (the figure is fixed by `figure=`), saves, the pointer checked in a
real menu (mapped, not tested), and the canvas at the game's own aspect
(the backend's surface is a fixed 640x528, scaled by CSS). `web/` is plain
JavaScript modules, not TypeScript as `ARCHITECTURE.md` planned. There is
no build step to justify yet.

**Headroom (after M5).** A person playing in a visible Chrome saw the
readout usually at ~80%, now and then ~28%, and 0% for a few seconds
while Eon spoke. The gameplay window (101%) sees none of that: it taps A
in place. Two new switches measure what play feels like:

- `--frame-times`: every frame's wall time against its emulated time,
  the speed in each wall second (what the page's readout shows: median,
  p25, p10, p1, worst), and each frame over 100 ms with what happened
  during it. The extra counters: GPU-thread compile time by phase
  (glslang, Tint, `CreateShaderModule`, `CreateRenderPipeline`), time in
  synchronous disc reads, and the CPU thread's idle waits.
- `--walk-from=TICKS`: from then on the Nunchuk stick walks a square
  (a quarter turn every 2 s) while `--input` keeps pressing.

*What they found* (WebGPU, JIT, the 400-s path, frames 23,846 in every
run):

| | taps A (the gate) | walks from 300 s |
|---|---|---|
| gameplay window | 100.6% | **91.3%** |
| per wall second in it: median / worst | 101% / 97% | 91% / 82% |
| whole run: frames over 100 ms | 13 | 13 (the same frames) |

- **The stalls are shader compiles.** Every frame that stalled (wall
  time far above its emulated time) had the GPU thread compiling, with
  the CPU thread waiting for it. The worst: 864 ms at 110 s, in the
  intro, 808 ms of it compiling. Disc reads totalled under 40 ms. Over
  the run: 364 shaders and 433 pipelines, 3.9 s. By phase: Tint 1.8 s,
  glslang 0.9 s, pipelines 0.7 s, modules 0.5 s.
- **Walking is the CPU thread.** A profile of the walking window: the
  CPU thread is busy 95% of the time, 83% of it in compiled guest code,
  spread thin (the top 500 blocks are 45% of block time). It waits on the
  GPU thread 5% of the time; the GPU thread is busy 47%. Walking's
  shortfall is the game's own code, not the backend.

*Stalls, fixed.* Dolphin's shader cache already records every pipeline a
run makes in `Cache/<game id>.uidcache`, and compiles all of them at
startup. The harness never created `Cache/`, so no run kept one; now it
does.
- `--dump-uidcache` prints the file at the end of a run.
- `--uidcache-url=` fetches one before the backend starts.
- `web/make-uidcache.sh` makes it (one 6-minute run), and the page loads
  it.

With it, every pipeline compiles at boot (about 3.5 s; the backend has no
background compiling, so on the GPU thread before the game runs):

| whole run | without | with the UID file |
|---|---|---|
| worst second | 21–42% | **80%** |
| p1 second | 77–79% | **82%** |
| frames over 100 ms | 13 | 6 (loading: each covers more emulated time than wall) |
| GPU-thread compile during them | 1,661 ms | 4 ms |

The file covers the harness's path, not the whole game: a level the path
never reaches still compiles its pipelines when first drawn. Persisting
the file in the browser (OPFS), so each session adds what it met, is the
follow-up. The file is generated, not committed (a record of the game's
render states); the page runs without it, with the stalls.

*Walking, taken apart.* Same path, walking from 300 s, one run each
unless stated, frames 23,846 in all but one:

| walking window | speed | MIPS | CPU thread waiting |
|---|---|---|---|
| Null | **101.3%** | 394 | 3.6% |
| WebGPU, uploads, draws and presenting all off | 97.5% | 380 | 3.9% |
| WebGPU, no uploads | 95.0% | 369 | 4.3% |
| WebGPU, no draws | 93.5% | 364 | 4.4% |
| WebGPU (four runs) | 90.9–91.4% | 354–357 | 5.0% |
| WebGPU, GPU loop sleeps after 1,000 empty runs | 88.4% | 344 | 4.9% |
| WebGPU, GPU loop sleeps after 50 | 79.9% | 310 | 4.5% (frames 23,847) |

- **The waiting hardly changes; the CPU thread's own speed does.** The
  same guest code runs 10% slower under WebGPU than under Null, and 3.5%
  slower even with every browser-side call off.
- **Letting the GPU thread sleep makes it worse, not better.** It is not
  a fight for a core. The likelier reading is clock speed: a busy
  neighbour keeps the cluster's clock up, and the GPU's and Chrome's work
  shares the fanless Air's power budget. `powermetrics` (which needs a
  password) would confirm it.
- **The ceiling for the backend is about 10 points**: about 6 for the
  browser's work (uploads ~4, draws ~2.5, overlapping) and about 4 for
  our own GPU-thread work with nothing sent. Null walking is 101%, so
  getting walking clearly over 100% also needs the recompiler.
- **What is uploaded, per present while walking:** vertices 3.7 MB,
  uniforms 1.9 MB (about 4.8 KB per draw, ~400 draws), indices 0.28 MB.
  Uniform blocks are re-sent whenever VideoCommon marks them dirty, and
  574 of those uploads a present carried the same bytes as before. They
  are skipped now (`Gfx::SetGXUniforms`; `--webgpu-no-uniform-dedup` for
  A/B), which removed 18% of the uniform bytes but no measurable speed
  (on 91.4 / 91.8%, off 91.6 / 91.4%): bytes are not what uploads cost.
  It stays, because offering every block every draw also fixes a latent
  bug. A clean block could point at bytes the uniform buffer had since
  wrapped over; now its upload is re-sent after a wrap. M3 passes.

### The recompiler, after headroom: code size, then accuracy

Walking under WebGPU was 91%, and the walking profile put 83% of the CPU
thread in compiled guest code, spread over ~14,000 blocks. All numbers
below are the walking window (`--walk-from`, 300-400 s), frames 23,846 in
every run.

*What it is not.* Chrome does optimise the blocks: with the fast
compiler only (`--liftoff-only`) the boot runs at 82% against 145%, but
forcing the optimising compiler on everything (`--no-liftoff`) or
optimising sooner (`--wasm-tiering-budget=100000`) leaves the walking
window where it was (107.3% / 107.3% / 109.1% against 107.3%).
`run-boot-browser.mjs` passes `JS_FLAGS` through for such A/Bs.

*What it is: code size.* `--jit-dump=PC[,PC...]` prints a block's
module and `harness/wasm-core/jit-asm.mjs` prints the ARM64 TurboFan
makes of it. The hottest block (`0x8037ac20`, ~60 guest instructions of
`lfs`/`fmuls`/`fadds`/`stfs`) was 5,874 machine instructions, 23 KB. TurboFan
lays rarely taken paths inline, so every memory access carried ~45
instructions of fallback (the hardware-register test, the hatch call,
the bail, the budget charges) around ~14 that run. A probe that adds one
more never-taken copy of that fallback to each access
(`--jit-probe-pad`) costs a fifth of the speed: **106.1% -> 86.4 / 86.8%**.

*What changed* (Null walking, each line on top of the one before):

| | walking, Null (points) | the change |
|---|---|---|
| before | 106.8-107.0% | |
| compact slow paths | +4 | each slow path is one call to a helper (`slowpaths.h`): the hatch, and an access the region table does not map; the helper charges the budget and leaves the exit PC in a fixed slot |
| FPRF as Dolphin's JITs | +2.3 | the inline FP forms leave FPSCR[FPRF] alone unless `Config::MAIN_FPRF` (default off) |
| FP at Dolphin's JIT accuracy | +2.4, then +3 | add/sub/mul/div and the paired multiply-adds without the operand tests and hatch fallbacks that only served FPSCR's exception bits; Force25Bit, the NI flush and the fmadd tie correction (`MAIN_ACCURATE_FMADDS`, default on) kept |
| short float conversions | +0.7 | `lfs`/`stfs`/`psq_l` through promote/demote, the bitwise path only for NaNs and inexact stores; exact |
| scalar fmadds, frsp, fctiwz inline | +3 | at Dolphin's JIT accuracy; interpreter traffic 476M -> 263M instructions over the run |
| **all** | **122.8 / 123.3%** | |

Under WebGPU, the number this started from, alternating runs: **old 90.9
/ 91.0%, new 103.1 / 103.2%**. Screenshots at the end of both show the
same scene and position.

*Accuracy.* The exact switches (compact, short conversions) pass the
differentials (`run-jitdiff.sh /tmp/jitdiff.js --compact
--short-convert`, now also nightly), and mutants of each fail them. The
chain differential now traps (`twi`) in one chain in eight, which is the
first thing to reach the hatch's own exit; before, an over-charge there
passed. The FP mode is not the interpreter's: FPSCR's exception bits and
NaN payloads are what Dolphin's own JITs make of them by default, and
`--jit-exact-fp --jit-fprf` restores the exact forms.

*Left.* The interpreter still takes stores to the hardware registers,
`cror`, `psq_st` to the pipe, `dcbz_l`, `ps_sum0`, `lwbrx`, `andi.`, `andis.`
(263M instructions over the run, ~2-3% of the CPU thread). Per access the
hot path is still ~14 instructions with a dependent table load, and
guest registers live in memory.

### A visible tab against the harness

The player's ~80% readout was far below anything the harness measured.
The candidates: presenting to a real screen (headless skips it), the
real-time limit (`--speed=1`, which the page always runs with and the
harness never did), and the page itself (live input through Chrome's
input pipeline instead of `--input`). `HEADED=1` now runs both
`run-boot-browser.mjs` and `web/smoke.mjs` in a visible window, and the
smoke test prints the distribution of the page's own readout.

The walking window under WebGPU, alternating, two runs each (frames
23,846 in all):

| | unlimited | `--speed=1` |
|---|---|---|
| headless | 102.5 / 102.2% | 99.9 / 99.8% |
| visible window | 101.8 / 101.6% | 99.7 / 99.8% |
| per wall second, all eight | median 100-102%, worst 92-93% | |

The real page in a visible window, keys sent through Chrome, walking on
Shattered Island for 130 s: **readout median 99%, p10 97%, worst 92%,
no second below 90%.**

- **A visible window costs about half a point, and nothing once the
  speed is limited.** Presenting is not the gap.
- **The limit costs nothing beyond the clamp.** Dolphin forgives a lag
  only once it passes `MAIN_MAX_FALLBACK` (100 ms); dips shorter than that
  are made up, and the throttled runs' seconds look like the unlimited
  ones clamped at 100.
- **The ~80% was the build.** It was the M5 build, before the UID cache
  (#97) and the recompiler work (#98); that build measured 91% walking
  (worst second 82%) in this same window. `web/dist` was still that
  build until rebuilt. What is left unexplained is the player's machine
  and session (their Chrome profile, other tabs, a hot laptop) and scenes
  past this path, which only a replay of the current build can show.

### Busy scenes: paired singles as vectors

A player's busy scenes (the first level's vortex, chests, fights) ran
60-70% on their laptop. On their save states in the harness
(`--load-state`), the CPU thread was busy 99%, 87% of it in compiled
guest code, and the hottest blocks were the game's skinning loops:
`psq_l` / `ps_madds0` / `psq_st`, ~5,800 machine instructions for ~100
guest instructions. Counting that code, the cost was not loads and stores
of guest registers (~300 of them) but each paired single done one half
at a time through i64 locals: ~550 moves between integer and float
registers, the conversions twice, the byte swaps twice.

A paired single is two doubles side by side in guest state, the layout
of an `f64x2`. With `StateLayout::simd_ps` (on; `--jit-no-simd` for A/B):

- `psq_l` loads both halves with one vector load, swaps bytes with one
  shuffle and converts with `f64x2.promote_low_f32x4`, one store. A float
  pair holding an infinity or a NaN takes the bit-by-bit path. Integer
  types extend, convert and scale as vectors.
- `psq_st` does `ConvertToSingleFTZ` on both lanes with integer vector
  operations (exact for every input, no branch), or clamps, truncates and
  narrows the integer types.
- At Dolphin's JIT accuracy (`fast_fp`, without `fprf`), the paired
  arithmetic (`ps_add`/`sub`/`mul`/`div`, `ps_muls0/1`, `ps_madds0/1`,
  `ps_madd`/`msub`/`nmadd`/`nmsub`) computes both halves with one vector
  operation each: Force25Bit, the rounding-tie correction (in an `if`
  taken only when a lane has a tie), the NI flush.
- `lfs` and the scalar single forms write both halves with one store.

The same bits either way. The differentials run with `--simd`. A new
section in `jitdiff.cpp` compares the vector arithmetic with the scalar
form at JIT accuracy: 6,000 single instructions (a fifth of the
multiply-adds built on a rounding tie, NI set in half) and 3,000
twelve-instruction blocks that mix the vector forms with merges, moves,
scalar arithmetic and quantized loads and stores, comparing registers,
FPSCR and memory. Mutants caught: the FTZ bound, a sign-extension, the
tie direction, the NI threshold and the negate mask.

Alternating runs, WebGPU, dual core:

| | before | vectors |
|---|---|---|
| vortex state, standing (27 emulated s) | 93.2 / 92.8% | **96.0 / 96.1%** |
| town gate state | 82.3 / 82.4% | **85.5 / 85.0%** |
| walking window (300-400 s) | 103.4 / 103.8% | **105.8 / 105.9%** |

Frames were the same in each pair (vortex 1,619 in the window; walking
23,846-23,847 over the run).

*Tried and not kept:* keeping each paired register in a v128 local
between instructions (write-through, so nothing flushed at an exit):
95.7 / 95.5% against 97.1 / 96.4% without, alternating. The values live
across the cold paths' calls, and the spills cost more than the loads
they save.

### Every figure's pipelines, and the shaders kept between visits

A figure the UID file had never seen compiled its pipelines the first
time it was put on: at the town gate, 1.3 s of compiling for one figure
and 2.2 s for another, with the worst second at 19-52%.
`web/make-uidcache.sh` now runs `--cycle-figures=240`: from 240 s, every
character Spyro's Adventure knows goes on the portal in turn, then each
magic item beside the last one, while `--input` presses A and B. The UID
file grows from 406 pipelines to 1,293, and swapping in a figure
compiles nothing more. Gate state, 15 s, today's file → all figures:

| | shaders / pipelines compiled in the run |
|---|---|
| figure 9 on from the start | 449 / 570 → 931 / 1,415 |
| swap to figure 9 | 452 / 573 → 934 / 1,419 |
| swap to figure 25 | **468 / 616** → **930 / 1,415** |

Compiling twice as many at boot costs time: 889 shaders take glslang
1.8 s and Tint 4.2 s on the GPU thread. So the backend now reports
`bSupportsShaderBinaries`. A shader's "binary" is its WGSL, after a
version tag (`WebGPU::kBinaryTag`), and Dolphin's shader cache keeps it
in `Cache/Shaders`, which the page keeps in IndexedDB (`web/saves.js`,
about 14 MB, not exported). A returning player skips glslang and Tint
for every shader they have met. A cache with another tag is not used,
and its shaders are compiled from source (137 of 137 on the level
capture). Dolphin's cache files carry Dolphin's revision, not this
backend's, so the tag must be bumped whenever the WGSL a shader becomes
changes.

**The Metal cache, and pipelines built in the background.** One reload
in nine took 44 s to bring the backend up. The macOS log for that minute
showed Chrome's GPU process compiling 467 Metal shaders from source;
every other run hit the system's Metal cache (shared by every Chrome
profile, so a fresh profile does not empty it). Chrome for Testing has
its own Metal cache, which a test can empty. With it empty, as on a
player's first visit, the boot on main froze the game for 30 s, and
this branch's first version for 50 s:

- With no compiler threads, Dolphin compiled the shaders at boot but
  left every pipeline of the UID file to the first frame, and built them
  there one after another, each waiting for the browser's answer: ~45 ms
  apiece on an empty Metal cache. A reload, with the shaders cached, built
  them at boot instead -- the 44 s.
- The shaders reached the cache files in that first frame too, after the
  page's first save, so the next visit compiled them again.
- Metal manages ~35-40 compiles a second however they are asked for:
  `createRenderPipelineAsync` ran up to 147 at once and 2,111 still took
  56 s. Building every figure's pipelines up front cannot be fast on a
  machine that has never run the game.

So `GFX_WAIT_FOR_SHADERS_BEFORE_STARTING` is set (the boot finishes its
shaders, and writes them, before the game starts), and the backend no
longer builds a pipeline when Dolphin creates one. `Pipeline::Create`
checks the description and returns. The browser's pipeline is built at
the first draw that needs it (one synchronous compile: ~2 ms with the
Metal cache warm), or in the background before that:
`Pipeline::BuildInBackground`, once a frame, keeps 4 asynchronous
requests in flight, so a draw is never queued behind hundreds.

Launch to the first second at ≥50% speed, in the page (Chrome for
Testing, its Metal cache emptied, fresh profile, then reloads):

| | first visit | reloads |
|---|---|---|
| main (406 pipelines) | 38 s (frozen 30 s) | 6-7 s |
| every figure, built at boot | 52 s (frozen ~40 s) | 4-5 s |
| every figure, built in the background | **12 s**, never frozen (worst seconds 65%, 75%) | **3-4 s** |

In the player's own Chrome (Metal cache warm), the backend comes up in
7.4 s on the first visit and 0.57 s after, against 3.2 s on every visit
before. The 1,253 pipelines of the boot's set are built in the first ~5
s of play; vortex state, alternating with main: 95.6 / 95.9% against
96.4 / 96.2% from 3 s, and 98.8 / 99.1% against 98.9 / 99.0% from 10 s,
so the cost is in those seconds only. A figure swap at the town gate
compiles for 55 ms instead of 96 ms.

The frames are the same bits. M3 gives the same numbers as before on all
four captures, and its captures replayed with their own cache gave
identical frames to a replay compiled from source (max difference 0,
three scenes).

The saves poll now reads a kept file only when its size or time has
changed since it was stored, so 14 MB is not copied and compared every
10 s.

*Tried and not kept:* compiling at boot on worker threads (Dolphin's
precompiler, with the module and pipeline made later on the GPU thread,
the only one that may touch the device). With six workers, glslang and
Tint each became 10-17 times slower per shader: they allocate heavily,
and dlmalloc has one lock. The backend came up in 18.5 s instead of 8.9 s.
mimalloc removed the contention (4.8 s), but a save state then peaked at
836 MB of heap instead of 489 MB, at the same speed (96.8% vs 97.2%).
That is 350 MB of a player's memory for 4 s at the first boot only.

### A draw waits 4 ms for its pipeline, then goes without

A 17-minute play session (sheep level, the fight; the log
`docs/HANDOFF-FIGHT-SPEED.md` §2 describes) had its seconds at 0-40%
when 10-23 shaders and pipelines were compiled in them, 40-60 ms each:
fight effects the UID file does not cover, each built the first time it
was drawn, with the GPU thread waiting for the browser's answer and the
CPU thread waiting for the GPU thread. A warm Metal cache answers in
1-4 ms; one that has never met the shader takes 25-50 ms, and Metal
manages ~40 a second however they are asked for.

Now every pipeline is asked for asynchronously. A draw whose pipeline is
not built asks for it ahead of the background's queue and waits up to
`Context::pipeline_wait_ns` (4 ms) for the answer; past that the draw is
skipped and a later frame draws it, once the answer has come with a
frame's yield. The wait is paid once per pipeline, not per draw or per
frame, and the background sends nothing more while a draw's pipeline is
in flight, so Metal's compilers are its alone. A FIFO replay (M3) waits
for every pipeline, as before: it compares frames.

The fight state, Chrome for Testing with its Metal cache emptied before
each run (a first visit's compiles), window from 3 s, one run each:

| wait for a pipeline | window | frames over 100 ms | CPU thread waited | worst second |
|---|---|---|---|---|
| until built (before) | 55.8% | 15, 7.0 s | 6.8 s (22%) | 12% |
| 8 ms | 64.6% | 3, 0.8 s | 1.1 s | 41% |
| **4 ms** | **66.2%** | **2, 0.4 s** | **0.7 s** | **46%** |
| 0 ms (never) | 65.3% | 1, 0.2 s | 0.4 s | 46% |

Cold, the 388 new pipelines of the fight scene arrive at Metal's pace:
about 20,000 of the run's 300,000 draws go without one, the scene
filling in over the first seconds instead of frozen for them. What is
left of the cold cost is the 117 new shaders, still compiled on the GPU
thread (glslang, Tint and the module: 1.3 s over the run), and Chrome's
GPU process compiling Metal shaders on the same four cores.

Warm (the player's own Chrome, the fight again): until built 77.4%,
382 ms waited; 4 ms 76.4%, 381 ms waited, 15 draws skipped over the
run, every wait under 4 ms (63 under 1 ms, 82 under 2, 184 under 4).
At 8 ms nothing was skipped and 17 waits took 4-8 ms, so 4 ms draws
about 5% of a warm scene's new pipelines one frame late and halves
what a cold scene pays in waits. Vortex 95.8 → 95.0%, gate 82.1 →
81.9%, walking 115.5 → 114.6%; M3 passes on all four captures.

`--webgpu-pipeline-wait=MS|forever` sets the bound; `--frame-times`
prints the waits by doubling milliseconds and the draws skipped.
`CHROME=<binary>` points the harness at Chrome for Testing for a cold
run (its Metal cache is `$(getconf DARWIN_USER_CACHE_DIR)com.google.chrome.for.testing*/com.apple.metal`;
the player's Chrome's is never touched).

### Hot blocks optimised sooner: the warm entry

The second item of `docs/HANDOFF-FIGHT-SPEED.md` §4. With nothing
compiling, the fight ran 77-78% in the harness window (3-20 emulated s
from the player's save state) and `--wasm-tiering-budget=100000` made
that 89.7%; in a later window, 30-50 s, 119.2% against 129.0%. A player
cannot pass V8 flags, so this is about how the blocks are packaged.

*How Chrome decides.* V8 15.6 (Chrome 156) compiles a wasm function
with Liftoff and recompiles it with TurboFan once a per-function budget
of 13,000,000 is spent. The budget is charged in machine-code bytes
executed, approximately: at every `return` and tail call, the offset of
that instruction in the function's code (its size up to there) plus 60;
at a loop back-edge, the distance jumped. Measured here with
`harness/wasm-core/tierup-probe/` (a page that builds a function, calls
it, and asks `%IsTurboFanFunction` under `--allow-natives-syntax`: a
flag for the measurement, never for a player). A block of this
recompiler averages 9 guest instructions and about 2 KB of Liftoff code
(1.25-2.9 bytes per wasm byte over the fight's hot blocks, 1.9 in the
middle, Chrome and node's Liftoff agreeing;
`harness/wasm-core/liftoff-size.mjs`), so it is optimised after some
six thousand executions -- and the fight runs 42,000 blocks, a flat
profile (the top 500 are 39% of block time), most of them a few
thousand times in twenty seconds. Most of a fight runs in Liftoff code.
Compilation hints (`metadata.code.compilation_priority`) are ignored by
Chrome 156, with or without `--experimental-wasm-compilation-hints`.

*The warm entry* (`StateLayout::warm_entry`). Every block begins with
one compare and branch: a NEGATIVE state argument, -K, runs none of the
block -- a loop around the block goes round K times with the argument
as its counter, each back-edge jumping over every byte of the block's
code and charging all of it, and the block returns 0. One call from
the dispatcher spends a block's budget in K passes of about 2.5 ns, and
V8 optimises it in the background (a batch of 27 blocks measured
optimised within 6 ms). K comes from the wasm body size and an assumed
1.4 bytes of code per byte; a block estimated above its real size gets
more passes than it needs (a second trip of the budget is ignored), one
estimated below is finished by its own runs.

*Which blocks, measured on the fight* (window 3-20 s, one run per cell,
noise about ±1 here; `--jit-warm`):

| policy | window | passes over the run |
|---|---|---|
| off (four runs) | 77.9-78.4% | |
| the entry test alone (`check`) | 77.7% | 0 |
| warm a block on the 1st chain that starts at it (`1`) | 83.3% | 15,187 blocks, 1.35 s |
| ... on the 2nd (`2`) | 81.5% | 11,543 blocks, 0.9 s |
| every block when installed (`install`), 1.4 bytes per byte assumed | 87.3% | 37,657 blocks, 552M passes, 2.9 s |
| ... 1.8 assumed | 87.0 / 86.7% | 439M passes, 2.4 s |
| ... 2.2 assumed | 85.7% | 364M passes, 2.1 s |
| `--wasm-tiering-budget=100000` (reference) | 89.7% | |

A chain starts where a slice ended or a link missed, which samples
blocks by the time spent in them -- but warming every block at install
beat it, even at 2.4 s of passes over the run: what matters is breadth
and earliness, not picking. Where the passes go: 62% to blocks under
512 bytes of wasm (a third of the blocks; a block of 300 bytes needs
28,000 passes, 150 µs), 89% to blocks under a kilobyte, most of them
cold.

Where the passes go also says where a cap would save: capped at 2,000,
4,000 or 8,000 passes at install, with the blocks a chain starts at
topped up to their whole budget afterwards, the window was 85.2, 83.2
and 86.3%; capped at 4,000 without the top-up, 81.7%. The small blocks
are worth their passes. So every block is warmed fully at install, and
the other policies stay behind flags for another machine to re-measure.

The first version of the warm path tail-called the block itself for
each pass (a direct-call hole, filled like a sibling's), which pays a
function entry and V8's stack check per pass: 5.3 ns, 2.9 s over the
run. The loop around the block: 2.4 ns, 1.35 s. The window was the same
(87.3 / 87.3% against 87.4%); the whole run, whose first three seconds
compile and warm most of the scene, 79.0% against 75.8%, and its worst
second 64-65% against 59%. Liftoff spills a function's locals at a loop
head and keeps them on the stack through the body, which the window
says costs nothing measurable.

*The result* (`--jit-warm=install`, the default, against `off`; each
pair alternated, one run each):

| scene | off | warmed at install | passes over the run |
|---|---|---|---|
| the fight, 3-20 s | 77.7% (median second 79%, p10 67%) | **87.7%** (88%, 79%) | 37,665 blocks, 1.3 s |
| the vortex (`s941.sav`), 3-30 s | 96.8% | **106.2%** | 26,925 blocks, 1.0 s |
| the town gate, 3-20 s | 83.2% | **91.0%** | 36,086 blocks, 1.3 s |
| walking, 300-400 s | 114.6% | 113.8% | 51,790 blocks, 1.8 s |

Frames identical in every pair (1197, 1799, 1198, 23846). The walking
window is a scene whose blocks were long since optimised either way, so
it shows what the entry test costs: nothing beyond that window's noise
(±3). The whole fight run from the save state, first seconds included,
72.5 → 79.6%, its worst second 60 → 65%. M3 passes on all four
captures; the differentials pass with and without the entry.

*Flags.* `--jit-warm=install[+N]|N|check|off` (the default is `install`),
`--jit-warm-cap=P` (passes at install at most), `--jit-warm-ratio=R`
and `--jit-warm-hot-ratio=R` (bytes of code assumed per wasm byte, at
install and for a block that started a chain), `--jit-warm-budget=B`.
The `jit warm` line of a run says how many blocks were warmed, topped
up, how many passes and milliseconds, and the passes by body size.
`run-jitdiff.sh --warm` compiles every differential's blocks with the
entry and calls each hand-written case with a negative state first.

### The memory path: the cold code was the cost, and a hint moves it

The third item of `docs/HANDOFF-FIGHT-SPEED.md` §4 called the memory path
"~14 wasm instructions per guest access with a dependent table load".
Measured first, from the machine code TurboFan makes of five hot fight
blocks (`--jit-dump`, `harness/wasm-core/jit-asm.mjs`): the table's
address is kept in a register for the whole block, so a translation is

```
lsr  w9, w2, #25            ; granule
add  w9, w10, w9, lsl #2    ; the entry's address
ldr  w9, [x1, x9]           ; the entry -- a load the data address waits for
cbnz w9, hit
...                         ; the miss path: ~45 instructions, inline
hit: and w13, w2, #0x1ffffff
add  w9, w9, w13            ; host address
```

six instructions and one dependent load, then the access itself (for a
word store: a register load, four for the byte swap, the store). The
spills the cold paths' calls force are per block, not per access.

*Two changes to the hot path, neither of which mattered.* Both exact,
both measured on the fight state (window 3-20 s, the four variants in
turn, two rounds):

- **Direct translation** (`--jit-mem=direct`, `StateLayout::direct_base`):
  the table's answer computed instead of loaded -- the top bit as the
  tier wants it, bit `(ea >> 25) & 31` of a mask of the RAM granules,
  `base + (ea & 0x03FFFFFF)` plus MEM2's distance from MEM1 when bit 28 is
  set. About nine instructions, no load, the same miss path.
- **Access groups** (`--jit-groups=on`, `StateLayout::mem_groups`): a
  block's displacement-form accesses through one base register share one
  translation. The block is compiled twice: the first pass records every
  instruction and its access, `BuildMemPlan` turns the record into roles,
  the second pass emits them. The group's first access translates as
  usual and then, in four instructions (`add; eor; tst; csel`), parks the
  host address of the group's lowest address in a local -- when the span
  lies in one 32 MB granule and is not the locked cache -- or zero. Each
  later member tests the local and adds its offset: `cbnz; add`. A member
  whose leader left zero translates on its own, so the pipe, the hardware
  registers and a bail behave as before. An update form and an `addi` to
  the register itself fold their constants into the offsets; any other
  write to the register ends the group (`GprWrites`, which ends every
  group at an instruction it does not know). Up to four groups a block.

| | table (before) | direct | groups | direct + groups |
|---|---|---|---|---|
| fight, 3-20 s | 86.7 / 87.3% | 87.9 / 87.2% | 87.1 / 86.7% | 87.6 / 87.4% |

Nothing, inside the noise, with 16,460 of 37,660 blocks carrying 24,200
groups. The instructions an access executes were not the limit.

*What was.* Three probes with flags that already existed, one run each:

| probe | fight | what it changes |
|---|---|---|
| none | 87.1-87.6% | |
| `--jit-no-gp` | 84.8% | the inline gather-pipe path removed from every store; those stores (1.5M a second) go through the hatch |
| `--jit-no-l1` | 71.0% | the locked-cache path removed; the fight keeps a lot in the locked cache, and every access there bails |
| `--jit-probe-pad` | 73.0% | one more never-taken copy of the miss path behind every access |

The pad costs a seventh of the speed for code that never runs. TurboFan
lays a wasm `if` out in program order, so each access's ~45 instructions
of miss path sat between its hot instructions and the next access's: the
hot path of a block was spread over three times its own length of code,
and a fight runs ten thousand blocks a second, most of them a few times.

*The hint.* The branch-hinting proposal's custom section
(`metadata.code.branch_hint`: per function, the byte offset of an `if` or
`br_if` and whether its condition is likely true) is decoded by V8 15.6
with no flag -- Chrome 156 honours it; node 22 needs
`--experimental-wasm-branch-hinting`. With `StateLayout::branch_hints`
(`--jit-hints`, the default), every branch to a bail, a translation miss,
a hatch, an exit or a float special case is hinted unlikely, and the
link epilogue's hit and a group member's short path likely. The emitters
mark a hint just before pushing the opcode, through a sink the compile
installs (`HintUnlikely`); the offsets move with the link and warm
wrapping as the direct-call holes do, and the module builder writes the
section before the code section with the locals declaration's length
added. The code is the same; TurboFan now puts every cold block after the
function's hot path and keeps the hot path's registers out of the stack:
a grouped store is `cbz; add; ldr; and; and; ror; orr; str`, nothing
between it and the next.

| | off | hints | hints + groups | hints + groups + direct |
|---|---|---|---|---|
| fight, window 3-20 s | 87.1 / 87.6% | **103.9 / 103.8%** | **104.9 / 105.4%** | 105.0 / 104.8% |
| median second | 87 / 88% | 105 / 105% | 107 / 107% | 107 / 105% |
| p25 | 82 / 81% | 100 / 100% | 102 / 101% | 101 / 103% |
| whole run from the save state | 79.1 / 79.1% | 91.5 / 91.2% | 91.4 / 91.5% | 91.1 / 91.0% |
| worst second | 66% | 73-75% | 71% | 71% |

Frames identical (1,197) in every run. The hint is worth sixteen points
on the window and twelve on the whole run; the groups a point more on
the window, consistent in both rounds and inside the noise of either;
the direct translation nothing, so it stays a flag. Hints and groups are
the defaults.

| scene | off | hints + groups |
|---|---|---|
| the vortex, 3-30 s | 105.4% | 127.0% |
| the town gate, 3-20 s | 92.7% | 110.4% |
| walking, 300-400 s | 114.6% | 134.0% |

Frames identical in every pair (1,799, 1,197 and 23,846). Walking, whose blocks are optimised either way, gains as much as the fight: the layout was the cost everywhere, not the tier.

*Checks.* The differentials pass with `--hints`, `--groups` and `--direct`
in every combination with the existing flags, with five new hand-written
cases (a stack frame's seven accesses, a load into the base register, a
span over a granule edge that must fall back and bail, float members, a
misaligned float member); the nightly now runs them with the dispatcher's
defaults, `--warm --hints --groups` included. M3 passes on all four captures. A hint is layout
only, so a wrong one costs speed and never correctness; V8 ignores a
malformed section silently, which is why `jit-asm.mjs` under node with the
flag is the check that the cold code moved; `harness/wasm-core/dumpblock.cpp` compiles a program natively and writes its module for that, or for `wasm2wat`.

### The transition: exits to earlier blocks bound as imports

The fifth item of `docs/HANDOFF-FIGHT-SPEED.md` §4 named the dispatcher,
6.5% of the CPU thread in the profile. The profile charges a block's exit
to the block, and that is where the transition's cost was. A block ends
with a tail call to the next: a `return_call` when the target is in the
same module (a sibling of the region), otherwise a lookup in the link
table and a `return_call_indirect` through the function table. From the
machine code of a fight block, the second is

```
ldr  w9, [x8, #772]; sub; str      ; the downcount
cmp  w9, #0; b.le                  ; the budget
movz/movk; ldr w8, [x1, x8]        ; the link table's slot, folded to a constant
movn/movk; cmp w8, w9; b.ne        ; the stored PC against this exit's
movz/movk; ldr w1, [x1, x6]        ; the function index
ldur x8; ldur w10; madd; ldur x6   ; the function table's length and base
cmp  w1, w10; b.hs                 ; its bounds check
ldr  w6; ldr w1; cmp; b.ne         ; the signature check
ldr  x7; ldr x1                    ; the callee's instance and entry
ldp; add sp; br x17                ; the tail call
```

thirty-five instructions before the callee's prologue, where the sibling
call is a jump. A fight makes 48 million transitions a second, and once
the game is warm most of a new region's exits lead to blocks compiled
before it, in other modules, which no sibling call can reach: of the
fight's 97,782 exit holes, 55,022 named a sibling.

*What changes.* A region's exits to blocks already compiled are bound as
imports of its module (`ModuleBuilder::ImportBlock`, typed as the block
signature; `--jit-imports`, the default): the hole becomes a
`return_call` of the import, which V8 compiles as a load of the callee's
entry and a jump, and `JitInstallBatch` binds each import to the earlier
block's exported function (`env.b0`, `env.b1`, ... by table index). The
fallback to the table stays for targets compiled later -- a module is
immutable once instantiated -- and for the dynamic exits (`blr`, `bcctr`),
whose targets are not known when the block is compiled. Over the fight:
25,017 more holes bound, through 16,277 imports, 3.3 a module.

| | off | imports |
|---|---|---|
| fight, 3-20 s | 104.2% (statistics build 95.9 / 95.8%) | 106.1% (99.0 / 98.6%) |
| the vortex, 3-30 s | 127.3% | 130.0% |
| the town gate, 3-20 s | 109.1% | 110.8% |
| walking, 300-400 s | 133.6 / 133.6% | 138.6 / 138.2% |

Frames identical in every pair. M3 passes on all four captures. The
chain differential now splits a third of its chains over two modules,
the second calling the first through imports, and passes.

*The call, in machine code* (a fight block from a run, `jit-asm.mjs`): an
exit bound to an import is

```
ldur x1, [x6, #7]; ldur x7, [x1, #39]   ; the import's instance
ldur x1, [x6, #31]; ldur x1, [x1, #39]  ; its entry
ldp fp, lr, [fp]; add sp, sp, #0xa0     ; this frame
mov x17, x1; br x17                     ; the tail call
```

nine instructions for the thirty-five of the table path: no lookup, no
bounds check, no signature check.

The walking pairs are off, on, on, off in one sitting: a single pair on
this scene moved 6-9 points with the run order alone (the second of two
long runs slower, the battery charging), and two pairs in opposite orders
said -9 and +6; four runs in a row say +5 twice, with the two offs equal.
### Guest registers in locals

The fourth item of `docs/HANDOFF-FIGHT-SPEED.md` §4. Every guest register
lived in `PowerPCState`: an instruction loaded its operands from memory
and stored its result there, so a dependent chain of guest instructions
paid a store-to-load forward at every link, and a block's hot path was
two loads and a store per instruction before it computed anything.

*What changes* (`StateLayout::reg_cache`, `--jit-regs`): a block keeps
the general-purpose registers in wasm locals, one per register after the
vector locals, and reads them from there. Writes are write-through -- the
value goes into the local AND into memory -- so memory is always current
and no exit has anything to flush: a bail, a tail call to the next block,
the link epilogue all leave as before. The registers an instruction
reads are brought into their locals at the top level before it
(`GprReads`, `PrefetchGpr`), once per block, so a read inside any depth of
control flow finds the local current; a read of a register not in its
local goes to memory, which is always right. Where the interpreter runs an
instruction in the middle of a block -- a hatch, the memory helper -- the
registers it may have written (`GprWrites`) are reloaded on that path,
and an instruction whose writes it cannot name leaves every local
untrusted for the rest of the block. TurboFan turns the locals into
SSA values and so into machine registers, spilling what does not fit.

*Measured.* Fight state, window 3-20 s, each pair alternated, two
rounds:

| | off | registers in locals |
|---|---|---|
| fight, 3-20 s | 104.9 / 104.9% | 106.4 / 106.0% |
| median second | 107% | 108% |
| whole run | 91.5 / 91.4% | 91.9 / 91.4% |

| scene | off | on |
|---|---|---|
| the vortex, 3-30 s | 127.5% | 129.8% |
| the town gate, 3-20 s | 108.5% | 111.6% |
| walking, 300-400 s (off, on, on, off) | 132.7 / 132.2% | 138.0 / 138.3% |

Frames identical in every pair. The walking runs are off, on, on, off in
one sitting, because a single pair on this scene moves several points
with the run order alone (the second long run slower). From the machine
code of a hot fight block: the hot path loses half its loads from the
register file (80 to 39) and a tenth of its length; TurboFan's compile
takes 6-8% longer; the cold reloads after a hatch add to the code that
never runs. M3 passes on all four captures. The differentials pass with `--regs` in every
combination with the existing flags; the random blocks and chains mix
hatched instructions with translated ones, which is where a stale local
would show. `--jit-regs=off` is the old code.

## 6. Risks

- **JSPI on a pthread** (§4) — the one that could reshape the design. S1.
- **Readback latency** — a GPU round trip per EFB peek; if the game peeks
  every frame, this is a fixed cost per frame in the GPU thread's budget.
  Dolphin's EFB peek cache exists for this and is already in VideoCommon.
- **Shader translation coverage and time** — S2.
- **The CPU margin is thin.** 4 points. Any cost the backend adds to the GPU
  thread past 1.0 s per emulated second shows up as speed lost.
- **The Null backend copies every EFB/XFB to RAM**; a backend that copies to
  VRAM skips those (`bSkipEFBCopyToRam`, `bSkipXFBCopyToRam` default on), so
  some GPU-thread costs measured on Null go away and others arrive.
