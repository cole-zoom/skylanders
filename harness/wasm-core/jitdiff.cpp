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
    const u32 got_pc = f(static_cast<u32>(reinterpret_cast<uintptr_t>(&state)));

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
        const int ix = InstallModule(rr.module.data(), static_cast<int>(rr.module.size()));
        if (ix < 0)
          continue;

        std::memcpy(state.gpr, before, sizeof(before));
        for (int i = 0; i < 8; ++i)
          state.cr.fields[i] = 0;
        state.spr[SPR_LR] = 0;
        state.downcount = 1000;
        state.pc = state.npc = BASE;
        reinterpret_cast<BlockFn>(ix)(static_cast<u32>(reinterpret_cast<uintptr_t>(&state)));

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

  std::printf("\n%s (%d failures)\n", g_failures ? "FAILED" : "PASSED", g_failures);
  std::fflush(stdout);
  return g_failures ? 1 : 0;
}
