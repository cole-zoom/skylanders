// PowerPC basic block -> WebAssembly.
//
// The emitted function has signature (i32 state) -> i32: it takes a pointer to
// the guest's PowerPCState in the host heap, applies the block's effects in
// place, and returns the PC to continue from. That shape is deliberate --
// Spike A measured 863.9 MIPS with exactly this topology (guest state in
// memory, blocks reached by indirect call) and far less with state held in
// host registers, which no real recompiler can arrange anyway.
//
// Only straight-line integer instructions are translated. The block ENDS at
// the first instruction that is not understood, and the caller falls back to
// the interpreter from there. That is not a limitation to apologise for: it
// is the tier-up design in docs/PLAN.md, where compiled code and the
// interpreter share one guest state and either can run at any boundary.
//
// What is deliberately absent: branches, the condition register, memory
// access, and anything floating point. Each needs a decision that is easier
// to make well once the simple case is measured.
#pragma once

#include <cstdint>
#include <vector>

#include "wasm_emit.h"

namespace ppc2wasm
{
struct BlockResult
{
  Bytes module;           // a complete wasm module, or empty if nothing compiled
  uint32_t instructions;  // PowerPC instructions translated
  uint32_t end_pc;        // where the interpreter should resume
  const char* stopped_on; // why the block ended, for diagnostics
};

// Offsets into the guest state struct. Passed in rather than hardcoded,
// because PowerPCState's layout is Dolphin's business and baking a number
// here would break silently the first time a field moved.
struct StateLayout
{
  uint32_t gpr_offset;

