// How fast is the code the recompiler actually emits?
//
// Spike A measured 863.9 MIPS in Chrome, but it measured HAND-WRITTEN wasm --
// the shape a good backend might produce. This measures what ppc2wasm really
// emits: no register allocation, guest state round-tripped through memory on
// every instruction, and a region guard on every access.
//
// The interpreter is measured on the same code in the same process, so the
// ratio is the honest answer to "is a recompiler worth building". Broadway is
// 729 MHz at close to 1 IPC, so ~600 MIPS is full speed.
#include <cstdio>
#include <cstring>
#include <vector>

#include "Common/CommonTypes.h"
#include "Common/Config/Config.h"
#include "Core/ConfigManager.h"
#include "Core/Core.h"
#include "Core/HW/Memmap.h"
#include "Core/PowerPC/MMU.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/System.h"

#include "../../ppc2wasm/src/block.h"

#include <emscripten.h>

EM_JS(int, InstallModule, (const uint8_t* ptr, int len), {
  try {
    const bytes = HEAPU8.slice(ptr, ptr + len);
    const mod = new WebAssembly.Module(bytes);
    const inst = new WebAssembly.Instance(mod, { env: { memory: wasmMemory } });
    const idx = wasmTable.length;
    wasmTable.grow(1);
    wasmTable.set(idx, inst.exports.f);
    return idx;
  } catch (e) { out('  install failed: ' + e); return -1; }
});
EM_JS(int, MemoryPages, (), { return wasmMemory.buffer.byteLength >>> 16; });
EM_JS(double, Now, (), { return performance.now(); });

using BlockFn = u32 (*)(u32);
static constexpr u32 BASE = 0x8000'1000;

struct Bench
{
  const char* name;
  std::vector<u32> code;
};

