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
#include <cstring>
#include <functional>
#include <string_view>
#include <vector>

#include "wasm_emit.h"

namespace ppc2wasm
{
// A reserved six-byte slot in a body where a direct tail call may go.
//
// The index of a sibling function is not known while a block is being
// emitted -- the region it belongs to has not finished being discovered --
// so the call site is emitted as a hole and filled in afterwards. Six bytes
// is `return_call` plus a five-byte LEB128 index, which is the widest a u32
// index can be; an unfilled hole becomes six `nop`s and the code falls
// through to the ordinary table lookup.
struct DirectFixup
{
  uint32_t offset;  // into BlockResult::body
  uint32_t target;  // the guest PC this call would go to
};

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
  // 64-bit scratch locals, declared after the i32 ones. Non-zero only for
  // blocks with floating-point or multi-word forms, and then the i32 count
  // is always 8 so that these are locals 9 and 10.
  uint32_t locals_i64 = 0;
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

  // Direct-call holes in `body`, to be filled by PatchDirectCalls once the
  // caller knows which guest PCs became which function indices. Empty
  // unless StateLayout::allow_direct is set.
  std::vector<DirectFixup> fixups;
};

// Fill the direct-call holes a block left behind.
//
// `lookup(target_pc)` returns the function index of that block within the
// SAME module, or a negative number when it is not there. A hole left
// unfilled stays six nops and the emitted fallback runs: the block sets up
// the next PC and drops through to the table lookup exactly as it would
// have without any of this.
//
// The index is written as a FIVE-BYTE LEB128 even when it would fit in
// one. WebAssembly allows a padded encoding -- the grammar only requires
// the final byte to carry the remaining bits -- and a fixed width is what
// makes patching possible at all, because the hole was reserved before the
// value was known.
template <typename F>
inline uint32_t PatchDirectCalls(Bytes& body, const std::vector<DirectFixup>& fixups, F lookup)
{
  uint32_t patched = 0;
  for (const DirectFixup& f : fixups)
  {
    const int index = lookup(f.target);
    if (index < 0)
      continue;
    uint32_t v = static_cast<uint32_t>(index);
    body[f.offset] = kReturnCall;
    for (uint32_t i = 0; i < 5; ++i)
    {
      const uint8_t seven = static_cast<uint8_t>(v & 0x7F);
      v >>= 7;
      body[f.offset + 1 + i] = (i == 4) ? seven : static_cast<uint8_t>(seven | 0x80);
    }
    ++patched;
  }
  return patched;
}

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
  // ── more of the address space than MEM1 ──
  //
  // When non-zero, the host address of a 128-entry table of u32 host bases
  // indexed by the top seven bits of a guest address (32 MB granules). An
  // access translates as table[ea >> 25] + (ea & 0x01FFFFFF), and a zero
  // entry means "not memory this code may touch" and bails exactly as the
  // MEM1 guard did. One table load replaces one compare, and the same code
  // now reaches MEM1, MEM2 and both of their uncached mirrors, which is
  // where a Wii game keeps its thread stacks and most of its heap.
  //
  // Measured with idle loops skipped, 21 of 40 million interpreted
  // instructions were loads and stores to MEM2, interpreted forever
  // because the block holding them refused on its first instruction and
  // was retired. This is what that measurement asked for.
  //
  // Zero keeps the MEM1-only guard, which the existing differentials were
  // written against.
  uint32_t region_table = 0;
  uint32_t xer_ca_offset = 0;    // offsetof(PowerPCState, xer_ca)
  uint32_t xer_stringctrl_offset = 0;  // offsetof(PowerPCState, xer_stringctrl)
  // Floating point. ps[] is 32 PairedSingle of two u64 each (ps0 at +0,
  // ps1 at +8); fpscr is the 32-bit status register. Zero disables every
  // floating-point form, since 0 is the PC's offset.
  uint32_t ps_offset = 0;
  uint32_t fpscr_offset = 0;
  // The reservation lwarx sets and stwcx. tests: a bool and a u32 in guest
  // state. Zero disables both forms.
  uint32_t reserve_offset = 0;
  uint32_t reserve_address_offset = 0;
  // The time base, for mftb. Four host addresses into CoreTiming's Globals
  // struct, read at run time: the value is start_value + (ticks -
  // start_ticks) / 12, where ticks = global_timer + slice_length -
  // downcount, exactly as CoreTiming::GetTicks and SystemTimers::
  // GetFakeTimeBase compute it. This assumes the CPU overclock factor is
  // 1.0, which is what DowncountToCycles multiplies by; the caller leaves
  // these zero when it is not, and mftb goes to the interpreter.
  uint32_t tb_global_timer_addr = 0;
  uint32_t tb_slice_length_addr = 0;
  uint32_t tb_start_value_addr = 0;
  uint32_t tb_start_ticks_addr = 0;
  // When the data cache is being emulated, dcbz and the cache-control
  // instructions go through Dolphin's cache model and are left to the
  // interpreter. When it is not, dcbf/dcbst/dcbi/dcbt are no-ops here,
  // exactly as they are in the interpreter, and dcbz zeroes 32 bytes.
  bool dcache_enabled = false;
  // Dolphin's "low dcbz hack" turns dcbz into a no-op below 0x80008000.
  // A game-specific option; when it is on, dcbz is left to the interpreter.
  bool low_dcbz_hack = false;

  uint32_t cr_offset = 0;        // offsetof(PowerPCState, cr)
  uint32_t xer_so_ov_offset = 0; // offsetof(PowerPCState, xer_so_ov)
  uint32_t cr_table_base = 0;    // host address of ConditionRegister::s_crTable
  uint32_t spr_offset = 0;       // offsetof(PowerPCState, spr)
  uint32_t lr_index = 8;         // SPR_LR
  uint32_t ctr_index = 9;        // SPR_CTR

  // When set, a block FOLLOWS a branch whose target is known at compile
  // time instead of ending at it. One entry then covers what used to be
  // two or more.
  //
  // Measured over an 800M-instruction boot, of 261.5 million block
  // entries: 126.8M left by a taken conditional branch, 40.2M by a call,
  // 25.4M by an unconditional branch, 2.2M by running off the end. Those
  // are not blocks that ran out of things to do -- they are blocks that
  // stopped at a transfer whose destination was sitting in the
  // instruction.
  bool trace_uncond = false;    // follow b and bl
  bool trace_backward = false;  // follow the taken edge of a backward branch

  // When set, an exit whose target is known at compile time reserves a
  // hole for a direct tail call to it. The caller fills the hole for
  // targets that ended up in the same module and leaves the rest as nops.
  //
  // Off by default: a hole that nobody patches is correct but pointless,
  // and a caller that does not call PatchDirectCalls should not pay for
  // the fallback code around it.
  bool allow_direct = false;

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

  // ── mtmsr without leaving the chain ──
  //
  // Measured over an 800M-instruction boot, 24.4 million of 32.5 million
  // dispatcher round-trips were mtmsr exits: the OS toggles MSR[EE] around
  // every critical section, and each toggle used to cost a full trip
  // through C++ -- MSRUpdated, CheckExceptions, the cache lookup and a
  // fresh indirect call -- at roughly 25 ns each, which is more than the
  // instructions around it cost to run.
  //
  // Almost none of those trips did anything. What the interpreter does
  // after the write only MATTERS when (a) a bit that changes address
  // translation or FP exception mode flipped, (b) EE was enabled while an
  // interrupt is pending, or (c) a page table update is waiting on DR.
  // With this set, the block tests exactly those three things and only
  // exits when one holds; otherwise the chain carries on. The dispatcher
  // still does the full fix-up on the exits that remain.
  bool mtmsr_continue = false;
  uint32_t exceptions_offset = 0;         // offsetof(PowerPCState, Exceptions)
  uint32_t pagetable_pending_offset = 0;  // offsetof(PowerPCState, pagetable_update_pending)

  // ── idle loops ──
  //
  // The 800M-instruction boot ends in `lwz r0,0x248(r13); cmpwi r0,0;
  // beq -8`, and that loop is about half of everything executed. Nothing
  // inside it can change its own outcome: only an interrupt can set the
  // flag, and interrupts only arrive when the chain returns and the
  // emulated clock advances. Spinning until the budget runs out is
  // therefore pure waste, and every emulator skips it -- Dolphin's
  // IsBusyWaitLoop is the rule copied here.
  //
  // When set, a block whose closing conditional branch targets its own
  // first instruction, and whose body is loads and integer arithmetic
  // that never write a register they read first, takes a different taken
  // edge: it writes 1 to this address and returns its own PC. The
  // dispatcher then hands the rest of the slice to CoreTiming::Idle, so
  // the next event fires immediately instead of after a few hundred more
  // iterations of a loop whose result is already known.
  uint32_t idle_flag_addr = 0;

  // ── the interpreter, in place ──
  //
  // When set, an instruction this compiler has no translation for does not
  // end the block: the block calls an imported host function,
  //
  //   env.ppc_interp_op(state, instruction, pc) -> i32
  //
  // which runs the interpreter's own handler for that one instruction on
  // the shared guest state, exactly as Interpreter::SingleStepInner would,
  // and returns non-zero when control did not simply fall through -- an
  // exception was taken, or the instruction branched. The block then
  // leaves with whatever PC the interpreter set. Otherwise the block
  // carries on as if it had translated the instruction itself.
  //
  // Measured over 3 billion instructions with the game past its boot, 52
  // million instructions were interpreted, each one ending a block, going
  // through the dispatcher and paying the interpreter's full per-step
  // overhead. Most were floating point, and all of them run through this
  // call instead -- with the interpreter's semantics, bit for bit, because
  // it IS the interpreter.
  //
  // The import is function index 0, so every direct call between blocks
  // must add one (ModuleBuilder::ImportCount) to the index it patches in.
  // Branches, mtmsr and the forms translated inline never take this path.
  bool interp_hatch = false;
  uint32_t pc_offset = 0;  // offsetof(PowerPCState, pc)
  // With the hatch, the paired fused multiply-adds (ps_madds0/1, ps_madd,
  // ps_msub, ps_nmadd, ps_nmsub) inline for float-exact operands, the
  // hatch for the rest; see EmitPsMadd. Off, all of them go through the
  // hatch as before.
  bool paired_madd = false;
  // With the hatch, a load or store whose address is in the hardware
  // register space -- 0xCC00xxxx and 0xCD00xxxx, above all the GX
  // write-gather pipe at 0xCC008000 that every graphics command goes
  // through -- runs through the interpreter in place instead of bailing.
  // Other addresses outside RAM still bail. A bail ended the chain
  // and, on the first instruction of a block, retired the block for good:
  // measured past the boot, the stores to the gather pipe were a third
  // of everything still interpreted. Off, the bail stays, which the chain
  // differential relies on to exercise that path.
  bool hatch_mmio = false;

  // ── quantized loads and stores ──
  //
  // psq_l and psq_st take their format from a GQR: a type (float, or a
  // signed or unsigned byte or halfword) and a power-of-two scale, one
  // pair of each for loads and for stores. The registers are written by
  // the game's startup and then read a billion times, so Dolphin's own
  // JIT specialises on the value a GQR holds when the block is compiled
  // and checks it at run time; this does the same. When non-null, the
  // eight GQR values to specialise against, read at compile time. The
  // emitted code compares the live register with the value it was built
  // for and, when they differ, runs the instruction through the hatch --
  // so the result is exact whatever the game does to its GQRs, and only
  // the speed depends on them holding still. Null leaves every psq form
  // to the hatch.
  //
  // Measured past the boot, in the tutorial level with the character on
  // screen: of 5.8 billion instructions that still went through the
  // hatch, psq_l was 29% and psq_st 14%.
  const uint32_t* gqr_values = nullptr;

  // ── the write-gather pipe ──
  //
  // Every GX command the CPU issues is a store to one 4 KB page of
  // hardware space. The interpreter appends the bytes to a 32-byte buffer
  // in host memory and, when the buffer is full, flushes it to the FIFO
  // (GPFifoManager::UpdateGatherPipe), which is a real piece of work.
  // When gather_pipe_ea is non-zero, a store whose guest address lies in
  // that page and which does NOT fill the buffer is done inline: the
  // bytes go to gather_pipe_ptr, in guest byte order, and the pointer
  // advances. A store that would fill the buffer takes the hatch as
  // before, so the flush stays the interpreter's and the two paths meet
  // at the same buffer state.
  //
  // The address compared is the GUEST address, before translation; the
  // harness sets this only when the DBATs map the page to the pipe
  // (MMU::IsOptimizableGatherPipeWrite), which the Wii's standard BATs
  // do and a game is not expected to change.
  //
  // Measured as above: stb and stw to the pipe were 14% of the hatch.
  uint32_t gather_pipe_ea = 0;
  uint32_t gather_pipe_ptr_offset = 0;   // offsetof(PowerPCState, gather_pipe_ptr)
  uint32_t gather_pipe_base_offset = 0;  // offsetof(PowerPCState, gather_pipe_base_ptr)

  // ── the locked L1 cache ──
  //
  // 256 KB of data cache a game can lock and DMA into, which every game
  // places at 0xE0000000 (Dolphin: "Locked L1 technically doesn't have a
  // fixed address, but games all use 0xE0000000"). Dolphin keeps it as a
  // separate host buffer, and the MMU reaches it when the DBATs map
  // 0xE0000000 and the address is below 0xE0040000. Too small for a
  // 32 MB region-table granule, so it gets its own compare on the miss
  // path: when non-zero, the host address of that buffer, and an address
  // in [0xE0000000, 0xE0040000) translates to it. The caller sets this
  // only while the DBATs map the range (MMU::IsOptimizableRAMAddress),
  // which the game does at its OS init and never undoes.
  //
  // Found by the retirement list past the boot: the character skinning
  // loop streams its vertices through the locked cache, and every one of
  // its quantized loads and stores bailed -- 450 million instructions
  // interpreted one at a time over 390 emulated seconds, 99% of
  // everything still interpreted.
  uint32_t l1_cache_base = 0;
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

// Emit: push 1 if the guest address in ea_local is in the hardware
// register space -- 0xCC00xxxx, 0xCD00xxxx or 0xCD80xxxx -- which is the
// only space outside RAM the interpreter can be handed an access to. The
// EFB (0xC8000000) needs a video backend, and an address in the gaps
// between the device blocks indexes the MMIO handler table out of bounds
// in a release build. The hatch takes the first; everything else bails.
inline void EmitIsMmio(Bytes& b, uint32_t ea_local)
{
  b.push_back(kLocalGet);
  PutU32(b, ea_local);
  b.push_back(kI32Const);
  // 0xCC00xxxx and 0xCD00xxxx differ in bit 24, which this clears. (It
  // used to clear bit 23, which merged 0xCC00 with 0xCC80 instead and
  // sent every 0xCD00xxxx access -- the Hollywood registers -- to a bail;
  // the retirement list showed lwzx/stwx at 0xCD00002E interpreted a
  // million times.)
  PutI32(b, static_cast<int32_t>(0xFEFF0000u));
  b.push_back(kI32And);
  b.push_back(kI32Const);
  PutI32(b, static_cast<int32_t>(0xCC000000u));
  b.push_back(kI32Eq);
  b.push_back(kLocalGet);
  PutU32(b, ea_local);
  b.push_back(kI32Const);
  PutI32(b, static_cast<int32_t>(0xFFFF0000u));
  b.push_back(kI32And);
  b.push_back(kI32Const);
  PutI32(b, static_cast<int32_t>(0xCD800000u));
  b.push_back(kI32Eq);
  b.push_back(kI32Or);
}

// Call the interpreter for `ins` at `pc` (StateLayout::interp_hatch) and,
// if control did not simply advance, charge the budget and leave with the
// PC the interpreter set. Leaves nothing on the stack.
inline void EmitHatchCall(Bytes& b, const StateLayout& l, uint32_t ins, uint32_t pc,
                          uint32_t insts_before)
{
  b.push_back(kLocalGet);
  PutU32(b, 0);
  b.push_back(kI32Const);
  PutI32(b, static_cast<int32_t>(ins));
  b.push_back(kI32Const);
  PutI32(b, static_cast<int32_t>(pc));
  b.push_back(kCall);
  PutU32(b, 0);
  b.push_back(kIf);
  b.push_back(kVoid);
  if (l.enable_linking)
  {
    b.push_back(kLocalGet);
    PutU32(b, 0);
    b.push_back(kLocalGet);
    PutU32(b, 0);
    b.push_back(kI32Load);
    PutU32(b, 2);
    PutU32(b, l.downcount_offset);
    b.push_back(kI32Const);
    PutI32(b, static_cast<int32_t>(insts_before + 1));
    b.push_back(kI32Sub);
    b.push_back(kI32Store);
    PutU32(b, 2);
    PutU32(b, l.downcount_offset);
  }
  b.push_back(kLocalGet);
  PutU32(b, 0);
  b.push_back(kI32Load);
  PutU32(b, 2);
  PutU32(b, l.pc_offset);
  b.push_back(kReturn);
  b.push_back(kEnd);
}

// A store that may take the gather-pipe fast path (StateLayout::
// gather_pipe_ea): `size` bytes, which `emit` writes in guest byte order
// at the host address held in the local it is handed. The translation's
// miss path is the only user; the caller promises `ptr_local` is free.
struct GatherPipeStore
{
  int size = 0;
  uint32_t ptr_local = 0;
  std::function<void(Bytes&, uint32_t)> emit;
};

