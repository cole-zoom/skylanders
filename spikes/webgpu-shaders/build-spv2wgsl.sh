#!/usr/bin/env bash
# Spike S2: Tint (SPIR-V reader, WGSL writer) linked into a wasm module, to
# measure what it adds and how fast it translates. Needs dawn/ (UPSTREAM.lock)
# with its Tint built for wasm into dawn/out/wasm -- see FINDINGS.md.
set -euo pipefail
cd "$(dirname "$0")"
[ -n "${EMSDK:-}" ] || source "$HOME/emsdk/emsdk_env.sh" >/dev/null 2>&1
D=../../dawn
LIBS=$(find $D/out/wasm/src/tint $D/out/wasm/third_party -name "*.a" | sort)
FLAGS="-DTINT_BUILD_SPV_READER=1 -DTINT_BUILD_WGSL_WRITER=1 -DTINT_BUILD_WGSL_READER=0
  -DTINT_BUILD_GLSL_WRITER=0 -DTINT_BUILD_GLSL_VALIDATOR=0 -DTINT_BUILD_HLSL_WRITER=0
  -DTINT_BUILD_MSL_WRITER=0 -DTINT_BUILD_SPV_WRITER=0 -DTINT_BUILD_NULL_WRITER=0
  -DTINT_BUILD_IR_BINARY=0 -DTINT_BUILD_TINTD=0 -DTINT_BUILD_IS_LINUX=1 -DTINT_BUILD_IS_MAC=0
  -DTINT_BUILD_IS_WIN=0 -DTINT_ENABLE_IR_DUMPING=0 -DTINT_ENABLE_IR_VALIDATION_ASSERTS=0"
mkdir -p out
em++ -std=c++20 -O2 -pthread -Wno-deprecated-pragma $FLAGS -I$D -I$D/include \
  spv2wgsl.cpp -Wl,--start-group $LIBS -Wl,--end-group \
  -sNODERAWFS -sALLOW_MEMORY_GROWTH -sSTACK_SIZE=4MB -o out/spv2wgsl.js
# The same program with the translation compiled out, so the difference
# in .wasm size is Tint's.
em++ -std=c++20 -O2 -pthread -x c++ - -o out/empty.js <<'CPP'
#include <cstdio>
int main() { std::puts("empty"); }
CPP
ls -l out/spv2wgsl.wasm out/empty.wasm
