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
✓ Wii memory map up                 MEM1 24 MB + MEM2 64 MB, both writable
✓ PowerPC interpreter initialises   no recompiler offered, thread affinity enforced
✓ Broadway instructions EXECUTE     arithmetic + load/store through the software MMU
✓ Paired singles execute            and the contraction risk is narrowed, not confirmed
✓ Differential widened              19 instructions, 41 assertions, native == wasm32
✓ MMU address translation           BAT-mapped loads/stores, verified host-side
✓ Exceptions                        DSI raised, dispatched, SRR0/DAR/DSISR correct
~ IOS kernel                        constructs under wasm with all device nodes; teardown traps
✓ Trace differ                      instruction-level, reports the FIRST divergence
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

### Wii mode, and the PowerPC core

`System::SetIsWii(true)` is all Wii mode needed — `Memmap` gates MEM2 on it,
and it defaults false, which is why the first run reported a null MEM2. The
full Wii memory map now comes up, and the PowerPC core initialises on top of
it:

```
MEM1 0x690000   real 0x01800000  rounded 0x02000000
MEM2 0x26d0000  real 0x04000000  rounded 0x04000000
MEM1 is 24 MB (Wii)                        ok
MEM2 is 64 MB (Wii)                        ok
MEM1 round-trips first and last byte       ok
MEM2 round-trips first and last byte       ok

PowerPC
  available CPU cores: 5 0
  interpreter is available                 ok
  no recompiler is offered (generic build) ok
  this thread is now the CPU thread        ok
  PowerPC initialised with the interpreter ok
  guest PC is settable                     ok
```

88 MB of guest RAM, allocated through `MemArenaEmscripten` and writable across
its real extent.

`available CPU cores: 5 0` is CachedInterpreter and Interpreter — **no
recompiler is offered**, confirming `ENABLE_GENERIC` is doing what it claims
rather than silently falling back to something.

Two things the earlier run got wrong, both the test's fault:

- **MEM1 "32 MB"** was `GetRamSize()`, which is `NextPowerOf2(GetRamSizeReal())`.
  The real size is 24 MB and always was. The test asserted the rounded accessor
  and failed for a reason unrelated to wasm.
- **`-D_M_GENERIC=1` was missing** from the test's own compile. The libraries
  had it; the test did not, so `MachineContext.h` fell through to
  `#error No context definition for OS` instead of taking its existing generic
  path. Anything that *had* compiled would have been an ODR hazard. Compile
  flags for a program linking these libraries must match what CMake used.

`PowerPC::Init` asserts `Core::IsCPUThread()`, which fired correctly and needed
`Core::DeclareAsCPUThread()`. That assertion being live under wasm is
reassuring for the worker topology Spike C validated — the thread-affinity
checks are real, not compiled out.

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

## Guest code runs

Everything above initialised things. This runs them: Broadway instructions
fetched from guest RAM, decoded and executed by the interpreter, under
WebAssembly.

```
Guest execution
  program reads back from guest RAM                 ok
  r3=0x00001234 r4=0x00000100 r5=0x00001334
  r6=0x00123400 r7=0x00001334  pc=0x80000014
  addi r3 -> 0x1234                                 ok
  addi r4 -> 0x0100                                 ok
  add   r5 = r3 + r4                                ok
  mullw r6 = r3 * r4                                ok
  or    r7 = r3 | r4                                ok
  pc advanced past the program                      ok
  stored r6=0x00123400 -> guest 0x200, loaded back r8=0x00123400
  lwz reads back what stw wrote                     ok
  host sees the guest store at the right address    ok
```

The encodings are hand-assembled so the expected results are checkable by hand
rather than taken from the emulator being tested:

| Instruction | Encoding | Result |
|---|---|---|
| `addi r3, r0, 0x1234` | `0x38601234` | `r3 = 0x1234` |
| `addi r4, r0, 0x0100` | `0x38800100` | `r4 = 0x0100` |
| `add r5, r3, r4` | `0x7CA32214` | `r5 = 0x1334` |
| `mullw r6, r3, r4` | `0x7CC321D6` | `r6 = 0x123400` |
| `or r7, r3, r4` | `0x7C672378` | `r7 = 0x1334` |
| `stw r6, 0x200(r0)` | `0x90C00200` | guest `0x200` = `0x123400` |
| `lwz r8, 0x200(r0)` | `0x81000200` | `r8 = 0x123400` |

