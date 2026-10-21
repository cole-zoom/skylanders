// Recompiled blocks against the interpreter, in the same process.
//
// This is the differential that matters for Phase 2. Every other comparison
// in this project runs the same interpreter on two targets; this runs TWO
// DIFFERENT EXECUTION ENGINES on one target and requires the guest state to
// come out identical.
//
// The interpreter is the oracle, which is the right choice for one reason:
// it is already verified byte-for-byte against native over billions of
// instructions, so a disagreement here is the recompiler's fault with very
// little room for argument.
//
// Runs only under Emscripten -- a compiled block is WebAssembly, and there is
// nothing to execute it with natively. The encoder half is tested separately
// in jittest.cpp, which does run in CI.
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

static int g_failures = 0;

static void check(bool ok, const char* what)
{
  std::printf("  %-58s %s\n", what, ok ? "ok" : "FAIL");
  if (!ok)
    ++g_failures;
}

EM_JS(int, InstallModule, (const uint8_t* ptr, int len), {
  try {
    const bytes = HEAPU8.slice(ptr, ptr + len);
    const mod = new WebAssembly.Module(bytes);
    const inst = new WebAssembly.Instance(mod, { env: { memory: wasmMemory } });
    const idx = wasmTable.length;
    wasmTable.grow(1);
    wasmTable.set(idx, inst.exports.f);
    return idx;
  } catch (e) {
    out('  install failed: ' + e);
    return -1;
  }
});

EM_JS(int, MemoryPages, (), { return wasmMemory.buffer.byteLength >>> 16; });

using BlockFn = u32 (*)(u32);

static constexpr u32 BASE = 0x8000'1000;

// One case: a program, how many instructions it should translate, and a name.
struct Case
{
  const char* name;
  std::vector<u32> code;
  // When set, the block is expected to refuse this instruction index and
  // return its PC instead of running it.
  int expect_bail_at = -1;
};

