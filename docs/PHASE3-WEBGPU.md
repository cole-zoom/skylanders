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

**M4 — speed.** The GPU thread's busy seconds per emulated second with the
backend drawing, against the CPU thread's. *Gate:* the gameplay window at or
above 100%. Expect work here: per-draw state changes, uniform uploads, and
the readbacks of §4.

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
