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

  // Where guest RAM lives in the host heap, and how a guest address maps into
  // it. Dolphin keeps guest memory in GUEST byte order, so a 32-bit load has
  // to be reassembled from bytes -- WebAssembly is little-endian and has no
  // byte-swap instruction.
  uint32_t ram_base = 0;              // host address of guest 0x80000000
  uint32_t ram_mask = 0x01FFFFFF;     // offset within MEM1 (24 MB, mirrored)
  uint32_t guard_mask = 0xFE000000;   // the bits that identify the region
  uint32_t guard_value = 0x80000000;  // ... and what they must equal

  // The condition register is NOT stored as PowerPC's four bits. Dolphin
  // keeps eight 64-bit fields in an encoding chosen so that common tests are
  // cheap, built from a 16-entry lookup table. Compiled code shares guest
  // state with the interpreter, so it has to produce that same encoding --
  // and the least error-prone way is to use the very same table rather than
  // re-deriving it.
  uint32_t cr_offset = 0;        // offsetof(PowerPCState, cr)
  uint32_t xer_so_ov_offset = 0; // offsetof(PowerPCState, xer_so_ov)
  uint32_t cr_table_base = 0;    // host address of ConditionRegister::s_crTable
  uint32_t spr_offset = 0;       // offsetof(PowerPCState, spr)
  uint32_t lr_index = 8;         // SPR_LR
  uint32_t ctr_index = 9;        // SPR_CTR

  // ── block linking ──
  //
  // When enabled, a block does not return to the dispatcher: it decrements
  // the guest downcount, looks the next PC up in a table the dispatcher
  // maintains, and tail-calls straight into the next block. A chain ends
  // when the downcount expires or the lookup misses.
  //
  // Linking is OFF by default so that the per-instruction differential keeps
  // compiling blocks that simply return -- the thing it checks is the guest
  // state a block produces, not how control leaves it.
  bool enable_linking = false;
  uint32_t downcount_offset = 0;  // offsetof(PowerPCState, downcount)
  uint32_t link_base = 0;         // host address of the link table
  uint32_t link_mask = 0;         // entry count - 1, entries are {pc, index}
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

// Emit: if ((ea & guard_mask) != guard_value) return bail_pc | 1;
//
// A compiled block may only touch MEM1. Anything else -- MEM2, MMIO, an
// unmapped address -- has semantics this code does not implement, so the
// block BAILS: it returns the PC of the offending instruction and the
// interpreter re-executes it.
//
// Bailing before the instruction's effects are applied is what makes this
// safe. Everything earlier in the block is already committed to guest state,
// and resuming at this PC re-runs exactly the instruction that was refused.
//
// The returned PC has its low bit SET to mark it as a bail. PowerPC
// instructions are four-byte aligned so the bit is otherwise unused, and the
// dispatcher needs the distinction: a bail means only part of the block ran,
// and the emulated clock must be advanced by that part rather than by the
// whole. Without the marker, a block whose branch happens to target its own
// first instruction is indistinguishable from one that refused immediately.
inline void EmitGuard(Bytes& b, const StateLayout& l, uint32_t ea_local, uint32_t bail_pc,
                      uint32_t insts_before)
{
  b.push_back(kLocalGet);
  PutU32(b, ea_local);
  b.push_back(kI32Const);
  PutI32(b, static_cast<int32_t>(l.guard_mask));
  b.push_back(kI32And);
  b.push_back(kI32Const);
  PutI32(b, static_cast<int32_t>(l.guard_value));
  b.push_back(kI32Ne);
  b.push_back(kIf);
  b.push_back(kVoid);
  // A bail skips the shared epilogue, so it has to account for its own
  // partial execution. Without this the dispatcher cannot tell how far a
  // CHAIN got: it sees only the final PC, and the blocks that ran before the
  // refusal are invisible.
  if (l.enable_linking && insts_before)
  {
    b.push_back(kLocalGet);
    PutU32(b, 0);
    b.push_back(kLocalGet);
    PutU32(b, 0);
    b.push_back(kI32Load);
    PutU32(b, 2);
    PutU32(b, l.downcount_offset);
    b.push_back(kI32Const);
    PutI32(b, static_cast<int32_t>(insts_before));
    b.push_back(kI32Sub);
    b.push_back(kI32Store);
    PutU32(b, 2);
    PutU32(b, l.downcount_offset);
  }
  b.push_back(kI32Const);
  PutI32(b, static_cast<int32_t>(bail_pc | 1u));
  b.push_back(kReturn);
  b.push_back(kEnd);
}

