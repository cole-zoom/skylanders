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
# The heap size is not optional. Each trial installs a module and the table
# holds a reference to every one of them -- roughly nine thousand by the end
# -- and on the default heap the run dies partway through the block
# differential with "Worker terminated due to reaching memory limit". That
# reads like a harness crash rather than an out-of-memory, and it lands in
# the middle of a passing run, so the sections after it simply never report.
set -euo pipefail
cd "$(dirname "$0")/../.."

OUT="${1:-/tmp/jitdiff.js}"
harness/wasm-core/link.sh harness/wasm-core/jitdiff.cpp "$OUT"
exec node --max-old-space-size=8192 "$OUT"
