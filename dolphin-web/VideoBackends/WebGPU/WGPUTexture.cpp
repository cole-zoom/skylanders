// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "WGPUTexture.h"

#include <algorithm>

#include <emscripten.h>

#include "Common/Assert.h"
#include "Common/Logging/Log.h"

#include "WGPUContext.h"
#include "WGPUGfx.h"

namespace WebGPU
{
wgpu::TextureFormat ToWGPU(AbstractTextureFormat format)
{
  switch (format)
  {
  case AbstractTextureFormat::RGBA8:
    return wgpu::TextureFormat::RGBA8Unorm;
  case AbstractTextureFormat::BGRA8:
    return wgpu::TextureFormat::BGRA8Unorm;
  case AbstractTextureFormat::RGB10_A2:
    return wgpu::TextureFormat::RGB10A2Unorm;
  case AbstractTextureFormat::RGBA16F:
    return wgpu::TextureFormat::RGBA16Float;
  case AbstractTextureFormat::DXT1:
    return wgpu::TextureFormat::BC1RGBAUnorm;
  case AbstractTextureFormat::DXT3:
    return wgpu::TextureFormat::BC2RGBAUnorm;
  case AbstractTextureFormat::DXT5:
    return wgpu::TextureFormat::BC3RGBAUnorm;
  case AbstractTextureFormat::BPTC:
    return wgpu::TextureFormat::BC7RGBAUnorm;
  case AbstractTextureFormat::R16:
    return g_context.has_unorm16 ? wgpu::TextureFormat::R16Unorm : wgpu::TextureFormat::R32Float;
  case AbstractTextureFormat::D16:
    return wgpu::TextureFormat::Depth16Unorm;
  case AbstractTextureFormat::D24_S8:
    return wgpu::TextureFormat::Depth24PlusStencil8;
  case AbstractTextureFormat::R32F:
    return wgpu::TextureFormat::R32Float;
  case AbstractTextureFormat::D32F:
    return wgpu::TextureFormat::Depth32Float;
  case AbstractTextureFormat::D32F_S8:
    return wgpu::TextureFormat::Depth32FloatStencil8;
  default:
    return wgpu::TextureFormat::Undefined;
  }
}

static wgpu::TextureAspect AspectOf(AbstractTextureFormat format)
{
  return AbstractTexture::IsDepthFormat(format) ? wgpu::TextureAspect::DepthOnly :
                                                  wgpu::TextureAspect::All;
}

Texture::Texture(const TextureConfig& config, wgpu::Texture texture)
    : AbstractTexture(config), m_texture(std::move(texture))
{
  static u64 next_id = 1;
  m_id = next_id++;
  wgpu::TextureViewDescriptor vdesc{};
  vdesc.dimension = wgpu::TextureViewDimension::e2DArray;
  vdesc.aspect = AspectOf(config.format);
  m_sample_view = m_texture.CreateView(&vdesc);
}

Texture::~Texture()
{
  if (g_gfx)
  {
    static_cast<Gfx*>(g_gfx.get())->UnbindTexture(this);
    static_cast<Gfx*>(g_gfx.get())->ForgetTexture(this);
  }
}

wgpu::TextureView Texture::CreateAttachmentView(u32 layer, u32 level) const
{
  wgpu::TextureViewDescriptor vdesc{};
  vdesc.dimension = wgpu::TextureViewDimension::e2D;
  vdesc.baseArrayLayer = layer;
  vdesc.arrayLayerCount = 1;
  vdesc.baseMipLevel = level;
  vdesc.mipLevelCount = 1;
  return m_texture.CreateView(&vdesc);
}

void Texture::CopyRectangleFromTexture(const AbstractTexture* src,
                                       const MathUtil::Rectangle<int>& src_rect, u32 src_layer,
                                       u32 src_level, const MathUtil::Rectangle<int>& dst_rect,
                                       u32 dst_layer, u32 dst_level)
{
  const Texture* source = static_cast<const Texture*>(src);
  wgpu::TexelCopyTextureInfo from{};
  from.texture = source->GetTexture();
  from.mipLevel = src_level;
  from.origin = {static_cast<u32>(src_rect.left), static_cast<u32>(src_rect.top), src_layer};
  from.aspect = AspectOf(source->GetFormat());
  wgpu::TexelCopyTextureInfo to{};
  to.texture = m_texture;
  to.mipLevel = dst_level;
  to.origin = {static_cast<u32>(dst_rect.left), static_cast<u32>(dst_rect.top), dst_layer};
  to.aspect = AspectOf(m_config.format);
  const wgpu::Extent3D size = {static_cast<u32>(src_rect.GetWidth()),
                               static_cast<u32>(src_rect.GetHeight()), 1};
  static_cast<Gfx*>(g_gfx.get())->GetCopyEncoder().CopyTextureToTexture(&from, &to, &size);
}

void Texture::ResolveFromTexture(const AbstractTexture*, const MathUtil::Rectangle<int>&, u32, u32)
{
  // Single-sampled only: the backend reports AAModes = {1}.
}

void Texture::Load(u32 level, u32 width, u32 height, u32 row_length, const u8* buffer,
                   size_t buffer_size, u32 layer)
{
  // Uploads are queue operations: they happen before any command buffer
  // submitted later, so the draws recorded after this see the data.
  const u32 block = IsCompressedFormat(m_config.format) ? 4 : 1;
  wgpu::TexelCopyTextureInfo to{};
  to.texture = m_texture;
  to.mipLevel = level;
  to.origin = {0, 0, layer};
  to.aspect = AspectOf(m_config.format);
  wgpu::TexelCopyBufferLayout layout{};
  layout.bytesPerRow = static_cast<u32>(CalculateStrideForFormat(m_config.format, row_length));
  layout.rowsPerImage = (height + block - 1) / block;
  // A compressed level smaller than a block is written as a whole block.
  const wgpu::Extent3D size = {(width + block - 1) / block * block,
                               (height + block - 1) / block * block, 1};
  g_context.queue.WriteTexture(&to, buffer, buffer_size, &layout, &size);
}

StagingTexture::StagingTexture(StagingTextureType type, const TextureConfig& config,
                               wgpu::Buffer buffer, u32 stride)
    : AbstractStagingTexture(type, config), m_buffer(std::move(buffer)), m_stride(stride)
{
  m_map_stride = stride;
}

StagingTexture::~StagingTexture()
{
  if (IsMapped())
    m_buffer.Unmap();
}

void StagingTexture::CopyFromTexture(const AbstractTexture* src,
                                     const MathUtil::Rectangle<int>& src_rect, u32 src_layer,
                                     u32 src_level, const MathUtil::Rectangle<int>& dst_rect)
{
  // A mapped buffer cannot be written by the GPU.
  if (IsMapped())
    Unmap();
  const Texture* source = static_cast<const Texture*>(src);
  wgpu::TexelCopyTextureInfo from{};
  from.texture = source->GetTexture();
  from.mipLevel = src_level;
  from.origin = {static_cast<u32>(src_rect.left), static_cast<u32>(src_rect.top), src_layer};
  from.aspect = AspectOf(source->GetFormat());
  wgpu::TexelCopyBufferInfo to{};
  to.buffer = m_buffer;
  to.layout.offset = static_cast<u64>(dst_rect.top) * m_stride + dst_rect.left * m_texel_size;
  to.layout.bytesPerRow = m_stride;
  to.layout.rowsPerImage = static_cast<u32>(src_rect.GetHeight());
  const wgpu::Extent3D size = {static_cast<u32>(src_rect.GetWidth()),
                               static_cast<u32>(src_rect.GetHeight()), 1};
  static_cast<Gfx*>(g_gfx.get())->GetCopyEncoder().CopyTextureToBuffer(&from, &to, &size);
  m_needs_flush = true;
}

void StagingTexture::CopyToTexture(const MathUtil::Rectangle<int>& src_rect, AbstractTexture* dst,
                                   const MathUtil::Rectangle<int>& dst_rect, u32 dst_layer,
                                   u32 dst_level)
{
  if (IsMapped())
    Unmap();
  Texture* dest = static_cast<Texture*>(dst);
  wgpu::TexelCopyBufferInfo from{};
  from.buffer = m_buffer;
  from.layout.offset = static_cast<u64>(src_rect.top) * m_stride + src_rect.left * m_texel_size;
  from.layout.bytesPerRow = m_stride;
  from.layout.rowsPerImage = static_cast<u32>(src_rect.GetHeight());
  wgpu::TexelCopyTextureInfo to{};
  to.texture = dest->GetTexture();
  to.mipLevel = dst_level;
  to.origin = {static_cast<u32>(dst_rect.left), static_cast<u32>(dst_rect.top), dst_layer};
  to.aspect = AspectOf(dest->GetFormat());
  const wgpu::Extent3D size = {static_cast<u32>(src_rect.GetWidth()),
                               static_cast<u32>(src_rect.GetHeight()), 1};
  static_cast<Gfx*>(g_gfx.get())->GetCopyEncoder().CopyBufferToTexture(&from, &to, &size);
}

bool StagingTexture::Map()
{
  if (IsMapped())
    return true;
  // The copies recorded into this buffer have to reach the GPU before the
  // map can complete: an EFB peek is a submit and a full round trip, ~0.4
  // ms (spikes/webgpu-thread/FINDINGS.md).
  if (m_needs_flush)
    Flush();
  const bool read = m_type == StagingTextureType::Readback;
  bool ok = false;
  const double t0 = emscripten_get_now();
  g_context.Wait(m_buffer.MapAsync(read ? wgpu::MapMode::Read : wgpu::MapMode::Write, 0,
                                   m_buffer.GetSize(), wgpu::CallbackMode::WaitAnyOnly,
                                   [&](wgpu::MapAsyncStatus status, wgpu::StringView message) {
                                     ok = status == wgpu::MapAsyncStatus::Success;
                                     if (!ok)
                                       WGPU_LOG("WebGPU: map failed: {}",
                                                     std::string_view(message.data,
                                                                      message.length));
                                   }));
  g_stats_wgpu.maps++;
  g_stats_wgpu.map_ms += emscripten_get_now() - t0;
  if (!ok)
    return false;
  m_map_pointer = read ? const_cast<char*>(static_cast<const char*>(
                             m_buffer.GetConstMappedRange(0, m_buffer.GetSize()))) :
                         static_cast<char*>(m_buffer.GetMappedRange(0, m_buffer.GetSize()));
  return m_map_pointer != nullptr;
}

void StagingTexture::Unmap()
{
  if (!IsMapped())
    return;
  m_buffer.Unmap();
  m_map_pointer = nullptr;
}

void StagingTexture::Flush()
{
  static_cast<Gfx*>(g_gfx.get())->Flush();
  m_needs_flush = false;
}

Framebuffer::Framebuffer(AbstractTexture* color, AbstractTexture* depth,
                         std::vector<AbstractTexture*> additional_colors,
                         AbstractTextureFormat color_format, AbstractTextureFormat depth_format,
                         u32 width, u32 height)
    : AbstractFramebuffer(color, depth, additional_colors, color_format, depth_format, width,
                          height, 1, 1)
{
  if (color)
    m_color_views.push_back(static_cast<Texture*>(color)->CreateAttachmentView(0, 0));
  for (AbstractTexture* extra : additional_colors)
    m_color_views.push_back(static_cast<Texture*>(extra)->CreateAttachmentView(0, 0));
  if (depth)
    m_depth_view = static_cast<Texture*>(depth)->CreateAttachmentView(0, 0);
}

Framebuffer::~Framebuffer() = default;
}  // namespace WebGPU
