#!/usr/bin/env bash
# Capture an instruction-level CPU trace from both targets and diff them.
#
# This is docs/PLAN.md Bet 04 in miniature: native Dolphin as the oracle, the
# port compared against it automatically, and the FIRST divergence reported
# rather than every consequence of it.
set -euo pipefail
cd "$(dirname "$0")/../.."

echo "==> building"
harness/wasm-core/link.sh        harness/wasm-core/tracetest.cpp /tmp/tracetest.js    >/dev/null
harness/wasm-core/link-native.sh harness/wasm-core/tracetest.cpp /tmp/tracetest-native >/dev/null

echo "==> tracing"
/tmp/tracetest-native > /tmp/trace-native.txt 2>&1 || true
node /tmp/tracetest.js > /tmp/trace-wasm.txt  2>&1 || true

grep '^#' /tmp/trace-native.txt | sed 's/^/  native /' || true
grep '^#' /tmp/trace-wasm.txt   | sed 's/^/  wasm32 /' || true

echo
python3 harness/wasm-core/difftrace.py /tmp/trace-native.txt /tmp/trace-wasm.txt
