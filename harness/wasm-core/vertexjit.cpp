// A wasm vertex-loader JIT: one wasm function per vertex format.
//
// Dolphin converts the game's vertices (big-endian, indexed or direct, 8- or
// 16-bit fixed point or float) into the backend's native layout with a JIT
// loader on x64 and ARM64, and with the generic VertexLoader everywhere else
// -- a function-pointer call per attribute per vertex. Under Emscripten that
// generic loader was most of the GPU thread's FIFO drain
// (docs/PHASE3-WEBGPU.md M4). This is the missing native loader: for each
// VertexLoaderUID it emits a wasm module whose one function converts `count`
// vertices, laid out as VertexLoaderARM64.cpp lays out its code, and installs
// it in the function table the way bootgame.cpp's JitInstall installs blocks.
//
// The output is the GENERIC loader's, bit for bit -- including where that
// differs from the ARM64 loader (the position scale applies with ByteDequant
// clear; texture matrix indices are masked to six bits) -- because the
// generic loader is what has been drawing every frame so far.
// --vertex-loader=compare runs both on every call and counts the
// differences.
//
// It plugs in through harness_create_vertex_loader
// (dolphin-web/patches/0020-vertex-loader-hook.patch).
//
// Threads: a function-table entry added on one pthread does not exist on
// another (each worker has its own table), so a loader installs itself on
// the first thread that runs it and any other thread gets the generic loader.
// In dual core every loader runs on the GPU thread.

#include <emscripten.h>
#include <pthread.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <memory>
#include <tuple>
#include <vector>

#include "Common/CommonTypes.h"
#include "VideoCommon/CPMemory.h"
#include "VideoCommon/VertexLoader.h"
#include "VideoCommon/VertexLoaderBase.h"
#include "VideoCommon/VertexLoaderManager.h"

#include "../../ppc2wasm/src/wasm_emit.h"

// 0 off (the generic loader), 1 the JIT (the default), 2 both, compared.
static int g_vtxjit_mode = 1;
static std::atomic<u32> g_vtxjit_compiled{0}, g_vtxjit_unsupported{0}, g_vtxjit_failed{0},
    g_vtxjit_foreign{0};
static std::atomic<u64> g_vtxjit_bytes{0}, g_vtxjit_compare_runs{0},
    g_vtxjit_compare_mismatches{0};
static std::atomic<double> g_vtxjit_install_ms{0};

void VertexJitSetMode(int mode)
{
  g_vtxjit_mode = mode;
}

EM_JS(int, VtxJitInstall, (const uint8_t* ptr, int len), {
  try {
    const mod = new WebAssembly.Module(HEAPU8.slice(ptr, ptr + len));
    const inst = new WebAssembly.Instance(mod, {env : {memory : wasmMemory}});
    const idx = wasmTable.length;
    wasmTable.grow(1);
    wasmTable.set(idx, inst.exports.f);
    return idx;
  } catch (e) {
    out('  vtxjit install failed: ' + e);
    return -1;
  }
});
EM_JS(int, VtxJitMemoryPages, (), { return wasmMemory.buffer.byteLength >>> 16; });
EM_JS(double, VtxJitNow, (), { return performance.now(); });

namespace
{
using namespace ppc2wasm;

// Opcodes wasm_emit.h does not name.
enum : uint8_t
{
  kF32Load = 0x2A,
  kF32Store = 0x38,
  kF32ConvertI32U = 0xB3,
  kI32Extend16S = 0xC1,
};

// The function's locals: the three parameters, then scratch.
enum : uint32_t
{
  L_SRC = 0,
  L_DST = 1,
  L_COUNT = 2,
  L_REMAINING = 3,
  L_SKIPPED = 4,
  L_SKIP = 5,
  L_ADDR = 6,
  L_T0 = 7,
  L_T1 = 8,
  // Then two per array the format indexes: its base and its stride.
  L_ARRAYS = 9,
};

class Emitter
{
public:
  Bytes b;

