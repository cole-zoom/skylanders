# Toolchain

Versions verified working on the development machine (Apple M4, macOS 24.6).
Recorded because "it built on mine" is not reproducibility.

| Tool | Version | Installed by | Notes |
|---|---|---|---|
| Rust | 1.99.1 | `rustup` | targets: `aarch64-apple-darwin`, `wasm32-unknown-unknown` |
| Emscripten | 6.0.12 | `emsdk` at `~/emsdk` | verified with `-pthread -msimd128` |
| CMake | 4.2.3 | preinstalled | |
| Ninja | 1.13.2 | `brew` | |
| ccache | installed | `brew` | optional; `build-oracle.sh` uses it if present |
| Node | 22.18.0 | preinstalled | emsdk bundles its own 24.19.0 |
| bash | 3.2.57 | macOS system | **constrains hook scripts** — see below |

## bash 3.2

macOS ships bash 3.2.57 and there is no newer bash on `PATH`. Anything in
`.githooks/` or `scripts/` must avoid bash 4+ features:

- no `mapfile` / `readarray` — use `while IFS= read -r`
- no `${var,,}` / `${var^^}` — use `tr '[:upper:]' '[:lower:]'`
- no bare `${#arr[@]}` under `set -u` on an empty array
- no associative arrays

## Emscripten

`emsdk` lives outside the repo at `~/emsdk`. Activate per shell:

```sh
source ~/emsdk/emsdk_env.sh
```

## Oracle build

```sh
./scripts/build-oracle.sh
```

Stock unmodified Dolphin, NoGUI, Release. Its only job is to be correct so the
WebAssembly build can be diffed against it.

Four submodules are deliberately skipped — `Qt` (we build NoGUI), `FFmpeg-bin`
(frame dumping only), `libadrenotools` (Android), `mGBA/mgba` (GBA link cable).
The matching CMake flags `-DUSE_MGBA=OFF -DUSE_RETRO_ACHIEVEMENTS=OFF` must stay
in sync with that list.

**Submodule fetches must run serially.** Two concurrent `git submodule update`
processes against one worktree collide on `.git/modules/**/shallow.lock`. The
recursive walk then aborts partway and later submodules are never fetched,
which surfaces much later as CMake errors about targets that "are not built by
this project" — nothing in that message points at the real cause.