// ea_local holds a guest address; replace it with the host address.
inline void EmitToHostAddress(Bytes& b, const StateLayout& l, uint32_t ea_local)
{
  b.push_back(kLocalGet);
  PutU32(b, ea_local);
  b.push_back(kI32Const);
  PutI32(b, static_cast<int32_t>(l.ram_mask));
  b.push_back(kI32And);
  b.push_back(kI32Const);
  PutI32(b, static_cast<int32_t>(l.ram_base));
  b.push_back(kI32Add);
  b.push_back(kLocalSet);
  PutU32(b, ea_local);
}

// Compute the effective address of a d(rA) operand into ea_local.
inline void EmitEffectiveAddress(Bytes& b, const StateLayout& l, uint32_t ra, int32_t disp,
                                 uint32_t ea_local)
{
  LoadGprOrZero(b, l, ra);
  if (disp)
  {
    b.push_back(kI32Const);
    PutI32(b, disp);
    b.push_back(kI32Add);
  }
  b.push_back(kLocalSet);
  PutU32(b, ea_local);
}

// Push a big-endian u32 read from the host address in ea_local.
//
// Four byte loads rather than one word load plus a swap: WebAssembly has no
// byte-swap instruction, so the alternative is a shift-and-or chain of
// similar length that also assumes the address is 4-byte aligned. PowerPC
// permits unaligned loads.
inline void EmitLoadBE32(Bytes& b, uint32_t ea_local)
{
  for (int byte = 0; byte < 4; ++byte)
  {
    b.push_back(kLocalGet);
    PutU32(b, ea_local);
    b.push_back(kI32Load8U);
    PutU32(b, 0);  // alignment 2^0
    PutU32(b, static_cast<uint32_t>(byte));
    const int shift = 24 - byte * 8;
    if (shift)
    {
      b.push_back(kI32Const);
      PutI32(b, shift);
      b.push_back(kI32Shl);
    }
    if (byte)
      b.push_back(kI32Or);
  }
}

// Emit a comparison of local 1 against local 2 into cr.fields[crf].
//
// PowerPC's four condition bits are LT=8, GT=4, EQ=2, SO=1, and the SO bit
// comes from XER rather than the comparison -- dropping it is a silent
// correctness bug, so it is read from guest state here.
//
// The resulting 4-bit value indexes Dolphin's s_crTable, whose entries are
// the 64-bit internal encoding. Using the table rather than recomputing the
// encoding means this cannot drift from the interpreter.
inline void EmitCompareToCR(Bytes& b, const StateLayout& l, uint32_t crf, bool is_signed)
{
  // Address for the i64 store comes first: the value is built on top of it.
  b.push_back(kLocalGet);
  PutU32(b, 0);

  // (a < b) << 3
  b.push_back(kLocalGet);
  PutU32(b, 1);
  b.push_back(kLocalGet);
  PutU32(b, 2);
  b.push_back(is_signed ? kI32LtS : kI32LtU);
  b.push_back(kI32Const);
  PutI32(b, 3);
  b.push_back(kI32Shl);

  // | (a > b) << 2
  b.push_back(kLocalGet);
  PutU32(b, 1);
  b.push_back(kLocalGet);
  PutU32(b, 2);
  b.push_back(is_signed ? kI32GtS : kI32GtU);
  b.push_back(kI32Const);
  PutI32(b, 2);
  b.push_back(kI32Shl);
  b.push_back(kI32Or);

  // | (a == b) << 1
  b.push_back(kLocalGet);
  PutU32(b, 1);
  b.push_back(kLocalGet);
  PutU32(b, 2);
  b.push_back(kI32Eq);
  b.push_back(kI32Const);
  PutI32(b, 1);
  b.push_back(kI32Shl);
  b.push_back(kI32Or);

  // | XER[SO], which lives as (SO << 1) | OV in a byte of guest state.
  b.push_back(kLocalGet);
  PutU32(b, 0);
  b.push_back(kI32Load8U);
  PutU32(b, 0);
  PutU32(b, l.xer_so_ov_offset);
  b.push_back(kI32Const);
  PutI32(b, 1);
  b.push_back(kI32ShrU);
  b.push_back(kI32Const);
  PutI32(b, 1);
  b.push_back(kI32And);
  b.push_back(kI32Or);

  // s_crTable[flags] -- eight bytes per entry.
  b.push_back(kI32Const);
  PutI32(b, 3);
  b.push_back(kI32Shl);
  b.push_back(kI32Const);
  PutI32(b, static_cast<int32_t>(l.cr_table_base));
  b.push_back(kI32Add);
  b.push_back(kI64Load);
  PutU32(b, 3);  // alignment 2^3
  PutU32(b, 0);

  b.push_back(kI64Store);
  PutU32(b, 3);
  PutU32(b, l.cr_offset + crf * 8);
}

