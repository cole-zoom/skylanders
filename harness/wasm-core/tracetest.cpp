// Emit an instruction-level CPU trace for differential comparison.
//
// Runs a deterministic guest program under the interpreter and prints one
// trace line per executed instruction. Native and wasm32 builds must produce
// byte-identical output; harness/wasm-core/difftrace.py finds the first step
// where they do not.
//
// Usage:
//   tracetest                 emit the trace
//   tracetest --dump-at=N     emit the trace, then full registers at step N
//
// The workload is intentionally branchy and arithmetic-heavy rather than a
// straight line: a loop that falls through on the wrong condition still
// produces a plausible trace, so the trace has to encode control flow, and a
// divergence in the condition register has to show up as a different PC
// sequence rather than only a different register value.

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "Common/CommonTypes.h"
#include "Common/Config/Config.h"
#include "Core/Config/MainSettings.h"
#include "Core/ConfigManager.h"
#include "Core/Core.h"
#include "Core/HW/Memmap.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/System.h"

#include "trace.h"

// A counted loop that accumulates, shifts and compares. Hand-assembled; the
// encodings are the same ones the coreboot differential already verifies.
//
//   addi   r3, r0, 0          0x38600000   acc = 0
//   addi   r4, r0, 16         0x38800010   i = 16
//   addi   r5, r0, 7          0x38A00007   step = 7
// loop:
//   add    r3, r3, r5         0x7C632A14   acc += step
//   rlwinm r6, r3, 1, 0, 30   0x54C6083C   r6 = acc << 1
//   xor    r3, r3, r6         0x7C661A78   acc ^= r6
//   addi   r4, r4, -1         0x3884FFFF   i -= 1
//   cmpwi  r4, 0              0x2C040000   compare i with 0
//   bne    loop               0x4082FFF0   branch back if i != 0
//   addi   r7, r0, 0x0D05     0x38E00D05   sentinel, proves the loop exited
static constexpr u32 PROGRAM[] = {
    0x3860'0000, 0x3880'0010, 0x38A0'0007, 0x7C63'2A14, 0x54C6'083C,
    0x7C66'1A78, 0x3884'FFFF, 0x2C04'0000, 0x4082'FFF0, 0x38E0'0D05,
};
static constexpr u32 BASE = 0x8000'1000;
static constexpr u32 LOOP_BODY = 6;   // instructions between `loop:` and `bne`
static constexpr u32 ITERATIONS = 16;
// 3 setup + 16 iterations of 6 + the sentinel.
static constexpr u64 TOTAL_STEPS = 3 + ITERATIONS * LOOP_BODY + 1;

int main(int argc, char** argv)
{
  std::setvbuf(stdout, nullptr, _IONBF, 0);

  s64 dump_at = -1;
  for (int i = 1; i < argc; ++i)
  {
    if (std::strncmp(argv[i], "--dump-at=", 10) == 0)
      dump_at = std::strtoll(argv[i] + 10, nullptr, 0);
  }

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

  for (u32 i = 0; i < std::size(PROGRAM); ++i)
    memory.Write_U32(PROGRAM[i], BASE + i * 4);

  for (int i = 0; i < 32; ++i)
    state.gpr[i] = 0;
  state.pc = BASE;
  state.npc = BASE;

  // The trace records state BEFORE each instruction, so step 0 is the initial
  // state and the final line shows the result of the last instruction.
  for (u64 step = 0; step <= TOTAL_STEPS; ++step)
  {
    Trace::EmitLine(step, state);
    if (static_cast<s64>(step) == dump_at)
      Trace::EmitFullState(step, state);
    if (step < TOTAL_STEPS)
      ppc.SingleStep();
  }

  // Printed outside the trace stream so the differ ignores it.
  std::printf("# steps=%llu final r3=%08x r7=%08x\n",
              static_cast<unsigned long long>(TOTAL_STEPS), state.gpr[3], state.gpr[7]);

  ppc.Shutdown();
  Core::UndeclareAsCPUThread();
  memory.Shutdown();
  return 0;
}
