#!/usr/bin/env bash
# Build Dolphin's core libraries for wasm32 under Emscripten.
#
# This is the Phase 1 browser build, distinct from the native oracle that
# scripts/build-oracle.sh produces. It is JIT-less by necessity.
set -euo pipefail
cd "$(dirname "$0")/.."

[ -n "${EMSDK:-}" ] || source "$HOME/emsdk/emsdk_env.sh" >/dev/null 2>&1 || {
  echo "Emscripten not found; expected ~/emsdk/emsdk_env.sh" >&2; exit 1; }

BUILD_DIR=dolphin-web/build-wasm
TARGET="${1:-common}"

# Apply the wasm port patches. Reverse-check first so re-runs are idempotent.
for p in dolphin-web/patches/000[234]-*.patch; do
  [ -e "$p" ] || continue
  case "$(basename "$p")" in
    *sfml*)  repo=dolphin/Externals/SFML/SFML; rel="../../../../$p" ;;
    *)       repo=dolphin;                     rel="../$p" ;;
  esac
  if git -C "$repo" apply --reverse --check "$rel" >/dev/null 2>&1; then
    echo "    $(basename "$p") already applied"
  elif git -C "$repo" apply "$rel" >/dev/null 2>&1; then
    echo "    $(basename "$p") applied"
  else
    echo "ERROR: $(basename "$p") does not apply to $repo" >&2; exit 1
  fi
done

# ENABLE_GENERIC is required: Dolphin detects wasm32 as 32-bit "x86" and
# refuses to configure otherwise. Generic is its first-class JIT-less path,
# which is exactly what Phase 1 wants -- interpreter only, correctness first.
#
# WITH_OPTIM=OFF disables zlib-ng's architecture-specific paths, which
# otherwise pull in x86 CPUID intrinsics that do not exist on wasm.
# -pthread enables wasm's atomics and bulk-memory target features. Without it
# anything using atomic builtins fails to compile ("needs target feature
# atomics") -- cubeb hits this first. It also commits the build to the
# threading model Spike C validated: SharedArrayBuffer plus COOP/COEP, which
# spikes/threading/serve.mjs already serves.
emcmake cmake -S dolphin -B "$BUILD_DIR" -G Ninja \
  -DCMAKE_C_FLAGS="-pthread" \
  -DCMAKE_CXX_FLAGS="-pthread" \
  -DCMAKE_BUILD_TYPE=Release \
  -DENABLE_GENERIC=ON \
  -DWITH_OPTIM=OFF \
  -DENABLE_QT=OFF -DENABLE_NOGUI=OFF -DENABLE_TESTS=OFF \
  -DENABLE_AUTOUPDATE=OFF -DENABLE_ANALYTICS=OFF -DENABLE_LTO=OFF \
  -DUSE_MGBA=OFF -DUSE_RETRO_ACHIEVEMENTS=OFF -DUSE_DISCORD_PRESENCE=OFF \
  -DENABLE_VULKAN=OFF -DENABLE_SDL=OFF

ninja -C "$BUILD_DIR" "$TARGET"
echo
ls -la "$BUILD_DIR"/Source/Core/*/lib*.a 2>/dev/null || true
