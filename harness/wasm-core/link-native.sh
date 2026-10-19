#!/usr/bin/env bash
shopt -s globstar nullglob 2>/dev/null || true
# Link the same program against Dolphin's core for the NATIVE host.
#
# Mirrors link.sh so the only difference between the two binaries is the
# target. Compile flags must match what CMake used for the libraries, or the
# headers present a different view to this translation unit than they did to
# libcore -- see link.sh for how that failed loudly once already.
set -euo pipefail
cd "$(dirname "$0")/../.."

SRC="${1:?usage: link-native.sh <main.cpp> <out>}"
OUT="${2:?usage: link-native.sh <main.cpp> <out>}"
D=dolphin
B=dolphin-web/build-native

# The native oracle is built with the AArch64 recompiler, not ENABLE_GENERIC,
# so these differ from link.sh's -D_M_GENERIC. They must match what CMake used
# for THIS build directory; getting it wrong fails in MachineContext.h with an
# error naming neither the flag nor the platform.
clang++ -std=c++23 -O2 -D_M_ARM_64=1 -D_ARCH_64=1 \
  -I$D/Source/Core -I$B/Source/Core -I/opt/homebrew/include -I$D/Externals/picojson \
  "$SRC" harness/wasm-core/host_stubs.cpp \
  $B/Source/Core/Core/libcore.a \
  $B/Source/Core/VideoCommon/libvideocommon.a \
  $B/Source/Core/VideoBackends/Null/libvideonull.a \
  $B/Source/Core/VideoBackends/Software/libvideosoftware.a \
  $B/Source/Core/AudioCommon/libaudiocommon.a \
  $B/Source/Core/InputCommon/libinputcommon.a \
  $B/Source/Core/DiscIO/libdiscio.a \
  $B/Source/Core/UICommon/libuicommon.a \
  $B/Source/Core/Common/libcommon.a \
  # Every external the oracle build produced. Hand-maintaining this list meant
  # discovering each missing one at RUNTIME via dyld, one per rebuild; globbing
  # is both shorter and fails at link time instead. Repeated twice because the
  # set has circular references between libraries.
  $(ls $B/Externals/**/*.a $B/Externals/*/*.a $B/Externals/*.a 2>/dev/null | sort -u) \
  $(ls $B/Externals/**/*.a $B/Externals/*/*.a $B/Externals/*.a 2>/dev/null | sort -u) \
  -L/opt/homebrew/lib -lfmt -liconv -lz -lbz2 -llzma -lcurl \
  -lavcodec -lavformat -lavutil -lswscale \
  -framework Foundation -framework IOKit -framework CoreFoundation \
  -framework AudioUnit -framework AudioToolbox -framework CoreAudio \
  -framework Metal -framework QuartzCore -framework Cocoa \
  -framework ForceFeedback -framework OpenGL -framework IOSurface \

  -o "$OUT"
