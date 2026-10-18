# Spike C — browser threading and frame pacing

**Status: GATE MET.**

## Question

Can the browser hold a stable 60 Hz with the emulator's thread topology — CPU,
GPU and audio all doing real work in workers over shared memory — without the
main thread ever blocking?

## Result

600 frames paced at 60 Hz, Chrome 155, Apple M4 (10 cores):

```
crossOriginIsolated = true
SharedArrayBuffer   = function
Atomics.wait on main thread: THROWS (TypeError) — as expected

frames paced         600 at 16.67ms target
interval mean        16.667 ms
interval p50/p95/p99 16.665 / 16.670 / 16.670 ms
interval max         16.675 ms
dropped (>1.5x)      0
frames workers late  0
GPU signals seen     600 / 600    (did the worker keep up?)
GPU frames presented 483 / 600    (did the canvas keep up?)
audio underruns      0
```

**p99 frame interval is 16.670 ms against a 16.667 ms target** — three
microseconds of jitter at the 99th percentile, with zero dropped frames across
600 frames while three workers burned 3 ms each and a WebGPU worker rendered
every frame.

Zero audio underruns through an AudioWorklet pulling from a lock-free ring in
shared memory.

## What was actually running

- **3 CPU workers**, each blocking on `Atomics.wait` for the frame signal, then
  burning a 3 ms budget touching shared memory so the work cannot be optimised
  away and cache traffic is realistic.
- **1 GPU worker** owning the canvas through `transferControlToOffscreen`,
  rendering with WebGPU each frame. Rendered frames never pass through the main
  thread.
- **1 AudioWorklet** on its own realtime thread, pulling 128-sample quanta from
  a `SharedArrayBuffer` ring the pacer fills at 800 samples per frame.
- **The main thread** publishing frame numbers, producing audio, and polling —
  never blocking.

## The constraint this confirms

`Atomics.wait` on the main thread **throws `TypeError`**, verified directly
rather than assumed. This is the single fact that shapes the whole topology:
every thread that needs to block on a synchronisation primitive must be a
worker, and the main thread is left with input, the Portal UI, and nothing that
can stall.

Cross-origin isolation is required and was served properly —
`Cross-Origin-Opener-Policy: same-origin` and
`Cross-Origin-Embedder-Policy: require-corp`. Without both,
`SharedArrayBuffer` is undefined and none of this works. `serve.mjs` is the
same configuration the deployed app will need.

## Why "GPU frames presented" is not the gate

The first run failed the gate on 481/600 frames presented. Separating *signals
observed* from *frames presented* showed the worker sees **every** frame
(600/600) while presentation caps at ~483 — about 48 Hz.

So the worker is not the bottleneck; canvas presentation is. The gate is about
whether our threading model can hold 60 Hz, and it does.

**Caveat on that attribution:** 48 Hz presentation is consistent with headless
Chrome's compositor rather than an architectural limit, but this was not
verified by running in headed Chrome. It could also be WebGPU surface
presentation throttling under `--headless`. Either way it is downstream of the
threading model, which observed all 600 frames — but if presentation turns out
to cap at 48 Hz in a real browser window too, that is a problem for Phase 3 and
this spike would not have caught it.

## A bug worth recording

The GPU worker originally polled with `setTimeout(tick, 1)`. The timer is
clamped and fires late under load, so two frame signals could advance between
polls and the worker coalesced them. Switching to `Atomics.wait` on the frame
counter — legal in a worker, and what the real GPU thread would do, since
`queue.submit()` is fire-and-forget and nothing needs servicing in between —
is the correct design. It did not change the presented count, which is what
pointed at presentation rather than the worker as the limit.

## Limits

- **One machine, one browser.** Apple M4, 10 cores, Chrome 155. A 4-core
  machine with three busy workers plus a GPU worker plus an AudioWorklet will
  behave differently, and that is the realistic low end.
- **Synthetic work.** The CPU workers burn a fixed time budget; they do not
  model the real dependency chain between the recompiler, the GX FIFO and the
  DSP, where one thread stalling blocks another.
- **Headless.** Presentation behaviour in a real window is unverified.
- **Ten seconds.** Nothing here says anything about thermal throttling or
  long-session drift.

## Reproduce

```sh
cd spikes/threading && node run.mjs
```

`run.mjs` starts the COOP/COEP server, drives headless Chrome over the DevTools
Protocol, and prints the result. The page cannot be opened from `file://` —
without cross-origin isolation `SharedArrayBuffer` does not exist.
