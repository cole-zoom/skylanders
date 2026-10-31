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
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include "Common/CommonTypes.h"
#include "Common/Config/Config.h"
#include "Core/ConfigManager.h"
#include "Core/CoreTiming.h"
#include "Core/HW/SystemTimers.h"
#include "Core/Core.h"
#include "Core/HW/GPFifo.h"
#include "Core/HW/Memmap.h"
#include "Core/PowerPC/ConditionRegister.h"
#include "Core/PowerPC/Interpreter/Interpreter.h"
#include "Core/PowerPC/Interpreter/Interpreter_FPUtils.h"
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

// A condition-register field as its four PowerPC bits, LT GT EQ SO.
//
// Fields are compared through this, not as 64-bit words. Dolphin's
// encoding has many words for the same four bits -- the interpreter's
// compare takes s_crTable's, its CR0 update a sign-extended value, its
// JIT a raw difference -- and every reader (GetBit, GetField, the emitted
// branch tests, mfcr) decodes. Two engines agree exactly when the bits do.
static u32 CrBits(u64 field)
{
  PowerPC::ConditionRegister cr{};
  cr.fields[0] = field;
  return cr.GetField(0);
}

// The interpreter hatch compiled blocks import; the same function the
// dispatcher exports (bootgame.cpp), so what the differential runs is what
// the emulator runs. See StateLayout::interp_hatch.
static Interpreter* g_hatch_interp = nullptr;
static PowerPC::PowerPCManager* g_hatch_ppc = nullptr;
static PowerPC::PowerPCState* g_hatch_state = nullptr;

extern "C" EMSCRIPTEN_KEEPALIVE u32 ppc_interp_op(u32 /*state*/, u32 ins, u32 pc)
{
  auto& st = *g_hatch_state;
  st.pc = pc;
  st.npc = pc + 4;
  Interpreter::RunInterpreterOp(*g_hatch_interp, UGeckoInstruction{ins});
  if ((st.Exceptions & EXCEPTION_DSI) != 0)
  {
    g_hatch_ppc->CheckExceptions();
    st.pc = st.npc;
    return 1;
  }
  st.pc = st.npc;
  if (st.Exceptions != 0)
  {
    g_hatch_ppc->CheckExceptions();
    st.pc = st.npc;
  }
  return st.pc != pc + 4 ? 1u : 0u;
}

#include "slowpaths.h"

EM_JS(void, JitImportsInit, (), {
  Module.__jitImports = {
    env: {
      memory: wasmMemory,
      __indirect_function_table: wasmTable,
      ppc_interp_op: wasmExports['ppc_interp_op'],
      ppc_hatch_exit: wasmExports['ppc_hatch_exit'],
      ppc_mem_slow: wasmExports['ppc_mem_slow'],
    },
  };
});

// Install a module, optionally REPLACING an existing table slot.
//
// Growing a fresh slot per trial pins every WebAssembly.Module and Instance
// for the life of the process. At roughly nine thousand of them the run used
// to die partway through with "Worker terminated due to reaching memory
// limit", which reads as a harness crash rather than an out-of-memory and
// lands in the middle of a passing run. Reusing a slot lets the previous
// instance be collected -- but only if it did not import the TABLE. Node
// 22's V8 keeps every instance that imports a table alive from that table,
// with a structure sized to it: measured, 6,000 instances importing a
// 10,000-entry table hold 1.38 GB after gc(), against 11 MB with a
// 4-entry one. A block imports wasmTable when linking is on, so the
// per-instruction differential compiles without it (see there).
EM_JS(int, InstallModuleAt, (const uint8_t* ptr, int len, int slot), {
  try {
    const bytes = HEAPU8.slice(ptr, ptr + len);
    const mod = new WebAssembly.Module(bytes);
    const inst = new WebAssembly.Instance(mod, Module.__jitImports);
    let idx = slot;
    if (idx < 0) {
      idx = wasmTable.length;
      wasmTable.grow(1);
    }
    wasmTable.set(idx, inst.exports.f);
    // Every few hundred installs, collect the modules this slot has replaced
    // -- when the runner exposes gc(), which run-jitdiff.sh does.
    Module.__installs = (Module.__installs | 0) + 1;
    if (typeof gc === 'function' && (Module.__installs % 500) === 0) gc();
    return idx;
  } catch (e) {
    out('  install failed: ' + e);
    return -1;
  }
});

static int InstallModule(const uint8_t* ptr, int len)
{
  return InstallModuleAt(ptr, len, -1);
}

// Install a module holding several blocks, the way the dispatcher does.
//
// A chain differential that installs one module per block cannot test a
// direct call, because a direct call names a function in the SAME module.
// `reserve` is how many consecutive table slots to claim the first time;
// after that the same slots are overwritten, so the instances stay
// collectable.
EM_JS(int, InstallBatchAt, (const uint8_t* ptr, int len, int nfuncs, int slot, int reserve), {
  try {
    const bytes = HEAPU8.slice(ptr, ptr + len);
    const mod = new WebAssembly.Module(bytes);
    const inst = new WebAssembly.Instance(mod, Module.__jitImports);
    let base = slot;
    if (base < 0) { base = wasmTable.length; wasmTable.grow(reserve); }
    for (let i = 0; i < nfuncs; i++)
      wasmTable.set(base + i, inst.exports['f' + i]);
    Module.__installs = (Module.__installs | 0) + 1;
    if (typeof gc === 'function' && (Module.__installs % 500) === 0) gc();
    return base;
  } catch (e) {
    out('  batch install failed: ' + e);
    return -1;
  }
});

// The same, for a module whose exits to blocks installed earlier are bound
// as imports (ModuleBuilder::ImportBlock): `imports` names `nimports` table
// slots, bound as env.b0, env.b1, ... in order, as the dispatcher's
// JitInstallBatch does. The slot must be given.
EM_JS(int, InstallBatchImportsAt,
      (const uint8_t* ptr, int len, int nfuncs, int slot, const int* imports, int nimports), {
  try {
    const bytes = HEAPU8.slice(ptr, ptr + len);
    const mod = new WebAssembly.Module(bytes);
    const env = Object.assign({}, Module.__jitImports.env);
    for (let k = 0; k < nimports; k++)
      env['b' + k] = wasmTable.get(HEAP32[(imports >> 2) + k]);
    const inst = new WebAssembly.Instance(mod, { env });
    for (let i = 0; i < nfuncs; i++)
      wasmTable.set(slot + i, inst.exports['f' + i]);
    Module.__installs = (Module.__installs | 0) + 1;
    if (typeof gc === 'function' && (Module.__installs % 500) === 0) gc();
    return slot;
  } catch (e) {
    out('  batch install with imports failed: ' + e);
    return -1;
  }
});

EM_JS(int, MemoryPages, (), { return wasmMemory.buffer.byteLength >>> 16; });

using BlockFn = u32 (*)(u32);

// Run a compiled block and apply the same fix-up the dispatcher would.
//
// A block that writes MSR stops and marks its return with bit 1. Everything
// the interpreter does after that write -- recomputing translation state and
// taking any interrupt that became deliverable -- happens outside the block,
// so a differential that skips it is comparing against a different machine.
static u32 RunBlock(int index, PowerPC::PowerPCState& state, PowerPC::PowerPCManager& ppc)
{
  const u32 ret = reinterpret_cast<BlockFn>(index)(
      static_cast<u32>(reinterpret_cast<uintptr_t>(&state)));
  // The PC is published BEFORE the exception path runs, because that is
  // what the dispatcher does (bootgame.cpp) and because CheckExceptions
  // saves SRR0 from state.pc. Leaving the stale PC here meant the
  // differentials were validating against a fix-up sequence the emulator
  // does not use.
  state.pc = state.npc = ret & ~3u;
  if (ret & 2u)
  {
    // What Interpreter::mtmsr does after the write, in the same order:
    // an FP exception already summarised in FPSCR[FEX] is taken the
    // moment FE0 or FE1 enables it.
    ppc.MSRUpdated();
    CheckFPExceptions(state);
    ppc.CheckExceptions();
    state.pc = state.npc;
  }
  return ret;
}

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

