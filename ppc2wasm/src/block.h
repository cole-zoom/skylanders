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
#include <string_view>
#include <vector>

#include "wasm_emit.h"

namespace ppc2wasm
{
struct BlockResult
{
  Bytes module;           // a complete wasm module, or empty if nothing compiled
  // The same code as a bare function BODY, with the locals it needs.
  //
  // A module costs about 22 us to compile and instantiate almost regardless
  // of its size, so that price is paid per MODULE, not per block. Handing
  // back the body lets a caller pack many blocks into one module and pay it
  // once -- and two blocks in the same module can call each other directly,
  // with no table and no PC compare.
  Bytes body;
  uint32_t locals = 0;
  uint32_t instructions;  // PowerPC instructions translated
  uint32_t end_pc;        // where the interpreter should resume
  const char* stopped_on; // why the block ended, for diagnostics

  // Where control can go from here, when that is knowable at compile time.
  //
  // This is what lets a caller compile a REGION: follow the successors,
  // compile them together, and pay one module install for all of them.
  // Only STATIC targets appear -- the fall-through, and a branch
  // displacement that is part of the instruction. A bclr or bcctr target
  // lives in a register and is not knowable here, so it is simply absent;
  // a region is an optimisation and an incomplete successor set costs
  // nothing but a smaller batch.
  //
  // These are guesses about reachability, not claims about it. The caller
  // must bound them to mapped memory before reading guest code at one.
  uint32_t succ[2] = {0, 0};
  uint32_t nsucc = 0;

