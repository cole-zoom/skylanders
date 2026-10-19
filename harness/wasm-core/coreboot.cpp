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
#if defined(_M_GENERIC)
  // Only the wasm build is generic. The native oracle is built with the
  // AArch64 recompiler, so it legitimately offers one -- asserting the same
  // thing on both targets would be testing the wrong property.
  check(!has_jit, "no recompiler is offered (generic build)");
#else
  check(has_jit, "a recompiler is offered (native build)");
#endif
  // The test always runs the plain interpreter regardless of what the build
  // would pick by default, so the paired-single comparison below is between
  // the same code path on both targets.
  std::printf("  default core: %d (test forces Interpreter)\n",
              static_cast<int>(PowerPC::DefaultCPUCore()));

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

  // ── branches, condition register, shifts ──
  // The differential only covers what the test covers, so this widens it past
  // straight-line arithmetic into control flow and the CR -- the parts a
  // decoder is most likely to get subtly wrong.
  //
  //   subf   r10, r4, r3      0x7D441850   r10 = r3 - r4
  //   neg    r11, r3          0x7D6300D0   r11 = -r3
  //   rlwinm r9, r3, 4, 0,27  0x54692036   r9  = (r3 << 4) & ~0xF
  //   cmpwi  cr0, r3, 0x1234  0x2C031234   sets cr0 EQ
  //   mfcr   r12              0x7D800026   r12 = CR
  //   beq    +8               0x41820008   taken, skips the next instruction
  //   addi   r13, r0, 0x0BAD  0x39A00BAD   must NOT execute
  //   addi   r14, r0, 0x600D  0x39C0600D   must execute
  std::printf("\nControl flow and CR\n");
  static constexpr u32 CTRLPROG[] = {
      0x3860'1234, 0x3880'0100, 0x7D44'1850, 0x7D63'00D0, 0x5469'2036,
      0x2C03'1234, 0x7D80'0026, 0x4182'0008, 0x39A0'0BAD, 0x39C0'600D,
  };
  constexpr u32 CBASE = 0x8000'0400;
  for (u32 i = 0; i < std::size(CTRLPROG); ++i)
    memory.Write_U32(CTRLPROG[i], CBASE + i * 4);

  state.pc = CBASE;
  state.npc = CBASE;
  for (u32 r = 9; r <= 14; ++r)
    state.gpr[r] = 0;
  // One step per instruction, minus the one the branch skips.
  for (u32 i = 0; i < std::size(CTRLPROG) - 1; ++i)
    ppc.SingleStep();

  std::printf("  r9=0x%08x r10=0x%08x r11=0x%08x r12=0x%08x r13=0x%08x r14=0x%08x\n",
              state.gpr[9], state.gpr[10], state.gpr[11], state.gpr[12], state.gpr[13],
              state.gpr[14]);
  check(state.gpr[10] == 0x0000'1134, "subf r10 = r3 - r4");
  check(state.gpr[11] == 0xFFFF'EDCC, "neg r11 = -r3");
  check(state.gpr[9] == 0x0001'2340, "rlwinm r9 = r3 << 4");
  check((state.gpr[12] & 0xF000'0000) == 0x2000'0000, "cmpwi set cr0 EQ, visible via mfcr");
  check(state.gpr[13] == 0, "branch skipped the instruction after it");
  check(state.gpr[14] == 0x0000'600D, "execution resumed at the branch target");

  // ── sub-word memory access ──
  // Broadway is big-endian and wasm32 is little-endian, so byte and halfword
  // accesses are where a byte-swap bug shows up. A word-sized round trip can
  // pass while these are wrong, because swapping twice cancels.
  //
  //   stb r3, 0x300(r0)   0x98600300   stores the LOW byte of r3     (0x34)
  //   lbz r15, 0x300(r0)  0x89E00300
  //   sth r3, 0x304(r0)   0xB0600304   stores the LOW halfword       (0x1234)
  //   lhz r16, 0x304(r0)  0xA2000304
  std::printf("\nSub-word memory\n");
  static constexpr u32 SUBPROG[] = {0x9860'0300, 0x89E0'0300, 0xB060'0304, 0xA200'0304};
  constexpr u32 SBASE = 0x8000'0500;
  for (u32 i = 0; i < std::size(SUBPROG); ++i)
    memory.Write_U32(SUBPROG[i], SBASE + i * 4);

  state.gpr[3] = 0x0000'1234;
  state.gpr[15] = 0;
  state.gpr[16] = 0;
  state.pc = SBASE;
  state.npc = SBASE;
  for (u32 i = 0; i < std::size(SUBPROG); ++i)
    ppc.SingleStep();

  std::printf("  r15(byte)=0x%08x r16(half)=0x%08x  guest[0x300]=0x%02x guest[0x304]=0x%04x\n",
              state.gpr[15], state.gpr[16], memory.Read_U8(0x8000'0300),
              memory.Read_U16(0x8000'0304));
  check(state.gpr[15] == 0x34, "lbz reads back the byte stb wrote");
  check(state.gpr[16] == 0x1234, "lhz reads back the halfword sth wrote");
  // Checked from the host side too: a consistent byte-swap bug would make the
  // guest round trip agree with itself while the stored bytes are wrong.
  check(memory.Read_U8(0x8000'0300) == 0x34, "host sees the byte at the right address");
  check(memory.Read_U16(0x8000'0304) == 0x1234, "host sees the halfword big-endian");

  // ── paired singles ──
  // docs/PLAN.md §2 flags this as the Fatal risk: aarch64 contracts
  // a*c + b*d into a fused multiply-add with one rounding step, while
  // WebAssembly has no scalar FMA and rounds twice. Spike A found exactly that
  // divergence in hand-written Rust. The question here is whether Dolphin's
  // interpreter reproduces Broadway's defined rounding under wasm32.
  //
  // ps_madd is the instruction that matters: frD = (frA * frC) + frB, fused by
  // definition on hardware. XO values confirmed against Interpreter_Tables.cpp
  // (ps_sub 20, ps_add 21, ps_mul 25, ps_madd 29).
  std::printf("\nPaired singles\n");
  state.msr.FP = 1;
  check(state.msr.FP == 1, "MSR[FP] enabled");

  // Broadway gates the paired-single unit behind HID2[PSE]; without it every
  // ps_* instruction raises a program exception instead of executing. The
  // first run of this test produced all-zero results and a PC that never
  // advanced, which is exactly what that exception looks like from outside.
  HID2(state).PSE = 1;
  HID2(state).LSQE = 1;
  check(HID2(state).PSE == 1, "HID2[PSE] enables the paired-single unit");

  // Inputs are float32-exact, because that is what paired singles hold.
  //
  // Two earlier attempts to build a fused-vs-unfused discriminator here were
  // wrong: the first used values that give the same answer either way, and the
  // second used values finer than float32 can represent, so they never
  // survived into the registers at all.
  //
  // Working it through explains why no such discriminator exists for this
  // path. Dolphin computes ps_madd in DOUBLE (NI_madd_msub), and a float32
  // significand is 24 bits, so a*c is at most 48 bits -- exact in double's 53.
  // When round(a*c) == a*c, fused and unfused both round exactly once, so they
  // cannot differ. Verified over 200,000 random float32 triples: zero
  // disagreements.
  //
  // So docs/PLAN.md §2's contraction risk does NOT reach paired singles with
  // legitimate float32 data. It remains open for PS registers holding values
  // that are not float32-exact, which other instructions can produce.
  const double a0 = 1.25f, c0 = 3.5f, b0 = -4.375f;   // 1.25 * 3.5 == 4.375
  const double a1 = 1.0000001192092896f;              // 1 + 2^-23, float32-exact
  const double c1 = 1.0000001192092896f;
  const double b1 = -1.0000002384185791f;             // -(1 + 2^-22)
  state.ps[1].SetBoth(a0, a1);
  state.ps[2].SetBoth(c0, c1);
  state.ps[6].SetBoth(b0, b1);

  //   ps_add  f3, f1, f2            (4<<26)|(3<<21)|(1<<16)|(2<<11)|(21<<1)
  //   ps_mul  f4, f1, f2            (4<<26)|(4<<21)|(1<<16)|(2<<6)|(25<<1)
  //   ps_madd f5, f1, f2, f6        (4<<26)|(5<<21)|(1<<16)|(6<<11)|(2<<6)|(29<<1)
  static constexpr u32 FPPROG[] = {0x1061'102A, 0x1081'00B2, 0x10A1'30BA};
  constexpr u32 FBASE = 0x8000'0300;
  for (u32 i = 0; i < std::size(FPPROG); ++i)
    memory.Write_U32(FPPROG[i], FBASE + i * 4);

  state.pc = FBASE;
  state.npc = FBASE;
  for (u32 i = 0; i < std::size(FPPROG); ++i)
    ppc.SingleStep();

  // Print raw bit patterns, not decimals: a one-ulp difference is the whole
  // point and decimal formatting would hide it.
  auto bits = [](double d) {
    u64 u;
    std::memcpy(&u, &d, sizeof(u));
    return u;
  };
  std::printf("  ps_add  f3 = %016llx %016llx\n",
              static_cast<unsigned long long>(bits(state.ps[3].PS0AsDouble())),
              static_cast<unsigned long long>(bits(state.ps[3].PS1AsDouble())));
  std::printf("  ps_mul  f4 = %016llx %016llx\n",
              static_cast<unsigned long long>(bits(state.ps[4].PS0AsDouble())),
              static_cast<unsigned long long>(bits(state.ps[4].PS1AsDouble())));
  std::printf("  ps_madd f5 = %016llx %016llx\n",
              static_cast<unsigned long long>(bits(state.ps[5].PS0AsDouble())),
              static_cast<unsigned long long>(bits(state.ps[5].PS1AsDouble())));

  u64 fp_hash = 0xcbf29ce484222325ULL;
  for (int r : {3, 4, 5})
    for (u64 v : {bits(state.ps[r].PS0AsDouble()), bits(state.ps[r].PS1AsDouble())})
      for (int byte = 0; byte < 8; ++byte)
      {
        fp_hash ^= static_cast<u8>(v >> (byte * 8));
        fp_hash *= 0x100000001b3ULL;
      }
  std::printf("  paired-single fnv1a=%016llx\n", static_cast<unsigned long long>(fp_hash));
  check(state.pc == FBASE + std::size(FPPROG) * 4, "pc advanced past the FP program");

  // 1.25 * 3.5 - 4.375 is exact in both float32 and double, so the only way
  // this is nonzero is if the paired-single path is broken outright.
  check(state.ps[5].PS0AsDouble() == 0.0, "ps_madd exact case cancels to zero");

  ppc.Shutdown();
  Core::UndeclareAsCPUThread();
  check(!Core::IsCPUThread(), "PowerPC shut down and CPU thread released");

  memory.Shutdown();
  check(true, "memory shut down cleanly");

  std::printf("\n%s (%d failures)\n", g_failures ? "FAILED" : "PASSED", g_failures);
  return g_failures ? 1 : 0;
}
