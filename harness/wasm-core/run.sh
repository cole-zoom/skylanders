#!/usr/bin/env bash
# Build the core test for both targets and diff their output.
#
# Both sides are ENABLE_GENERIC builds, so the ONLY difference between the two
# binaries is the target. Everything except pointer width and the two target
# banners must match byte for byte.
set -euo pipefail
cd "$(dirname "$0")/../.."

echo "==> wasm32";  harness/wasm-core/link.sh        harness/wasm-core/coreboot.cpp /tmp/coreboot.js
echo "==> native";  harness/wasm-core/link-native.sh harness/wasm-core/coreboot.cpp /tmp/coreboot-native

node /tmp/coreboot.js  > /tmp/core-wasm.txt   2>&1 || true
/tmp/coreboot-native   > /tmp/core-native.txt 2>&1 || true

echo; echo "═══ NATIVE ═══"; cat /tmp/core-native.txt
echo; echo "═══ WASM32 ═══"; cat /tmp/core-wasm.txt

echo; echo "══ differential ══"
# Drop the two lines that are expected to differ: the target banner and the
# host pointers inside MEM1/MEM2 (allocation addresses, not emulated state).
norm() { grep -vE '^target:' "$1" | sed -E 's/(MEM[12]) 0x[0-9a-f]+/\1 <addr>/'; }
if diff -q <(norm /tmp/core-native.txt) <(norm /tmp/core-wasm.txt) >/dev/null; then
  echo "MATCH -- native and wasm32 agree on every value"
else
  echo "MISMATCH:"; diff <(norm /tmp/core-native.txt) <(norm /tmp/core-wasm.txt); exit 1
fi
grep -q "^PASSED" /tmp/core-native.txt && grep -q "^PASSED" /tmp/core-wasm.txt \
  && echo "both targets PASSED" || { echo "a target FAILED"; exit 1; }