  // Whether this block performs any guest memory access, and so depends on
  // MSR[DR] being set and the BATs being the ones it was compiled under.
  // The check is the caller's -- see the note in CompileBlock.
  bool touches_memory = false;
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
  uint32_t msr_offset = 0;        // offsetof(PowerPCState, msr)
  uint32_t downcount_offset = 0;  // offsetof(PowerPCState, downcount)
  uint32_t link_base = 0;         // host address of the link table
  uint32_t link_mask = 0;         // entry count - 1, entries are {pc, index}
  // Optional counters, as four consecutive u32: hits, link attempts,
  // budget exits, and exits taken because the lookup missed. Counting all
  // four is what separates "chains are short" from "the lookup never
  // matches" from "the budget runs out" -- three different causes with three
  // different fixes.
  uint32_t link_stats_addr = 0;
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

// The body of a refusal, assuming an `if` is already open.
inline void EmitBailBody(Bytes& b, const StateLayout& l, uint32_t bail_pc, uint32_t insts_before)
{
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
  EmitBailBody(b, l, bail_pc, insts_before);
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

// Write CR0 the way an Rc (dot) form does.
//
// NOT via s_crTable, which is what every other condition-register write
// here uses. Interpreter::Helper_UpdateCR0 builds the 64-bit encoding
// directly out of the value instead:
//
//   cr_val = (s64)(s32)value
//   if (value == 0)  cr_val |= 1 << 63
//   cr_val = (cr_val & ~(1 << 59)) | (XER[SO] << 59)
//
// Those two encodings are equivalent to every READER -- LT is bit 62, GT
// is (s64)v > 0, EQ is the low word being zero, SO is bit 59 -- and they
// are different 64-bit numbers. Compiled code shares guest state with the
// interpreter and the differential compares cr.fields byte for byte, so
// the only correct choice is the one the interpreter makes.
//
// The zero case is why bit 63 is forced: GT is "sign clear AND non-zero",
// so setting the SO bit on an all-zero field would otherwise turn a result
// of zero into GT.
//
// Built from two i32 halves rather than one i64, matching the rest of the
// emitter: the low half is the value, the high half carries the sign, the
// zero marker and SO.
inline void EmitUpdateCR0(Bytes& b, const StateLayout& l, uint32_t reg)
{
  b.push_back(kLocalGet);
  PutU32(b, 0);
  LoadGpr(b, l, reg);
  b.push_back(kI32Store);
  PutU32(b, 2);
  PutU32(b, l.cr_offset + 0);

  b.push_back(kLocalGet);
  PutU32(b, 0);
  // sign extension into the high half
  LoadGpr(b, l, reg);
  b.push_back(kI32Const);
  PutI32(b, 31);
  b.push_back(kI32ShrS);
  // | (value == 0) << 31, which is bit 63 of the field
  LoadGpr(b, l, reg);
  b.push_back(kI32Eqz);
  b.push_back(kI32Const);
  PutI32(b, 31);
  b.push_back(kI32Shl);
  b.push_back(kI32Or);
  // clear bit 59, which is bit 27 of the high half
  b.push_back(kI32Const);
  PutI32(b, ~(1 << 27));
  b.push_back(kI32And);
  // | XER[SO], stored as (SO << 1) | OV in a byte of guest state
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
  b.push_back(kI32Const);
  PutI32(b, 27);
  b.push_back(kI32Shl);
  b.push_back(kI32Or);
  b.push_back(kI32Store);
  PutU32(b, 2);
  PutU32(b, l.cr_offset + 4);
}

// Which register an Rc form writes, or -1 when the form has no Rc.
//
// The destination is NOT always the same field: the arithmetic forms write
// rD and the logical ones write rA, which is the field everything else
// reads as a source. Getting that backwards silently updates CR0 from an
// operand instead of the result.
inline int RcDestination(uint32_t ins)
{
  const uint32_t op = Opcode(ins);
  if (op == 21)  // rlwinm
    return static_cast<int>(RA(ins));
  if (op != 31)
    return -1;
  switch (XO(ins))
  {
  case 266:  // add
  case 40:   // subf
  case 235:  // mullw
  case 104:  // neg
    return static_cast<int>(RS(ins));
  case 28:   // and
  case 444:  // or
  case 316:  // xor
  case 476:  // nand
  case 124:  // nor
  case 60:   // andc
  case 412:  // orc
  case 284:  // eqv
  case 24:   // slw
  case 536:  // srw
  case 954:  // extsb
  case 922:  // extsh
  case 26:   // cntlzw
    return static_cast<int>(RA(ins));
  default:
    return -1;
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

// ── the BO field, and what a conditional branch actually tests ──
//
// PowerPC's BO is five bits, numbered 0..4 from the most significant. Here
// they appear as the low five bits of the RS field, so BO0 is 0x10:
//
//   0x10  BO0  ignore the condition register entirely
//   0x08  BO1  the CR bit value to branch on
//   0x04  BO2  do NOT decrement CTR
//   0x02  BO3  with CTR: branch when it is zero (set) or non-zero (clear)
//   0x01  BO4  a prediction hint, which means nothing here
//
// The branch is taken when BOTH tests pass, and the CTR decrement happens
// whether or not it is. This mirrors Interpreter::bcx exactly; the shape of
// that function is the specification.
//
// Blocks used to refuse every form with BO2 clear. Measured over an
// 800M-instruction boot, that was 3.68 million interpreted instructions --
// the largest single entry in the interpreter fallback -- and worse than
// its share suggests, because bdnz is the BACK EDGE of a counted loop. An
// uncompiled back edge means every iteration leaves compiled code,
// interprets one instruction and re-enters, which caps chain length at one
// loop body no matter how well the link table performs.
inline void EmitDecrementCtr(Bytes& b, const StateLayout& l)
{
  b.push_back(kLocalGet);
  PutU32(b, 0);
  b.push_back(kLocalGet);
  PutU32(b, 0);
  b.push_back(kI32Load);
  PutU32(b, 2);
  PutU32(b, l.spr_offset + l.ctr_index * 4);
  b.push_back(kI32Const);
  PutI32(b, 1);
  b.push_back(kI32Sub);
  b.push_back(kI32Store);
  PutU32(b, 2);
  PutU32(b, l.spr_offset + l.ctr_index * 4);
}

// Push 1 if the branch should be taken, 0 if not.
//
// Emits the CTR decrement first when BO asks for it, because the decrement
// is unconditional and the test reads the DECREMENTED value.
inline void EmitBranchTaken(Bytes& b, const StateLayout& l, uint32_t bo, uint32_t bi)
{
  const bool tests_ctr = (bo & 0x04) == 0;
  const bool tests_cr = (bo & 0x10) == 0;

  if (tests_ctr)
    EmitDecrementCtr(b, l);

  if (tests_ctr)
  {
    b.push_back(kLocalGet);
    PutU32(b, 0);
    b.push_back(kI32Load);
    PutU32(b, 2);
    PutU32(b, l.spr_offset + l.ctr_index * 4);
    if (bo & 0x02)
    {
      b.push_back(kI32Eqz);  // branch when the counter reached zero
    }
    else
    {
      b.push_back(kI32Const);
      PutI32(b, 0);
      b.push_back(kI32Ne);  // ... or when it has not
    }
  }

  if (tests_cr)
  {
    EmitCRBit(b, l, bi);
    b.push_back(kI32Const);
    PutI32(b, (bo & 0x08) ? 1 : 0);
    b.push_back(kI32Eq);
  }

  if (tests_ctr && tests_cr)
    b.push_back(kI32And);
  else if (!tests_ctr && !tests_cr)
  {
    // Neither test applies: an unconditional branch written the long way.
    b.push_back(kI32Const);
    PutI32(b, 1);
  }
}

// Increment one of the link counters.
inline void EmitBumpStat(Bytes& b, const StateLayout& l, uint32_t slot)
{
  if (!l.link_stats_addr)
    return;
  const uint32_t addr = l.link_stats_addr + slot * 4;
  b.push_back(kI32Const);
  PutI32(b, static_cast<int32_t>(addr));
  b.push_back(kI32Const);
  PutI32(b, static_cast<int32_t>(addr));
  b.push_back(kI32Load);
  PutU32(b, 2);
  PutU32(b, 0);
  b.push_back(kI32Const);
  PutI32(b, 1);
  b.push_back(kI32Add);
  b.push_back(kI32Store);
  PutU32(b, 2);
  PutU32(b, 0);
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
inline void EmitGoTo(Bytes& b, const StateLayout& l, uint32_t br_depth, uint32_t insts_so_far)
{
  if (!l.enable_linking)
  {
    b.push_back(kReturn);
    return;
  }
  b.push_back(kLocalSet);
  PutU32(b, 4);
  // How many instructions this particular exit has run.
  //
  // It used to be a constant folded into the epilogue, which was correct
  // only while a block had exactly one exit. A block that continues past a
  // conditional branch has several, each having run a different number of
  // instructions, and charging the budget for the whole block at an early
  // exit would advance the emulated clock for work that never happened.
  b.push_back(kI32Const);
  PutI32(b, static_cast<int32_t>(insts_so_far));
  b.push_back(kLocalSet);
  PutU32(b, 7);
  b.push_back(kBr);
  PutU32(b, br_depth);
}

inline void EmitLinkEpilogue(Bytes& b, const StateLayout& l)
{
  // downcount -= local 7, the count the exiting path parked there
  b.push_back(kLocalGet);
  PutU32(b, 0);
  b.push_back(kLocalGet);
  PutU32(b, 0);
  b.push_back(kI32Load);
  PutU32(b, 2);
  PutU32(b, l.downcount_offset);
  b.push_back(kLocalGet);
  PutU32(b, 7);
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
  EmitBumpStat(b, l, 2);  // budget exhausted
  b.push_back(kLocalGet);
  PutU32(b, 4);
  b.push_back(kReturn);
  b.push_back(kEnd);

  EmitBumpStat(b, l, 1);  // reached the lookup

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
  EmitBumpStat(b, l, 0);  // hit
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
  EmitBumpStat(b, l, 3);
  b.push_back(kLocalGet);
  PutU32(b, 4);
}

// One emitter for every integer load and store.
//
// PowerPC spells these many ways -- displacement or indexed addressing, one
// two or four bytes, zero or sign extended, with or without writing the
// address back -- but they differ only in those four axes. Writing them out
// separately is how a decoder ends up with the byte order right in three
// places and wrong in the fourth.
//
// Locals: 1 is the address, 2 the value, 3 the guest address kept for an
// update form.
inline void EmitMemOp(Bytes& b, const StateLayout& l, uint32_t this_pc, uint32_t insts_before,
                      uint32_t ra, uint32_t rb, int32_t disp, bool indexed, uint32_t reg,
                      int size, bool is_store, bool sign_extend, bool update)
{
  // Effective address.
  LoadGprOrZero(b, l, ra);
  if (indexed)
  {
    LoadGpr(b, l, rb);
    b.push_back(kI32Add);
  }
  else if (disp)
  {
    b.push_back(kI32Const);
    PutI32(b, disp);
    b.push_back(kI32Add);
  }
  b.push_back(kLocalSet);
  PutU32(b, 1);

  EmitGuard(b, l, 1, this_pc, insts_before);

  if (update)
  {
    // The GUEST address is written back, not the host one.
    b.push_back(kLocalGet);
    PutU32(b, 1);
    b.push_back(kLocalSet);
    PutU32(b, 3);
  }

  EmitToHostAddress(b, l, 1);

  if (is_store)
  {
    LoadGpr(b, l, reg);
    b.push_back(kLocalSet);
    PutU32(b, 2);
    for (int byte = 0; byte < size; ++byte)
    {
      b.push_back(kLocalGet);
      PutU32(b, 1);
      b.push_back(kLocalGet);
      PutU32(b, 2);
      const int shift = (size - 1 - byte) * 8;
      if (shift)
      {
        b.push_back(kI32Const);
        PutI32(b, shift);
        b.push_back(kI32ShrU);
      }
      b.push_back(kI32Store8);
      PutU32(b, 0);
      PutU32(b, static_cast<uint32_t>(byte));
    }
  }
  else
  {
    BeginStoreGpr(b);
    for (int byte = 0; byte < size; ++byte)
    {
      b.push_back(kLocalGet);
      PutU32(b, 1);
      b.push_back(kI32Load8U);
      PutU32(b, 0);
      PutU32(b, static_cast<uint32_t>(byte));
      const int shift = (size - 1 - byte) * 8;
      if (shift)
      {
        b.push_back(kI32Const);
        PutI32(b, shift);
        b.push_back(kI32Shl);
      }
      if (byte)
        b.push_back(kI32Or);
    }
    if (sign_extend && size < 4)
    {
      const int bits = 32 - size * 8;
      b.push_back(kI32Const);
      PutI32(b, bits);
      b.push_back(kI32Shl);
      b.push_back(kI32Const);
      PutI32(b, bits);
      b.push_back(kI32ShrS);
    }
    EndStoreGpr(b, l, reg);
  }

  if (update)
  {
    BeginStoreGpr(b);
    b.push_back(kLocalGet);
    PutU32(b, 3);
    EndStoreGpr(b, l, ra);
  }
}
}  // namespace detail

// Translate instructions starting at `code[0]` (guest address `start_pc`)
// until one is not understood or `max_insts` is reached.
// emit_module builds the single-function module in BlockResult::module.
// A caller that is packing bodies into a shared module does not need it,
// and building one is a second pass over the code plus the section
// scaffolding around it.
inline BlockResult CompileBlock(const uint32_t* code, uint32_t start_pc, uint32_t max_insts,
                                const StateLayout& layout, bool emit_module = true)
{
  using namespace detail;
  Bytes body;
  uint32_t n = 0;
  // Scratch locals above the state parameter: 1 = effective/host address,
  // 2 = the value being stored. Declared only if used, so a block of pure
  // register arithmetic still emits none.
  uint32_t used_locals = 0;
  // Whether the block performs any guest memory access. Such a block is only
  // valid while data address translation is on, and that is checked once at
  // entry rather than at every access.
  bool touches_memory = false;
  const char* stopped = "instruction limit";
  // Static successors, collected as they are found.
  //
  // BOUNDS-CHECKED through one helper rather than by each caller. A block
  // used to end at the first branch, so at most one site ever wrote here;
  // a superblock runs through conditional branches and can reach several
  // of them before an unconditional one, and the first version of that
  // change overflowed this array on the very first region the boot
  // compiled -- a stack smash that presented as "memory access out of
  // bounds" from inside the compiler, nowhere near the code it emits.
  uint32_t succ[2] = {0, 0};
  uint32_t nsucc = 0;
  auto add_succ = [&succ, &nsucc](uint32_t p) {
    if (nsucc < 2)
      succ[nsucc++] = p;
  };

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
      // Rc is allowed only for the forms whose destination register is
      // known. For everything else in this opcode space -- memory, SPR
      // moves, mtmsr -- bit 0 is reserved, and a set bit means an encoding
      // this does not understand.
      if (Rc(ins) && RcDestination(ins) < 0)
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

      // dcbz is NOT translated, despite being cheap to emit.
      //
      // It raises an alignment exception when HID0[DCE] is clear
      // (Interpreter_LoadStore.cpp:539) and takes a Dolphin-specific path
      // when data cache emulation is off. Emitting a plain 32-byte zero is
      // right only when the cache is enabled, and wrong silently otherwise --
      // the first test written for it showed the interpreter at the alignment
      // vector while the compiled block zeroed memory and carried on.

      // ── integer loads and stores, indexed form ──
      {
        struct XForm { int size; bool store, sign, update; };
        XForm xf{};
        bool is_mem = true;
        switch (xo)
        {
        case 23:  xf = {4, false, false, false}; break;  // lwzx
        case 55:  xf = {4, false, false, true};  break;  // lwzux
        case 87:  xf = {1, false, false, false}; break;  // lbzx
        case 119: xf = {1, false, false, true};  break;  // lbzux
        case 279: xf = {2, false, false, false}; break;  // lhzx
        case 311: xf = {2, false, false, true};  break;  // lhzux
        case 343: xf = {2, false, true,  false}; break;  // lhax
        case 375: xf = {2, false, true,  true};  break;  // lhaux
        case 151: xf = {4, true,  false, false}; break;  // stwx
        case 183: xf = {4, true,  false, true};  break;  // stwux
        case 215: xf = {1, true,  false, false}; break;  // stbx
        case 247: xf = {1, true,  false, true};  break;  // stbux
        case 407: xf = {2, true,  false, false}; break;  // sthx
        case 439: xf = {2, true,  false, true};  break;  // sthux
        default: is_mem = false; break;
        }
        if (is_mem)
        {
          if (xf.update && RA(ins) == 0)
            break;
          EmitMemOp(body, layout, start_pc + n * 4, n, RA(ins), RB(ins), 0, true, RS(ins),
                    xf.size, xf.store, xf.sign, xf.update);
      touches_memory = true;
          used_locals = used_locals < 4 ? 4 : used_locals;
          handled = true;
          break;
        }
      }

      // ── single-operand and complement forms ──
      //
      // All pure register arithmetic: no memory, no carry, no condition
      // register. They are implemented together because each is two or three
      // wasm instructions and because the LONG TAIL is what caps block
      // length -- at 17% unsupported, a block truncates after about six
      // instructions no matter which 17% it is.
      if (xo == 954 || xo == 922)  // extsb, extsh
      {
        const int bits = (xo == 954) ? 24 : 16;
        BeginStoreGpr(body);
        LoadGpr(body, layout, RS(ins));
        body.push_back(kI32Const);
        PutI32(body, bits);
        body.push_back(kI32Shl);
        body.push_back(kI32Const);
        PutI32(body, bits);
        body.push_back(kI32ShrS);  // arithmetic shift back sign-extends
        EndStoreGpr(body, layout, RA(ins));
        handled = true;
        break;
      }
      if (xo == 104)  // neg rD, rA
      {
        BeginStoreGpr(body);
        body.push_back(kI32Const);
        PutI32(body, 0);
        LoadGpr(body, layout, RA(ins));
        body.push_back(kI32Sub);
        EndStoreGpr(body, layout, RS(ins));
        handled = true;
        break;
      }
      if (xo == 26)  // cntlzw rA, rS
      {
        BeginStoreGpr(body);
        LoadGpr(body, layout, RS(ins));
        body.push_back(kI32Clz);
        EndStoreGpr(body, layout, RA(ins));
        handled = true;
        break;
      }
      if (xo == 60 || xo == 412 || xo == 124 || xo == 476 || xo == 284)
      {
        // andc, orc, nor, nand, eqv -- a plain operation with one or both
        // sides complemented. WebAssembly has no NOT, so each complement is
        // an xor with -1.
        auto emit_not = [&] {
          body.push_back(kI32Const);
          PutI32(body, -1);
          body.push_back(kI32Xor);
        };
        BeginStoreGpr(body);
        LoadGpr(body, layout, RS(ins));
        if (xo == 284)  // eqv = ~(a ^ b)
        {
          LoadGpr(body, layout, RB(ins));
          body.push_back(kI32Xor);
          emit_not();
        }
        else
        {
          LoadGpr(body, layout, RB(ins));
          if (xo == 60 || xo == 412)
            emit_not();  // andc / orc complement only the second operand
          body.push_back((xo == 60) ? kI32And
                                    : (xo == 412) ? kI32Or
                                                  : (xo == 124) ? kI32Or : kI32And);
          if (xo == 124 || xo == 476)
            emit_not();  // nor / nand complement the result
        }
        EndStoreGpr(body, layout, RA(ins));
        handled = true;
        break;
      }
      if (xo == 235)  // mullw rD, rA, rB -- low 32 bits of the product
      {
        BeginStoreGpr(body);
        LoadGpr(body, layout, RA(ins));
        LoadGpr(body, layout, RB(ins));
        body.push_back(kI32Mul);
        EndStoreGpr(body, layout, RS(ins));
        handled = true;
        break;
      }

      if (xo == 83)  // mfmsr rD
      {
        // A pure read of the machine state register -- except that it is
        // PRIVILEGED. In user mode it raises a program exception instead,
        // which this cannot do, so the block refuses in that case and the
        // interpreter takes the exception properly.
        //
        // Game code runs supervisor-side, so the guard is expected never to
        // fire. It is emitted anyway: "this bit is never set in practice" is
        // how a recompiler acquires a bug that only appears in the one place
        // it matters.
        //
        // mtmsr is NOT translated. Writing the register requires
        // PowerPC::MSRUpdated() -- address translation and the exception
        // check are recomputed from it -- and that is host code a compiled
        // block cannot call.
        body.push_back(kLocalGet);
        PutU32(body, 0);
        body.push_back(kI32Load);
        PutU32(body, 2);
        PutU32(body, layout.msr_offset);
        body.push_back(kI32Const);
        PutI32(body, 1 << 14);  // MSR[PR]
        body.push_back(kI32And);
        body.push_back(kIf);
        body.push_back(kVoid);
        EmitBailBody(body, layout, start_pc + n * 4, n);
        body.push_back(kEnd);

        BeginStoreGpr(body);
        body.push_back(kLocalGet);
        PutU32(body, 0);
        body.push_back(kI32Load);
        PutU32(body, 2);
        PutU32(body, layout.msr_offset);
        EndStoreGpr(body, layout, RS(ins));
        handled = true;
        break;
      }

      if (xo == 146)  // mtmsr rS
      {
        // The write itself is compiled; everything that must happen AFTER it
        // is handed back to the dispatcher.
        //
        // The interpreter does four things here: write the register, call
        // PowerPC::MSRUpdated(), check floating-point exceptions, and call
        // CheckExceptions() -- which matters because setting MSR[EE] has to
        // take any interrupt that was already pending. None of that is
        // reachable from inside a wasm module.
        //
        // So the block exits with bit 1 of the returned PC set, meaning "the
        // machine state register changed". PowerPC instructions are
        // four-byte aligned, so bits 0 and 1 are both free: bit 0 already
        // means a bail, and this is the second marker. The dispatcher does
        // the rest, and the interpreter ends its own block here too
        // (Interpreter_SystemRegisters.cpp sets m_end_block), so this is not
        // a shorter block than the interpreter would have run.
        body.push_back(kLocalGet);
        PutU32(body, 0);
        body.push_back(kI32Load);
        PutU32(body, 2);
        PutU32(body, layout.msr_offset);
        body.push_back(kI32Const);
        PutI32(body, 1 << 14);  // MSR[PR] -- privileged, as mfmsr
        body.push_back(kI32And);
        body.push_back(kIf);
        body.push_back(kVoid);
        EmitBailBody(body, layout, start_pc + n * 4, n);
        body.push_back(kEnd);

        // msr = gpr[rS]
        body.push_back(kLocalGet);
        PutU32(body, 0);
        LoadGpr(body, layout, RS(ins));
        body.push_back(kI32Store);
        PutU32(body, 2);
        PutU32(body, layout.msr_offset);

        // Leave immediately, marked. This instruction DID run, so with
        // linking the budget must account for it.
        if (layout.enable_linking)
        {
          body.push_back(kLocalGet);
          PutU32(body, 0);
          body.push_back(kLocalGet);
          PutU32(body, 0);
          body.push_back(kI32Load);
          PutU32(body, 2);
          PutU32(body, layout.downcount_offset);
          body.push_back(kI32Const);
          PutI32(body, static_cast<int32_t>(n + 1));
          body.push_back(kI32Sub);
          body.push_back(kI32Store);
          PutU32(body, 2);
          PutU32(body, layout.downcount_offset);
        }
        body.push_back(kI32Const);
        PutI32(body, static_cast<int32_t>((start_pc + (n + 1) * 4) | 2u));
        body.push_back(kReturn);
        add_succ(start_pc + (n + 1) * 4);
        handled = true;
        ends_block = true;
        break;
      }

      if (xo == 24 || xo == 536)  // slw, srw
      {
        // PowerPC uses a SIX-bit shift count and yields zero when it is 32 or
        // more. WebAssembly's i32.shl masks the count to five bits, so a
        // shift of 32 becomes a shift of 0 -- the value passes through
        // unchanged where the guest expects it cleared.
        //
        // The result is masked by (((rB >> 5) & 1) - 1), which is 0 when bit
        // five is set and -1 otherwise. Branchless, and it costs three
        // instructions on a path that is common enough to matter.
        BeginStoreGpr(body);
        LoadGpr(body, layout, RS(ins));
        LoadGpr(body, layout, RB(ins));
        body.push_back(kI32Const);
        PutI32(body, 31);
        body.push_back(kI32And);
        body.push_back((xo == 24) ? kI32Shl : kI32ShrU);
        LoadGpr(body, layout, RB(ins));
        body.push_back(kI32Const);
        PutI32(body, 5);
        body.push_back(kI32ShrU);
        body.push_back(kI32Const);
        PutI32(body, 1);
        body.push_back(kI32And);
        body.push_back(kI32Const);
        PutI32(body, 1);
        body.push_back(kI32Sub);
        body.push_back(kI32And);
        EndStoreGpr(body, layout, RA(ins));
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
      EmitGoTo(body, layout, 0, n + 1);
      add_succ(target);
      // A call does not fall through, but its RETURN SITE is a block entry
      // and a hot one, so it is worth compiling in the same region.
      if (link)
        add_succ(this_pc + 4);
      handled = true;
      ends_block = true;
      break;
    }

    case 16:  // bc / bca / bcl / bcla
    {
      const uint32_t bo = RS(ins);
      const uint32_t bi = RA(ins);
      const uint32_t this_pc = start_pc + n * 4;
      int32_t bd = static_cast<int32_t>(ins & 0x0000FFFC);
      if (bd & 0x8000)
        bd -= 0x10000;  // sign extend from 16 bits
      const bool absolute = (ins & 2) != 0;
      const bool link = (ins & 1) != 0;
      const uint32_t target = absolute ? static_cast<uint32_t>(bd) : this_pc + bd;

      // The link register is written INSIDE the taken path, not before the
      // test. bcl only sets LR when it branches -- Interpreter::bcx writes
      // it within "if (counter && condition)" -- and writing it
      // unconditionally corrupts the return address of whatever call the
      // not-taken path eventually returns from.
      if ((bo & 0x14) == 0x14)
      {
        // Neither CTR nor the condition register is consulted.
        if (link)
          EmitSetSpr(body, layout, layout.lr_index, this_pc + 4);
        body.push_back(kI32Const);
        PutI32(body, static_cast<int32_t>(target));
        EmitGoTo(body, layout, 0, n + 1);
      }
      else
      {
        EmitBranchTaken(body, layout, bo, bi);
        body.push_back(kIf);
        body.push_back(kVoid);
        if (link)
          EmitSetSpr(body, layout, layout.lr_index, this_pc + 4);
        body.push_back(kI32Const);
        PutI32(body, static_cast<int32_t>(target));
        EmitGoTo(body, layout, 1, n + 1);  // 0 is the if, 1 the enclosing block
        body.push_back(kEnd);
        // Falling through leaves the block's trailing end_pc as the result,
        // which is this instruction's PC plus four.
      }
      if (nsucc < 2)
        add_succ(target);
      handled = true;
      // A CONDITIONAL branch does not end the block.
      //
      // The taken edge has just exited; what follows the branch in memory
      // is the not-taken edge, and it is the next instruction either way.
      // Stopping here was costing the single largest line in the profile:
      // 277.8 million block entries at 2.83 guest instructions each, every
      // one paying a function prologue, the MSR[DR] guard and the whole
      // link epilogue for three instructions of work.
      //
      // Continuing makes this a superblock -- one entry, several exits. A
      // guest that jumps to the not-taken edge directly gets its own block
      // compiled there, so the code is duplicated rather than shared. That
      // is the standard trade and it is the cheap side of it.
      ends_block = ((bo & 0x14) == 0x14);
      break;
    }

    case 19:  // bclr (return) and bcctr (indirect branch)
    {
      const bool via_ctr = (XO(ins) == 528);
      if (XO(ins) != 16 && !via_ctr)
        break;
      const uint32_t bo = RS(ins);
      const uint32_t bi = RA(ins);
      // bcctr with BO2 clear is architecturally invalid -- CTR cannot be
      // both the counter and the target -- and Dolphin only asserts on it.
      // bclr with BO2 clear is bdnzlr and is perfectly ordinary.
      if (via_ctr && (bo & 0x04) == 0)
        break;

      const uint32_t this_pc = start_pc + n * 4;
      const bool link = (ins & 1) != 0;

      // Read the target register BEFORE any write to the link register:
      // bclrl both branches to the old LR and replaces it, and doing those in
      // the wrong order makes a call return to itself. bcctr reads CTR, which
      // the link write cannot disturb, but the same shape is used so there is
      // one ordering to get right rather than two.
      body.push_back(kLocalGet);
      PutU32(body, 0);
      body.push_back(kI32Load);
      PutU32(body, 2);
      PutU32(body, layout.spr_offset +
                       (via_ctr ? layout.ctr_index : layout.lr_index) * 4);
      // The low two bits are not part of the address.
      body.push_back(kI32Const);
      PutI32(body, -4);
      body.push_back(kI32And);
      body.push_back(kLocalSet);
      PutU32(body, 1);

      // As in bc: the link write belongs to the TAKEN path only. The target
      // was already read into local 1 above, so writing LR here cannot
      // disturb it -- which is the whole reason the read came first.
      if ((bo & 0x14) == 0x14)
      {
        if (link)
          EmitSetSpr(body, layout, layout.lr_index, this_pc + 4);
        body.push_back(kLocalGet);
        PutU32(body, 1);
        EmitGoTo(body, layout, 0, n + 1);
      }
      else
      {
        EmitBranchTaken(body, layout, bo, bi);
        body.push_back(kIf);
        body.push_back(kVoid);
        if (link)
          EmitSetSpr(body, layout, layout.lr_index, this_pc + 4);
        body.push_back(kLocalGet);
        PutU32(body, 1);
        EmitGoTo(body, layout, 1, n + 1);
        body.push_back(kEnd);
      }
      used_locals = used_locals < 2 ? 2 : used_locals;
      handled = true;
      // Same as bc: a conditional return exits on one edge and carries on
      // along the other. An unconditional one really does end the block,
      // and its target is in a register, so it reports no successor.
      ends_block = ((bo & 0x14) == 0x14);
      break;
    }

    case 7:  // mulli rD, rA, SIMM
      BeginStoreGpr(body);
      LoadGpr(body, layout, RA(ins));
      body.push_back(kI32Const);
      PutI32(body, SIMM(ins));
      body.push_back(kI32Mul);
      EndStoreGpr(body, layout, RS(ins));
      handled = true;
      break;

    // ── integer loads and stores, displacement form ──
    case 32: case 33: case 34: case 35: case 40: case 41: case 42: case 43:
    case 36: case 37: case 38: case 39: case 44: case 45:
    {
      struct Form { int size; bool store, sign, update; };
      Form form{};
      switch (op)
      {
      case 32: form = {4, false, false, false}; break;  // lwz
      case 33: form = {4, false, false, true};  break;  // lwzu
      case 34: form = {1, false, false, false}; break;  // lbz
      case 35: form = {1, false, false, true};  break;  // lbzu
      case 40: form = {2, false, false, false}; break;  // lhz
      case 41: form = {2, false, false, true};  break;  // lhzu
      case 42: form = {2, false, true,  false}; break;  // lha
      case 43: form = {2, false, true,  true};  break;  // lhau
      case 36: form = {4, true,  false, false}; break;  // stw
      case 37: form = {4, true,  false, true};  break;  // stwu
      case 38: form = {1, true,  false, false}; break;  // stb
      case 39: form = {1, true,  false, true};  break;  // stbu
      case 44: form = {2, true,  false, false}; break;  // sth
      case 45: form = {2, true,  false, true};  break;  // sthu
      default: break;
      }
      // An update form with rA == 0 has no register to write back.
      if (form.update && RA(ins) == 0)
        break;
      EmitMemOp(body, layout, start_pc + n * 4, n, RA(ins), 0, SIMM(ins), false, RS(ins),
                form.size, form.store, form.sign, form.update);
      touches_memory = true;
      used_locals = used_locals < 4 ? 4 : used_locals;
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
    // The dot forms set CR0 from the result they just wrote. Emitted after
    // the instruction rather than inside each handler, because the rule is
    // one rule and the handlers are seventeen.
    if (Rc(ins))
    {
      const int rc_dest = RcDestination(ins);
      if (rc_dest >= 0)
        EmitUpdateCR0(body, layout, static_cast<uint32_t>(rc_dest));
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
  // Static branch targets first, then the fall-through.
  //
  // Only an UNCONDITIONAL exit ends a block now, so "ended on a branch"
  // means control genuinely cannot continue past it. Everything else runs
  // off the end of the block and into the next instruction -- past the one
  // that was refused, when that is why it stopped, because the interpreter
  // will have run exactly that one.
  //
  // The distinction matters most for an unconditional blr or bcctr: those
  // record no successor on purpose, because the target is in a register,
  // and treating that as a fall-through would hand the caller the address
  // after a return instruction, which is usually the next function's first
  // byte and is not reachable from here at all.
  const bool ended_on_branch = (stopped == std::string_view("branch"));
  uint32_t out = 0;
  for (uint32_t i = 0; i < nsucc && out < 2; ++i)
    r.succ[out++] = succ[i];
  if (!ended_on_branch && n != 0 && out < 2)
    r.succ[out++] = (stopped == std::string_view("unsupported instruction")) ? r.end_pc + 4
                                                                             : r.end_pc;
  r.nsucc = out;
  if (n == 0)
    return r;  // nothing compiled; module stays empty

  r.touches_memory = touches_memory;

  // ── the translation guard is the CALLER's, once per chain ──
  //
  // Every memory access this block emits maps a guest address with
  // ram_base + (ea & mask), which is the right translation only while
  // MSR[DR] is set and the BATs are the ones the boot installed. A block
  // compiled under those conditions can be reached later under different
  // ones, because the cache is keyed by PC and not by machine state.
  //
  // That check used to be a prologue on every memory-touching block --
  // five wasm instructions executed at every entry, and entries are the
  // largest line in the profile at 261 million of them.
  //
  // It does not need to be per block, because MSR cannot change inside a
  // chain. The only instruction that writes it is mtmsr, which ends its
  // block AND returns to the dispatcher rather than continuing, precisely
  // so the interpreter can recompute translation state and take any
  // interrupt that became deliverable. Compiled code raises no exceptions
  // of its own, and CoreTiming only advances when a chain returns. So DR
  // is constant for the whole of a chain, and one check before entering
  // it is exactly as strong as one check per block inside it.
  //
  // The caller must make that check. BlockResult::touches_memory says
  // which blocks care; a caller that cannot track it per chain should
  // check DR before entering ANY compiled block, which is what the
  // dispatcher does.

  if (layout.enable_linking)
  {
    // Wrap everything so a branch can leave the instruction stream without
    // leaving the function, then run the shared epilogue exactly once.
    Bytes wrapped;
    wrapped.push_back(kBlock);
    wrapped.push_back(kVoid);
    PutBytes(wrapped, body);
    // Fall-through case: continue at the instruction after the block,
    // having run all of it.
    wrapped.push_back(kI32Const);
    PutI32(wrapped, static_cast<int32_t>(r.end_pc));
    wrapped.push_back(kLocalSet);
    PutU32(wrapped, 4);
    wrapped.push_back(kI32Const);
    PutI32(wrapped, static_cast<int32_t>(n));
    wrapped.push_back(kLocalSet);
    PutU32(wrapped, 7);
    wrapped.push_back(kEnd);
    EmitLinkEpilogue(wrapped, layout);
    body = std::move(wrapped);
    used_locals = used_locals < 8 ? 8 : used_locals;
  }
  else
  {
    // The block's result is the PC to resume at.
    body.push_back(kI32Const);
    PutI32(body, static_cast<int32_t>(r.end_pc));
  }

  r.locals = used_locals;
  if (!emit_module)
  {
    r.body = std::move(body);
    return r;
  }

  r.body = body;
  ModuleBuilder mb;
  mb.ImportMemory(layout.memory_pages, layout.memory_pages, layout.memory_shared);
  if (layout.enable_linking)
    mb.ImportTable();
  mb.SetFunction({kI32}, {kI32}, body, "f", used_locals);
  r.module = mb.Build();
  return r;
}
}  // namespace ppc2wasm
