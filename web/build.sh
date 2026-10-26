#!/usr/bin/env bash
# Build the browser shell's module: the emulator with the WebGPU backend,
# into web/dist/ (gitignored). Then serve it:
#
#   web/build.sh && node web/serve.mjs /tmp/ssa.iso
#
# web/make-uidcache.sh once as well (six minutes), or the game stalls the
# first time it draws with each pipeline.
#
# The module is harness/wasm-core/bootgame.cpp, the same program every
# measurement runs; the page turns on what playing needs (--live-input,
# --speed=1). Needs the prebuilt libraries in dolphin-web/build-wasm and
# dawn/, as harness/wasm-core/link.sh says.
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p web/dist
WEBGPU=1 harness/wasm-core/link.sh harness/wasm-core/bootgame.cpp web/dist/skylanders.js
ls -l web/dist
