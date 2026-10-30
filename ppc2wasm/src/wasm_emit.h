// A minimal WebAssembly module encoder.
//
// This exists because of a constraint that shapes the whole recompiler:
// WebAssembly has no writable-executable memory, so there is no way to emit
// machine code into a buffer and jump to it. What a browser DOES allow is
// building a new wasm MODULE at runtime, compiling it, and installing its
// functions into the indirect function table the running program already
// dispatches through.
//
// So "JIT" here means: emit wasm bytecode, hand it to WebAssembly.Module,
// install the exports in the table, and call them with call_indirect. That is
// the shape Spike A measured at 863.9 MIPS in Chrome.
//
// Only the subset needed to emit straight-line integer blocks is implemented.
// The encoder is deliberately dumb -- no optimisation, no validation beyond
// what the browser will do anyway, because a browser rejecting a malformed
// module is a far better error than a subtly wrong one this code "fixed".
#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace ppc2wasm
{
using Bytes = std::vector<uint8_t>;

// A branch hint for an `if` or `br_if` at `offset` in a function body:
// whether its condition is likely true. See ModuleBuilder::AddFunction.
struct BranchHint
{
  uint32_t offset;
  bool likely;
};

// LEB128, the variable-length integer encoding wasm uses everywhere.
inline void PutU32(Bytes& out, uint32_t v)
{
  do
  {
    uint8_t b = v & 0x7F;
    v >>= 7;
    if (v)
      b |= 0x80;
    out.push_back(b);
  } while (v);
}

// Signed LEB128. Different from the unsigned form in its termination rule, and
// getting it wrong produces a module that validates but computes nonsense --
// i32.const is signed.
inline void PutI32(Bytes& out, int32_t v)
{
  bool more = true;
  while (more)
  {
    uint8_t b = v & 0x7F;
    v >>= 7;
    if ((v == 0 && !(b & 0x40)) || (v == -1 && (b & 0x40)))
      more = false;
    else
      b |= 0x80;
    out.push_back(b);
  }
}

inline void PutBytes(Bytes& out, const Bytes& in)
{
  out.insert(out.end(), in.begin(), in.end());
}

inline void PutName(Bytes& out, const std::string& s)
{
  PutU32(out, static_cast<uint32_t>(s.size()));
  out.insert(out.end(), s.begin(), s.end());
}

// A section is id, byte length, then payload. The length is only known after
// the payload is built, which is why every section is assembled separately
// and spliced in.
inline void PutSection(Bytes& out, uint8_t id, const Bytes& payload)
{
  out.push_back(id);
  PutU32(out, static_cast<uint32_t>(payload.size()));
  PutBytes(out, payload);
}

// ── value types and opcodes ──
enum : uint8_t
{
  kI32 = 0x7F,
  kI64 = 0x7E,
  kF64 = 0x7C,
  kV128 = 0x7B,
  kFuncType = 0x60,

  kBlock = 0x02,
  kIf = 0x04,
  kElse = 0x05,
  kEnd = 0x0B,
  kBr = 0x0C,
  kReturn = 0x0F,
  kNop = 0x01,
  kDrop = 0x1A,
  // A direct tail call, naming a function index rather than looking one up
  // in a table. Only reachable between functions in the SAME module, which
  // is what region compilation arranges.
  kReturnCall = 0x12,
  kReturnCallIndirect = 0x13,
  kLocalTee = 0x22,
  kVoid = 0x40,  // block type: no result
  kLocalGet = 0x20,
  kLocalSet = 0x21,
  kCall = 0x10,
  kI32Const = 0x41,
  kI32Load = 0x28,
  kI32Load8U = 0x2D,
  kI32Store = 0x36,
  kI32Store8 = 0x3A,
  kI32Add = 0x6A,
  kI32Sub = 0x6B,
  kI32Mul = 0x6C,
  kI32And = 0x71,
  kI32Or = 0x72,
  kI32Xor = 0x73,
  kI32Shl = 0x74,
  kI32ShrS = 0x75,
  kI32ShrU = 0x76,
  kI32Rotl = 0x77,
  kI32Eq = 0x46,
  kI32Ne = 0x47,
  kI32LtS = 0x48,
  kI32LtU = 0x49,
  kI32GtS = 0x4A,
  kI32GtU = 0x4B,
  kI32LeS = 0x4C,
  kI32Eqz = 0x45,
  kI32Clz = 0x67,

  kI64Load = 0x29,
  kI64Store = 0x37,
  kI32Rotr = 0x78,

  kSelect = 0x1B,
  kBrIf = 0x0D,
  kLoop = 0x03,
  kI32GeU = 0x4F,
  kI32LeU = 0x4D,
  kI32GeS = 0x4E,
  kI32DivS = 0x6D,
  kI32DivU = 0x6E,
  kI64Const = 0x42,
  kI64Mul = 0x7E,
  kI64ShrS = 0x87,
  kI64ShrU = 0x88,
  kI64ExtendI32S = 0xAC,
  kI64ExtendI32U = 0xAD,
  kI32WrapI64 = 0xA7,
  // Prefix byte for the bulk-memory instructions; memory.fill is 0x0B.
  kPrefixFC = 0xFC,
  kMemoryFill = 0x0B,

  kI64Eqz = 0x50,
  kI64Eq = 0x51,
  kI64Ne = 0x52,
  kI64LtU = 0x54,
  kI64Add = 0x7C,
  kI64Sub = 0x7D,
  kI64DivU = 0x80,
  kI64And = 0x83,
  kI64Or = 0x84,
  kI64Xor = 0x85,
  kI64Shl = 0x86,
  kI64Rotl = 0x89,
  kI64Load32U = 0x35,
  kF64Load = 0x2B,
  kF64Store = 0x39,
  kF64Const = 0x44,
  kF64Ge = 0x66,
  kI64GtU = 0x56,
  kF64Add = 0xA0,
  kF64Sub = 0xA1,
  kF64Mul = 0xA2,
  kF64Div = 0xA3,
  kF32DemoteF64 = 0xB6,
  kF64PromoteF32 = 0xBB,
  kI32ReinterpretF32 = 0xBC,
  kI64ReinterpretF64 = 0xBD,
  kF32ReinterpretI32 = 0xBE,
  kF64ReinterpretI64 = 0xBF,
  // For the quantized paired-single forms and the gather pipe.
  kI32Load8S = 0x2C,
  kI32Load16S = 0x2E,
  kI32Load16U = 0x2F,
  kI32Store16 = 0x3B,
  kF32Const = 0x43,
  kF32Mul = 0x94,
  kF32Min = 0x96,
  kF32Max = 0x97,
  kF32ConvertI32S = 0xB2,
  kI32TruncSatF32S = 0x00,  // after kPrefixFC: saturating, NaN -> 0
  // For the float compares and the condition-register moves.
  kF64Eq = 0x61,
  kF64Lt = 0x63,
  kF64Gt = 0x64,
  kI64GtS = 0x55,
};

// ── 128-bit SIMD ──
//
// The prefix byte, then the operation as a LEB128 u32 (PutU32), so the
// values above 0x7F take two bytes. Loads and stores take a memarg like
// the scalar ones; the lane forms add a lane index byte after it.
constexpr uint8_t kPrefixFD = 0xFD;
enum : uint32_t
{
  kV128Load = 0x00,
  kV128Load64Splat = 0x0A,
  kV128Store = 0x0B,
  kV128Const = 0x0C,
  kI8x16Shuffle = 0x0D,
  kI64x2Splat = 0x12,
  kF64x2Splat = 0x14,
  kI64x2ExtractLane = 0x1D,
  kF64x2ExtractLane = 0x21,
  kF64x2ReplaceLane = 0x22,
  kI32x4Eq = 0x37,
  kF64x2Eq = 0x47,
  kF64x2Ne = 0x48,
  kF64x2Gt = 0x4A,
  kV128Not = 0x4D,
  kV128And = 0x4E,
  kV128AndNot = 0x4F,
  kV128Or = 0x50,
  kV128Xor = 0x51,
  kV128Bitselect = 0x52,
  kV128AnyTrue = 0x53,
  kV128Store8Lane = 0x58,
  kV128Store16Lane = 0x59,
  kV128Store32Lane = 0x5A,
  kV128Store64Lane = 0x5B,
  kV128Load32Zero = 0x5C,
  kV128Load64Zero = 0x5D,
  kF32x4DemoteF64x2Zero = 0x5E,
  kF64x2PromoteLowF32x4 = 0x5F,
  kI8x16NarrowI16x8S = 0x65,
  kI8x16NarrowI16x8U = 0x66,
  kI16x8NarrowI32x4S = 0x85,
  kI16x8NarrowI32x4U = 0x86,
  kI16x8ExtendLowI8x16S = 0x87,
  kI16x8ExtendLowI8x16U = 0x89,
  kI32x4ExtendLowI16x8S = 0xA7,
  kI32x4ExtendLowI16x8U = 0xA9,
  kI64x2ShrU = 0xCD,
  kI64x2Add = 0xCE,
  kI64x2Sub = 0xD1,
  kI64x2Eq = 0xD6,
  kI64x2LtS = 0xD8,
  kI64x2GtS = 0xD9,
  kF32x4Mul = 0xE6,
  kF32x4Min = 0xE8,
  kF32x4Max = 0xE9,
  kF64x2Add = 0xF0,
  kF64x2Sub = 0xF1,
  kF64x2Mul = 0xF2,
  kF64x2Div = 0xF3,
  kI32x4TruncSatF32x4S = 0xF8,
  kF32x4ConvertI32x4S = 0xFA,
};

// Signed LEB128 for i64.const.
inline void PutI64(Bytes& out, int64_t v)
{
  bool more = true;
  while (more)
  {
    uint8_t b = v & 0x7F;
    v >>= 7;
    if ((v == 0 && !(b & 0x40)) || (v == -1 && (b & 0x40)))
      more = false;
    else
      b |= 0x80;
    out.push_back(b);
  }
}

// An f64.const immediate: the IEEE bits, little-endian, not LEB128.
inline void PutF64(Bytes& out, double v)
{
  uint64_t bits;
  std::memcpy(&bits, &v, sizeof(bits));
  for (int i = 0; i < 8; ++i)
    out.push_back(static_cast<uint8_t>(bits >> (8 * i)));
}

// Builds a module exporting one function, optionally importing linear memory
// from "env" so the emitted code can reach guest state.
//
// The memory import must match the host's memory EXACTLY -- shared flag,
// initial pages, maximum pages -- or instantiation fails with a link error.
// Under -pthread the Emscripten heap is shared, so `shared` is not optional.
class ModuleBuilder
{
public:
  // Import the host's indirect function table, so emitted code can
  // return_call_indirect into another compiled block. Without this a block
  // can only return to the dispatcher.
  void ImportTable() { m_import_table = true; }

  void ImportMemory(uint32_t initial_pages, uint32_t max_pages, bool shared)
  {
    m_import_memory = true;
    m_mem_initial = initial_pages;
    m_mem_max = max_pages;
    m_mem_shared = shared;
  }

  // Import a function from the host. Imported functions take the LOW
  // function indices, in the order they are added, and every defined
  // function shifts up by ImportCount(): a direct call between blocks must
  // add that offset, or it is valid wasm that calls the wrong function.
  // Each import gets its own type, after type 0 (the block signature).
  uint32_t ImportFunction(const std::string& module, const std::string& name,
                          const std::vector<uint8_t>& params, const std::vector<uint8_t>& results)
  {
    m_func_imports.push_back({module, name, params, results});
    return static_cast<uint32_t>(m_func_imports.size() - 1);
  }

  // Import another module's BLOCK under "env": a function of the block
  // signature (type 0), so that a direct tail call to it is a `return_call`
  // of an import -- V8 makes that a load of the callee's entry and a jump,
  // where a call through the function table costs the table's bounds and
  // signature checks and the lookup before them. The caller binds the
  // name to the earlier block's exported function when it instantiates.
  uint32_t ImportBlock(const std::string& name)
  {
    m_func_imports.push_back({"env", name, m_params, m_results, true});
    return static_cast<uint32_t>(m_func_imports.size() - 1);
  }
  uint32_t ImportCount() const { return static_cast<uint32_t>(m_func_imports.size()); }

  // params/results describe the signature EVERY function in the module
  // shares. A compiled block is always (i32 state) -> i32, so one type
  // serves all of them and a direct call between two of them needs no
  // second signature.
  //
  // i32_locals are scratch slots ABOVE the parameters: with one parameter,
  // local 0 is that parameter and the first scratch local is 1. Address
  // computations need somewhere to keep the effective address, since wasm's
  // stack cannot be indexed twice without recomputing.
  void SetFunction(const std::vector<uint8_t>& params, const std::vector<uint8_t>& results,
                   const Bytes& body, const std::string& export_name, uint32_t i32_locals = 0,
                   uint32_t i64_locals = 0, uint32_t v128_locals = 0,
                   uint32_t tail_i32_locals = 0, std::vector<BranchHint> hints = {})
  {
    m_params = params;
    m_results = results;
    m_funcs.clear();
    AddFunction(body, export_name, i32_locals, i64_locals, v128_locals, tail_i32_locals,
                std::move(hints));
  }

  // Add one more function to the module, returning its index.
  //
  // Several blocks in ONE module is how installation stops dominating a
  // run: every WebAssembly.Module costs about 22 us to compile and
  // instantiate almost regardless of its size, so the price is paid per
  // module and not per block. The index returned is also a wasm function
  // index, which is what a direct call between two blocks in the same
  // module needs.
  // i64_locals follow the i32 locals: with one parameter and N i32 locals,
  // the first i64 local is index N + 1. Floating-point blocks use them for
  // 64-bit register values and byte swaps. v128_locals follow those, for
  // paired singles held as vectors.
  // tail_i32_locals are i32 locals declared AFTER the v128 ones, so their
  // indices start at 1 + i32 + i64 + v128: the block compiler's access
  // groups keep a host base in each (BlockResult::locals_tail), and a
  // fourth run is what lets them exist without moving the fixed slots
  // the floating-point and vector forms index by number.
  //
  // `hints` are the body's branch hints (BranchHint): the offset of an
  // `if` or `br_if` opcode within `body`, and whether its condition is
  // likely true. Build writes them to the "metadata.code.branch_hint"
  // custom section with the locals declaration's length added, as the
  // proposal counts offsets from there.
  uint32_t AddFunction(const Bytes& body, const std::string& export_name,
                       uint32_t i32_locals = 0, uint32_t i64_locals = 0,
                       uint32_t v128_locals = 0, uint32_t tail_i32_locals = 0,
                       std::vector<BranchHint> hints = {})
  {
    m_funcs.push_back({body, export_name, i32_locals, i64_locals, v128_locals, tail_i32_locals,
                       std::move(hints)});
    return static_cast<uint32_t>(m_funcs.size() - 1);
  }

  // Signature for a module whose functions are added with AddFunction.
  void SetSignature(const std::vector<uint8_t>& params, const std::vector<uint8_t>& results)
  {
    m_params = params;
    m_results = results;
  }

  uint32_t FunctionCount() const { return static_cast<uint32_t>(m_funcs.size()); }

  // Give function `index` a name in the module's custom "name" section.
  // Engines show it in profiles and stack traces; it has no other effect.
  // Costs the bytes of the name per function, so it is opt-in.
  void SetFunctionName(uint32_t index, const std::string& name)
  {
    if (index < m_funcs.size())
      m_funcs[index].debug_name = name;
  }

  Bytes Build() const
  {
    Bytes out{0x00, 0x61, 0x73, 0x6D, 0x01, 0x00, 0x00, 0x00};

    // Type section: the block signature, then one type per imported host
    // function; an imported block uses type 0.
    std::vector<uint32_t> import_type(m_func_imports.size(), 0);
    {
      Bytes p;
      uint32_t host_imports = 0;
      for (const FuncImport& fi : m_func_imports)
        host_imports += fi.block_type ? 0 : 1;
      PutU32(p, 1 + host_imports);
      auto put_type = [&](const std::vector<uint8_t>& params, const std::vector<uint8_t>& results) {
        p.push_back(kFuncType);
        PutU32(p, static_cast<uint32_t>(params.size()));
        for (uint8_t t : params)
          p.push_back(t);
        PutU32(p, static_cast<uint32_t>(results.size()));
        for (uint8_t t : results)
          p.push_back(t);
      };
      put_type(m_params, m_results);
      uint32_t next = 1;
      for (size_t i = 0; i < m_func_imports.size(); ++i)
      {
        if (m_func_imports[i].block_type)
          continue;
        put_type(m_func_imports[i].params, m_func_imports[i].results);
        import_type[i] = next++;
      }
      PutSection(out, 1, p);
    }

    // Import section: linear memory, the function table when the block
    // links onward, and any host functions.
    if (m_import_memory || m_import_table || !m_func_imports.empty())
    {
      Bytes p;
      PutU32(p, (m_import_memory ? 1u : 0u) + (m_import_table ? 1u : 0u) +
                    static_cast<uint32_t>(m_func_imports.size()));
      if (m_import_memory)
      {
        PutName(p, "env");
        PutName(p, "memory");
        p.push_back(0x02);  // memory
        // limits flags: bit0 = has max, bit1 = shared
        p.push_back(static_cast<uint8_t>((m_mem_shared ? 0x02 : 0x00) | 0x01));
        PutU32(p, m_mem_initial);
        PutU32(p, m_mem_max);
      }
      if (m_import_table)
      {
        PutName(p, "env");
        PutName(p, "__indirect_function_table");
        p.push_back(0x01);  // table
        p.push_back(0x70);  // funcref
        p.push_back(0x00);  // limits: minimum only
        PutU32(p, 0);
      }
      for (size_t i = 0; i < m_func_imports.size(); ++i)
      {
        PutName(p, m_func_imports[i].module);
        PutName(p, m_func_imports[i].name);
        p.push_back(0x00);  // func
        PutU32(p, import_type[i]);
      }
      PutSection(out, 2, p);
    }

    // Function section: every function uses type 0.
    //
    // Defined function k is wasm function index ImportCount() + k.
    {
      Bytes p;
      PutU32(p, static_cast<uint32_t>(m_funcs.size()));
      for (size_t i = 0; i < m_funcs.size(); ++i)
        PutU32(p, 0);
      PutSection(out, 3, p);
    }

    // Export section.
    {
      Bytes p;
      PutU32(p, static_cast<uint32_t>(m_funcs.size()));
      for (size_t i = 0; i < m_funcs.size(); ++i)
      {
        PutName(p, m_funcs[i].export_name);
        p.push_back(0x00);  // func
        PutU32(p, static_cast<uint32_t>(m_func_imports.size() + i));
      }
      PutSection(out, 7, p);
    }

    // The locals declaration a function body starts with: the runs of
    // i32, i64, v128 and trailing i32 locals.
    auto locals_decl = [](const Func& f) {
      Bytes d;
      const uint32_t groups = (f.locals ? 1u : 0u) + (f.locals_i64 ? 1u : 0u) +
                              (f.locals_v128 ? 1u : 0u) + (f.locals_tail ? 1u : 0u);
      PutU32(d, groups);
      if (f.locals)
      {
        PutU32(d, f.locals);  // how many
        d.push_back(kI32);
      }
      if (f.locals_i64)
      {
        PutU32(d, f.locals_i64);
        d.push_back(kI64);
      }
      if (f.locals_v128)
      {
        PutU32(d, f.locals_v128);
        d.push_back(kV128);
      }
      if (f.locals_tail)
      {
        PutU32(d, f.locals_tail);
        d.push_back(kI32);
      }
      return d;
    };

    // Branch hints (the branch-hinting proposal, decoded by V8 without a
    // flag): a custom section "metadata.code.branch_hint" that must come
    // before the code section. Per function with hints, in index order:
    // the function index, then each hint as the opcode's byte offset from
    // the start of the locals declaration, a size of 1, and 0 (likely
    // false) or 1 (likely true). Offsets must increase strictly, so they
    // are sorted and a repeated offset keeps its first hint. V8 ignores a
    // malformed section silently; harness/wasm-core/jit-asm.mjs shows
    // whether the cold code moved.
    {
      Bytes p;
      uint32_t count = 0;
      for (size_t i = 0; i < m_funcs.size(); ++i)
      {
        const Func& f = m_funcs[i];
        if (f.hints.empty())
          continue;
        std::vector<BranchHint> hints = f.hints;
        std::sort(hints.begin(), hints.end(),
                  [](const BranchHint& a, const BranchHint& b) { return a.offset < b.offset; });
        Bytes h;
        uint32_t n = 0;
        const uint32_t base = static_cast<uint32_t>(locals_decl(f).size());
        for (size_t k = 0; k < hints.size(); ++k)
        {
          if (k && hints[k].offset == hints[k - 1].offset)
            continue;
          PutU32(h, base + hints[k].offset);
          PutU32(h, 1);
          h.push_back(hints[k].likely ? 1 : 0);
          ++n;
        }
        PutU32(p, static_cast<uint32_t>(m_func_imports.size() + i));
        PutU32(p, n);
        PutBytes(p, h);
        ++count;
      }
      if (count)
      {
        Bytes s;
        PutName(s, "metadata.code.branch_hint");
        PutU32(s, count);
        PutBytes(s, p);
        PutSection(out, 0, s);
      }
    }

    // Code section.
    {
      Bytes p;
      PutU32(p, static_cast<uint32_t>(m_funcs.size()));
      for (const Func& f : m_funcs)
      {
        Bytes body = locals_decl(f);
        PutBytes(body, f.body);
        body.push_back(kEnd);

        PutU32(p, static_cast<uint32_t>(body.size()));
        PutBytes(p, body);
      }
      PutSection(out, 10, p);
    }

    // Name section, only when some function has a name. A custom section:
    // id 0, the name "name", then subsection 1 (function names) holding a
    // vector of (index, name) pairs in index order.
    bool any_named = false;
    for (const Func& f : m_funcs)
      any_named |= !f.debug_name.empty();
    if (any_named)
    {
      Bytes names;
      uint32_t count = 0;
      for (uint32_t i = 0; i < m_funcs.size(); ++i)
      {
        if (m_funcs[i].debug_name.empty())
          continue;
        PutU32(names, static_cast<uint32_t>(m_func_imports.size()) + i);
        PutName(names, m_funcs[i].debug_name);
        ++count;
      }
      Bytes sub;
      PutU32(sub, count);
      PutBytes(sub, names);
      Bytes p;
      PutName(p, "name");
      p.push_back(1);  // subsection: function names
      PutU32(p, static_cast<uint32_t>(sub.size()));
      PutBytes(p, sub);
      PutSection(out, 0, p);
    }

    return out;
  }

private:
  struct Func
  {
    Bytes body;
    std::string export_name;
    uint32_t locals = 0;
    uint32_t locals_i64 = 0;
    uint32_t locals_v128 = 0;
    uint32_t locals_tail = 0;
    std::vector<BranchHint> hints;
    std::string debug_name;
  };

  struct FuncImport
  {
    std::string module, name;
    std::vector<uint8_t> params, results;
    bool block_type = false;  // typed as the block signature, type 0
  };
  std::vector<FuncImport> m_func_imports;
  bool m_import_memory = false;
  bool m_import_table = false;
  uint32_t m_mem_initial = 0, m_mem_max = 0;
  bool m_mem_shared = false;
  std::vector<uint8_t> m_params, m_results;
  std::vector<Func> m_funcs;
};
}  // namespace ppc2wasm
