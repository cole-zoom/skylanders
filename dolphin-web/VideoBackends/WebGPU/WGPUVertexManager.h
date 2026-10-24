// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <vector>

#include "VideoCommon/VertexManagerBase.h"

namespace WebGPU
{
// Vertices and indices are generated into CPU memory and written to the
// streaming buffers once per batch (Gfx's StreamBuffer).
class VertexManager final : public VertexManagerBase
{
public:
  VertexManager();
  ~VertexManager() override;

  void UploadUtilityUniforms(const void* uniforms, u32 uniforms_size) override;

protected:
  void ResetBuffer(u32 vertex_stride) override;
  void CommitBuffer(u32 num_vertices, u32 vertex_stride, u32 num_indices, u32* out_base_vertex,
                    u32* out_base_index) override;
  void UploadUniforms() override;

private:
  std::vector<u8> m_cpu_vertices;
  std::vector<u16> m_cpu_indices;
  bool m_uniforms_uploaded = false;
};
}  // namespace WebGPU
