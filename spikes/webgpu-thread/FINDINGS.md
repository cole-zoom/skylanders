# Spike S1 — a WebGPU device on Dolphin's GPU thread

**Status: ANSWERED, favourable, with one design constraint.** Plan:
`docs/PHASE3-WEBGPU.md` §5.

Dolphin's GPU thread is an Emscripten pthread running a blocking loop that
never returns to its worker's event loop. WebGPU is promise-based. The
question was whether that thread can own a device, draw into the page's
canvas, and block on a readback.

`s1.cpp` (one pthread, `emdawnwebgpu`, `-sJSPI`, `-sOFFSCREENCANVAS_SUPPORT`;
`build.sh`, `run.mjs`). Chrome 155, Apple M4, headless. Each blocking wait is
`wgpu::Instance::WaitAny`, which under JSPI suspends the wasm stack, lets the
worker's event loop run, and resumes.

| question | result |
|---|---|
| adapter and device on the pthread | yes — device in 50 ms |
| a surface on a canvas transferred to the pthread | yes (`emscripten_pthread_attr_settransferredcanvases`, `EmscriptenSurfaceSourceCanvasHTMLSelector`) |
| a blocking readback: clear, copy to a `MAP_READ` buffer, `MapAsync`, wait, read | yes, values correct. **~0.4 ms** a round trip once warm (a 640×528 frame: 0.38 ms mean, 0.79 worst over 50); the first one 21 ms |
| frames reach the screen while the thread never yields | **no** — 60 frames and a 300 ms busy hold left the canvas black |

## The constraint: the GPU loop must yield once per presented frame

A canvas owned by a worker shows a frame only after the worker's current
task ends. A thread inside `RunGpuLoop` never ends its task. So after each
present the loop has to suspend once, through JSPI, and let the event loop
turn. How it yields matters:

| yield after each present | cost per frame | frame shown |
|---|---|---|
| none | — | no |
| `setTimeout(0)` (a timer task) | **5.3 ms** — nested timers are clamped | yes |
| a `MessageChannel` message (a task, not clamped) | **0.021 ms** | yes |
| `await Promise.resolve()` (a microtask) | 0.007 ms | **no** — not a task |

A message task: about 21 µs a frame, ~1.3 ms a second at 60 frames. That is
the one to use. The same suspension is what lets `mapAsync` resolve, so a
readback and a present are the same mechanism.

## What this decides

- The design of `docs/PHASE3-WEBGPU.md` §4 holds: JSPI on the GPU pthread,
  device acquired before the loop starts, readbacks by blocking `WaitAny`.
- The backend's present path ends with a message-task yield.
- A readback costs ~0.4 ms. An EFB peek per frame is affordable; one per
  draw is not. How often this game peeks, and whether Dolphin's peek cache
  absorbs it, is the next thing to count on the real backend.

## Not answered here

- JSPI across the full harness: `bootgame` is built without it, and JSPI
  changes how every import that can suspend is called. Its cost on the CPU
  thread, which never suspends, is unmeasured.
- Presentation pacing: 60 frames with message yields took 1.3 ms, so frames
  are presented faster than the display refreshes and most are dropped. The
  backend presents at the game's rate; whether that needs a vsync-like wait
  is a later question.
- A canvas that is not headless (real compositing, real vsync).