  void Op(uint8_t o) { b.push_back(o); }
  void Get(uint32_t l)
  {
    Op(kLocalGet);
    PutU32(b, l);
  }
  void Set(uint32_t l)
  {
    Op(kLocalSet);
    PutU32(b, l);
  }
  void Tee(uint32_t l)
  {
    Op(kLocalTee);
    PutU32(b, l);
  }
  void Const(u32 v)
  {
    Op(kI32Const);
    PutI32(b, static_cast<int32_t>(v));
  }
  // A load or store with its memarg: alignment 0 (the vertex data is
  // unaligned) and a constant offset.
  void Mem(uint8_t o, u32 offset)
  {
    Op(o);
    PutU32(b, 0);
    PutU32(b, offset);
  }
  void F32(float v)
  {
    Op(kF32Const);
    u32 bits;
    std::memcpy(&bits, &v, 4);
    for (int i = 0; i < 4; ++i)
      b.push_back(static_cast<uint8_t>(bits >> (8 * i)));
  }
  // Two bytes, big-endian, at the address on the stack plus `offset`.
  void LoadBE16(u32 offset)
  {
    Mem(kI32Load16U, offset);
    Tee(L_T0);
    Const(8);
    Op(kI32ShrU);
    Get(L_T0);
    Const(0xFF);
    Op(kI32And);
    Const(8);
    Op(kI32Shl);
    Op(kI32Or);
  }
  void LoadBE32(u32 offset)
  {
    Mem(kI32Load, offset);
    Tee(L_T0);
    Const(0x00FF00FF);
    Op(kI32And);
    Const(24);
    Op(kI32Rotl);
    Get(L_T0);
    Const(0xFF00FF00);
    Op(kI32And);
    Const(8);
    Op(kI32Rotl);
    Op(kI32Or);
  }
};

class VertexLoaderWasm final : public VertexLoaderBase
{
public:
  VertexLoaderWasm(const TVtxDesc& vtx_desc, const VAT& vtx_attr,
                   std::unique_ptr<VertexLoader> generic)
      : VertexLoaderBase(vtx_desc, vtx_attr), m_generic(std::move(generic))
  {
    m_native_vtx_decl = m_generic->m_native_vtx_decl;
  }

  bool Generate();

  int RunVertices(const u8* src, u8* dst, int count) override
  {
    m_numLoadedVertices += count;
    if (!m_install_tried)
      Install();
    if (m_fn == nullptr || !pthread_equal(pthread_self(), m_thread))
    {
      if (m_fn != nullptr)
        ++g_vtxjit_foreign;
      return m_generic->RunVertices(src, dst, count);
    }
    if (g_vtxjit_mode == 2)
      return RunCompared(src, dst, count);
    return m_fn(src, dst, count);
  }

private:
  using Fn = int (*)(const u8*, u8*, int);

  void Install()
  {
    m_install_tried = true;
    const double t0 = VtxJitNow();
    const int idx = VtxJitInstall(m_module.data(), static_cast<int>(m_module.size()));
    g_vtxjit_install_ms = g_vtxjit_install_ms + (VtxJitNow() - t0);
    if (idx < 0)
    {
      ++g_vtxjit_failed;
      return;
    }
    m_fn = reinterpret_cast<Fn>(static_cast<uintptr_t>(idx));
    m_thread = pthread_self();
    ++g_vtxjit_compiled;
    g_vtxjit_bytes += m_module.size();
    m_module = {};
  }

