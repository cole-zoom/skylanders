#!/usr/bin/env bash
# Link a program against Dolphin's full emulator core for wasm32.
#
# Usage: harness/wasm-core/link.sh <main.cpp> <out.js>
#
# Order matters: wasm-ld resolves left to right, so Core must precede the
# subsystems it calls into, and those must precede the third-party libraries.
set -euo pipefail
cd "$(dirname "$0")/../.."
[ -n "${EMSDK:-}" ] || source "$HOME/emsdk/emsdk_env.sh" >/dev/null 2>&1

SRC="${1:?usage: link.sh <main.cpp> <out.js>}"
OUT="${2:?usage: link.sh <main.cpp> <out.js>}"
D=dolphin
B=dolphin-web/build-wasm

# INITIAL_MEMORY must cover guest RAM before anything else: MEM1 (24 MB) plus
# MEM2 (64 MB) plus the emulator's own allocations. 512 MB leaves headroom.
# PROXY_TO_PTHREAD keeps main off the main thread, matching Spike C's topology.
# These must match what CMake compiled the libraries with. Without
# -D_M_GENERIC the headers present a different view to this translation unit
# than they did to libcore -- MachineContext.h in particular falls through to
# "#error No context definition for OS" instead of taking its generic path,
# and anything that compiled would be an ODR hazard rather than a build error.
em++ -std=c++23 -O2 -pthread -sPROXY_TO_PTHREAD -sEXIT_RUNTIME=1 \
  -D_M_GENERIC=1 -D_ARCH_32=1 \
  -sASSERTIONS=1 -sINITIAL_MEMORY=536870912 -sALLOW_MEMORY_GROWTH=0 \
  -sFETCH=1 -sUSE_BZIP2=1 \
  -sEXPORTED_RUNTIME_METHODS=HEAPU8 \
  -sALLOW_TABLE_GROWTH=1 \
  -sPTHREAD_POOL_SIZE=8 \
  -I$D/Source/Core -I$B/Source/Core \
  -I$D/Externals/fmt/fmt/include -I$D/Externals/picojson \
  "$SRC" harness/wasm-core/host_stubs.cpp \
  $B/Source/Core/Core/libcore.a \
  $B/Source/Core/VideoCommon/libvideocommon.a \
  $B/Source/Core/VideoBackends/Null/libvideonull.a \
  $B/Source/Core/VideoBackends/Software/libvideosoftware.a \
  $B/Source/Core/VideoBackends/OGL/libvideoogl.a \
  $B/Source/Core/AudioCommon/libaudiocommon.a \
  $B/Source/Core/InputCommon/libinputcommon.a \
  $B/Source/Core/DiscIO/libdiscio.a \
  $B/Source/Core/UICommon/libuicommon.a \
  $B/Source/Core/Common/libcommon.a \
  $B/Externals/cubeb/libcubeb.a \
  $B/Externals/FatFs/libFatFs.a \
  $B/Externals/FreeSurround/libFreeSurround.a \
  $B/Externals/minizip-ng/minizip-ng/libminizip-ng.a \
  $B/Externals/pugixml/pugixml/libpugixml.a \
  $B/Externals/miniupnpc/miniupnp/miniupnpc/libminiupnpc.a \
  $B/Externals/implot/libimplot.a $B/Externals/imgui/libimgui.a \
  $B/Externals/SFML/libsfml-network.a $B/Externals/SFML/libsfml-system.a \
  $B/Externals/curl/curl/lib/libcurl.a $B/Externals/enet/enet/libenet.a \
  $B/Externals/libspng/libspng/libspng_static.a \
  $B/Externals/xxhash/libxxhash.a $B/Externals/zlib-ng/zlib-ng/libz.a \
  $B/Externals/zstd/zstd/build/cmake/lib/libzstd.a \
  $B/Externals/fmt/fmt/libfmt.a \
  $B/Externals/mbedtls/library/libmbedtls.a \
  $B/Externals/mbedtls/library/libmbedx509.a \
  $B/Externals/mbedtls/library/libmbedcrypto.a \
  $B/Externals/lz4/lz4/build/cmake/liblz4.a \
  $B/Externals/liblzma/liblzma.a \
  $B/Externals/zlib-ng/zlib-ng/libz.a \
  $B/Externals/zstd/zstd/build/cmake/lib/libzstd.a \
  -o "$OUT"
