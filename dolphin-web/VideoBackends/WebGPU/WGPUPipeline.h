// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <memory>
#include <string>
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
  Pipeline(const AbstractPipelineConfig& config, wgpu::ShaderModule vs, wgpu::ShaderModule ps,
           std::vector<Binding> bindings);
  ~Pipeline() override;

  // The browser's pipeline, built now if the background has not got to it
  // (one compile: ~2 ms on a browser that has met it before, ~40 ms on one
  // whose Metal cache never has). Null if the browser refused it (logged
  // then), and the draw is skipped. GPU thread only.
  const wgpu::RenderPipeline& GetPipeline() const;
  // The union of the vertex and pixel shaders' declarations: what a bind
  // group for this pipeline must contain, no more.
  const std::vector<Binding>& GetBindings() const { return m_bindings; }
  bool IsUtility() const { return m_config.usage == AbstractPipelineUsage::Utility; }

  // Checks the description and returns without asking the browser: the
  // pipeline is built at its first draw or in the background, whichever
  // comes first. Dolphin makes every pipeline the UID file names at boot,
  // over a thousand with every figure's; built there, a browser whose Metal
  // cache had never seen them froze for a minute (Metal manages ~40
  // compiles a second however they are asked for).
  static std::unique_ptr<Pipeline> Create(const AbstractPipelineConfig& config);

  // Once a frame: takes the background's answers, and asks for more
  // (createRenderPipelineAsync), never more than a few at once, so a draw
  // that needs one now is never queued behind hundreds.
  static void BuildInBackground();

private:
  struct Description;
  struct Answer
  {
    wgpu::RenderPipeline pipeline;
    std::string error;
  };
  enum class State
  {
    Waiting,    // not asked for yet
    Requested,  // asked for in the background
    Built,
  };

  void Request() const;
  void TakeAnswer() const;
  void Build() const;

  wgpu::ShaderModule m_vs;
  wgpu::ShaderModule m_ps;
  std::vector<Binding> m_bindings;
  mutable State m_state = State::Waiting;
  mutable wgpu::Future m_future;
  mutable std::shared_ptr<Answer> m_answer;
  mutable wgpu::RenderPipeline m_pipeline;
};
}  // namespace WebGPU
