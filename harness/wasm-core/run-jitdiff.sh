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
#
#   harness/wasm-core/run-jitdiff.sh [out.js] [--compact] [--short-convert] [--simd]
#
# The flags compile as the dispatcher does by default: the slow paths as
# helper calls (StateLayout::hatch_exit_index), the float conversions
# through promote/demote (StateLayout::short_convert) and the paired
# singles as vectors (StateLayout::simd_ps). All are meant to be exact, so
# the same expectations hold either way.
set -euo pipefail
cd "$(dirname "$0")/../.."

OUT="${1:-/tmp/jitdiff.js}"
harness/wasm-core/link.sh harness/wasm-core/jitdiff.cpp "$OUT"
# A larger heap, and gc() exposed so the install helpers can collect the
# modules they replace. Reusing table slots made old instances collectable,
# but with seventy-odd templates and FP state the suite installs some 20,000
# modules and the worker's default heap filled before the collector caught
# up -- it died in the block differential with "reaching memory limit".
exec node --max-old-space-size=8192 --expose-gc "$OUT" "${@:2}"
