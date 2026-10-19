// Can Dolphin's emulator core come up under WebAssembly?
//
// Everything so far has tested leaf components -- figure crypto, portal
// replies -- that happen not to need the core. This is the first thing that
// requires Core::System itself: the singleton, its memory manager, and guest
// RAM allocated through MemArenaEmscripten.
//
// Goal of this step: MEM1 and MEM2 exist, are the right size, and are
// addressable. That is "the core is alive", not "the game runs".

#include <cstdio>
#include <cstring>
#include <iterator>

#include "Common/CommonTypes.h"
#include "Core/Core.h"
#include "Core/HW/Memmap.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/System.h"

static int g_failures = 0;

static void check(bool ok, const char* what)
{
  std::printf("  %-52s %s\n", what, ok ? "ok" : "FAIL");
  if (!ok)
    ++g_failures;
}

int main()
{
  // Unbuffered: a trap under PROXY_TO_PTHREAD loses buffered output, which
  // makes a crash look like it happened one step earlier than it did.
  std::setvbuf(stdout, nullptr, _IONBF, 0);

#if defined(__EMSCRIPTEN__)
  std::printf("target: wasm32 (emscripten)\n\n");
#else
  std::printf("target: native\n\n");
#endif

  std::printf("Core::System\n");
  auto& system = Core::System::GetInstance();
  check(true, "singleton constructed");

  auto& memory = system.GetMemory();
  check(true, "memory manager reachable");

  // Memmap gates MEM2 on System::IsWii(), which defaults false. Without this
  // the core comes up as a GameCube and MEM2 is never allocated -- which is
  // correct behaviour, and was the reason the first run reported a null MEM2.
  // Skylanders is a Wii title, so Wii mode is what has to work.
  system.SetIsWii(true);
  check(system.IsWii(), "core is in Wii mode");

  memory.Init();
  // GetRamSize() is NextPowerOf2(GetRamSizeReal()), so a 24 MB Wii MEM1
  // reports as 32 MB through the rounded accessor. The real size is the one
  // that matches hardware; the first version of this test asserted the rounded
  // one and failed for a reason that had nothing to do with wasm.
  std::printf("  MEM1 %p  real 0x%08x  rounded 0x%08x\n", static_cast<void*>(memory.GetRAM()),
              memory.GetRamSizeReal(), memory.GetRamSize());
  std::printf("  MEM2 %p  real 0x%08x  rounded 0x%08x\n", static_cast<void*>(memory.GetEXRAM()),
              memory.GetExRamSizeReal(), memory.GetExRamSize());

  check(memory.GetRAM() != nullptr, "MEM1 allocated");
  check(memory.GetRamSizeReal() == 0x0180'0000, "MEM1 is 24 MB (Wii)");
  check(memory.GetEXRAM() != nullptr, "MEM2 allocated");
  check(memory.GetExRamSizeReal() == 0x0400'0000, "MEM2 is 64 MB (Wii)");

  // Guest RAM must be writable through the pointers the emulator hands out,
  // across the REAL extent -- the rounded size is larger than what is mapped.
  if (u8* ram = memory.GetRAM())
  {
    const u32 n = memory.GetRamSizeReal();
    ram[0] = 0xAB;
    ram[n - 1] = 0xCD;
    check(ram[0] == 0xAB && ram[n - 1] == 0xCD, "MEM1 round-trips first and last byte");
  }
  if (u8* exram = memory.GetEXRAM())
  {
    const u32 n = memory.GetExRamSizeReal();
    exram[0] = 0x12;
    exram[n - 1] = 0x34;
    check(exram[0] == 0x12 && exram[n - 1] == 0x34, "MEM2 round-trips first and last byte");
  }

  // ── PowerPC ──
  // ENABLE_GENERIC excludes both recompilers, so the interpreter should be the
  // only core offered. If a JIT showed up here the generic build would not be
  // what it claims to be.
  std::printf("\nPowerPC\n");
  const auto cores = PowerPC::AvailableCPUCores();
  std::printf("  available CPU cores:");
  bool has_interp = false, has_jit = false;
  for (const auto c : cores)
  {
    std::printf(" %d", static_cast<int>(c));
    if (c == PowerPC::CPUCore::Interpreter)
      has_interp = true;
    if (c == PowerPC::CPUCore::JIT64 || c == PowerPC::CPUCore::JITARM64)
      has_jit = true;
  }
  std::printf("\n");
  check(has_interp, "interpreter is available");
  check(!has_jit, "no recompiler is offered (generic build)");
  check(PowerPC::DefaultCPUCore() == PowerPC::CPUCore::Interpreter ||
            PowerPC::DefaultCPUCore() == PowerPC::CPUCore::CachedInterpreter,
        "default core is an interpreter");

  // PowerPC::Init asserts Core::IsCPUThread(). Dolphin enforces thread
  // affinity on the emulated CPU, and that assertion fires correctly under
  // wasm -- which is reassuring for the worker topology Spike C validated,
  // since it means the checks guarding it are live rather than compiled out.
  Core::DeclareAsCPUThread();
  check(Core::IsCPUThread(), "this thread is now the CPU thread");

  auto& ppc = system.GetPowerPC();
  ppc.Init(PowerPC::CPUCore::Interpreter);
  check(true, "PowerPC initialised with the interpreter");

  auto& state = ppc.GetPPCState();
  state.pc = 0x8000'0000;
  state.gpr[1] = 0x8170'0000;
  check(state.pc == 0x8000'0000, "guest PC is settable");

  // ── execute guest code ──
  // Up to here the interpreter was only initialised. This actually fetches,
  // decodes and runs Broadway instructions out of guest RAM.
  //
  // Encodings, hand-assembled so the expected results are checkable by hand:
  //   addi  r3, r0, 0x1234   0x38601234   r3 = 0x1234
  //   addi  r4, r0, 0x0100   0x38800100   r4 = 0x0100
  //   add   r5, r3, r4       0x7CA32214   r5 = 0x1334
  //   mullw r6, r3, r4       0x7CC321D6   r6 = 0x123400
  //   or    r7, r3, r4       0x7C672378   r7 = 0x1334
  std::printf("\nGuest execution\n");
  static constexpr u32 PROGRAM[] = {
      0x3860'1234, 0x3880'0100, 0x7CA3'2214, 0x7CC3'21D6, 0x7C67'2378,
  };
  constexpr u32 BASE = 0x8000'0000;

  for (u32 i = 0; i < std::size(PROGRAM); ++i)
    memory.Write_U32(PROGRAM[i], BASE + i * 4);
  check(memory.Read_U32(BASE) == PROGRAM[0], "program reads back from guest RAM");

  state.pc = BASE;
  state.npc = BASE;
  for (u32 i = 0; i < 3; ++i)
    state.gpr[3 + i] = 0;

  for (u32 i = 0; i < std::size(PROGRAM); ++i)
    ppc.SingleStep();

  std::printf("  r3=0x%08x r4=0x%08x r5=0x%08x r6=0x%08x r7=0x%08x  pc=0x%08x\n", state.gpr[3],
              state.gpr[4], state.gpr[5], state.gpr[6], state.gpr[7], state.pc);

  check(state.gpr[3] == 0x0000'1234, "addi r3 -> 0x1234");
  check(state.gpr[4] == 0x0000'0100, "addi r4 -> 0x0100");
  check(state.gpr[5] == 0x0000'1334, "add r5 = r3 + r4");
  check(state.gpr[6] == 0x0012'3400, "mullw r6 = r3 * r4");
  check(state.gpr[7] == 0x0000'1334, "or r7 = r3 | r4");
  check(state.pc == BASE + std::size(PROGRAM) * 4, "pc advanced past the program");

  // Guest load/store. This is the path MemArenaEmscripten exists to serve:
  // every guest memory access goes through the software MMU, since fastmem is
  // refused on this backend. Arithmetic working proves the decoder; this
  // proves the memory path.
  //   stw r6, 0x200(r0)   0x90C00200   store r6 to guest 0x200
  //   lwz r8, 0x200(r0)   0x81000200   load it back into r8
  static constexpr u32 MEMPROG[] = {0x90C0'0200, 0x8100'0200};
  constexpr u32 MBASE = 0x8000'0100;
  for (u32 i = 0; i < std::size(MEMPROG); ++i)
    memory.Write_U32(MEMPROG[i], MBASE + i * 4);

  state.pc = MBASE;
  state.npc = MBASE;
  state.gpr[8] = 0;
  for (u32 i = 0; i < std::size(MEMPROG); ++i)
    ppc.SingleStep();

  std::printf("  stored r6=0x%08x -> guest 0x200, loaded back r8=0x%08x\n", state.gpr[6],
              state.gpr[8]);
  check(state.gpr[8] == state.gpr[6], "lwz reads back what stw wrote");
  check(memory.Read_U32(0x8000'0200) == state.gpr[6],
        "host sees the guest store at the right address");

  ppc.Shutdown();
  Core::UndeclareAsCPUThread();
  check(!Core::IsCPUThread(), "PowerPC shut down and CPU thread released");

  memory.Shutdown();
  check(true, "memory shut down cleanly");

  std::printf("\n%s (%d failures)\n", g_failures ? "FAILED" : "PASSED", g_failures);
  return g_failures ? 1 : 0;
}