  // Both loaders, from the same caches; the generic loader's output is the
  // one kept. As VertexLoaderTester does it, but counted rather than
  // asserted.
  int RunCompared(const u8* src, u8* dst, int count)
  {
    using VertexLoaderManager::binormal_cache;
    using VertexLoaderManager::normal_cache;
    using VertexLoaderManager::position_cache;
    using VertexLoaderManager::position_matrix_index_cache;
    using VertexLoaderManager::tangent_cache;
    const u32 stride = m_native_vtx_decl.stride;
    m_scratch.assign(static_cast<size_t>(count) * stride + 4, 0xCD);

    const auto pmi = position_matrix_index_cache;
    const auto pos = position_cache;
    const auto nrm = normal_cache, tan = tangent_cache, bin = binormal_cache;
    const int count_jit = m_fn(src, m_scratch.data(), count);
    const auto pmi_j = position_matrix_index_cache;
    const auto pos_j = position_cache;
    const auto nrm_j = normal_cache, tan_j = tangent_cache, bin_j = binormal_cache;
    position_matrix_index_cache = pmi;
    position_cache = pos;
    normal_cache = nrm;
    tangent_cache = tan;
    binormal_cache = bin;
    const int count_gen = m_generic->RunVertices(src, dst, count);

    ++g_vtxjit_compare_runs;
    const size_t n = static_cast<size_t>(std::min(count_jit, count_gen)) * stride;
    const bool same = count_jit == count_gen && std::memcmp(dst, m_scratch.data(), n) == 0 &&
                      pmi_j == position_matrix_index_cache && pos_j == position_cache &&
                      nrm_j == normal_cache && tan_j == tangent_cache &&
                      bin_j == binormal_cache;
    if (!same && g_vtxjit_compare_mismatches++ < 8)
    {
      size_t at = 0;
      while (at < n && dst[at] == m_scratch[at])
        ++at;
      std::printf("  vtxjit MISMATCH vcd %08x %08x vat %08x %08x %08x  count %d: jit %d "
                  "generic %d  first byte differing %zu of %zu (stride %u)\n",
                  m_VtxDesc.low.Hex, m_VtxDesc.high.Hex, m_VtxAttr.g0.Hex, m_VtxAttr.g1.Hex,
                  m_VtxAttr.g2.Hex, count, count_jit, count_gen, at, n, stride);
    }
    return count_gen;
  }

  // The local holding an array's base, loading it and its stride before the
  // loop on first use.
  uint32_t ArrayLocal(CPArray array)
  {
    const u32 a = static_cast<u32>(array);
    for (size_t i = 0; i < m_arrays.size(); ++i)
      if (m_arrays[i] == a)
        return L_ARRAYS + 2 * static_cast<uint32_t>(i);
    m_arrays.push_back(a);
    return L_ARRAYS + 2 * static_cast<uint32_t>(m_arrays.size() - 1);
  }

  // Where an attribute is read from, as {local, offset}. Direct: the source
  // at the running offset. Indexed: the index is read and the element's
  // address computed into L_ADDR (for the position, also whether the index
  // skips the vertex, into L_SKIP).
  std::pair<uint32_t, u32> Addr(Emitter& e, CPArray array, VertexComponentFormat mode)
  {
    if (!IsIndexed(mode))
      return {L_SRC, m_src_ofs};
    const uint32_t base = ArrayLocal(array);
    Emitter idx;
    idx.Get(L_SRC);
    if (mode == VertexComponentFormat::Index8)
    {
      idx.Mem(kI32Load8U, m_src_ofs);
      m_src_ofs += 1;
    }
    else
    {
      idx.LoadBE16(m_src_ofs);
      m_src_ofs += 2;
    }
    if (array == CPArray::Position)
    {
      idx.Tee(L_T1);
      idx.Const(mode == VertexComponentFormat::Index8 ? 0xFF : 0xFFFF);
      idx.Op(kI32Eq);
      idx.Set(L_SKIP);
      idx.Get(L_T1);
    }
    PutBytes(e.b, idx.b);
    e.Get(base + 1);
    e.Op(kI32Mul);
    e.Get(base);
    e.Op(kI32Add);
    e.Set(L_ADDR);
    return {L_ADDR, 0};
  }

