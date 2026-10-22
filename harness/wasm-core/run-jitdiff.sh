#!/usr/bin/env bash
# Build and run the recompiler differentials.
#
# Four comparisons, each strictly wider than the last:
#   - hand-written cases, one per instruction form
#   - a randomised per-instruction differential
#   - a randomised BLOCK differential, including a window of guest memory
#   - a randomised CHAIN differential, which links blocks and runs them
#
# Node, not a browser. The differential needs no DOM, no disc and no fetch --
# only WebAssembly, a shared memory and a growable table, all of which node
# has. The browser runners in this directory exist because their tests need
# something node genuinely lacks (emscripten_fetch is XMLHttpRequest); this
# one does not, and a command that runs in four seconds gets run.
#
# Runs on a default node heap. It did not always: growing a fresh table slot
# per trial pinned every WebAssembly.Module and Instance, and at roughly nine
# thousand of them the run died partway through with "Worker terminated due
# to reaching memory limit" -- which reads as a harness crash rather than an
# out-of-memory, and lands mid-run so the sections after it never report.
# Each section now reuses its slots, so the instances are collectable.
set -euo pipefail
cd "$(dirname "$0")/../.."

OUT="${1:-/tmp/jitdiff.js}"
harness/wasm-core/link.sh harness/wasm-core/jitdiff.cpp "$OUT"
exec node "$OUT"