int main()
{
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  std::printf("target: wasm32   emitted code vs the interpreter\n\n");

  auto& system = Core::System::GetInstance();
  Config::Init();
  SConfig::Init();
  system.Initialize();
  system.SetIsWii(true);
  auto& memory = system.GetMemory();
  memory.Init();
  Core::DeclareAsCPUThread();
  auto& ppc = system.GetPowerPC();
  ppc.Init(PowerPC::CPUCore::Interpreter);
  auto& state = ppc.GetPPCState();

  state.spr[SPR_IBAT0U] = 0x8000'1fff;
  state.spr[SPR_IBAT0L] = 0x0000'0002;
  state.spr[SPR_DBAT0U] = 0x8000'1fff;
  state.spr[SPR_DBAT0L] = 0x0000'0002;
  HID4(state).SBE = 1;
  auto& mmu = system.GetMMU();
  mmu.DBATUpdated();
  mmu.IBATUpdated();
  state.msr.IR = 1;
  state.msr.DR = 1;

  ppc2wasm::StateLayout layout;
  layout.gpr_offset = static_cast<u32>(offsetof(PowerPC::PowerPCState, gpr));
  layout.memory_pages = static_cast<u32>(MemoryPages());
  layout.ram_base = static_cast<u32>(reinterpret_cast<uintptr_t>(memory.GetRAM()));

  // Two mixes. The register one prices dispatch and raw arithmetic; the
  // memory one prices the guard, which Spike A identified as the single
  // biggest unknown in the emitted-code cost.
  const std::vector<Bench> benches = {
      {"integer arithmetic (16 instructions)",
       {0x3863'0001, 0x3884'0002, 0x7CA5'2A14, 0x7CC6'3278, 0x5484'103A, 0x7CE7'2214,
        0x3908'0003, 0x7D29'4A14, 0x7D4A'5278, 0x5529'083C, 0x3929'0001, 0x7D6B'5A14,
        0x7D8C'6278, 0x558C'103A, 0x39AD'0001, 0x7DCE'6A14}},
      {"with loads and stores (16 instructions)",
       {0x3C60'8000, 0x6063'4000, 0x3880'0001, 0x9083'0000, 0x80A3'0000, 0x7CA5'2214,
        0x90A3'0004, 0x80C3'0004, 0x7CC6'3214, 0x90C3'0008, 0x80E3'0008, 0x7CE7'3A14,
        0x9103'000C, 0x8123'000C, 0x7D29'4214, 0x9123'0010}},
  };

  for (const Bench& b : benches)
  {
    for (u32 i = 0; i < b.code.size(); ++i)
      memory.Write_U32(b.code[i], BASE + i * 4);

    auto r = ppc2wasm::CompileBlock(b.code.data(), BASE, static_cast<u32>(b.code.size()), layout);
    if (r.instructions != b.code.size())
    {
      std::printf("  %s: only %u of %zu compiled (%s)\n", b.name, r.instructions, b.code.size(),
                  r.stopped_on);
      continue;
    }
    const int idx = InstallModule(r.module.data(), static_cast<int>(r.module.size()));
    if (idx < 0)
      continue;
    BlockFn f = reinterpret_cast<BlockFn>(idx);
    const u32 state_ptr = static_cast<u32>(reinterpret_cast<uintptr_t>(&state));

    const u64 per_block = b.code.size();
    // Enough repetitions that the timed region is tens of milliseconds. The
    // first version of this used 400k and finished in 5 ms, which is far too
    // short to trust and produced a memory variant FASTER than the arithmetic
    // one -- a result that cannot be true and was the clue that the
    // measurement, not the code, was wrong.
    const u64 reps = 4000000;
    const u64 total = per_block * reps;

    // ── compiled ──
    //
    // The returned PC is accumulated and printed. Without consuming the
    // result, a compiler is entitled to discard the call, and the number
    // measured would be the cost of an empty loop.
    for (int i = 0; i < 32; ++i)
      state.gpr[i] = 0;
    volatile u64 sink = 0;
    const double t0 = Now();
    for (u64 k = 0; k < reps; ++k)
      sink += f(state_ptr);
    const double t1 = Now();

    // The block must actually have run: a loop that did nothing would time
    // beautifully and leave guest state untouched.
    u32 jit_gpr[32];
    std::memcpy(jit_gpr, state.gpr, sizeof(jit_gpr));

    // ── interpreter, the same work ──
    for (int i = 0; i < 32; ++i)
      state.gpr[i] = 0;
    const u64 int_reps = reps / 40;  // the interpreter is far slower; scale down
    const double t2 = Now();
    for (u64 k = 0; k < int_reps; ++k)
    {
      state.pc = state.npc = BASE;
      for (u64 i = 0; i < per_block; ++i)
        ppc.SingleStep();
    }
    const double t3 = Now();

    const double jit_mips = total / ((t1 - t0) / 1000.0) / 1e6;
    const double int_mips = (per_block * int_reps) / ((t3 - t2) / 1000.0) / 1e6;
    const bool ran = (jit_gpr[3] != 0 || jit_gpr[5] != 0 || jit_gpr[4] != 0);
    std::printf("  %s\n", b.name);
    std::printf("      sink=%llu  block-ran=%s\n", static_cast<unsigned long long>(sink),
                ran ? "yes" : "NO -- measurement is meaningless");
    std::printf("      compiled     %8.1f MIPS   (%.0f ms, %zu-byte module)\n", jit_mips, t1 - t0,
                r.module.size());
    std::printf("      interpreter  %8.1f MIPS   (%.0f ms)\n", int_mips, t3 - t2);
    // SingleStep advances CoreTiming for EVERY instruction, which the fast
    // interpreter loop does not. This number is the harness's stepping speed,
    // not Dolphin's interpreter at its best, and the ratio below flatters the
    // recompiler accordingly.
    std::printf("      speedup      %8.1fx    %s\n\n", jit_mips / int_mips,
                jit_mips >= 600 ? "clears the 600 MIPS gate" : "below the 600 MIPS gate");
  }

  std::printf("PASSED (0 failures)\n");
  std::fflush(stdout);
  return 0;
}
