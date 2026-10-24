// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "WGPUVertexManager.h"

#include "Core/System.h"
#include "VideoCommon/GeometryShaderManager.h"
#include "VideoCommon/PixelShaderManager.h"
#include "VideoCommon/Statistics.h"
#include "VideoCommon/VertexShaderManager.h"

#include "WGPUContext.h"
#include "WGPUGfx.h"

namespace WebGPU
{
static Gfx& GetGfx()
{
  return *static_cast<Gfx*>(g_gfx.get());
}

VertexManager::VertexManager()
    : m_cpu_vertices(MAXVBUFFERSIZE), m_cpu_indices(MAXIBUFFERSIZE)
{
}

VertexManager::~VertexManager() = default;

void VertexManager::ResetBuffer(u32 vertex_stride)
{
  m_base_buffer_pointer = m_cpu_vertices.data();
  m_cur_buffer_pointer = m_base_buffer_pointer;
  m_end_buffer_pointer = m_base_buffer_pointer + m_cpu_vertices.size();
  m_index_generator.Start(m_cpu_indices.data());
}

void VertexManager::CommitBuffer(u32 num_vertices, u32 vertex_stride, u32 num_indices,
                                 u32* out_base_vertex, u32* out_base_index)
{
  StreamBuffer& vertices = GetGfx().Vertices();
  StreamBuffer& indices = GetGfx().Indices();
  // The base vertex is the offset in strides, so the offset is a multiple
  // of the stride -- and of 4, which buffer writes require.
  const u32 vsize = num_vertices * vertex_stride;
  u64 alignment = vertex_stride ? vertex_stride : 4;
  while (alignment % 4)
    alignment += vertex_stride;
  const u64 vat = vertices.Allocate((vsize + 3) & ~3u, alignment);
  const u32 isize = num_indices * sizeof(u16);
  const u64 iat = indices.Allocate((isize + 3) & ~3u, 4);
  if (vsize)
    g_context.queue.WriteBuffer(vertices.buffer, vat, m_cpu_vertices.data(), (vsize + 3) & ~3u);
  if (isize)
    g_context.queue.WriteBuffer(indices.buffer, iat, m_cpu_indices.data(), (isize + 3) & ~3u);
  ADDSTAT(g_stats.this_frame.bytes_vertex_streamed, vsize);
  ADDSTAT(g_stats.this_frame.bytes_index_streamed, isize);
  *out_base_vertex = vertex_stride ? static_cast<u32>(vat / vertex_stride) : 0;
  *out_base_index = static_cast<u32>(iat / sizeof(u16));
}

void VertexManager::UploadUniforms()
{
  auto& system = Core::System::GetInstance();
  auto& vs = system.GetVertexShaderManager();
  auto& gs = system.GetGeometryShaderManager();
  auto& ps = system.GetPixelShaderManager();
  if (!m_uniforms_uploaded || ps.dirty)
    GetGfx().SetGXUniforms(0, &ps.constants, sizeof(ps.constants));
  if (!m_uniforms_uploaded || vs.dirty)
    GetGfx().SetGXUniforms(1, &vs.constants, sizeof(vs.constants));
  if (!m_uniforms_uploaded || gs.dirty)
    GetGfx().SetGXUniforms(2, &gs.constants, sizeof(gs.constants));
  m_uniforms_uploaded = true;
  vs.dirty = false;
  gs.dirty = false;
  ps.dirty = false;
}

void VertexManager::UploadUtilityUniforms(const void* uniforms, u32 uniforms_size)
{
  GetGfx().SetUtilityUniforms(uniforms, uniforms_size);
  // A utility draw rebinds slots 0 and 1; GX constants go back on the
  // next GX draw.
  m_uniforms_uploaded = false;
}
}  // namespace WebGPU
