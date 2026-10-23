#!/usr/bin/env bash
# Spike S2: glsl2spv, natively, against Dolphin's own glslang (the native
# oracle build, scripts/build-oracle.sh).
set -euo pipefail
cd "$(dirname "$0")"
G=../../dolphin/Externals/glslang
B=../../dolphin-web/build-native/Externals/glslang
mkdir -p out
clang++ -std=c++20 -O2 -I$G -I$G/glslang -I$B -I$B/glslang glsl2spv.cpp \
  $G/glslang/glslang/ResourceLimits/ResourceLimits.cpp \
  $B/glslang/glslang/libglslang.a $B/glslang/SPIRV/libSPIRV.a -o out/glsl2spv
