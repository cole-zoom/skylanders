// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <memory>
#include <vector>

#include <webgpu/webgpu_cpp.h>

#include "VideoCommon/AbstractPipeline.h"
#include "VideoCommon/NativeVertexFormat.h"

#include "WGPUShader.h"

namespace WebGPU
{
class VertexFormat final : public NativeVertexFormat
{
public:
  explicit VertexFormat(const PortableVertexDeclaration& decl);

  const wgpu::VertexBufferLayout& GetLayout() const { return m_layout; }

private:
  std::vector<wgpu::VertexAttribute> m_attributes;
  wgpu::VertexBufferLayout m_layout{};
};

class Pipeline final : public AbstractPipeline
{
public:
  Pipeline(const AbstractPipelineConfig& config, wgpu::RenderPipeline pipeline,
           std::vector<Binding> bindings);
  ~Pipeline() override;

  const wgpu::RenderPipeline& GetPipeline() const { return m_pipeline; }
  // The union of the vertex and pixel shaders' declarations: what a bind
  // group for this pipeline must contain, no more.
  const std::vector<Binding>& GetBindings() const { return m_bindings; }
  bool IsUtility() const { return m_config.usage == AbstractPipelineUsage::Utility; }

  static std::unique_ptr<Pipeline> Create(const AbstractPipelineConfig& config);

private:
  wgpu::RenderPipeline m_pipeline;
  std::vector<Binding> m_bindings;
};
}  // namespace WebGPU
