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
  kFuncType = 0x60,

  kBlock = 0x02,
  kIf = 0x04,
  kElse = 0x05,
  kEnd = 0x0B,
  kBr = 0x0C,
  kReturn = 0x0F,
  kReturnCallIndirect = 0x13,
  kLocalTee = 0x22,
  kVoid = 0x40,  // block type: no result
  kLocalGet = 0x20,
  kLocalSet = 0x21,
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
};

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
                   const Bytes& body, const std::string& export_name, uint32_t i32_locals = 0)
  {
    m_params = params;
    m_results = results;
    m_funcs.clear();
    AddFunction(body, export_name, i32_locals);
  }

  // Add one more function to the module, returning its index.
  //
  // Several blocks in ONE module is how installation stops dominating a
  // run: every WebAssembly.Module costs about 22 us to compile and
  // instantiate almost regardless of its size, so the price is paid per
  // module and not per block. The index returned is also a wasm function
  // index, which is what a direct call between two blocks in the same
  // module needs.
  uint32_t AddFunction(const Bytes& body, const std::string& export_name,
                       uint32_t i32_locals = 0)
  {
    m_funcs.push_back({body, export_name, i32_locals});
    return static_cast<uint32_t>(m_funcs.size() - 1);
  }

  // Signature for a module whose functions are added with AddFunction.
  void SetSignature(const std::vector<uint8_t>& params, const std::vector<uint8_t>& results)
  {
    m_params = params;
    m_results = results;
  }

  uint32_t FunctionCount() const { return static_cast<uint32_t>(m_funcs.size()); }

  Bytes Build() const
  {
    Bytes out{0x00, 0x61, 0x73, 0x6D, 0x01, 0x00, 0x00, 0x00};

    // Type section: one signature.
    {
      Bytes p;
      PutU32(p, 1);
      p.push_back(kFuncType);
      PutU32(p, static_cast<uint32_t>(m_params.size()));
      for (uint8_t t : m_params)
        p.push_back(t);
      PutU32(p, static_cast<uint32_t>(m_results.size()));
      for (uint8_t t : m_results)
        p.push_back(t);
      PutSection(out, 1, p);
    }

    // Import section: linear memory, and the function table when the block
    // links onward.
    if (m_import_memory || m_import_table)
    {
      Bytes p;
      PutU32(p, (m_import_memory ? 1u : 0u) + (m_import_table ? 1u : 0u));
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
      PutSection(out, 2, p);
    }

    // Function section: every function uses type 0.
    //
    // There are no imported FUNCTIONS -- the imports above are a memory and
    // a table -- so defined function k is also wasm function index k, and a
    // direct call can name it without an offset.
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
        PutU32(p, static_cast<uint32_t>(i));
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
        if (f.locals)
        {
          PutU32(body, 1);       // one local-type group
          PutU32(body, f.locals);  // how many
          body.push_back(kI32);
        }
        else
        {
          PutU32(body, 0);
        }
        PutBytes(body, f.body);
        body.push_back(kEnd);

        PutU32(p, static_cast<uint32_t>(body.size()));
        PutBytes(p, body);
      }
      PutSection(out, 10, p);
    }

    return out;
  }

private:
  struct Func
  {
    Bytes body;
    std::string export_name;
    uint32_t locals = 0;
  };

  bool m_import_memory = false;
  bool m_import_table = false;
  uint32_t m_mem_initial = 0, m_mem_max = 0;
  bool m_mem_shared = false;
  std::vector<uint8_t> m_params, m_results;
  std::vector<Func> m_funcs;
};
}  // namespace ppc2wasm
