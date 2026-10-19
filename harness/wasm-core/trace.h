// Instruction-level CPU trace, emitted identically on native and wasm32.
//
// docs/PLAN.md Bet 04 makes native Dolphin the test oracle: capture traces
// from a known-good build and diff them against the port. Until now that was
// aspirational -- every assertion in this harness has been hand-written, and
// six of them turned out to be wrong, each failing in the direction of looking
// like a port bug.
//
// Hand-written assertions do not scale to a boot sequence that executes
// millions of instructions. This does.
//
// ── format ──
//
// One line per executed instruction, written to stdout so neither side has to
// read the other's filesystem (Emscripten's MEMFS does not share with the
// host, and NODERAWFS conflicts with PROXY_TO_PTHREAD):
//
//   T <step> <pc> <gpr_hash> <cr> <msr> <lr> <ctr>
//
// Everything is fixed-width hex, so a divergence is findable with diff and
// readable without tooling. GPRs are hashed rather than printed because 32
// registers per line would make a boot trace unusable; when a hash differs,
// --dump-at replays to that step and prints the registers in full.
//
// The 'T' prefix lets trace lines be separated from ordinary test output in
// the same stream.

#pragma once

#include <cstdint>
#include <cstdio>

#include "Common/CommonTypes.h"
#include "Core/PowerPC/PowerPC.h"

namespace Trace
{
// FNV-1a over the 32 general purpose registers. Order matters and is fixed.
inline u64 HashGPRs(const PowerPC::PowerPCState& s)
{
  u64 h = 0xcbf29ce484222325ULL;
  for (int i = 0; i < 32; ++i)
  {
    const u32 v = s.gpr[i];
    for (int b = 0; b < 4; ++b)
    {
      h ^= static_cast<u8>(v >> (b * 8));
      h *= 0x100000001b3ULL;
    }
  }
  return h;
}

inline void EmitLine(u64 step, const PowerPC::PowerPCState& s)
{
  // msr and cr are read through their accessors so the emitted value matches
  // what the emulator considers current, not a stale cached field.
  std::printf("T %08llx %08x %016llx %08x %08x %08x %08x\n",
              static_cast<unsigned long long>(step), s.pc,
              static_cast<unsigned long long>(HashGPRs(s)), s.cr.Get(), s.msr.Hex, s.spr[SPR_LR],
              s.spr[SPR_CTR]);
}

// Full register dump, printed only at a step the differ asked about. This is
// the second pass: the first says WHERE the two targets diverged, this says
// WHAT differs there.
inline void EmitFullState(u64 step, const PowerPC::PowerPCState& s)
{
  std::printf("D step=%llu pc=%08x cr=%08x msr=%08x lr=%08x ctr=%08x\n",
              static_cast<unsigned long long>(step), s.pc, s.cr.Get(), s.msr.Hex, s.spr[SPR_LR],
              s.spr[SPR_CTR]);
  for (int i = 0; i < 32; i += 4)
  {
    std::printf("D   r%-2d=%08x r%-2d=%08x r%-2d=%08x r%-2d=%08x\n", i, s.gpr[i], i + 1,
                s.gpr[i + 1], i + 2, s.gpr[i + 2], i + 3, s.gpr[i + 3]);
  }
}
}  // namespace Trace
