#!/usr/bin/env bash
# Prove the dispatch variant actually dispatches.
#
# The previous version of this spike produced a plausible-looking MIPS number
# from a loop LLVM had devirtualized and inlined. A number is not evidence;
# the emitted module is. This checks the structure, not the speed.
set -euo pipefail
cd "$(dirname "$0")"

WASM=spike.wasm
[ -f "$WASM" ] || { echo "no $WASM — build first" >&2; exit 1; }
wasm2wat "$WASM" -o spike.wat

fail=0
check() { # name, expected-min, actual
  if [ "$3" -ge "$2" ]; then printf '  PASS  %-44s %s\n' "$1" "$3"
  else printf '  FAIL  %-44s %s (want >= %s)\n' "$1" "$3" "$2"; fail=1; fi
}

echo "Structural checks on $WASM:"
check "call_indirect present in module" 1 "$(grep -c 'call_indirect' spike.wat || true)"
# Extract exactly one function body: start at the target's (func line, stop at
# the next one at the same indent. A range regex does not work here -- it
# matches the opening line and terminates immediately.
body() { awk -v t="\$$1" '
  /^  \(func / { if (f) exit; if ($2==t) f=1 }
  f { print }' spike.wat; }

check "call_indirect inside run_dispatched" 1 \
  "$(body spike_run_dispatched | grep -c 'call_indirect' || true)"

# The eight blocks must survive as distinct functions. If LLVM folded them,
# the funcref table will hold fewer than 8 unique entries.
# Every block must survive as its own indirect target. Fewer than 8 means
# either LLVM folded identical functions or the install permutation is not a
# bijection and some blocks were never address-taken.
elems=$(grep -o 'spike_a_throughput7block_[0-7]' spike.wat | sort -u | wc -l | tr -d ' ')
check "distinct blocks as indirect targets" 8 "$elems"

# A devirtualized build inlines the block body into the dispatch loop, which
# makes that function large. A real indirect call keeps it small.
sz=$(body spike_run_dispatched | wc -l | tr -d ' ')
if [ "$sz" -lt 120 ]; then printf '  PASS  %-44s %s lines\n' "run_dispatched not inlined-fat" "$sz"
else printf '  FAIL  %-44s %s lines (block was inlined)\n' "run_dispatched not inlined-fat" "$sz"; fail=1; fi

echo
[ "$fail" -eq 0 ] && echo "STRUCTURE OK — timings from this module are meaningful." \
                  || { echo "STRUCTURE BAD — do not trust any MIPS number from this module." >&2; exit 1; }
