# Phase 1 — Dolphin core under Emscripten

**Status: `libcommon.a` builds for wasm32.** First milestone of five.

## Where this is

```
✓ Common builds for wasm32          1.96 MB, 68 objects
✓ Core builds for wasm32           18.75 MB, 307 objects
✓ Memory arena replaced             MemArenaEmscripten.cpp, refuses what it cannot do
✓ Skylander portal survives         Skylander/SkylanderCrypto/SkylanderFigure all present
✓ Recompilers excluded              9 interpreter objects, 0 JIT objects
✓ First execution under wasm32      harness/wasm-smoke, both targets PASSED
✓ MIFARE crypto verified            3 figures byte-identical vs native
✓ Portal runs under wasm32          SkylanderPortal, 64 block replies identical
✓ Emulator core boots under wasm32  Core::System + memory manager, 3.8 MB module
· Host USB stubbed                  not started
· HttpBlobReader                    not started
· Differential harness in CI        not started
```

## First execution — and the first real differential result

`harness/wasm-smoke/run.sh` builds the same source for wasm32 and native and
diffs the output.

```
                                                 native    wasm32
MemArena: views, zeroing, round-trip             ok        ok
MemArena: non-overlapping regions                ok        ok
ReserveMemoryRegion                              succeeds  refuses
Spyro          id=16  var=0x0000   fnv1a=  e32e5ffc0f14cafe = e32e5ffc0f14cafe
Gill Grunt     id=14  var=0x0000   fnv1a=  323814c7d30deee9 = 323814c7d30deee9
Whirlwind (S2) id=0   var=0x1801   fnv1a=  cc48aed2b4be0e86 = cc48aed2b4be0e86
```

**The MIFARE crypto produces byte-identical figure data under wasm32.** That
was the most load-bearing untested claim in the project — an endianness or
integer-width difference would have produced figures the game silently rejects.
Three figures, fixed NUID so `Create()` is deterministic, hashing the decrypted
image that the game actually reads back through the portal.

`MemArenaEmscripten` works at runtime, and refuses the fastmem reservation as
designed while the native backend grants it. That asymmetry is the point, not a
discrepancy.

### The portal itself

Correct figure bytes do not prove the portal answers correctly — the game reads
figures back through `GetStatus()` and `QueryBlock()`, and it is those replies
that have to match hardware. `SkylanderPortal` now runs under wasm32:

```
status reply is framed 'S'                      ok    ok
figure accepted into a portal slot              ok    ok
status changes once a figure is present         ok    ok
all 64 block replies are correctly framed       ok    ok
written block reads back identically            ok    ok
figure removed from the portal                  ok    ok

Spyro on portal, 64 blocks   fnv1a=  3c788e7b70c99db0 = 3c788e7b70c99db0
```

Every block reply — framing byte, status byte, block index and the 16 bytes of
encrypted payload the game decrypts — is byte-identical across targets. The
write/read check matters because that is how the game saves levels and gold
onto a figure.

## The core comes up

`harness/wasm-core/` links a program against the **whole** emulator core for
wasm32 — `libcore` plus VideoCommon, the Null/Software/OGL backends,
AudioCommon, InputCommon, DiscIO, UICommon and Common, 3.8 MB of module — and
runs it.

```
target: wasm32 (emscripten)

Core::System
  singleton constructed                     ok
  memory manager reachable                  ok
  MEM1 0x690000 size 0x02000000
  MEM2 0        size 0x04000000
  MEM1 allocated                            ok
  MEM1 has a nonzero size                   ok
  MEM1 round-trips first and last byte      ok
  memory shut down cleanly                  ok
PASSED
```

Guest RAM is allocated through `MemArenaEmscripten`, is writable through the
pointers the emulator hands out, and shuts down cleanly.

**MEM2 is null** because without a game loaded the core comes up in GameCube
mode, where MEM2 does not exist. The 64 MB figure beside it is the size
constant, not an allocation. Reaching Wii mode needs a boot configuration, not
a port fix — that is the next step, not a defect.

### Host USB is compiled out, the way Android does it

The first run aborted immediately:

```
host_stubs: libusb_init was called.
This build must use the EMULATED Skylander portal, never host USB.
```

That abort did its job. `Core::System` construction initialises the USB
subsystem, which calls `libusb_init()` — so a browser build would have silently
enumerated nothing.

The fix follows an existing upstream pattern: Android already runs without
`__LIBUSB__`, and every host-USB path in `Core` is guarded on that macro.
Emscripten now joins it.