int main()
{
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  std::printf("target: wasm32  recompiled blocks vs the interpreter\n\n");

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

  // Address translation, so the INTERPRETER can perform data accesses at all.
  // Without this a guest store raises a DSI exception and reads come back
  // zero -- which looks exactly like a recompiler bug, and did: the emitted
  // code was returning the right values while the oracle returned nothing.
  //
  // Values copied from CBoot::SetupBAT (private, hence duplicated):
  //   DBAT0/IBAT0: 0x80000000 -> physical 0x00000000 (MEM1)
  //   DBAT4/IBAT4: 0x90000000 -> physical 0x10000000 (MEM2)
  state.spr[SPR_IBAT0U] = 0x8000'1fff;
  state.spr[SPR_IBAT0L] = 0x0000'0002;
  state.spr[SPR_DBAT0U] = 0x8000'1fff;
  state.spr[SPR_DBAT0L] = 0x0000'0002;
  state.spr[SPR_IBAT4U] = 0x9000'1fff;
  state.spr[SPR_IBAT4L] = 0x1000'0002;
  state.spr[SPR_DBAT4U] = 0x9000'1fff;
  state.spr[SPR_DBAT4L] = 0x1000'0002;
  HID4(state).SBE = 1;
  auto& mmu = system.GetMMU();
  mmu.DBATUpdated();
  mmu.IBATUpdated();
  state.msr.IR = 1;
  state.msr.DR = 1;

  ppc2wasm::StateLayout layout;
  layout.gpr_offset = static_cast<u32>(offsetof(PowerPC::PowerPCState, gpr));
  layout.memory_pages = static_cast<u32>(MemoryPages());
  layout.memory_shared = true;
  // Guest RAM's host address. The emitted code adds the masked guest offset
  // to this, so it has to be the same buffer the interpreter uses -- not a
  // copy, or the two engines would agree about registers and disagree about
  // memory.
  layout.ram_base = static_cast<u32>(reinterpret_cast<uintptr_t>(memory.GetRAM()));
  std::printf("  gpr offset in PowerPCState: %u\n", layout.gpr_offset);
  std::printf("  heap: %u pages   guest RAM at host 0x%08x\n\n", layout.memory_pages,
              layout.ram_base);

  const std::vector<Case> cases = {
      {"addi / add", {0x3860'0005, 0x3883'0007, 0x7CA3'2214}},
      {"addis and lis", {0x3C60'1234, 0x3880'0001, 0x7C83'2214}},
      {"ori / oris / xori", {0x3860'00FF, 0x6063'FF00, 0x6463'0F0F, 0x6863'00FF}},
      {"logical ops", {0x3860'00F0, 0x3880'000F, 0x7C65'2378, 0x7C66'2038, 0x7C67'2278}},
      {"shifts", {0x3860'0001, 0x3880'0004, 0x7C65'2030, 0x7C86'2430}},
      {"rlwinm rotate and mask", {0x3860'1234, 0x5464'103A, 0x5465'4000}},
      {"subf", {0x3860'0014, 0x3880'0005, 0x7CA3'2050}},
      {"r0 means literal zero in addi", {0x3860'0042, 0x3803'0001}},

      // ── memory ──
      // lis r3, 0x8000 ; ori r3, r3, 0x2000 ; addi r4, r0, 0x1234 ;
      // stw r4, 0(r3) ; lwz r5, 0(r3)
      {"stw then lwz round-trips a word",
       {0x3C60'8000, 0x6063'2000, 0x3880'1234, 0x9083'0000, 0x80A3'0000}},
      // Same, but read back into a different register so a store that did
      // nothing would be visible.
      {"lwz reads what stw wrote",
       {0x3C60'8000, 0x6063'2000, 0x3880'5678, 0x9083'0000, 0x80A3'0000}},
      // Byte order: store a word, read its first byte. On a big-endian guest
      // that byte is the MOST significant one, which is the whole reason the
      // emitted load reassembles from bytes.
      {"lbz sees the big-endian high byte",
       {0x3C60'8000, 0x6063'3000, 0x3C80'ABCD, 0x9083'0000, 0x88A3'0000}},
      {"stb writes a single byte",
       {0x3C60'8000, 0x6063'4000, 0x3880'0042, 0x9883'0000, 0x88A3'0000}},
      // Negative displacement, which is a signed field and a classic decode
      // slip.
      {"negative displacement",
       {0x3C60'8000, 0x6063'5000, 0x3880'0099, 0x9083'FFFC, 0x80A3'FFFC}},

      // ── the guard ──
      // MEM2 lives at 0x90000000 and a compiled block may not touch it, so
      // the block must refuse instruction 2 and hand back its PC.
      {"an address outside MEM1 bails to the interpreter",
       {0x3C60'9000, 0x3880'0001, 0x8083'0000}, 2},
  };

  for (const Case& c : cases)
  {
    // ── interpreter ──
    for (u32 i = 0; i < c.code.size(); ++i)
      memory.Write_U32(c.code[i], BASE + i * 4);
    for (int i = 0; i < 32; ++i)
      state.gpr[i] = 0;
    state.pc = state.npc = BASE;
    for (size_t i = 0; i < c.code.size(); ++i)
      ppc.SingleStep();

    u32 expect[32];
    std::memcpy(expect, state.gpr, sizeof(expect));
    const u32 expect_pc = state.pc;

    // ── recompiled ──
    auto r = ppc2wasm::CompileBlock(c.code.data(), BASE, static_cast<u32>(c.code.size()), layout);
    if (r.instructions != c.code.size())
    {
      std::printf("  %-58s %s\n", c.name, "FAIL (not all instructions compiled)");
      std::printf("      compiled %u of %zu, stopped on %s\n", r.instructions, c.code.size(),
                  r.stopped_on);
      ++g_failures;
      continue;
    }

    const int idx = InstallModule(r.module.data(), static_cast<int>(r.module.size()));
    if (idx < 0)
    {
      std::printf("  %-58s %s\n", c.name, "FAIL (module would not install)");
      ++g_failures;
      continue;
    }

    for (int i = 0; i < 32; ++i)
      state.gpr[i] = 0;
    state.pc = state.npc = BASE;

    BlockFn f = reinterpret_cast<BlockFn>(idx);
    const u32 got_pc = f(static_cast<u32>(reinterpret_cast<uintptr_t>(&state)));

    if (c.expect_bail_at >= 0)
    {
      const u32 want_pc = BASE + static_cast<u32>(c.expect_bail_at) * 4;
      const bool bailed = (got_pc == want_pc);
      char lbl[128];
      std::snprintf(lbl, sizeof(lbl), "%s (returned %08x)", c.name, got_pc);
      check(bailed, lbl);
      if (!bailed)
        std::printf("      wanted the block to stop at %08x\n", want_pc);
      continue;
    }

    bool same = (got_pc == expect_pc);
    int first_bad = -1;
    for (int i = 0; i < 32; ++i)
    {
      if (state.gpr[i] != expect[i])
      {
        same = false;
        if (first_bad < 0)
          first_bad = i;
      }
    }

    char label[128];
    std::snprintf(label, sizeof(label), "%s (%u instructions, %zu bytes)", c.name, r.instructions,
                  r.module.size());
    check(same, label);
    if (!same)
    {
      if (first_bad >= 0)
        std::printf("      r%-2d interpreter=%08x recompiled=%08x\n", first_bad, expect[first_bad],
                    state.gpr[first_bad]);
      if (got_pc != expect_pc)
        std::printf("      pc  interpreter=%08x recompiled=%08x\n", expect_pc, got_pc);
    }
  }

  std::printf("\n%s (%d failures)\n", g_failures ? "FAILED" : "PASSED", g_failures);
  std::fflush(stdout);
  return g_failures ? 1 : 0;
}
