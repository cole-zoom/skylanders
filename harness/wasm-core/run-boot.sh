#!/usr/bin/env bash
# Boot the game on both targets and diff the instruction traces.
#
# This is the Phase 1 gate from docs/PLAN.md §5: "the game's boot sequence
# executes under the interpreter and CPU state matches a captured native trace
# instruction for instruction."
set -euo pipefail
cd "$(dirname "$0")/../.."
ISO="${1:-/tmp/ssa.iso}"
STEPS="${2:-20000}"
[ -f "$ISO" ] || { echo "no disc at $ISO" >&2; exit 77; }

echo "══ NATIVE ══"
harness/wasm-core/link-native.sh harness/wasm-core/bootgame.cpp /tmp/bootgame-native >/dev/null
/tmp/bootgame-native "$ISO" "$STEPS" --trace > /tmp/boot-native.txt 2>&1 || true
grep -vE '^(T |[0-9]{2}:)' /tmp/boot-native.txt | tail -12

echo
echo "══ BROWSER ══"
harness/wasm-core/link.sh harness/wasm-core/bootgame.cpp /tmp/bootgame.js >/dev/null
mkdir -p /tmp/bootweb && cp /tmp/bootgame.js /tmp/bootgame.wasm /tmp/bootweb/
cat > /tmp/bootweb/index.html <<HTML
<!doctype html><meta charset=utf8><title>booting Skylanders</title>
<pre id=out>running…</pre>
<script>
// Output is accumulated in an array the runner drains INCREMENTALLY.
//
// The DOM is only a human-visible tail. Rendering every line into it, and
// having the runner read the whole thing each poll, meant shipping a growing
// buffer across CDP twice a second -- at 20k trace lines that is megabytes per
// poll, and the page died partway through a run. The trace it had produced up
// to that point was correct, so the failure looked like an emulation
// divergence when it was only a transport that could not keep up.
window.__lines = [];
window.__done = false;
const tail = () => {
  const l = window.__lines;
  document.getElementById('out').textContent = l.slice(-40).join('\n');
};
var Module = {
  arguments: ['http://127.0.0.1:8101/', '$STEPS', '--trace'],
  print:    (t) => { window.__lines.push(t); if (window.__lines.length % 500 === 0) tail(); },
  printErr: (t) => { window.__lines.push('! ' + t); },
  onExit:   (c) => { window.__lines.push('EXIT ' + c); window.__done = true; tail(); },
};
</script>
<script src="bootgame.js"></script>
HTML
node harness/wasm-core/run-boot-browser.mjs > /tmp/boot-wasm.txt 2>&1 || true
grep -vE '^(T |[0-9]{2}:)' /tmp/boot-wasm.txt | tail -12

echo
echo "══ TRACE DIFFERENTIAL ══"
python3 harness/wasm-core/difftrace.py /tmp/boot-native.txt /tmp/boot-wasm.txt
