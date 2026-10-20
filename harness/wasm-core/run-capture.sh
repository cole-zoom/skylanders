#!/usr/bin/env bash
# Capture the executed GX command stream on both targets and compare them.
#
# This is the only check that covers the GPU-FACING path. Every other
# differential here compares CPU state -- registers, PCs, the first FIFO write
# -- and none of them would notice a browser that executed identical
# instructions while sending different commands to the GPU.
#
# The stream captured is what the GPU executed, with display lists expanded
# (patch 0009), so it is the same shape Dolphin's FifoRecorder writes and the
# existing decoder reads it unchanged.
#
# Usage:  run-capture.sh [disc] [bytes]
set -euo pipefail
cd "$(dirname "$0")/../.."
ISO="${1:-/tmp/ssa.iso}"
BYTES="${2:-262144}"
# A fixed step count makes the two runs directly comparable: whichever limit
# is hit first stops the run, so capturing to a BYTE limit lets the two sides
# stop at different instruction counts and the streams can differ purely from
# where they were cut. Bounding by steps removes that variable.
STEPS="${3:-20000000000}"
# Extra flags passed to BOTH targets, e.g. --input. Both, always: a flag given
# to one side only makes the two runs different by construction, which is how
# the Wiimote and network divergences happened.
EXTRA="${4:-}"
[ -f "$ISO" ] || { echo "no disc at $ISO" >&2; exit 77; }

echo "══ NATIVE ══"
harness/wasm-core/link-native.sh harness/wasm-core/bootgame.cpp /tmp/bootgame-native >/dev/null
/tmp/bootgame-native "$ISO" "$STEPS" "--capture-gx=$BYTES" $EXTRA > /tmp/gx-native.txt 2>&1 || true
grep -E "gx cap|executed|disc |PASSED|FAILED" /tmp/gx-native.txt || true

echo
# Build the JS argument fragment here rather than inside the heredoc. An
# attempt to do it with a command substitution inside the heredoc produced the
# LITERAL text $f as an argument, so the browser silently ran without the
# flags native had -- the exact one-sided configuration this script's EXTRA
# comment warns about, introduced while writing that comment.
EXTRA_JS=""
for f in $EXTRA; do EXTRA_JS="$EXTRA_JS, '$f'"; done

echo "══ BROWSER ══"
harness/wasm-core/link.sh harness/wasm-core/bootgame.cpp /tmp/bootgame.js >/dev/null
mkdir -p /tmp/bootweb && cp /tmp/bootgame.js /tmp/bootgame.wasm /tmp/bootweb/
cat > /tmp/bootweb/index.html <<HTML
<!doctype html><meta charset=utf8><title>Skylanders GX capture</title>
<pre id=out>running…</pre>
<script>
window.__lines = [];
window.__done = false;
const tail = () => {
  document.getElementById('out').textContent = window.__lines.slice(-30).join('\n');
};
var Module = {
  arguments: ['http://127.0.0.1:8101/', '$STEPS', '--capture-gx=$BYTES'$EXTRA_JS],
  print:    (t) => { window.__lines.push(t); if (window.__lines.length % 200 === 0) tail(); },
  printErr: (t) => { window.__lines.push('! ' + t); },
  onExit:   (c) => { window.__lines.push('EXIT ' + c); window.__done = true; tail(); },
  // Take the stage pointer exactly once, here. Calling an exported function
  // before the runtime is up ABORTS the module, and Module.calledRun is not a
  // usable guard under PROXY_TO_PTHREAD -- main() runs on a worker, so it
  // never becomes true on this thread. The poller needs this pointer: during
  // the stepping phase it is the ONLY sign the run is alive, because nothing
  // is printed for minutes at a time.
  onRuntimeInitialized: () => {
    try { window.__stagePtr = Module._harness_stage(); } catch (e) {}
  },
};
</script>
<script src="bootgame.js"></script>
HTML
node harness/wasm-core/run-boot-browser.mjs > /tmp/gx-wasm.txt 2>&1 || true
grep -E "gx cap|executed|PASSED|FAILED|STALLED|TIMEOUT|PAGE LOST" /tmp/gx-wasm.txt || true

echo
echo "══ COMMAND STREAM DIFFERENTIAL ══"
python3 harness/src/decode_capture.py /tmp/gx-native.txt /tmp/gx-wasm.txt