  // One component, converted, left on the stack: an f32 for the integer
  // formats (returns true), the float's bits as an i32 for the float ones
  // (false; stored with i32.store, so a NaN's bits survive as the generic
  // loader's memcpy keeps them).
  static bool ReadComponent(Emitter& e, uint32_t base, u32 offset, ComponentFormat format,
                            float scale)
  {
    e.Get(base);
    switch (format)
    {
    case ComponentFormat::UByte:
      e.Mem(kI32Load8U, offset);
      break;
    case ComponentFormat::Byte:
      e.Mem(kI32Load8S, offset);
      break;
    case ComponentFormat::UShort:
      e.LoadBE16(offset);
      break;
    case ComponentFormat::Short:
      e.LoadBE16(offset);
      e.Op(kI32Extend16S);
      break;
    default:
      e.LoadBE32(offset);
      return false;
    }
    e.Op(kF32ConvertI32S);
    if (scale != 1.0f)
    {
      e.F32(scale);
      e.Op(kF32Mul);
    }
    return true;
  }

  // `n` components to the output at m_dst_ofs.
  void ReadVector(Emitter& e, uint32_t base, u32 offset, ComponentFormat format, int n,
                  float scale)
  {
    const u32 size = GetElementSize(format);
    for (int i = 0; i < n; ++i)
    {
      e.Get(L_DST);
      const bool as_f32 = ReadComponent(e, base, offset + size * i, format, scale);
      e.Mem(as_f32 ? uint8_t{kF32Store} : uint8_t{kI32Store}, m_dst_ofs + 4 * i);
    }
  }

  // The `n` words just written at `dst_ofs` to an absolute address plus
  // `index_local` * 16 (or a fixed address with no index).
  void CopyOut(Emitter& e, u32 dst_ofs, int n, const void* to, int index_local)
  {
    for (int i = 0; i < n; ++i)
    {
      if (index_local >= 0)
      {
        e.Get(static_cast<uint32_t>(index_local));
        e.Const(4);
        e.Op(kI32Shl);
      }
      else
      {
        e.Const(0);
      }
      e.Get(L_DST);
      e.Mem(kI32Load, dst_ofs + 4 * i);
      e.Mem(kI32Store, static_cast<u32>(reinterpret_cast<uintptr_t>(to)) + 4 * i);
    }
  }

  void Color(Emitter& e, uint32_t base, u32 offset, ColorFormat format);

