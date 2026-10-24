#!/usr/bin/env bash
# docs/PHASE3-WEBGPU.md M3: the WebGPU backend's frames against Dolphin's
# Software renderer, from the same FIFO captures.
#
# The captures: the three recorded natively in harness/traces (boot, title,
# the save-slot menu), and the tutorial level at 390 s when it has been
# recorded -- harness/traces is not committed, and that one is made with
#
#   harness/wasm-core/run-jit.sh 300000000000 --jit --dual-core --input=A --skylander \
#     --ticks=291600000000 --record-fifo=284310000000 > rec.log
#   (the .dff is the base64 on the "D " lines; see docs/PHASE3-WEBGPU.md M3)
#
# Each capture in harness/traces is replayed twice in the browser harness --
# once with the Software renderer, Dolphin's reference rasteriser, and once
# with the WebGPU backend writing its XFB to RAM -- and the last frame of each
# is compared (harness/src/compare_frames.py).
#
#   harness/wasm-core/run-m3.sh [outdir] [capture.dff ...]
set -euo pipefail
cd "$(dirname "$0")/../.."
OUT="${1:-/tmp/m3}"
shift || true
CAPTURES=("$@")
if [ ${#CAPTURES[@]} -eq 0 ]; then
  CAPTURES=(ssa-boot.dff ssa-title.dff ssa-gameplay.dff)
  [ -f harness/traces/ssa-level390.dff ] && CAPTURES+=(ssa-level390.dff)
fi
failed=0
mkdir -p "$OUT" /tmp/bootweb

# One build for both, so both renderers run the same emulator.
WEBGPU=1 harness/wasm-core/link.sh harness/wasm-core/bootgame.cpp /tmp/bootgame.js >/dev/null
for dff in "${CAPTURES[@]}"; do
  name="${dff%.dff}"
  cp "harness/traces/$dff" /tmp/bootweb/
  echo "== $name"
  # run-jit.sh relinks; WEBGPU=1 makes that the same build.
  WEBGPU=1 harness/wasm-core/run-jit.sh 1 --jit --fifo="$dff" --render --dump-xfb=x \
    > "$OUT/$name-software.log" 2>&1
  WEBGPU=1 SCREENSHOT="$OUT/$name-canvas.png" harness/wasm-core/run-jit.sh 1 --jit --fifo="$dff" \
    --webgpu --xfb-to-ram --no-osd --dump-xfb=x > "$OUT/$name-webgpu.log" 2>&1
  grep -hE "webgpu .*(rejected|refused|not compiled|WebGPU error|lost)" "$OUT/$name-webgpu.log" \
    | sort | uniq -c | head -5 || true
  python3 -I harness/src/compare_frames.py "$OUT/$name-software.log" "$OUT/$name-webgpu.log" \
    --png "$OUT/$name-compare.png" || failed=$((failed + 1))
done
echo
[ "$failed" -eq 0 ] && echo "M3 PASSED (${#CAPTURES[@]} scenes)" || echo "M3 FAILED ($failed of ${#CAPTURES[@]} scenes)"
exit "$failed"
