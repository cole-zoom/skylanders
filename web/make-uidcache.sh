#!/usr/bin/env bash
# Make web/dist/SSPE52.uidcache: every pipeline the game draws with on the
# harness's path (boot, intro, the first level, walking), recorded by
# Dolphin's shader cache. The page hands it to the emulator, which compiles
# them all at boot (about 3.5 s) instead of stalling the first time each is
# drawn -- up to 0.9 s at once in the intro, with Eon speaking.
#
# One headless run of about 6 minutes. Needs what harness/wasm-core/
# run-jit.sh needs (the disc at /tmp/ssa.iso). The file is generated, not
# committed: it is a record of the game's render states.
set -euo pipefail
cd "$(dirname "$0")/.."
LOG="$(mktemp)"
trap 'rm -f "$LOG"' EXIT
WEBGPU=1 harness/wasm-core/run-jit.sh 300000000000 --jit --dual-core --no-osd --input=A \
  --skylander --ticks=291600000000 --webgpu --walk-from=218700000000 --dump-uidcache > "$LOG" 2>&1
node web/extract-uidcache.mjs "$LOG" web/dist