  std::unique_ptr<VertexLoader> m_generic;
  Bytes m_module;
  Fn m_fn = nullptr;
  pthread_t m_thread{};
  bool m_install_tried = false;
  std::vector<u8> m_scratch;
  u32 m_src_ofs = 0, m_dst_ofs = 0;
  std::vector<u32> m_arrays;
};

void VertexLoaderWasm::Color(Emitter& e, uint32_t base, u32 offset, ColorFormat format)
{
  // The color's word, from VertexLoader_Color.cpp's formulas, then stored.
  e.Get(L_DST);
  switch (format)
  {
  case ColorFormat::RGB888:
  case ColorFormat::RGB888x:
    e.Get(base);
    e.Mem(kI32Load, offset);
    e.Const(0xFF000000);
    e.Op(kI32Or);
    break;
  case ColorFormat::RGBA8888:
    e.Get(base);
    e.Mem(kI32Load, offset);
    break;
  case ColorFormat::RGB565:
  {
    // val = BE16; col = (val >> 8) & 0xF8 | (val << 5) & 0xFC00 | (val << 19) & 0xF80000;
    // col |= (col >> 5) & 0x070007; col |= (col >> 6) & 0x300; col | alpha
    e.Get(base);
    e.LoadBE16(offset);
    e.Set(L_T1);
    auto term = [&](uint8_t shift_op, u32 amount, u32 mask) {
      e.Get(L_T1);
      e.Const(amount);
      e.Op(shift_op);
      e.Const(mask);
      e.Op(kI32And);
    };
    term(kI32ShrU, 8, 0xF8);
    term(kI32Shl, 5, 0xFC00);
    e.Op(kI32Or);
    term(kI32Shl, 19, 0xF80000);
    e.Op(kI32Or);
    e.Set(L_T1);
    term(kI32ShrU, 5, 0x070007);
    e.Get(L_T1);
    e.Op(kI32Or);
    e.Set(L_T1);
    term(kI32ShrU, 6, 0x300);
    e.Get(L_T1);
    e.Op(kI32Or);
    e.Const(0xFF000000);
    e.Op(kI32Or);
    break;
  }
  case ColorFormat::RGBA4444:
  {
    // val = the two bytes as a little-endian u16;
    // col = val & 0xF0 | (val & 0xF) << 12 | (val & 0xF000) << 8 | (val & 0xF00) << 20;
    // col |= col >> 4
    e.Get(base);
    e.Mem(kI32Load16U, offset);
    e.Set(L_T1);
    auto term = [&](u32 mask, u32 shift) {
      e.Get(L_T1);
      e.Const(mask);
      e.Op(kI32And);
      if (shift)
      {
        e.Const(shift);
        e.Op(kI32Shl);
      }
    };
    term(0xF0, 0);
    term(0xF, 12);
    e.Op(kI32Or);
    term(0xF000, 8);
    e.Op(kI32Or);
    term(0x0F00, 20);
    e.Op(kI32Or);
    e.Tee(L_T1);
    e.Get(L_T1);
    e.Const(4);
    e.Op(kI32ShrU);
    e.Op(kI32Or);
    break;
  }
  case ColorFormat::RGBA6666:
  {
    // val = the three bytes big-endian;
    // col = (val >> 16) & 0xFC | (val >> 2) & 0xFC00 | (val << 12) & 0xFC0000 |
    //       (val << 26) & 0xFC000000; col |= (col >> 6) & 0x03030303
    for (u32 i = 0; i < 3; ++i)
    {
      e.Get(base);
      e.Mem(kI32Load8U, offset + i);
      if (i < 2)
      {
        e.Const(16 - 8 * i);
        e.Op(kI32Shl);
      }
      if (i > 0)
        e.Op(kI32Or);
    }
    e.Set(L_T1);
    auto term = [&](uint8_t shift_op, u32 amount, u32 mask) {
      e.Get(L_T1);
      e.Const(amount);
      e.Op(shift_op);
      e.Const(mask);
      e.Op(kI32And);
    };
    term(kI32ShrU, 16, 0xFC);
    term(kI32ShrU, 2, 0xFC00);
    e.Op(kI32Or);
    term(kI32Shl, 12, 0xFC0000);
    e.Op(kI32Or);
    term(kI32Shl, 26, 0xFC000000);
    e.Op(kI32Or);
    e.Set(L_T1);
    term(kI32ShrU, 6, 0x03030303);
    e.Get(L_T1);
    e.Op(kI32Or);
    break;
  }
  }
  e.Mem(kI32Store, m_dst_ofs);
}

bool VertexLoaderWasm::Generate()
{
  const VertexComponentFormat pos_mode = m_VtxDesc.low.Position;
  if (pos_mode == VertexComponentFormat::NotPresent)
    return false;
  for (u32 i = 0; i < m_VtxDesc.low.Color.Size(); i++)
  {
    if (m_VtxDesc.low.Color[i] != VertexComponentFormat::NotPresent &&
        m_VtxAttr.GetColorFormat(i) > ColorFormat::RGBA8888)
      return false;
  }

  const bool pos_indexed = IsIndexed(pos_mode);
  Emitter body;  // the loop's body; the prologue is emitted last, once the arrays are known

  if (m_VtxDesc.low.PosMatIdx)
  {
    body.Get(L_DST);
    body.Get(L_SRC);
    body.Mem(kI32Load8U, m_src_ofs);
    body.Const(0x3F);
    body.Op(kI32And);
    body.Mem(kI32Store, m_dst_ofs);
    // position_matrix_index_cache[remaining] for the last three
    body.Get(L_REMAINING);
    body.Const(3);
    body.Op(kI32LtS);
    body.Op(kIf);
    body.Op(kVoid);
    body.Get(L_REMAINING);
    body.Const(2);
    body.Op(kI32Shl);
    body.Get(L_DST);
    body.Mem(kI32Load, m_dst_ofs);
    body.Mem(kI32Store, static_cast<u32>(reinterpret_cast<uintptr_t>(
                            VertexLoaderManager::position_matrix_index_cache.data())));
    body.Op(kEnd);
    m_src_ofs += 1;
    m_dst_ofs += 4;
  }

  std::array<u32, 8> texmtx_ofs{};
  for (size_t i = 0; i < m_VtxDesc.low.TexMatIdx.Size(); i++)
  {
    if (m_VtxDesc.low.TexMatIdx[i])
      texmtx_ofs[i] = m_src_ofs++;
  }

  // Position
  {
    const int n = m_VtxAttr.g0.PosElements == CoordComponentCount::XY ? 2 : 3;
    const ComponentFormat format = m_VtxAttr.g0.PosFormat;
    const auto [base, offset] = Addr(body, CPArray::Position, pos_mode);
    const u32 dst_ofs = m_dst_ofs;
    ReadVector(body, base, offset, format, n, 1.0f / (1U << m_VtxAttr.g0.PosFrac));
    if (!pos_indexed)
      m_src_ofs += GetElementSize(format) * n;
    m_dst_ofs += 4 * n;
    // position_cache[remaining] for the last three, unless the vertex is skipped
    body.Get(L_REMAINING);
    body.Const(3);
    body.Op(kI32LtS);
    if (pos_indexed)
    {
      body.Get(L_SKIP);
      body.Op(kI32Eqz);
      body.Op(kI32And);
    }
    body.Op(kIf);
    body.Op(kVoid);
    CopyOut(body, dst_ofs, n, VertexLoaderManager::position_cache.data(), L_REMAINING);
    body.Op(kEnd);
  }

  // Normals: N, or N, T and B; with Index3, three indices.
  if (m_VtxDesc.low.Normal != VertexComponentFormat::NotPresent)
  {
    const VertexComponentFormat mode = m_VtxDesc.low.Normal;
    const ComponentFormat format = m_VtxAttr.g0.NormalFormat;
    const bool ntb = m_VtxAttr.g0.NormalElements == NormalComponentCount::NTB;
    const bool index3 = ntb && IsIndexed(mode) && m_VtxAttr.g0.NormalIndex3;
    const u32 elem = GetElementSize(format);
    // FracAdjust: u8 / 2^7, s8 / 2^6, u16 / 2^15, s16 / 2^14. A division by
    // a power of two, and so exactly this multiplication.
    float scale = 1.0f;
    switch (format)
    {
    case ComponentFormat::UByte:
      scale = 1.0f / 128;
      break;
    case ComponentFormat::Byte:
      scale = 1.0f / 64;
      break;
    case ComponentFormat::UShort:
      scale = 1.0f / 32768;
      break;
    case ComponentFormat::Short:
      scale = 1.0f / 16384;
      break;
    default:
      break;
    }
    const u32 dst_start = m_dst_ofs;
    auto [base, offset] = Addr(body, CPArray::Normal, mode);
    if (!index3)
    {
      ReadVector(body, base, offset, format, ntb ? 9 : 3, scale);
      if (!IsIndexed(mode))
        m_src_ofs += elem * (ntb ? 9 : 3);
      m_dst_ofs += 4 * (ntb ? 9 : 3);
    }
    else
    {
      // Each of the three vectors its own index, reading the k-th vector at it.
      for (u32 k = 0; k < 3; ++k)
      {
        if (k > 0)
          std::tie(base, offset) = Addr(body, CPArray::Normal, mode);
        ReadVector(body, base, offset + elem * 3 * k, format, 3, scale);
        m_dst_ofs += 12;
      }
    }
    // the caches, from the last vertex
    body.Get(L_REMAINING);
    body.Op(kI32Eqz);
    body.Op(kIf);
    body.Op(kVoid);
    CopyOut(body, dst_start, 3, VertexLoaderManager::normal_cache.data(), -1);
    if (ntb)
    {
      CopyOut(body, dst_start + 12, 3, VertexLoaderManager::tangent_cache.data(), -1);
      CopyOut(body, dst_start + 24, 3, VertexLoaderManager::binormal_cache.data(), -1);
    }
    body.Op(kEnd);
  }

  for (u8 i = 0; i < m_VtxDesc.low.Color.Size(); i++)
  {
    const VertexComponentFormat mode = m_VtxDesc.low.Color[i];
    if (mode == VertexComponentFormat::NotPresent)
      continue;
    const ColorFormat format = m_VtxAttr.GetColorFormat(i);
    const auto [base, offset] = Addr(body, CPArray::Color0 + i, mode);
    Color(body, base, offset, format);
    if (!IsIndexed(mode))
    {
      static constexpr u32 sizes[] = {2, 3, 4, 2, 3, 4};
      m_src_ofs += sizes[static_cast<u32>(format)];
    }
    m_dst_ofs += 4;
  }

  for (u8 i = 0; i < m_VtxDesc.high.TexCoord.Size(); i++)
  {
    const VertexComponentFormat mode = m_VtxDesc.high.TexCoord[i];
    const bool st = m_VtxAttr.GetTexElements(i) == TexComponentCount::ST;
    const int n = st ? 2 : 1;
    const bool has_mtx = m_VtxDesc.low.TexMatIdx[i];
    if (mode != VertexComponentFormat::NotPresent)
    {
      const ComponentFormat format = m_VtxAttr.GetTexFormat(i);
      const auto [base, offset] = Addr(body, CPArray::TexCoord0 + i, mode);
      ReadVector(body, base, offset, format, n, 1.0f / (1U << m_VtxAttr.GetTexFrac(i)));
      if (!IsIndexed(mode))
        m_src_ofs += GetElementSize(format) * n;
      m_dst_ofs += 4 * n;
    }
    if (has_mtx)
    {
      // Zeros up to the third component, then the matrix index as a float.
      const int zeros = mode == VertexComponentFormat::NotPresent ? 2 : (st ? 0 : 1);
      for (int z = 0; z < zeros; ++z)
      {
        body.Get(L_DST);
        body.Const(0);
        body.Mem(kI32Store, m_dst_ofs);
        m_dst_ofs += 4;
      }
      body.Get(L_DST);
      body.Get(L_SRC);
      body.Mem(kI32Load8U, texmtx_ofs[i]);
      body.Const(0x3F);
      body.Op(kI32And);
      body.Op(kF32ConvertI32S);
      body.Mem(kF32Store, m_dst_ofs);
      m_dst_ofs += 4;
    }
  }

  if (m_src_ofs != m_vertex_size || m_dst_ofs != m_native_vtx_decl.stride)
  {
    std::printf("  vtxjit layout disagrees with the generic loader: src %u/%u dst %u/%u, vcd "
                "%08x %08x vat %08x %08x %08x\n",
                m_src_ofs, m_vertex_size, m_dst_ofs, m_native_vtx_decl.stride,
                m_VtxDesc.low.Hex, m_VtxDesc.high.Hex, m_VtxAttr.g0.Hex, m_VtxAttr.g1.Hex,
                m_VtxAttr.g2.Hex);
    return false;
  }

  // The function: (src, dst, count) -> vertices loaded.
  Emitter f;
  for (size_t i = 0; i < m_arrays.size(); ++i)
  {
    const uint32_t l = L_ARRAYS + 2 * static_cast<uint32_t>(i);
    f.Const(0);
    f.Mem(kI32Load, static_cast<u32>(reinterpret_cast<uintptr_t>(
                        &VertexLoaderManager::cached_arraybases[static_cast<CPArray>(m_arrays[i])])));
    f.Set(l);
    f.Const(0);
    f.Mem(kI32Load, static_cast<u32>(reinterpret_cast<uintptr_t>(
                        &g_main_cp_state.array_strides[static_cast<CPArray>(m_arrays[i])])));
    f.Set(l + 1);
  }
  f.Get(L_COUNT);
  f.Const(1);
  f.Op(kI32Sub);
  f.Set(L_REMAINING);
  f.Op(kLoop);
  f.Op(kVoid);
  PutBytes(f.b, body.b);
  f.Get(L_SRC);
  f.Const(m_src_ofs);
  f.Op(kI32Add);
  f.Set(L_SRC);
  f.Get(L_DST);
  if (pos_indexed)
  {
    // A skipped vertex leaves dst where it was, to be written over.
    f.Const(0);
    f.Const(m_dst_ofs);
    f.Get(L_SKIP);
    f.Op(kSelect);
    f.Op(kI32Add);
    f.Set(L_DST);
    f.Get(L_SKIPPED);
    f.Get(L_SKIP);
    f.Op(kI32Add);
    f.Set(L_SKIPPED);
  }
  else
  {
    f.Const(m_dst_ofs);
    f.Op(kI32Add);
    f.Set(L_DST);
  }
  f.Get(L_REMAINING);
  f.Const(1);
  f.Op(kI32Sub);
  f.Tee(L_REMAINING);
  f.Const(0);
  f.Op(kI32GeS);
  f.Op(kBrIf);
  PutU32(f.b, 0);
  f.Op(kEnd);
  f.Get(L_COUNT);
  f.Get(L_SKIPPED);
  f.Op(kI32Sub);

  ModuleBuilder mb;
  const u32 pages = static_cast<u32>(VtxJitMemoryPages());
  mb.ImportMemory(pages, pages, true);
  mb.SetFunction({kI32, kI32, kI32}, {kI32}, f.b, "f",
                 L_ARRAYS - 3 + 2 * static_cast<uint32_t>(m_arrays.size()));
  m_module = mb.Build();
  return true;
}
}  // namespace

