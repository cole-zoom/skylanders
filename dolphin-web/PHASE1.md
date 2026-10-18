# Phase 1 — Dolphin core under Emscripten

**Status: `libcommon.a` builds for wasm32.** First milestone of five.

## Where this is

```
✓ Common builds for wasm32          1.96 MB, 68 objects
· Core / PowerPC                    not attempted yet
· Memory arena replaced             compiles, will fail at runtime (see below)
· Host USB stubbed                  not started
· HttpBlobReader                    not started
· Differential harness in CI        not started
```

Build it with `scripts/build-wasm.sh`.

## The two things that unblocked configuring

**`ENABLE_GENERIC=ON` is mandatory.** Dolphin detects wasm32 as 32-bit `x86`
and refuses outright:

```
You're building on an unsupported platform: 'x86' with 4-byte pointers.
Enable generic build if you really want a JIT-less binary.
```

`ENABLE_GENERIC` is a first-class upstream option — "a generic build that
should run on any little-endian host" — and it sets `_M_GENERIC`, selecting the
interpreter and excluding both recompilers. That is exactly the Phase 1 target:
correctness first, speed later via `ppc2wasm`.

**`WITH_OPTIM=OFF`** disables zlib-ng's architecture-specific paths, which
otherwise pull in `cpuid.h` and inline x86 asm that cannot exist on wasm.

## Patches so far

Only two, both small, and only one touches Dolphin's own code:

- `0002-common-emscripten-clock.patch` — `Timer.cpp` selects `CLOCK_UPTIME` on
  any non-Linux, non-Apple platform. Emscripten has no such constant. Mapped to
  `CLOCK_MONOTONIC`, whose Linux semantics (not counting suspended time) are
  actually what we want: wall time spent with the tab backgrounded should not be
  charged to the emulated machine.
- `0003-sfml-emscripten-platform.patch` — SFML's `Config.hpp` tests `__unix__`
  then recognises only Linux/Android/BSD, so it `#error`s on Emscripten. Mapped
  to `SFML_SYSTEM_LINUX`; Emscripten provides the Linux-like POSIX layer and BSD
  sockets that `sfml-network` uses.

Everything else that failed was third-party build configuration, not emulator
source. That is a better ratio than expected.

## The real problem: the memory arena

`MemArenaUnix.cpp` was selected by CMake's generic-Unix branch and **compiled
cleanly**. It will not work.

It calls `shm_open`, then `mmap` with `MAP_SHARED` and `MAP_FIXED` to alias the
same physical pages at several virtual addresses — which is how Dolphin's fast
memory path makes guest addresses directly dereferenceable. Emscripten has no
meaningful `mmap`: no shared mappings, no fixed mappings, no aliasing. Linear
memory is a single flat buffer.

**Compiling is not working, and this is the clearest example of it in the
project so far.** Nothing in the build output hints at a problem.

The plan's answer is `MAIN_FASTMEM_ARENA=false`, which `docs/PLAN.md` §1 records
as an independent upstream toggle. Next step is to confirm the software MMU path
actually runs without the arena, and to add a `MemArenaEmscripten.cpp` that
fails loudly on construction rather than silently returning broken mappings —
so that if the arena is ever reached, it says so instead of corrupting guest
memory.

## Next

1. **`MemArenaEmscripten.cpp`** that refuses to pretend, plus confirming
   `MAIN_FASTMEM_ARENA=false` is viable end to end.
2. **Build `core`**, which is where the interesting failures will be: threading,
   filesystem, the `IOS::HLE::USB` stack, and the emulated Skylander portal that
   has to survive.
3. **Stub host USB** so libusb is never reached while the emulated portal is.
4. **`HttpBlobReader`** — a `DiscIO::BlobReader` subclass issuing range requests
   against the RVZ chunk index.
5. **Differential harness in CI**, comparing floats bit-exactly (see
   `docs/PLAN.md` §2 on contraction).

## Toolchain note

CMake 4.2.3 warns that it "does not support emscripten shared libraries — use
cmake < 4.2.0 or cmake > 4.3.3". Harmless for a static build, but it will matter
if dynamic linking is ever wanted (for example `MAIN_DYNAMIC_LIBRARY` style
plugin loading, or `dlopen`-based tiering). Worth knowing before that decision.
