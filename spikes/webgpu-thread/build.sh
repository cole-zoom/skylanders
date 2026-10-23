#!/usr/bin/env bash
# Spike S1: a WebGPU device on an Emscripten pthread, through JSPI.
set -euo pipefail
cd "$(dirname "$0")"
[ -n "${EMSDK:-}" ] || source "$HOME/emsdk/emsdk_env.sh" >/dev/null 2>&1
em++ -std=c++20 -O2 -pthread --use-port=emdawnwebgpu -sJSPI -sOFFSCREENCANVAS_SUPPORT \
  -sPTHREAD_POOL_SIZE=2 -sALLOW_MEMORY_GROWTH -sEXIT_RUNTIME=0 \
  s1.cpp -o s1.js
