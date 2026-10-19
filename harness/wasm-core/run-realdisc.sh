#!/usr/bin/env bash
# Read the real Skylanders disc, natively and in a browser over HTTP ranges.
#
# Needs a raw disc image. The repo ships none -- convert your own:
#   dolphin-web/build-native/Binaries/dolphin-tool convert \
#     -u /tmp/dolphin-convert -i "skylanders-rom/<disc>.rvz" -o /tmp/ssa.iso -f iso
set -euo pipefail
cd "$(dirname "$0")/../.."
ISO="${1:-/tmp/ssa.iso}"

[ -f "$ISO" ] || { echo "no disc image at $ISO -- see the header of this script" >&2; exit 77; }

echo "══ NATIVE (direct file reads) ══"
harness/wasm-core/link-native.sh harness/wasm-core/realdisc.cpp /tmp/realdisc-native >/dev/null
/tmp/realdisc-native "$ISO"

echo
echo "══ BROWSER (HTTP range requests) ══"
harness/wasm-core/link.sh harness/wasm-core/realdisc.cpp /tmp/realdisc.js >/dev/null
mkdir -p /tmp/discweb
cp /tmp/realdisc.js /tmp/realdisc.wasm /tmp/discweb/
cat > /tmp/discweb/index.html <<'HTML'
<!doctype html><meta charset=utf8><title>real disc over HTTP ranges</title>
<pre id=out>running…</pre>
<script>
const lines = [];
var Module = {
  arguments: ['http://127.0.0.1:8101/'],
  print:    (t) => { lines.push(t);           document.getElementById('out').textContent = lines.join('\n'); },
  printErr: (t) => { lines.push('! ' + t);    document.getElementById('out').textContent = lines.join('\n'); },
  onExit:   (c) => { lines.push('EXIT ' + c); document.getElementById('out').textContent = lines.join('\n'); },
};
</script>
<script src="realdisc.js"></script>
HTML
node harness/wasm-core/run-disc-browser.mjs
