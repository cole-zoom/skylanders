#!/usr/bin/env bash
# Link a standalone wasm test with runtime code generation enabled.
#
# Separate from link.sh because this links none of Dolphin -- the question it
# answers is about WebAssembly itself, and dragging in a hundred megabytes of
# emulator would make a two-second build a seven-minute one.
#
# Two flags here are load-bearing:
#
#   ALLOW_TABLE_GROWTH  the indirect function table is fixed-size by default.
#                       A recompiler installs a new entry per compiled block,
#                       so without this the table cannot accept them.
#
#   PROXY_TO_PTHREAD    puts main() on a worker. Chrome refuses SYNCHRONOUS
#                       WebAssembly compilation of modules over 4 KB on the
#                       main thread, and a compiled block will exceed that.
set -euo pipefail
cd "$(dirname "$0")/../.."
[ -n "${EMSDK:-}" ] || source "$HOME/emsdk/emsdk_env.sh" >/dev/null 2>&1
SRC="${1:?usage: link-jit.sh <source.cpp> <out.js>}"
OUT="${2:?}"
em++ -std=c++23 -O2 -pthread -sPROXY_TO_PTHREAD -sEXIT_RUNTIME=1 \
  -sALLOW_TABLE_GROWTH=1 \
  -sINITIAL_MEMORY=268435456 -sALLOW_MEMORY_GROWTH=0 \
  -sEXPORTED_RUNTIME_METHODS=HEAPU8 \
  -sASSERTIONS=1 \
  "$SRC" -o "$OUT"