One wrinkle made this more than a one-line change. The `LibUSB::LibUSB` CMake
target must still exist, because `hidapi` and `inputcommon` link it
unconditionally — so the find cannot simply be skipped as it is on Android.
But `Core/CMakeLists.txt` selects the host-USB **sources** on
`TARGET LibUSB::LibUSB` while the code inside them selects on `__LIBUSB__`.
Leaving the target present and the macro undefined compiles `BTReal.cpp`
against the stub header, which fails with errors naming neither libusb nor
Emscripten. A `DOLPHIN_USE_LIBUSB` variable now keeps both gates in agreement.

### A sixth allowlist

`MemoryUtil.cpp`'s `MemPhysical()` falls through to Linux's `sysinfo()` on any
unrecognised platform. Emscripten now returns `emscripten_get_heap_max()`,
which is the meaningful equivalent — callers are asking how much memory is
available, and for wasm that is how far the heap can grow.

### LazyMemoryRegion

Missed when `MemArenaEmscripten.cpp` was first written, and only surfaced at
link time here. Every other backend makes it genuinely lazy — reserve address
space, commit pages on touch. WebAssembly cannot reserve without committing, so
it allocates up front with `calloc`. The semantics callers rely on (zeroed on
`Create`, zeroed on `Clear`) are preserved; **the memory saving is not**, and a
caller asking for a large region gets all of it resident immediately.

### Where the smoke test stops, and why

`SkylanderUSB` — the device wrapper the emulated IOS talks to — **cannot be
reached from a smoke test at all.** This was tried and abandoned deliberately.

Merely constructing a `SkylanderUSB` emits its vtable. The vtable references
every virtual, including `SubmitTransfer`, whose body calls
`cmd->GetEmulationKernel().GetSystem()`, then `system.GetMemory()`, then
`memory.GetPointerForRange()`. So the link pulls in `Core::System`,
`Memory::MemoryManager`, the IOS kernel, `Mixer`, and the sound stream — the
whole emulator singleton.

That is why the earlier link succeeded with `Skylander.cpp.o` present: nothing
instantiated the device, so the vtable was dead-stripped. Instantiating it is a
cliff, not a slope:

```
wasm-ld: undefined symbol: IOS::HLE::USB::Device::~Device()
wasm-ld: undefined symbol: IOS::HLE::USB::TransferCommand::GetEmulationKernel() const
wasm-ld: undefined symbol: Core::System::GetMemory() const
wasm-ld: undefined symbol: Memory::MemoryManager::GetPointerForRange(u32, size_t) const
wasm-ld: undefined symbol: Core::System::GetSkylanderPortal() const
wasm-ld: undefined symbol: Mixer::PushSkylanderPortalSamples(u8 const*, size_t)
...
```

So the honest split is:

| Layer | Status |
|---|---|
| `SkylanderFigure` — figure data and MIFARE crypto | verified, byte-identical |
| `SkylanderPortal` — status, block reads and writes | verified, byte-identical |
| `SkylanderUSB` — USB descriptors and transfer dispatch | **requires the emulator core** |
| The game talking to any of it | requires the core booted |

Everything the game actually does goes through the layer that is not covered.
Testing it is not a bigger smoke test; it is the next real Phase 1 milestone —
standing up `Core::System` under wasm32, which is what "the boot sequence
executes under the interpreter" in `docs/PLAN.md` §5 means. `libcore.a` defines
5,023 text symbols, and this is the point where they start being needed rather
than dead-stripped.

### Three times the test was wrong, not the code

Worth recording, because the pattern is the same one that has cost the most
time in this project — asserting an implementation detail as if it were a
contract:

1. Asserted a second view sits at `base + offset`. True only on Emscripten;
   native backends map at an unrelated virtual address.
2. Asserted overlapping views alias. True on neither, and `vm_remap` on Darwin
   does not behave that way.
3. Only then checked what `Memory::Init` actually does: one view per physical
   region, at distinct non-overlapping offsets. That is the contract, and both
   targets satisfy it.

### Linking notes

The wasm link needs only `SkylanderFigure.cpp.o` and `SkylanderCrypto.cpp.o`.
The native link additionally needs `list_skylanders` and
`NormalizeSkylanderType`, because `ld64` keeps the unreached reference from
`GetData()` that `wasm-ld` eliminates at `-O2`. `native_stubs.cpp` supplies
them, and `NormalizeSkylanderType` **aborts** rather than guessing, so the stub
cannot quietly answer wrong if the figure-data path is ever exercised.

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