// Check the guest address in ea_local and replace it with the host address,
// through the region table when there is one and the MEM1 guard otherwise.
//
// When `ins` is non-zero and the hatch handles MMIO, an address outside RAM
// runs the whole instruction through the interpreter and then branches
// `skip_depth` levels out -- past the rest of the instruction's own code,
// which the caller has wrapped in a block for the purpose. A store with a
// GatherPipeStore is first tried against the pipe, the same way out.
// `scratch_local` holds the table entry for a moment; it may be any local
// the caller is not using yet.
inline void EmitTranslate(Bytes& b, const StateLayout& l, uint32_t ea_local,
                          uint32_t scratch_local, uint32_t bail_pc, uint32_t insts_before,
                          uint32_t ins = 0, uint32_t skip_depth = 0,
                          const GatherPipeStore* gp = nullptr)
{
  const bool via_hatch = l.interp_hatch && l.hatch_mmio && ins != 0;
  // The gather pipe, inside the miss branch: the address is in the pipe's
  // page and the buffer has room for the whole store without filling.
  // Then the bytes go to the buffer, the pointer moves on, and the rest
  // of the instruction is skipped exactly as after the hatch.
  auto emit_gather_pipe = [&]() {
    if (!gp || !l.gather_pipe_ea)
      return;
    b.push_back(kLocalGet);
    PutU32(b, ea_local);
    b.push_back(kI32Const);
    PutI32(b, static_cast<int32_t>(0xFFFFF000u));
    b.push_back(kI32And);
    b.push_back(kI32Const);
    PutI32(b, static_cast<int32_t>(l.gather_pipe_ea));
    b.push_back(kI32Eq);
    b.push_back(kLocalGet);
    PutU32(b, 0);
    b.push_back(kI32Load);
    PutU32(b, 2);
    PutU32(b, l.gather_pipe_ptr_offset);
    b.push_back(kLocalTee);
    PutU32(b, gp->ptr_local);
    b.push_back(kLocalGet);
    PutU32(b, 0);
    b.push_back(kI32Load);
    PutU32(b, 2);
    PutU32(b, l.gather_pipe_base_offset);
    b.push_back(kI32Sub);
    b.push_back(kI32Const);
    PutI32(b, 32 - gp->size);  // GPFifo::GATHER_PIPE_SIZE
    b.push_back(kI32LtU);
    b.push_back(kI32And);
    b.push_back(kIf);
    b.push_back(kVoid);
    gp->emit(b, gp->ptr_local);
    b.push_back(kLocalGet);
    PutU32(b, 0);
    b.push_back(kLocalGet);
    PutU32(b, gp->ptr_local);
    b.push_back(kI32Const);
    PutI32(b, gp->size);
    b.push_back(kI32Add);
    b.push_back(kI32Store);
    PutU32(b, 2);
    PutU32(b, l.gather_pipe_ptr_offset);
    b.push_back(kBr);
    PutU32(b, skip_depth + 2);  // out of this if, the miss if, and the caller's block
    b.push_back(kEnd);
  };
  if (!l.region_table)
  {
    // The MEM1-only guard, with the same choice of what to do on a miss.
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
    emit_gather_pipe();
    if (via_hatch)
    {
      EmitIsMmio(b, ea_local);
      b.push_back(kIf);
      b.push_back(kVoid);
      EmitHatchCall(b, l, ins, bail_pc, insts_before);
      b.push_back(kBr);
      PutU32(b, skip_depth + 2);  // out of this if, the outer if, and the caller's block
      b.push_back(kEnd);
    }
    EmitBailBody(b, l, bail_pc, insts_before);
    b.push_back(kEnd);
    EmitToHostAddress(b, l, ea_local);
    return;
  }
  // base = table[ea >> 25]
  b.push_back(kLocalGet);
  PutU32(b, ea_local);
  b.push_back(kI32Const);
  PutI32(b, 25);
  b.push_back(kI32ShrU);
  b.push_back(kI32Const);
  PutI32(b, 2);
  b.push_back(kI32Shl);
  b.push_back(kI32Const);
  PutI32(b, static_cast<int32_t>(l.region_table));
  b.push_back(kI32Add);
  b.push_back(kI32Load);
  PutU32(b, 2);
  PutU32(b, 0);
  b.push_back(kLocalTee);
  PutU32(b, scratch_local);
  b.push_back(kI32Eqz);
  b.push_back(kIf);
  b.push_back(kVoid);
  if (l.l1_cache_base)
  {
    // The locked cache: (ea - 0xE0000000) < 0x40000. Its base goes where
    // the table entry would have been, and since the range's low 25 bits
    // are its offset, the tail below computes the right host address.
    b.push_back(kLocalGet);
    PutU32(b, ea_local);
    b.push_back(kI32Const);
    PutI32(b, static_cast<int32_t>(0xE0000000u));
    b.push_back(kI32Sub);
    b.push_back(kI32Const);
    PutI32(b, 0x40000);
    b.push_back(kI32LtU);
    b.push_back(kIf);
    b.push_back(kVoid);
    b.push_back(kI32Const);
    PutI32(b, static_cast<int32_t>(l.l1_cache_base));
    b.push_back(kLocalSet);
    PutU32(b, scratch_local);
    b.push_back(kBr);
    PutU32(b, 1);  // out of the miss branch, to the tail
    b.push_back(kEnd);
  }
  emit_gather_pipe();
  if (via_hatch)
  {
    // The hardware registers go to the interpreter; anything else outside
    // RAM still bails, exactly as without the hatch.
    EmitIsMmio(b, ea_local);
    b.push_back(kIf);
    b.push_back(kVoid);
    EmitHatchCall(b, l, ins, bail_pc, insts_before);
    b.push_back(kBr);
    PutU32(b, skip_depth + 2);  // out of this if, the outer if, and the caller's block
    b.push_back(kEnd);
  }
  EmitBailBody(b, l, bail_pc, insts_before);
  b.push_back(kEnd);
  // ea = base + (ea & 0x01FFFFFF)
  b.push_back(kLocalGet);
  PutU32(b, scratch_local);
  b.push_back(kLocalGet);
  PutU32(b, ea_local);
  b.push_back(kI32Const);
  PutI32(b, 0x01FFFFFF);
  b.push_back(kI32And);
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
  if (op == 21 || op == 20 || op == 23)  // rlwinm, rlwimi, rlwnm
    return static_cast<int>(RA(ins));
  if (op != 31)
    return -1;
  switch (XO(ins))
  {
  case 266:  // add
  case 40:   // subf
  case 235:  // mullw
  case 104:  // neg
  case 10:   // addc
  case 8:    // subfc
  case 138:  // adde
  case 136:  // subfe
  case 202:  // addze
  case 200:  // subfze
  case 491:  // divw
  case 459:  // divwu
  case 75:   // mulhw
  case 11:   // mulhwu
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
  case 824:  // srawi
  case 792:  // sraw
    return static_cast<int>(RA(ins));
  default:
    return -1;
  }
}

// Store a 0/1 already on the stack into XER[CA]. The address must have been
// pushed BEFORE the value, so callers emit `local.get 0` first, then the
// value, then this.
inline void EmitEndStoreCA(Bytes& b, const StateLayout& l)
{
  b.push_back(kI32Store8);
  PutU32(b, 0);
  PutU32(b, l.xer_ca_offset);
}

inline void EmitLoadCA(Bytes& b, const StateLayout& l)
{
  b.push_back(kLocalGet);
  PutU32(b, 0);
  b.push_back(kI32Load8U);
  PutU32(b, 0);
  PutU32(b, l.xer_ca_offset);
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

// Increment a link counter by a signed amount.
inline void EmitBumpStatBy(Bytes& b, const StateLayout& l, uint32_t slot, int32_t delta)
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
  PutI32(b, delta);
  b.push_back(kI32Add);
  b.push_back(kI32Store);
  PutU32(b, 2);
  PutU32(b, 0);
}