The arithmetic proves the decoder and the architectural state updates. The
load/store pair proves the **memory path** — every guest access goes through
the software MMU, because `MemArenaEmscripten` refuses fastmem, so this is the
configuration the whole port depends on.

**What this is not.** Seven instructions out of roughly a thousand Broadway
opcodes. No branches, no condition register, no floating point, and above all
**no paired singles** — the instruction class `docs/PLAN.md` §2 flags as a
Fatal risk because of float contraction. Nothing is compared against native
here either; the expected values are hand-derived, which catches a broken
decoder but not a subtly wrong one.

## Paired singles, and what happened to the contraction risk

`docs/PLAN.md` §2 carried float contraction as a **Fatal** risk: aarch64 fuses
`a*c + b` into one rounding step while WebAssembly has no scalar FMA and rounds
twice. Spike A found exactly that divergence in hand-written Rust.

Paired singles now execute under wasm32 — they need `MSR[FP]` *and*
`HID2[PSE]`, Broadway's paired-single enable, without which every `ps_*`
instruction raises a program exception. The first run produced all-zero results
and a PC that never advanced, which is what that exception looks like from
outside.

```
ps_add  f3 = 4013000000000000 4000000020000000
ps_mul  f4 = 4011800000000000 3ff0000040000000
ps_madd f5 = 0000000000000000 3d10000000000000
ps_madd exact case cancels to zero                    ok
```

**The risk is narrower than it looked.** Dolphin computes `ps_madd` in *double*
(`NI_madd_msub`, which deliberately emulates Broadway's unusual FMA semantics
in software). A float32 significand is 24 bits, so `a*c` is at most 48 bits —
**exact** in double's 53. When `round(a*c) == a*c`, fused and unfused both round
exactly once and cannot differ. Checked over 200,000 random float32 triples:
zero disagreements.

So contraction cannot change the result for paired singles carrying legitimate
float32 data. It remains open for two cases: PS registers holding values that
are *not* float32-exact, and `ppc2wasm`, which will compute in whatever
precision it emits rather than inheriting Dolphin's doubles.

**Two wrong attempts before that reasoning.** The first discriminator used
values that give the same answer fused or unfused — it could not have detected
contraction at all. The second used values finer than float32 can represent, so
they never survived into the registers. Both passed or failed for reasons
unrelated to the question being asked.

### The native differential now exists

The previous attempt was abandoned: linking against the **oracle** build pulled
in FFmpeg, Discord, libusb, cubeb and a long circular external set, with each
missing library surfacing one at a time in `dyld` at runtime. And a native
*generic* build — the one that would actually match the wasm configuration —
did not compile at all.

Both are fixed. `harness/wasm-core/run.sh` builds the same source for wasm32
and for a native **generic** build and diffs the output:

```
══ differential ══
MATCH -- native and wasm32 agree on every value
both targets PASSED
```

Every value agrees: the integer results, the guest load/store, and the
paired-single bit patterns including `paired-single fnv1a=d0bbd6b37621525d`.
Only the target banner and the host allocation addresses differ, and the runner
normalises those away explicitly rather than ignoring whole lines.

## Address translation and exceptions

Everything before this ran with the MMU **off**, so guest `0x80000000` reached
MEM1 only because translation was bypassed. Real game code runs translated, and
with fastmem refused on this backend every translated access goes through the
software MMU — the path `MemArenaEmscripten` exists to serve.

```
Address translation
  MSR[IR] and MSR[DR] enabled                              ok
  translated store/load: r17=0xfeedbeef  MEM1[0x40]=0xfeedbeef
  lwz reads back through translation                       ok
  the store landed at the translated physical address      ok

Exceptions
  DAR=0xa0000000 DSISR=0x40000000 SRR0=0x80000704 pc=0x00000300
  DAR records the faulting address                         ok
  DSISR describes the fault                                ok
  control transferred to the DSI vector at 0x300           ok
  SRR0 points at the faulting instruction                  ok
  the faulting load did not write its target register      ok
```

