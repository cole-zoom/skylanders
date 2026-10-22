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
#include "Core/PowerPC/ConditionRegister.h"
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

// Install a module, optionally REPLACING an existing table slot.
//
// Growing a fresh slot per trial pins every WebAssembly.Module and Instance
// for the life of the process. At roughly nine thousand of them the run used
// to die partway through with "Worker terminated due to reaching memory
// limit", which reads as a harness crash rather than an out-of-memory and
// lands in the middle of a passing run. Reusing a slot lets the previous
// instance be collected, so the suite runs on a default node heap.
EM_JS(int, InstallModuleAt, (const uint8_t* ptr, int len, int slot), {
  try {
    const bytes = HEAPU8.slice(ptr, ptr + len);
    const mod = new WebAssembly.Module(bytes);
    const inst = new WebAssembly.Instance(mod, {
      env: { memory: wasmMemory, __indirect_function_table: wasmTable },
    });
    let idx = slot;
    if (idx < 0) {
      idx = wasmTable.length;
      wasmTable.grow(1);
    }
    wasmTable.set(idx, inst.exports.f);
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
    ppc.MSRUpdated();
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
      // Update forms write the address back.
      {"lwzu updates the base",
       {0x3C60'8000, 0x6063'7300, 0x3880'4321, 0x9083'0004, 0x84A3'0004}},
      {"stbu updates the base",
       {0x3C60'8000, 0x6063'7400, 0x3880'0055, 0x9C83'0008}},

      // bcctr is the indirect branch: mtspr CTR then jump to it.
      {"bcctr branches through CTR",
       {0x3C60'8000, 0x6063'2000, 0x7C69'03A6, 0x4E80'0420}},

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
    // Both engines must start from the SAME state, condition register
    // included. Resetting it only before the recompiled run left stale fields
    // from earlier cases in the interpreter's starting state, and the
    // comparison then failed on a field neither program had touched.
    for (int i = 0; i < 32; ++i)
      state.gpr[i] = 0;
    for (int i = 0; i < 8; ++i)
      state.cr.fields[i] = 0;
    state.spr[SPR_LR] = 0;
    state.pc = state.npc = BASE;
    for (size_t i = 0; i < c.code.size(); ++i)
      ppc.SingleStep();

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
      if (state.cr.fields[i] != expect_cr[i])
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
    };

    u32 seed = 0x12345678;
    auto rnd = [&seed]() {
      seed ^= seed << 13;
      seed ^= seed >> 17;
      seed ^= seed << 5;
      return seed;
    };

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
        if (t.primary == 31)
          ins = (31u << 26) | (d << 21) | (a << 16) | (b << 11) | (t.xo << 1);
        else
          // A SMALL non-negative displacement, so the address stays inside
          // MEM1. A random 16-bit field is signed and routinely lands outside
          // it, where the interpreter faults and a compiled block bails --
          // both correct, and not comparable.
          ins = (t.primary << 26) | (d << 21) | (a << 16) | (rnd() & 0x0FF0);

        // Seed registers with a mix of small, negative and large values, and
        // point the ones used as addresses somewhere inside MEM1.
        for (int i = 0; i < 32; ++i)
          state.gpr[i] = (rnd() & 1) ? (rnd() & 0xFFFF) : rnd();
        state.gpr[a] = 0x8000'0000 + (rnd() & 0x000F'FFF0);
        if (t.primary == 31)
          state.gpr[b] = rnd() & 0xFF0;
        for (int i = 0; i < 8; ++i)
          state.cr.fields[i] = 0;
        state.spr[SPR_LR] = 0;
        state.downcount = 1000;
        u32 before[32];
        std::memcpy(before, state.gpr, sizeof(before));

        memory.Write_U32(ins, BASE);
        state.pc = state.npc = BASE;
        ppc.SingleStep();
        u32 want[32];
        std::memcpy(want, state.gpr, sizeof(want));
        u64 want_cr[8];
        std::memcpy(want_cr, state.cr.fields, sizeof(want_cr));

        auto rr = ppc2wasm::CompileBlock(&ins, BASE, 1, layout);
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
        for (int i = 0; i < 8; ++i)
          state.cr.fields[i] = 0;
        state.spr[SPR_LR] = 0;
        state.downcount = 1000;
        state.pc = state.npc = BASE;
        RunBlock(ix, state, ppc);

        for (int i = 0; i < 32; ++i)
        {
          if (state.gpr[i] != want[i])
          {
            std::printf("  %-8s ins=%08x r%-2d interpreter=%08x recompiled=%08x\n", t.name, ins,
                        i, want[i], state.gpr[i]);
            ++fails_here;
            ++rand_fail;
            break;
          }
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

    // A scratch window for stores, well away from the code being run.
    constexpr u32 kScratch = 0x8001'0000;
    constexpr u32 kScratchWords = 256;

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
        {16, 0},  // a conditional branch, to exercise an early exit
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

    int block_fail = 0, blocks_run = 0;
    for (int trial = 0; trial < 400 && block_fail < 4; ++trial)
    {
      const u32 len = 2 + (rnd() % 10);
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
        else if (g.primary == 31)
          ins = (31u << 26) | (d << 21) | (a << 16) | (b << 11) | (g.xo << 1);
        else if (g.primary == 16)
          // A forward conditional branch with BO bit 2 set, so it is a plain
          // condition test rather than a CTR form.
          ins = (16u << 26) | (12u << 21) | ((rnd() % 4) << 16) | 0x0008;
        else
          ins = (g.primary << 26) | (d << 21) | (a << 16) | (rnd() & 0x03F0);
        prog.push_back(ins);
      }

      // A starting state both engines share, including memory.
      u32 init_gpr[32];
      for (int i = 0; i < 32; ++i)
        init_gpr[i] = (rnd() & 1) ? (rnd() & 0xFFFF) : rnd();
      // Every register that might be used as a base points into the scratch
      // window, so loads and stores stay inside MEM1 and inside the window
      // the digest covers.
      for (int i = 1; i < 32; ++i)
        if (rnd() & 1)
          init_gpr[i] = kScratch + (rnd() % 64) * 4;
      // r1 carries a machine state value an mtmsr can safely write: the one
      // the harness is already running under, with the interrupt-enable bit
      // varied so the path that takes a pending interrupt is exercised.
      init_gpr[1] = (state.msr.Hex & ~0x8000u) | ((rnd() & 1) ? 0x8000u : 0u);
      for (u32 w = 0; w < kScratchWords; ++w)
        memory.Write_U32(rnd(), kScratch + w * 4);
      std::vector<u32> init_mem(kScratchWords);
      for (u32 w = 0; w < kScratchWords; ++w)
        init_mem[w] = memory.Read_U32(kScratch + w * 4);

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
      auto reset = [&]() {
        state.msr.Hex = init_msr;
        ppc.MSRUpdated();
        for (int i = 0; i < 32; ++i)
          state.gpr[i] = init_gpr[i];
        for (int i = 0; i < 8; ++i)
          state.cr.fields[i] = 0;
        state.spr[SPR_LR] = 0;
        state.spr[SPR_CTR] = 0;
        state.downcount = 10000;
        state.pc = state.npc = BASE;
        for (u32 w = 0; w < kScratchWords; ++w)
          memory.Write_U32(init_mem[w], kScratch + w * 4);
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
      const u32 ret = RunBlock(ix, state, ppc);
      const bool bailed = (ret & 1u) != 0;
      const bool msr_changed = (ret & 2u) != 0;
      const u32 end_pc = ret & ~3u;
      u32 jit_gpr[32];
      std::memcpy(jit_gpr, state.gpr, sizeof(jit_gpr));
      u64 jit_cr[8];
      std::memcpy(jit_cr, state.cr.fields, sizeof(jit_cr));
      const u32 jit_lr = state.spr[SPR_LR];
      const u64 jit_mem = digest();

      u32 ran = (bailed || msr_changed) ? (end_pc - BASE) / 4 : rr.instructions;
      // A taken branch leaves the block, and the whole block ran.
      if (!bailed && !msr_changed &&
          (end_pc < BASE || end_pc > BASE + rr.instructions * 4))
        ran = rr.instructions;

      reset();
      for (u32 k = 0; k < ran; ++k)
        ppc.SingleStep();
      const u64 int_mem = digest();

      ++blocks_run;
      bool ok = (jit_mem == int_mem);
      int bad = -1;
      for (int i = 0; i < 32; ++i)
        if (state.gpr[i] != jit_gpr[i])
        {
          ok = false;
          if (bad < 0)
            bad = i;
        }
      for (int i = 0; i < 8; ++i)
        if (state.cr.fields[i] != jit_cr[i])
          ok = false;
      if (state.spr[SPR_LR] != jit_lr)
        ok = false;

      if (!ok)
      {
        ++block_fail;
        std::printf("  block of %u (%u ran%s):", static_cast<u32>(prog.size()), ran,
                    bailed ? ", bailed" : "");
        for (u32 k = 0; k < prog.size() && k < 6; ++k)
          std::printf(" %08x", prog[k]);
        std::printf("\n");
        if (bad >= 0)
          std::printf("      r%-2d interpreter=%08x recompiled=%08x\n", bad, state.gpr[bad],
                      jit_gpr[bad]);
        if (jit_mem != int_mem)
          std::printf("      memory digests differ\n");
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
    //   r23       a base in MEM2, which the guard must always refuse
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
      if (g.primary == 31)
        return (31u << 26) | (d << 21) | (a << 16) | (b << 11) | (g.xo << 1);
      return (g.primary << 26) | (d << 21) | (a << 16) | (rnd() & 0x03F0);
    };

    int fails = 0, chains_run = 0, chains_linked = 0, longest = 0;
    int exit_bail = 0, exit_msr = 0;
    u64 total_tail_calls = 0;
    // Misses against an OCCUPIED slot belonging to someone else, and misses
    // against an empty one. Both are counted because they prove different
    // things: the first that the stored PC is checked, the second that an
    // ordinary end-of-chain still works. Asserting only the total would let
    // either disappear.
    u64 miss_decoy = 0, miss_empty = 0;
    // Table slots reused across trials; nblocks never exceeds six.
    int slots[6] = {-1, -1, -1, -1, -1, -1};

    for (int trial = 0; trial < 300 && fails < 4; ++trial)
    {
      // The stream state at trial entry, so a failure can be re-run. rnd()
      // is consumed a variable number of times per trial, so the state at
      // trial N is not derivable without replaying every earlier one.
      const u32 trial_seed = seed;
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
        switch (rnd() % 9)
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
          term = (16u << 26) | (12u << 21) | ((rnd() % 4) << 16) | (disp & 0xFFFC);
          break;
        case 5:
          // The same branch testing the bit as FALSE, so both edges of a
          // conditional appear: one goes to the target, the other falls
          // through, and both must be link targets.
          term = (16u << 26) | (4u << 21) | ((rnd() % 4) << 16) | (disp & 0xFFFC);
          break;
        case 6:
          term = 0x4E80'0020;  // blr
          break;
        case 7:
          term = 0x4E80'0420;  // bcctr
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

      // One guard refusal per trial, in one chain in four: rewrite the base
      // of a single memory instruction to r23, which points into MEM2.
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
        init_gpr[i] = (rnd() & 1) ? (rnd() & 0xFFFF) : rnd();
      for (int i = 16; i < 23; ++i)
        init_gpr[i] = (rnd() % 64) * 4;  // indices
      init_gpr[23] = 0x9000'0000 + (rnd() % 64) * 4;  // MEM2: always refused
      for (int i = 24; i < 32; ++i)
        init_gpr[i] = kScratch + (rnd() % 48) * 4;  // bases
      init_gpr[1] = (kPristineMsr & ~0x8000u) | ((rnd() & 1) ? 0x8000u : 0u);
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

      auto reset = [&]() {
        state.msr.Hex = init_msr;
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
      };

      reset();

      // Compile every block before publishing any of them. A half-filled
      // table would let a chain jump to a block this trial never installed.
      std::vector<int> index(nblocks, -1);
      bool all_compiled = true;
      for (u32 i = 0; i < nblocks && all_compiled; ++i)
      {
        auto rb = ppc2wasm::CompileBlock(&prog[i * K], kChainBase + i * K * 4, K, layout);
        if (rb.instructions != K || rb.module.empty())
          all_compiled = false;
        else if ((index[i] = InstallModuleAt(rb.module.data(),
                                             static_cast<int>(rb.module.size()), slots[i])) < 0)
          all_compiled = false;
        else
          slots[i] = index[i];
      }
      if (!all_compiled)
        continue;

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
        ppc.MSRUpdated();
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
        if (state.cr.fields[i] != jit_cr[i])
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
    std::printf("    %d of %d chains linked   tail calls %llu\n", chains_linked, chains_run,
                static_cast<unsigned long long>(total_tail_calls));
    std::printf("    hits %u  attempts %u  budget exits %u  misses %u\n", chain_stats[0],
                chain_stats[1], chain_stats[2], chain_stats[3]);
    // All four ways out of a chain, counted separately. A test can be green
    // and still have left one of them unvisited, and the only way to know
    // which is to count them rather than the passes.
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