  // The emitted code loads and stores guest registers, so the module must
  // import the host's linear memory -- and the import has to match the
  // running instance exactly (shared flag, initial pages, maximum) or
  // instantiation fails with a link error rather than a useful message.
  uint32_t memory_pages = 0;
  bool memory_shared = true;
};

namespace detail
{
inline uint32_t Opcode(uint32_t i) { return i >> 26; }
inline uint32_t RS(uint32_t i) { return (i >> 21) & 0x1F; }
inline uint32_t RA(uint32_t i) { return (i >> 16) & 0x1F; }
inline uint32_t RB(uint32_t i) { return (i >> 11) & 0x1F; }
inline int32_t SIMM(uint32_t i) { return static_cast<int16_t>(i & 0xFFFF); }
inline uint32_t UIMM(uint32_t i) { return i & 0xFFFF; }
inline uint32_t XO(uint32_t i) { return (i >> 1) & 0x3FF; }
inline uint32_t SH(uint32_t i) { return (i >> 11) & 0x1F; }
inline uint32_t MB(uint32_t i) { return (i >> 6) & 0x1F; }
inline uint32_t ME(uint32_t i) { return (i >> 1) & 0x1F; }
inline bool Rc(uint32_t i) { return (i & 1) != 0; }

// PowerPC's mask from MB..ME, inclusive, with bit 0 the MOST significant.
// Wrapping (MB > ME) is legal and means the complement of the gap, which is
// why this is a loop rather than a shift expression.
inline uint32_t Mask(uint32_t mb, uint32_t me)
{
  uint32_t m = 0;
  for (uint32_t b = 0; b < 32; ++b)
  {
    const bool in = (mb <= me) ? (b >= mb && b <= me) : (b >= mb || b <= me);
    if (in)
      m |= 0x80000000u >> b;
  }
  return m;
}

// Push gpr[n] onto the wasm stack. `state` is local 0.
inline void LoadGpr(Bytes& b, const StateLayout& l, uint32_t n)
{
  b.push_back(kLocalGet);
  PutU32(b, 0);
  b.push_back(kI32Load);
  PutU32(b, 2);  // alignment 2^2
  PutU32(b, l.gpr_offset + n * 4);
}

// In several instructions the rA field means LITERAL ZERO rather than r0.
// Reading r0 instead is a real bug this project has already hit once, in a
// test assertion rather than in emitted code.
inline void LoadGprOrZero(Bytes& b, const StateLayout& l, uint32_t n)
{
  if (n == 0)
  {
    b.push_back(kI32Const);
    PutI32(b, 0);
    return;
  }
  LoadGpr(b, l, n);
}

// Begin a store to gpr[n]: pushes the address. The value must follow, then
// EndStoreGpr.
inline void BeginStoreGpr(Bytes& b)
{
  b.push_back(kLocalGet);
  PutU32(b, 0);
}

inline void EndStoreGpr(Bytes& b, const StateLayout& l, uint32_t n)
{
  b.push_back(kI32Store);
  PutU32(b, 2);
  PutU32(b, l.gpr_offset + n * 4);
}
}  // namespace detail

// Translate instructions starting at `code[0]` (guest address `start_pc`)
// until one is not understood or `max_insts` is reached.
inline BlockResult CompileBlock(const uint32_t* code, uint32_t start_pc, uint32_t max_insts,
                                const StateLayout& layout)
{
  using namespace detail;
  Bytes body;
  uint32_t n = 0;
  const char* stopped = "instruction limit";

  for (; n < max_insts; ++n)
  {
    const uint32_t ins = code[n];
    const uint32_t op = Opcode(ins);
    bool handled = false;

    switch (op)
    {
    case 14:  // addi rD, rA, SIMM  (rA == 0 means literal zero)
      BeginStoreGpr(body);
      LoadGprOrZero(body, layout, RA(ins));
      body.push_back(kI32Const);
      PutI32(body, SIMM(ins));
      body.push_back(kI32Add);
      EndStoreGpr(body, layout, RS(ins));
      handled = true;
      break;

    case 15:  // addis rD, rA, SIMM  (shifted; also how lis is written)
      BeginStoreGpr(body);
      LoadGprOrZero(body, layout, RA(ins));
      body.push_back(kI32Const);
      PutI32(body, SIMM(ins) << 16);
      body.push_back(kI32Add);
      EndStoreGpr(body, layout, RS(ins));
      handled = true;
      break;

    case 24:  // ori rA, rS, UIMM
    case 25:  // oris
    case 26:  // xori
    case 27:  // xoris
    {
      const bool shifted = (op == 25 || op == 27);
      const bool is_xor = (op >= 26);
      BeginStoreGpr(body);
      LoadGpr(body, layout, RS(ins));
      body.push_back(kI32Const);
      PutI32(body, static_cast<int32_t>(shifted ? (UIMM(ins) << 16) : UIMM(ins)));
      body.push_back(is_xor ? kI32Xor : kI32Or);
      EndStoreGpr(body, layout, RA(ins));
      handled = true;
      break;
    }

    case 21:  // rlwinm rA, rS, SH, MB, ME -- rotate left then mask
    {
      // Rc (the dot form) sets the condition register, which is not modelled
      // yet, so those stop the block rather than being silently ignored.
      if (Rc(ins))
        break;
      BeginStoreGpr(body);
      LoadGpr(body, layout, RS(ins));
      if (SH(ins))
      {
        body.push_back(kI32Const);
        PutI32(body, static_cast<int32_t>(SH(ins)));
        body.push_back(kI32Rotl);
      }
      body.push_back(kI32Const);
      PutI32(body, static_cast<int32_t>(Mask(MB(ins), ME(ins))));
      body.push_back(kI32And);
      EndStoreGpr(body, layout, RA(ins));
      handled = true;
      break;
    }

    case 31:  // the extended-opcode space
    {
      if (Rc(ins))
        break;
      const uint32_t xo = XO(ins);
      uint8_t wasm_op = 0;
      bool swap = false;  // subf computes rB - rA, the reverse of the field order
      switch (xo)
      {
      case 266: wasm_op = kI32Add; break;   // add
      case 40:  wasm_op = kI32Sub; swap = true; break;  // subf
      case 444: wasm_op = kI32Or; break;    // or (and mr, which is or rA,rS,rS)
      case 28:  wasm_op = kI32And; break;   // and
      case 316: wasm_op = kI32Xor; break;   // xor
      case 24:  wasm_op = kI32Shl; break;   // slw
      case 536: wasm_op = kI32ShrU; break;  // srw
      default: break;
      }
      if (!wasm_op)
        break;

      // add/subf write rD (the RS field); the logical ops write rA.
      const bool writes_rd = (xo == 266 || xo == 40);
      BeginStoreGpr(body);
      if (swap)
      {
        LoadGpr(body, layout, RB(ins));
        LoadGpr(body, layout, RA(ins));
      }
      else if (writes_rd)
      {
        LoadGpr(body, layout, RA(ins));
        LoadGpr(body, layout, RB(ins));
      }
      else
      {
        LoadGpr(body, layout, RS(ins));
        LoadGpr(body, layout, RB(ins));
      }
      body.push_back(wasm_op);
      EndStoreGpr(body, layout, writes_rd ? RS(ins) : RA(ins));
      handled = true;
      break;
    }

    default:
      break;
    }

    if (!handled)
    {
      stopped = "unsupported instruction";
      break;
    }
  }

  BlockResult r;
  r.instructions = n;
  r.end_pc = start_pc + n * 4;
  r.stopped_on = stopped;
  if (n == 0)
    return r;  // nothing compiled; module stays empty

  // The block's result is the PC to resume at.
  body.push_back(kI32Const);
  PutI32(body, static_cast<int32_t>(r.end_pc));

  ModuleBuilder mb;
  mb.ImportMemory(layout.memory_pages, layout.memory_pages, layout.memory_shared);
  mb.SetFunction({kI32}, {kI32}, body, "f");
  r.module = mb.Build();
  return r;
}
}  // namespace ppc2wasm
