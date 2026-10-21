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
  kReturn = 0x0F,
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
  void ImportMemory(uint32_t initial_pages, uint32_t max_pages, bool shared)
  {
    m_import_memory = true;
    m_mem_initial = initial_pages;
    m_mem_max = max_pages;
    m_mem_shared = shared;
  }

  // params/results describe the single exported function.
  // i32_locals are scratch slots ABOVE the parameters: with one parameter,
  // local 0 is that parameter and the first scratch local is 1. Address
  // computations need somewhere to keep the effective address, since wasm's
  // stack cannot be indexed twice without recomputing.
  void SetFunction(const std::vector<uint8_t>& params, const std::vector<uint8_t>& results,
                   const Bytes& body, const std::string& export_name, uint32_t i32_locals = 0)
  {
    m_params = params;
    m_results = results;
    m_body = body;
    m_export = export_name;
    m_locals = i32_locals;
  }

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

    // Import section: linear memory, when the block touches guest state.
    if (m_import_memory)
    {
      Bytes p;
      PutU32(p, 1);
      PutName(p, "env");
      PutName(p, "memory");
      p.push_back(0x02);  // memory
      // limits flags: bit0 = has max, bit1 = shared
      p.push_back(static_cast<uint8_t>((m_mem_shared ? 0x02 : 0x00) | 0x01));
      PutU32(p, m_mem_initial);
      PutU32(p, m_mem_max);
      PutSection(out, 2, p);
    }

    // Function section: our one function uses type 0.
    {
      Bytes p;
      PutU32(p, 1);
      PutU32(p, 0);
      PutSection(out, 3, p);
    }

    // Export section.
    {
      Bytes p;
      PutU32(p, 1);
      PutName(p, m_export);
      p.push_back(0x00);  // func
      PutU32(p, 0);
      PutSection(out, 7, p);
    }

    // Code section: no locals beyond parameters.
    {
      Bytes body;
      if (m_locals)
      {
        PutU32(body, 1);        // one local-type group
        PutU32(body, m_locals); // how many
        body.push_back(kI32);
      }
      else
      {
        PutU32(body, 0);
      }
      PutBytes(body, m_body);
      body.push_back(kEnd);

      Bytes p;
      PutU32(p, 1);
      PutU32(p, static_cast<uint32_t>(body.size()));
      PutBytes(p, body);
      PutSection(out, 10, p);
    }

    return out;
  }

private:
  bool m_import_memory = false;
  uint32_t m_mem_initial = 0, m_mem_max = 0;
  bool m_mem_shared = false;
  std::vector<uint8_t> m_params, m_results;
  Bytes m_body;
  std::string m_export = "f";
  uint32_t m_locals = 0;
};
}  // namespace ppc2wasm
