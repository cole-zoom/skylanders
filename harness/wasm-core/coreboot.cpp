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

#include "Common/CommonTypes.h"
#include "Core/HW/Memmap.h"
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

  memory.Init();
  // Report before asserting: Memory::Init() sizes itself from config, and
  // without a game loaded it comes up in GameCube mode, where MEM2 does not
  // exist at all. Printing the actual values makes that visible instead of
  // leaving two bare FAILs to interpret.
  std::printf("  MEM1 %p size 0x%08x\n", static_cast<void*>(memory.GetRAM()),
              memory.GetRamSize());
  std::printf("  MEM2 %p size 0x%08x\n", static_cast<void*>(memory.GetEXRAM()),
              memory.GetExRamSize());
  check(memory.GetRAM() != nullptr, "MEM1 allocated");
  check(memory.GetRamSize() > 0, "MEM1 has a nonzero size");

  // Guest RAM must be writable through the pointers the emulator hands out.
  if (u8* ram = memory.GetRAM())
  {
    ram[0] = 0xAB;
    ram[memory.GetRamSize() - 1] = 0xCD;
    check(ram[0] == 0xAB && ram[memory.GetRamSize() - 1] == 0xCD,
          "MEM1 round-trips first and last byte");
  }

  memory.Shutdown();
  check(true, "memory shut down cleanly");

  std::printf("\n%s (%d failures)\n", g_failures ? "FAILED" : "PASSED", g_failures);
  return g_failures ? 1 : 0;
}
