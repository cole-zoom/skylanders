#!/usr/bin/env bash
# Run both targets to the first graphics FIFO write and compare the milestone.
#
# This is the second half of the Phase 1 gate in docs/PLAN.md §5: "Boot to the
# first rendered frame's FIFO write, verified against the oracle."
#
# run-boot.sh compares a full instruction trace, which is only affordable for
# a few tens of thousands of steps. Reaching the FIFO takes ~63 MILLION, so the
# comparison here is the milestone itself rather than every step: the step
# INDEX at which the Command Processor is configured and first written, plus
# the PC at each. Those are exact integers derived from the whole execution --
# any instruction diverging anywhere in those 63 million steps moves them.
set -euo pipefail
cd "$(dirname "$0")/../.."
ISO="${1:-/tmp/ssa.iso}"
CAP="${2:-500000000}"
[ -f "$ISO" ] || { echo "no disc at $ISO" >&2; exit 77; }

echo "══ NATIVE ══"
harness/wasm-core/link-native.sh harness/wasm-core/bootgame.cpp /tmp/bootgame-native >/dev/null
/tmp/bootgame-native "$ISO" "$CAP" --until-fifo > /tmp/fifo-native.txt 2>&1 || true
grep -E "fifo (cfg|wr)|executed|PASSED|FAILED" /tmp/fifo-native.txt || true

echo
echo "══ BROWSER ══"
harness/wasm-core/link.sh harness/wasm-core/bootgame.cpp /tmp/bootgame.js >/dev/null
mkdir -p /tmp/bootweb && cp /tmp/bootgame.js /tmp/bootgame.wasm /tmp/bootweb/
cat > /tmp/bootweb/index.html <<HTML
<!doctype html><meta charset=utf8><title>Skylanders to first FIFO write</title>
<pre id=out>running…</pre>
<script>
window.__lines = [];
window.__done = false;
const tail = () => {
  document.getElementById('out').textContent = window.__lines.slice(-40).join('\n');
};
var Module = {
  arguments: ['http://127.0.0.1:8101/', '$CAP', '--until-fifo'],
  print:    (t) => { window.__lines.push(t); tail(); },
  printErr: (t) => { window.__lines.push('! ' + t); },
  onExit:   (c) => { window.__lines.push('EXIT ' + c); window.__done = true; tail(); },
};
</script>
<script src="bootgame.js"></script>
HTML
node harness/wasm-core/run-boot-browser.mjs > /tmp/fifo-wasm.txt 2>&1 || true
grep -E "fifo (cfg|wr)|executed|PASSED|FAILED" /tmp/fifo-wasm.txt || true

echo
echo "══ FIFO MILESTONE DIFFERENTIAL ══"
# Compare only the milestone lines. Everything else (timings, request counts)
# is allowed to differ between a local file and an HTTP range source.
n=$(grep -E "fifo (cfg|wr)" /tmp/fifo-native.txt || true)
w=$(grep -E "fifo (cfg|wr)" /tmp/fifo-wasm.txt || true)
if [ -z "$n" ] || [ -z "$w" ]; then
  echo "ERROR: one side produced no milestone -- did it reach the FIFO?"
  echo "native: ${n:-<none>}"
  echo "wasm32: ${w:-<none>}"
  exit 1
fi
if [ "$n" = "$w" ]; then
  echo "MATCH -- both targets reach the first FIFO write identically"
  echo "$n" | sed 's/^/  /'
else
  echo "DIVERGED"
  diff <(echo "$n") <(echo "$w") | sed 's/^/  /' || true
  exit 1
fi
