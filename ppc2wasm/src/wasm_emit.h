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

#include <cstdint>
#include <string>
#include <vector>

namespace ppc2wasm
{
using Bytes = std::vector<uint8_t>;

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
                   uint32_t i64_locals = 0)
  {
    m_params = params;
    m_results = results;
    m_funcs.clear();
    AddFunction(body, export_name, i32_locals, i64_locals);
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
  // 64-bit register values and byte swaps.
  uint32_t AddFunction(const Bytes& body, const std::string& export_name,
                       uint32_t i32_locals = 0, uint32_t i64_locals = 0)
  {
    m_funcs.push_back({body, export_name, i32_locals, i64_locals});
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

    // Type section: the block signature, then one type per imported
    // function.
    {
      Bytes p;
      PutU32(p, 1 + static_cast<uint32_t>(m_func_imports.size()));
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
      for (const FuncImport& fi : m_func_imports)
        put_type(fi.params, fi.results);
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
        PutU32(p, static_cast<uint32_t>(1 + i));  // its type
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

    // Code section.
    {
      Bytes p;
      PutU32(p, static_cast<uint32_t>(m_funcs.size()));
      for (const Func& f : m_funcs)
      {
        Bytes body;
        const uint32_t groups = (f.locals ? 1u : 0u) + (f.locals_i64 ? 1u : 0u);
        PutU32(body, groups);
        if (f.locals)
        {
          PutU32(body, f.locals);  // how many
          body.push_back(kI32);
        }
        if (f.locals_i64)
        {
          PutU32(body, f.locals_i64);
          body.push_back(kI64);
        }
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
    std::string debug_name;
  };

  struct FuncImport
  {
    std::string module, name;
    std::vector<uint8_t> params, results;
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
