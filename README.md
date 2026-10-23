# Skylanders on WebAssembly

Running *Skylanders: Spyro's Adventure* (Wii) entirely client-side in a browser, with all 530
figures available through an emulated Portal of Power.

No streaming. No server-side emulation. The game runs in the tab.

> **Status: Phase 3.** The game boots at 145% of real time in a browser and plays: pressing A
> takes it from the title into the tutorial level with the character on screen, at 70% of real
> time in gameplay and 88% over the first 390 emulated seconds with the GPU on its own thread
> (59% and 74% single-core), CPU-side with nothing drawn.
> See [docs/PHASE3-FINDINGS.md](docs/PHASE3-FINDINGS.md) for the current state and
> [docs/PLAN.md](docs/PLAN.md) for the plan.

## The idea

WebAssembly cannot write executable memory, so Dolphin's two recompilers have no browser
equivalent and a conventional JIT is off the table. The way through is that **this is not a Wii
emulator — it is a runtime for one known game.** Skylanders' code ships on the disc as a `DOL`
plus `REL` modules, so most of it can be translated to WebAssembly *before the browser ever runs
it*, with an interpreter and a runtime tier covering what static analysis cannot reach.

The Skylanders-specific half is already solved upstream: Dolphin emulates the USB portal and ships
a database of 530 figure variants with working MIFARE crypto. That code is platform-independent
C++ and ports for free.

## Layout

| Path | What |
|---|---|
| `docs/PLAN.md` | The engineering plan — feasibility audit, bets, roadmap, risks |
| `docs/PHASE2-PROFILE.md` | Where the recompiler's time was thought to go at 195 MIPS, and the three negative results |
| `docs/PHASE3-DENSITY.md` | The plan that followed from it — code density, staged. Superseded by the next row |
| `docs/PHASE3-FINDINGS.md` | What was actually slow, why the density plan would not have found it, what moved the boot to 145% of real time and gameplay to 70%, and the facts about gameplay speed. **The current state.** |
| `docs/PHASE2-HANDOFF.md` | Written at 54 MIPS; its methodology and landmines still hold |
| `docs/ARCHITECTURE.md` | Component map |
| `docs/COMMIT_CONVENTION.md` | Every commit explains its own reasoning. Required. |
| `ppc2wasm/` | Rust crate. PowerPC (Broadway) → WebAssembly. Built native (AOT) and `wasm32` (JIT tier). |
| `dolphin-web/` | Dolphin fork: Emscripten build, WebGPU backend, `HttpBlobReader` |
| `harness/` | Differential test rig. Native Dolphin is the oracle. |
| `web/` | Browser shell, Portal UI, input, OPFS cache |
| `spikes/` | Phase 0 de-risking experiments |

## Getting set up

```sh
./scripts/bootstrap.sh
```

Fetches pinned upstream sources (`UPSTREAM.lock`), points `commit.template` at `.gitmessage`,
and installs the `pre-commit` hook that blocks disc images and oversized blobs.

You supply your own disc dump at `skylanders-rom/`. It is gitignored and never committed —
GitHub rejects blobs over 100 MB, and distributing game images is not something this repo does.

## Contributing

Read [docs/COMMIT_CONVENTION.md](docs/COMMIT_CONVENTION.md) first. Every commit carries five
sections: what it is, design choices, why, caveats and risks, and what might go wrong. All five
are required.

## Licence

Dolphin is GPL-2.0-or-later. Anything derived from it in `dolphin-web/` inherits those
obligations. Original work in `ppc2wasm/`, `harness/` and `web/` is not yet licensed — to be
decided before the repo goes public.