Translation is checked **behaviourally** — by where a guest store actually
lands, confirmed from the host side — rather than by calling the translator.
`MMU::TranslateAddress` is private, and the observable effect is what matters
anyway: a translator returning the right number while stores went elsewhere
would pass a direct test and fail this one.

The exception test is the first here that asserts the emulator **refuses**
something rather than succeeds at it.

### Four things this cost, each a wrong assumption

1. **`r0` means literal zero** in the `rA` field of a load or store. So
   `stw r3, 0x40(r0)` addresses `0x40`, not `0x80000040` — outside the BAT
   range, which faults once translation is on. The untranslated tests got away
   with it because effective and physical addresses were the same.
2. **Accurate MMU is off by default.** Dolphin logs bad accesses and continues,
   because most games never rely on MMU exceptions and checking costs speed.
   The log line says so explicitly: *"Enable MMU in advanced settings to
   accurately emulate game crashes."*
3. **`Config::SetCurrent` needs `Config::Init()`** or the write is silently
   dropped and `Config::Get` keeps returning the compiled-in default. Adding
   UICommon's *base* layer on top is worse — `GenerateBaseConfigLoader` reads
   `Dolphin.ini` and drags in the IOS filesystem, which asserts on an unset
   user path.
4. **Exception vectors are at `0x300`, not `0x80000300`.** PowerPC puts them at
   the low addresses when `MSR[IP]` is clear. The `0x8000_0000` form is where
   they are *reached from* once a BAT maps them, not where the architecture
   puts them.

### The differential caught something wasm alone would have missed

`System::Initialize()` reads `SConfig::GetInstance().GetSimulatedMemorySize()`.
Without `SConfig::Init()` first, the native build **segfaults** — and wasm32
did **not**. The uninitialised singleton silently returned something under
wasm and the test sailed past it.

That is the failure direction this project has hit repeatedly, now caught
rather than shipped. It is also the clearest argument yet for the differential
existing at all: a wasm-only run would have reported PASSED.

## The trace differ

`docs/PLAN.md` Bet 04 makes native Dolphin the test oracle. Until now that was
aspirational: every assertion in this harness was hand-written, and **six of
them turned out to be wrong**, each failing in the direction of looking like a
port bug. That approach does not survive a boot sequence of millions of
instructions.

`harness/wasm-core/run-trace.sh` captures an instruction-level CPU trace from
both targets and diffs them:

```
native: 101 steps    wasm32: 101 steps
MATCH -- 101 steps identical
```

One line per executed instruction — step, PC, a hash of all 32 GPRs, CR, MSR,
LR, CTR — written to **stdout**, because Emscripten's MEMFS is not shared with
the host and `NODERAWFS` conflicts with `PROXY_TO_PTHREAD`. The `T ` prefix
separates trace lines from ordinary program output in the same stream.

**The value is in the word *first*.** Once two runs disagree, every later line
disagrees too; a plain `diff` reports thousands of lines of consequence. The
differ reports the earliest divergence, six lines of context, and which fields
changed:

```
FIRST DIVERGENCE at trace line 39 (step 00000027)
  ok   step=00000026  pc=80001020  gpr_hash=3290fca432f7e02c  ...
  NAT  step=00000027  pc=80001010  gpr_hash=3290fca432f7e02c  ...
  WASM step=00000027  pc=80001010  gpr_hash=deadbeefdeadbeef  ...

  fields that differ: gpr_hash
  only register contents differ. Re-run both with --dump-at=39
```

It distinguishes the two cases that matter: a differing **PC** means control
flow diverged — a branch or exception resolved differently — while a differing
**gpr_hash** means only data changed. `--dump-at=N` is the second pass,
replaying to that step and printing all 32 registers.

**It was verified by breaking it.** A differ that always says MATCH is worse
than no differ, so a divergence was injected into a known-good trace and the
tool was checked for finding exactly that step and exiting non-zero.

### One real bug it surfaced immediately

Linking the trace test warned about a `pthread_setname_np` signature mismatch:
the stub took one argument, Dolphin calls the two-argument form. **`wasm-ld`
only warns** — the wrong arity links cleanly and corrupts the call at runtime.
Fixing it then broke the native build, because macOS declares the
single-argument form and supplies a real implementation, so the stub is now
wasm-only.

