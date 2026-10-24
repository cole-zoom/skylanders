// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <memory>
#include <unordered_map>
#include <vector>

#include <webgpu/webgpu_cpp.h>

#include "VideoCommon/AbstractGfx.h"
#include "VideoCommon/RenderState.h"

namespace WebGPU
{
class Framebuffer;
class Pipeline;
class Texture;

// A region of guest-visible GPU memory written by the CPU through the
// queue: vertices, indices, uniforms. Writes are queue operations, so they
// land before the command buffer they are recorded for is submitted; the
// one thing to avoid is overwriting a range a recorded-but-unsubmitted
// draw still reads, which Allocate prevents by submitting before it wraps.
struct StreamBuffer
{
  wgpu::Buffer buffer;
  u64 size = 0;
  u64 offset = 0;

  void Create(u64 bytes, wgpu::BufferUsage usage, const char* label);
  // An offset `alignment`-aligned with `bytes` free after it.
  u64 Allocate(u64 bytes, u64 alignment);
};

class Gfx final : public ::AbstractGfx
{
public:
  Gfx();
  ~Gfx() override;

  bool IsHeadless() const override { return false; }

  std::unique_ptr<AbstractTexture> CreateTexture(const TextureConfig& config,
                                                 std::string_view name) override;
  std::unique_ptr<AbstractStagingTexture>
  CreateStagingTexture(StagingTextureType type, const TextureConfig& config) override;
  std::unique_ptr<AbstractFramebuffer>
  CreateFramebuffer(AbstractTexture* color_attachment, AbstractTexture* depth_attachment,
                    std::vector<AbstractTexture*> additional_color_attachments) override;
  std::unique_ptr<AbstractShader>
  CreateShaderFromSource(ShaderStage stage, std::string_view source,
                         VideoCommon::ShaderIncluder* shader_includer,
                         std::string_view name) override;
  std::unique_ptr<AbstractShader> CreateShaderFromBinary(ShaderStage stage, const void* data,
                                                         size_t length,
                                                         std::string_view name) override;
  std::unique_ptr<NativeVertexFormat>
  CreateNativeVertexFormat(const PortableVertexDeclaration& vtx_decl) override;
  std::unique_ptr<AbstractPipeline> CreatePipeline(const AbstractPipelineConfig& config,
                                                   const void* cache_data = nullptr,
                                                   size_t cache_data_length = 0) override;

  void SetPipeline(const AbstractPipeline* pipeline) override;
  void SetFramebuffer(AbstractFramebuffer* framebuffer) override;
  void SetAndDiscardFramebuffer(AbstractFramebuffer* framebuffer) override;
  void SetAndClearFramebuffer(AbstractFramebuffer* framebuffer, const ClearColor& color_value = {},
                              float depth_value = 0.0f) override;
  void SetScissorRect(const MathUtil::Rectangle<int>& rc) override;
  void SetTexture(u32 index, const AbstractTexture* texture) override;
  void SetSamplerState(u32 index, const SamplerState& state) override;
  void UnbindTexture(const AbstractTexture* texture) override;
  void SetViewport(float x, float y, float width, float height, float near_depth,
                   float far_depth) override;
  void Draw(u32 base_vertex, u32 num_vertices) override;
  void DrawIndexed(u32 base_index, u32 num_indices, u32 base_vertex) override;
  bool BindBackbuffer(const ClearColor& clear_color = {}) override;
  void PresentBackbuffer() override;
  void Flush() override;
  void WaitForGPUIdle() override;
  SurfaceInfo GetSurfaceInfo() const override;

  // Copies and other non-pass work go into the frame's command encoder,
  // which means the render pass, if one is open, has to end first.
  wgpu::CommandEncoder& GetCopyEncoder();

  // The streaming buffers VertexManager writes into.
  StreamBuffer& Vertices() { return m_vertices; }
  StreamBuffer& Indices() { return m_indices; }
  // Uniform data for the next draws: per stage for GX draws (0 pixel,
  // 1 vertex, 2 geometry), one block for both stages for utility draws.
  void SetGXUniforms(u32 slot, const void* data, u32 size);
  void SetUtilityUniforms(const void* data, u32 size);

private:
  void EnsureEncoder();
  void BeginPass();
  void EndPass();
  bool PrepareDraw();
  wgpu::Sampler GetSampler(const SamplerState& state);
  u64 WriteUniforms(const void* data, u32 size);

  wgpu::CommandEncoder m_encoder;
  wgpu::RenderPassEncoder m_pass;

  Framebuffer* m_framebuffer = nullptr;
  const Pipeline* m_pipeline = nullptr;
  std::array<const Texture*, 8> m_textures{};
  std::array<SamplerState, 8> m_samplers{};
  bool m_clear_pending = false;
  ClearColor m_clear_color{};
  float m_clear_depth = 0.0f;
  float m_viewport[6] = {};
  MathUtil::Rectangle<int> m_scissor{};

  StreamBuffer m_vertices;
  StreamBuffer m_indices;
  StreamBuffer m_uniforms;
  // Offsets into m_uniforms of the blocks the next draw binds.
  std::array<u64, 3> m_gx_uniform{};
  u64 m_utility_uniform = 0;

  std::unordered_map<u64, wgpu::Sampler> m_sampler_cache;
  std::unique_ptr<Texture> m_null_texture;
  // The presenter draws into this, not into the canvas: a canvas texture
  // is valid only until the worker yields, and any wait -- a readback, a
  // shader's validation -- yields. The canvas texture is taken, filled by
  // a copy and submitted inside PresentBackbuffer, with no wait between.
  std::unique_ptr<Texture> m_backbuffer_texture;
  std::unique_ptr<Framebuffer> m_backbuffer;
};
}  // namespace WebGPU