// Push 1 or 0 for condition register bit `bit` (0..31).
//
// Within a field the bits are LT, GT, EQ, SO, and each is extracted
// differently because Dolphin's 64-bit encoding is not a packed nibble:
//
//   LT  bit 62        -> bit 30 of the high word
//   GT  (s64)v > 0    -> sign clear AND the value non-zero
//   EQ  low 32 == 0
//   SO  bit 59        -> bit 27 of the high word
//
// The halves are loaded as two i32s rather than one i64, which keeps the
// emitter free of 64-bit arithmetic for what is otherwise a bit test.
inline void EmitCRBit(Bytes& b, const StateLayout& l, uint32_t bit)
{
  const uint32_t field = bit >> 2;
  const uint32_t which = bit & 3;
  const uint32_t lo = l.cr_offset + field * 8;
  const uint32_t hi = lo + 4;

  auto load_at = [&](uint32_t off) {
    b.push_back(kLocalGet);
    PutU32(b, 0);
    b.push_back(kI32Load);
    PutU32(b, 2);
    PutU32(b, off);
  };

  switch (which)
  {
  case 0:  // LT
    load_at(hi);
    b.push_back(kI32Const);
    PutI32(b, 30);
    b.push_back(kI32ShrU);
    b.push_back(kI32Const);
    PutI32(b, 1);
    b.push_back(kI32And);
    break;
  case 1:  // GT -- (s64)value > 0, so sign clear and value non-zero
    load_at(hi);
    b.push_back(kI32Const);
    PutI32(b, 31);
    b.push_back(kI32ShrU);
    b.push_back(kI32Const);
    PutI32(b, 1);
    b.push_back(kI32Xor);
    load_at(hi);
    load_at(lo);
    b.push_back(kI32Or);
    b.push_back(kI32Const);
    PutI32(b, 0);
    b.push_back(kI32Ne);
    b.push_back(kI32And);
    break;
  case 2:  // EQ -- the low word is zero
    load_at(lo);
    b.push_back(kI32Const);
    PutI32(b, 0);
    b.push_back(kI32Eq);
    break;
  default:  // SO
    load_at(hi);
    b.push_back(kI32Const);
    PutI32(b, 27);
    b.push_back(kI32ShrU);
    b.push_back(kI32Const);
    PutI32(b, 1);
    b.push_back(kI32And);
    break;
  }
}

// Store a value into an SPR (used for the link register).
inline void EmitSetSpr(Bytes& b, const StateLayout& l, uint32_t index, uint32_t value)
{
  b.push_back(kLocalGet);
  PutU32(b, 0);
  b.push_back(kI32Const);
  PutI32(b, static_cast<int32_t>(value));
  b.push_back(kI32Store);
  PutU32(b, 2);
  PutU32(b, l.spr_offset + index * 4);
}

