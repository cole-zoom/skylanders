// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <vector>

#include <webgpu/webgpu_cpp.h>

#include "VideoCommon/AbstractFramebuffer.h"
#include "VideoCommon/AbstractStagingTexture.h"
#include "VideoCommon/AbstractTexture.h"

namespace WebGPU
{
wgpu::TextureFormat ToWGPU(AbstractTextureFormat format);

class Texture final : public AbstractTexture
{
public:
  Texture(const TextureConfig& config, wgpu::Texture texture);
  ~Texture() override;

  void CopyRectangleFromTexture(const AbstractTexture* src,
                                const MathUtil::Rectangle<int>& src_rect, u32 src_layer,
                                u32 src_level, const MathUtil::Rectangle<int>& dst_rect,
                                u32 dst_layer, u32 dst_level) override;
  void ResolveFromTexture(const AbstractTexture* src, const MathUtil::Rectangle<int>& rect,
                          u32 layer, u32 level) override;
  void Load(u32 level, u32 width, u32 height, u32 row_length, const u8* buffer, size_t buffer_size,
            u32 layer) override;

  const wgpu::Texture& GetTexture() const { return m_texture; }
  // Every layer and level, as the shaders sample it: texture_2d_array<f32>.
  const wgpu::TextureView& GetSampleView() const { return m_sample_view; }
  // One layer and level, as a render pass attaches it.
  wgpu::TextureView CreateAttachmentView(u32 layer, u32 level) const;
  bool IsDepth() const { return IsDepthFormat(m_config.format); }
  // Never reused, unlike the address: what Gfx's bind group cache keys on.
  u64 GetId() const { return m_id; }

private:
  u64 m_id;
  wgpu::Texture m_texture;
  wgpu::TextureView m_sample_view;
};

class StagingTexture final : public AbstractStagingTexture
{
public:
  StagingTexture(StagingTextureType type, const TextureConfig& config, wgpu::Buffer buffer,
                 u32 stride);
  ~StagingTexture() override;

  void CopyFromTexture(const AbstractTexture* src, const MathUtil::Rectangle<int>& src_rect,
                       u32 src_layer, u32 src_level,
                       const MathUtil::Rectangle<int>& dst_rect) override;
  void CopyToTexture(const MathUtil::Rectangle<int>& src_rect, AbstractTexture* dst,
                     const MathUtil::Rectangle<int>& dst_rect, u32 dst_layer,
                     u32 dst_level) override;
  bool Map() override;
  void Unmap() override;
  void Flush() override;

private:
  wgpu::Buffer m_buffer;
  u32 m_stride;
};

class Framebuffer final : public AbstractFramebuffer
{
public:
  Framebuffer(AbstractTexture* color, AbstractTexture* depth,
              std::vector<AbstractTexture*> additional_colors, AbstractTextureFormat color_format,
              AbstractTextureFormat depth_format, u32 width, u32 height);
  ~Framebuffer() override;

  // The attachments a render pass on this framebuffer uses, in order:
  // the colour attachment, then the additional ones.
  const std::vector<wgpu::TextureView>& GetColorViews() const { return m_color_views; }
  const wgpu::TextureView& GetDepthView() const { return m_depth_view; }

  // The backbuffer's colour attachment is the canvas's current texture,
  // which changes every frame.
  void SetBackbufferView(wgpu::TextureView view) { m_color_views = {std::move(view)}; }

private:
  std::vector<wgpu::TextureView> m_color_views;
  wgpu::TextureView m_depth_view;
};
}  // namespace WebGPU
