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
of a different session, so the gate is met but not by much. Item 2 is
unchanged and not yet run: `--webgpu-no-present` (frames drawn and
submitted, nothing copied to the canvas, no yield) is in the tree as its
first bisect step, and `powermetrics` needs a password. Item 3's shaves
(vertices generated straight into the gathered-write buffer; repeated
vertex-constant uploads skipped) and a SIMD conversion path in the JIT are
the next points to buy headroom.

**M5 — the browser shell.** `web/`: the page, the canvas, input, the disc
streaming that already works in the harness. Overlaps Phase 4.

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
