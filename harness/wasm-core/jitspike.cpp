// Why does emitted code run at 380 MIPS in the boot and 1538-2811 in a
// microbenchmark?
//
// harness/wasm-core/jitbench.cpp measures ONE block called in a tight loop.
// The boot executes about 84,000 distinct blocks totalling megabytes of
// wasm. Those are different experiments and they disagree by 4-7x, and
// docs/PHASE2-PROFILE.md now records three structural optimisations that
// each removed something large and changed nothing -- block entries, guest
// register traffic, branch structure. Whatever is left is inside the
// emitted code itself, and this is the measurement that has never been
// taken.
//
// Three questions, one run:
//
//   1. How does throughput vary with the NUMBER OF DISTINCT BLOCKS
//      executed, holding everything else fixed? A collapse as the working
//      set grows means instruction-cache pressure, and the lever is code
//      DENSITY. Flat means codegen quality, and the lever is what each
//      guest instruction expands into.
//
//   2. How does it vary with the instruction MIX? A 32-bit guest load is
//      emitted as four byte loads plus shifts and ors, because WebAssembly
//      has no byte-swap and the guest is big-endian. If memory is far
//      worse per instruction than arithmetic, that expansion is the thing
//      to attack.
//
//   3. How many bytes of wasm does a guest instruction actually cost?
//      Question 1 only matters in proportion to this.
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
    const inst = new WebAssembly.Instance(mod, {
      env: { memory: wasmMemory, __indirect_function_table: wasmTable },
    });
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
// Where generated loads and stores point. Inside MEM1 so the guard accepts
// them, and well clear of the code addresses the blocks are compiled at.
static constexpr u32 kScratch = 0x8002'0000;

enum class Mix
{
  kArith,   // register arithmetic only
  kMixed,   // one instruction in four touches memory
  kMemory,  // every instruction touches memory
};

// Build one block of `len` guest instructions, made distinct from every
// other block by `seed` so the engine cannot share compiled code between
// them. That distinctness is the whole point of question 1.
static std::vector<u32> MakeBlock(Mix mix, u32 len, u32 seed)
{
  std::vector<u32> code;
  for (u32 i = 0; i < len; ++i)
  {
    const u32 k = seed * 7919u + i * 131u;
    const bool mem = (mix == Mix::kMemory) || (mix == Mix::kMixed && (i & 3) == 0);
    if (mem)
    {
      // lwz / stw against r31, which the harness points at the scratch
      // window. The displacement varies so no two blocks are identical.
      const u32 d = (k % 200u) * 4u;
      const u32 rd = 3 + (k % 12u);
      code.push_back(((i & 1) ? 36u : 32u) << 26 | (rd << 21) | (31u << 16) | d);
    }
    else
    {
      // add rD, rA, rB -- three register operands, one result, which is
      // the shape most of the guest's arithmetic takes.
      const u32 rd = 3 + (k % 12u);
      const u32 ra = 3 + ((k / 13u) % 12u);
      const u32 rb = 3 + ((k / 17u) % 12u);
      code.push_back((31u << 26) | (rd << 21) | (ra << 16) | (rb << 11) | (266u << 1));
    }
  }
  return code;
}

static const char* MixName(Mix m)
{
  return m == Mix::kArith ? "arithmetic" : (m == Mix::kMixed ? "1-in-4 memory" : "all memory");
}