// The shared block epilogue, used only when linking is enabled.
//
// local 4 holds the PC to continue at. The block has already applied all its
// effects; what remains is deciding whether to keep running compiled code.
//
// The downcount is guest state the interpreter also uses, so decrementing it
// here keeps one clock rather than two. When it expires the chain returns to
// the dispatcher, which advances CoreTiming by however much was consumed --
// that is the only point at which scheduled events can fire, so chain length
// directly sets timing granularity.
// Leave the block with the PC already on the stack.
//
// Without linking that is simply a return. With linking the value is parked
// in local 4 and control branches to the shared epilogue, so every exit --
// fall-through, taken branch, return instruction -- goes through the same
// downcount and lookup logic exactly once.
inline void EmitGoTo(Bytes& b, const StateLayout& l, uint32_t br_depth)
{
  if (!l.enable_linking)
  {
    b.push_back(kReturn);
    return;
  }
  b.push_back(kLocalSet);
  PutU32(b, 4);
  b.push_back(kBr);
  PutU32(b, br_depth);
}

inline void EmitLinkEpilogue(Bytes& b, const StateLayout& l, uint32_t insts)
{
  // downcount -= insts, keeping the new value
  b.push_back(kLocalGet);
  PutU32(b, 0);
  b.push_back(kLocalGet);
  PutU32(b, 0);
  b.push_back(kI32Load);
  PutU32(b, 2);
  PutU32(b, l.downcount_offset);
  b.push_back(kI32Const);
  PutI32(b, static_cast<int32_t>(insts));
  b.push_back(kI32Sub);
  b.push_back(kLocalTee);
  PutU32(b, 5);
  b.push_back(kI32Store);
  PutU32(b, 2);
  PutU32(b, l.downcount_offset);

  // Out of budget: hand back to the dispatcher.
  b.push_back(kLocalGet);
  PutU32(b, 5);
  b.push_back(kI32Const);
  PutI32(b, 0);
  b.push_back(kI32LeS);
  b.push_back(kIf);
  b.push_back(kVoid);
  b.push_back(kLocalGet);
  PutU32(b, 4);
  b.push_back(kReturn);
  b.push_back(kEnd);

  // slot = link_base + ((next_pc >> 2) & mask) * 8
  b.push_back(kLocalGet);
  PutU32(b, 4);
  b.push_back(kI32Const);
  PutI32(b, 2);
  b.push_back(kI32ShrU);
  b.push_back(kI32Const);
  PutI32(b, static_cast<int32_t>(l.link_mask));
  b.push_back(kI32And);
  b.push_back(kI32Const);
  PutI32(b, 3);
  b.push_back(kI32Shl);
  b.push_back(kI32Const);
  PutI32(b, static_cast<int32_t>(l.link_base));
  b.push_back(kI32Add);
  b.push_back(kLocalSet);
  PutU32(b, 6);

  // The stored PC is checked, not assumed: the table is a direct-mapped
  // cache and two PCs can land in the same slot. A miss is correct and
  // cheap; a wrong hit would execute another block's code.
  b.push_back(kLocalGet);
  PutU32(b, 6);
  b.push_back(kI32Load);
  PutU32(b, 2);
  PutU32(b, 0);
  b.push_back(kLocalGet);
  PutU32(b, 4);
  b.push_back(kI32Eq);
  b.push_back(kIf);
  b.push_back(kVoid);
  b.push_back(kLocalGet);
  PutU32(b, 0);  // the next block takes the same state pointer
  b.push_back(kLocalGet);
  PutU32(b, 6);
  b.push_back(kI32Load);
  PutU32(b, 2);
  PutU32(b, 4);
  // A tail call, not a call: a linked chain is a sequence of jumps and costs
  // no stack, so chain length is bounded by the downcount rather than by how
  // deep the host stack can go.
  b.push_back(kReturnCallIndirect);
  PutU32(b, 0);  // type index
  PutU32(b, 0);  // table index
  b.push_back(kEnd);

  // Lookup missed: the dispatcher will compile or find it.
  b.push_back(kLocalGet);
  PutU32(b, 4);
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
  // Scratch locals above the state parameter: 1 = effective/host address,
  // 2 = the value being stored. Declared only if used, so a block of pure
  // register arithmetic still emits none.
  uint32_t used_locals = 0;
  const char* stopped = "instruction limit";

  for (; n < max_insts; ++n)
  {
    const uint32_t ins = code[n];
    const uint32_t op = Opcode(ins);
    bool handled = false;
    // A branch is translated and then TERMINATES the block: control leaves
    // here, so anything after it in the instruction stream belongs to a
    // different block.
    bool ends_block = false;

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

      // cmp (XO 0) and cmpl (XO 32) are compares, not arithmetic.
      if (xo == 0 || xo == 32)
      {
        const bool is_signed = (xo == 0);
        const uint32_t crf = (RS(ins) >> 2) & 7;
        LoadGpr(body, layout, RA(ins));
        body.push_back(kLocalSet);
        PutU32(body, 1);
        LoadGpr(body, layout, RB(ins));
        body.push_back(kLocalSet);
        PutU32(body, 2);
        EmitCompareToCR(body, layout, crf, is_signed);
        used_locals = used_locals < 3 ? 3 : used_locals;
        handled = true;
        break;
      }

      // mfspr / mtspr, restricted to the link and count registers.
      //
      // Other SPRs have side effects -- writing the decrementer reschedules
      // an interrupt, writing a BAT changes address translation -- and a
      // compiled block that moved those around without telling the rest of
      // the emulator would be wrong in ways no register comparison catches.
      if (xo == 339 || xo == 467)
      {
        // The SPR field is SPLIT and the halves are swapped, which is the
        // classic way to decode this one wrong.
        const uint32_t spr = ((ins >> 16) & 0x1F) | (((ins >> 11) & 0x1F) << 5);
        if (spr != layout.lr_index && spr != layout.ctr_index)
          break;
        if (xo == 339)  // mfspr rD, SPR
        {
          BeginStoreGpr(body);
          body.push_back(kLocalGet);
          PutU32(body, 0);
          body.push_back(kI32Load);
          PutU32(body, 2);
          PutU32(body, layout.spr_offset + spr * 4);
          EndStoreGpr(body, layout, RS(ins));
        }
        else  // mtspr SPR, rS
        {
          body.push_back(kLocalGet);
          PutU32(body, 0);
          LoadGpr(body, layout, RS(ins));
          body.push_back(kI32Store);
          PutU32(body, 2);
          PutU32(body, layout.spr_offset + spr * 4);
        }
        handled = true;
        break;
      }

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

    case 11:  // cmpi  crfD, rA, SIMM   (signed)
    case 10:  // cmpli crfD, rA, UIMM   (unsigned)
    {
      const bool is_signed = (op == 11);
      const uint32_t crf = (RS(ins) >> 2) & 7;
      LoadGpr(body, layout, RA(ins));
      body.push_back(kLocalSet);
      PutU32(body, 1);
      body.push_back(kI32Const);
      if (is_signed)
        PutI32(body, SIMM(ins));
      else
        PutI32(body, static_cast<int32_t>(UIMM(ins)));
      body.push_back(kLocalSet);
      PutU32(body, 2);
      EmitCompareToCR(body, layout, crf, is_signed);
      used_locals = used_locals < 3 ? 3 : used_locals;
      handled = true;
      break;
    }

    case 18:  // b / ba / bl / bla
    {
      const uint32_t this_pc = start_pc + n * 4;
      // LI is a signed 24-bit word displacement, stored shifted left by two.
      int32_t li = static_cast<int32_t>(ins & 0x03FFFFFC);
      if (li & 0x02000000)
        li -= 0x04000000;  // sign extend from 26 bits
      const bool absolute = (ins & 2) != 0;
      const bool link = (ins & 1) != 0;
      const uint32_t target = absolute ? static_cast<uint32_t>(li) : this_pc + li;
      if (link)
        EmitSetSpr(body, layout, layout.lr_index, this_pc + 4);
      body.push_back(kI32Const);
      PutI32(body, static_cast<int32_t>(target));
      EmitGoTo(body, layout, 0);
      handled = true;
      ends_block = true;
      break;
    }

    case 16:  // bc / bca / bcl / bcla
    {
      const uint32_t bo = RS(ins);
      const uint32_t bi = RA(ins);
      // BO bit 2 clear means the branch also decrements CTR, which is not
      // modelled. Treating it as a plain conditional would skip the
      // decrement and loop forever.
      if ((bo & 0x04) == 0)
        break;

      const uint32_t this_pc = start_pc + n * 4;
      int32_t bd = static_cast<int32_t>(ins & 0x0000FFFC);
      if (bd & 0x8000)
        bd -= 0x10000;  // sign extend from 16 bits
      const bool absolute = (ins & 2) != 0;
      const bool link = (ins & 1) != 0;
      const uint32_t target = absolute ? static_cast<uint32_t>(bd) : this_pc + bd;

      if (link)
        EmitSetSpr(body, layout, layout.lr_index, this_pc + 4);

      if (bo & 0x10)
      {
        // Branch always -- the condition is ignored.
        body.push_back(kI32Const);
        PutI32(body, static_cast<int32_t>(target));
        EmitGoTo(body, layout, 0);
      }
      else
      {
        // Taken when the tested bit equals BO bit 3.
        EmitCRBit(body, layout, bi);
        body.push_back(kI32Const);
        PutI32(body, (bo & 0x08) ? 1 : 0);
        body.push_back(kI32Eq);
        body.push_back(kIf);
        body.push_back(kVoid);
        body.push_back(kI32Const);
        PutI32(body, static_cast<int32_t>(target));
        EmitGoTo(body, layout, 1);  // 0 is the if, 1 the enclosing block
        body.push_back(kEnd);
        // Falling through leaves the block's trailing end_pc as the result,
        // which is this instruction's PC plus four.
      }
      handled = true;
      ends_block = true;
      break;
    }

    case 19:  // bclr -- the return instruction
    {
      if (XO(ins) != 16)
        break;
      const uint32_t bo = RS(ins);
      const uint32_t bi = RA(ins);
      if ((bo & 0x04) == 0)
        break;  // CTR-decrementing form, not modelled

      const uint32_t this_pc = start_pc + n * 4;
      const bool link = (ins & 1) != 0;

      // Read the link register BEFORE any write to it: bclrl both branches
      // to the old LR and replaces it, and doing those in the wrong order
      // makes a call return to itself.
      body.push_back(kLocalGet);
      PutU32(body, 0);
      body.push_back(kI32Load);
      PutU32(body, 2);
      PutU32(body, layout.spr_offset + layout.lr_index * 4);
      // The low two bits are not part of the address.
      body.push_back(kI32Const);
      PutI32(body, -4);
      body.push_back(kI32And);
      body.push_back(kLocalSet);
      PutU32(body, 1);

      if (link)
        EmitSetSpr(body, layout, layout.lr_index, this_pc + 4);

      if (bo & 0x10)
      {
        body.push_back(kLocalGet);
        PutU32(body, 1);
        EmitGoTo(body, layout, 0);
      }
      else
      {
        EmitCRBit(body, layout, bi);
        body.push_back(kI32Const);
        PutI32(body, (bo & 0x08) ? 1 : 0);
        body.push_back(kI32Eq);
        body.push_back(kIf);
        body.push_back(kVoid);
        body.push_back(kLocalGet);
        PutU32(body, 1);
        EmitGoTo(body, layout, 1);
        body.push_back(kEnd);
      }
      used_locals = used_locals < 2 ? 2 : used_locals;
      handled = true;
      ends_block = true;
      break;
    }

    case 37:  // stwu rS, d(rA) -- store and write the address back
    {
      // rA == 0 is an invalid form: there would be no register to update.
      if (RA(ins) == 0)
        break;
      const uint32_t this_pc = start_pc + n * 4;
      EmitEffectiveAddress(body, layout, RA(ins), SIMM(ins), 1);
      EmitGuard(body, layout, 1, this_pc, n);
      // Keep the GUEST address: it is what gets written back, not the host
      // address the store uses.
      body.push_back(kLocalGet);
      PutU32(body, 1);
      body.push_back(kLocalSet);
      PutU32(body, 3);
      EmitToHostAddress(body, layout, 1);
      LoadGpr(body, layout, RS(ins));
      body.push_back(kLocalSet);
      PutU32(body, 2);
      for (int byte = 0; byte < 4; ++byte)
      {
        body.push_back(kLocalGet);
        PutU32(body, 1);
        body.push_back(kLocalGet);
        PutU32(body, 2);
        const int shift = 24 - byte * 8;
        if (shift)
        {
          body.push_back(kI32Const);
          PutI32(body, shift);
          body.push_back(kI32ShrU);
        }
        body.push_back(kI32Store8);
        PutU32(body, 0);
        PutU32(body, static_cast<uint32_t>(byte));
      }
      BeginStoreGpr(body);
      body.push_back(kLocalGet);
      PutU32(body, 3);
      EndStoreGpr(body, layout, RA(ins));
      used_locals = used_locals < 4 ? 4 : used_locals;
      handled = true;
      break;
    }

    case 32:  // lwz rD, d(rA)
    case 34:  // lbz rD, d(rA)
    {
      const uint32_t this_pc = start_pc + n * 4;
      EmitEffectiveAddress(body, layout, RA(ins), SIMM(ins), 1);
      EmitGuard(body, layout, 1, this_pc, n);
      EmitToHostAddress(body, layout, 1);
      BeginStoreGpr(body);
      if (op == 32)
      {
        EmitLoadBE32(body, 1);
      }
      else
      {
        body.push_back(kLocalGet);
        PutU32(body, 1);
        body.push_back(kI32Load8U);
        PutU32(body, 0);
        PutU32(body, 0);
      }
      EndStoreGpr(body, layout, RS(ins));
      used_locals = 2;
      handled = true;
      break;
    }

    case 36:  // stw rS, d(rA)
    case 38:  // stb rS, d(rA)
    {
      const uint32_t this_pc = start_pc + n * 4;
      EmitEffectiveAddress(body, layout, RA(ins), SIMM(ins), 1);
      EmitGuard(body, layout, 1, this_pc, n);
      EmitToHostAddress(body, layout, 1);
      // Keep the value in a local: a byte-order store reads it four times,
      // and the wasm stack cannot be indexed.
      LoadGpr(body, layout, RS(ins));
      body.push_back(kLocalSet);
      PutU32(body, 2);
      if (op == 36)
      {
        for (int byte = 0; byte < 4; ++byte)
        {
          body.push_back(kLocalGet);
          PutU32(body, 1);
          body.push_back(kLocalGet);
          PutU32(body, 2);
          const int shift = 24 - byte * 8;
          if (shift)
          {
            body.push_back(kI32Const);
            PutI32(body, shift);
            body.push_back(kI32ShrU);
          }
          body.push_back(kI32Store8);
          PutU32(body, 0);
          PutU32(body, static_cast<uint32_t>(byte));
        }
      }
      else
      {
        body.push_back(kLocalGet);
        PutU32(body, 1);
        body.push_back(kLocalGet);
        PutU32(body, 2);
        body.push_back(kI32Store8);
        PutU32(body, 0);
        PutU32(body, 0);
      }
      used_locals = 3;
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
    if (ends_block)
    {
      ++n;
      stopped = "branch";
      break;
    }
  }

  BlockResult r;
  r.instructions = n;
  r.end_pc = start_pc + n * 4;
  r.stopped_on = stopped;
  if (n == 0)
    return r;  // nothing compiled; module stays empty

  if (layout.enable_linking)
  {
    // Wrap everything so a branch can leave the instruction stream without
    // leaving the function, then run the shared epilogue exactly once.
    Bytes wrapped;
    wrapped.push_back(kBlock);
    wrapped.push_back(kVoid);
    PutBytes(wrapped, body);
    // Fall-through case: continue at the instruction after the block.
    wrapped.push_back(kI32Const);
    PutI32(wrapped, static_cast<int32_t>(r.end_pc));
    wrapped.push_back(kLocalSet);
    PutU32(wrapped, 4);
    wrapped.push_back(kEnd);
    EmitLinkEpilogue(wrapped, layout, n);
    body = std::move(wrapped);
    used_locals = used_locals < 6 ? 6 : used_locals;
  }
  else
  {
    // The block's result is the PC to resume at.
    body.push_back(kI32Const);
    PutI32(body, static_cast<int32_t>(r.end_pc));
  }

  ModuleBuilder mb;
  mb.ImportMemory(layout.memory_pages, layout.memory_pages, layout.memory_shared);
  if (layout.enable_linking)
    mb.ImportTable();
  mb.SetFunction({kI32}, {kI32}, body, "f", used_locals);
  r.module = mb.Build();
  return r;
}
}  // namespace ppc2wasm
