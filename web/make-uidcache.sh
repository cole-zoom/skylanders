#!/usr/bin/env bash
# Make web/dist/SSPE52.uidcache: every pipeline the game draws with on the
# harness's path (boot, intro, the first level, walking) and with every
# figure on the portal (--cycle-figures: each character, then each magic
# item), recorded by Dolphin's shader cache. The page hands it to the
# emulator, which compiles them all at boot instead of stalling the first
# time each is drawn -- up to 0.9 s at once in the intro, with Eon speaking,
# and 1-2 s the first time a new figure is put on.
#
# Compiling their shaders (889) takes about 7 s at a browser's first boot;
# after that the page keeps their WGSL (web/saves.js, Cache/Shaders) and the
# video backend is up in under a second. The pipelines themselves are built
# in the first seconds of play (WebGPU::Pipeline::BuildInBackground).
#
# One headless run of about 8 minutes. Needs what harness/wasm-core/
# run-jit.sh needs (the disc at /tmp/ssa.iso). The file is generated, not
# committed: it is a record of the game's render states.
set -euo pipefail
cd "$(dirname "$0")/.."
LOG="$(mktemp)"
trap 'rm -f "$LOG"' EXIT
WEBGPU=1 harness/wasm-core/run-jit.sh 900000000000 --jit --dual-core --no-osd --input=A \
  --skylander --ticks=437400000000 --webgpu --walk-from=218700000000 --cycle-figures=240 \
  --dump-uidcache > "$LOG" 2>&1
node web/extract-uidcache.mjs "$LOG" web/dist
