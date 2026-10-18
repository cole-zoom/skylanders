#!/usr/bin/env bash
# Build the smoke test for both targets and diff their output.
#
# The figure hashes must be IDENTICAL. The MemArena lines differ by design:
# fastmem is expected to succeed natively and be refused on wasm.
set -euo pipefail
cd "$(dirname "$0")/../.."

[ -n "${EMSDK:-}" ] || source "$HOME/emsdk/emsdk_env.sh" >/dev/null 2>&1

D=dolphin
BW=dolphin-web/build-wasm
BN=dolphin-web/build-native
SKYW="$BW/Source/Core/Core/CMakeFiles/core.dir/IOS/USB/Emulated/Skylanders"
SKYN="$BN/Source/Core/Core/CMakeFiles/core.dir/IOS/USB/Emulated/Skylanders"

for p in "$SKYW/SkylanderFigure.cpp.o" "$SKYN/SkylanderFigure.cpp.o"; do
  [ -f "$p" ] || { echo "missing $p -- run scripts/build-wasm.sh core and scripts/build-oracle.sh" >&2; exit 1; }
done

echo "==> wasm32"
# PROXY_TO_PTHREAD moves main off the browser/Node main thread, matching the
# topology Spike C validated. Without EXIT_RUNTIME the process would not flush
# and exit cleanly.
em++ -std=c++23 -O2 -pthread -sPROXY_TO_PTHREAD -sEXIT_RUNTIME=1 -sINITIAL_MEMORY=268435456 \
  -I$D/Source/Core -I$BW/Source/Core -I$D/Externals/fmt/fmt/include -I$D/Externals/picojson \
  harness/wasm-smoke/smoke.cpp "$SKYW/SkylanderFigure.cpp.o" "$SKYW/SkylanderCrypto.cpp.o" \
  $BW/Source/Core/Common/libcommon.a $BW/Externals/fmt/fmt/libfmt.a \
  $BW/Externals/mbedtls/library/libmbedtls.a \
  $BW/Externals/mbedtls/library/libmbedx509.a \
  $BW/Externals/mbedtls/library/libmbedcrypto.a \
  -o /tmp/smoke.js

echo "==> native"
# native_stubs.cpp stands in for Skylander.cpp, which would drag the whole
# emulated USB device stack in. ld64 keeps the unreached reference that
# wasm-ld eliminates, hence the asymmetry.
clang++ -std=c++23 -O2 \
  -I$D/Source/Core -I$BN/Source/Core -I/opt/homebrew/include -I$D/Externals/picojson \
  harness/wasm-smoke/smoke.cpp harness/wasm-smoke/native_stubs.cpp \
  "$SKYN/SkylanderFigure.cpp.o" "$SKYN/SkylanderCrypto.cpp.o" \
  $BN/Source/Core/Common/libcommon.a \
  $BN/Externals/mbedtls/library/libmbedtls.a \
  $BN/Externals/mbedtls/library/libmbedx509.a \
  $BN/Externals/mbedtls/library/libmbedcrypto.a \
  -L/opt/homebrew/lib -lfmt -liconv \
  -framework Foundation -framework IOKit -framework CoreFoundation \
  -o /tmp/smoke-native

/tmp/smoke-native > /tmp/smoke-native.txt 2>&1 || true
node /tmp/smoke.js > /tmp/smoke-wasm.txt 2>&1 || true

echo; echo "═══ NATIVE ═══"; cat /tmp/smoke-native.txt
echo; echo "═══ WASM32 ═══"; cat /tmp/smoke-wasm.txt

echo; echo "══ differential check: figure hashes must match ══"
grep 'fnv1a=' /tmp/smoke-native.txt | sed 's/.*fnv1a=//' | sort > /tmp/h-native
grep 'fnv1a=' /tmp/smoke-wasm.txt   | sed 's/.*fnv1a=//' | sort > /tmp/h-wasm
if [ ! -s /tmp/h-native ]; then echo "no hashes produced"; exit 1; fi
if diff -q /tmp/h-native /tmp/h-wasm >/dev/null; then
  echo "MATCH -- $(wc -l < /tmp/h-native | tr -d ' ') figures byte-identical across targets"
else
  echo "MISMATCH -- the MIFARE crypto differs between native and wasm32:"; diff /tmp/h-native /tmp/h-wasm; exit 1
fi
grep -q "^PASSED" /tmp/smoke-native.txt && grep -q "^PASSED" /tmp/smoke-wasm.txt \
  && echo "both targets PASSED" || { echo "a target FAILED"; exit 1; }