## IOS — partial, and deliberately outside the differential

IOS is the Wii's operating system, and it is the layer the game reaches the
Skylander portal through. `harness/wasm-core/ios_probe.cpp` is how far it gets.

**Under wasm32 the kernel constructs and every device node is present:**

```
  guest memory up                                      ok
  session NAND root is set                             ok
  core timing initialised                              ok
  IOS kernel constructed                               ok
  /dev/aes                                             ok
  /dev/sha                                             ok
  /dev/es                                              ok
  /dev/usb/oh0                                         ok      <- the portal's node
  [about to destroy the kernel -- wasm32 traps here]
```

`/dev/usb/oh0` existing is the point: that is the node the game opens to reach
the portal, and it is present under WebAssembly.

**But both targets fail, at different points.**

| | Outcome |
|---|---|
| wasm32 | kernel constructs, all nodes present, then the **destructor traps** |
| native | **SIGSEGV during `IOS::HLE::Init`**, before the kernel exists |

Two targets failing *differently* is the signature of **missing
initialisation**, not a port bug. Dolphin normally reaches IOS through
`Core::Init`, which brings up considerably more than the probe does. The
destructor that traps under wasm (`EmulationKernel::~EmulationKernel`) removes
its CoreTiming events, clears the device map, and resets the socket manager —
one of those three, not yet narrowed further.

**Why it is not in the differential.** `coreboot.cpp` stays green on both
targets; adding IOS would have made it red on both for a reason that is about
setup rather than about the port. The probe records the state instead. Getting
IOS up properly means standing up more of `Core::Init`, which is the next real
block of work rather than another increment.

### Two things that were needed just to get this far

`UICommon::SetUserDirectory` alone leaves `D_SESSION_WIIROOT_IDX` empty, and
`FS::MakeFileSystem` asserts on an empty root path.
`Core::InitializeWiiRoot(true)` fills it in with a temporary NAND.

Linking IOS pulled in the Wii Speak microphone, which needs cubeb — plus FatFs,
FreeSurround, minizip-ng, pugixml, miniupnpc and hidapi. All were already built
for wasm; they just had to be added to the link line.

### What the differential covers

19 distinct Broadway instructions across five areas, 41 assertions, every value
agreeing between native and wasm32:

| Area | Instructions |
|---|---|
| Integer arithmetic | `addi` `add` `subf` `neg` `mullw` `or` |
| Shift / rotate | `rlwinm` |
| Condition register | `cmpwi` `mfcr` |
| Control flow | `bc` (taken `beq`) |
| Word memory | `stw` `lwz` |
| Sub-word memory | `stb` `lbz` `sth` `lhz` |
| Paired singles | `ps_add` `ps_mul` `ps_madd` |

Two of these are worth more than their count suggests.

**The conditional branch** is checked by what it *skips*: the instruction after
`beq` writes `0x0BAD` into a register, and the test asserts that register is
still zero. A branch that silently fell through would pass a test that only
looked at the target.

**Sub-word memory is the endianness check.** Broadway is big-endian, wasm32 is
little-endian, and a word-sized round trip can pass while byte swapping is
wrong — swapping twice cancels. `stb`/`lbz` and `sth`/`lhz` are verified from
*both* sides: the guest reads back what it wrote, and the host independently
sees the right bytes at the right guest addresses. A consistent swap bug would
satisfy the first check and fail the second.

**Why generic rather than the oracle.** Both sides being `ENABLE_GENERIC`
builds means the only difference between the two binaries is the target. A
differential against the JIT-enabled oracle would have compared two different
configurations and called the result a port check.

**The `MemTools.cpp` fix.** That file is the fastmem fault handler. Its POSIX
branch already excluded `_M_GENERIC`; its Apple branch did not, so a generic
build on macOS hit `#error Unsupported architecture` before reaching the
unsupported-platform stub at the bottom of the same file. Two guards make the
Apple branch match the POSIX one. A JIT-less build has no fastmem faults to
catch, so falling through to the stub is correct.

This upgrades the paired-single result from "wasm-only, reasoned from
IEEE-754" to the same differential footing as the figure crypto and the portal.

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
