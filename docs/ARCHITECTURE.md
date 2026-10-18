# Architecture

Component map. For the reasoning behind these choices — the feasibility audit, the architectural
bets, the roadmap and the risk register — see [PLAN.md](PLAN.md). This file is the "what lives
where"; that one is the "why".

## Components

| Path | Language | Role |
|---|---|---|
| `ppc2wasm/` | Rust | PowerPC (Gekko/Broadway, incl. paired singles) → WebAssembly. One crate, two build targets: native for the ahead-of-time pass, `wasm32` for the runtime promotion tier. |
| `dolphin-web/` | C++ | Fork of Dolphin. Emscripten build, `VideoBackends/WebGPU`, `HttpBlobReader`, software MMU path. |
| `harness/` | TypeScript | Differential test rig. Captures CPU/FIFO/DSP/IOS traces from native Dolphin and diffs them against the browser build in CI. |
| `web/` | TypeScript | Browser shell. Worker topology, Portal UI, input, OPFS cache. |
| `spikes/` | mixed | Phase 0 experiments. Throwaway by design — each answers one question and gets deleted or promoted. |
| `dolphin/` | — | Pinned upstream, read-only. The test oracle and the fork point. Not committed; see `UPSTREAM.lock`. |
| `rea/` | — | Reverse-engineering tooling, used in Phase 2 to drive static analysis over the game binaries. Not committed. |

## Execution model

Three tiers behind one `WebAssembly.Table`:

1. **Ahead of time** — `ppc2wasm` (native) translates the game's `DOL` and `REL` code at build
   time. Covers most of what executes.
2. **Interpreter** — handles cold paths, dynamically loaded modules, and anything static analysis
   could not reach.
3. **Runtime tier** — `ppc2wasm` (`wasm32`) promotes blocks the interpreter has run repeatedly.

The dispatcher maps guest PC to a table slot and uses `call_indirect`. It does not know or care
which tier produced a given block.

## Thread topology

`Atomics.wait` throws on the browser main thread, so everything that blocks is a worker:

- **Main thread** — Portal UI and gamepad sampling. Writes input lock-free; never blocks.
  Receives frames via `transferControlToOffscreen`, so UI work cannot drop a frame.
- **CPU worker** — recompiler tiers, software MMU.
- **GPU worker** — `VideoCommon` and the WebGPU backend.
- **AudioWorklet** — DSP-HLE over a lock-free ring.
- **Storage worker** — `HttpBlobReader`, OPFS block cache, HTTP range requests against the RVZ
  chunk index.

All share one `SharedArrayBuffer` holding the 88 MB of guest RAM, the GX FIFO, and the audio ring.
Requires `Cross-Origin-Opener-Policy: same-origin` and `Cross-Origin-Embedder-Policy: require-corp`.
