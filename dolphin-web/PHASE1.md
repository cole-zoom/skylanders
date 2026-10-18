# Phase 1 — Dolphin core under Emscripten

**Status: `libcommon.a` builds for wasm32.** First milestone of five.

## Where this is

```
✓ Common builds for wasm32          1.96 MB, 68 objects
✓ Core builds for wasm32           18.75 MB, 307 objects
✓ Memory arena replaced             MemArenaEmscripten.cpp, refuses what it cannot do
✓ Skylander portal survives         Skylander/SkylanderCrypto/SkylanderFigure all present
✓ Recompilers excluded              9 interpreter objects, 0 JIT objects
· Host USB stubbed                  not started
· HttpBlobReader                    not started
· Differential harness in CI        not started
· Anything actually RUN             not started
```

Verified in the archive rather than assumed:

```
Skylander.cpp.o  SkylanderCrypto.cpp.o  SkylanderFigure.cpp.o   <- portal intact
Interpreter objects: 9      JIT objects: 0                      <- _M_GENERIC working
MemArenaEmscripten.cpp.o                                        <- not the Unix one
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

## Dolphin's platform detection is allowlist-shaped

Five separate build failures had one cause: `#if` chains that enumerate known
platforms with no `#else`. A new platform does not get a clear "unsupported"
error — it gets a confusing one, far from the actual cause:

| File | Symptom | Real cause |
|---|---|---|
| SFML `Config.hpp` | `#error This UNIX operating system is not supported` | the only honest one |
| `Timer.cpp` | `use of undeclared identifier 'CLOCK_UPTIME'` | no `#else` branch |
| `DriverDetails.cpp` | `invalid use of non-static data member 'm_os'` | `m_os` never defined, so the name resolved to a struct member |
| `Socket.h` | `use of undeclared identifier 'pollfd_t'` | fallback `#else` omits `<poll.h>` |
| `EXI_DeviceEthernet.h` | `use of undeclared identifier 'm_network_ref'` | members declared only inside the allowlist |

The `DriverDetails` one is the worst of them: the error names a struct member
that has nothing to do with the problem.

Emscripten was added to each allowlist rather than given a fallback branch,
because the thing these guards actually gate on — BSD sockets, `<poll.h>`,
threads — Emscripten genuinely provides.

**One self-inflicted bug worth recording.** The first attempt at the
`EXI_DeviceEthernet.h` allowlists used a regex that matched only the first line
of a multi-line `#if`, splicing text into the middle and orphaning the rest of
the condition. It produced a file that looked patched and was syntactically
broken. Reverted and redone by appending to the end of the full condition.

## The memory arena

`MemArenaUnix.cpp` was selected by CMake's generic-Unix branch and **compiled
cleanly**. It will not work.

It calls `shm_open`, then `mmap` with `MAP_SHARED` and `MAP_FIXED` to alias the
same physical pages at several virtual addresses — which is how Dolphin's fast
memory path makes guest addresses directly dereferenceable. Emscripten has no
meaningful `mmap`: no shared mappings, no fixed mappings, no aliasing. Linear
memory is a single flat buffer.

**Compiling is not working, and this is the clearest example of it in the
project so far.** Nothing in the build output hints at a problem.

**Fixed.** `MemArenaEmscripten.cpp` now implements only what can work and
refuses the rest:

- `GrabSHMSegment` / `CreateView` / `GetPageSize` — a flat allocation with views
  as offsets into it. This is all `Memory::Init` needs.
- `ReserveMemoryRegion` — returns `nullptr`. `Memmap.cpp` already treats that as
  "fastmem unavailable" and falls back to the software MMU, which is exactly
  right.
- `MapInMemoryRegion` / `ChangeMappingProtection` / `UnmapFromMemoryRegion` —
  **panic**. They cannot be implemented and must never be reached; returning a
  plausible pointer would silently corrupt guest memory.

The CMake branch is placed *before* the generic-Unix one, since Emscripten
defines `__unix__` and would otherwise keep selecting `MemArenaUnix.cpp`.

Note that `CreateView` returning offsets means two views of the same offset get
the *same* address rather than two addresses aliasing one page. Memory mirrors
therefore cannot work on this backend. Nothing needs them with fastmem off, but
it is a real limitation, not an implementation detail.

`MemoryUtil.cpp` still references `mmap`/`mprotect`. Its anonymous-private
mappings work under Emscripten; the `PROT_EXEC` and `mprotect` paths are
JIT-only and `_M_GENERIC` excludes them. Unverified at runtime.

## Next

1. **Run something.** Nothing in either library has been executed. A linking
   archive says nothing about whether it works, and this project has repeatedly
   found that gap the expensive way.
2. **Stub host USB** so libusb is never reached while the emulated portal is.
4. **`HttpBlobReader`** — a `DiscIO::BlobReader` subclass issuing range requests
   against the RVZ chunk index.
5. **Differential harness in CI**, comparing floats bit-exactly (see
   `docs/PLAN.md` §2 on contraction).

## Toolchain note

CMake 4.2.3 warns that it "does not support emscripten shared libraries — use
cmake < 4.2.0 or cmake > 4.3.3". Harmless for a static build, but it will matter
if dynamic linking is ever wanted (for example `MAIN_DYNAMIC_LIBRARY` style
plugin loading, or `dlopen`-based tiering). Worth knowing before that decision.
