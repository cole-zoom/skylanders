#!/usr/bin/env bash
# Link the same program against Dolphin's core for the NATIVE host, built with
# the SAME configuration as the wasm build.
#
# Usage: harness/wasm-core/link-native.sh <main.cpp> <out>
#
# This uses build-native-generic, not the oracle in build-native. The oracle is
# a full JIT build: linking it drags in FFmpeg, Discord, libusb, cubeb and a
# long circular external set, and the failures surface one at a time in dyld at
# RUNTIME rather than at link time. A generic build mirrors the wasm one, so
# the only difference between the two binaries is the target -- which is the
# entire point of a differential.
set -euo pipefail
cd "$(dirname "$0")/../.."

SRC="${1:?usage: link-native.sh <main.cpp> <out>}"
OUT="${2:?usage: link-native.sh <main.cpp> <out>}"
D=dolphin
B=dolphin-web/build-native-generic

[ -f "$B/Source/Core/Core/libcore.a" ] || {
  echo "native generic build missing; see harness/wasm-core/README or re-run cmake" >&2
  exit 1; }

# -D_M_GENERIC must match what CMake used, exactly as in link.sh. Mismatched
# defines make the headers present a different view to this translation unit
# than they did to libcore.
clang++ -std=c++23 -O2 -D_M_GENERIC=1 -D_ARCH_64=1 \
  -I$D/Source/Core -I$B/Source/Core -I/opt/homebrew/include -I/usr/local/include \
  -I$D/Externals/fmt/fmt/include -I$D/Externals/picojson \
  "$SRC" harness/wasm-core/host_stubs.cpp \
  $B/Source/Core/Core/libcore.a \
  $B/Source/Core/VideoCommon/libvideocommon.a \
  $B/Source/Core/VideoBackends/Null/libvideonull.a \
  $B/Source/Core/VideoBackends/Software/libvideosoftware.a \
  $B/Source/Core/VideoBackends/OGL/libvideoogl.a \
  $B/Source/Core/VideoBackends/Metal/libvideometal.a \
  $B/Source/Core/AudioCommon/libaudiocommon.a \
  $B/Source/Core/InputCommon/libinputcommon.a \
  $B/Source/Core/DiscIO/libdiscio.a \
  $B/Source/Core/UICommon/libuicommon.a \
  $B/Source/Core/Common/libcommon.a \
  $B/Externals/implot/libimplot.a $B/Externals/imgui/libimgui.a \
  $B/Externals/SFML/libsfml-network.a $B/Externals/SFML/libsfml-system.a \
  $B/Externals/enet/enet/libenet.a \
  $B/Externals/libspng/libspng/libspng_static.a \
  $B/Externals/zlib-ng/zlib-ng/libz.a \
  $B/Externals/tinygltf/libtinygltf.a \
  $B/Externals/spirv_cross/libspirv_cross.a \
  $B/Externals/glslang/glslang/glslang/libglslang.a \
  $B/Externals/glslang/glslang/SPIRV/libSPIRV.a \
  $B/Externals/cpp-optparse/libcpp-optparse.a \
  $B/Externals/mbedtls/library/libmbedtls.a \
  $B/Externals/mbedtls/library/libmbedx509.a \
  $B/Externals/mbedtls/library/libmbedcrypto.a \
  $B/Externals/minizip-ng/minizip-ng/libminizip-ng.a \
  $B/Externals/pugixml/pugixml/libpugixml.a \
  $B/Externals/FatFs/libFatFs.a \
  $B/Externals/FreeSurround/libFreeSurround.a \
  $B/Externals/cubeb/libcubeb.a \
  $B/Externals/miniupnpc/miniupnp/miniupnpc/libminiupnpc.a \
  $B/Externals/hidapi/libhidapi.a \
  -L/opt/homebrew/lib -lfmt -liconv -lbz2 -llzma -lcurl -lzstd -lavcodec -lavformat -lavutil -lswscale -lusb-1.0 -llz4 -llzo2 -lxxhash -lresolv \
  -framework Foundation -framework IOKit -framework CoreFoundation \
  -framework AudioUnit -framework AudioToolbox -framework CoreAudio \
  -framework Cocoa -framework ForceFeedback -framework OpenGL -framework Metal -framework QuartzCore -framework IOSurface \
  -o "$OUT"
