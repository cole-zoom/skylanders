#!/usr/bin/env bash
# Build the native Dolphin binaries used as the differential test oracle.
#
# This is NOT the browser build. It is a stock, unmodified Dolphin whose only
# job is to be correct, so we can diff the WebAssembly build against it.
#
# Produces:
#   dolphin-web/build-native/Binaries/dolphin-emu-nogui   trace capture
#   dolphin-web/build-native/Binaries/dolphin-tool        disc inspection, RVZ recompression
set -euo pipefail
cd "$(dirname "$0")/.."

BUILD_DIR="dolphin-web/build-native"

if [ ! -d dolphin/.git ]; then
  echo "dolphin/ not present. Run scripts/bootstrap.sh first." >&2
  exit 1
fi

# ── Submodules ───────────────────────────────────────────────────────────
#
# Four are deliberately skipped:
#   Qt              we build NoGUI; Qt is the single largest dependency
#   FFmpeg-bin      prebuilt binaries, only needed for frame dumping
#   libadrenotools  Android-only
#   mGBA/mgba       GBA link-cable emulation, irrelevant to Skylanders
#
# Run SERIALLY. Concurrent `git submodule update` processes against one
# worktree collide on .git/modules/**/shallow.lock, and the failure is
# silent-ish: the recursive walk aborts partway and later submodules are
# simply never fetched, surfacing much later as confusing CMake errors
# about missing targets.
SKIP='^Externals/(Qt|FFmpeg-bin|libadrenotools|mGBA/mgba)$'

echo "==> submodules"
find dolphin/.git/modules -name '*.lock' -delete 2>/dev/null || true
paths=$(grep 'path =' dolphin/.gitmodules | sed 's/.*= //' | grep -vE "$SKIP")
git -C dolphin submodule update --init --recursive --depth 1 $(echo $paths) >/dev/null 2>&1 || true

missing=""
while read -r p; do
  [ -z "$p" ] && continue
  n=$(ls -A "dolphin/$p" 2>/dev/null | grep -v '^\.git$' | wc -l | tr -d ' ')
  [ "$n" = "0" ] && missing="$missing $p"
done <<EOF
$(echo "$paths")
EOF

if [ -n "$missing" ]; then
  echo "ERROR: submodules still empty after fetch:" >&2
  for m in $missing; do echo "    $m" >&2; done
  echo "    Re-run this script; if it persists, fetch them individually." >&2
  exit 1
fi
echo "    all required submodules populated"

# ── Configure ────────────────────────────────────────────────────────────
#
# USE_MGBA=OFF and USE_RETRO_ACHIEVEMENTS=OFF match the skipped submodules.
# Leaving them ON with the submodule absent produces "target not built by
# this project" errors rather than anything that names the real cause.
echo "==> configure"
CCACHE_ARGS=""
if command -v ccache >/dev/null 2>&1; then
  CCACHE_ARGS="-DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache"
fi

cmake -S dolphin -B "$BUILD_DIR" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DENABLE_QT=OFF \
  -DENABLE_NOGUI=ON \
  -DENABLE_TESTS=OFF \
  -DENABLE_AUTOUPDATE=OFF \
  -DENABLE_ANALYTICS=OFF \
  -DENABLE_LTO=OFF \
  -DUSE_MGBA=OFF \
  -DUSE_RETRO_ACHIEVEMENTS=OFF \
  $CCACHE_ARGS

# ── Build ────────────────────────────────────────────────────────────────
echo "==> build"
ninja -C "$BUILD_DIR" dolphin-emu-nogui dolphin-tool

echo
echo "Oracle built:"
ls -la "$BUILD_DIR/Binaries/" 2>/dev/null | grep -E 'dolphin' || true
