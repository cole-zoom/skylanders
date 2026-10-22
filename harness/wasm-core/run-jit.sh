#!/usr/bin/env bash
# Run the boot in headless Chrome with chosen flags, for measurement.
#
#   harness/wasm-core/run-jit.sh 800000000 --jit
#   harness/wasm-core/run-jit.sh 800000000 --jit --jit-probe=1
#
# Separate from run-boot.sh, which builds BOTH targets and diffs their traces.
# That is the correctness gate; this is the stopwatch, and it skips the native
# half because there is nothing to compare -- the recompiler only exists on
# wasm.
#
# Needs the disc at /tmp/ssa.iso, which run-boot-browser.mjs serves on :8101.
#
# Measure at several step counts before believing any single number. Module
# installation is a ONE-OFF cost of about 22 us per block, so a short run
# reports a speed dominated by compilation rather than by execution: the same
# build measures 54.7 MIPS over 200M instructions and 117.0 MIPS over 800M.
set -euo pipefail
cd "$(dirname "$0")/../.."
STEPS="${1:?usage: run-jit.sh <steps> [flags...]}"; shift || true

harness/wasm-core/link.sh harness/wasm-core/bootgame.cpp /tmp/bootgame.js >/dev/null
mkdir -p /tmp/bootweb && cp /tmp/bootgame.js /tmp/bootgame.wasm /tmp/bootweb/

ARGS=""
for a in "$@"; do ARGS="$ARGS, '$a'"; done
cat > /tmp/bootweb/index.html <<HTML
<!doctype html><meta charset=utf8><title>booting Skylanders</title>
<pre id=out>running…</pre>
<script>
window.__lines = [];
window.__done = false;
const tail = () => {
  document.getElementById('out').textContent = window.__lines.slice(-40).join('\n');
};
var Module = {
  arguments: ['http://127.0.0.1:8101/', '$STEPS'$ARGS],
  print:    (t) => { window.__lines.push(t); if (window.__lines.length % 500 === 0) tail(); },
  printErr: (t) => { window.__lines.push('! ' + t); },
  onExit:   (c) => { window.__lines.push('EXIT ' + c); window.__done = true; tail(); },
  onRuntimeInitialized: () => {
    try { window.__stagePtr = Module._harness_stage(); } catch (e) {}
  },
};
</script>
<script src="bootgame.js"></script>
HTML
exec node harness/wasm-core/run-boot-browser.mjs