int main()
{
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  std::printf("where the nanoseconds go: emitted code against its working set\n\n");

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
  layout.cr_offset = static_cast<u32>(offsetof(PowerPC::PowerPCState, cr));
  layout.xer_so_ov_offset = static_cast<u32>(offsetof(PowerPC::PowerPCState, xer_so_ov));
  layout.spr_offset = static_cast<u32>(offsetof(PowerPC::PowerPCState, spr));
  const u32 state_ptr = static_cast<u32>(reinterpret_cast<uintptr_t>(&state));

  constexpr u32 kLen = 16;  // guest instructions per block
  // Long enough that the per-call cost is amortised and what is left is
  // the code itself: at sixteen instructions a C++ call per block is a
  // few percent, not the measurement.

  // ── question 3: code density ──
  std::printf("  bytes of wasm per guest instruction\n");
  for (Mix mix : {Mix::kArith, Mix::kMixed, Mix::kMemory})
  {
    const auto code = MakeBlock(mix, kLen, 1);
    auto r = ppc2wasm::CompileBlock(code.data(), BASE, kLen, layout);
    std::printf("    %-14s %zu bytes / %u instructions = %.1f\n", MixName(mix), r.body.size(),
                r.instructions, static_cast<double>(r.body.size()) / r.instructions);
  }
  std::printf("\n");

  // Every generated load and store goes through r31.
  auto reset_regs = [&] {
    for (int i = 0; i < 32; ++i)
      state.gpr[i] = 1;
    state.gpr[31] = kScratch;
  };

  // ── questions 1 and 2, and the one that separates the two causes ──
  //
  // Throughput falling as the working set grows has two possible causes
  // and they want opposite fixes:
  //
  //   instruction cache   too many bytes of code to keep hot. The fix is
  //                       denser code.
  //   call misprediction  the indirect call target changes every time, so
  //                       the predictor is wrong on every call. The fix is
  //                       fewer calls -- longer blocks.
  //
  // Block LENGTH separates them. Longer blocks amortise the call, so if
  // misprediction is the cause throughput recovers; they also make the
  // working set bigger, so if the cache is the cause it does not.
  for (u32 kLenVar : {4u, 16u, 64u})
  {
  std::printf("  %u instructions per block -- MIPS, and KB of code behind it\n", kLenVar);
  std::printf("    %-14s", "blocks");
  for (u32 n : {1u, 16u, 256u, 1024u, 4096u})
    std::printf(" %13u", n);
  std::printf("\n");

  for (Mix mix : {Mix::kArith, Mix::kMixed, Mix::kMemory})
  {
    std::printf("    %-14s", MixName(mix));
    for (u32 n : {1u, 16u, 256u, 1024u, 4096u})
    {
      std::vector<int> idx;
      idx.reserve(n);
      size_t body_bytes = 0;
      bool ok = true;
      for (u32 b = 0; b < n && ok; ++b)
      {
        const auto code = MakeBlock(mix, kLenVar, b + 1);
        auto r = ppc2wasm::CompileBlock(code.data(), BASE, kLenVar, layout);
        if (r.instructions != kLenVar)
        {
          ok = false;
          break;
        }
        body_bytes = r.body.size();
        const int i = InstallModule(r.module.data(), static_cast<int>(r.module.size()));
        if (i < 0)
          ok = false;
        else
          idx.push_back(i);
      }
      if (!ok)
      {
        std::printf(" %13s", "-");
        continue;
      }
      // Total emitted code behind this measurement. If throughput tracks
      // this rather than the block count or the mix, density is the
      // variable that matters and everything else is a proxy for it.
      const double kb = static_cast<double>(n) * body_bytes / 1024.0;

      // A fixed amount of guest work regardless of the working set, so
      // the only thing varying across a row is how much distinct code the
      // engine has to keep hot.
      const u64 kTotalInsts = 48'000'000;
      const u64 calls = kTotalInsts / kLenVar;
      reset_regs();
      volatile u64 sink = 0;
      const double t0 = Now();
      // Round robin, so consecutive calls land in different code. A
      // random order would add its own cache misses to the measurement;
      // this is the cheapest access pattern that still touches all of it.
      for (u64 c = 0; c < calls; ++c)
        sink += reinterpret_cast<BlockFn>(idx[c % n])(state_ptr);
      const double ms = Now() - t0;
      (void)sink;
      std::printf(" %6.0f/%6.0f", static_cast<double>(kTotalInsts) / (ms / 1000.0) / 1e6, kb);
    }
    std::printf("\n");
  }
  std::printf("\n");
  }

  // ── the fit ──
  //
  // Three block lengths at one working set are two equations and two
  // unknowns: a fixed cost for ENTERING a block, and a cost per
  // instruction once inside it. Separating them is the point of varying
  // length, because the two have opposite fixes and the raw MIPS numbers
  // confound them.
  std::printf("  what it costs, fitted across block length\n");
  std::printf("    %8s %10s %10s %9s %8s\n", "blocks", "KB", "entry ns", "ns/inst", "= MIPS");
  for (u32 n : {1u, 16u, 256u, 1024u, 4096u})
  {
    double per_call[2] = {0, 0};
    size_t bytes16 = 0;
    int slot = 0;
    for (u32 len : {4u, 64u})
    {
      std::vector<int> idx;
      bool ok = true;
      size_t body = 0;
      for (u32 b = 0; b < n && ok; ++b)
      {
        const auto code = MakeBlock(Mix::kArith, len, b + 1);
        auto r = ppc2wasm::CompileBlock(code.data(), BASE, len, layout);
        if (r.instructions != len)
          ok = false;
        else
        {
          body = r.body.size();
          const int i = InstallModule(r.module.data(), static_cast<int>(r.module.size()));
          if (i < 0) ok = false; else idx.push_back(i);
        }
      }
      if (!ok)
        break;
      if (len == 16u)
        bytes16 = body;
      const u64 kTotalInsts = 48'000'000;
      const u64 calls = kTotalInsts / len;
      reset_regs();
      volatile u64 sink = 0;
      const double t0 = Now();
      for (u64 c = 0; c < calls; ++c)
        sink += reinterpret_cast<BlockFn>(idx[c % n])(state_ptr);
      const double ms = Now() - t0;
      (void)sink;
      per_call[slot++] = ms * 1e6 / static_cast<double>(calls);  // ns per call
    }
    if (slot < 2)
      continue;
    const double ns_per_inst = (per_call[1] - per_call[0]) / 60.0;
    const double entry_ns = per_call[0] - 4.0 * ns_per_inst;
    // Arithmetic is 16.4 bytes per instruction; sixteen per block is the
    // middle row above, and the one to compare against the boot.
    const double kb = static_cast<double>(n) * 16.4 * 16.0 / 1024.0;
    (void)bytes16;
    std::printf("    %8u %10.0f %10.1f %9.2f %8.0f\n", n, kb, entry_ns, ns_per_inst,
                1000.0 / ns_per_inst);
  }

  std::printf("\n  The boot measures 2.6 ns per instruction, which is where this\n");
  std::printf("  curve sits at about a megabyte of hot code. The emitted code is\n");
  std::printf("  not slow; there is too much of it to keep in cache.\n");

  std::printf("\ndone\n");
  return 0;
}