int main(int argc, char** argv)
{
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  // --compact: the slow paths as helper calls (StateLayout::hatch_exit_index).
  // --short-convert: float conversions through promote/demote. Both are
  // meant to be exact, so every expectation below holds with them on.
  // --simd: paired singles as vectors (StateLayout::simd_ps); here, with
  // the arithmetic exact, that is the quantized loads and stores.
  // --warm: the warm entry (StateLayout::warm_entry), as the dispatcher
  // has it with --jit-warm: every block carries the negative-state test,
  // and the hand-written cases also call each block with a negative state
  // and expect nothing to have run.
  // --direct: addresses translated arithmetically instead of through the
  // region table (StateLayout::direct_base). --groups: accesses through
  // one base register share a translation (StateLayout::mem_groups). Both
  // exact, so every expectation holds with them on.
  bool want_compact = false, want_short_convert = false, want_simd = false, want_warm = false;
  bool want_direct = false, want_groups = false, want_hints = false, want_regs = false;
  for (int i = 1; i < argc; ++i)
  {
    if (std::strcmp(argv[i], "--hints") == 0)
      want_hints = true;  // StateLayout::branch_hints: layout only, so every expectation holds
    else if (std::strcmp(argv[i], "--regs") == 0)
      want_regs = true;  // StateLayout::reg_cache: registers in locals, write-through
    if (std::strcmp(argv[i], "--compact") == 0)
      want_compact = true;
    else if (std::strcmp(argv[i], "--short-convert") == 0)
      want_short_convert = true;
    else if (std::strcmp(argv[i], "--simd") == 0)
      want_simd = true;
    else if (std::strcmp(argv[i], "--warm") == 0)
      want_warm = true;
    else if (std::strcmp(argv[i], "--direct") == 0)
      want_direct = true;
    else if (std::strcmp(argv[i], "--groups") == 0)
      want_groups = true;
  }
  std::printf("target: wasm32  recompiled blocks vs the interpreter%s%s%s%s%s%s%s%s\n\n",
              want_compact ? "  [compact]" : "", want_short_convert ? "  [short-convert]" : "",
              want_simd ? "  [simd]" : "", want_warm ? "  [warm]" : "",
              want_direct ? "  [direct]" : "", want_groups ? "  [groups]" : "",
              want_hints ? "  [hints]" : "", want_regs ? "  [regs]" : "");

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
  // The uncached mirrors, DBAT1 and DBAT5 in CBoot::SetupBAT. Compiled code
  // reaches them through the region table; without these the interpreter
  // drops every access to them and the two engines are comparing different
  // machines.
  state.spr[SPR_DBAT1U] = 0xc000'1fff;
  state.spr[SPR_DBAT1L] = 0x0000'002a;
  // The locked L1 cache at 0xE0000000, 256 KB, as a game's OS init maps it.
  state.spr[SPR_DBAT2U] = 0xe000'0007;
  state.spr[SPR_DBAT2L] = 0xe000'0002;
  state.spr[SPR_DBAT5U] = 0xd000'1fff;
  state.spr[SPR_DBAT5L] = 0x1000'002a;
  HID4(state).SBE = 1;
  auto& mmu = system.GetMMU();
  mmu.DBATUpdated();
  mmu.IBATUpdated();
  state.msr.IR = 1;
  state.msr.DR = 1;
  // Floating point on, and paired singles enabled through HID2[PSE], as
  // the boot leaves them; the FP forms raise exceptions otherwise, and
  // both engines agreeing on that would test nothing.
  state.msr.FP = 1;
  state.spr[SPR_HID2] |= (1u << 29) | (1u << 31);  // PSE, LSQE
  state.spr[SPR_HID2] |= 1u << 28;                 // LCE, for dcbz_l
  // GQR0 is plain floats, as the ABI requires; GQR1 to GQR4 cover the four
  // integer types with scales positive, zero and negative (a six-bit
  // scale of 60 or 61 is -4 or -3). psq_l/psq_st draw their GQR from
  // these five.
  state.spr[SPR_GQR0 + 1] = (7u << 16) | (2u << 24) | 7u | (2u << 8);    // s16, ld 2, st 2
  state.spr[SPR_GQR0 + 2] = (4u << 16) | 4u;                             // u8, scales 0
  state.spr[SPR_GQR0 + 3] = (5u << 16) | (5u << 24) | 5u | (61u << 8);   // u16, ld 5, st -3
  state.spr[SPR_GQR0 + 4] = (6u << 16) | (60u << 24) | 6u | (7u << 8);   // s8, ld -4, st 7
  // The machine state every differential below restores from. Captured here,
  // once, because an exception taken inside any trial masks MSR down.
  const u32 kPristineMsr = state.msr.Hex;

  // An empty link table: every lookup misses, so a linked block behaves like
  // an unlinked one and the same expectations hold. What this checks is that
  // the epilogue itself -- downcount handling, the wrapping block, branches
  // exiting via br rather than return -- does not change what a block
  // computes.
  std::vector<u32> link_table(2 * 4096, 0);

  ppc2wasm::StateLayout layout;
  layout.gpr_offset = static_cast<u32>(offsetof(PowerPC::PowerPCState, gpr));
  layout.memory_pages = static_cast<u32>(MemoryPages());
  layout.memory_shared = true;
  // Guest RAM's host address. The emitted code adds the masked guest offset
  // to this, so it has to be the same buffer the interpreter uses -- not a
  // copy, or the two engines would agree about registers and disagree about
  // memory.
  layout.ram_base = static_cast<u32>(reinterpret_cast<uintptr_t>(memory.GetRAM()));
  layout.cr_offset = static_cast<u32>(offsetof(PowerPC::PowerPCState, cr));
  layout.xer_so_ov_offset = static_cast<u32>(offsetof(PowerPC::PowerPCState, xer_so_ov));
  // The interpreter's own table, by address. Recomputing the encoding here
  // would be a second source of truth for something that must match exactly.
  layout.spr_offset = static_cast<u32>(offsetof(PowerPC::PowerPCState, spr));
  layout.msr_offset = static_cast<u32>(offsetof(PowerPC::PowerPCState, msr));
  layout.downcount_offset = static_cast<u32>(offsetof(PowerPC::PowerPCState, downcount));
  layout.link_base = static_cast<u32>(reinterpret_cast<uintptr_t>(link_table.data()));
  layout.link_mask = 4095;
  layout.enable_linking = true;
  layout.cr_table_base =
      static_cast<u32>(reinterpret_cast<uintptr_t>(PowerPC::ConditionRegister::s_crTable.data()));
  layout.xer_ca_offset = static_cast<u32>(offsetof(PowerPC::PowerPCState, xer_ca));
  layout.xer_stringctrl_offset = static_cast<u32>(offsetof(PowerPC::PowerPCState, xer_stringctrl));
  layout.ps_offset = static_cast<u32>(offsetof(PowerPC::PowerPCState, ps));
  layout.fpscr_offset = static_cast<u32>(offsetof(PowerPC::PowerPCState, fpscr));
  layout.reserve_offset = static_cast<u32>(offsetof(PowerPC::PowerPCState, reserve));
  layout.reserve_address_offset = static_cast<u32>(offsetof(PowerPC::PowerPCState, reserve_address));
  layout.interp_hatch = true;
  layout.pc_offset = static_cast<u32>(offsetof(PowerPC::PowerPCState, pc));
  // The paired multiply-adds inline (EmitPsMadd), as the dispatcher has
  // them unless --jit-no-madd.
  layout.paired_madd = true;
  // Compares as the dispatcher has them unless --jit-no-crdiff: the
  // operands' difference, not s_crTable (EmitCompareToCR).
  layout.cr_diff = true;
  if (want_compact)
  {
    layout.hatch_exit_index = 1;
    layout.mem_slow_index = 2;
    layout.exit_value_addr = static_cast<u32>(reinterpret_cast<uintptr_t>(&g_jit_exit_value));
  }
  layout.short_convert = want_short_convert;
  layout.simd_ps = want_simd;
  layout.warm_entry = want_warm;
  // The GQRs above, specialised on as the dispatcher does; the guard the
  // emitted code carries is exercised by the mtspr-to-GQR case below.
  layout.gqr_values = &state.spr[SPR_GQR0];
  // The locked cache, mapped by the DBAT above; the dispatcher checks the
  // same thing before enabling it.
  if (memory.GetL1Cache() && mmu.IsOptimizableRAMAddress(0xE0000000u, 32))
    layout.l1_cache_base = static_cast<u32>(reinterpret_cast<uintptr_t>(memory.GetL1Cache()));
  g_hatch_interp = &system.GetInterpreter();
  g_hatch_ppc = &ppc;
  g_hatch_state = &state;
  JitImportsInit();
  {
    auto& g = system.GetCoreTiming().GetGlobals();
    layout.tb_global_timer_addr = static_cast<u32>(reinterpret_cast<uintptr_t>(&g.global_timer));
    layout.tb_slice_length_addr = static_cast<u32>(reinterpret_cast<uintptr_t>(&g.slice_length));
    layout.tb_start_value_addr = static_cast<u32>(reinterpret_cast<uintptr_t>(&g.fake_TB_start_value));
    layout.tb_start_ticks_addr = static_cast<u32>(reinterpret_cast<uintptr_t>(&g.fake_TB_start_ticks));
  }
  layout.dcache_enabled = state.m_enable_dcache;
  // The region table, as the dispatcher builds it: MEM1, MEM2 and their
  // uncached mirrors. The block differential points some bases into MEM2 so
  // the second table entry is exercised and not just present.
  std::vector<u32> region_table(128, 0);
  {
    const u32 ram = static_cast<u32>(reinterpret_cast<uintptr_t>(memory.GetRAM()));
    const u32 exram = static_cast<u32>(reinterpret_cast<uintptr_t>(memory.GetEXRAM()));
    region_table[0x80000000u >> 25] = ram;
    region_table[0xC0000000u >> 25] = ram;
    region_table[0x90000000u >> 25] = exram;
    region_table[0x92000000u >> 25] = exram + 0x02000000u;
    region_table[0xD0000000u >> 25] = exram;
    region_table[0xD2000000u >> 25] = exram + 0x02000000u;
  }
  layout.region_table = static_cast<u32>(reinterpret_cast<uintptr_t>(region_table.data()));
  if (want_direct)
  {
    layout.direct_base = static_cast<u32>(reinterpret_cast<uintptr_t>(memory.GetRAM()));
    layout.direct_mem2_delta =
        static_cast<u32>(reinterpret_cast<uintptr_t>(memory.GetEXRAM())) - layout.direct_base;
    layout.direct_mask = 0x301;
  }
  layout.mem_groups = want_groups;
  layout.branch_hints = want_hints;
  layout.reg_cache = want_regs;
  // dcbz needs HID0[DCE]; the boot sets it and so does this.
  state.spr[SPR_HID0] |= 1u << 14;
  // Floating-point registers: seeded with a mix of ordinary doubles, bit
  // patterns of singles widened the way lfs widens them, and the specials
  // the conversion routines branch on -- zero, denormals, infinities and
  // both kinds of NaN -- because those branches are where a bit-exact
  // reimplementation goes wrong.
  auto seed_ps = [&](auto& rnd) {
    static const u64 kSpecials[] = {
        0x0000000000000000ull, 0x8000000000000000ull,  // +0, -0
        0x7FF0000000000000ull, 0xFFF0000000000000ull,  // +inf, -inf
        0x7FF8000000000000ull, 0x7FF4000000000000ull,  // quiet NaN, signalling NaN
        0x0008000000000000ull, 0x0000000000000001ull,  // double denormals
        0x3810000000000000ull, 0x380FFFFFFFFFFFFFull,  // smallest normal single, just below
        0x36A0000000000000ull, 0x3800000000000000ull,  // in the denormal-single range
        0x3FF0000000000000ull, 0x4000000000000000ull,  // 1.0, 2.0
        0x47EFFFFFE0000000ull, 0xC7EFFFFFE0000000ull,  // max single, as a double
    };
    for (int i = 0; i < 32; ++i)
    {
      const u32 pick = rnd() % 4;
      u64 v;
      if (pick == 0)
        v = kSpecials[rnd() % (sizeof(kSpecials) / sizeof(kSpecials[0]))];
      else if (pick == 1)
        v = (static_cast<u64>(rnd()) << 32) | rnd();
      else if (pick == 2)
        v = std::bit_cast<u64>(static_cast<double>(static_cast<s32>(rnd())) / 1024.0);
      else
      {
        // A single widened as lfs widens it -- what a paired-single
        // computation holds, and the operand EmitPsMadd's fast path needs.
        // Half the time any float pattern, half the time a modest exponent
        // so that arithmetic between two of them stays finite.
        u32 f = rnd();
        if (rnd() & 1)
          f = (f & 0x807FFFFFu) | ((107u + rnd() % 41) << 23);
        v = std::bit_cast<u64>(static_cast<double>(std::bit_cast<float>(f)));
      }
      state.ps[i].SetPS0(v);
      state.ps[i].SetPS1((rnd() & 1) ? v : ((static_cast<u64>(rnd()) << 32) | rnd()));
    }
  };
  // A random register value used as an address must not land in
  // 0xC8000000-0xCFFFFFFF: the BAT maps it onto the EFB and hardware
  // register space, and a store the interpreter routes there with an
  // address no device claims indexes MMIO::Mapping's handler table past
  // its end (no bounds check in a release build) -- a wasm trap, not a
  // result. No game stores at random addresses there; the generator must
  // not either. Before the hatch this never ran, because such an access
  // bailed and the oracle was only run up to the bail.
  auto safe_random = [](u32 v) {
    return ((v & 0xF8000000u) == 0xC8000000u) ? (v ^ 0x40000000u) : v;
  };
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

      // ── compares ──
      // cmpwi r3, 5 with r3 held at 1, 5 and 9 covers LT, EQ and GT, which
      // are three different entries in the encoding table.
      {"cmpwi less than", {0x3860'0001, 0x2C03'0005}},
      {"cmpwi equal", {0x3860'0005, 0x2C03'0005}},
      {"cmpwi greater than", {0x3860'0009, 0x2C03'0005}},
      // A negative value: signed and unsigned compares must disagree, and
      // getting that backwards is the classic compare bug.
      {"cmpwi signed sees a negative as less", {0x3860'FFFF, 0x2C03'0001}},
      {"cmplwi unsigned sees it as greater", {0x3860'FFFF, 0x2803'0001}},
      // Register form, into a condition field other than 0.
      {"cmpw into cr3", {0x3860'0007, 0x3880'0007, 0x7D83'2000}},
      {"cmplw register unsigned", {0x3860'FFFF, 0x3880'0002, 0x7C03'2040}},

      // ── branches ──
      // The block must return the TARGET, not the fall-through, so the
      // interpreter does not have to re-execute the branch.
      {"unconditional branch forward", {0x3860'0001, 0x4800'0010}},
      {"unconditional branch backward", {0x3860'0001, 0x4BFF'FFFC}},
      // bl must also leave the return address in the link register.
      {"bl sets the link register", {0x3860'0001, 0x4800'0011}},
      // beq cr0 taken: compare equal, then branch if EQ.
      {"beq taken", {0x3860'0005, 0x2C03'0005, 0x4182'0008}},
      // beq not taken: the block falls through to the next PC.
      {"beq not taken", {0x3860'0009, 0x2C03'0005, 0x4182'0008}},
      // bne is the same instruction testing the bit as FALSE.
      {"bne taken", {0x3860'0009, 0x2C03'0005, 0x4082'0008}},
      {"bne not taken", {0x3860'0005, 0x2C03'0005, 0x4082'0008}},
      // blt / bgt exercise the other two condition bits, which are extracted
      // from the 64-bit encoding differently from EQ.
      {"blt taken", {0x3860'0001, 0x2C03'0005, 0x4180'0008}},
      {"bgt taken", {0x3860'0009, 0x2C03'0005, 0x4181'0008}},
      {"bgt not taken when equal", {0x3860'0005, 0x2C03'0005, 0x4181'0008}},

      // ── returns, stack frames and SPR moves ──
      // mtspr LR then blr: the canonical function return.
      {"mtspr LR then blr returns to it",
       {0x3C60'8000, 0x6063'2000, 0x7C68'03A6, 0x4E80'0020}},
      // mfspr reads it back into a register.
      {"mfspr reads the link register",
       {0x3C60'8000, 0x7C68'03A6, 0x7C88'02A6}},
      // CTR is the other register this allows.
      {"mtspr / mfspr on CTR", {0x3860'1234, 0x7C69'03A6, 0x7C89'02A6}},
      // stwu is how a stack frame is pushed: store, then update the base.
      {"stwu stores and updates the base",
       {0x3C60'8000, 0x6063'6000, 0x3880'4321, 0x9483'FFF0}},
      // A conditional return, which must read LR before writing it.
      {"beqlr taken", {0x3C60'8000, 0x6063'2000, 0x7C68'03A6, 0x3880'0005,
                       0x2C04'0005, 0x4D82'0020}},
      {"beqlr not taken", {0x3C60'8000, 0x6063'2000, 0x7C68'03A6, 0x3880'0009,
                           0x2C04'0005, 0x4D82'0020}},

      // ── the long tail ──
      // Sign extension: 0x80 as a byte is negative, which a zero-extending
      // implementation gets wrong only for values with the high bit set.
      {"extsb sign-extends a negative byte", {0x3860'0080, 0x7C64'0774}},
      {"extsb on a positive byte", {0x3860'007F, 0x7C64'0774}},
      {"extsh sign-extends a halfword", {0x3C60'8000, 0x7C64'0734}},
      {"neg", {0x3860'0005, 0x7C83'00D0}},
      {"cntlzw counts leading zeros", {0x3860'0001, 0x7C64'0034}},
      {"cntlzw of zero is 32", {0x3860'0000, 0x7C64'0034}},
      // The complement forms: each gets the NOT in a different place.
      {"andc", {0x3860'00FF, 0x3880'000F, 0x7C65'2078}},
      {"orc", {0x3860'00F0, 0x3880'000F, 0x7C65'2338}},
      {"nor", {0x3860'00F0, 0x3880'000F, 0x7C65'20F8}},
      {"nand", {0x3860'00FF, 0x3880'0F0F, 0x7C65'23B8}},
      {"eqv", {0x3860'00FF, 0x3880'0F0F, 0x7C65'2238}},
      {"mullw", {0x3860'0007, 0x3880'0006, 0x7CA3'21D6}},
      {"mulli", {0x3860'0007, 0x1C83'0005}},
      // Halfword memory, where byte order matters most.
      {"sth then lhz round-trips a halfword",
       {0x3C60'8000, 0x6063'7000, 0x3880'ABCD, 0xB083'0000, 0xA0A3'0000}},
      {"lha sign-extends what sth wrote",
       {0x3C60'8000, 0x6063'7100, 0x3880'8001, 0xB083'0000, 0xA8A3'0000}},
      // Indexed addressing.
      {"stwx then lwzx",
       {0x3C60'8000, 0x6063'7200, 0x3880'0010, 0x38A0'1234, 0x7CA3'212E, 0x7CC3'202E}},
      // lwbrx is not translated: it runs in the interpreter through the
      // hatch and writes r6 there. The addi after it must see what the
      // interpreter wrote, not what r6 held when the block was entered --
      // the case any caching of guest registers in the block has to get
      // right, and which nothing here exercised before.
      {"a hatched lwbrx writes a register the next instruction reads",
       {0x3C60'8000, 0x6063'7200, 0x3880'0010, 0x38A0'1234, 0x7CA3'212E, 0x7CC3'242C,
        0x38E6'0001}},
      // Update forms write the address back.
      {"lwzu updates the base",
       {0x3C60'8000, 0x6063'7300, 0x3880'4321, 0x9083'0004, 0x84A3'0004}},
      {"stbu updates the base",
       {0x3C60'8000, 0x6063'7400, 0x3880'0055, 0x9C83'0008}},

      // A conditional branch-and-link that is NOT taken must leave the link
      // register alone. Dolphin writes LR inside the taken path; writing it
      // unconditionally is invisible to every existing case here, because
      // none of them sets LK on a conditional branch.
      // lis r3,0x8000 ; ori r3,r3,0x2000 ; mtspr LR,r3 ; addi r4,r0,9 ;
      // cmpwi r4,5 ; beql (not taken)
      {"beql not taken leaves LR alone",
       {0x3C60'8000, 0x6063'2000, 0x7C68'03A6, 0x3880'0009, 0x2C04'0005, 0x4182'0009}},
      {"beql taken sets LR", {0x3860'0005, 0x2C03'0005, 0x4182'0009}},
      // The same for the return form: bnelr+ with link, not taken.
      {"bnelrl not taken leaves LR alone",
       {0x3C60'8000, 0x6063'2000, 0x7C68'03A6, 0x3880'0005, 0x2C04'0005, 0x4C82'0021}},

      // ── CTR-decrementing branches ──
      // bdnz is the back edge of every counted loop. BO=16 decrements CTR
      // and branches while it is non-zero; BO=18 branches when it reaches
      // zero. The decrement happens whether or not the branch is taken,
      // which is the part that is easy to get wrong and impossible to see
      // from the PC alone.
      // li r3,3 ; mtspr CTR,r3 ; bdnz +8   -> CTR 2, taken
      {"bdnz decrements CTR and branches while non-zero",
       {0x3860'0003, 0x7C69'03A6, 0x4200'0008}},
      // li r3,1 ; mtspr CTR,r3 ; bdnz +8   -> CTR 0, falls through
      {"bdnz falls through when the counter hits zero",
       {0x3860'0001, 0x7C69'03A6, 0x4200'0008}},
      // bdz is the same decrement with the opposite test.
      {"bdz branches when the counter reaches zero",
       {0x3860'0001, 0x7C69'03A6, 0x4240'0008}},
      {"bdz falls through while the counter is non-zero",
       {0x3860'0005, 0x7C69'03A6, 0x4240'0008}},
      // The decrement wraps rather than saturating, and zero is the case a
      // saturating implementation would get wrong.
      {"bdnz from zero wraps to 0xffffffff and branches",
       {0x3860'0000, 0x7C69'03A6, 0x4200'0008}},
      // bdnzf/bdnzt also consult a condition bit: both tests must pass.
      // li r3,3 ; mtspr CTR,r3 ; li r4,5 ; cmpwi r4,5 ; bdnzt eq,+8
      {"bdnzt needs both the counter and the condition",
       {0x3860'0003, 0x7C69'03A6, 0x3880'0005, 0x2C04'0005, 0x4160'0008}},
      {"bdnzt not taken when the condition fails, but still decrements",
       {0x3860'0003, 0x7C69'03A6, 0x3880'0009, 0x2C04'0005, 0x4160'0008}},
      // bdnzlr: the counted form of a return.
      {"bdnzlr decrements and returns through LR",
       {0x3C60'8000, 0x6063'2000, 0x7C68'03A6, 0x3880'0003, 0x7C89'03A6, 0x4E00'0020}},
      {"bdnzlr falls through when the counter hits zero",
       {0x3C60'8000, 0x6063'2000, 0x7C68'03A6, 0x3880'0001, 0x7C89'03A6, 0x4E00'0020}},

      // bcctr is the indirect branch: mtspr CTR then jump to it.
      {"bcctr branches through CTR",
       {0x3C60'8000, 0x6063'2000, 0x7C69'03A6, 0x4E80'0420}},

      // ── the guard ──
      // The hardware registers live at 0xCC000000 and a compiled block may
      // not touch them, so the block must refuse instruction 2 and hand back
      // its PC. (This used to use MEM2, which compiled code now reaches
      // through the region table.)
      {"an address outside RAM bails to the interpreter",
       {0x3C60'CC00, 0x3880'0001, 0x8083'0000}, 2},

      // ── access groups (StateLayout::mem_groups) ──
      // A function prologue and epilogue: seven accesses through r1, with
      // the update form's and the addi's constants folded into the group.
      // lis r1,0x8000 ; ori r1,r1,0x7800 ; li r4,0x1111 ; li r5,0x2222 ;
      // stwu r1,-0x40(r1) ; stw r4,0x3c(r1) ; stw r5,0x38(r1) ;
      // lwz r6,0x3c(r1) ; lwz r7,0x38(r1) ; lwz r8,0(r1) ; addi r1,r1,0x40 ;
      // lwz r9,-0x40(r1)
      {"a stack frame's stores and loads share one translation",
       {0x3C20'8000, 0x6021'7800, 0x3880'1111, 0x38A0'2222, 0x9421'FFC0, 0x9081'003C,
        0x90A1'0038, 0x80C1'003C, 0x80E1'0038, 0x8101'0000, 0x3821'0040, 0x8121'FFC0}},
      // A load into the base register ends the group: the next access must
      // translate the NEW pointer, not add to the old one.
      // lis r3,0x8000 ; ori r3,r3,0x7000 ; addi r4,r3,0x100 ; stw r4,0(r3) ;
      // li r5,0x77 ; stw r5,0x100(r3) ; lwz r3,0(r3) ; lwz r6,0(r3)
      {"a load into the base register ends its group",
       {0x3C60'8000, 0x6063'7000, 0x3883'0100, 0x9083'0000, 0x38A0'0077, 0x90A3'0100,
        0x8063'0000, 0x80C3'0000}},
      // A group whose span crosses out of MEM1's granule: the leader must
      // not vouch for it, and the member past the edge bails as it always
      // did. (The interpreter reads a mirror there; the block refuses.)
      // lis r3,0x8200 ; addi r3,r3,-8 ; li r4,0x11 ; li r5,0x22 ;
      // stw r4,0(r3) ; stw r5,4(r3) ; lwz r6,8(r3)
      {"a group spanning a granule edge falls back and bails",
       {0x3C60'8200, 0x3863'FFF8, 0x3880'0011, 0x38A0'0022, 0x9083'0000, 0x90A3'0004,
        0x80C3'0008}, 6},
      // Float members: a word stored, loaded as a single, stored back.
      // lis r3,0x8000 ; ori r3,r3,0x7600 ; lis r4,0x3f80 ; stw r4,0(r3) ;
      // lfs f1,0(r3) ; stfs f1,4(r3) ; lwz r5,4(r3)
      {"float loads and stores join an integer group",
       {0x3C60'8000, 0x6063'7600, 0x3C80'3F80, 0x9083'0000, 0xC023'0000, 0xD023'0004,
        0x80A3'0004}},
      // A misaligned float member still takes the alignment exception.
      // lis r3,0x8000 ; ori r3,r3,0x7600 ; stw r4,0(r3) ; lfs f1,2(r3)
      {"a misaligned float member bails",
       {0x3C60'8000, 0x6063'7600, 0x9083'0000, 0xC023'0002}, 3},

      // ── the condition register's own ops ──
      // li r3,5 ; cmpwi r3,5 (cr0: EQ) ; li r4,9 ; cmpwi cr1,r4,5 (cr1: GT) ;
      // cror 8,2,5 ; crxor 9,2,5 ; crandc 10,2,5 ; crnor 11,2,6 ; crand 12,2,5 ;
      // crnand 13,2,5 ; creqv 14,2,6 ; crorc 15,6,5 ; mcrf cr4,cr2 ; mfcr r5
      {"the CR logical ops and mcrf",
       {0x3860'0005, 0x2C03'0005, 0x3880'0009, 0x2C84'0005, 0x4D02'2B82, 0x4D22'2982,
        0x4D42'2902, 0x4D62'3042, 0x4D82'2A02, 0x4DA2'29C2, 0x4DC2'3242, 0x4DE6'2B42,
        0x4E08'0000, 0x7CA0'0026}},
      // andi. and andis. set CR0 whatever their low bit says.
      // li r3,0xff0 ; andi. r4,r3,0xf0 ; andis. r5,r3,0x8000
      {"andi. and andis. set CR0", {0x3860'0FF0, 0x7064'00F0, 0x7465'8000}},
      // dcbz_l zeroes a line of the locked cache (HID2[LCE] is set below).
      // lis r3,0xe000 ; ori r3,r3,0x1000 ; li r4,0x1234 ; stw r4,0(r3) ;
      // stw r4,28(r3) ; dcbz_l 0,r3 ; lwz r5,0(r3) ; lwz r6,28(r3)
      {"dcbz_l zeroes a locked-cache line",
       {0x3C60'E000, 0x6063'1000, 0x3880'1234, 0x9083'0000, 0x9083'001C, 0x1000'1FEC,
        0x80A3'0000, 0x80C3'001C}},
  };

  for (const Case& c : cases)
  {
    // ── interpreter ──
    for (u32 i = 0; i < c.code.size(); ++i)
      memory.Write_U32(c.code[i], BASE + i * 4);
    // Both engines must start from the SAME state, condition register
    // included. Resetting it only before the recompiled run left stale fields
    // from earlier cases in the interpreter's starting state, and the
    // comparison then failed on a field neither program had touched.
    for (int i = 0; i < 32; ++i)
      state.gpr[i] = 0;
    for (int i = 0; i < 8; ++i)
      state.cr.fields[i] = 0;
    state.spr[SPR_LR] = 0;
    state.msr.Hex = kPristineMsr;
    state.pc = state.npc = BASE;
    // Up to an expected bail only: the instruction the block refuses may
    // be one the interpreter takes an exception on (a misaligned float
    // load, an address no BAT maps), and an exception masks MSR down for
    // every case after this one.
    for (size_t i = 0; i < c.code.size(); ++i)
    {
      if (c.expect_bail_at >= 0 && i >= static_cast<size_t>(c.expect_bail_at))
        break;
      ppc.SingleStep();
    }

    u32 expect[32];
    std::memcpy(expect, state.gpr, sizeof(expect));
    u64 expect_cr[8];
    std::memcpy(expect_cr, state.cr.fields, sizeof(expect_cr));
    const u32 expect_lr = state.spr[SPR_LR];
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
    for (int i = 0; i < 8; ++i)
      state.cr.fields[i] = 0;
    state.spr[SPR_LR] = 0;
    state.downcount = 1000;
    state.pc = state.npc = BASE;

    BlockFn f = reinterpret_cast<BlockFn>(idx);
    if (want_warm)
    {
      // A negative state runs nothing: three passes of the warm loop, 0
      // back, and the guest as it was.
      const u32 r0 = f(static_cast<u32>(-3));
      bool untouched = r0 == 0 && state.pc == BASE && state.downcount == 1000;
      for (int i = 0; i < 32 && untouched; ++i)
        untouched = state.gpr[i] == 0;
      if (!untouched)
      {
        char lbl[128];
        std::snprintf(lbl, sizeof(lbl), "%s: warm call ran something (returned %08x)",
                      c.name, r0);
        check(false, lbl);
        continue;
      }
    }
    const u32 got_pc = RunBlock(idx, state, ppc);

    if (c.expect_bail_at >= 0)
    {
      // A bail is marked by the low bit, which a PC never has.
      const u32 want_pc = (BASE + static_cast<u32>(c.expect_bail_at) * 4) | 1u;
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
    const bool lr_same = (state.spr[SPR_LR] == expect_lr);
    if (!lr_same)
      same = false;
    int bad_cr = -1;
    for (int i = 0; i < 8; ++i)
    {
      if (CrBits(state.cr.fields[i]) != CrBits(expect_cr[i]))
      {
        same = false;
        if (bad_cr < 0)
          bad_cr = i;
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
      if (bad_cr >= 0)
        std::printf("      cr%d interpreter=%016llx recompiled=%016llx\n", bad_cr,
                    static_cast<unsigned long long>(expect_cr[bad_cr]),
                    static_cast<unsigned long long>(state.cr.fields[bad_cr]));
      if (!lr_same)
        std::printf("      lr  interpreter=%08x recompiled=%08x\n", expect_lr, state.spr[SPR_LR]);
      if (got_pc != expect_pc)
        std::printf("      pc  interpreter=%08x recompiled=%08x\n", expect_pc, got_pc);
    }
  }

  // ── mftb ──
  //
  // Not comparable against the interpreter the way everything else is: the
  // interpreter's SingleStep advances the clock, so the two engines would
  // read different times. Checked against the formula instead, computed
  // from the same globals the block reads, at the downcount the block had
  // when it ran the instruction.
  {
    const u32 prog[2] = {0x7C6C42E6, 0x7C8D42E6};  // mftb r3 ; mftbu r4
    auto r = ppc2wasm::CompileBlock(prog, BASE, 2, layout);
    bool ok = r.instructions == 2;
    if (ok)
    {
      const int idx = InstallModule(r.module.data(), static_cast<int>(r.module.size()));
      ok = idx >= 0;
      if (ok)
      {
        auto& g = system.GetCoreTiming().GetGlobals();
        state.downcount = 1000;
        state.pc = state.npc = BASE;
        const u64 ticks = static_cast<u64>(g.global_timer) + static_cast<u64>(g.slice_length - 1000);
        const u64 want = g.fake_TB_start_value + (ticks - g.fake_TB_start_ticks) / 12;
        RunBlock(idx, state, ppc);
        ok = state.gpr[3] == static_cast<u32>(want) && state.gpr[4] == static_cast<u32>(want >> 32) &&
             state.spr[SPR_TL] == static_cast<u32>(want) && state.spr[SPR_TU] == static_cast<u32>(want >> 32);
        if (!ok)
          std::printf("      want %016llx got %08x:%08x\n", static_cast<unsigned long long>(want),
                      state.gpr[4], state.gpr[3]);
      }
    }
    check(ok, "mftb / mftbu read the time base CoreTiming would report");
  }

  // From here on an access outside RAM runs through the hatch rather than
  // bailing, as the dispatcher has it. The hand-written bail case above
  // needed the bail; the chain differential below turns this off again to
  // keep that path exercised.
  layout.hatch_mmio = true;

  // ── the write-gather pipe ──
  //
  // A store to the pipe's page appends to a host buffer; compiled code
  // writes the buffer directly while the store leaves room, and the one
  // that would fill it takes the hatch, where the interpreter flushes.
  // Only the inline path is run here: a flush reaches the command
  // processor and the video FIFO, which this harness never initialises.
  // The block is compiled WITHOUT the MMIO hatch, so a store that missed
  // the pipe path would bail and the comparison would see the stores
  // after it missing. Checked from an empty buffer and from one filled
  // to the last byte the stores fit in.
  {
    system.GetGPFifo().Init();
    layout.gather_pipe_ea = 0xCC008000u;
    layout.gather_pipe_ptr_offset =
        static_cast<u32>(offsetof(PowerPC::PowerPCState, gather_pipe_ptr));
    layout.gather_pipe_base_offset =
        static_cast<u32>(offsetof(PowerPC::PowerPCState, gather_pipe_base_ptr));
    layout.hatch_mmio = false;
    // stw r4,0(r3) ; sth r5,0(r3) ; stb r4,0(r3) ; stfs f1,0(r3) ;
    // stwx r5,r3,r6 ; stb r5,0x10(r3)             -- 16 bytes
    const u32 prog[] = {0x9083'0000, 0xB0A3'0000, 0x9883'0000,
                        0xD023'0000, 0x7CA3'312E, 0x98A3'0010};
    constexpr u32 kN = sizeof(prog) / sizeof(prog[0]);
    auto setup = [&](size_t fill) {
      for (u32 i = 0; i < kN; ++i)
        memory.Write_U32(prog[i], BASE + i * 4);
      state.pc = state.npc = BASE;
      state.gpr[3] = 0xCC008000u;
      state.gpr[4] = 0x11223344u;
      state.gpr[5] = 0xAABBCCDDu;
      state.gpr[6] = 0x20;
      state.ps[1].SetPS0(std::bit_cast<u64>(1.5));
      std::memset(state.gather_pipe_base_ptr, 0, 32);
      state.gather_pipe_ptr = state.gather_pipe_base_ptr + fill;
      state.downcount = 1000;
    };
    for (size_t fill : {size_t{0}, size_t{15}})
    {
      setup(fill);
      for (u32 i = 0; i < kN; ++i)
        ppc.SingleStep();
      u8 want[32];
      std::memcpy(want, state.gather_pipe_base_ptr, sizeof(want));
      const size_t want_count = state.gather_pipe_ptr - state.gather_pipe_base_ptr;

      auto r = ppc2wasm::CompileBlock(prog, BASE, kN, layout);
      bool ok = r.instructions == kN;
      if (ok)
      {
        const int idx = InstallModule(r.module.data(), static_cast<int>(r.module.size()));
        ok = idx >= 0;
        if (ok)
        {
          setup(fill);
          const u32 ret = RunBlock(idx, state, ppc);
          const size_t got_count = state.gather_pipe_ptr - state.gather_pipe_base_ptr;
          ok = (ret & ~3u) == BASE + 4 * kN && got_count == want_count &&
               std::memcmp(want, state.gather_pipe_base_ptr, sizeof(want)) == 0;
          if (!ok)
          {
            std::printf("      pc %08x count %zu (want %zu)\n", ret, got_count, want_count);
            for (int i = 0; i < 32; ++i)
              std::printf("%02x%s", state.gather_pipe_base_ptr[i], i == 31 ? "\n" : " ");
            for (int i = 0; i < 32; ++i)
              std::printf("%02x%s", want[i], i == 31 ? "\n" : " ");
          }
        }
      }
      char label[96];
      std::snprintf(label, sizeof(label), "gather pipe: six stores inline from a buffer at %zu",
                    fill);
      check(ok, label);
    }
    layout.hatch_mmio = true;
  }

  // ── the locked L1 cache ──
  //
  // Stores and loads at 0xE0001000 reach Dolphin's locked-cache buffer in
  // both engines; the buffer, the registers and the paired-single result
  // are compared. Compiled without the MMIO hatch, so an access that
  // missed the locked-cache path would bail and the later stores would
  // be missing from the buffer.
  {
    layout.hatch_mmio = false;
    // lis r3,0xE000 ; ori r3,r3,0x1000 ; lis r4,0x1122 ; ori r4,r4,0x3344 ;
    // stw r4,0(r3) ; lwz r5,0(r3) ; lbz r6,1(r3) ; sth r4,8(r3) ;
    // psq_st f1,0x10(r3),0,0 ; psq_l f2,0x10(r3),0,0 ; lhz r7,8(r3)
    const u32 prog[] = {0x3C60'E000, 0x6063'1000, 0x3C80'1122, 0x6084'3344, 0x9083'0000,
                        0x80A3'0000, 0x88C3'0001, 0xB083'0008, 0xF023'0010, 0xE043'0010,
                        0xA0E3'0008};
    constexpr u32 kN = sizeof(prog) / sizeof(prog[0]);
    u8* l1 = memory.GetL1Cache();
    auto setup = [&]() {
      for (u32 i = 0; i < kN; ++i)
        memory.Write_U32(prog[i], BASE + i * 4);
      state.pc = state.npc = BASE;
      for (int i = 0; i < 32; ++i)
        state.gpr[i] = 0;
      state.ps[1].SetBoth(1.5, -2.25);
      state.ps[2].SetBoth(0.0, 0.0);
      std::memset(l1 + 0x1000, 0, 32);
      state.downcount = 1000;
    };
    bool ok = l1 != nullptr && layout.l1_cache_base != 0;
    if (ok)
    {
      setup();
      for (u32 i = 0; i < kN; ++i)
        ppc.SingleStep();
      u8 want[32];
      std::memcpy(want, l1 + 0x1000, sizeof(want));
      u32 want_gpr[32];
      std::memcpy(want_gpr, state.gpr, sizeof(want_gpr));
      const u64 want_ps[2] = {state.ps[2].ps0, state.ps[2].ps1};
      auto r = ppc2wasm::CompileBlock(prog, BASE, kN, layout);
      ok = r.instructions == kN;
      if (ok)
      {
        const int idx = InstallModule(r.module.data(), static_cast<int>(r.module.size()));
        ok = idx >= 0;
        if (ok)
        {
          setup();
          const u32 ret = RunBlock(idx, state, ppc);
          ok = (ret & ~3u) == BASE + 4 * kN && std::memcmp(want, l1 + 0x1000, sizeof(want)) == 0 &&
               std::memcmp(want_gpr, state.gpr, sizeof(want_gpr)) == 0 &&
               state.ps[2].ps0 == want_ps[0] && state.ps[2].ps1 == want_ps[1];
          if (!ok)
          {
            std::printf("      pc %08x r5 %08x (want %08x) r6 %08x (want %08x) r7 %08x (want %08x)\n",
                        ret, state.gpr[5], want_gpr[5], state.gpr[6], want_gpr[6], state.gpr[7],
                        want_gpr[7]);
            std::printf("      f2 %016llx:%016llx (want %016llx:%016llx)\n",
                        (unsigned long long)state.ps[2].ps0, (unsigned long long)state.ps[2].ps1,
                        (unsigned long long)want_ps[0], (unsigned long long)want_ps[1]);
          }
        }
      }
    }
    check(ok, "locked L1 cache: stores, loads and a quantized pair at 0xE0001000");
    layout.hatch_mmio = true;
  }

  // ── the GQR guard ──
  //
  // psq_l is specialised on GQR1's value at compile time. A block that
  // writes GQR1 and then loads through it must get the new format: the
  // guard sees the register differ and the hatch runs the load.
  {
    // lis r3,0x8000 ; ori r3,r3,0x7800 ; lis r4,0x0007 ; ori r4,r4,0x0007 ;
    // li r5,0x1234 ; stw r5,0(r3) ; mtspr GQR1,r4 ; psq_l f1,0(r3),0,1 ;
    // mtspr GQR1,r6 (restores the seeded value) ; psq_l f2,0(r3),0,1
    const u32 saved = state.spr[SPR_GQR0 + 1];
    const u32 prog[] = {0x3C60'8000, 0x6063'7800, 0x3C80'0007, 0x6084'0007, 0x38A0'1234,
                        0x90A3'0000, 0x7C91'E3A6, 0xE023'1000, 0x7CD1'E3A6, 0xE043'1000};
    constexpr u32 kN = sizeof(prog) / sizeof(prog[0]);
    auto setup = [&]() {
      for (u32 i = 0; i < kN; ++i)
        memory.Write_U32(prog[i], BASE + i * 4);
      state.pc = state.npc = BASE;
      state.spr[SPR_GQR0 + 1] = saved;
      state.gpr[6] = saved;
      state.ps[1].SetBoth(0.0, 0.0);
      state.ps[2].SetBoth(0.0, 0.0);
      state.downcount = 1000;
    };
    setup();
    for (u32 i = 0; i < kN; ++i)
      ppc.SingleStep();
    const u64 want1[2] = {state.ps[1].ps0, state.ps[1].ps1};
    const u64 want2[2] = {state.ps[2].ps0, state.ps[2].ps1};
    auto r = ppc2wasm::CompileBlock(prog, BASE, kN, layout);
    bool ok = r.instructions == kN;
    if (ok)
    {
      const int idx = InstallModule(r.module.data(), static_cast<int>(r.module.size()));
      ok = idx >= 0;
      if (ok)
      {
        setup();
        const u32 ret = RunBlock(idx, state, ppc);
        ok = (ret & ~3u) == BASE + 4 * kN && state.ps[1].ps0 == want1[0] &&
             state.ps[1].ps1 == want1[1] && state.ps[2].ps0 == want2[0] &&
             state.ps[2].ps1 == want2[1];
        if (!ok)
          std::printf("      pc %08x f1 %016llx:%016llx (want %016llx:%016llx) f2 %016llx:%016llx "
                      "(want %016llx:%016llx)\n",
                      ret, (unsigned long long)state.ps[1].ps0, (unsigned long long)state.ps[1].ps1,
                      (unsigned long long)want1[0], (unsigned long long)want1[1],
                      (unsigned long long)state.ps[2].ps0, (unsigned long long)state.ps[2].ps1,
                      (unsigned long long)want2[0], (unsigned long long)want2[1]);
      }
    }
    check(ok, "psq_l after mtspr GQR1 takes the guard and gets the new format");
    state.spr[SPR_GQR0 + 1] = saved;
  }

  // ── paired arithmetic as vectors ──
  //
  // At Dolphin's JIT accuracy (fast_fp, without fprf) the paired forms
  // are not the interpreter's, so the oracle is the scalar form of the
  // same thing: each instruction compiled twice, one half at a time and
  // as vectors (StateLayout::simd_ps), run from the same state, and every
  // register compared bit for bit. A quarter of the trials build a
  // multiply-add whose sum lands on a float rounding tie, the one branch
  // random operands almost never reach; FPSCR[NI] is set in half.
  {
    std::printf("\n  paired arithmetic as vectors, against the scalar forms\n");
    struct P { u32 xo5; const char* name; };
    static const P forms[] = {
        {21, "ps_add"},  {20, "ps_sub"},  {25, "ps_mul"},   {18, "ps_div"},
        {12, "ps_muls0"}, {13, "ps_muls1"}, {14, "ps_madds0"}, {15, "ps_madds1"},
        {28, "ps_msub"}, {29, "ps_madd"}, {30, "ps_nmsub"}, {31, "ps_nmadd"},
    };
    constexpr u32 kForms = sizeof(forms) / sizeof(forms[0]);
    ppc2wasm::StateLayout scalar_layout = layout;
    scalar_layout.fast_fp = true;
    scalar_layout.fprf = false;
    scalar_layout.simd_ps = false;
    ppc2wasm::StateLayout vector_layout = scalar_layout;
    vector_layout.simd_ps = true;
    u32 seed = 0x5EED'1234;
    auto rnd = [&seed]() {
      seed ^= seed << 13;
      seed ^= seed >> 17;
      seed ^= seed << 5;
      return seed;
    };
    const u32 saved_fpscr = state.fpscr.Hex;
    u32 runs[kForms] = {}, bad[kForms] = {}, ties = 0;
    int slot_s = -1, slot_v = -1;
    constexpr u32 kTrials = 6000;
    for (u32 t = 0; t < kTrials; ++t)
    {
      const u32 f = rnd() % kForms;
      const u32 fd = rnd() % 32, fa = rnd() % 32, fb = rnd() % 32, fc = rnd() % 32;
      const u32 ins = (4u << 26) | (fd << 21) | (fa << 16) | (fb << 11) | (fc << 6) |
                      (forms[f].xo5 << 1);
      seed_ps(rnd);
      if (forms[f].xo5 >= 14 && (rnd() % 4) == 0)
      {
        // a * c lands exactly on a tie (1 + 2^-24, scaled), and b is zero
        // or far below it, so the sum rounds onto the tie with an error.
        const int k = static_cast<int>(rnd() % 40) - 20;
        const double one = std::ldexp((rnd() & 1) ? -1.0 : 1.0, k);
        const double cv = 1.0 + std::ldexp(1.0, -24);
        const u32 pick = rnd() % 3;
        const double bv = pick == 0 ? 0.0 : std::ldexp(pick == 1 ? 1.0 : -1.0, k - 60);
        state.ps[fa].SetBoth(std::bit_cast<u64>(one), std::bit_cast<u64>(one));
        state.ps[fc].SetBoth(std::bit_cast<u64>(cv), std::bit_cast<u64>(cv));
        if (fb != fa && fb != fc)
          state.ps[fb].SetBoth(std::bit_cast<u64>(bv), std::bit_cast<u64>(-bv));
        ++ties;
      }
      state.fpscr.Hex = (rnd() & 1) ? 4u : 0u;
      PowerPC::PairedSingle start[32];
      for (int i = 0; i < 32; ++i)
        start[i] = state.ps[i];
      const u32 start_fpscr = state.fpscr.Hex;
      auto rs = ppc2wasm::CompileBlock(&ins, BASE, 1, scalar_layout);
      auto rv = ppc2wasm::CompileBlock(&ins, BASE, 1, vector_layout);
      if (rs.instructions != 1 || rv.instructions != 1)
        continue;
      slot_s = InstallModuleAt(rs.module.data(), static_cast<int>(rs.module.size()), slot_s);
      slot_v = InstallModuleAt(rv.module.data(), static_cast<int>(rv.module.size()), slot_v);
      if (slot_s < 0 || slot_v < 0)
      {
        ++bad[f];
        break;
      }
      state.pc = state.npc = BASE;
      state.downcount = 1000;
      RunBlock(slot_s, state, ppc);
      PowerPC::PairedSingle want[32];
      for (int i = 0; i < 32; ++i)
        want[i] = state.ps[i];
      const u32 want_fpscr = state.fpscr.Hex;
      for (int i = 0; i < 32; ++i)
        state.ps[i] = start[i];
      state.fpscr.Hex = start_fpscr;
      state.pc = state.npc = BASE;
      state.downcount = 1000;
      RunBlock(slot_v, state, ppc);
      bool same = state.fpscr.Hex == want_fpscr;
      for (int i = 0; i < 32; ++i)
        same = same && state.ps[i].ps0 == want[i].ps0 && state.ps[i].ps1 == want[i].ps1;
      ++runs[f];
      if (!same)
      {
        if (bad[f]++ < 3)
          std::printf("      %s %08x: f%u %016llx:%016llx, scalar %016llx:%016llx\n",
                      forms[f].name, ins, fd, (unsigned long long)state.ps[fd].ps0,
                      (unsigned long long)state.ps[fd].ps1, (unsigned long long)want[fd].ps0,
                      (unsigned long long)want[fd].ps1);
      }
    }
    char line[96];
    for (u32 f = 0; f < kForms; ++f)
    {
      std::snprintf(line, sizeof(line), "%s: %u runs, the same bits", forms[f].name, runs[f]);
      check(runs[f] > 0 && bad[f] == 0, line);
    }
    std::printf("      %u trials built on a rounding tie\n", ties);

    // Blocks: twelve instructions drawn from the paired arithmetic, the
    // forms that write a paired register without the vectors (merges,
    // moves, scalar arithmetic) and the quantized loads and stores through
    // r3, so the vector and scalar forms meet in one block. Registers,
    // FPSCR and the memory window are compared.
    constexpr u32 kK = 12, kBlocks = 3000;
    constexpr u32 kWin = BASE + 0x4000, kWinBytes = 256;
    u32 block_runs = 0, block_bad = 0;
    std::vector<u8> win0(kWinBytes), want_win(kWinBytes);
    for (u32 t = 0; t < kBlocks; ++t)
    {
      u32 prog[kK];
      for (u32 i = 0; i < kK; ++i)
      {
        const u32 fd = rnd() % 12, fa = rnd() % 12, fb = rnd() % 12, fc = rnd() % 12;
        const u32 kind = rnd() % 8;
        if (kind < 4)
          prog[i] = (4u << 26) | (fd << 21) | (fa << 16) | (fb << 11) | (fc << 6) |
                    (forms[rnd() % kForms].xo5 << 1);
        else if (kind == 4)
        {
          static const u32 xo[] = {528, 560, 592, 624, 72, 40};  // merges, ps_mr, ps_neg
          prog[i] = (4u << 26) | (fd << 21) | (fa << 16) | (fb << 11) | (xo[rnd() % 6] << 1);
        }
        else if (kind == 5)
        {
          static const u32 xo5[] = {21, 25, 29};  // fadds, fmuls, fmadds
          prog[i] = (59u << 26) | (fd << 21) | (fa << 16) | (fb << 11) | (fc << 6) |
                    (xo5[rnd() % 3] << 1);
        }
        else
        {
          // psq_l (56) or psq_st (60), d(r3), W random, GQR0..4.
          const u32 opc = (kind == 6) ? 56u : 60u;
          const u32 d = (rnd() % 24) * 8;
          prog[i] = (opc << 26) | (fd << 21) | (3u << 16) | ((rnd() & 1) << 15) |
                    ((rnd() % 5) << 12) | d;
        }
      }
      seed_ps(rnd);
      state.fpscr.Hex = (rnd() & 1) ? 4u : 0u;
      state.gpr[3] = kWin;
      for (u32 i = 0; i < kWinBytes; i += 4)
        memory.Write_U32(rnd(), kWin + i);
      PowerPC::PairedSingle start[32];
      for (int i = 0; i < 32; ++i)
        start[i] = state.ps[i];
      const u32 start_fpscr = state.fpscr.Hex;
      memory.CopyFromEmu(win0.data(), kWin, kWinBytes);
      auto rs = ppc2wasm::CompileBlock(prog, BASE, kK, scalar_layout);
      auto rv = ppc2wasm::CompileBlock(prog, BASE, kK, vector_layout);
      if (rs.instructions != kK || rv.instructions != kK)
        continue;
      slot_s = InstallModuleAt(rs.module.data(), static_cast<int>(rs.module.size()), slot_s);
      slot_v = InstallModuleAt(rv.module.data(), static_cast<int>(rv.module.size()), slot_v);
      if (slot_s < 0 || slot_v < 0)
      {
        ++block_bad;
        break;
      }
      state.pc = state.npc = BASE;
      state.downcount = 1000;
      const u32 ret_s = RunBlock(slot_s, state, ppc);
      PowerPC::PairedSingle want[32];
      for (int i = 0; i < 32; ++i)
        want[i] = state.ps[i];
      const u32 want_fpscr = state.fpscr.Hex;
      memory.CopyFromEmu(want_win.data(), kWin, kWinBytes);
      for (int i = 0; i < 32; ++i)
        state.ps[i] = start[i];
      state.fpscr.Hex = start_fpscr;
      memory.CopyToEmu(kWin, win0.data(), kWinBytes);
      state.gpr[3] = kWin;
      state.pc = state.npc = BASE;
      state.downcount = 1000;
      const u32 ret_v = RunBlock(slot_v, state, ppc);
      bool same = ret_s == ret_v && state.fpscr.Hex == want_fpscr;
      for (int i = 0; i < 32; ++i)
        same = same && state.ps[i].ps0 == want[i].ps0 && state.ps[i].ps1 == want[i].ps1;
      std::vector<u8> got_win(kWinBytes);
      memory.CopyFromEmu(got_win.data(), kWin, kWinBytes);
      same = same && got_win == want_win;
      ++block_runs;
      if (!same && block_bad++ < 3)
      {
        std::printf("      block %u differs:", t);
        for (u32 i = 0; i < kK; ++i)
          std::printf(" %08x", prog[i]);
        std::printf("\n");
      }
    }
    state.fpscr.Hex = saved_fpscr;
    std::snprintf(line, sizeof(line), "blocks: %u runs, the same bits", block_runs);
    check(block_runs > kBlocks / 2 && block_bad == 0, line);
  }

  // ── randomised differential ──
  //
  // Thirty-odd hand-written cases check the instructions someone thought to
  // write a case for, with the register numbers and operand values they
  // happened to choose. That is exactly the coverage that lets a decode bug
  // reach a two-hundred-million-instruction boot and crash it.
  //
  // This fires random encodings of every supported form at both engines, with
  // random register numbers and random register contents, and compares the
  // whole guest state.
  {
    std::printf("\n  randomised differential\n");
    // Templates: primary opcode, and for the extended space an XO.
    struct T { u32 primary; u32 xo; const char* name; };
    static const T templates[] = {
        {14, 0, "addi"},    {15, 0, "addis"},  {7, 0, "mulli"},   {11, 0, "cmpi"},
        {10, 0, "cmpli"},   {24, 0, "ori"},    {25, 0, "oris"},   {26, 0, "xori"},
        {27, 0, "xoris"},   {21, 0, "rlwinm"},
        {32, 0, "lwz"},     {34, 0, "lbz"},    {40, 0, "lhz"},    {42, 0, "lha"},
        {36, 0, "stw"},     {38, 0, "stb"},    {44, 0, "sth"},
        {31, 266, "add"},   {31, 40, "subf"},  {31, 444, "or"},   {31, 28, "and"},
        {31, 316, "xor"},   {31, 24, "slw"},   {31, 536, "srw"},  {31, 0, "cmp"},
        {31, 32, "cmpl"},   {31, 954, "extsb"},{31, 922, "extsh"},{31, 104, "neg"},
        {31, 26, "cntlzw"}, {31, 60, "andc"},  {31, 412, "orc"},  {31, 124, "nor"},
        {31, 476, "nand"},  {31, 284, "eqv"},  {31, 235, "mullw"},
        {31, 83, "mfmsr"}, {31, 146, "mtmsr"},
        {31, 23, "lwzx"},   {31, 151, "stwx"}, {31, 87, "lbzx"},  {31, 215, "stbx"},
        {31, 279, "lhzx"},  {31, 407, "sthx"},
        // Update forms and the SPR moves. Both write a GPR in a shape no
        // other template here produces -- the update forms write rA, the
        // BASE register, on top of whatever they load; mfspr writes a
        // register from somewhere that is not another register. The boot
        // is full of both and neither had ever been generated.
        {33, 0, "lwzu"},    {37, 0, "stwu"},   {35, 0, "lbzu"},   {39, 0, "stbu"},
        {41, 0, "lhzu"},    {45, 0, "sthu"},
        {31, 55, "lwzux"},  {31, 183, "stwux"},
        {31, 339, "mfspr"}, {31, 467, "mtspr"},
        // The carry forms, which write XER[CA] -- compared below, because a
        // wrong carry is invisible in the registers for a whole block.
        {12, 0, "addic"},   {13, 0, "addic."}, {8, 0, "subfic"},
        {31, 10, "addc"},   {31, 8, "subfc"},  {31, 138, "adde"}, {31, 136, "subfe"},
        {31, 202, "addze"}, {31, 200, "subfze"},
        {31, 824, "srawi"}, {31, 792, "sraw"},
        {31, 491, "divw"},  {31, 459, "divwu"}, {31, 75, "mulhw"}, {31, 11, "mulhwu"},
        {20, 0, "rlwimi"},  {23, 0, "rlwnm"},
        {31, 1014, "dcbz"}, {31, 86, "dcbf"}, {31, 54, "dcbst"}, {31, 278, "dcbt"},
        {31, 598, "sync"},  {31, 854, "eieio"},
        // Floating-point loads and stores, and the moves that touch no flags.
        {48, 0, "lfs"},     {49, 0, "lfsu"},   {50, 0, "lfd"},    {51, 0, "lfdu"},
        {52, 0, "stfs"},    {53, 0, "stfsu"},  {54, 0, "stfd"},   {55, 0, "stfdu"},
        {31, 535, "lfsx"},  {31, 567, "lfsux"}, {31, 599, "lfdx"}, {31, 631, "lfdux"},
        {31, 663, "stfsx"}, {31, 695, "stfsux"}, {31, 727, "stfdx"}, {31, 759, "stfdux"},
        {31, 983, "stfiwx"},
        {63, 72, "fmr"},    {63, 40, "fneg"},  {63, 264, "fabs"}, {63, 136, "fnabs"},
        {63, 23, "fsel"},
        {4, 72, "ps_mr"},   {4, 40, "ps_neg"}, {4, 264, "ps_abs"}, {4, 136, "ps_nabs"},
        {4, 528, "ps_merge00"}, {4, 560, "ps_merge01"}, {4, 592, "ps_merge10"},
        {4, 624, "ps_merge11"}, {4, 23, "ps_sel"},
        {46, 0, "lmw"},     {47, 0, "stmw"},
        {31, 20, "lwarx"},  {31, 150, "stwcx."},
        // Through the interpreter hatch: FP arithmetic, compares and
        // conversions, the paired forms, quantized loads and stores, and
        // the condition-register moves.
        {63, 21, "fadd"},   {63, 20, "fsub"},  {63, 25, "fmul"},  {63, 18, "fdiv"},
        {63, 29, "fmadd"},  {63, 28, "fmsub"}, {63, 31, "fnmadd"}, {63, 12, "frsp"},
        {63, 15, "fctiwz"}, {63, 14, "fctiw"}, {63, 0, "fcmpu"},  {63, 32, "fcmpo"},
        {63, 583, "mffs"},  {63, 26, "frsqrte"},
        {59, 21, "fadds"},  {59, 20, "fsubs"}, {59, 25, "fmuls"}, {59, 18, "fdivs"},
        {59, 29, "fmadds"}, {59, 28, "fmsubs"}, {59, 30, "fnmsubs"}, {59, 24, "fres"},
        {4, 21, "ps_add"},  {4, 20, "ps_sub"}, {4, 25, "ps_mul"}, {4, 18, "ps_div"},
        {4, 29, "ps_madd"}, {4, 28, "ps_msub"}, {4, 10, "ps_sum0"}, {4, 11, "ps_sum1"},
        {4, 12, "ps_muls0"}, {4, 13, "ps_muls1"}, {4, 14, "ps_madds0"}, {4, 0, "ps_cmpu0"},
        {4, 24, "ps_res"},  {4, 15, "ps_madds1"}, {4, 30, "ps_nmsub"}, {4, 31, "ps_nmadd"},
        {56, 0, "psq_l"},   {60, 0, "psq_st"}, {57, 0, "psq_lu"}, {61, 0, "psq_stu"},
        {4, 6, "psq_lx"},   {4, 7, "psq_stx"}, {4, 38, "psq_lux"}, {4, 39, "psq_stux"},
        {31, 19, "mfcr"},   {31, 144, "mtcrf"}, {19, 193, "crxor"}, {19, 449, "cror"},
        {19, 0, "mcrf"},
    };

    u32 seed = 0x12345678;
    auto rnd = [&seed]() {
      seed ^= seed << 13;
      seed ^= seed >> 17;
      seed ^= seed << 5;
      return seed;
    };

    // Linking off: what is compared here is one instruction's effect on
    // the state, which linking does not change -- it decides how a block
    // EXITS, and the block and chain differentials below test that with it
    // on. A linked block imports the function table, and Node keeps every
    // instance that does (see InstallModuleAt); some 24,000 single-
    // instruction modules held about 2.5 GB, and three more templates took
    // the worker past its limit at the start of the block differential.
    ppc2wasm::StateLayout single_layout = layout;
    single_layout.enable_linking = false;

    int rand_fail = 0;
    for (const T& t : templates)
    {
      int fails_here = 0;
      for (int trial = 0; trial < 200 && fails_here < 2; ++trial)
      {
        const u32 a = rnd() % 31 + 1;
        // b must differ from a: the two are given different roles below (base
        // address and offset), and when they collide the second assignment
        // destroys the first, producing an address outside MEM1 that the
        // interpreter faults on and a compiled block refuses. Both are
        // correct and they are not comparable.
        u32 b = rnd() % 31 + 1;
        if (b == a)
          b = (a % 31) + 1;
        const u32 d = rnd() % 31 + 1;
        u32 ins;
        if (t.primary == 31 && (t.xo == 339 || t.xo == 467))
        {
          // The SPR field is split and swapped, and only LR and CTR are
          // translated. Anything else stops the block and compares
          // nothing.
          // GQRs are read but never written here: a random quantization type
          // is undefined (Helper_Dequantize has no case for it), and the
          // interpreter traps on one.
          static const u32 kSprs[] = {8, 9, 26, 27, 272, 273, 274, 275, 912, 915, 919, 1, 18, 19};
          static const u32 kWritableSprs[] = {8, 9, 26, 27, 272, 273, 274, 275, 1, 18, 19};
          const u32 spr = (t.xo == 339) ? kSprs[rnd() % (sizeof(kSprs) / sizeof(kSprs[0]))]
                                        : kWritableSprs[rnd() % (sizeof(kWritableSprs) / sizeof(kWritableSprs[0]))];
          ins = (31u << 26) | (d << 21) | ((spr & 0x1F) << 16) |
                (((spr >> 5) & 0x1F) << 11) | (t.xo << 1);
        }
        else if (t.primary == 31)
        {
          ins = (31u << 26) | (d << 21) | (a << 16) | (b << 11) | (t.xo << 1);
          // Set Rc half the time wherever the form has one. CR0 after a
          // dot form uses an encoding the interpreter builds by hand
          // rather than from s_crTable, so it is a second thing that can
          // be wrong and the only check on it is this comparison.
          if ((rnd() & 1) && ppc2wasm::detail::RcDestination(ins | 1u) >= 0)
            ins |= 1u;
        }
        else if (t.primary == 4 && (t.xo == 6 || t.xo == 7 || t.xo == 38 || t.xo == 39))
        {
          // psq_lx / psq_stx / psq_lux / psq_stux: W and the GQR sit in the
          // low bits of the ten-bit field, above the six-bit opcode.
          ins = (4u << 26) | (d << 21) | (a << 16) | (b << 11) | ((rnd() & 1) << 10) |
                ((rnd() % 5) << 7) | (t.xo << 1);
        }
        else if (t.primary == 63 || t.primary == 4 || t.primary == 59)
        {
          // A-forms (five-bit XO, frC above it) for the arithmetic; the
          // compares put a CR field where rD would be; everything else is
          // an X-form with the full ten-bit XO.
          const u32 c = rnd() % 32;
          // 12, 14 and 15 are X-forms on op 63 (frsp, fctiw, fctiwz) and
          // A-forms on op 4 (ps_muls0, ps_madds0, ps_madds1), with an frC.
          const bool a_form = t.xo < 32 && t.xo != 0 &&
                              (t.primary == 4 || (t.xo != 12 && t.xo != 14 && t.xo != 15));
          if (a_form)
            ins = (t.primary << 26) | (d << 21) | (a << 16) | (b << 11) | (c << 6) | (t.xo << 1);
          else if (t.xo == 0 || t.xo == 32)  // fcmpu, fcmpo, ps_cmpu0
            ins = (t.primary << 26) | ((rnd() % 8) << 23) | (a << 16) | (b << 11) | (t.xo << 1);
          else
            ins = (t.primary << 26) | (d << 21) | (a << 16) | (b << 11) | (t.xo << 1);
          if ((rnd() & 1) && t.xo != 0 && t.xo != 32)
            ins |= 1u;  // Rc: CR1 from FPSCR
        }
        else if (t.primary >= 56 && t.primary <= 61)
        {
          // psq_l / psq_st and their update forms: W selects one or both
          // halves, I one of the five GQRs seeded above, and a small
          // positive displacement keeps the address in the window.
          ins = (t.primary << 26) | (d << 21) | (a << 16) | ((rnd() & 1) << 15) |
                ((rnd() % 5) << 12) | (rnd() & 0x07F0);  // twelve-bit field: bit 11 is the sign
        }
        else if (t.primary == 19)
        {
          // crxor / cror take three CR bit numbers; mcrf two fields.
          if (t.xo == 0)
            ins = (19u << 26) | ((rnd() % 8) << 23) | ((rnd() % 8) << 18) | (t.xo << 1);
          else
            ins = (19u << 26) | ((rnd() % 32) << 21) | ((rnd() % 32) << 16) | ((rnd() % 32) << 11) |
                  (t.xo << 1);
        }
        else if (t.primary == 31 && t.xo == 144)  // mtcrf: a random field mask
          ins = (31u << 26) | (d << 21) | ((rnd() & 0xFF) << 12) | (t.xo << 1);
        else if (t.primary == 46 || t.primary == 47)
        {
          // lmw/stmw from a high register so the run stays short enough to
          // land inside MEM1 near the base.
          ins = (t.primary << 26) | ((20 + rnd() % 12) << 21) | (a << 16) | (rnd() & 0x00F0);
        }
        else
        {
          // A SMALL non-negative displacement, so the address stays inside
          // MEM1. A random 16-bit field is signed and routinely lands outside
          // it, where the interpreter faults and a compiled block bails --
          // both correct, and not comparable.
          ins = (t.primary << 26) | (d << 21) | (a << 16) | (rnd() & 0x0FF0);
          if ((t.primary == 21 || t.primary == 20 || t.primary == 23) && (rnd() & 1))
            ins |= 1u;  // rlwinm. / rlwimi. / rlwnm.
        }

        // Restore the machine state before every trial.
        //
        // One of the templates is mtmsr, and it writes a register seeded
        // with a random 32-bit value -- which clears MSR[DR]. From that
        // trial onward the INTERPRETER cannot translate a data address,
        // so it reads zero while compiled code reads real memory, and the
        // oracle is the side that is wrong. It presents as a load
        // mismatch, with "Unable to resolve read address" in the log for
        // anyone who thinks to look.
        //
        // The same bug was found by review in the block and chain
        // differentials. It was latent here for exactly as long, and only
        // surfaced when an unrelated change reshuffled the random stream.
        state.msr.Hex = kPristineMsr;
        ppc.MSRUpdated();

        // Seed registers with a mix of small, negative and large values, and
        // point the ones used as addresses somewhere inside MEM1.
        for (int i = 0; i < 32; ++i)
          state.gpr[i] = (rnd() & 1) ? (rnd() & 0xFFFF) : safe_random(rnd());
        state.gpr[a] = 0x8000'0000 + (rnd() & 0x000F'FFF0);
        if (t.primary == 31 || t.primary == 4)
          state.gpr[b] = rnd() & 0xFF0;
        for (int i = 0; i < 8; ++i)
          state.cr.fields[i] = 0;
        state.xer_ca = static_cast<u8>(rnd() & 1);
        const u8 init_ca = state.xer_ca;
        // The rest of XER too. SO folds into every compare and every CR0
        // update, and mtxer now writes all three fields from compiled code;
        // left unreset, a value one run wrote leaks into the other's start.
        state.xer_so_ov = static_cast<u8>(rnd() & 3);
        state.xer_stringctrl = static_cast<u16>(rnd() & 0xFF7F);
        const u8 init_so_ov = state.xer_so_ov;
        const u16 init_sc = state.xer_stringctrl;
        // The reservation, half the time held for the address the memory
        // forms are about to use, so stwcx. takes both of its paths.
        state.reserve = (rnd() & 1) != 0;
        state.reserve_address = (rnd() & 1) ? state.gpr[a] + state.gpr[b] : rnd();
        const bool init_reserve = state.reserve;
        const u32 init_reserve_address = state.reserve_address;
        state.spr[SPR_LR] = 0;
        state.downcount = 1000;
        u32 before[32];
        std::memcpy(before, state.gpr, sizeof(before));
        seed_ps(rnd);
        // The paired multiply-adds, one trial in four: operands whose
        // product lands EXACTLY on a float rounding tie, which is the only
        // case where NI_madd_msub corrects the fma's rounding, and which
        // random operands reach about once in 2^25. a is +-2^k; c is a
        // single plus exactly half its ulp (bit 28, which Force25Bit
        // keeps), so a * c has the tie pattern; b is a signed zero (the
        // tie stands, error zero), a tiny value of either sign far below
        // the product's last bit (the tie stands, the error rounds it up
        // or down), or a single (the tie usually breaks).
        if (t.primary == 4 && (t.xo == 14 || t.xo == 15 || (t.xo >= 28 && t.xo <= 31)) &&
            trial % 4 == 0)
        {
          const u32 fa = (ins >> 16) & 0x1F, fb = (ins >> 11) & 0x1F, fc = (ins >> 6) & 0x1F;
          u64 av[2], cv[2], bv[2];
          for (int h = 0; h < 2; ++h)
          {
            const int k = static_cast<int>(rnd() % 41) - 20;
            av[h] = (static_cast<u64>(1023 + k) << 52) | ((rnd() & 1) ? 0x8000000000000000ull : 0);
            const u32 f = (rnd() & 0x807FFFFFu) | ((107u + rnd() % 41) << 23);
            cv[h] = std::bit_cast<u64>(static_cast<double>(std::bit_cast<float>(f))) | 0x10000000ull;
            const double p = std::bit_cast<double>(av[h]) * std::bit_cast<double>(cv[h]);
            const u32 kind = rnd() % 4;
            const double sign = (rnd() & 1) ? -1.0 : 1.0;
            if (kind == 0)
              bv[h] = std::bit_cast<u64>(sign * 0.0);
            else if (kind == 3)
              bv[h] = std::bit_cast<u64>(static_cast<double>(std::bit_cast<float>(
                  (rnd() & 0x807FFFFFu) | ((107u + rnd() % 41) << 23))));
            else
              bv[h] = std::bit_cast<u64>(sign * std::ldexp(1.0 + (rnd() & 0xFFFF) / 65536.0,
                                                           std::ilogb(p) - 60 -
                                                               static_cast<int>(rnd() % 20)));
          }
          state.ps[fc].SetPS0(cv[0]);
          state.ps[fc].SetPS1(cv[1]);
          state.ps[fb].SetPS0(bv[0]);
          state.ps[fb].SetPS1(bv[1]);
          state.ps[fa].SetPS0(av[0]);
          state.ps[fa].SetPS1(av[1]);
        }
        PowerPC::PairedSingle before_ps[32];
        std::memcpy(before_ps, state.ps, sizeof(before_ps));
        // FPSCR too: the Rc forms copy its top bits into CR1.
        // FPSCR: the sticky bits sometimes set, NI (flush denormals) and the
        // rounding mode varied, the exception-enable bits clear so nothing
        // traps -- traps go through the same path on both engines, but
        // they would end most trials before the interesting part.
        state.fpscr.Hex = ((rnd() & 1) ? (rnd() & 0xF0000000u) : 0) | ((rnd() & 1) << 2) | (rnd() & 3);
        const u32 init_fpscr = state.fpscr.Hex;

        memory.Write_U32(ins, BASE);
        state.pc = state.npc = BASE;
        ppc.SingleStep();
        u32 want[32];
        std::memcpy(want, state.gpr, sizeof(want));
        u64 want_cr[8];
        std::memcpy(want_cr, state.cr.fields, sizeof(want_cr));
        const u8 want_ca = state.xer_ca;
        const u8 want_so_ov = state.xer_so_ov;
        const u16 want_sc = state.xer_stringctrl;
        const bool want_reserve = state.reserve;
        const u32 want_reserve_address = state.reserve_address;
        const u32 want_fpscr = state.fpscr.Hex;
        PowerPC::PairedSingle want_ps[32];
        std::memcpy(want_ps, state.ps, sizeof(want_ps));

        auto rr = ppc2wasm::CompileBlock(&ins, BASE, 1, single_layout);
        if (rr.instructions != 1)
          continue;  // refused; nothing to compare
        // One slot, reused for every trial: 8,800 pinned instances is what
        // used to exhaust the heap mid-suite.
        static int slot = -1;
        const int ix = InstallModuleAt(rr.module.data(), static_cast<int>(rr.module.size()), slot);
        if (ix < 0)
          continue;
        slot = ix;

        std::memcpy(state.gpr, before, sizeof(before));
        std::memcpy(state.ps, before_ps, sizeof(before_ps));
        state.fpscr.Hex = init_fpscr;
        for (int i = 0; i < 8; ++i)
          state.cr.fields[i] = 0;
        state.xer_ca = init_ca;
        state.xer_so_ov = init_so_ov;
        state.xer_stringctrl = init_sc;
        state.reserve = init_reserve;
        state.reserve_address = init_reserve_address;
        state.spr[SPR_LR] = 0;
        state.downcount = 1000;
        state.pc = state.npc = BASE;
        RunBlock(ix, state, ppc);

        bool bad = false;
        for (int i = 0; i < 32 && !bad; ++i)
        {
          if (state.gpr[i] != want[i])
          {
            std::printf("  %-8s ins=%08x r%-2d interpreter=%08x recompiled=%08x\n", t.name, ins,
                        i, want[i], state.gpr[i]);
            bad = true;
          }
        }
        // The condition register, which an Rc form writes and nothing here
        // used to look at. A dot form that computed the right value and
        // the wrong CR0 would have passed.
        for (int i = 0; i < 8 && !bad; ++i)
        {
          if (CrBits(state.cr.fields[i]) != CrBits(want_cr[i]))
          {
            std::printf("  %-8s ins=%08x cr%d interpreter=%016llx recompiled=%016llx\n", t.name,
                        ins, i, static_cast<unsigned long long>(want_cr[i]),
                        static_cast<unsigned long long>(state.cr.fields[i]));
            bad = true;
          }
        }
        // The carry, which the add/subtract-with-carry forms, subfic and
        // the arithmetic shifts write, and which nothing in the registers
        // or the condition fields reveals.
        if (!bad && (state.xer_ca != want_ca || state.xer_so_ov != want_so_ov ||
                     state.xer_stringctrl != want_sc))
        {
          std::printf("  %-8s ins=%08x XER interpreter=ca%u so_ov%u sc%04x recompiled=ca%u so_ov%u sc%04x\n",
                      t.name, ins, want_ca, want_so_ov, want_sc, state.xer_ca, state.xer_so_ov,
                      state.xer_stringctrl);
          bad = true;
        }
        if (!bad && state.fpscr.Hex != want_fpscr)
        {
          std::printf("  %-8s ins=%08x FPSCR interpreter=%08x recompiled=%08x\n", t.name, ins,
                      want_fpscr, state.fpscr.Hex);
          bad = true;
        }
        if (!bad && (state.reserve != want_reserve ||
                     (state.reserve && state.reserve_address != want_reserve_address)))
        {
          std::printf("  %-8s ins=%08x reservation interpreter=%d@%08x recompiled=%d@%08x\n", t.name,
                      ins, want_reserve, want_reserve_address, state.reserve, state.reserve_address);
          bad = true;
        }
        for (int i = 0; i < 32 && !bad; ++i)
        {
          if (state.ps[i].PS0AsU64() != want_ps[i].PS0AsU64() ||
              state.ps[i].PS1AsU64() != want_ps[i].PS1AsU64())
          {
            std::printf("  %-8s ins=%08x f%-2d interpreter=%016llx/%016llx recompiled=%016llx/%016llx\n",
                        t.name, ins, i, static_cast<unsigned long long>(want_ps[i].PS0AsU64()),
                        static_cast<unsigned long long>(want_ps[i].PS1AsU64()),
                        static_cast<unsigned long long>(state.ps[i].PS0AsU64()),
                        static_cast<unsigned long long>(state.ps[i].PS1AsU64()));
            bad = true;
          }
        }
        if (bad)
        {
          ++fails_here;
          ++rand_fail;
        }
      }
    }
    check(rand_fail == 0, "randomised differential over every supported form");
  }

  // ── randomised BLOCK differential ──
  //
  // The per-instruction differential checks each form in isolation. What it
  // cannot see is everything that only exists once instructions are put
  // together: the block epilogue, a branch leaving mid-block, a guard
  // refusing partway through, and whether stores landed where they should.
  //
  // This generates random SEQUENCES, runs the interpreter for exactly as many
  // instructions as the compiled block actually executed, and compares the
  // whole of the guest state -- registers, condition fields, link and count
  // registers, and a window of guest MEMORY. Memory matters: a store to the
  // wrong address is invisible to any comparison that only reads registers.
  {
    std::printf("\n  randomised block differential\n");

    // A scratch window for stores, well away from the code being run, and a
    // second one in MEM2 so the region table's second entry is exercised:
    // a base register points into one or the other.
    constexpr u32 kScratch = 0x8001'0000;
    constexpr u32 kScratch2 = 0x9001'0000;
    constexpr u32 kScratchWords = 256;
    // ...and the megabyte above each, saved before the compiled run,
    // restored before the interpreter's, and compared after both. An
    // indexed form adds two registers, and a scratch pointer plus a 16-bit
    // value lands up to 64 KB past the window -- outside what reset()
    // rewrites. A store there by the compiled run used to survive into the
    // interpreter's, which then loaded what the compiled run had stored: a
    // psq_lx after an stwux to the same address read back a value it never
    // had and reported a mismatch that was the harness's.
    constexpr u32 kWideBytes = 1u << 20;
    u8* const wide[2] = {memory.GetRAM() + (kScratch & 0x01FFFFFFu),
                         memory.GetEXRAM() + (kScratch2 & 0x03FFFFFFu)};
    std::vector<u8> init_wide[2] = {std::vector<u8>(kWideBytes), std::vector<u8>(kWideBytes)};
    std::vector<u8> jit_wide[2] = {std::vector<u8>(kWideBytes), std::vector<u8>(kWideBytes)};

    // Instructions the generator draws from. Deliberately a mix of
    // arithmetic, memory, compares and branches, because a block of pure
    // arithmetic exercises none of the interesting paths.
    struct Gen { u32 primary; u32 xo; };
    static const Gen pool[] = {
        {14, 0}, {15, 0}, {7, 0},  {24, 0}, {26, 0}, {21, 0},
        {11, 0}, {10, 0},
        {32, 0}, {34, 0}, {40, 0}, {36, 0}, {38, 0}, {44, 0},
        {31, 266}, {31, 40}, {31, 444}, {31, 28}, {31, 316},
        {31, 24}, {31, 536}, {31, 954}, {31, 922}, {31, 104},
        {31, 26}, {31, 60}, {31, 124}, {31, 235},
        {31, 23}, {31, 151}, {31, 87}, {31, 215}, {31, 83}, {31, 146},
        // lwbrx and lhbrx run in the interpreter through the hatch and
        // write rD there; a later read in the same block must see it.
        {31, 534}, {31, 790},
        // Update forms, which write rA as well as the loaded register, and
        // mfspr, which writes a register from somewhere that is not
        // another register. Both are everywhere in the boot and neither
        // was ever generated here -- so every GPR write this pool
        // exercised came from the same two or three shapes.
        {33, 0},   // lwzu
        {37, 0},   // stwu
        {35, 0},   // lbzu
        {39, 0},   // stbu
        {41, 0},   // lhzu
        {45, 0},   // sthu
        {31, 55},  // lwzux
        {31, 183}, // stwux
        {31, 339}, // mfspr
        {31, 467}, // mtspr
        {16, 0},  // a conditional branch, to exercise an early exit
        // The carry forms, shifts, divides and dcbz. dcbz zeroes 32 bytes
        // of the scratch window, which the digest sees.
        {12, 0}, {13, 0}, {8, 0}, {31, 10}, {31, 8}, {31, 138}, {31, 136}, {31, 202}, {31, 200},
        {31, 824}, {31, 792}, {31, 491}, {31, 459}, {31, 75}, {31, 11}, {20, 0}, {23, 0},
        {31, 1014}, {31, 86}, {31, 278}, {31, 598},
        // Floating-point loads, stores and moves; lmw/stmw.
        {48, 0}, {50, 0}, {52, 0}, {54, 0}, {49, 0}, {53, 0},
        {31, 535}, {31, 599}, {31, 663}, {31, 727}, {31, 983},
        {63, 72}, {63, 40}, {63, 264}, {63, 136}, {63, 23},
        {4, 72}, {4, 40}, {4, 264}, {4, 528}, {4, 624}, {4, 23},
        {46, 0}, {47, 0},
        {31, 20}, {31, 150},  // lwarx, stwcx.
        // Through the hatch.
        {63, 21}, {63, 25}, {63, 29}, {63, 12}, {63, 15}, {63, 0}, {63, 32},
        {59, 21}, {59, 25}, {59, 29}, {59, 20}, {59, 18},
        {4, 21}, {4, 25}, {4, 29}, {4, 10}, {4, 12}, {4, 0},
        {4, 14}, {4, 15}, {4, 28}, {4, 31},
        {56, 0}, {60, 0}, {57, 0}, {61, 0}, {4, 6}, {4, 7}, {4, 38}, {4, 39},
        {31, 19}, {31, 144}, {19, 193},
    };
    // Forms that carry an Rc bit, drawn with it set so CR0 is exercised
    // inside a block and not only in isolation -- a dot form mid-block
    // writes CR0 that a later conditional branch then reads.
    auto maybe_rc = [](u32 ins, bool want) {
      return (want && ppc2wasm::detail::RcDestination(ins | 1u) >= 0) ? (ins | 1u) : ins;
    };

    u32 seed = 0xC0FFEE11;
    auto rnd = [&seed]() {
      seed ^= seed << 13;
      seed ^= seed >> 17;
      seed ^= seed << 5;
      return seed;
    };

    auto digest = [&]() {
      u64 h = 0xcbf29ce484222325ULL;
      for (u32 base : {kScratch, kScratch2})
        for (u32 w = 0; w < kScratchWords; ++w)
        {
          const u32 v = memory.Read_U32(base + w * 4);
          for (int by = 0; by < 4; ++by)
          {
            h ^= (v >> (by * 8)) & 0xFF;
            h *= 0x100000001b3ULL;
          }
        }
      return h;
    };

    // Deliberately far larger than any block, so the budget never expires
    // mid-block and the delta measures only what the block itself ran.
    constexpr u32 kBudget = 10000;

    int block_fail = 0, blocks_run = 0;
    for (int trial = 0; trial < 1500 && block_fail < 4; ++trial)
    {
      const u32 len = 2 + (rnd() % 40);
      std::vector<u32> prog;
      for (u32 k = 0; k < len; ++k)
      {
        const Gen& g = pool[rnd() % (sizeof(pool) / sizeof(pool[0]))];
        const u32 a = rnd() % 31 + 1;
        u32 b = rnd() % 31 + 1;
        if (b == a)
          b = (a % 31) + 1;
        const u32 d = rnd() % 31 + 1;
        u32 ins;
        if (g.primary == 31 && g.xo == 146)
          // mtmsr always takes r1, which is seeded with a REALISTIC machine
          // state value. A random 32-bit word in MSR turns address
          // translation off and arms exceptions the guest never asked for;
          // what follows is a divergence between two engines that are both
          // behaving correctly for the state they were given, and it says
          // nothing about the compiler.
          ins = (31u << 26) | (1u << 21) | (146u << 1);
        else if (g.primary == 31 && (g.xo == 339 || g.xo == 467))
        {
          // The SPR field is split and swapped.
          static const u32 kSprs[] = {8, 9, 26, 27, 272, 275, 912, 919, 1, 18};
          static const u32 kWritableSprs[] = {8, 9, 26, 27, 272, 275, 1, 18};
          const u32 spr = (g.xo == 339) ? kSprs[rnd() % (sizeof(kSprs) / sizeof(kSprs[0]))]
                                        : kWritableSprs[rnd() % (sizeof(kWritableSprs) / sizeof(kWritableSprs[0]))];
          ins = (31u << 26) | (d << 21) | ((spr & 0x1F) << 16) | (((spr >> 5) & 0x1F) << 11) |
                (g.xo << 1);
        }
        else if (g.primary == 31)
          ins = maybe_rc((31u << 26) | (d << 21) | (a << 16) | (b << 11) | (g.xo << 1),
                         (rnd() & 1) != 0);
        else if (g.primary == 4 && (g.xo == 6 || g.xo == 7 || g.xo == 38 || g.xo == 39))
          ins = (4u << 26) | (d << 21) | (a << 16) | (b << 11) | ((rnd() & 1) << 10) |
                ((rnd() % 5) << 7) | (g.xo << 1);
        else if (g.primary == 63 || g.primary == 4 || g.primary == 59)
        {
          const u32 c = rnd() % 32;
          const bool a_form = g.xo < 32 && g.xo != 0 &&
                              (g.primary == 4 || (g.xo != 12 && g.xo != 14 && g.xo != 15));
          if (a_form)
            ins = (g.primary << 26) | (d << 21) | (a << 16) | (b << 11) | (c << 6) | (g.xo << 1);
          else if (g.xo == 0 || g.xo == 32)
            ins = (g.primary << 26) | ((rnd() % 8) << 23) | (a << 16) | (b << 11) | (g.xo << 1);
          else
            ins = (g.primary << 26) | (d << 21) | (a << 16) | (b << 11) | (g.xo << 1);
          if ((rnd() & 1) && g.xo != 0 && g.xo != 32)
            ins |= 1u;
        }
        else if (g.primary >= 56 && g.primary <= 61)
          ins = (g.primary << 26) | (d << 21) | (a << 16) | ((rnd() & 1) << 15) |
                ((rnd() % 5) << 12) | (rnd() & 0x03F0);
        else if (g.primary == 19)
          ins = (19u << 26) | ((rnd() % 32) << 21) | ((rnd() % 32) << 16) | ((rnd() % 32) << 11) |
                (g.xo << 1);
        else if (g.primary == 31 && g.xo == 144)
          ins = (31u << 26) | (d << 21) | ((rnd() & 0xFF) << 12) | (g.xo << 1);
        else if (g.primary == 46 || g.primary == 47)
          ins = (g.primary << 26) | ((24 + rnd() % 8) << 21) | (a << 16) | (rnd() & 0x00F0);
        else if (g.primary == 16)
        {
          // A forward conditional branch, drawn across all six shapes of
          // BO: branch on a condition bit being true or false, the two
          // CTR-decrementing forms, and the two that test both. CTR is
          // seeded near zero below, so the decrement crosses the boundary
          // that decides the branch rather than always staying far from it.
          //
          // LK varies too, because a conditional branch-and-link that is
          // NOT taken must leave the link register alone -- with LK pinned
          // to zero no randomised case could ever have noticed that it did
          // not.
          static const u32 kBo[] = {12, 4, 16, 18, 0, 8};
          const u32 bo = kBo[rnd() % 6];
          ins = (16u << 26) | (bo << 21) | ((rnd() % 4) << 16) | 0x0008 | (rnd() & 1);
        }
        else
          ins = maybe_rc((g.primary << 26) | (d << 21) | (a << 16) | (rnd() & 0x03F0),
                         (g.primary == 21 || g.primary == 20 || g.primary == 23) &&
                             (rnd() & 1) != 0);
        prog.push_back(ins);
      }

      // A starting state both engines share, including memory.
      u32 init_gpr[32];
      for (int i = 0; i < 32; ++i)
        init_gpr[i] = (rnd() & 1) ? (rnd() & 0xFFFF) : safe_random(rnd());
      // Every register that might be used as a base points into one of the
      // scratch windows, so loads and stores stay inside memory the digest
      // covers -- and a third of them into the MEM2 window.
      for (int i = 1; i < 32; ++i)
        if (rnd() & 1)
          init_gpr[i] = ((rnd() % 3) == 0 ? kScratch2 : kScratch) + (rnd() % 64) * 4;
      const u8 init_ca = static_cast<u8>(rnd() & 1);
      const u8 init_so_ov = static_cast<u8>(rnd() & 3);
      const u16 init_sc = static_cast<u16>(rnd() & 0xFF7F);
      const bool init_reserve = (rnd() & 1) != 0;
      // r1 carries a machine state value an mtmsr can safely write: the one
      // the harness is already running under, with the interrupt-enable bit
      // varied so the path that takes a pending interrupt is exercised.
      init_gpr[1] = (state.msr.Hex & ~0x8000u) | ((rnd() & 1) ? 0x8000u : 0u);
      std::vector<u32> init_mem(kScratchWords * 2);
      for (u32 w = 0; w < kScratchWords; ++w)
      {
        init_mem[w] = rnd();
        init_mem[kScratchWords + w] = rnd();
      }

      // MSR is part of the starting state, and a block containing mtmsr
      // changes it. Restoring only registers and memory leaves the second
      // engine running under the machine state the first one left behind --
      // which showed up as the two disagreeing about a bit that neither
      // program had computed.
      //
      // Pinned once, outside the loop, rather than read live here: an
      // exception taken during a trial masks MSR down and clears DR, and a
      // per-trial read carries that forward so every later block refuses on
      // the entry guard. Measured in the chain differential, where it had
      // silently cut linked chains from 244 to 104.
      const u32 init_msr = kPristineMsr;
      const u32 init_ctr = rnd() % 4;
      seed_ps(rnd);
      PowerPC::PairedSingle init_ps[32];
      std::memcpy(init_ps, state.ps, sizeof(init_ps));
      const u32 init_fpscr =
          ((rnd() & 1) ? (rnd() & 0xF0000000u) : 0) | ((rnd() & 1) << 2) | (rnd() & 3);
      auto reset = [&]() {
        state.msr.Hex = init_msr;
        ppc.MSRUpdated();
        for (int i = 0; i < 32; ++i)
          state.gpr[i] = init_gpr[i];
        for (int i = 0; i < 8; ++i)
          state.cr.fields[i] = 0;
        state.spr[SPR_LR] = 0;
        // Small, so a bdnz in the program crosses zero rather than counting
        // down from a number it could never reach. Zero itself is included:
        // the decrement wraps to 0xffffffff, which a saturating
        // implementation would get wrong and no larger seed would expose.
        state.spr[SPR_CTR] = init_ctr;
        state.xer_ca = init_ca;
        state.xer_so_ov = init_so_ov;
        state.xer_stringctrl = init_sc;
        state.reserve = init_reserve;
        state.reserve_address = init_gpr[24] + init_gpr[16];  // a plausible lwarx address
        std::memcpy(state.ps, init_ps, sizeof(init_ps));
        state.fpscr.Hex = init_fpscr;
        state.downcount = static_cast<int>(kBudget);
        state.pc = state.npc = BASE;
        for (u32 w = 0; w < kScratchWords; ++w)
        {
          memory.Write_U32(init_mem[w], kScratch + w * 4);
          memory.Write_U32(init_mem[kScratchWords + w], kScratch2 + w * 4);
        }
      };

      for (u32 k = 0; k < prog.size(); ++k)
        memory.Write_U32(prog[k], BASE + k * 4);

      auto rr = ppc2wasm::CompileBlock(prog.data(), BASE, static_cast<u32>(prog.size()), layout);
      if (rr.instructions == 0 || rr.module.empty())
        continue;
      static int slot = -1;
      const int ix = InstallModuleAt(rr.module.data(), static_cast<int>(rr.module.size()), slot);
      if (ix < 0)
        continue;
      slot = ix;

      // Compiled first, so how far it got decides how far the interpreter
      // is allowed to run. A bail means it stopped early, and comparing
      // against an interpreter that ran the whole block would report a
      // difference that is the harness's fault.
      reset();
      for (int w = 0; w < 2; ++w)
        std::memcpy(init_wide[w].data(), wide[w], kWideBytes);
      const u32 ret = RunBlock(ix, state, ppc);
      for (int w = 0; w < 2; ++w)
        std::memcpy(jit_wide[w].data(), wide[w], kWideBytes);
      // How far it got comes from the DOWNCOUNT, not the returned PC.
      //
      // The PC said how much of a block had run only while a block had
      // exactly one exit, at its end. A block that continues past a
      // conditional branch has several, and a taken branch returns a
      // TARGET -- which, if it lands inside the block's own address range,
      // reads as an instruction count and is not one. That produced a
      // register mismatch that looked exactly like a compiler bug.
      //
      // Every exit charges the budget for what it ran: the epilogue from
      // the count the exiting path parked, a bail for the part before it,
      // mtmsr for itself. The delta covers all of them, which is why the
      // chain differential has always used it.
      const int ran_i = static_cast<int>(kBudget) - state.downcount;
      const bool bailed = (ret & 1u) != 0;
      const bool msr_changed = (ret & 2u) != 0;
      u32 jit_gpr[32];
      std::memcpy(jit_gpr, state.gpr, sizeof(jit_gpr));
      u64 jit_cr[8];
      std::memcpy(jit_cr, state.cr.fields, sizeof(jit_cr));
      const u32 jit_lr = state.spr[SPR_LR];
      const u8 jit_ca = state.xer_ca;
      const u8 jit_so_ov = state.xer_so_ov;
      const u16 jit_sc = state.xer_stringctrl;
      const bool jit_reserve = state.reserve;
      const u32 jit_reserve_address = state.reserve_address;
      const u32 jit_fpscr = state.fpscr.Hex;
      const u64 jit_mem = digest();
      PowerPC::PairedSingle jit_ps[32];
      std::memcpy(jit_ps, state.ps, sizeof(jit_ps));

      const u32 ran = ran_i < 0 ? 0u : static_cast<u32>(ran_i);

      reset();
      for (int w = 0; w < 2; ++w)
        std::memcpy(wide[w], init_wide[w].data(), kWideBytes);
      for (u32 k = 0; k < ran; ++k)
        ppc.SingleStep();
      const u64 int_mem = digest();
      const bool wide_ok = std::memcmp(jit_wide[0].data(), wide[0], kWideBytes) == 0 &&
                           std::memcmp(jit_wide[1].data(), wide[1], kWideBytes) == 0;

      ++blocks_run;
      bool ok = (jit_mem == int_mem) && wide_ok && (state.xer_ca == jit_ca) &&
                (state.xer_so_ov == jit_so_ov) && (state.xer_stringctrl == jit_sc) &&
                (state.reserve == jit_reserve) &&
                (!state.reserve || state.reserve_address == jit_reserve_address) &&
                (state.fpscr.Hex == jit_fpscr);
      int bad_ps = -1;
      for (int i = 0; i < 32; ++i)
        if (state.ps[i].PS0AsU64() != jit_ps[i].PS0AsU64() ||
            state.ps[i].PS1AsU64() != jit_ps[i].PS1AsU64())
        {
          ok = false;
          if (bad_ps < 0)
            bad_ps = i;
        }
      int bad = -1;
      for (int i = 0; i < 32; ++i)
        if (state.gpr[i] != jit_gpr[i])
        {
          ok = false;
          if (bad < 0)
            bad = i;
        }
      for (int i = 0; i < 8; ++i)
        if (CrBits(state.cr.fields[i]) != CrBits(jit_cr[i]))
          ok = false;
      if (state.spr[SPR_LR] != jit_lr)
        ok = false;

      if (!ok)
      {
        ++block_fail;
        std::printf("  block of %u (%u ran%s):", static_cast<u32>(prog.size()), ran,
                    bailed ? ", bailed" : "");
        for (u32 k = 0; k < prog.size() && k < 40; ++k)
          std::printf(" %08x", prog[k]);
        std::printf("\n");
        if (bad >= 0)
          std::printf("      r%-2d interpreter=%08x recompiled=%08x\n", bad, state.gpr[bad],
                      jit_gpr[bad]);
        if (jit_mem != int_mem)
          std::printf("      memory digests differ\n");
        for (int w = 0; w < 2 && !wide_ok; ++w)
          for (u32 o = 0; o < kWideBytes; ++o)
            if (jit_wide[w][o] != wide[w][o])
            {
              std::printf("      memory differs from %08x: interpreter=%02x recompiled=%02x\n",
                          (w ? kScratch2 : kScratch) + o, wide[w][o], jit_wide[w][o]);
              break;
            }
        if (state.xer_ca != jit_ca || state.xer_so_ov != jit_so_ov || state.xer_stringctrl != jit_sc)
          std::printf("      XER interpreter=ca%u so_ov%u sc%04x recompiled=ca%u so_ov%u sc%04x\n",
                      state.xer_ca, state.xer_so_ov, state.xer_stringctrl, jit_ca, jit_so_ov, jit_sc);
        if (state.fpscr.Hex != jit_fpscr)
          std::printf("      FPSCR interpreter=%08x recompiled=%08x\n", state.fpscr.Hex, jit_fpscr);
        if (bad_ps >= 0)
          std::printf("      f%-2d interpreter=%016llx/%016llx recompiled=%016llx/%016llx\n", bad_ps,
                      static_cast<unsigned long long>(state.ps[bad_ps].PS0AsU64()),
                      static_cast<unsigned long long>(state.ps[bad_ps].PS1AsU64()),
                      static_cast<unsigned long long>(jit_ps[bad_ps].PS0AsU64()),
                      static_cast<unsigned long long>(jit_ps[bad_ps].PS1AsU64()));
        for (int i = 0; i < 8; ++i)
          if (CrBits(state.cr.fields[i]) != CrBits(jit_cr[i]))
            std::printf("      cr%d interpreter=%016llx recompiled=%016llx\n", i,
                        static_cast<unsigned long long>(state.cr.fields[i]),
                        static_cast<unsigned long long>(jit_cr[i]));
        if (state.spr[SPR_LR] != jit_lr)
          std::printf("      lr interpreter=%08x recompiled=%08x\n", state.spr[SPR_LR], jit_lr);
        std::printf("      returned %08x\n", ret);
      }
    }
    char lbl[128];
    std::snprintf(lbl, sizeof(lbl), "randomised block differential (%d blocks)", blocks_run);
    check(block_fail == 0, lbl);
  }

  // ── randomised CHAIN differential ──
  //
  // Everything above compiles with linking ENABLED against an EMPTY link
  // table, so every lookup misses and no block ever tail-calls another. What
  // that checks is that the epilogue does not change what a single block
  // computes. It never executes a chain -- and linking is the default now,
  // doing 95% of the work in a real boot. Until this existed, the only
  // evidence that chains run correctly was that the boot completed.
  //
  // So: lay several blocks out back to back, publish each in the link table
  // exactly as the dispatcher does, and enter the first. Control then leaves
  // wasm only when the budget runs out, a lookup misses, a guard refuses or
  // MSR is written -- the same four exits the emulator sees.
  //
  // How far the chain got is read from the DOWNCOUNT, not from the returned
  // PC. Every exit accounts for itself there -- the epilogue once per block,
  // a bail for the part of a block that ran, mtmsr for itself -- so the
  // budget delta is the one number that covers all four. The returned PC
  // cannot: it says where the chain stopped, not how many blocks it crossed.
  {
    std::printf("\n  randomised chain differential\n");

    // Chains run with the two dispatcher-facing options the boot uses:
    // mtmsr continuing when the write has no consequence, and busy-wait
    // loops exiting through the idle flag. Neither changes what a chain
    // computes -- an idle exit stops after charging what ran, and the
    // interpreter is run for exactly that many instructions -- so the
    // comparison below is the check that they do not.
    layout.exceptions_offset = static_cast<u32>(offsetof(PowerPC::PowerPCState, Exceptions));
    layout.pagetable_pending_offset =
        static_cast<u32>(offsetof(PowerPC::PowerPCState, pagetable_update_pending));
    layout.mtmsr_continue = true;
    layout.hatch_mmio = false;
    static u32 idle_flag = 0;
    layout.idle_flag_addr = static_cast<u32>(reinterpret_cast<uintptr_t>(&idle_flag));

    // Clear of the code the earlier sections wrote, and of their scratch.
    constexpr u32 kChainBase = 0x8000'2000;
    constexpr u32 kScratch = 0x8002'0000;
    // Wide enough to cover every address the generator can produce: a base
    // register up to 47 words into the window plus the largest displacement
    // it emits. A digest that stopped short would read as agreement on the
    // stores that landed past its end.
    constexpr u32 kScratchWords = 512;

    // The link counters the epilogue bumps. These are the evidence that this
    // test did what it claims: a chain differential whose lookups all missed
    // would pass in exactly the same silence as one that worked.
    u32 chain_stats[4] = {0, 0, 0, 0};
    layout.link_stats_addr = static_cast<u32>(reinterpret_cast<uintptr_t>(chain_stats));

    // Instructions that never END a block, so a block's length is decided
    // here rather than by what the generator happened to draw. Blocks of a
    // known, fixed length are what make a branch target computable: the
    // chain only links if the target is a block ENTRY, and a generator that
    // did not know where entries were would almost never produce one.
    struct Gen { u32 primary; u32 xo; };
    static const Gen mid[] = {
        {14, 0}, {15, 0}, {7, 0},  {24, 0}, {26, 0}, {21, 0},
        {11, 0}, {10, 0},
        {32, 0}, {34, 0}, {40, 0}, {36, 0}, {38, 0}, {44, 0},
        {31, 266}, {31, 40}, {31, 444}, {31, 28}, {31, 316},
        {31, 24}, {31, 536}, {31, 954}, {31, 922}, {31, 104},
        {31, 26}, {31, 60}, {31, 124}, {31, 235},
        {31, 23}, {31, 151}, {31, 87}, {31, 215}, {31, 83},
    };

    // ── one pristine machine state, captured ONCE ──
    //
    // Every trial runs mtmsr and then CheckExceptions, and an external
    // interrupt taken there masks MSR down -- clearing IR and DR. Reading
    // the starting MSR from live state at the top of each trial therefore
    // carries that damage forward: from the first taken exception onward,
    // DR is clear, the per-block entry guard refuses every memory-touching
    // block before its first instruction, and every chain ends immediately.
    //
    // That is not hypothetical. It was happening: 240 of 300 chains were
    // exiting on a bail and only 104 linked at all, while the suite
    // reported PASSED -- the three coverage assertions were satisfied by
    // counters accumulated before the damage. A differential that quietly
    // stops testing is the exact failure this section exists to prevent,
    // so the machine state is pinned here and restored from the pin.
    state.msr.Hex = kPristineMsr;
    ppc.MSRUpdated();

    u32 seed = 0x5EED'C4A1;
    auto rnd = [&seed]() {
      seed ^= seed << 13;
      seed ^= seed >> 17;
      seed ^= seed << 5;
      return seed;
    };

    auto digest = [&]() {
      u64 h = 0xcbf29ce484222325ULL;
      for (u32 w = 0; w < kScratchWords; ++w)
      {
        const u32 v = memory.Read_U32(kScratch + w * 4);
        for (int by = 0; by < 4; ++by)
        {
          h ^= (v >> (by * 8)) & 0xFF;
          h *= 0x100000001b3ULL;
        }
      }
      return h;
    };

    // Registers are given ROLES, and the generator respects them:
    //
    //   r1        the machine state value an mtmsr may safely write
    //   r2-r15    results -- the only registers any instruction WRITES
    //   r16-r22   small non-negative indices for the indexed forms
    //   r23       a base in the hardware registers, which the guard must always refuse
    //   r24-r31   bases inside the scratch window
    //
    // The first version drew every field at random, as the block
    // differential does, and measured as a far weaker test than it looked:
    // two chains in three died on the MEM1 guard before reaching a single
    // lookup, and 300 chains produced four link misses between them.
    //
    // The reason is worth writing down, because it is an easy decode slip to
    // repeat. In PowerPC the destination is NOT always the rD field: ori,
    // xori, rlwinm and all the logical forms of opcode 31 write rA, the
    // field the other instructions use as a SOURCE. Drawing rA at random
    // therefore clobbered the very registers that were keeping addresses
    // inside MEM1. Writes are confined to r2-r15 here for that reason.
    //
    // r23 keeps bails present rather than banishing them: a chain that
    // refuses partway through has to account for the blocks that already
    // ran, and that accounting is only reachable with a guard that fires.
    //
    // It is placed ONCE PER TRIAL, in one chain in four, rather than drawn
    // per instruction. Per instruction the probability compounds along the
    // chain -- with chains reaching thirty blocks, a one-in-sixteen base
    // made most chains die on the guard, and the rate swung from 32 bails
    // in 300 chains to 231 on nothing more than a reshuffle of the random
    // stream. A test whose coverage moves that far for that reason is not
    // measuring what it claims.
    auto is_mem_ins = [](u32 ins) {
      const u32 primary = ins >> 26;
      if (primary == 31)
      {
        const u32 xo = (ins >> 1) & 0x3FF;
        return xo == 23 || xo == 151 || xo == 87 || xo == 215;
      }
      return primary == 32 || primary == 34 || primary == 40 || primary == 36 ||
             primary == 38 || primary == 44;
    };

    auto gen_mid = [&]() {
      const Gen& g = mid[rnd() % (sizeof(mid) / sizeof(mid[0]))];
      const bool mem = is_mem_ins((g.primary << 26) | (g.xo << 1));
      const u32 a = mem ? (24 + rnd() % 8) : (2 + rnd() % 14);
      const u32 b = mem ? (16 + rnd() % 7) : (rnd() % 31 + 1);
      const u32 d = 2 + rnd() % 14;
      const bool want_rc = (rnd() & 1) != 0;
      auto rc = [&](u32 i) {
        return (want_rc && ppc2wasm::detail::RcDestination(i | 1u) >= 0) ? (i | 1u) : i;
      };
      if (g.primary == 31)
        return rc((31u << 26) | (d << 21) | (a << 16) | (b << 11) | (g.xo << 1));
      return rc((g.primary << 26) | (d << 21) | (a << 16) | (rnd() & 0x03F0));
    };

    int fails = 0, chains_run = 0, chains_linked = 0, longest = 0;
    int exit_bail = 0, exit_msr = 0, exit_trap = 0;
    u64 total_tail_calls = 0;
    // Misses against an OCCUPIED slot belonging to someone else, and misses
    // against an empty one. Both are counted because they prove different
    // things: the first that the stored PC is checked, the second that an
    // ordinary end-of-chain still works. Asserting only the total would let
    // either disappear.
    u64 miss_decoy = 0, miss_empty = 0;
    // One run of six consecutive table slots, reused by every trial.
    int chain_slot_base = -1;
    u64 direct_calls_patched = 0;
    int direct_chains = 0, indirect_chains = 0;
    // Chains split over two modules, the second's exits to the first bound
    // as imports (the dispatcher's way to an earlier block), and the holes
    // so bound.
    int import_chains = 0;
    u64 import_calls_patched = 0;

    for (int trial = 0; trial < 800 && fails < 4; ++trial)
    {
      // The stream state at trial entry, so a failure can be re-run. rnd()
      // is consumed a variable number of times per trial, so the state at
      // trial N is not derivable without replaying every earlier one.
      const u32 trial_seed = seed;
      const bool direct_trial = (rnd() & 1) != 0;
      const u32 K = 2 + rnd() % 4;         // instructions per block
      const u32 nblocks = 2 + rnd() % 5;   // blocks laid end to end

      // The LAST instruction of each block is what decides where the chain
      // goes next. Every form here targets a block entry, because a target
      // that is not an entry is a lookup miss -- correct, but it ends the
      // chain and tests nothing new.
      std::vector<u32> prog(K * nblocks);
      for (u32 i = 0; i < nblocks; ++i)
      {
        for (u32 k = 0; k + 1 < K; ++k)
          prog[i * K + k] = gen_mid();

        const u32 this_pc = kChainBase + (i * K + K - 1) * 4;
        const u32 target = kChainBase + (rnd() % nblocks) * K * 4;
        const u32 disp = target - this_pc;
        u32 term;
        switch (rnd() % 10)
        {
        case 0:
        case 1:
          term = gen_mid();  // falls through into the next block
          break;
        case 2:
          term = (18u << 26) | (disp & 0x03FF'FFFC);  // b
          break;
        case 3:
          // bl, which also leaves the NEXT block's entry in the link
          // register -- so a later blr lands on an entry too, and the chain
          // follows a call and its return the way real code does.
          term = (18u << 26) | (disp & 0x03FF'FFFC) | 1u;
          break;
        case 4:
          // LK varies here too. When a bcl IS taken its link write lands on
          // the next block's entry, so the chain stays linkable and a later
          // blr still finds a target.
          term = (16u << 26) | (12u << 21) | ((rnd() % 4) << 16) | (disp & 0xFFFC) | (rnd() & 1);
          break;
        case 5:
          // The same branch testing the bit as FALSE, so both edges of a
          // conditional appear: one goes to the target, the other falls
          // through, and both must be link targets.
          term = (16u << 26) | (4u << 21) | ((rnd() % 4) << 16) | (disp & 0xFFFC) | (rnd() & 1);
          break;
        case 6:
          term = 0x4E80'0020 | (rnd() & 1);  // blr / blrl
          break;
        case 7:
          term = 0x4E80'0420;  // bcctr
          break;
        case 8:
          // bdnz / bdz: the back edge of a counted loop, now that it
          // compiles. CTR starts at a block address here, so bdnz always
          // branches and bdz never does -- which is the point: a compiled
          // back edge lets one chain span many iterations, and that is the
          // behaviour the whole change exists to produce.
          term = (16u << 26) | (((rnd() & 1) ? 16u : 18u) << 21) | (disp & 0xFFFC);
          break;
        default:
          // mtmsr r1 ends the chain with the MSR marker. The emulator's
          // dispatcher has to unwind a chain from that exit, and it is the
          // one exit that is neither a bail nor a budget expiry.
          term = (31u << 26) | (1u << 21) | (146u << 1);
          break;
        }
        prog[i * K + K - 1] = term;
      }

      // One trap in one chain in eight: twi 31,r0,0 always traps. The
      // compiler has no translation for it, so the hatch runs it, the
      // interpreter takes the program exception, and the block leaves
      // through the hatch's exit -- the one exit that charges the budget
      // for the hatched instruction itself, which nothing else generated
      // here reaches.
      if ((rnd() % 8) == 0)
        prog[rnd() % static_cast<u32>(prog.size())] = 0x0FE0'0000;

      // One guard refusal per trial, in one chain in four: rewrite the base
      // of a single memory instruction to r23, which points at MMIO.
      if ((rnd() % 4) == 0)
      {
        const u32 from = rnd() % static_cast<u32>(prog.size());
        for (u32 k = 0; k < prog.size(); ++k)
        {
          const u32 at = (from + k) % static_cast<u32>(prog.size());
          if (is_mem_ins(prog[at]))
          {
            prog[at] = (prog[at] & ~(0x1Fu << 16)) | (23u << 16);
            break;
          }
        }
      }

      u32 init_gpr[32];
      for (int i = 0; i < 32; ++i)
        init_gpr[i] = (rnd() & 1) ? (rnd() & 0xFFFF) : safe_random(rnd());
      for (int i = 16; i < 23; ++i)
        init_gpr[i] = (rnd() % 64) * 4;  // indices
      // The hardware registers: not memory, always refused. (This was MEM2
      // until compiled code learned to reach it through the region table.)
      init_gpr[23] = 0xCC00'0000 + (rnd() % 64) * 4;
      for (int i = 24; i < 32; ++i)
        init_gpr[i] = kScratch + (rnd() % 48) * 4;  // bases
      // EE varies so an mtmsr sometimes enables interrupts; FE0 varies so
      // an mtmsr sometimes flips a bit the block cannot handle itself and
      // has to exit on. With mtmsr_continue, a write that changes neither
      // simply continues the chain, and both shapes must stay covered.
      init_gpr[1] = (kPristineMsr & ~0x8800u) | ((rnd() & 1) ? 0x8000u : 0u) |
                    ((rnd() % 4) == 0 ? 0x0800u : 0u);
      // LR and CTR start at block entries, so blr and bcctr are chain steps
      // rather than immediate misses.
      const u32 init_lr = kChainBase + (rnd() % nblocks) * K * 4;
      const u32 init_ctr = kChainBase + (rnd() % nblocks) * K * 4;

      std::vector<u32> init_mem(kScratchWords);
      for (u32 w = 0; w < kScratchWords; ++w)
      {
        init_mem[w] = rnd();
        memory.Write_U32(init_mem[w], kScratch + w * 4);
      }

      // Small, so a chain that loops back on itself stops quickly -- and so
      // the budget exit is reached often rather than as a rarity.
      const u32 budget = 6 + rnd() % 60;
      const u32 init_msr = kPristineMsr;
      // FPSCR too, as the block differential seeds it. Left unreset, the
      // value the last block trial happened to leave -- FEX among its
      // bits -- decided whether an mtmsr here that sets FE0 raises the
      // floating-point program exception, and which trials that fell on
      // moved whenever the random stream before this section changed.
      const u32 init_fpscr =
          ((rnd() & 1) ? (rnd() & 0xF0000000u) : 0) | ((rnd() & 1) << 2) | (rnd() & 3);

      auto reset = [&]() {
        state.msr.Hex = init_msr;
        state.fpscr.Hex = init_fpscr;
        ppc.MSRUpdated();
        for (int i = 0; i < 32; ++i)
          state.gpr[i] = init_gpr[i];
        for (int i = 0; i < 8; ++i)
          state.cr.fields[i] = 0;
        state.spr[SPR_LR] = init_lr;
        state.spr[SPR_CTR] = init_ctr;
        // XER is read by every compare (SO folds into the result) and
        // written by the carry forms. Nothing in the pool writes it today,
        // so both engines happen to read the same stale value -- which
        // makes this free now and load-bearing the moment addc/adde/subfe
        // join the pool, which is the next planned widening.
        state.xer_ca = 0;
        state.xer_so_ov = 0;
        state.downcount = static_cast<int>(budget);
        state.pc = state.npc = kChainBase;
        for (u32 w = 0; w < kScratchWords; ++w)
          memory.Write_U32(init_mem[w], kScratch + w * 4);
        // The CODE is restored too, not just registers and scratch. A random
        // register used as a store base can land on the program, and then
        // the second engine would be running a different one from the first
        // -- a divergence that is entirely the harness's.
        for (size_t k = 0; k < prog.size(); ++k)
          memory.Write_U32(prog[k], kChainBase + static_cast<u32>(k) * 4);
        // addi r30,r30,1 at the program-exception vector, where a trap
        // (above) sends the PC. The replay steps the interpreter as many
        // times as the chain charged; one step too many then shows in r30
        // instead of re-raising the exception on an empty word unseen.
        memory.Write_U32(0x3BDE'0001, 0x8000'0700);
      };

      reset();

      // Compile every block before publishing any of them, into ONE
      // module, exactly as the dispatcher's region compiler does.
      //
      // One module is what makes a direct call possible: `return_call`
      // names a function index, and an index only exists within a module.
      // Installing a block at a time -- which this used to do -- cannot
      // reach that path at all.
      //
      // allow_direct alternates per trial so both halves stay covered. A
      // chain with it ON takes direct calls between its blocks and the
      // table lookup only where a target is outside the module, which is
      // the end of the chain; a chain with it OFF is the mechanism that
      // shipped before this and still has to work.
      layout.allow_direct = direct_trial;

      std::vector<int> index(nblocks, -1);
      std::vector<ppc2wasm::Bytes> bodies(nblocks);
      std::vector<u32> block_locals(nblocks, 0), block_locals_i64(nblocks, 0),
          block_locals_v128(nblocks, 0), block_locals_tail(nblocks, 0);
      std::vector<std::vector<ppc2wasm::DirectFixup>> block_fixups(nblocks);
      std::vector<std::vector<ppc2wasm::BranchHint>> block_hints(nblocks);
      bool all_compiled = true;
      for (u32 i = 0; i < nblocks && all_compiled; ++i)
      {
        auto rb = ppc2wasm::CompileBlock(&prog[i * K], kChainBase + i * K * 4, K, layout, false);
        if (rb.instructions != K || rb.body.empty())
          all_compiled = false;
        else
        {
          bodies[i] = std::move(rb.body);
          block_locals[i] = rb.locals;
          block_locals_i64[i] = rb.locals_i64;
          block_locals_v128[i] = rb.locals_v128;
          block_locals_tail[i] = rb.locals_tail;
          block_fixups[i] = std::move(rb.fixups);
          block_hints[i] = std::move(rb.hints);
        }
      }
      if (!all_compiled)
        continue;

      // Function index i is block i, so a hole naming block i's start PC
      // resolves to i. Targets outside the chain -- the address past the
      // end, an LR or CTR value -- stay nops and fall back to the table.
      // One module holding blocks [first, last), installed at table slot
      // `slot`. A hole naming a sibling resolves to its function index
      // (its position plus the imports before it); a hole naming a block
      // of an EARLIER module -- one already installed, below `first` --
      // is bound as an import of this module, as the dispatcher binds a
      // region's exits to blocks compiled before it; anything else (the
      // address past the end, an LR or CTR value) stays nops and falls
      // back to the table.
      u32 trial_direct = 0, trial_imports = 0;
      auto build_and_install = [&](u32 first, u32 last, int slot) -> int {
        ppc2wasm::ModuleBuilder cmb;
        cmb.ImportMemory(layout.memory_pages, layout.memory_pages, layout.memory_shared);
        cmb.ImportTable();
        cmb.ImportFunction("env", "ppc_interp_op",
                           {ppc2wasm::kI32, ppc2wasm::kI32, ppc2wasm::kI32}, {ppc2wasm::kI32});
        if (layout.hatch_exit_index)
        {
          cmb.ImportFunction("env", "ppc_hatch_exit",
                             {ppc2wasm::kI32, ppc2wasm::kI32, ppc2wasm::kI32, ppc2wasm::kI32},
                             {ppc2wasm::kI32});
          cmb.ImportFunction("env", "ppc_mem_slow",
                             {ppc2wasm::kI32, ppc2wasm::kI32, ppc2wasm::kI32, ppc2wasm::kI32,
                              ppc2wasm::kI32},
                             {ppc2wasm::kI32});
        }
        cmb.SetSignature({ppc2wasm::kI32}, {ppc2wasm::kI32});
        const int host_imports = static_cast<int>(cmb.ImportCount());
        std::vector<u32> import_blocks;  // block number per import, in order
        std::vector<int> import_slots;
        for (u32 i = first; i < last; ++i)
          for (const ppc2wasm::DirectFixup& f : block_fixups[i])
            for (u32 j = 0; j < first; ++j)
              if (kChainBase + j * K * 4 == f.target &&
                  std::find(import_blocks.begin(), import_blocks.end(), j) == import_blocks.end())
              {
                import_blocks.push_back(j);
                import_slots.push_back(index[j]);
                cmb.ImportBlock("b" + std::to_string(import_blocks.size() - 1));
              }
        const int import_count = static_cast<int>(cmb.ImportCount());
        auto index_of = [&](u32 target) -> int {
          for (u32 i = first; i < last; ++i)
            if (kChainBase + i * K * 4 == target)
              return static_cast<int>(i - first) + import_count;
          for (size_t k = 0; k < import_blocks.size(); ++k)
            if (kChainBase + import_blocks[k] * K * 4 == target)
            {
              ++trial_imports;
              return host_imports + static_cast<int>(k);
            }
          return -1;
        };
        for (u32 i = first; i < last; ++i)
        {
          trial_direct += ppc2wasm::PatchDirectCalls(bodies[i], block_fixups[i], index_of);
          cmb.AddFunction(bodies[i], "f" + std::to_string(i - first), block_locals[i],
                          block_locals_i64[i], block_locals_v128[i], block_locals_tail[i],
                          block_hints[i]);
        }
        const auto module = cmb.Build();
        if (import_blocks.empty())
          return InstallBatchAt(module.data(), static_cast<int>(module.size()),
                                static_cast<int>(last - first), slot, 6);
        return InstallBatchImportsAt(module.data(), static_cast<int>(module.size()),
                                     static_cast<int>(last - first), slot, import_slots.data(),
                                     static_cast<int>(import_slots.size()));
      };
      // A third of the trials split the chain over two modules, once the
      // slots exist, so the second's calls to the first go through imports.
      const u32 split = (chain_slot_base >= 0 && nblocks >= 2 && (trial % 3) == 1) ? nblocks / 2 : 0;
      int base = -1;
      if (split == 0)
      {
        base = build_and_install(0, nblocks, chain_slot_base);
      }
      else
      {
        base = build_and_install(0, split, chain_slot_base);
        if (base >= 0)
        {
          for (u32 i = 0; i < split; ++i)
            index[i] = base + static_cast<int>(i);
          if (build_and_install(split, nblocks, base + static_cast<int>(split)) < 0)
            base = -1;
        }
      }
      if (base < 0)
        continue;
      chain_slot_base = base;
      for (u32 i = 0; i < nblocks; ++i)
        index[i] = base + static_cast<int>(i);
      direct_calls_patched += trial_direct - trial_imports;
      import_calls_patched += trial_imports;
      if (split)
        ++import_chains;
      if (direct_trial)
        ++direct_chains;
      else
        ++indirect_chains;

      for (u32 i = 0; i < nblocks; ++i)
      {
        const u32 pc = kChainBase + i * K * 4;
        const u32 slot = (pc >> 2) & layout.link_mask;
        link_table[slot * 2 + 0] = pc;
        link_table[slot * 2 + 1] = static_cast<u32>(index[i]);
      }

      // Poison the slot a chain reaches by running off the end, with an
      // entry for a DIFFERENT pc pointing at block 0.
      //
      // Without this every miss in this test is a miss against an empty
      // slot, and "the stored PC is checked, not assumed" stays an
      // untested claim. The table is direct-mapped, so the case that
      // actually occurs in the emulator is an OCCUPIED slot belonging to
      // someone else -- and the consequence of accepting it is not a slow
      // chain but a jump into another block's code.
      //
      // It matters now rather than later: the next optimisation the handoff
      // ranks is associativity or a better hash for exactly this table, and
      // this is the invariant that work must not break.
      // Half the trials, so the OTHER half still exercises a miss against
      // an empty slot -- the ordinary end of a chain. With the decoy always
      // present, every miss in this section was a decoy miss and the plain
      // case had no coverage at all.
      const bool poisoned = (rnd() & 1) != 0;
      const u32 past_end = kChainBase + nblocks * K * 4;
      const u32 decoy = (past_end >> 2) & layout.link_mask;
      if (poisoned)
      {
        link_table[decoy * 2 + 0] = past_end + 4;  // not the pc that will be looked up
        link_table[decoy * 2 + 1] = static_cast<u32>(index[0]);
      }

      const u32 hits_before = chain_stats[0];
      const u32 misses_before = chain_stats[3];

      // Enter the chain. Not RunBlock: the budget has to be read before
      // anything else touches guest state, because the MSR fix-up runs the
      // interpreter's exception path and that is free to write it.
      const u32 ret = reinterpret_cast<BlockFn>(index[0])(
          static_cast<u32>(reinterpret_cast<uintptr_t>(&state)));
      const int ran = static_cast<int>(budget) - state.downcount;
      const bool bailed = (ret & 1u) != 0;
      const bool msr_changed = (ret & 2u) != 0;
      state.pc = state.npc = ret & ~3u;
      if (msr_changed)
      {
        // As the dispatcher and RunBlock do: Interpreter::mtmsr's steps
        // after the write, FP exception check included.
        ppc.MSRUpdated();
        CheckFPExceptions(state);
        ppc.CheckExceptions();
        state.pc = state.npc;
      }

      const u32 tail_calls = chain_stats[0] - hits_before;
      (poisoned ? miss_decoy : miss_empty) += chain_stats[3] - misses_before;
      u32 jit_gpr[32];
      std::memcpy(jit_gpr, state.gpr, sizeof(jit_gpr));
      u64 jit_cr[8];
      std::memcpy(jit_cr, state.cr.fields, sizeof(jit_cr));
      const u32 jit_lr = state.spr[SPR_LR];
      const u32 jit_ctr = state.spr[SPR_CTR];
      const u32 jit_pc = state.pc;
      const u8 jit_ca = state.xer_ca, jit_so = state.xer_so_ov;
      const u64 jit_mem = digest();

      // Unpublish before the interpreter runs. The table is shared with
      // every other trial, and a stale entry pointing at a retired module is
      // how a chain ends up executing code from a program that no longer
      // exists.
      for (u32 i = 0; i < nblocks; ++i)
      {
        const u32 slot = ((kChainBase + i * K * 4) >> 2) & layout.link_mask;
        link_table[slot * 2 + 0] = 0;
        link_table[slot * 2 + 1] = 0;
      }
      if (poisoned)
      {
        link_table[decoy * 2 + 0] = 0;
        link_table[decoy * 2 + 1] = 0;
      }

      ++chains_run;
      exit_bail += bailed ? 1 : 0;
      exit_msr += msr_changed ? 1 : 0;
      exit_trap += (jit_pc & 0x0FFFFFFFu) == 0x700 ? 1 : 0;
      total_tail_calls += tail_calls;
      if (tail_calls > 0)
        ++chains_linked;
      if (static_cast<int>(tail_calls) + 1 > longest)
        longest = static_cast<int>(tail_calls) + 1;

      // The budget is a SOFT limit: the epilogue subtracts a block's length
      // and only then tests, so the block that exhausts it has already run
      // in full and the chain overshoots by up to one block. That is the
      // emulator's behaviour too -- the dispatcher advances CoreTiming by
      // whatever the chain actually consumed, not by the slice it offered.
      // What would be a real defect is the budget moving backwards, or an
      // overshoot larger than one block, which would mean an exit path
      // accounts for itself wrongly.
      // To ENTER a block the previous epilogue must have left the budget at
      // one or more, so the largest correct overshoot is a full block minus
      // that one: budget + K - 1. Allowing budget + K admits an exit path
      // that over-charges by exactly one instruction.
      if (ran < 0 || ran > static_cast<int>(budget + K) - 1)
      {
        ++fails;
        std::printf("  chain of %u x %u: budget %u, downcount %d after\n", nblocks, K, budget,
                    state.downcount);
        continue;
      }

      reset();
      for (int k = 0; k < ran; ++k)
        ppc.SingleStep();
      const u64 int_mem = digest();

      bool ok = (jit_mem == int_mem) && (jit_pc == state.pc);
      int bad = -1;
      for (int i = 0; i < 32; ++i)
        if (state.gpr[i] != jit_gpr[i])
        {
          ok = false;
          if (bad < 0)
            bad = i;
        }
      int bad_cr = -1;
      for (int i = 0; i < 8; ++i)
        if (CrBits(state.cr.fields[i]) != CrBits(jit_cr[i]))
        {
          ok = false;
          if (bad_cr < 0)
            bad_cr = i;
        }
      if (state.spr[SPR_LR] != jit_lr || state.spr[SPR_CTR] != jit_ctr)
        ok = false;
      if (state.xer_ca != jit_ca || state.xer_so_ov != jit_so)
        ok = false;

      if (!ok)
      {
        ++fails;
        std::printf("  trial %d seed %08x budget %u: chain %u blocks x %u insts, "
                    "%u tail calls, %d ran%s%s:\n",
                    trial, trial_seed, budget, nblocks, K, tail_calls, ran,
                    bailed ? ", bailed" : "", msr_changed ? ", msr" : "");
        for (size_t k = 0; k < prog.size() && k < 12; ++k)
          std::printf("      [%08x] %08x\n", kChainBase + static_cast<u32>(k) * 4, prog[k]);
        if (bad >= 0)
          std::printf("      r%-2d interpreter=%08x chained=%08x\n", bad, state.gpr[bad],
                      jit_gpr[bad]);
        if (bad_cr >= 0)
          std::printf("      cr%d interpreter=%016llx chained=%016llx\n", bad_cr,
                      static_cast<unsigned long long>(state.cr.fields[bad_cr]),
                      static_cast<unsigned long long>(jit_cr[bad_cr]));
        if (state.spr[SPR_LR] != jit_lr)
          std::printf("      lr  interpreter=%08x chained=%08x\n", state.spr[SPR_LR], jit_lr);
        if (state.spr[SPR_CTR] != jit_ctr)
          std::printf("      ctr interpreter=%08x chained=%08x\n", state.spr[SPR_CTR], jit_ctr);
        if (jit_pc != state.pc)
          std::printf("      pc  interpreter=%08x chained=%08x\n", state.pc, jit_pc);
        if (jit_mem != int_mem)
          std::printf("      memory digests differ\n");
      }
    }

    char lbl[160];
    std::snprintf(lbl, sizeof(lbl), "randomised chain differential (%d chains, longest %d blocks)",
                  chains_run, longest);
    check(fails == 0, lbl);
    // Coverage is asserted, not just printed. Every one of these can fall to
    // zero while the state comparisons stay green, and each has a plausible
    // way of doing so: break the epilogue's hash and nothing links; let the
    // guard accept MEM2 and no chain bails; keep chains from running off the
    // end and the stored-PC check goes untested. A silent collapse is the
    // failure mode this section exists to prevent, so it is a failure.
    check(chains_run > 200, "enough chains actually ran");
    check(total_tail_calls > 0, "chains actually tail-called into a successor");
    check(chain_stats[2] > 0, "at least one chain exited on the budget");
    check(miss_empty > 0, "a chain missed against an empty slot");
    check(miss_decoy > 0, "a chain rejected an occupied slot holding another pc");
    check(exit_bail > 0, "a chain bailed partway through");
    check(exit_msr > 0, "a chain exited on an mtmsr");
    check(exit_trap > 0, "a chain left through the hatch on a trap");
    // Direct calls are a second linking mechanism and need their own
    // floor. A chain whose holes all went unfilled behaves exactly like
    // one compiled without them, and every comparison here would stay
    // green while the mechanism this section exists to cover never ran.
    check(direct_calls_patched > 0, "chains made direct same-module calls");
    {
      char lbl[128];
      std::snprintf(lbl, sizeof(lbl),
                    "chains split over two modules called the first through imports (%d chains, "
                    "%llu calls)",
                    import_chains, static_cast<unsigned long long>(import_calls_patched));
      check(import_chains > 0 && import_calls_patched > 0, lbl);
    }
    check(direct_chains > 0 && indirect_chains > 0,
          "both the direct and the table-lookup paths were compiled");
    std::printf("    %d of %d chains linked   tail calls %llu\n", chains_linked, chains_run,
                static_cast<unsigned long long>(total_tail_calls));
    std::printf("    %d direct-capable chains, %d table-only, %llu holes filled\n",
                direct_chains, indirect_chains,
                static_cast<unsigned long long>(direct_calls_patched));
    std::printf("    hits %u  attempts %u  budget exits %u  misses %u\n", chain_stats[0],
                chain_stats[1], chain_stats[2], chain_stats[3]);
    // All four ways out of a chain, counted separately. A test can be green
    // and still have left one of them unvisited, and the only way to know
    // which is to count them rather than the passes.
    std::printf("    exits: trap %d\n", exit_trap);
    std::printf("    exits: budget %u  miss %u (empty %llu, decoy %llu)  bail %d  msr %d\n",
                chain_stats[2], chain_stats[3], static_cast<unsigned long long>(miss_empty),
                static_cast<unsigned long long>(miss_decoy), exit_bail, exit_msr);
    // The counters are live host memory baked into every module compiled
    // while this is set. Clearing it keeps a later section from emitting
    // stores into this scope's dead stack.
    layout.link_stats_addr = 0;
  }

  std::printf("\n%s (%d failures)\n", g_failures ? "FAILED" : "PASSED", g_failures);
  std::fflush(stdout);
  return g_failures ? 1 : 0;
}
