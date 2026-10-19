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
const lines = [];
const flush = () => document.getElementById('out').textContent = lines.join('\n');
var Module = {
  arguments: ['http://127.0.0.1:8101/', '$STEPS', '--trace'],
  // Flush every line until stepping starts. Batching from the beginning hides
  // the setup phase entirely, which is exactly where a browser boot stalls.
  print:    (t) => { lines.push(t); if (lines.length < 400 || lines.length % 2000 === 0) flush(); },
  printErr: (t) => { lines.push('! ' + t); },
  onExit:   (c) => { lines.push('EXIT ' + c); flush(); },
};
</script>
<script src="bootgame.js"></script>
HTML
node harness/wasm-core/run-boot-browser.mjs > /tmp/boot-wasm.txt 2>&1 || true
grep -vE '^(T |[0-9]{2}:)' /tmp/boot-wasm.txt | tail -12

echo
echo "══ TRACE DIFFERENTIAL ══"
python3 harness/wasm-core/difftrace.py /tmp/boot-native.txt /tmp/boot-wasm.txt