std::unique_ptr<VertexLoaderBase> harness_create_vertex_loader(const TVtxDesc& vtx_desc,
                                                               const VAT& vtx_attr)
{
  if (g_vtxjit_mode == 0)
    return nullptr;
  auto loader = std::make_unique<VertexLoaderWasm>(vtx_desc, vtx_attr,
                                                   std::make_unique<VertexLoader>(vtx_desc, vtx_attr));
  if (!loader->Generate())
  {
    ++g_vtxjit_unsupported;
    return nullptr;
  }
  return loader;
}

void VertexJitReport()
{
  if (g_vtxjit_mode == 0)
    return;
  std::printf("  vtxjit  %s: %u loaders compiled (%llu bytes, %.1f ms installing), %u left to "
              "the generic loader, %u failed to install, %u calls from another thread",
              g_vtxjit_mode == 2 ? "compare" : "on", g_vtxjit_compiled.load(),
              static_cast<unsigned long long>(g_vtxjit_bytes.load()), g_vtxjit_install_ms.load(),
              g_vtxjit_unsupported.load(), g_vtxjit_failed.load(), g_vtxjit_foreign.load());
  if (g_vtxjit_mode == 2)
    std::printf("; compared %llu runs, %llu differed",
                static_cast<unsigned long long>(g_vtxjit_compare_runs.load()),
                static_cast<unsigned long long>(g_vtxjit_compare_mismatches.load()));
  std::printf("\n");
}