// An exit whose target is a compile-time constant.
//
// Without allow_direct this is what it always was: push the PC and let the
// shared epilogue charge the budget and look the target up.
//
// With it, the budget is charged HERE and a six-byte hole is reserved for
// a direct tail call to the target. Filled, the exit is one instruction --
// no hash, no table load, no stored-PC compare, no runtime type check.
// Unfilled, the hole is six nops and control drops through to exactly the
// fallback that would have run anyway, with the budget charge zeroed so
// the epilogue does not take it twice.
//
// Everything after a filled `return_call` is unreachable. WebAssembly
// validates unreachable code against a polymorphic stack, so the same
// bytes are well-formed whichever way the hole is filled -- which is the
// property that makes patching rather than recompiling possible.
inline void EmitStaticExit(Bytes& b, const StateLayout& l, uint32_t target,
                           uint32_t insts_so_far, uint32_t br_depth,
                           std::vector<DirectFixup>& fixups, uint32_t kind_slot = 0)
{
  // Which kind of exit this is, counted only when the counters are on.
  // Slots 4..7 are free; see the dispatcher for what each means.
  if (kind_slot)
    EmitBumpStatBy(b, l, kind_slot, 1);
  if (!l.enable_linking || !l.allow_direct)
  {
    b.push_back(kI32Const);
    PutI32(b, static_cast<int32_t>(target));
    EmitGoTo(b, l, br_depth, insts_so_far);
    return;
  }

  // downcount -= insts_so_far
  b.push_back(kLocalGet);
  PutU32(b, 0);
  b.push_back(kLocalGet);
  PutU32(b, 0);
  b.push_back(kI32Load);
  PutU32(b, 2);
  PutU32(b, l.downcount_offset);
  b.push_back(kI32Const);
  PutI32(b, static_cast<int32_t>(insts_so_far));
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
  EmitBumpStatBy(b, l, 2, 1);  // budget exhausted
  b.push_back(kI32Const);
  PutI32(b, static_cast<int32_t>(target));
  b.push_back(kReturn);
  b.push_back(kEnd);

  // Counted as a hit up front and taken back below if the hole stays
  // empty, because the code that would know lives on the other side of a
  // tail call that never returns.
  EmitBumpStatBy(b, l, 0, 1);
  EmitBumpStatBy(b, l, 1, 1);

  b.push_back(kLocalGet);
  PutU32(b, 0);  // the successor takes the same state pointer
  DirectFixup fx;
  fx.offset = static_cast<uint32_t>(b.size());
  fx.target = target;
  fixups.push_back(fx);
  for (int i = 0; i < 6; ++i)
    b.push_back(kNop);

  // Only reached when the hole was not filled.
  b.push_back(kDrop);
  EmitBumpStatBy(b, l, 0, -1);
  EmitBumpStatBy(b, l, 1, -1);
  b.push_back(kI32Const);
  PutI32(b, static_cast<int32_t>(target));
  b.push_back(kLocalSet);
  PutU32(b, 4);
  b.push_back(kI32Const);
  PutI32(b, 0);  // the budget is already charged
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

// Byte-swap the i32 in `local`, leaving the result on the stack. See the
// note in EmitMemOp for the rotate trick.
inline void EmitBswap32(Bytes& b, uint32_t local)
{
  b.push_back(kLocalGet);
  PutU32(b, local);
  b.push_back(kI32Const);
  PutI32(b, 0x00FF00FF);
  b.push_back(kI32And);
  b.push_back(kI32Const);
  PutI32(b, 8);
  b.push_back(kI32Rotr);
  b.push_back(kLocalGet);
  PutU32(b, local);
  b.push_back(kI32Const);
  PutI32(b, static_cast<int32_t>(0xFF00FF00u));
  b.push_back(kI32And);
  b.push_back(kI32Const);
  PutI32(b, 8);
  b.push_back(kI32Rotl);
  b.push_back(kI32Or);
}

// Byte-swap the i64 in `local`, leaving the result on the stack: swap
// adjacent bytes, then adjacent halfwords, then the two words.
inline void EmitBswap64(Bytes& b, uint32_t local)
{
  auto step = [&](int64_t mask, int32_t shift) {
    b.push_back(kLocalGet);
    PutU32(b, local);
    b.push_back(kI64Const);
    PutI64(b, mask);
    b.push_back(kI64And);
    b.push_back(kI64Const);
    PutI64(b, shift);
    b.push_back(kI64Shl);
    b.push_back(kLocalGet);
    PutU32(b, local);
    b.push_back(kI64Const);
    PutI64(b, shift);
    b.push_back(kI64ShrU);
    b.push_back(kI64Const);
    PutI64(b, mask);
    b.push_back(kI64And);
    b.push_back(kI64Or);
    b.push_back(kLocalSet);
    PutU32(b, local);
  };
  step(static_cast<int64_t>(0x00FF00FF00FF00FFull), 8);
  step(static_cast<int64_t>(0x0000FFFF0000FFFFull), 16);
  b.push_back(kLocalGet);
  PutU32(b, local);
  b.push_back(kI64Const);
  PutI64(b, 32);
  b.push_back(kI64Rotl);
}

// Dolphin's ConvertToDouble (Interpreter_FPUtils.h), the PowerPC rule for
// loading a single: the i32 in `local` becomes the i64 bit pattern of the
// double on the stack. Not f64.promote_f32 -- that would quiet a
// signalling NaN, and the guest keeps it signalling. Uses i32 locals 5 and
// 6 as scratch; they belong to the link epilogue and are dead here.
inline void EmitConvertToDouble(Bytes& b, uint32_t local)
{
  // exp -> local 5, frac -> local 6
  b.push_back(kLocalGet);
  PutU32(b, local);
  b.push_back(kI32Const);
  PutI32(b, 23);
  b.push_back(kI32ShrU);
  b.push_back(kI32Const);
  PutI32(b, 0xFF);
  b.push_back(kI32And);
  b.push_back(kLocalSet);
  PutU32(b, 5);
  b.push_back(kLocalGet);
  PutU32(b, local);
  b.push_back(kI32Const);
  PutI32(b, 0x007FFFFF);
  b.push_back(kI32And);
  b.push_back(kLocalSet);
  PutU32(b, 6);
  // subnormal: exp == 0 && frac != 0
  b.push_back(kLocalGet);
  PutU32(b, 5);
  b.push_back(kI32Eqz);
  b.push_back(kLocalGet);
  PutU32(b, 6);
  b.push_back(kI32Const);
  PutI32(b, 0);
  b.push_back(kI32Ne);
  b.push_back(kI32And);
  b.push_back(kIf);
  b.push_back(kI64);
  {
    // shift = clz(frac) - 8 brings the leading one to bit 23, which is
    // what the interpreter's loop does one bit at a time.
    b.push_back(kLocalGet);
    PutU32(b, 6);
    b.push_back(kI32Clz);
    b.push_back(kI32Const);
    PutI32(b, 8);
    b.push_back(kI32Sub);
    b.push_back(kLocalSet);
    PutU32(b, 5);
    // ((x & 0x80000000) << 32)
    b.push_back(kLocalGet);
    PutU32(b, local);
    b.push_back(kI32Const);
    PutI32(b, static_cast<int32_t>(0x80000000u));
    b.push_back(kI32And);
    b.push_back(kI64ExtendI32U);
    b.push_back(kI64Const);
    PutI64(b, 32);
    b.push_back(kI64Shl);
    // | ((897 - shift) << 52)
    b.push_back(kI32Const);
    PutI32(b, 897);
    b.push_back(kLocalGet);
    PutU32(b, 5);
    b.push_back(kI32Sub);
    b.push_back(kI64ExtendI32U);
    b.push_back(kI64Const);
    PutI64(b, 52);
    b.push_back(kI64Shl);
    b.push_back(kI64Or);
    // | (((frac << shift) & 0x7fffff) << 29)
    b.push_back(kLocalGet);
    PutU32(b, 6);
    b.push_back(kLocalGet);
    PutU32(b, 5);
    b.push_back(kI32Shl);
    b.push_back(kI32Const);
    PutI32(b, 0x007FFFFF);
    b.push_back(kI32And);
    b.push_back(kI64ExtendI32U);
    b.push_back(kI64Const);
    PutI64(b, 29);
    b.push_back(kI64Shl);
    b.push_back(kI64Or);
  }
  b.push_back(kElse);
  {
    // ((x & 0xc0000000) << 32) | (y * 0x3800000000000000) | ((x & 0x3fffffff) << 29)
    // with y = (exp == 255) | (1 <= exp <= 127): the interpreter's !(exp >> 7)
    // for normals and exp >> 7 for the zero/inf/NaN exponents.
    b.push_back(kLocalGet);
    PutU32(b, local);
    b.push_back(kI32Const);
    PutI32(b, static_cast<int32_t>(0xC0000000u));
    b.push_back(kI32And);
    b.push_back(kI64ExtendI32U);
    b.push_back(kI64Const);
    PutI64(b, 32);
    b.push_back(kI64Shl);
    b.push_back(kLocalGet);
    PutU32(b, 5);
    b.push_back(kI32Const);
    PutI32(b, 255);
    b.push_back(kI32Eq);
    b.push_back(kLocalGet);
    PutU32(b, 5);
    b.push_back(kI32Const);
    PutI32(b, 1);
    b.push_back(kI32Sub);
    b.push_back(kI32Const);
    PutI32(b, 127);
    b.push_back(kI32LtU);
    b.push_back(kI32Or);
    b.push_back(kI64ExtendI32U);
    b.push_back(kI64Const);
    PutI64(b, static_cast<int64_t>(0x3800000000000000ull));
    b.push_back(kI64Mul);
    b.push_back(kI64Or);
    b.push_back(kLocalGet);
    PutU32(b, local);
    b.push_back(kI32Const);
    PutI32(b, 0x3FFFFFFF);
    b.push_back(kI32And);
    b.push_back(kI64ExtendI32U);
    b.push_back(kI64Const);
    PutI64(b, 29);
    b.push_back(kI64Shl);
    b.push_back(kI64Or);
  }
  b.push_back(kEnd);
}

// Dolphin's ConvertToSingle: the i64 double bit pattern in `local` (an i64
// local) becomes the i32 single bit pattern on the stack. Truncation, not
// rounding -- stfs on this CPU drops the extra mantissa bits -- with the
// denormal-single range handled the way hardware tests say it is. Uses i32
// local 5 as scratch.
inline void EmitConvertToSingle(Bytes& b, uint32_t local)
{
  // exp -> local 5
  b.push_back(kLocalGet);
  PutU32(b, local);
  b.push_back(kI64Const);
  PutI64(b, 52);
  b.push_back(kI64ShrU);
  b.push_back(kI32WrapI64);
  b.push_back(kI32Const);
  PutI32(b, 0x7FF);
  b.push_back(kI32And);
  b.push_back(kLocalSet);
  PutU32(b, 5);
  // denormal single: 874 <= exp <= 896, and not (plus or minus) zero
  b.push_back(kLocalGet);
  PutU32(b, 5);
  b.push_back(kI32Const);
  PutI32(b, 874);
  b.push_back(kI32Sub);
  b.push_back(kI32Const);
  PutI32(b, 23);
  b.push_back(kI32LtU);
  b.push_back(kLocalGet);
  PutU32(b, local);
  b.push_back(kI64Const);
  PutI64(b, 0x7FFFFFFFFFFFFFFFll);
  b.push_back(kI64And);
  b.push_back(kI64Eqz);
  b.push_back(kI32Eqz);
  b.push_back(kI32And);
  b.push_back(kIf);
  b.push_back(kI32);
  {
    // t = (0x80000000 | ((x & FRAC) >> 21)) >> (905 - exp); t |= sign
    b.push_back(kLocalGet);
    PutU32(b, local);
    b.push_back(kI64Const);
    PutI64(b, 0x000FFFFFFFFFFFFFll);
    b.push_back(kI64And);
    b.push_back(kI64Const);
    PutI64(b, 21);
    b.push_back(kI64ShrU);
    b.push_back(kI32WrapI64);
    b.push_back(kI32Const);
    PutI32(b, static_cast<int32_t>(0x80000000u));
    b.push_back(kI32Or);
    b.push_back(kI32Const);
    PutI32(b, 905);
    b.push_back(kLocalGet);
    PutU32(b, 5);
    b.push_back(kI32Sub);
    b.push_back(kI32ShrU);
    b.push_back(kLocalGet);
    PutU32(b, local);
    b.push_back(kI64Const);
    PutI64(b, 32);
    b.push_back(kI64ShrU);
    b.push_back(kI32WrapI64);
    b.push_back(kI32Const);
    PutI32(b, static_cast<int32_t>(0x80000000u));
    b.push_back(kI32And);
    b.push_back(kI32Or);
  }
  b.push_back(kElse);
  {
    // ((x >> 32) & 0xc0000000) | ((x >> 29) & 0x3fffffff)
    b.push_back(kLocalGet);
    PutU32(b, local);
    b.push_back(kI64Const);
    PutI64(b, 32);
    b.push_back(kI64ShrU);
    b.push_back(kI32WrapI64);
    b.push_back(kI32Const);
    PutI32(b, static_cast<int32_t>(0xC0000000u));
    b.push_back(kI32And);
    b.push_back(kLocalGet);
    PutU32(b, local);
    b.push_back(kI64Const);
    PutI64(b, 29);
    b.push_back(kI64ShrU);
    b.push_back(kI32WrapI64);
    b.push_back(kI32Const);
    PutI32(b, 0x3FFFFFFF);
    b.push_back(kI32And);
    b.push_back(kI32Or);
  }
  b.push_back(kEnd);
}

// CR1 after a floating-point Rc form: FPSCR[FX, FEX, VX, OX], which are the
// top four bits of FPSCR, through s_crTable (PowerPCState::UpdateCR1).
inline void EmitUpdateCR1(Bytes& b, const StateLayout& l)
{
  b.push_back(kLocalGet);
  PutU32(b, 0);
  b.push_back(kLocalGet);
  PutU32(b, 0);
  b.push_back(kI32Load);
  PutU32(b, 2);
  PutU32(b, l.fpscr_offset);
  b.push_back(kI32Const);
  PutI32(b, 28);
  b.push_back(kI32ShrU);
  b.push_back(kI32Const);
  PutI32(b, 3);
  b.push_back(kI32Shl);
  b.push_back(kI32Const);
  PutI32(b, static_cast<int32_t>(l.cr_table_base));
  b.push_back(kI32Add);
  b.push_back(kI64Load);
  PutU32(b, 3);
  PutU32(b, 0);
  b.push_back(kI64Store);
  PutU32(b, 3);
  PutU32(b, l.cr_offset + 8);
}

// Emit: if (!MSR[FP]) bail. Once per block, before its first floating-point
// instruction: the Wii's OS switches FP contexts lazily, clearing MSR[FP]
// on a thread switch and letting the first FP instruction trap, so the
// bit is genuinely clear sometimes and the interpreter must take that
// exception. FP is also one of the bits an in-chain mtmsr may not change,
// so one check per block covers every FP instruction after it.
inline void EmitFpAvailable(Bytes& b, const StateLayout& l, uint32_t bail_pc,
                            uint32_t insts_before)
{
  b.push_back(kLocalGet);
  PutU32(b, 0);
  b.push_back(kI32Load);
  PutU32(b, 2);
  PutU32(b, l.msr_offset);
  b.push_back(kI32Const);
  PutI32(b, 1 << 13);
  b.push_back(kI32And);
  b.push_back(kI32Eqz);
  b.push_back(kIf);
  b.push_back(kVoid);
  EmitBailBody(b, l, bail_pc, insts_before);
  b.push_back(kEnd);
}

// Emit: if (!HID2[PSE]) bail, for the paired-single forms, which are
// illegal instructions while that bit is clear
// (Interpreter::IsInvalidPairedSingleExecution). HID2 is SPR 920 and PSE
// is bit 29.
inline void EmitPseAvailable(Bytes& b, const StateLayout& l, uint32_t bail_pc,
                             uint32_t insts_before)
{
  b.push_back(kLocalGet);
  PutU32(b, 0);
  b.push_back(kI32Load);
  PutU32(b, 2);
  PutU32(b, l.spr_offset + 920 * 4);
  b.push_back(kI32Const);
  PutI32(b, 1 << 29);
  b.push_back(kI32And);
  b.push_back(kI32Eqz);
  b.push_back(kIf);
  b.push_back(kVoid);
  EmitBailBody(b, l, bail_pc, insts_before);
  b.push_back(kEnd);
}

// Floating-point loads and stores. Alignment is checked as the
// interpreter checks it (a misaligned address is an alignment exception,
// so the block bails and the interpreter raises it); the data is a word
// or doubleword in guest byte order; a single is converted on the way in
// and out with the CPU's own rules; an update form writes the guest
// address back to rA. stfiwx stores the low word of the register as is.
//
// Locals: 1 the address, 2 an i32 scratch, 3 the guest address for an
// update form, 9 and 10 the i64 scratch.
inline void EmitFpMemOp(Bytes& b, const StateLayout& l, uint32_t this_pc, uint32_t insts_before,
                        uint32_t ra, uint32_t rb, int32_t disp, bool indexed, uint32_t freg,
                        bool is_double, bool is_store, bool update, bool as_integer,
                        uint32_t ins = 0)
{
  b.push_back(kBlock);
  b.push_back(kVoid);
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
  // (ea & 3) != 0 -> alignment exception, the interpreter's business
  b.push_back(kLocalGet);
  PutU32(b, 1);
  b.push_back(kI32Const);
  PutI32(b, 3);
  b.push_back(kI32And);
  b.push_back(kIf);
  b.push_back(kVoid);
  EmitBailBody(b, l, this_pc, insts_before);
  b.push_back(kEnd);
  if (update)
  {
    b.push_back(kLocalGet);
    PutU32(b, 1);
    b.push_back(kLocalSet);
    PutU32(b, 3);
  }
  const uint32_t ps0 = l.ps_offset + freg * 16;
  // stfs is how a float reaches the GX command stream (GXPosition3f32 and
  // its kin are a store of each coordinate to the pipe), so a plain
  // single store may take the gather-pipe path, converted exactly as for
  // RAM. Local 3 is free when there is no update.
  GatherPipeStore gp;
  if (is_store && !update && !is_double && !as_integer && l.gather_pipe_ea)
  {
    gp.size = 4;
    gp.ptr_local = 3;
    gp.emit = [&l, ps0](Bytes& bb, uint32_t addr_local) {
      bb.push_back(kLocalGet);
      PutU32(bb, 0);
      bb.push_back(kI64Load);
      PutU32(bb, 3);
      PutU32(bb, ps0);
      bb.push_back(kLocalSet);
      PutU32(bb, 9);
      EmitConvertToSingle(bb, 9);
      bb.push_back(kLocalSet);
      PutU32(bb, 2);
      bb.push_back(kLocalGet);
      PutU32(bb, addr_local);
      EmitBswap32(bb, 2);
      bb.push_back(kI32Store);
      PutU32(bb, 0);
      PutU32(bb, 0);
    };
  }
  EmitTranslate(b, l, 1, 2, this_pc, insts_before, ins, 0, gp.size ? &gp : nullptr);

  if (is_store)
  {
    b.push_back(kLocalGet);
    PutU32(b, 1);  // the address, under the value
    if (as_integer)
    {
      b.push_back(kLocalGet);
      PutU32(b, 0);
      b.push_back(kI32Load);
      PutU32(b, 2);
      PutU32(b, ps0);  // the low word of ps0, which is where the host keeps it
      b.push_back(kLocalSet);
      PutU32(b, 2);
      EmitBswap32(b, 2);
      b.push_back(kI32Store);
      PutU32(b, 0);
      PutU32(b, 0);
    }
    else if (is_double)
    {
      b.push_back(kLocalGet);
      PutU32(b, 0);
      b.push_back(kI64Load);
      PutU32(b, 3);
      PutU32(b, ps0);
      b.push_back(kLocalSet);
      PutU32(b, 9);
      EmitBswap64(b, 9);
      b.push_back(kI64Store);
      PutU32(b, 0);
      PutU32(b, 0);
    }
    else
    {
      b.push_back(kLocalGet);
      PutU32(b, 0);
      b.push_back(kI64Load);
      PutU32(b, 3);
      PutU32(b, ps0);
      b.push_back(kLocalSet);
      PutU32(b, 9);
      EmitConvertToSingle(b, 9);
      b.push_back(kLocalSet);
      PutU32(b, 2);
      EmitBswap32(b, 2);
      b.push_back(kI32Store);
      PutU32(b, 0);
      PutU32(b, 0);
    }
  }
  else if (is_double)
  {
    // lfd writes ps0 only.
    b.push_back(kLocalGet);
    PutU32(b, 0);
    b.push_back(kLocalGet);
    PutU32(b, 1);
    b.push_back(kI64Load);
    PutU32(b, 0);
    PutU32(b, 0);
    b.push_back(kLocalSet);
    PutU32(b, 9);
    EmitBswap64(b, 9);
    b.push_back(kI64Store);
    PutU32(b, 3);
    PutU32(b, ps0);
  }
  else
  {
    // lfs fills BOTH halves with the converted double.
    b.push_back(kLocalGet);
    PutU32(b, 1);
    b.push_back(kI32Load);
    PutU32(b, 0);
    PutU32(b, 0);
    b.push_back(kLocalSet);
    PutU32(b, 2);
    EmitBswap32(b, 2);
    b.push_back(kLocalSet);
    PutU32(b, 2);
    EmitConvertToDouble(b, 2);
    b.push_back(kLocalSet);
    PutU32(b, 9);
    b.push_back(kLocalGet);
    PutU32(b, 0);
    b.push_back(kLocalGet);
    PutU32(b, 9);
    b.push_back(kI64Store);
    PutU32(b, 3);
    PutU32(b, ps0);
    b.push_back(kLocalGet);
    PutU32(b, 0);
    b.push_back(kLocalGet);
    PutU32(b, 9);
    b.push_back(kI64Store);
    PutU32(b, 3);
    PutU32(b, ps0 + 8);
  }

  if (update)
  {
    BeginStoreGpr(b);
    b.push_back(kLocalGet);
    PutU32(b, 3);
    EndStoreGpr(b, l, ra);
  }
  b.push_back(kEnd);
}

// ── quantized paired-single loads and stores ──
//
// Interpreter_LoadStorePaired.cpp is the specification. A GQR holds, for
// each direction, a type and a six-bit scale: type 0 is a float (two
// singles, converted exactly as lfs and stfs convert), 4 to 7 are an
// unsigned byte, an unsigned halfword, a signed byte and a signed
// halfword, and 1 to 3 are undefined. The scale is a signed six-bit
// exponent k: a load multiplies by 2^-k, a store by 2^k, in single
// precision, where the product is exact. A store of an integer type
// rounds the double to a single, scales, clamps to the type's range and
// truncates toward zero; NaN clamps to itself and converts to 0, which
// is what the interpreter's cast does on this host. A load of an integer
// type leaves ps1 at 1.0 when W selects one half.
//
// The code is specialised on the GQR's value at compile time
// (StateLayout::gqr_values) and guarded: when the live register differs,
// the hatch runs the instruction instead.

// Dolphin's ConvertToSingleFTZ: ConvertToSingle, except that a double too
// small for a single becomes a signed zero rather than a denormal.
// Uses local 5 as scratch, like the exact conversion.
inline void EmitConvertToSingleFTZ(Bytes& b, uint32_t local)
{
  b.push_back(kLocalGet);
  PutU32(b, local);
  b.push_back(kI64Const);
  PutI64(b, 52);
  b.push_back(kI64ShrU);
  b.push_back(kI32WrapI64);
  b.push_back(kI32Const);
  PutI32(b, 0x7FF);
  b.push_back(kI32And);
  b.push_back(kLocalSet);
  PutU32(b, 5);
  // exp > 896 || (x & ~SIGN) == 0
  b.push_back(kLocalGet);
  PutU32(b, 5);
  b.push_back(kI32Const);
  PutI32(b, 896);
  b.push_back(kI32GtU);
  b.push_back(kLocalGet);
  PutU32(b, local);
  b.push_back(kI64Const);
  PutI64(b, 0x7FFFFFFFFFFFFFFFll);
  b.push_back(kI64And);
  b.push_back(kI64Eqz);
  b.push_back(kI32Or);
  b.push_back(kIf);
  b.push_back(kI32);
  {
    b.push_back(kLocalGet);
    PutU32(b, local);
    b.push_back(kI64Const);
    PutI64(b, 32);
    b.push_back(kI64ShrU);
    b.push_back(kI32WrapI64);
    b.push_back(kI32Const);
    PutI32(b, static_cast<int32_t>(0xC0000000u));
    b.push_back(kI32And);
    b.push_back(kLocalGet);
    PutU32(b, local);
    b.push_back(kI64Const);
    PutI64(b, 29);
    b.push_back(kI64ShrU);
    b.push_back(kI32WrapI64);
    b.push_back(kI32Const);
    PutI32(b, 0x3FFFFFFF);
    b.push_back(kI32And);
    b.push_back(kI32Or);
  }
  b.push_back(kElse);
  {
    b.push_back(kLocalGet);
    PutU32(b, local);
    b.push_back(kI64Const);
    PutI64(b, 32);
    b.push_back(kI64ShrU);
    b.push_back(kI32WrapI64);
    b.push_back(kI32Const);
    PutI32(b, static_cast<int32_t>(0x80000000u));
    b.push_back(kI32And);
  }
  b.push_back(kEnd);
}

// Emit: if (!HID2[LSQE]) bail. The non-indexed quantized forms are
// illegal while that bit (31) is clear; the indexed ones are not.
inline void EmitLsqeAvailable(Bytes& b, const StateLayout& l, uint32_t bail_pc,
                              uint32_t insts_before)
{
  b.push_back(kLocalGet);
  PutU32(b, 0);
  b.push_back(kI32Load);
  PutU32(b, 2);
  PutU32(b, l.spr_offset + 920 * 4);
  b.push_back(kI32Const);
  PutI32(b, static_cast<int32_t>(1u << 31));
  b.push_back(kI32And);
  b.push_back(kI32Eqz);
  b.push_back(kIf);
  b.push_back(kVoid);
  EmitBailBody(b, l, bail_pc, insts_before);
  b.push_back(kEnd);
}

// Whether a psq form with this GQR can be emitted inline: there must be
// values to specialise on, a hatch for the guard to fall back to, and a
// defined type.
inline bool PsqInline(const StateLayout& l, uint32_t gqr_index, bool is_store)
{
  if (!l.gqr_values || !l.interp_hatch || !l.ps_offset)
    return false;
  const uint32_t gqr = l.gqr_values[gqr_index];
  const uint32_t type = is_store ? (gqr & 7) : ((gqr >> 16) & 7);
  return type == 0 || type >= 4;
}

// Locals: 1 the address, 2 an i32 scratch, 3 the guest address for an
// update form, 5 scratch for the conversions, 9 the i64 scratch.
inline void EmitPsq(Bytes& b, const StateLayout& l, uint32_t this_pc, uint32_t insts_before,
                    uint32_t ra, uint32_t rb, int32_t disp, bool indexed, uint32_t freg,
                    uint32_t gqr_index, bool w, bool is_store, bool update, uint32_t ins)
{
  const uint32_t gqr = l.gqr_values[gqr_index];
  const uint32_t type = is_store ? (gqr & 7) : ((gqr >> 16) & 7);
  const uint32_t scale = is_store ? ((gqr >> 8) & 63) : ((gqr >> 24) & 63);
  const bool is_float = (type == 0);
  const int size = is_float ? 4 : ((type & 1) ? 2 : 1);
  const bool is_signed = (type & 2) != 0;
  // The scale as the signed exponent k, and the single 2^(+-k) as bits.
  const int32_t k = (scale < 32) ? static_cast<int32_t>(scale) : static_cast<int32_t>(scale) - 64;
  const int32_t e = is_store ? k : -k;
  const uint32_t factor_bits = static_cast<uint32_t>(127 + e) << 23;
  auto put_f32 = [&](uint32_t bits) {
    b.push_back(kF32Const);
    for (int i = 0; i < 4; ++i)
      b.push_back(static_cast<uint8_t>(bits >> (8 * i)));
  };
  const uint32_t ps0 = l.ps_offset + freg * 16;
  const int halves = w ? 1 : 2;

  b.push_back(kBlock);
  b.push_back(kVoid);
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
  if (update)
  {
    b.push_back(kLocalGet);
    PutU32(b, 1);
    b.push_back(kLocalSet);
    PutU32(b, 3);
  }
  // The guard: the GQR must still hold the value this code was built for.
  b.push_back(kLocalGet);
  PutU32(b, 0);
  b.push_back(kI32Load);
  PutU32(b, 2);
  PutU32(b, l.spr_offset + (912 + gqr_index) * 4);
  b.push_back(kI32Const);
  PutI32(b, static_cast<int32_t>(gqr));
  b.push_back(kI32Ne);
  b.push_back(kIf);
  b.push_back(kVoid);
  EmitHatchCall(b, l, ins, this_pc, insts_before);
  b.push_back(kBr);
  PutU32(b, 1);
  b.push_back(kEnd);
  EmitTranslate(b, l, 1, 2, this_pc, insts_before, ins, 0);

  if (!is_store && is_float)
  {
    for (int h = 0; h < halves; ++h)
    {
      b.push_back(kLocalGet);
      PutU32(b, 1);
      b.push_back(kI32Load);
      PutU32(b, 0);
      PutU32(b, static_cast<uint32_t>(4 * h));
      b.push_back(kLocalSet);
      PutU32(b, 2);
      EmitBswap32(b, 2);
      b.push_back(kLocalSet);
      PutU32(b, 2);
      EmitConvertToDouble(b, 2);
      b.push_back(kLocalSet);
      PutU32(b, 9);
      b.push_back(kLocalGet);
      PutU32(b, 0);
      b.push_back(kLocalGet);
      PutU32(b, 9);
      b.push_back(kI64Store);
      PutU32(b, 3);
      PutU32(b, ps0 + 8 * h);
    }
  }
  else if (!is_store)
  {
    for (int h = 0; h < halves; ++h)
    {
      b.push_back(kLocalGet);
      PutU32(b, 0);
      // The integer, from one or two bytes in guest order; the high byte
      // carries the sign for a signed type.
      b.push_back(kLocalGet);
      PutU32(b, 1);
      b.push_back(is_signed ? kI32Load8S : kI32Load8U);
      PutU32(b, 0);
      PutU32(b, static_cast<uint32_t>(size * h));
      if (size == 2)
      {
        b.push_back(kI32Const);
        PutI32(b, 8);
        b.push_back(kI32Shl);
        b.push_back(kLocalGet);
        PutU32(b, 1);
        b.push_back(kI32Load8U);
        PutU32(b, 0);
        PutU32(b, static_cast<uint32_t>(size * h + 1));
        b.push_back(kI32Or);
      }
      b.push_back(kF32ConvertI32S);
      if (e != 0)
      {
        put_f32(factor_bits);
        b.push_back(kF32Mul);
      }
      b.push_back(kF64PromoteF32);
      b.push_back(kF64Store);
      PutU32(b, 3);
      PutU32(b, ps0 + 8 * h);
    }
  }
  else if (is_float)
  {
    for (int h = 0; h < halves; ++h)
    {
      b.push_back(kLocalGet);
      PutU32(b, 0);
      b.push_back(kI64Load);
      PutU32(b, 3);
      PutU32(b, ps0 + 8 * h);
      b.push_back(kLocalSet);
      PutU32(b, 9);
      EmitConvertToSingleFTZ(b, 9);
      b.push_back(kLocalSet);
      PutU32(b, 2);
      b.push_back(kLocalGet);
      PutU32(b, 1);
      EmitBswap32(b, 2);
      b.push_back(kI32Store);
      PutU32(b, 0);
      PutU32(b, static_cast<uint32_t>(4 * h));
    }
  }
  else
  {
    // ScaleAndClamp: float(ps) * 2^k, clamped to the type, truncated.
    const float lo = is_signed ? (size == 2 ? -32768.0f : -128.0f) : 0.0f;
    const float hi = is_signed ? (size == 2 ? 32767.0f : 127.0f) : (size == 2 ? 65535.0f : 255.0f);
    auto bits_of = [](float f) {
      uint32_t u;
      std::memcpy(&u, &f, sizeof(u));
      return u;
    };
    for (int h = 0; h < halves; ++h)
    {
      b.push_back(kLocalGet);
      PutU32(b, 0);
      b.push_back(kF64Load);
      PutU32(b, 3);
      PutU32(b, ps0 + 8 * h);
      b.push_back(kF32DemoteF64);
      if (e != 0)
      {
        put_f32(factor_bits);
        b.push_back(kF32Mul);
      }
      put_f32(bits_of(hi));
      b.push_back(kF32Min);
      put_f32(bits_of(lo));
      b.push_back(kF32Max);
      b.push_back(kPrefixFC);
      PutU32(b, kI32TruncSatF32S);
      b.push_back(kLocalSet);
      PutU32(b, 2);
      if (size == 2)
      {
        b.push_back(kLocalGet);
        PutU32(b, 1);
        b.push_back(kLocalGet);
        PutU32(b, 2);
        b.push_back(kI32Const);
        PutI32(b, 8);
        b.push_back(kI32ShrU);
        b.push_back(kI32Store8);
        PutU32(b, 0);
        PutU32(b, static_cast<uint32_t>(2 * h));
      }
      b.push_back(kLocalGet);
      PutU32(b, 1);
      b.push_back(kLocalGet);
      PutU32(b, 2);
      b.push_back(kI32Store8);
      PutU32(b, 0);
      PutU32(b, static_cast<uint32_t>(size * h + size - 1));
    }
  }
  if (!is_store && w)
  {
    // One half loaded: ps1 is 1.0.
    b.push_back(kLocalGet);
    PutU32(b, 0);
    b.push_back(kI64Const);
    PutI64(b, 0x3FF0000000000000ll);
    b.push_back(kI64Store);
    PutU32(b, 3);
    PutU32(b, ps0 + 8);
  }
  if (update)
  {
    BeginStoreGpr(b);
    b.push_back(kLocalGet);
    PutU32(b, 3);
    EndStoreGpr(b, l, ra);
  }
  b.push_back(kEnd);
}

// ── the condition register as a word ──
//
// Dolphin keeps each CR field as a 64-bit value whose SO is bit 59, LT bit
// 62, EQ "the low word is zero" and GT "the value is positive as a signed
// 64-bit integer" (ConditionRegister.h). mfcr decodes all eight fields
// into PowerPC's 32-bit CR; mtcrf encodes the selected fields back
// through s_crTable, exactly as ConditionRegister::Get and Set do.
//
// Measured past the boot, in the tutorial level: mfcr and mtcrf were 5%
// of what still went through the hatch, mostly in the OS's interrupt
// enable/disable pairs around every critical section.
// Push ConditionRegister::GetField(i): the field's four PowerPC bits.
// Uses i64 local 9.
inline void EmitCrFieldDecode(Bytes& b, const StateLayout& l, uint32_t i)
{
  b.push_back(kLocalGet);
  PutU32(b, 0);
  b.push_back(kI64Load);
  PutU32(b, 3);
  PutU32(b, l.cr_offset + 8 * i);
  b.push_back(kLocalTee);
  PutU32(b, 9);
  // (v >> 59) & 9: SO in bit 0 and LT in bit 3 of the field
  b.push_back(kI64Const);
  PutI64(b, 59);
  b.push_back(kI64ShrU);
  b.push_back(kI32WrapI64);
  b.push_back(kI32Const);
  PutI32(b, 9);
  b.push_back(kI32And);
  // EQ: the low word is zero
  b.push_back(kLocalGet);
  PutU32(b, 9);
  b.push_back(kI32WrapI64);
  b.push_back(kI32Eqz);
  b.push_back(kI32Const);
  PutI32(b, 1);
  b.push_back(kI32Shl);
  b.push_back(kI32Or);
  // GT: positive as a signed 64-bit value
  b.push_back(kLocalGet);
  PutU32(b, 9);
  b.push_back(kI64Const);
  PutI64(b, 0);
  b.push_back(kI64GtS);
  b.push_back(kI32Const);
  PutI32(b, 2);
  b.push_back(kI32Shl);
  b.push_back(kI32Or);
}

inline void EmitMfcr(Bytes& b, const StateLayout& l, uint32_t rd)
{
  BeginStoreGpr(b);
  for (uint32_t i = 0; i < 8; ++i)
  {
    EmitCrFieldDecode(b, l, i);
    // into its place in the word
    const int shift = 28 - 4 * static_cast<int>(i);
    if (shift)
    {
      b.push_back(kI32Const);
      PutI32(b, shift);
      b.push_back(kI32Shl);
    }
    if (i)
      b.push_back(kI32Or);
  }
  EndStoreGpr(b, l, rd);
}

// mtcrf. ConditionRegister::Set re-encodes ALL eight fields through
// s_crTable, so a field the mask leaves alone still comes out normalised
// -- a compare result stored as a sign-extended difference becomes the
// table's entry for the same four bits. Observable only as the raw field
// value, which is exactly what the differential compares, so it is
// matched: an unselected field is decoded and re-encoded.
inline void EmitMtcrf(Bytes& b, const StateLayout& l, uint32_t rs, uint32_t crm)
{
  LoadGpr(b, l, rs);
  b.push_back(kLocalSet);
  PutU32(b, 2);
  for (uint32_t i = 0; i < 8; ++i)
  {
    b.push_back(kLocalGet);
    PutU32(b, 0);
    if (crm & (0x80u >> i))  // CRM bit 0 (the MSB) selects CR0
    {
      b.push_back(kLocalGet);
      PutU32(b, 2);
      const int shift = 28 - 4 * static_cast<int>(i);
      if (shift)
      {
        b.push_back(kI32Const);
        PutI32(b, shift);
        b.push_back(kI32ShrU);
      }
      b.push_back(kI32Const);
      PutI32(b, 0xF);
      b.push_back(kI32And);
    }
    else
    {
      EmitCrFieldDecode(b, l, i);
    }
    b.push_back(kI32Const);
    PutI32(b, 3);
    b.push_back(kI32Shl);
    b.push_back(kI32Const);
    PutI32(b, static_cast<int32_t>(l.cr_table_base));
    b.push_back(kI32Add);
    b.push_back(kI64Load);
    PutU32(b, 3);
    PutU32(b, 0);
    b.push_back(kI64Store);
    PutU32(b, 3);
    PutU32(b, l.cr_offset + 8 * i);
  }
}

// ── fcmpu and fcmpo ──
//
// Helper_FloatCompareUnordered/Ordered: FL (8), FG (4) or FE (2) into the
// FPCC bits of FPSCR and into the named CR field. A NaN operand gives FU
// and, for the ordered compare or a signalling NaN, raises exceptions;
// both engines handle that in the interpreter, through the hatch, so the
// inline path is the ordered three-way result and nothing else.
// Locals: 2 scratch, 9 and 10 the operands.
inline void EmitFcmp(Bytes& b, const StateLayout& l, uint32_t ins, uint32_t pc,
                     uint32_t insts_before)
{
  const uint32_t crf = (RS(ins) >> 2) & 7;
  const uint32_t pa = l.ps_offset + RA(ins) * 16;
  const uint32_t pb = l.ps_offset + RB(ins) * 16;
  b.push_back(kBlock);
  b.push_back(kVoid);
  b.push_back(kLocalGet);
  PutU32(b, 0);
  b.push_back(kI64Load);
  PutU32(b, 3);
  PutU32(b, pa);
  b.push_back(kLocalSet);
  PutU32(b, 9);
  b.push_back(kLocalGet);
  PutU32(b, 0);
  b.push_back(kI64Load);
  PutU32(b, 3);
  PutU32(b, pb);
  b.push_back(kLocalSet);
  PutU32(b, 10);
  // a NaN in either: (x & ~sign) > +inf
  for (uint32_t loc : {9u, 10u})
  {
    b.push_back(kLocalGet);
    PutU32(b, loc);
    b.push_back(kI64Const);
    PutI64(b, 0x7FFFFFFFFFFFFFFFll);
    b.push_back(kI64And);
    b.push_back(kI64Const);
    PutI64(b, 0x7FF0000000000000ll);
    b.push_back(kI64GtU);
  }
  b.push_back(kI32Or);
  b.push_back(kIf);
  b.push_back(kVoid);
  EmitHatchCall(b, l, ins, pc, insts_before);
  b.push_back(kBr);
  PutU32(b, 1);
  b.push_back(kEnd);
  // result = a < b ? 8 : (a > b ? 4 : 2)
  b.push_back(kI32Const);
  PutI32(b, 8);
  b.push_back(kI32Const);
  PutI32(b, 4);
  b.push_back(kI32Const);
  PutI32(b, 2);
  b.push_back(kLocalGet);
  PutU32(b, 9);
  b.push_back(kF64ReinterpretI64);
  b.push_back(kLocalGet);
  PutU32(b, 10);
  b.push_back(kF64ReinterpretI64);
  b.push_back(kF64Gt);
  b.push_back(kSelect);
  b.push_back(kLocalGet);
  PutU32(b, 9);
  b.push_back(kF64ReinterpretI64);
  b.push_back(kLocalGet);
  PutU32(b, 10);
  b.push_back(kF64ReinterpretI64);
  b.push_back(kF64Lt);
  b.push_back(kSelect);
  b.push_back(kLocalSet);
  PutU32(b, 2);
  // FPSCR[FPCC] |= result. The interpreter means to clear FPCC first
  // (`fpscr.FPRF & ~FPCC_MASK`), but FPRF is a five-bit field and the
  // mask is at bit 12, so nothing is cleared and the result is ORed in.
  // That is the behaviour to match.
  b.push_back(kLocalGet);
  PutU32(b, 0);
  b.push_back(kLocalGet);
  PutU32(b, 0);
  b.push_back(kI32Load);
  PutU32(b, 2);
  PutU32(b, l.fpscr_offset);
  b.push_back(kLocalGet);
  PutU32(b, 2);
  b.push_back(kI32Const);
  PutI32(b, 12);
  b.push_back(kI32Shl);
  b.push_back(kI32Or);
  b.push_back(kI32Store);
  PutU32(b, 2);
  PutU32(b, l.fpscr_offset);
  // cr[crf] = s_crTable[result]
  b.push_back(kLocalGet);
  PutU32(b, 0);
  b.push_back(kLocalGet);
  PutU32(b, 2);
  b.push_back(kI32Const);
  PutI32(b, 3);
  b.push_back(kI32Shl);
  b.push_back(kI32Const);
  PutI32(b, static_cast<int32_t>(l.cr_table_base));
  b.push_back(kI32Add);
  b.push_back(kI64Load);
  PutU32(b, 3);
  PutU32(b, 0);
  b.push_back(kI64Store);
  PutU32(b, 3);
  PutU32(b, l.cr_offset + 8 * crf);
  b.push_back(kEnd);
}

// ForceSingle on the double in i64 local `src`, into the float bits in i32
// local `dst`, for a result that is about to be written. Under NI a value
// below the smallest normal single is flushed to a signed zero before
// rounding (the quirk); otherwise it rounds to float, which may give a
// denormal, kept as it is. An infinite (or NaN) float result is the
// interpreter's: the hatch runs the whole instruction and the enclosing
// block is left, so this must be emitted one `if` deep in that block.
inline void EmitForceSingleOrHatch(Bytes& b, const StateLayout& l, uint32_t ins, uint32_t pc,
                                   uint32_t n, uint32_t src, uint32_t dst)
{
  auto op = [&](uint8_t o) { b.push_back(o); };
  auto u = [&](uint32_t v) { PutU32(b, v); };
  auto i32c = [&](int32_t v) { op(kI32Const); PutI32(b, v); };
  auto i64c = [&](int64_t v) { op(kI64Const); PutI64(b, v); };
  auto lget = [&](uint32_t i) { op(kLocalGet); u(i); };
  auto lset = [&](uint32_t i) { op(kLocalSet); u(i); };
  lget(0); op(kI32Load); u(2); u(l.fpscr_offset); i32c(4); op(kI32And); i32c(0); op(kI32Ne);
  lget(src); i64c(0x7FFFFFFFFFFFFFFFll); op(kI64And); i64c(0x3810000000000000ll); op(kI64LtU);
  op(kI32And);
  op(kIf); op(kVoid);
  lget(src); i64c(static_cast<int64_t>(0x8000000000000000ull)); op(kI64And); lset(src);
  op(kEnd);
  lget(src); op(kF64ReinterpretI64); op(kF32DemoteF64); op(kI32ReinterpretF32); lset(dst);
  lget(dst); i32c(0x7F800000); op(kI32And); i32c(0x7F800000); op(kI32Eq);
  op(kIf); op(kVoid);
  EmitHatchCall(b, l, ins, pc, n);
  op(kBr); u(1);
  op(kEnd);
}

// The writes of a single-precision result: ps0 (and ps1) from the float in
// i32 local 2 (and 3 when `paired`; otherwise 2 fills both halves),
// widened -- exact for a normal, zero or denormal single, and the
// interpreter's Fill(float) is this same cast. Then FPRF's code for the
// float in local 2 into local 6, using 5: normal 0x4/0x8, zero 0x2/0x12,
// denormal 0x14/0x18 (positive/negative). `select` takes its first
// operand when the condition holds, so the negative code goes first.
inline void EmitSingleWrites(Bytes& b, uint32_t pd, bool paired)
{
  auto op = [&](uint8_t o) { b.push_back(o); };
  auto u = [&](uint32_t v) { PutU32(b, v); };
  auto i32c = [&](int32_t v) { op(kI32Const); PutI32(b, v); };
  auto lget = [&](uint32_t i) { op(kLocalGet); u(i); };
  auto lset = [&](uint32_t i) { op(kLocalSet); u(i); };
  lget(0); lget(2); op(kF32ReinterpretI32); op(kF64PromoteF32); op(kI64ReinterpretF64);
  op(kI64Store); u(3); u(pd);
  lget(0); lget(paired ? 3 : 2); op(kF32ReinterpretI32); op(kF64PromoteF32);
  op(kI64ReinterpretF64); op(kI64Store); u(3); u(pd + 8);
  lget(2); i32c(31); op(kI32ShrU); lset(5);  // sign
  // zero code | denormal code, by the mantissa
  i32c(0x12); i32c(0x2); lget(5); op(kSelect);
  i32c(0x18); i32c(0x14); lget(5); op(kSelect);
  lget(2); i32c(0x007FFFFF); op(kI32And); op(kI32Eqz); op(kSelect);
  // ... or the normal code, by the exponent
  i32c(0x8); i32c(0x4); lget(5); op(kSelect);
  lget(2); i32c(0x7F800000); op(kI32And); op(kI32Eqz); op(kSelect);
  lset(6);
}

// ── floating-point arithmetic, the common case inline ──
//
// Interpreter_FloatingPoint.cpp is the specification. For add, subtract,
// multiply and divide the interpreter computes in double precision, then:
// propagates NaNs in a defined order and raises VXSNAN/VXISI/VXIMZ/VXZDZ/ZX
// when an operand or the result is a NaN, an infinity or (for division) a
// zero divisor; rounds the multiplicand of a single-precision multiply to
// 25 bits (Force25Bit), which takes a different path for a subnormal;
// converts a single-precision result to float with the NI flush quirk
// (ForceSingle) or flushes a subnormal double (ForceDouble); and writes
// FPRF with the result's class, clearing FI and FR for a multiply.
//
// The common case -- finite operands, a finite result -- is emitted inline
// below: ~60 wasm instructions and no call. Everything else is detected
// BEFORE anything is written and goes to the interpreter through the
// hatch, which then executes the whole instruction from the registers as
// they were. Measured past the boot, fadds/fsubs/fmuls were 60% of what
// went through the hatch and about half of the wall clock.
//
// kind: 0 add, 1 sub, 2 mul, 3 div. single: the result is rounded to float
// and fills both halves. paired: both halves are computed (op 4);
// c_half >= 0 picks one half of the multiplicand for both (ps_muls0/1).
//
// Locals: i64 9 and 10 hold the operands and the double result; i32 2 and
// 3 the single results of the two halves; i32 5 the slow-path flag.
inline void EmitFpBinary(Bytes& b, const StateLayout& l, uint32_t ins, uint32_t pc, uint32_t n,
                         int kind, bool single, bool paired, int c_half)
{
  const uint32_t fd = (ins >> 21) & 0x1F, fa = (ins >> 16) & 0x1F, fb = (ins >> 11) & 0x1F,
                 fc = (ins >> 6) & 0x1F;
  const uint32_t src2 = (kind == 2) ? fc : fb;
  const uint32_t pd = l.ps_offset + fd * 16, pa = l.ps_offset + fa * 16,
                 p2 = l.ps_offset + src2 * 16;
  const int halves = paired ? 2 : 1;
  auto op = [&](uint8_t o) { b.push_back(o); };
  auto u = [&](uint32_t v) { PutU32(b, v); };
  auto i32c = [&](int32_t v) { op(kI32Const); PutI32(b, v); };
  auto i64c = [&](int64_t v) { op(kI64Const); PutI64(b, v); };
  auto lget = [&](uint32_t i) { op(kLocalGet); u(i); };
  auto lset = [&](uint32_t i) { op(kLocalSet); u(i); };
  auto ltee = [&](uint32_t i) { op(kLocalTee); u(i); };
  auto load64 = [&](uint32_t off) { lget(0); op(kI64Load); u(3); u(off); };
  // Operand x (i64 on the stack) is an infinity or a NaN: exponent all ones.
  auto is_special = [&]() {
    i64c(0x7FF0000000000000ll); op(kI64And); i64c(0x7FF0000000000000ll); op(kI64Eq);
  };
  auto fpscr_load = [&]() { lget(0); op(kI32Load); u(2); u(l.fpscr_offset); };

  op(kBlock);
  op(kVoid);

  // ── the tests, before anything is written ──
  i32c(0);
  lset(5);
  for (int h = 0; h < halves; ++h)
  {
    const uint32_t ha = pa + h * 8;
    const uint32_t h2 = p2 + ((kind == 2 && c_half >= 0) ? c_half : h) * 8;
    load64(ha); is_special(); lget(5); op(kI32Or); lset(5);
    load64(h2); is_special(); lget(5); op(kI32Or); lset(5);
    if (kind == 2 && single)
    {
      // A subnormal multiplicand rounds differently (Force25Bit).
      load64(h2); i64c(0x7FF0000000000000ll); op(kI64And); op(kI64Eqz);
      load64(h2); i64c(0x000FFFFFFFFFFFFFll); op(kI64And); op(kI64Eqz); op(kI32Eqz);
      op(kI32And); lget(5); op(kI32Or); lset(5);
    }
    if (kind == 3)
    {
      // Division by zero raises ZX and yields an infinity.
      load64(h2); i64c(0x7FFFFFFFFFFFFFFFll); op(kI64And); op(kI64Eqz);
      lget(5); op(kI32Or); lset(5);
    }
  }
  lget(5);
  op(kIf); op(kVoid);
  EmitHatchCall(b, l, ins, pc, n);
  op(kBr); u(1);
  op(kEnd);

  // ── the arithmetic, one half at a time ──
  for (int h = 0; h < halves; ++h)
  {
    const uint32_t ha = pa + h * 8;
    const uint32_t h2 = p2 + ((kind == 2 && c_half >= 0) ? c_half : h) * 8;
    load64(ha); lset(9);
    load64(h2); lset(10);
    if (kind == 2 && single)
    {
      // Force25Bit for a normal (or zero) multiplicand: keep 25 mantissa
      // bits, rounding up on the 26th.
      lget(10); i64c(static_cast<int64_t>(0xFFFFFFFFF8000000ull)); op(kI64And);
      lget(10); i64c(0x0000000008000000ll); op(kI64And); op(kI64Add); lset(10);
    }
    lget(9); op(kF64ReinterpretI64);
    lget(10); op(kF64ReinterpretI64);
    op(kind == 0 ? kF64Add : kind == 1 ? kF64Sub : kind == 2 ? kF64Mul : kF64Div);
    op(kI64ReinterpretF64);
    lset(9);

    if (single)
    {
      EmitForceSingleOrHatch(b, l, ins, pc, n, 9, 2 + h);
    }
    else
    {
      // ForceDouble: under NI a subnormal result is flushed to a signed
      // zero. An infinite result is the interpreter's.
      fpscr_load(); i32c(4); op(kI32And); i32c(0); op(kI32Ne);
      lget(9); i64c(0x7FF0000000000000ll); op(kI64And); op(kI64Eqz);
      op(kI32And);
      op(kIf); op(kVoid);
      lget(9); i64c(static_cast<int64_t>(0x8000000000000000ull)); op(kI64And); lset(9);
      op(kEnd);
      lget(9); is_special();
      op(kIf); op(kVoid);
      EmitHatchCall(b, l, ins, pc, n);
      op(kBr); u(1);
      op(kEnd);
    }
  }

  // ── the writes ──
  if (single)
  {
    EmitSingleWrites(b, pd, paired);
  }
  else
  {
    lget(0); lget(9); op(kI64Store); u(3); u(pd);
    lget(9); i64c(63); op(kI64ShrU); op(kI32WrapI64); lset(5);  // sign
    i32c(0x12); i32c(0x2); lget(5); op(kSelect);
    i32c(0x18); i32c(0x14); lget(5); op(kSelect);
    lget(9); i64c(0x000FFFFFFFFFFFFFll); op(kI64And); op(kI64Eqz); op(kSelect);
    i32c(0x8); i32c(0x4); lget(5); op(kSelect);
    lget(9); i64c(0x7FF0000000000000ll); op(kI64And); op(kI64Eqz); op(kSelect);
    lset(6);
  }
  // fpscr = (fpscr & ~FPRF [& ~(FI|FR) for a multiply]) | code << 12
  lget(0);
  fpscr_load();
  i32c(static_cast<int32_t>(~((0x1Fu << 12) | ((kind == 2 && !paired) ? (0x3u << 17) : 0u))));
  op(kI32And);
  lget(6); i32c(12); op(kI32Shl); op(kI32Or);
  op(kI32Store); u(2); u(l.fpscr_offset);
  op(kEnd);
}

// ── the paired fused multiply-adds, the common case inline ──
//
// ps_madds0, ps_madds1, ps_madd, ps_msub, ps_nmadd, ps_nmsub. The
// specification is Interpreter_Paired.cpp (each half through NI_madd or
// NI_msub<single>, ForceSingle, negated unless NaN for the nm forms,
// SetBoth, UpdateFPRFSingle(ps0)) and NI_madd_msub in
// Interpreter_FPUtils.h, which computes fma(a, Force25Bit(c), +-b) and then
// corrects a result that lands exactly on a float rounding tie, using an
// error-free 2Sum whose one fma is fma(a, c_round, a').
//
// wasm has no fma; the interpreter's is musl's software one, and this
// family was the whole of the interpreter's share of the CPU thread.
// The common case needs no fma: when a has at most 24 significant bits
// (its low 29 mantissa bits zero -- every value a paired-single
// computation produces) and c_round has 25, the product has at most 49
// and is EXACT in double, provided it stays clear of overflow and of the
// subnormal range. Then f64.mul followed by f64.add rounds once, exactly
// as the fma does, signed zeros included, and so does the 2Sum's
// fma(a, c_round, a') as p + a'. The guard, per half and before anything
// is written: a, b and c finite; a float-exact; a and c zero or with an
// exponent within 2^+-200 (products within 2^+-400, comfortably normal).
// Anything else goes to the interpreter through the hatch.
//
// sub: msub/nmsub. negate: nmadd/nmsub. c_half >= 0: ps_madds0/1, one
// half of c for both.
//
// Locals: i64 9 a, 10 c_round, 11 +-b, 12 the product (then the error),
// 13 the result, 14 a'; i32 2 and 3 the single results, 5 the slow-path
// flag, 6 FPRF's code.
inline void EmitPsMadd(Bytes& b, const StateLayout& l, uint32_t ins, uint32_t pc, uint32_t n,
                       bool sub, bool negate, int c_half)
{
  const uint32_t fd = (ins >> 21) & 0x1F, fa = (ins >> 16) & 0x1F, fb = (ins >> 11) & 0x1F,
                 fc = (ins >> 6) & 0x1F;
  const uint32_t pd = l.ps_offset + fd * 16, pa = l.ps_offset + fa * 16,
                 pb = l.ps_offset + fb * 16, pc_ = l.ps_offset + fc * 16;
  auto op = [&](uint8_t o) { b.push_back(o); };
  auto u = [&](uint32_t v) { PutU32(b, v); };
  auto i32c = [&](int32_t v) { op(kI32Const); PutI32(b, v); };
  auto i64c = [&](int64_t v) { op(kI64Const); PutI64(b, v); };
  auto lget = [&](uint32_t i) { op(kLocalGet); u(i); };
  auto lset = [&](uint32_t i) { op(kLocalSet); u(i); };
  auto load64 = [&](uint32_t off) { lget(0); op(kI64Load); u(3); u(off); };
  auto fget = [&](uint32_t i) { lget(i); op(kF64ReinterpretI64); };
  auto fset = [&](uint32_t i) { op(kI64ReinterpretF64); lset(i); };
  // Fold "this operand is slow" (i32 on the stack) into the flag.
  auto slow = [&]() { lget(5); op(kI32Or); lset(5); };
  // x (i64 on the stack, consumed) is an infinity or a NaN.
  auto is_special = [&]() {
    i64c(0x7FF0000000000000ll); op(kI64And); i64c(0x7FF0000000000000ll); op(kI64Eq);
  };
  // The operand at `off` is nonzero with an exponent outside [-200, +200].
  // Biased, that is outside [823, 1223]: (e - 823) >= 401 unsigned.
  auto out_of_range = [&](uint32_t off) {
    load64(off); i64c(0x7FFFFFFFFFFFFFFFll); op(kI64And); op(kI64Eqz); op(kI32Eqz);
    load64(off); i64c(52); op(kI64ShrU); i64c(0x7FF); op(kI64And); i64c(823); op(kI64Sub);
    i64c(400); op(kI64GtU);
    op(kI32And);
  };
  auto c_off = [&](int h) { return pc_ + (c_half >= 0 ? c_half : h) * 8; };

  op(kBlock);
  op(kVoid);

  // ── the tests, before anything is written ──
  i32c(0);
  lset(5);
  for (int h = 0; h < 2; ++h)
  {
    load64(pa + h * 8); is_special(); slow();
    load64(pb + h * 8); is_special(); slow();
    load64(c_off(h)); is_special(); slow();
    load64(pa + h * 8); i64c(0x1FFFFFFF); op(kI64And); op(kI64Eqz); op(kI32Eqz); slow();
    out_of_range(pa + h * 8); slow();
    out_of_range(c_off(h)); slow();
  }
  lget(5);
  op(kIf); op(kVoid);
  EmitHatchCall(b, l, ins, pc, n);
  op(kBr); u(1);
  op(kEnd);

  // ── the arithmetic, one half at a time ──
  for (int h = 0; h < 2; ++h)
  {
    load64(pa + h * 8); lset(9);
    // Force25Bit: keep 25 mantissa bits, rounding up on the 26th. c is
    // normal or zero here, which is the case EmitFpBinary handles too.
    load64(c_off(h)); i64c(static_cast<int64_t>(0xFFFFFFFFF8000000ull)); op(kI64And);
    load64(c_off(h)); i64c(0x0000000008000000ll); op(kI64And); op(kI64Add); lset(10);
    load64(pb + h * 8);
    if (sub)
    {
      i64c(static_cast<int64_t>(0x8000000000000000ull)); op(kI64Xor);
    }
    lset(11);
    // p = a * c_round, exact; r = p + (+-b), rounded once.
    fget(9); fget(10); op(kF64Mul); fset(12);
    fget(12); fget(11); op(kF64Add); fset(13);

    // An even tie for the float rounding: the low 29 bits are 1 followed
    // by zeros. Then a' = b - r; error = (p + a') + (b - (r + a')), and a
    // nonzero error moves r one ulp away from the tie, up when it agrees
    // in sign with r.
    lget(13); i64c(0x1FFFFFFF); op(kI64And); i64c(0x10000000); op(kI64Eq);
    op(kIf); op(kVoid);
    fget(11); fget(13); op(kF64Sub); fset(14);
    fget(12); fget(14); op(kF64Add);
    fget(11); fget(13); fget(14); op(kF64Add); op(kF64Sub);
    op(kF64Add); fset(12);
    fget(12); op(kF64Const); PutF64(b, 0.0); op(kF64Eq); op(kI32Eqz);
    op(kIf); op(kVoid);
    lget(13); i64c(1); op(kI64Add);
    lget(13); i64c(1); op(kI64Sub);
    fget(12); op(kF64Const); PutF64(b, 0.0); op(kF64Gt);
    fget(13); op(kF64Const); PutF64(b, 0.0); op(kF64Gt);
    op(kI32Eq);
    op(kSelect);
    lset(13);
    op(kEnd);
    op(kEnd);

    EmitForceSingleOrHatch(b, l, ins, pc, n, 13, 2 + h);
    if (negate)
    {
      // Never a NaN here: an infinite or NaN float went to the hatch.
      lget(2 + h); i32c(static_cast<int32_t>(0x80000000u)); op(kI32Xor); lset(2 + h);
    }
  }

  // ── the writes ──
  EmitSingleWrites(b, pd, true);
  // fpscr = (fpscr & ~FPRF) | code << 12; FI and FR are not touched.
  lget(0);
  lget(0); op(kI32Load); u(2); u(l.fpscr_offset);
  i32c(static_cast<int32_t>(~(0x1Fu << 12)));
  op(kI32And);
  lget(6); i32c(12); op(kI32Shl); op(kI32Or);
  op(kI32Store); u(2); u(l.fpscr_offset);
  op(kEnd);
}

// frsp, the common case inline: a value that is already exactly
// representable as a single rounds to itself, which is what a pipeline
// working in singles sees every time. Then FI and FR are cleared, FPRF
// written and both halves filled. Anything inexact, infinite, NaN, or in
// the NI flush range goes to the interpreter, which raises XX for it.
inline void EmitFrsp(Bytes& b, const StateLayout& l, uint32_t ins, uint32_t pc, uint32_t n)
{
  const uint32_t fd = (ins >> 21) & 0x1F, fb = (ins >> 11) & 0x1F;
  const uint32_t pd = l.ps_offset + fd * 16, pb = l.ps_offset + fb * 16;
  auto op = [&](uint8_t o) { b.push_back(o); };
  auto u = [&](uint32_t v) { PutU32(b, v); };
  auto i32c = [&](int32_t v) { op(kI32Const); PutI32(b, v); };
  auto i64c = [&](int64_t v) { op(kI64Const); PutI64(b, v); };
  auto lget = [&](uint32_t i) { op(kLocalGet); u(i); };
  auto lset = [&](uint32_t i) { op(kLocalSet); u(i); };
  op(kBlock);
  op(kVoid);
  lget(0); op(kI64Load); u(3); u(pb); lset(9);
  // slow: NaN/inf, or (NI and 0 < |b| < 2^-126), or inexact
  lget(9); i64c(0x7FF0000000000000ll); op(kI64And); i64c(0x7FF0000000000000ll); op(kI64Eq);
  lget(0); op(kI32Load); u(2); u(l.fpscr_offset); i32c(4); op(kI32And); i32c(0); op(kI32Ne);
  lget(9); i64c(0x7FFFFFFFFFFFFFFFll); op(kI64And); i64c(0x3810000000000000ll); op(kI64LtU);
  lget(9); i64c(0x7FFFFFFFFFFFFFFFll); op(kI64And); op(kI64Eqz); op(kI32Eqz);
  op(kI32And); op(kI32And); op(kI32Or);
  lget(9); op(kF64ReinterpretI64); op(kF32DemoteF64); op(kI32ReinterpretF32); lset(2);
  lget(2); op(kF32ReinterpretI32); op(kF64PromoteF32); op(kI64ReinterpretF64);
  lget(9); op(kI64Ne); op(kI32Or);
  op(kIf); op(kVoid);
  EmitHatchCall(b, l, ins, pc, n);
  op(kBr); u(1);
  op(kEnd);
  lget(0); lget(9); op(kI64Store); u(3); u(pd);
  lget(0); lget(9); op(kI64Store); u(3); u(pd + 8);
  lget(2); i32c(31); op(kI32ShrU); lset(5);
  i32c(0x12); i32c(0x2); lget(5); op(kSelect);
  i32c(0x18); i32c(0x14); lget(5); op(kSelect);
  lget(2); i32c(0x007FFFFF); op(kI32And); op(kI32Eqz); op(kSelect);
  i32c(0x8); i32c(0x4); lget(5); op(kSelect);
  lget(2); i32c(0x7F800000); op(kI32And); op(kI32Eqz); op(kSelect);
  lset(6);
  lget(0);
  lget(0); op(kI32Load); u(2); u(l.fpscr_offset);
  i32c(static_cast<int32_t>(~((0x1Fu << 12) | (0x3u << 17))));
  op(kI32And);
  lget(6); i32c(12); op(kI32Shl); op(kI32Or);
  op(kI32Store); u(2); u(l.fpscr_offset);
  op(kEnd);
}

// lmw / stmw: registers rD..r31 to or from consecutive words. One
// alignment check and one translation for the whole run; the interpreter
// also refuses a misaligned address, and a run that crossed out of its
// region would be a guest bug this does not try to emulate.
inline void EmitLoadStoreMultiple(Bytes& b, const StateLayout& l, uint32_t this_pc,
                                  uint32_t insts_before, uint32_t ra, int32_t disp, uint32_t rd,
                                  bool is_store)
{
  LoadGprOrZero(b, l, ra);
  if (disp)
  {
    b.push_back(kI32Const);
    PutI32(b, disp);
    b.push_back(kI32Add);
  }
  b.push_back(kLocalSet);
  PutU32(b, 1);
  b.push_back(kLocalGet);
  PutU32(b, 1);
  b.push_back(kI32Const);
  PutI32(b, 3);
  b.push_back(kI32And);
  b.push_back(kIf);
  b.push_back(kVoid);
  EmitBailBody(b, l, this_pc, insts_before);
  b.push_back(kEnd);
  EmitTranslate(b, l, 1, 2, this_pc, insts_before);
  for (uint32_t r = rd; r < 32; ++r)
  {
    const uint32_t off = (r - rd) * 4;
    if (is_store)
    {
      b.push_back(kLocalGet);
      PutU32(b, 1);
      LoadGpr(b, l, r);
      b.push_back(kLocalSet);
      PutU32(b, 2);
      EmitBswap32(b, 2);
      b.push_back(kI32Store);
      PutU32(b, 0);
      PutU32(b, off);
    }
    else
    {
      BeginStoreGpr(b);
      b.push_back(kLocalGet);
      PutU32(b, 1);
      b.push_back(kI32Load);
      PutU32(b, 0);
      PutU32(b, off);
      b.push_back(kLocalSet);
      PutU32(b, 2);
      EmitBswap32(b, 2);
      EndStoreGpr(b, l, r);
    }
  }
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
                      int size, bool is_store, bool sign_extend, bool update, uint32_t ins = 0)
{
  // Everything after the address computation sits in a block, so that a
  // translation miss handled by the interpreter can skip it.
  b.push_back(kBlock);
  b.push_back(kVoid);
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

  if (update)
  {
    // The GUEST address is written back, not the host one. Parking it in
    // a local changes no guest state, so it is safe to do before the
    // translation decides whether this access bails.
    b.push_back(kLocalGet);
    PutU32(b, 1);
    b.push_back(kLocalSet);
    PutU32(b, 3);
  }

  // Words go through ONE load or store and a byte swap built from two
  // rotates. A 32-bit byte swap is rotr(x & 0x00FF00FF, 8) | rotl(x &
  // 0xFF00FF00, 8): the even bytes rotate one way, the odd bytes the
  // other, and each lands where the guest's byte order puts it. Twelve
  // wasm instructions and one memory access, against fifteen and four
  // when it was assembled from bytes -- and lwz alone is 17% of the
  // instruction stream. The alignment hint is 1, which is a hint: an
  // unaligned address is simply slower, never wrong.
  auto emit_bswap32_of_local2 = [&]() { EmitBswap32(b, 2); };
  // The store, at the host address in `addr_local`: the value through
  // local 2. Written once and used twice -- for RAM, and for the gather
  // pipe, where the address is the pipe's buffer instead.
  auto emit_store_at = [&](Bytes& bb, uint32_t addr_local) {
    LoadGpr(bb, l, reg);
    bb.push_back(kLocalSet);
    PutU32(bb, 2);
    if (size == 4)
    {
      bb.push_back(kLocalGet);
      PutU32(bb, addr_local);
      EmitBswap32(bb, 2);
      bb.push_back(kI32Store);
      PutU32(bb, 0);  // alignment hint 2^0
      PutU32(bb, 0);
      return;
    }
    for (int byte = 0; byte < size; ++byte)
    {
      bb.push_back(kLocalGet);
      PutU32(bb, addr_local);
      bb.push_back(kLocalGet);
      PutU32(bb, 2);
      const int shift = (size - 1 - byte) * 8;
      if (shift)
      {
        bb.push_back(kI32Const);
        PutI32(bb, shift);
        bb.push_back(kI32ShrU);
      }
      bb.push_back(kI32Store8);
      PutU32(bb, 0);
      PutU32(bb, static_cast<uint32_t>(byte));
    }
  };
  // A plain store may go to the gather pipe. Local 3 holds the pipe
  // pointer, which an update form needs for the address written back --
  // and no GX command is issued with an update form.
  GatherPipeStore gp;
  if (is_store && !update && l.gather_pipe_ea)
  {
    gp.size = size;
    gp.ptr_local = 3;
    gp.emit = emit_store_at;
  }

  // Local 2 is the value, which is not loaded until after translation, so
  // it is free to hold the table entry here.
  EmitTranslate(b, l, 1, 2, this_pc, insts_before, ins, 0, gp.size ? &gp : nullptr);

  if (is_store)
  {
    emit_store_at(b, 1);
  }
  else if (size == 4)
  {
    BeginStoreGpr(b);
    b.push_back(kLocalGet);
    PutU32(b, 1);
    b.push_back(kI32Load);
    PutU32(b, 0);
    PutU32(b, 0);
    b.push_back(kLocalSet);
    PutU32(b, 2);
    emit_bswap32_of_local2();
    EndStoreGpr(b, l, reg);
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
  b.push_back(kEnd);
}

// Which general-purpose registers an instruction reads and writes, for the
// busy-wait rule -- and whether the instruction is allowed in a busy-wait
// body at all.
//
// Only forms CompileBlockFetch translates need an answer, and the answer
// is deliberately conservative: anything with a side effect outside the
// register file (a store, an SPR move, MSR, a branch) is not idle, because
// skipping the loop would skip the effect.
//
// Returns false for "not eligible". `in` and `out` are bitmasks of GPRs.
inline bool IdleClassify(uint32_t ins, uint32_t* in, uint32_t* out)
{
  *in = 0;
  *out = 0;
  const auto reg = [](uint32_t r) { return 1u << r; };
  const uint32_t op = Opcode(ins);
  switch (op)
  {
  case 14: case 15:  // addi, addis -- rA == 0 is literal zero
    if (RA(ins)) *in |= reg(RA(ins));
    *out |= reg(RS(ins));
    return true;
  case 24: case 25: case 26: case 27:  // ori, oris, xori, xoris
  case 21:                             // rlwinm
    *in |= reg(RS(ins));
    *out |= reg(RA(ins));
    return true;
  case 7:  // mulli
    *in |= reg(RA(ins));
    *out |= reg(RS(ins));
    return true;
  case 10: case 11:  // cmpli, cmpi
    *in |= reg(RA(ins));
    return true;
  case 32: case 34: case 40: case 42:  // lwz, lbz, lhz, lha
  case 33: case 35: case 41: case 43:  // ... and the update forms
    if (RA(ins)) *in |= reg(RA(ins));
    *out |= reg(RS(ins));
    if (op & 1) *out |= reg(RA(ins));
    return true;
  case 31:
  {
    const uint32_t xo = XO(ins);
    switch (xo)
    {
    case 0: case 32:  // cmp, cmpl
      *in |= reg(RA(ins)) | reg(RB(ins));
      return true;
    case 23: case 87: case 279: case 343:    // lwzx, lbzx, lhzx, lhax
    case 55: case 119: case 311: case 375:   // ... update forms
      if (RA(ins)) *in |= reg(RA(ins));
      *in |= reg(RB(ins));
      *out |= reg(RS(ins));
      if (xo == 55 || xo == 119 || xo == 311 || xo == 375) *out |= reg(RA(ins));
      return true;
    case 266: case 40: case 235:  // add, subf, mullw
      *in |= reg(RA(ins)) | reg(RB(ins));
      *out |= reg(RS(ins));
      return true;
    case 104:  // neg
      *in |= reg(RA(ins));
      *out |= reg(RS(ins));
      return true;
    case 28: case 444: case 316: case 476: case 124: case 60: case 412: case 284:
    case 24: case 536:  // and, or, xor, nand, nor, andc, orc, eqv, slw, srw
      *in |= reg(RS(ins)) | reg(RB(ins));
      *out |= reg(RA(ins));
      return true;
    case 954: case 922: case 26:  // extsb, extsh, cntlzw
      *in |= reg(RS(ins));
      *out |= reg(RA(ins));
      return true;
    default:
      return false;  // stores, SPR and MSR moves, anything else
    }
  }
  default:
    return false;  // stores, branches, everything not listed
  }
}
}  // namespace detail

// Translate instructions starting at `code[0]` (guest address `start_pc`)
// until one is not understood or `max_insts` is reached.
// emit_module builds the single-function module in BlockResult::module.
// A caller that is packing bodies into a shared module does not need it,
// and building one is a second pass over the code plus the section
// scaffolding around it.
// `fetch(pc)` returns the guest instruction at that address, or anything
// that does not decode (zero will do) for an address the caller cannot
// supply. Threading a PC rather than walking an array is what lets a block
// FOLLOW a branch instead of stopping at it.
template <typename Fetch>
inline BlockResult CompileBlockFetch(Fetch fetch, uint32_t start_pc, uint32_t max_insts,
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
  std::vector<DirectFixup> fixups;
  uint32_t succ[2] = {0, 0};
  uint32_t nsucc = 0;
  auto add_succ = [&succ, &nsucc](uint32_t p) {
    if (nsucc < 2)
      succ[nsucc++] = p;
  };

  // The guest PC being compiled, tracked SEPARATELY from how many
  // instructions have been compiled. They are the same thing only while a
  // block is a straight run of memory; following a branch breaks the
  // relationship, and every place that used to say start_pc + n * 4 now
  // says this_pc instead.
  uint32_t cur_pc = start_pc;
  // The busy-wait rule, tracked as the block is emitted: a register read
  // before it is written in the loop may never be written in the loop. A
  // loop that fails it changes its own inputs and is doing real work.
  bool idle_ok = layout.idle_flag_addr != 0 && layout.enable_linking;
  uint32_t idle_written = 0, idle_disallowed = 0;
  // Floating point: whether this block has checked MSR[FP] (and HID2[PSE]
  // for paired singles) yet, and whether it needs the two i64 locals.
  bool fp_checked = false, pse_checked = false, lsqe_checked = false;
  uint32_t used_i64 = 0;
  // The two instructions compiled before the current one, for the mtmsr
  // peephole below. Zero when there is none.
  uint32_t prev_ins = 0, prev2_ins = 0;
  const bool fp_ok = layout.ps_offset != 0 && layout.fpscr_offset != 0;
  auto need_fp = [&](uint32_t pc, uint32_t before) {
    if (!fp_checked)
      EmitFpAvailable(body, layout, pc, before);
    fp_checked = true;
    used_i64 = used_i64 < 2 ? 2 : used_i64;
  };
  auto need_pse = [&](uint32_t pc, uint32_t before) {
    need_fp(pc, before);
    if (!pse_checked)
      EmitPseAvailable(body, layout, pc, before);
    pse_checked = true;
  };
  auto need_lsqe = [&](uint32_t pc, uint32_t before) {
    need_pse(pc, before);
    if (!lsqe_checked)
      EmitLsqeAvailable(body, layout, pc, before);
    lsqe_checked = true;
  };
  while (n < max_insts)
  {
    const uint32_t this_pc = cur_pc;
    const uint32_t ins = fetch(this_pc);
    // Classify for the idle rule BEFORE the instruction is emitted, since
    // the switch below `break`s out of several nested scopes.
    if (idle_ok && ins != 0x60000000u)  // nop is `ori r0,r0,0` and would fail the rule
    {
      uint32_t idle_in = 0, idle_out = 0;
      if (Opcode(ins) == 16 || !IdleClassify(ins, &idle_in, &idle_out))
      {
        // A conditional branch is only fine as the CLOSING branch, which is
        // decided where it is emitted; any other branch, or any form with
        // side effects, disqualifies the block. The flag is checked at the
        // branch before this update lands, so the closing branch itself
        // does not trip it.
        if (Opcode(ins) != 16)
          idle_ok = false;
      }
      else
      {
        idle_disallowed |= idle_in & ~idle_written;
        if (idle_out & idle_disallowed)
          idle_ok = false;
        idle_written |= idle_out;
      }
    }
    // Set by a handler that FOLLOWED a transfer: it has advanced n and
    // cur_pc itself, and the block carries on at the target.
    bool followed = false;
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

    // ── the carry forms with an immediate ──
    //
    // XER[CA] lives in its own byte of guest state (xer_ca), and the
    // interpreter's Helper_Carry(a, b) is "a + b overflows", which is the
    // unsigned test (a + b) < a. Each form below computes the result into
    // a local first so the carry can be derived from it.
    //
    // Measured with idle loops skipped: subfic was 6.2 million interpreted
    // instructions and addic. 1.8 million, the first and third largest
    // lines of what the interpreter was left holding.
    case 12:  // addic  rD, rA, SIMM
    case 13:  // addic. -- the Rc is implicit, NOT bit 0 of the instruction
    {
      if (!layout.xer_ca_offset)
        break;
      LoadGpr(body, layout, RA(ins));
      body.push_back(kLocalSet);
      PutU32(body, 1);
      BeginStoreGpr(body);
      body.push_back(kLocalGet);
      PutU32(body, 1);
      body.push_back(kI32Const);
      PutI32(body, SIMM(ins));
      body.push_back(kI32Add);
      body.push_back(kLocalTee);
      PutU32(body, 2);
      EndStoreGpr(body, layout, RS(ins));
      // CA = result <u a
      body.push_back(kLocalGet);
      PutU32(body, 0);
      body.push_back(kLocalGet);
      PutU32(body, 2);
      body.push_back(kLocalGet);
      PutU32(body, 1);
      body.push_back(kI32LtU);
      EmitEndStoreCA(body, layout);
      if (op == 13)
        EmitUpdateCR0(body, layout, RS(ins));
      used_locals = used_locals < 3 ? 3 : used_locals;
      handled = true;
      break;
    }

    case 8:  // subfic rD, rA, SIMM -- rD = SIMM - rA
    {
      if (!layout.xer_ca_offset)
        break;
      LoadGpr(body, layout, RA(ins));
      body.push_back(kLocalSet);
      PutU32(body, 1);
      BeginStoreGpr(body);
      body.push_back(kI32Const);
      PutI32(body, SIMM(ins));
      body.push_back(kLocalGet);
      PutU32(body, 1);
      body.push_back(kI32Sub);
      body.push_back(kLocalTee);
      PutU32(body, 2);
      EndStoreGpr(body, layout, RS(ins));
      // CA = (a == 0) | (result <u (0 - a)), which is the interpreter's
      // (a == 0) || Helper_Carry(0 - a, imm) written as the overflow test.
      body.push_back(kLocalGet);
      PutU32(body, 0);
      body.push_back(kLocalGet);
      PutU32(body, 1);
      body.push_back(kI32Eqz);
      body.push_back(kLocalGet);
      PutU32(body, 2);
      body.push_back(kI32Const);
      PutI32(body, 0);
      body.push_back(kLocalGet);
      PutU32(body, 1);
      body.push_back(kI32Sub);
      body.push_back(kI32LtU);
      body.push_back(kI32Or);
      EmitEndStoreCA(body, layout);
      used_locals = used_locals < 3 ? 3 : used_locals;
      handled = true;
      break;
    }

    case 20:  // rlwimi rA, rS, SH, MB, ME -- insert under mask
    {
      const uint32_t mask = Mask(MB(ins), ME(ins));
      BeginStoreGpr(body);
      LoadGpr(body, layout, RA(ins));
      body.push_back(kI32Const);
      PutI32(body, static_cast<int32_t>(~mask));
      body.push_back(kI32And);
      LoadGpr(body, layout, RS(ins));
      if (SH(ins))
      {
        body.push_back(kI32Const);
        PutI32(body, static_cast<int32_t>(SH(ins)));
        body.push_back(kI32Rotl);
      }
      body.push_back(kI32Const);
      PutI32(body, static_cast<int32_t>(mask));
      body.push_back(kI32And);
      body.push_back(kI32Or);
      EndStoreGpr(body, layout, RA(ins));
      handled = true;
      break;
    }

    case 23:  // rlwnm rA, rS, rB, MB, ME -- rotate by a register
    {
      BeginStoreGpr(body);
      LoadGpr(body, layout, RS(ins));
      LoadGpr(body, layout, RB(ins));
      body.push_back(kI32Const);
      PutI32(body, 31);
      body.push_back(kI32And);
      body.push_back(kI32Rotl);
      body.push_back(kI32Const);
      PutI32(body, static_cast<int32_t>(Mask(MB(ins), ME(ins))));
      body.push_back(kI32And);
      EndStoreGpr(body, layout, RA(ins));
      handled = true;
      break;
    }

    case 46:  // lmw
    case 47:  // stmw
      EmitLoadStoreMultiple(body, layout, this_pc, n, RA(ins), SIMM(ins), RS(ins), op == 47);
      touches_memory = true;
      used_locals = used_locals < 3 ? 3 : used_locals;
      handled = true;
      break;

    // ── floating-point loads and stores, displacement form ──
    case 48: case 49: case 50: case 51: case 52: case 53: case 54: case 55:
    {
      if (!fp_ok)
        break;
      const bool is_double = (op == 50 || op == 51 || op == 54 || op == 55);
      const bool is_store = op >= 52;
      const bool update = (op & 1) != 0;
      if (update && RA(ins) == 0)
        break;
      need_fp(this_pc, n);
      EmitFpMemOp(body, layout, this_pc, n, RA(ins), 0, SIMM(ins), false, RS(ins), is_double,
                  is_store, update, false, ins);
      touches_memory = true;
      used_locals = used_locals < 4 ? 4 : used_locals;
      handled = true;
      break;
    }

    // ── quantized paired-single loads and stores, displacement form ──
    case 56: case 57: case 60: case 61:
    {
      if (!fp_ok)
        break;
      const bool is_store = (op >= 60);
      const bool update = (op & 1) != 0;
      if (update && RA(ins) == 0)
        break;
      const uint32_t gqr_i = (ins >> 12) & 7;
      const bool w = ((ins >> 15) & 1) != 0;
      const int32_t d12 = static_cast<int32_t>(ins << 20) >> 20;
      if (!PsqInline(layout, gqr_i, is_store))
        break;
      need_lsqe(this_pc, n);
      EmitPsq(body, layout, this_pc, n, RA(ins), 0, d12, false, RS(ins), gqr_i, w, is_store,
              update, ins);
      touches_memory = true;
      used_locals = used_locals < 4 ? 4 : used_locals;
      handled = true;
      break;
    }

    // ── floating-point arithmetic ──
    case 59:
    {
      if (!fp_ok || !layout.interp_hatch)
        break;
      const uint32_t xo5 = (ins >> 1) & 0x1F;
      int kind = -1;
      if (xo5 == 21) kind = 0;
      else if (xo5 == 20) kind = 1;
      else if (xo5 == 25) kind = 2;
      else if (xo5 == 18) kind = 3;
      if (kind < 0)
        break;
      need_fp(this_pc, n);
      EmitFpBinary(body, layout, ins, this_pc, n, kind, true, false, -1);
      if (Rc(ins))
        EmitUpdateCR1(body, layout);
      touches_memory = true;
      handled = true;
      break;
    }

    // ── floating-point moves, which touch no flags ──
    //
    // fmr, fneg, fabs, fnabs and fsel are bit operations on ps0; the
    // paired forms in opcode 4 do the same to both halves, and the merges
    // shuffle halves between two registers. Every source is read into an
    // i64 local before any destination is written, since D may equal A or B.
    case 63:
    case 4:
    {
      if (!fp_ok)
        break;
      const uint32_t xo = XO(ins);
      const bool paired = (op == 4);
      // psq_lx, psq_stx, psq_lux, psq_stux: the X-forms, with the GQR and
      // W in the low bits of the ten-bit field. No other opcode-4 form
      // has 6, 7, 38 or 39 in its low six bits.
      if (paired)
      {
        const uint32_t xo6 = (ins >> 1) & 0x3F;
        if (xo6 == 6 || xo6 == 7 || xo6 == 38 || xo6 == 39)
        {
          const bool is_store = (xo6 & 1) != 0;
          const bool update = xo6 >= 38;
          if (update && RA(ins) == 0)
            break;
          const uint32_t gqr_i = (ins >> 7) & 7;
          const bool w = ((ins >> 10) & 1) != 0;
          if (!PsqInline(layout, gqr_i, is_store))
            break;
          need_pse(this_pc, n);
          EmitPsq(body, layout, this_pc, n, RA(ins), RB(ins), 0, true, RS(ins), gqr_i, w,
                  is_store, update, ins);
          touches_memory = true;
          used_locals = used_locals < 4 ? 4 : used_locals;
          handled = true;
          break;
        }
      }
      const bool is_move = (xo == 72 || xo == 40 || xo == 264 || xo == 136);
      const bool is_merge = paired && (xo == 528 || xo == 560 || xo == 592 || xo == 624);
      // fsel/ps_sel use the A-form: the five-bit XO is 23 and the rest of
      // the ten-bit field is frC. The X-form moves above match on all ten
      // bits, and no A-form opcode has 8 or 16 in its low five, so the two
      // tests cannot both be true.
      const bool is_sel = ((ins >> 1) & 0x1F) == 23;
      if (!paired && (xo == 0 || xo == 32) && layout.interp_hatch)
      {
        // fcmpu / fcmpo; bit 0 is reserved and the low two bits of the
        // CR field operand are zero.
        if (Rc(ins) || (RS(ins) & 3))
          break;
        need_fp(this_pc, n);
        EmitFcmp(body, layout, ins, this_pc, n);
        touches_memory = true;
        handled = true;
        break;
      }
      if (!is_move && !is_merge && !is_sel)
      {
        // The arithmetic: fadd/fsub/fmul/fdiv and frsp for op 63; ps_add,
        // ps_sub, ps_mul, ps_div, ps_muls0 and ps_muls1 for op 4.
        if (!layout.interp_hatch)
          break;
        const uint32_t xo5 = (ins >> 1) & 0x1F;
        int kind = -1, c_half = -1;
        if (xo5 == 21) kind = 0;
        else if (xo5 == 20) kind = 1;
        else if (xo5 == 25) kind = 2;
        else if (xo5 == 18) kind = 3;
        else if (paired && xo5 == 12) { kind = 2; c_half = 0; }
        else if (paired && xo5 == 13) { kind = 2; c_half = 1; }
        const bool frsp = !paired && xo == 12;
        // ps_madds0 14, ps_madds1 15, ps_msub 28, ps_madd 29, ps_nmsub 30,
        // ps_nmadd 31.
        const bool madd = paired && layout.paired_madd &&
                          (xo5 == 14 || xo5 == 15 || (xo5 >= 28 && xo5 <= 31));
        if (madd)
        {
          need_pse(this_pc, n);
          used_i64 = used_i64 < 6 ? 6 : used_i64;
          EmitPsMadd(body, layout, ins, this_pc, n, xo5 == 28 || xo5 == 30,
                     xo5 == 30 || xo5 == 31, xo5 == 14 ? 0 : xo5 == 15 ? 1 : -1);
          if (Rc(ins))
            EmitUpdateCR1(body, layout);
          touches_memory = true;
          handled = true;
          break;
        }
        if (kind < 0 && !frsp)
          break;
        if (paired)
          need_pse(this_pc, n);
        else
          need_fp(this_pc, n);
        if (frsp)
          EmitFrsp(body, layout, ins, this_pc, n);
        else
          EmitFpBinary(body, layout, ins, this_pc, n, kind, paired, paired, c_half);
        if (Rc(ins))
          EmitUpdateCR1(body, layout);
        touches_memory = true;
        handled = true;
        break;
      }
      const uint32_t fd = RS(ins), fa = RA(ins), fb = RB(ins), fc = (ins >> 6) & 0x1F;
      if (paired)
        need_pse(this_pc, n);
      else
        need_fp(this_pc, n);
      const uint32_t pd = layout.ps_offset + fd * 16;
      const uint32_t pa = layout.ps_offset + fa * 16;
      const uint32_t pb = layout.ps_offset + fb * 16;
      const uint32_t pc_ = layout.ps_offset + fc * 16;
      auto load64 = [&](uint32_t off) {
        body.push_back(kLocalGet);
        PutU32(body, 0);
        body.push_back(kI64Load);
        PutU32(body, 3);
        PutU32(body, off);
      };
      auto store64_from_local = [&](uint32_t off, uint32_t local) {
        body.push_back(kLocalGet);
        PutU32(body, 0);
        body.push_back(kLocalGet);
        PutU32(body, local);
        body.push_back(kI64Store);
        PutU32(body, 3);
        PutU32(body, off);
      };
      auto apply = [&](uint32_t xo_) {
        // The value is on the stack; apply the move's bit operation.
        if (xo_ == 40)  // neg: flip the sign
        {
          body.push_back(kI64Const);
          PutI64(body, static_cast<int64_t>(0x8000000000000000ull));
          body.push_back(kI64Xor);
        }
        else if (xo_ == 264)  // abs: clear the sign
        {
          body.push_back(kI64Const);
          PutI64(body, 0x7FFFFFFFFFFFFFFFll);
          body.push_back(kI64And);
        }
        else if (xo_ == 136)  // nabs: set the sign
        {
          body.push_back(kI64Const);
          PutI64(body, static_cast<int64_t>(0x8000000000000000ull));
          body.push_back(kI64Or);
        }
      };
      if (is_sel)
      {
        // d = (a >= -0.0) ? c : b, per half for ps_sel. NaN compares false
        // and selects b, as the interpreter's C++ does.
        for (int half = 0; half < (paired ? 2 : 1); ++half)
        {
          const uint32_t h = half * 8;
          load64(pc_ + h);
          load64(pb + h);
          body.push_back(kLocalGet);
          PutU32(body, 0);
          body.push_back(kF64Load);
          PutU32(body, 3);
          PutU32(body, pa + h);
          body.push_back(kF64Const);
          for (int k = 0; k < 8; ++k)
            body.push_back(k == 7 ? 0x80 : 0x00);  // -0.0
          body.push_back(kF64Ge);
          body.push_back(kSelect);
          body.push_back(kLocalSet);
          PutU32(body, 9 + half);
        }
        for (int half = 0; half < (paired ? 2 : 1); ++half)
          store64_from_local(pd + half * 8, 9 + half);
      }
      else if (is_merge)
      {
        const bool a_hi = (xo == 592 || xo == 624);
        const bool b_hi = (xo == 560 || xo == 624);
        load64(pa + (a_hi ? 8 : 0));
        body.push_back(kLocalSet);
        PutU32(body, 9);
        load64(pb + (b_hi ? 8 : 0));
        body.push_back(kLocalSet);
        PutU32(body, 10);
        store64_from_local(pd, 9);
        store64_from_local(pd + 8, 10);
      }
      else
      {
        load64(pb);
        apply(xo);
        body.push_back(kLocalSet);
        PutU32(body, 9);
        if (paired)
        {
          load64(pb + 8);
          apply(xo);
          body.push_back(kLocalSet);
          PutU32(body, 10);
        }
        store64_from_local(pd, 9);
        if (paired)
          store64_from_local(pd + 8, 10);
      }
      if (Rc(ins))
        EmitUpdateCR1(body, layout);
      handled = true;
      break;
    }

    case 31:  // the extended-opcode space
    {
      // Rc is allowed only for the forms whose destination register is
      // known, plus stwcx., whose Rc bit is always set and whose CR0 write
      // is part of the instruction. For everything else in this opcode
      // space -- memory, SPR moves, mtmsr -- bit 0 is reserved, and a set
      // bit means an encoding this does not understand.
      if (Rc(ins) && RcDestination(ins) < 0 && XO(ins) != 150)
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

      // mfcr and mtcrf: the whole condition register as a word.
      if (xo == 19 && layout.cr_offset)
      {
        EmitMfcr(body, layout, RS(ins));
        used_i64 = used_i64 < 2 ? 2 : used_i64;
        handled = true;
        break;
      }
      if (xo == 144 && layout.cr_offset && layout.cr_table_base && !(ins & (1u << 20)))
      {
        EmitMtcrf(body, layout, RS(ins), (ins >> 12) & 0xFF);
        used_i64 = used_i64 < 2 ? 2 : used_i64;
        handled = true;
        break;
      }

      // mfspr / mtspr, restricted to the link and count registers.
      //
      // Other SPRs have side effects -- writing the decrementer reschedules
      // an interrupt, writing a BAT changes address translation -- and a
      // compiled block that moved those around without telling the rest of
      // the emulator would be wrong in ways no register comparison catches.
      // mftb: the time base, computed from CoreTiming's globals exactly as
      // the interpreter computes it, and written to TL and TU as
      // WriteFullTimeBaseValue does before the requested half is copied.
      if (xo == 371 && layout.tb_global_timer_addr && layout.ps_offset)
      {
        const uint32_t tbr = ((ins >> 16) & 0x1F) | (((ins >> 11) & 0x1F) << 5);
        if (tbr != 268 && tbr != 269)
          break;
        used_i64 = used_i64 < 2 ? 2 : used_i64;
        // ticks = global_timer + (slice_length - downcount)
        body.push_back(kI32Const);
        PutI32(body, static_cast<int32_t>(layout.tb_global_timer_addr));
        body.push_back(kI64Load);
        PutU32(body, 3);
        PutU32(body, 0);
        body.push_back(kI32Const);
        PutI32(body, static_cast<int32_t>(layout.tb_slice_length_addr));
        body.push_back(kI32Load);
        PutU32(body, 2);
        PutU32(body, 0);
        body.push_back(kLocalGet);
        PutU32(body, 0);
        body.push_back(kI32Load);
        PutU32(body, 2);
        PutU32(body, layout.downcount_offset);
        body.push_back(kI32Sub);
        body.push_back(kI64ExtendI32S);
        body.push_back(kI64Add);
        // tb = start_value + (ticks - start_ticks) / 12
        body.push_back(kI32Const);
        PutI32(body, static_cast<int32_t>(layout.tb_start_ticks_addr));
        body.push_back(kI64Load);
        PutU32(body, 3);
        PutU32(body, 0);
        body.push_back(kI64Sub);
        body.push_back(kI64Const);
        PutI64(body, 12);
        body.push_back(kI64DivU);
        body.push_back(kI32Const);
        PutI32(body, static_cast<int32_t>(layout.tb_start_value_addr));
        body.push_back(kI64Load);
        PutU32(body, 3);
        PutU32(body, 0);
        body.push_back(kI64Add);
        body.push_back(kLocalSet);
        PutU32(body, 9);
        // spr[TL] = low word, spr[TU] = high word
        body.push_back(kLocalGet);
        PutU32(body, 0);
        body.push_back(kLocalGet);
        PutU32(body, 9);
        body.push_back(kI32WrapI64);
        body.push_back(kI32Store);
        PutU32(body, 2);
        PutU32(body, layout.spr_offset + 268 * 4);
        body.push_back(kLocalGet);
        PutU32(body, 0);
        body.push_back(kLocalGet);
        PutU32(body, 9);
        body.push_back(kI64Const);
        PutI64(body, 32);
        body.push_back(kI64ShrU);
        body.push_back(kI32WrapI64);
        body.push_back(kI32Store);
        PutU32(body, 2);
        PutU32(body, layout.spr_offset + 269 * 4);
        // rD = the half asked for
        BeginStoreGpr(body);
        body.push_back(kLocalGet);
        PutU32(body, 0);
        body.push_back(kI32Load);
        PutU32(body, 2);
        PutU32(body, layout.spr_offset + tbr * 4);
        EndStoreGpr(body, layout, RS(ins));
        handled = true;
        break;
      }

      // ── floating-point loads and stores, indexed form ──
      if (fp_ok)
      {
        struct FForm { bool is_double, store, update, as_int; };
        FForm ff{};
        bool is_fmem = true;
        switch (xo)
        {
        case 535: ff = {false, false, false, false}; break;  // lfsx
        case 567: ff = {false, false, true,  false}; break;  // lfsux
        case 599: ff = {true,  false, false, false}; break;  // lfdx
        case 631: ff = {true,  false, true,  false}; break;  // lfdux
        case 663: ff = {false, true,  false, false}; break;  // stfsx
        case 695: ff = {false, true,  true,  false}; break;  // stfsux
        case 727: ff = {true,  true,  false, false}; break;  // stfdx
        case 759: ff = {true,  true,  true,  false}; break;  // stfdux
        case 983: ff = {false, true,  false, true};  break;  // stfiwx
        default: is_fmem = false; break;
        }
        if (is_fmem)
        {
          if (ff.update && RA(ins) == 0)
            break;
          need_fp(this_pc, n);
          EmitFpMemOp(body, layout, this_pc, n, RA(ins), RB(ins), 0, true, RS(ins), ff.is_double,
                      ff.store, ff.update, ff.as_int, ins);
          touches_memory = true;
          used_locals = used_locals < 4 ? 4 : used_locals;
          handled = true;
          break;
        }
      }

      if (xo == 339 || xo == 467)
      {
        // The SPR field is SPLIT and the halves are swapped, which is the
        // classic way to decode this one wrong.
        const uint32_t spr = ((ins >> 16) & 0x1F) | (((ins >> 11) & 0x1F) << 5);

        // XER is kept in three fields of guest state and reassembled or
        // split here as PowerPCState::GetXER / SetXER do.
        if (spr == 1 && layout.xer_ca_offset && layout.xer_stringctrl_offset)
        {
          if (xo == 339)
          {
            BeginStoreGpr(body);
            body.push_back(kLocalGet);
            PutU32(body, 0);
            body.push_back(0x2F);  // i32.load16_u
            PutU32(body, 1);
            PutU32(body, layout.xer_stringctrl_offset);
            EmitLoadCA(body, layout);
            body.push_back(kI32Const);
            PutI32(body, 29);
            body.push_back(kI32Shl);
            body.push_back(kI32Or);
            body.push_back(kLocalGet);
            PutU32(body, 0);
            body.push_back(kI32Load8U);
            PutU32(body, 0);
            PutU32(body, layout.xer_so_ov_offset);
            body.push_back(kI32Const);
            PutI32(body, 30);
            body.push_back(kI32Shl);
            body.push_back(kI32Or);
            EndStoreGpr(body, layout, RS(ins));
          }
          else
          {
            LoadGpr(body, layout, RS(ins));
            body.push_back(kLocalSet);
            PutU32(body, 1);
            // stringctrl = BYTE_COUNT | BYTE_CMP << 8 = xer & 0xFF7F
            body.push_back(kLocalGet);
            PutU32(body, 0);
            body.push_back(kLocalGet);
            PutU32(body, 1);
            body.push_back(kI32Const);
            PutI32(body, 0xFF7F);
            body.push_back(kI32And);
            body.push_back(0x3B);  // i32.store16
            PutU32(body, 1);
            PutU32(body, layout.xer_stringctrl_offset);
            // ca = bit 29
            body.push_back(kLocalGet);
            PutU32(body, 0);
            body.push_back(kLocalGet);
            PutU32(body, 1);
            body.push_back(kI32Const);
            PutI32(body, 29);
            body.push_back(kI32ShrU);
            body.push_back(kI32Const);
            PutI32(body, 1);
            body.push_back(kI32And);
            EmitEndStoreCA(body, layout);
            // so_ov = (SO << 1) | OV = bits 31 and 30
            body.push_back(kLocalGet);
            PutU32(body, 0);
            body.push_back(kLocalGet);
            PutU32(body, 1);
            body.push_back(kI32Const);
            PutI32(body, 30);
            body.push_back(kI32ShrU);
            body.push_back(kI32Const);
            PutI32(body, 3);
            body.push_back(kI32And);
            body.push_back(kI32Store8);
            PutU32(body, 0);
            PutU32(body, layout.xer_so_ov_offset);
            used_locals = used_locals < 2 ? 2 : used_locals;
          }
          handled = true;
          break;
        }

        // Plain moves: registers the interpreter reads and writes with no
        // side effect (Interpreter_SystemRegisters.cpp's default cases).
        // Everything else -- DEC, HID0, the BATs, WPAR, the performance
        // monitor -- has one, and stays with the interpreter.
        const bool user_spr = (spr == layout.lr_index || spr == layout.ctr_index);
        const bool plain_spr = user_spr || spr == 26 || spr == 27 ||        // SRR0, SRR1
                               spr == 18 || spr == 19 ||                    // DSISR, DAR
                               (spr >= 272 && spr <= 275) ||                // SPRG0-3
                               (spr >= 912 && spr <= 919);                  // GQR0-7
        const bool read_only_plain = (xo == 339) && (spr == 287 || spr == 1008 ||  // PVR, HID0
                                                     spr == 920 || spr == 1011);   // HID2, HID4
        if (!plain_spr && !read_only_plain)
          break;
        if (!user_spr)
        {
          // Privileged: in user mode the interpreter raises a program
          // exception, so the block refuses there.
          body.push_back(kLocalGet);
          PutU32(body, 0);
          body.push_back(kI32Load);
          PutU32(body, 2);
          PutU32(body, layout.msr_offset);
          body.push_back(kI32Const);
          PutI32(body, 1 << 14);
          body.push_back(kI32And);
          body.push_back(kIf);
          body.push_back(kVoid);
          EmitBailBody(body, layout, this_pc, n);
          body.push_back(kEnd);
        }
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
          EmitMemOp(body, layout, this_pc, n, RA(ins), RB(ins), 0, true, RS(ins),
                    xf.size, xf.store, xf.sign, xf.update, ins);
      touches_memory = true;
          used_locals = used_locals < 4 ? 4 : used_locals;
          handled = true;
          break;
        }
      }

      // ── the reservation ──
      //
      // lwarx loads a word and records the address; stwcx. stores only if
      // the reservation is still held for that address, and reports which
      // in CR0 (Interpreter_LoadStore.cpp lwarx / stwcxd). Nothing else in
      // compiled code clears the reservation; the interpreter's own
      // stwcx. does, and an exception path does not, which matches.
      if ((xo == 20 || xo == 150) && layout.reserve_offset)
      {
        LoadGprOrZero(body, layout, RA(ins));
        LoadGpr(body, layout, RB(ins));
        body.push_back(kI32Add);
        body.push_back(kLocalTee);
        PutU32(body, 1);
        body.push_back(kLocalSet);
        PutU32(body, 3);  // the guest address, kept for the reservation
        body.push_back(kLocalGet);
        PutU32(body, 1);
        body.push_back(kI32Const);
        PutI32(body, 3);
        body.push_back(kI32And);
        body.push_back(kIf);
        body.push_back(kVoid);
        EmitBailBody(body, layout, this_pc, n);
        body.push_back(kEnd);
        if (xo == 20)  // lwarx
        {
          EmitTranslate(body, layout, 1, 2, this_pc, n);
          BeginStoreGpr(body);
          body.push_back(kLocalGet);
          PutU32(body, 1);
          body.push_back(kI32Load);
          PutU32(body, 0);
          PutU32(body, 0);
          body.push_back(kLocalSet);
          PutU32(body, 2);
          EmitBswap32(body, 2);
          EndStoreGpr(body, layout, RS(ins));
          body.push_back(kLocalGet);
          PutU32(body, 0);
          body.push_back(kLocalGet);
          PutU32(body, 3);
          body.push_back(kI32Store);
          PutU32(body, 2);
          PutU32(body, layout.reserve_address_offset);
          body.push_back(kLocalGet);
          PutU32(body, 0);
          body.push_back(kI32Const);
          PutI32(body, 1);
          body.push_back(kI32Store8);
          PutU32(body, 0);
          PutU32(body, layout.reserve_offset);
        }
        else  // stwcx.
        {
          // CR0 = s_crTable[(stored ? 2 : 0) | XER[SO]], and the address
          // for that store goes under everything else.
          body.push_back(kLocalGet);
          PutU32(body, 0);
          // held = reserve && reserve_address == ea
          body.push_back(kLocalGet);
          PutU32(body, 0);
          body.push_back(kI32Load8U);
          PutU32(body, 0);
          PutU32(body, layout.reserve_offset);
          body.push_back(kLocalGet);
          PutU32(body, 0);
          body.push_back(kI32Load);
          PutU32(body, 2);
          PutU32(body, layout.reserve_address_offset);
          body.push_back(kLocalGet);
          PutU32(body, 3);
          body.push_back(kI32Eq);
          body.push_back(kI32And);
          body.push_back(kIf);
          body.push_back(kI32);
          {
            EmitTranslate(body, layout, 1, 2, this_pc, n);
            body.push_back(kLocalGet);
            PutU32(body, 1);
            LoadGpr(body, layout, RS(ins));
            body.push_back(kLocalSet);
            PutU32(body, 2);
            EmitBswap32(body, 2);
            body.push_back(kI32Store);
            PutU32(body, 0);
            PutU32(body, 0);
            body.push_back(kLocalGet);
            PutU32(body, 0);
            body.push_back(kI32Const);
            PutI32(body, 0);
            body.push_back(kI32Store8);
            PutU32(body, 0);
            PutU32(body, layout.reserve_offset);
            body.push_back(kI32Const);
            PutI32(body, 2);
          }
          body.push_back(kElse);
          body.push_back(kI32Const);
          PutI32(body, 0);
          body.push_back(kEnd);
          // | XER[SO]
          body.push_back(kLocalGet);
          PutU32(body, 0);
          body.push_back(kI32Load8U);
          PutU32(body, 0);
          PutU32(body, layout.xer_so_ov_offset);
          body.push_back(kI32Const);
          PutI32(body, 1);
          body.push_back(kI32ShrU);
          body.push_back(kI32Const);
          PutI32(body, 1);
          body.push_back(kI32And);
          body.push_back(kI32Or);
          body.push_back(kI32Const);
          PutI32(body, 3);
          body.push_back(kI32Shl);
          body.push_back(kI32Const);
          PutI32(body, static_cast<int32_t>(layout.cr_table_base));
          body.push_back(kI32Add);
          body.push_back(kI64Load);
          PutU32(body, 3);
          PutU32(body, 0);
          body.push_back(kI64Store);
          PutU32(body, 3);
          PutU32(body, layout.cr_offset);
        }
        touches_memory = true;
        used_locals = used_locals < 4 ? 4 : used_locals;
        handled = true;
        break;
      }

      // ── cache control and barriers ──
      //
      // Without data cache emulation, the interpreter's dcbf, dcbst and
      // dcbi do nothing but poke a JIT interface that has no JIT behind
      // it; dcbt, dcbtst, sync and eieio do nothing at all. Measured with
      // idle loops skipped, dcbi and dcbf together were 1.45 million
      // interpreted instructions, each one ending a block.
      if (xo == 278 || xo == 246 || xo == 598 || xo == 854)  // dcbt, dcbtst, sync, eieio
      {
        handled = true;
        break;
      }
      if ((xo == 54 || xo == 86 || xo == 470) && !layout.dcache_enabled)  // dcbst, dcbf, dcbi
      {
        if (xo == 470)
        {
          // dcbi is privileged; in user mode it is a program exception,
          // which the interpreter raises.
          body.push_back(kLocalGet);
          PutU32(body, 0);
          body.push_back(kI32Load);
          PutU32(body, 2);
          PutU32(body, layout.msr_offset);
          body.push_back(kI32Const);
          PutI32(body, 1 << 14);
          body.push_back(kI32And);
          body.push_back(kIf);
          body.push_back(kVoid);
          EmitBailBody(body, layout, this_pc, n);
          body.push_back(kEnd);
        }
        handled = true;
        break;
      }

      // dcbz: zero the 32-byte line holding the address.
      //
      // Refused when HID0[DCE] is clear, where the interpreter raises an
      // alignment exception instead (Interpreter_LoadStore.cpp), and left
      // to the interpreter entirely when the data cache is emulated or
      // the low-memory hack is on. Measured over the boot: 2.34 million
      // interpreted instructions, the largest single line before MEM2.
      if (xo == 1014 && !layout.dcache_enabled && !layout.low_dcbz_hack)
      {
        // HID0[DCE] is bit 14 of SPR 1008.
        body.push_back(kLocalGet);
        PutU32(body, 0);
        body.push_back(kI32Load);
        PutU32(body, 2);
        PutU32(body, layout.spr_offset + 1008 * 4);
        body.push_back(kI32Const);
        PutI32(body, 1 << 14);
        body.push_back(kI32And);
        body.push_back(kI32Eqz);
        body.push_back(kIf);
        body.push_back(kVoid);
        EmitBailBody(body, layout, this_pc, n);
        body.push_back(kEnd);
        // ea = (rA|0 + rB) & ~31
        LoadGprOrZero(body, layout, RA(ins));
        LoadGpr(body, layout, RB(ins));
        body.push_back(kI32Add);
        body.push_back(kI32Const);
        PutI32(body, -32);
        body.push_back(kI32And);
        body.push_back(kLocalSet);
        PutU32(body, 1);
        EmitTranslate(body, layout, 1, 2, this_pc, n);
        body.push_back(kLocalGet);
        PutU32(body, 1);
        body.push_back(kI32Const);
        PutI32(body, 0);
        body.push_back(kI32Const);
        PutI32(body, 32);
        body.push_back(kPrefixFC);
        PutU32(body, kMemoryFill);
        body.push_back(0x00);  // memory index
        touches_memory = true;
        used_locals = used_locals < 3 ? 3 : used_locals;
        handled = true;
        break;
      }

      // ── the carry forms with a register operand ──
      //
      // OE (bit 10) forms also set XER[OV] and are left to the interpreter.
      if ((xo == 10 || xo == 8 || xo == 138 || xo == 136 || xo == 202 || xo == 200) &&
          layout.xer_ca_offset && !(ins & (1u << 10)))
      {
        // local 1 = the first operand as the carry test sees it: a for the
        // add forms, ~a for the subtract-from forms, which the interpreter
        // computes as ~a + b + carry.
        LoadGpr(body, layout, RA(ins));
        if (xo == 136 || xo == 200)
        {
          body.push_back(kI32Const);
          PutI32(body, -1);
          body.push_back(kI32Xor);
        }
        body.push_back(kLocalSet);
        PutU32(body, 1);

        if (xo == 10)  // addc: r = a + b, CA = r <u a
        {
          BeginStoreGpr(body);
          body.push_back(kLocalGet);
          PutU32(body, 1);
          LoadGpr(body, layout, RB(ins));
          body.push_back(kI32Add);
          body.push_back(kLocalTee);
          PutU32(body, 2);
          EndStoreGpr(body, layout, RS(ins));
          body.push_back(kLocalGet);
          PutU32(body, 0);
          body.push_back(kLocalGet);
          PutU32(body, 2);
          body.push_back(kLocalGet);
          PutU32(body, 1);
          body.push_back(kI32LtU);
          EmitEndStoreCA(body, layout);
        }
        else if (xo == 8)  // subfc: r = b - a, CA = (a == 0) | (r <u (0 - a))
        {
          BeginStoreGpr(body);
          LoadGpr(body, layout, RB(ins));
          body.push_back(kLocalGet);
          PutU32(body, 1);
          body.push_back(kI32Sub);
          body.push_back(kLocalTee);
          PutU32(body, 2);
          EndStoreGpr(body, layout, RS(ins));
          body.push_back(kLocalGet);
          PutU32(body, 0);
          body.push_back(kLocalGet);
          PutU32(body, 1);
          body.push_back(kI32Eqz);
          body.push_back(kLocalGet);
          PutU32(body, 2);
          body.push_back(kI32Const);
          PutI32(body, 0);
          body.push_back(kLocalGet);
          PutU32(body, 1);
          body.push_back(kI32Sub);
          body.push_back(kI32LtU);
          body.push_back(kI32Or);
          EmitEndStoreCA(body, layout);
        }
        else if (xo == 138 || xo == 136)  // adde, subfe: t = x + b; r = t + ca
        {
          // CA = (t <u x) | (r <u t): either addition can carry, never both.
          body.push_back(kLocalGet);
          PutU32(body, 1);
          LoadGpr(body, layout, RB(ins));
          body.push_back(kI32Add);
          body.push_back(kLocalSet);
          PutU32(body, 2);
          BeginStoreGpr(body);
          body.push_back(kLocalGet);
          PutU32(body, 2);
          EmitLoadCA(body, layout);
          body.push_back(kI32Add);
          body.push_back(kLocalTee);
          PutU32(body, 3);
          EndStoreGpr(body, layout, RS(ins));
          body.push_back(kLocalGet);
          PutU32(body, 0);
          body.push_back(kLocalGet);
          PutU32(body, 2);
          body.push_back(kLocalGet);
          PutU32(body, 1);
          body.push_back(kI32LtU);
          body.push_back(kLocalGet);
          PutU32(body, 3);
          body.push_back(kLocalGet);
          PutU32(body, 2);
          body.push_back(kI32LtU);
          body.push_back(kI32Or);
          EmitEndStoreCA(body, layout);
        }
        else  // addze, subfze: r = x + ca, CA = r <u x
        {
          BeginStoreGpr(body);
          body.push_back(kLocalGet);
          PutU32(body, 1);
          EmitLoadCA(body, layout);
          body.push_back(kI32Add);
          body.push_back(kLocalTee);
          PutU32(body, 2);
          EndStoreGpr(body, layout, RS(ins));
          body.push_back(kLocalGet);
          PutU32(body, 0);
          body.push_back(kLocalGet);
          PutU32(body, 2);
          body.push_back(kLocalGet);
          PutU32(body, 1);
          body.push_back(kI32LtU);
          EmitEndStoreCA(body, layout);
        }
        used_locals = used_locals < 4 ? 4 : used_locals;
        handled = true;
        break;
      }

      // ── divide and high multiply ──
      //
      // WebAssembly traps on division by zero and on INT_MIN / -1; PowerPC
      // defines both (Interpreter_Integer.cpp), so the cases are tested
      // first and the division only runs when it cannot trap.
      if ((xo == 491 || xo == 459) && !(ins & (1u << 10)))
      {
        BeginStoreGpr(body);
        if (xo == 459)  // divwu: b == 0 ? 0 : a / b
        {
          LoadGpr(body, layout, RB(ins));
          body.push_back(kI32Eqz);
          body.push_back(kIf);
          body.push_back(kI32);
          body.push_back(kI32Const);
          PutI32(body, 0);
          body.push_back(kElse);
          LoadGpr(body, layout, RA(ins));
          LoadGpr(body, layout, RB(ins));
          body.push_back(kI32DivU);
          body.push_back(kEnd);
        }
        else  // divw: overflow ? (a < 0 ? -1 : 0) : a / b
        {
          LoadGpr(body, layout, RB(ins));
          body.push_back(kI32Eqz);
          LoadGpr(body, layout, RA(ins));
          body.push_back(kI32Const);
          PutI32(body, static_cast<int32_t>(0x80000000u));
          body.push_back(kI32Eq);
          LoadGpr(body, layout, RB(ins));
          body.push_back(kI32Const);
          PutI32(body, -1);
          body.push_back(kI32Eq);
          body.push_back(kI32And);
          body.push_back(kI32Or);
          body.push_back(kIf);
          body.push_back(kI32);
          LoadGpr(body, layout, RA(ins));
          body.push_back(kI32Const);
          PutI32(body, 31);
          body.push_back(kI32ShrS);
          body.push_back(kElse);
          LoadGpr(body, layout, RA(ins));
          LoadGpr(body, layout, RB(ins));
          body.push_back(kI32DivS);
          body.push_back(kEnd);
        }
        EndStoreGpr(body, layout, RS(ins));
        handled = true;
        break;
      }
      if (xo == 75 || xo == 11)  // mulhw, mulhwu: the high word of the product
      {
        BeginStoreGpr(body);
        LoadGpr(body, layout, RA(ins));
        body.push_back(xo == 75 ? kI64ExtendI32S : kI64ExtendI32U);
        LoadGpr(body, layout, RB(ins));
        body.push_back(xo == 75 ? kI64ExtendI32S : kI64ExtendI32U);
        body.push_back(kI64Mul);
        body.push_back(kI64Const);
        PutI32(body, 32);
        body.push_back(xo == 75 ? kI64ShrS : kI64ShrU);
        body.push_back(kI32WrapI64);
        EndStoreGpr(body, layout, RS(ins));
        handled = true;
        break;
      }

      // ── arithmetic shifts right, which set the carry ──
      //
      // CA is set when the value was negative and ones were shifted out
      // (Interpreter_Integer.cpp srawix/srawx), and for a count of 32 or
      // more the result is the sign fill and CA is simply the sign.
      if (xo == 824 && layout.xer_ca_offset)  // srawi rA, rS, SH
      {
        const uint32_t sh = SH(ins);
        LoadGpr(body, layout, RS(ins));
        body.push_back(kLocalSet);
        PutU32(body, 1);
        BeginStoreGpr(body);
        body.push_back(kLocalGet);
        PutU32(body, 1);
        if (sh)
        {
          body.push_back(kI32Const);
          PutI32(body, static_cast<int32_t>(sh));
          body.push_back(kI32ShrS);
        }
        EndStoreGpr(body, layout, RA(ins));
        body.push_back(kLocalGet);
        PutU32(body, 0);
        if (sh == 0)
        {
          body.push_back(kI32Const);
          PutI32(body, 0);
        }
        else
        {
          // (rS >> 31) & ((rS << (32 - sh)) != 0)
          body.push_back(kLocalGet);
          PutU32(body, 1);
          body.push_back(kI32Const);
          PutI32(body, 31);
          body.push_back(kI32ShrU);
          body.push_back(kLocalGet);
          PutU32(body, 1);
          body.push_back(kI32Const);
          PutI32(body, static_cast<int32_t>(32 - sh));
          body.push_back(kI32Shl);
          body.push_back(kI32Const);
          PutI32(body, 0);
          body.push_back(kI32Ne);
          body.push_back(kI32And);
        }
        EmitEndStoreCA(body, layout);
        used_locals = used_locals < 2 ? 2 : used_locals;
        handled = true;
        break;
      }
      if (xo == 792 && layout.xer_ca_offset)  // sraw rA, rS, rB
      {
        LoadGpr(body, layout, RS(ins));
        body.push_back(kLocalSet);
        PutU32(body, 1);
        LoadGpr(body, layout, RB(ins));
        body.push_back(kI32Const);
        PutI32(body, 63);
        body.push_back(kI32And);
        body.push_back(kLocalSet);
        PutU32(body, 2);
        // rA = rS >>s min(amount, 31): a count of 32 or more is the sign fill.
        BeginStoreGpr(body);
        body.push_back(kLocalGet);
        PutU32(body, 1);
        body.push_back(kLocalGet);
        PutU32(body, 2);
        body.push_back(kI32Const);
        PutI32(body, 31);
        body.push_back(kLocalGet);
        PutU32(body, 2);
        body.push_back(kI32Const);
        PutI32(body, 32);
        body.push_back(kI32LtU);
        body.push_back(kSelect);
        body.push_back(kI32ShrS);
        EndStoreGpr(body, layout, RA(ins));
        // CA = (rS >> 31) & (amount != 0) & ((amount >= 32) | ((rS << (32 - amount)) != 0))
        // -- wasm masks the shift count to five bits, which for amounts in
        // 1..31 is exactly 32 - amount.
        body.push_back(kLocalGet);
        PutU32(body, 0);
        body.push_back(kLocalGet);
        PutU32(body, 1);
        body.push_back(kI32Const);
        PutI32(body, 31);
        body.push_back(kI32ShrU);
        body.push_back(kLocalGet);
        PutU32(body, 2);
        body.push_back(kI32Const);
        PutI32(body, 0);
        body.push_back(kI32Ne);
        body.push_back(kI32And);
        body.push_back(kLocalGet);
        PutU32(body, 2);
        body.push_back(kI32Const);
        PutI32(body, 32);
        body.push_back(kI32GeU);
        body.push_back(kLocalGet);
        PutU32(body, 1);
        body.push_back(kI32Const);
        PutI32(body, 32);
        body.push_back(kLocalGet);
        PutU32(body, 2);
        body.push_back(kI32Sub);
        body.push_back(kI32Shl);
        body.push_back(kI32Const);
        PutI32(body, 0);
        body.push_back(kI32Ne);
        body.push_back(kI32Or);
        body.push_back(kI32And);
        EmitEndStoreCA(body, layout);
        used_locals = used_locals < 3 ? 3 : used_locals;
        handled = true;
        break;
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
        EmitBailBody(body, layout, this_pc, n);
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
        EmitBailBody(body, layout, this_pc, n);
        body.push_back(kEnd);

        // ── the OS's interrupt toggles ──
        //
        // OSDisableInterrupts is `mfmsr rX; rlwinm rY,rX,0,17,15; mtmsr rY`
        // and OSEnableInterrupts is `mfmsr rX; ori rY,rX,0x8000; mtmsr rY`.
        // Measured over the boot they are the two hottest blocks after the
        // idle loop, called 40 million times between them. When the
        // previous two instructions are exactly that, the value written is
        // provably the old MSR with only EE changed: no sensitive bit can
        // have flipped, so the general check below is replaced by the one
        // or two loads that can still matter. The PR test is skipped too --
        // the mfmsr two instructions ago already made it.
        if (layout.mtmsr_continue && layout.enable_linking && Opcode(prev2_ins) == 31 &&
            XO(prev2_ins) == 83 && !Rc(prev2_ins) && RS(prev_ins) == RS(prev2_ins) &&
            RA(prev_ins) == RS(ins) &&
            ((Opcode(prev_ins) == 21 && !Rc(prev_ins) && SH(prev_ins) == 0 &&
              MB(prev_ins) == 17 && ME(prev_ins) == 15) ||
             (Opcode(prev_ins) == 24 && UIMM(prev_ins) == 0x8000)))
        {
          const bool enabling = Opcode(prev_ins) == 24;
          // msr = gpr[rY]
          body.push_back(kLocalGet);
          PutU32(body, 0);
          LoadGpr(body, layout, RS(ins));
          body.push_back(kI32Store);
          PutU32(body, 2);
          PutU32(body, layout.msr_offset);
          // Leave if a page table update is waiting, or -- when enabling --
          // if an interrupt is pending behind EE.
          body.push_back(kLocalGet);
          PutU32(body, 0);
          body.push_back(kI32Load8U);
          PutU32(body, 0);
          PutU32(body, layout.pagetable_pending_offset);
          if (enabling)
          {
            body.push_back(kLocalGet);
            PutU32(body, 0);
            body.push_back(kI32Load);
            PutU32(body, 2);
            PutU32(body, layout.exceptions_offset);
            body.push_back(kI32Or);
          }
          body.push_back(kIf);
          body.push_back(kVoid);
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
          body.push_back(kI32Const);
          PutI32(body, static_cast<int32_t>((this_pc + 4) | 2u));
          body.push_back(kReturn);
          body.push_back(kEnd);
          handled = true;
          break;
        }

        if (layout.mtmsr_continue && layout.enable_linking)
        {
          // Keep the old value, write the new one, and leave only if the
          // write has a consequence this code cannot deliver itself. See
          // StateLayout::mtmsr_continue for the three cases.
          body.push_back(kLocalGet);
          PutU32(body, 0);
          body.push_back(kI32Load);
          PutU32(body, 2);
          PutU32(body, layout.msr_offset);
          body.push_back(kLocalSet);
          PutU32(body, 1);

          body.push_back(kLocalGet);
          PutU32(body, 0);
          LoadGpr(body, layout, RS(ins));
          body.push_back(kI32Store);
          PutU32(body, 2);
          PutU32(body, layout.msr_offset);

          // (old ^ new) & (PR | FP | IR | DR | FE0 | FE1) -- FP because a
          // block checks it once, before its first FP instruction
          body.push_back(kLocalGet);
          PutU32(body, 1);
          body.push_back(kLocalGet);
          PutU32(body, 0);
          body.push_back(kI32Load);
          PutU32(body, 2);
          PutU32(body, layout.msr_offset);
          body.push_back(kI32Xor);
          body.push_back(kI32Const);
          PutI32(body, (1 << 14) | (1 << 13) | (1 << 5) | (1 << 4) | (1 << 11) | (1 << 8));
          body.push_back(kI32And);
          // | (Exceptions * new.EE) -- an interrupt pending behind EE
          body.push_back(kLocalGet);
          PutU32(body, 0);
          body.push_back(kI32Load);
          PutU32(body, 2);
          PutU32(body, layout.exceptions_offset);
          body.push_back(kLocalGet);
          PutU32(body, 0);
          body.push_back(kI32Load);
          PutU32(body, 2);
          PutU32(body, layout.msr_offset);
          body.push_back(kI32Const);
          PutI32(body, 15);
          body.push_back(kI32ShrU);
          body.push_back(kI32Const);
          PutI32(body, 1);
          body.push_back(kI32And);
          body.push_back(kI32Mul);
          body.push_back(kI32Or);
          // | pagetable_update_pending
          body.push_back(kLocalGet);
          PutU32(body, 0);
          body.push_back(kI32Load8U);
          PutU32(body, 0);
          PutU32(body, layout.pagetable_pending_offset);
          body.push_back(kI32Or);
          body.push_back(kIf);
          body.push_back(kVoid);
          // downcount -= n + 1: this instruction ran.
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
          body.push_back(kI32Const);
          PutI32(body, static_cast<int32_t>((this_pc + 4) | 2u));
          body.push_back(kReturn);
          body.push_back(kEnd);
          used_locals = used_locals < 2 ? 2 : used_locals;
          handled = true;
          break;
        }

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
        PutI32(body, static_cast<int32_t>((this_pc + 4) | 2u));
        body.push_back(kReturn);
        add_succ(this_pc + 4);
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
      // LI is a signed 24-bit word displacement, stored shifted left by two.
      int32_t li = static_cast<int32_t>(ins & 0x03FFFFFC);
      if (li & 0x02000000)
        li -= 0x04000000;  // sign extend from 26 bits
      const bool absolute = (ins & 2) != 0;
      const bool link = (ins & 1) != 0;
      const uint32_t target = absolute ? static_cast<uint32_t>(li) : this_pc + li;
      if (link)
        EmitSetSpr(body, layout, layout.lr_index, this_pc + 4);
      handled = true;
      if (layout.trace_uncond)
      {
        // An unconditional transfer goes exactly one place, so following
        // it needs no prediction and no guard: carry on compiling there.
        // For a call that means inlining the callee, which then ends at
        // its own return -- an indirect branch this cannot follow.
        ++n;
        cur_pc = target;
        if (link)
          add_succ(this_pc + 4);  // the return site is still a block entry
        followed = true;
        break;
      }
      EmitStaticExit(body, layout, target, n + 1, 0, fixups, link ? 5 : 4);
      add_succ(target);
      // A call does not fall through, but its RETURN SITE is a block entry
      // and a hot one, so it is worth compiling in the same region.
      if (link)
        add_succ(this_pc + 4);
      ends_block = true;
      break;
    }

    case 16:  // bc / bca / bcl / bcla
    {
      const uint32_t bo = RS(ins);
      const uint32_t bi = RA(ins);
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
        EmitStaticExit(body, layout, target, n + 1, 0, fixups);
      }
      else
      {
        // Which edge to follow, and which to exit on.
        //
        // A block normally exits on TAKEN and carries on along the
        // not-taken edge, because that edge is simply the next address.
        // For a BACKWARD branch that is the wrong way round: a backward
        // conditional branch is a loop back edge, taken on every
        // iteration but the last, and exiting there costs one block entry
        // per iteration. 126.8 million of 261.5 million block entries
        // leave by a taken conditional branch.
        //
        // Following it instead unrolls the loop into the block: the test
        // is inverted so the exit is the not-taken edge, and compilation
        // continues at the target, which is code this block already
        // contains. Nothing is predicted and nothing is guessed -- both
        // edges still go exactly where they went.
        const bool follow_taken = layout.trace_backward && target < this_pc;

        // A busy-wait loop closes here: the branch goes back to the block's
        // own first instruction, tests the condition register and not CTR,
        // and nothing in the body could have changed the outcome. The
        // taken edge then idles instead of looping -- see
        // StateLayout::idle_flag_addr.
        const bool idle_exit = idle_ok && !link && !follow_taken && target == start_pc &&
                               (bo & 0x04) != 0 && (bo & 0x10) == 0 && n > 0;

        EmitBranchTaken(body, layout, bo, bi);
        if (follow_taken)
          body.push_back(kI32Eqz);  // exit on NOT taken instead
        body.push_back(kIf);
        body.push_back(kVoid);
        if (idle_exit)
        {
          // downcount -= n + 1, as any exit charges what ran ...
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
          // ... then tell the dispatcher the rest of the slice is idle ...
          body.push_back(kI32Const);
          PutI32(body, static_cast<int32_t>(layout.idle_flag_addr));
          body.push_back(kI32Const);
          PutI32(body, 1);
          body.push_back(kI32Store);
          PutU32(body, 2);
          PutU32(body, 0);
          // ... and come back here when time has moved on.
          body.push_back(kI32Const);
          PutI32(body, static_cast<int32_t>(start_pc));
          body.push_back(kReturn);
        }
        else
        {
          // bcl writes LR only when it branches, so the write belongs on
          // whichever edge is the taken one: inside the if normally, and
          // after it when the taken edge is the one being followed.
          if (link && !follow_taken)
            EmitSetSpr(body, layout, layout.lr_index, this_pc + 4);
          // 0 is the if, 1 the enclosing block
          EmitStaticExit(body, layout, follow_taken ? this_pc + 4 : target, n + 1, 1, fixups, 6);
        }
        body.push_back(kEnd);
        // Whatever this branch was, a loop closing AFTER it would contain a
        // branch, which the busy-wait rule forbids.
        idle_ok = false;

        if (follow_taken)
        {
          if (link)
            EmitSetSpr(body, layout, layout.lr_index, this_pc + 4);
          if (nsucc < 2)
            add_succ(this_pc + 4);
          handled = true;
          ++n;
          cur_pc = target;
          followed = true;
          break;
        }
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
      if (XO(ins) == 150)  // isync: nothing to do, as in the interpreter
      {
        handled = true;
        break;
      }
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
      EmitMemOp(body, layout, this_pc, n, RA(ins), 0, SIMM(ins), false, RS(ins),
                form.size, form.store, form.sign, form.update, ins);
      touches_memory = true;
      used_locals = used_locals < 4 ? 4 : used_locals;
      handled = true;
      break;
    }

    default:
      break;
    }

    if (!handled && layout.interp_hatch)
    {
      // Everything except the transfers of control this compiler owns.
      // mtmsr is inline above; a branch form it refused (bcctr that
      // decrements CTR) is fine here, since the helper reports a changed
      // PC and the block leaves with it.
      const bool transfer = (op == 16 || op == 18 ||
                             (op == 19 && (XO(ins) == 16 || XO(ins) == 528)) ||
                             (op == 31 && XO(ins) == 146));
      if (!transfer)
      {
        // The interpreter's handler does not check that floating point is
        // available -- SingleStepInner does that around it -- so the block
        // does, once, as for any FP instruction it translates itself.
        if (op == 4)
          need_pse(this_pc, n);
        else if ((op >= 48 && op <= 63) || op == 4)
          need_fp(this_pc, n);
        body.push_back(kLocalGet);
        PutU32(body, 0);
        body.push_back(kI32Const);
        PutI32(body, static_cast<int32_t>(ins));
        body.push_back(kI32Const);
        PutI32(body, static_cast<int32_t>(this_pc));
        body.push_back(kCall);
        PutU32(body, 0);
        body.push_back(kIf);
        body.push_back(kVoid);
        if (layout.enable_linking)
        {
          // The instruction ran; charge it with the rest of the block.
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
        // Leave with the PC the interpreter set. Unmarked: nothing is
        // left for the dispatcher to do.
        body.push_back(kLocalGet);
        PutU32(body, 0);
        body.push_back(kI32Load);
        PutU32(body, 2);
        PutU32(body, layout.pc_offset);
        body.push_back(kReturn);
        body.push_back(kEnd);
        touches_memory = true;
        handled = true;
        // An mtspr the interpreter ran may have written HID2, which the
        // paired-single checks above read once per block.
        if (op == 31 && XO(ins) == 467)
          pse_checked = lsqe_checked = false;
      }
    }
    if (!handled)
    {
      stopped = "unsupported instruction";
      break;
    }
    if (followed)
      continue;
    prev2_ins = prev_ins;
    prev_ins = ins;
    ++n;
    cur_pc = this_pc + 4;
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
      stopped = "branch";
      break;
    }
  }

  BlockResult r;
  r.instructions = n;
  r.end_pc = cur_pc;
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
    // Every fixup recorded so far is an offset into `body`, which is about
    // to be copied two bytes into `wrapped`. The patcher indexes the final
    // body, so they move with it. Getting this wrong would write a
    // function index over two bytes of real code and leave the call site
    // as nops -- wrong code that still validates, which is the worst
    // shape a bug can have here.
    for (DirectFixup& f : fixups)
      f.offset += 2;
    PutBytes(wrapped, body);
    // Fall-through case: continue at the instruction after the block,
    // having run all of it. A static target like any other, and the most
    // common one, so it gets a direct-call hole too.
    //
    // The fixup offsets recorded here are relative to `wrapped`, which is
    // about to become the body -- the same buffer the patcher indexes.
    if (layout.allow_direct)
    {
      EmitStaticExit(wrapped, layout, r.end_pc, n, 0, fixups, 7);
    }
    else
    {
      wrapped.push_back(kI32Const);
      PutI32(wrapped, static_cast<int32_t>(r.end_pc));
      wrapped.push_back(kLocalSet);
      PutU32(wrapped, 4);
      wrapped.push_back(kI32Const);
      PutI32(wrapped, static_cast<int32_t>(n));
      wrapped.push_back(kLocalSet);
      PutU32(wrapped, 7);
    }
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

  // The i64 locals start at 9 by construction -- 9 and 10 for most forms,
  // up to 14 for the paired multiply-adds -- so a block that uses them
  // declares all eight i32 locals whether or not it needs them.
  if (used_i64)
    used_locals = 8;
  r.locals = used_locals;
  r.locals_i64 = used_i64;
  r.fixups = std::move(fixups);
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
  if (layout.interp_hatch)
    mb.ImportFunction("env", "ppc_interp_op", {kI32, kI32, kI32}, {kI32});
  mb.SetFunction({kI32}, {kI32}, body, "f", used_locals, used_i64);
  r.module = mb.Build();
  return r;
}

// The sequential form every existing caller uses: `code` holds max_insts
// instructions starting at start_pc.
//
// Addresses outside that window fetch as zero, which does not decode and
// so ends the block. That is what bounds a followed branch to memory the
// caller actually provided.
inline BlockResult CompileBlock(const uint32_t* code, uint32_t start_pc, uint32_t max_insts,
                                const StateLayout& layout, bool emit_module = true)
{
  return CompileBlockFetch(
      [code, start_pc, max_insts](uint32_t pc) -> uint32_t {
        const uint32_t index = (pc - start_pc) / 4;
        return index < max_insts ? code[index] : 0u;
      },
      start_pc, max_insts, layout, emit_module);
}
}  // namespace ppc2wasm
