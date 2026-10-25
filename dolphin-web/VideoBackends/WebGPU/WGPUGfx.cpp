// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "WGPUGfx.h"

#include <algorithm>
#include <cstring>

#include <emscripten.h>

#include "Common/Assert.h"
#include "Common/Logging/Log.h"
#include "VideoCommon/VideoConfig.h"

#include "WGPUContext.h"
#include "WGPUPipeline.h"
#include "WGPUShader.h"
#include "WGPUTexture.h"

namespace WebGPU
{
namespace
{
constexpr u64 kVertexBufferSize = 32 << 20;
constexpr u64 kIndexBufferSize = 8 << 20;
constexpr u64 kUniformBufferSize = 16 << 20;
// What a uniform binding covers: more than any of Dolphin's uniform
// blocks, so the binding is never smaller than the struct a shader
// declares, whatever the data uploaded for it.
constexpr u64 kUniformWindow = 16 << 10;
// WebGPU's minUniformBufferOffsetAlignment.
constexpr u64 kUniformAlignment = 256;

constexpr u64 Align(u64 v, u64 a)
{
  return (v + a - 1) / a * a;
}

AbstractTextureFormat FromWGPU(wgpu::TextureFormat format)
{
  return format == wgpu::TextureFormat::BGRA8Unorm ? AbstractTextureFormat::BGRA8 :
                                                     AbstractTextureFormat::RGBA8;
}
}  // namespace

void StreamBuffer::Create(u64 bytes, wgpu::BufferUsage usage, const char* label)
{
  wgpu::BufferDescriptor desc{};
  desc.label = label;
  desc.size = bytes;
  desc.usage = usage | wgpu::BufferUsage::CopyDst;
  buffer = g_context.device.CreateBuffer(&desc);
  size = bytes;
  offset = 0;
}

u64 StreamBuffer::Allocate(u64 bytes, u64 alignment)
{
  u64 at = Align(offset, alignment);
  if (at + bytes > size)
  {
    // Everything recorded so far reads the older ranges; once it is
    // submitted, writes to them are ordered after it.
    static_cast<Gfx*>(g_gfx.get())->Flush();
    at = 0;
  }
  offset = at + bytes;
  return at;
}

void StreamBuffer::Write(u64 at, const void* data, u64 bytes)
{
  g_stats_wgpu.write_bytes += bytes;
  if (g_context.no_upload)
    return;
  if (!g_context.fast_path)
  {
    g_context.queue.WriteBuffer(buffer, at, data, bytes);
    g_stats_wgpu.write_buffers++;
    return;
  }
  // Behind what is gathered: only after a wrap, which submitted it, so
  // this cannot happen -- but if it did, the gathered range goes first.
  if (!m_pending.empty() && at < m_pending_at + m_pending.size())
    FlushWrites();
  if (m_pending.empty())
    m_pending_at = at;
  const u64 from = at - m_pending_at;
  m_pending.resize(from + bytes);
  std::memcpy(m_pending.data() + from, data, bytes);
}

void StreamBuffer::FlushWrites()
{
  if (m_pending.empty())
    return;
  g_context.queue.WriteBuffer(buffer, m_pending_at, m_pending.data(), m_pending.size());
  g_stats_wgpu.write_buffers++;
  m_pending.clear();
}

Gfx::Gfx()
{
  // g_ActiveConfig is what VideoCommon reads from here on (sample counts,
  // EFB scale); the Null and Metal backends make it current here too.
  UpdateActiveConfig();
  m_vertices.Create(kVertexBufferSize, wgpu::BufferUsage::Vertex, "vertices");
  m_indices.Create(kIndexBufferSize, wgpu::BufferUsage::Index, "indices");
  m_uniforms.Create(kUniformBufferSize, wgpu::BufferUsage::Uniform, "uniforms");
  {
    wgpu::BindGroupEntry entries[3] = {};
    for (u32 i = 0; i < 3; ++i)
    {
      entries[i].binding = i;
      entries[i].buffer = m_uniforms.buffer;
      entries[i].size = kUniformWindow;
    }
    wgpu::BindGroupDescriptor desc{};
    desc.layout = g_context.uniform_layout;
    desc.entryCount = 3;
    desc.entries = entries;
    m_uniform_group = g_context.device.CreateBindGroup(&desc);
  }

  // What an unbound texture slot samples.
  wgpu::TextureDescriptor tdesc{};
  tdesc.size = {1, 1, 1};
  tdesc.format = wgpu::TextureFormat::RGBA8Unorm;
  tdesc.usage = wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::CopyDst;
  m_null_texture = std::make_unique<Texture>(
      TextureConfig(1, 1, 1, 1, 1, AbstractTextureFormat::RGBA8, 0,
                    AbstractTextureType::Texture_2DArray),
      g_context.device.CreateTexture(&tdesc));
  const u32 black = 0;
  m_null_texture->Load(0, 1, 1, 1, reinterpret_cast<const u8*>(&black), 4, 0);

  const TextureConfig bb_config(g_context.surface_width, g_context.surface_height, 1, 1, 1,
                                FromWGPU(g_context.surface_format),
                                AbstractTextureFlag_RenderTarget,
                                AbstractTextureType::Texture_2DArray);
  m_backbuffer_texture.reset(static_cast<Texture*>(CreateTexture(bb_config, "backbuffer").release()));
  m_backbuffer = std::make_unique<Framebuffer>(
      m_backbuffer_texture.get(), nullptr, std::vector<AbstractTexture*>{},
      bb_config.format, AbstractTextureFormat::Undefined, bb_config.width, bb_config.height);
}

Gfx::~Gfx() = default;

std::unique_ptr<AbstractTexture> Gfx::CreateTexture(const TextureConfig& config,
                                                    std::string_view name)
{
  wgpu::TextureDescriptor desc{};
  desc.label = {name.data(), name.size()};
  desc.size = {config.width, config.height, config.layers};
  desc.mipLevelCount = config.levels;
  desc.sampleCount = config.samples;
  desc.format = ToWGPU(config.format);
  desc.usage = wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::CopyDst |
               wgpu::TextureUsage::CopySrc;
  if (config.IsRenderTarget())
    desc.usage |= wgpu::TextureUsage::RenderAttachment;
  if (desc.format == wgpu::TextureFormat::Undefined)
  {
    WGPU_LOG("WebGPU: no format for texture {} (format {})", name,
                  static_cast<u32>(config.format));
    return nullptr;
  }
  wgpu::Texture texture;
  const std::string error =
      g_context.CaptureValidation([&] { texture = g_context.device.CreateTexture(&desc); });
  if (!error.empty())
  {
    WGPU_LOG("WebGPU: texture {} rejected: {}", name, error);
    return nullptr;
  }
  WGPU_LOG("WebGPU: texture {} {}x{} layers {} levels {} format {} rt {}", name,
               config.width, config.height, config.layers, config.levels,
               static_cast<u32>(config.format), config.IsRenderTarget());
  return std::make_unique<Texture>(config, std::move(texture));
}

std::unique_ptr<AbstractStagingTexture> Gfx::CreateStagingTexture(StagingTextureType type,
                                                                  const TextureConfig& config)
{
  // Rows of a texture-buffer copy are 256-byte aligned.
  const u32 stride = static_cast<u32>(
      Align(AbstractTexture::CalculateStrideForFormat(config.format, config.width), 256));
  wgpu::BufferDescriptor desc{};
  desc.size = static_cast<u64>(stride) * config.height;
  desc.usage = type == StagingTextureType::Readback ?
                   wgpu::BufferUsage::MapRead | wgpu::BufferUsage::CopyDst :
                   wgpu::BufferUsage::MapWrite | wgpu::BufferUsage::CopySrc;
  return std::make_unique<StagingTexture>(type, config, g_context.device.CreateBuffer(&desc),
                                          stride);
}

std::unique_ptr<AbstractFramebuffer>
Gfx::CreateFramebuffer(AbstractTexture* color_attachment, AbstractTexture* depth_attachment,
                       std::vector<AbstractTexture*> additional_color_attachments)
{
  AbstractTexture* any = color_attachment ? color_attachment : depth_attachment;
  if (!any)
    return nullptr;
  return std::make_unique<Framebuffer>(
      color_attachment, depth_attachment, std::move(additional_color_attachments),
      color_attachment ? color_attachment->GetFormat() : AbstractTextureFormat::Undefined,
      depth_attachment ? depth_attachment->GetFormat() : AbstractTextureFormat::Undefined,
      any->GetWidth(), any->GetHeight());
}

std::unique_ptr<AbstractShader> Gfx::CreateShaderFromSource(ShaderStage stage,
                                                            std::string_view source,
                                                            VideoCommon::ShaderIncluder*,
                                                            std::string_view name)
{
  return Shader::Compile(stage, source, name);
}

std::unique_ptr<AbstractShader> Gfx::CreateShaderFromBinary(ShaderStage, const void*, size_t,
                                                            std::string_view)
{
  return nullptr;
}

std::unique_ptr<NativeVertexFormat>
Gfx::CreateNativeVertexFormat(const PortableVertexDeclaration& vtx_decl)
{
  return std::make_unique<VertexFormat>(vtx_decl);
}

std::unique_ptr<AbstractPipeline> Gfx::CreatePipeline(const AbstractPipelineConfig& config,
                                                      const void*, size_t)
{
  return Pipeline::Create(config);
}

void Gfx::EnsureEncoder()
{
  if (!m_encoder)
    m_encoder = g_context.device.CreateCommandEncoder();
}

wgpu::CommandEncoder& Gfx::GetCopyEncoder()
{
  EndPass();
  EnsureEncoder();
  return m_encoder;
}

void Gfx::BeginPass()
{
  if (m_pass || !m_framebuffer)
    return;
  EnsureEncoder();
  std::vector<wgpu::RenderPassColorAttachment> colors;
  for (const wgpu::TextureView& view : m_framebuffer->GetColorViews())
  {
    wgpu::RenderPassColorAttachment a{};
    a.view = view;
    a.loadOp = m_clear_pending ? wgpu::LoadOp::Clear : wgpu::LoadOp::Load;
    a.storeOp = wgpu::StoreOp::Store;
    a.clearValue = {m_clear_color[0], m_clear_color[1], m_clear_color[2], m_clear_color[3]};
    colors.push_back(a);
  }
  wgpu::RenderPassDepthStencilAttachment depth{};
  wgpu::RenderPassDescriptor desc{};
  desc.colorAttachmentCount = colors.size();
  desc.colorAttachments = colors.data();
  if (m_framebuffer->GetDepthView())
  {
    depth.view = m_framebuffer->GetDepthView();
    depth.depthLoadOp = m_clear_pending ? wgpu::LoadOp::Clear : wgpu::LoadOp::Load;
    depth.depthStoreOp = wgpu::StoreOp::Store;
    depth.depthClearValue = m_clear_depth;
    desc.depthStencilAttachment = &depth;
  }
  m_clear_pending = false;
  m_pass = m_encoder.BeginRenderPass(&desc);
  m_pass_pipeline = nullptr;
  m_pass_state_set = false;
  // A pass starts with the viewport and scissor at their defaults.
  SetViewport(m_viewport[0], m_viewport[1], m_viewport[2], m_viewport[3], m_viewport[4],
              m_viewport[5]);
  SetScissorRect(m_scissor);
}

void Gfx::EndPass()
{
  if (!m_pass)
    return;
  m_pass.End();
  m_pass = nullptr;
}

void Gfx::Flush()
{
  EndPass();
  // Queue writes land before any command buffer submitted after them.
  m_vertices.FlushWrites();
  m_indices.FlushWrites();
  m_uniforms.FlushWrites();
  if (!m_encoder)
    return;
  wgpu::CommandBuffer commands = m_encoder.Finish();
  g_context.queue.Submit(1, &commands);
  g_stats_wgpu.submits++;
  m_encoder = nullptr;
}

void Gfx::WaitForGPUIdle()
{
  Flush();
  g_context.Wait(g_context.queue.OnSubmittedWorkDone(wgpu::CallbackMode::WaitAnyOnly,
                                                     [](wgpu::QueueWorkDoneStatus, wgpu::StringView) {}));
}

void Gfx::SetPipeline(const AbstractPipeline* pipeline)
{
  m_pipeline = static_cast<const Pipeline*>(pipeline);
}

void Gfx::SetFramebuffer(AbstractFramebuffer* framebuffer)
{
  if (m_framebuffer == framebuffer)
    return;
  // A clear asked for and never drawn over still has to happen.
  if (m_clear_pending)
    BeginPass();
  EndPass();
  m_framebuffer = static_cast<Framebuffer*>(framebuffer);
  m_current_framebuffer = framebuffer;
}

void Gfx::SetAndDiscardFramebuffer(AbstractFramebuffer* framebuffer)
{
  SetFramebuffer(framebuffer);
}

void Gfx::SetAndClearFramebuffer(AbstractFramebuffer* framebuffer, const ClearColor& color_value,
                                 float depth_value)
{
  SetFramebuffer(framebuffer);
  EndPass();
  m_clear_pending = true;
  m_clear_color = color_value;
  m_clear_depth = depth_value;
  BeginPass();
}

void Gfx::SetScissorRect(const MathUtil::Rectangle<int>& rc)
{
  m_scissor = rc;
  if (!m_pass || !m_framebuffer)
    return;
  // WebGPU rejects a scissor outside the attachment.
  const int w = static_cast<int>(m_framebuffer->GetWidth());
  const int h = static_cast<int>(m_framebuffer->GetHeight());
  const int left = std::clamp(rc.left, 0, w), top = std::clamp(rc.top, 0, h);
  const int right = std::clamp(rc.right, left, w), bottom = std::clamp(rc.bottom, top, h);
  m_pass.SetScissorRect(left, top, right - left, bottom - top);
}

void Gfx::SetViewport(float x, float y, float width, float height, float near_depth,
                      float far_depth)
{
  m_viewport[0] = x;
  m_viewport[1] = y;
  m_viewport[2] = width;
  m_viewport[3] = height;
  m_viewport[4] = near_depth;
  m_viewport[5] = far_depth;
  if (!m_pass)
    return;
  if (width <= 0 || height <= 0)
    return;
  m_pass.SetViewport(x, y, width, height, std::clamp(near_depth, 0.0f, 1.0f),
                     std::clamp(far_depth, 0.0f, 1.0f));
}

void Gfx::SetTexture(u32 index, const AbstractTexture* texture)
{
  if (index < m_textures.size())
    m_textures[index] = static_cast<const Texture*>(texture);
}

void Gfx::SetSamplerState(u32 index, const SamplerState& state)
{
  if (index < m_samplers.size())
    m_samplers[index] = state;
}

void Gfx::ForgetTexture(const Texture* texture)
{
  // Rare (the texture cache keeps its textures for reuse), so the whole
  // cache goes rather than a search of it.
  if (!m_texture_groups.empty())
  {
    m_texture_groups.clear();
    g_stats_wgpu.texture_group_clears++;
  }
  m_pass_state_set = false;
}

void Gfx::UnbindTexture(const AbstractTexture* texture)
{
  for (const Texture*& t : m_textures)
  {
    if (t == texture)
      t = nullptr;
  }
}

wgpu::Sampler Gfx::GetSampler(const SamplerState& state)
{
  auto it = m_sampler_cache.find(state.Hex());
  if (it != m_sampler_cache.end())
    return it->second;
  auto wrap = [](WrapMode mode) {
    switch (mode)
    {
    case WrapMode::Repeat:
      return wgpu::AddressMode::Repeat;
    case WrapMode::Mirror:
      return wgpu::AddressMode::MirrorRepeat;
    default:
      return wgpu::AddressMode::ClampToEdge;
    }
  };
  const bool linear_min = state.tm0.min_filter == FilterMode::Linear;
  const bool linear_mag = state.tm0.mag_filter == FilterMode::Linear;
  wgpu::SamplerDescriptor desc{};
  desc.addressModeU = wrap(state.tm0.wrap_u);
  desc.addressModeV = wrap(state.tm0.wrap_v);
  desc.addressModeW = wgpu::AddressMode::ClampToEdge;
  desc.minFilter = linear_min ? wgpu::FilterMode::Linear : wgpu::FilterMode::Nearest;
  desc.magFilter = linear_mag ? wgpu::FilterMode::Linear : wgpu::FilterMode::Nearest;
  desc.mipmapFilter = state.tm0.mipmap_filter == FilterMode::Linear ?
                          wgpu::MipmapFilterMode::Linear :
                          wgpu::MipmapFilterMode::Nearest;
  desc.lodMinClamp = state.tm1.min_lod / 16.0f;
  desc.lodMaxClamp = std::max(desc.lodMinClamp, state.tm1.max_lod / 16.0f);
  wgpu::Sampler sampler = g_context.device.CreateSampler(&desc);
  m_sampler_cache.emplace(state.Hex(), sampler);
  return sampler;
}

u64 Gfx::WriteUniforms(const void* data, u32 size)
{
  // Room for the whole window a binding covers, past the data itself.
  const u64 at = m_uniforms.Allocate(std::max<u64>(size, kUniformWindow), kUniformAlignment);
  m_uniforms.offset = at + Align(size, kUniformAlignment);
  m_uniforms.Write(at, data, Align(size, 4));
  return at;
}

void Gfx::SetGXUniforms(u32 slot, const void* data, u32 size)
{
  m_gx_uniform[slot] = WriteUniforms(data, size);
}

void Gfx::SetUtilityUniforms(const void* data, u32 size)
{
  m_utility_uniform = WriteUniforms(data, size);
}

bool Gfx::PrepareDraw()
{
  if (!m_pipeline || !m_framebuffer)
    return false;

  // The shared layout (WGPUContext.h): every texture slot, whether this
  // pipeline's shaders use it or not.
  std::array<const Texture*, 8> textures;
  TextureGroupKey key;
  for (u32 i = 0; i < 8; ++i)
  {
    const Texture* t = m_textures[i];
    // Every slot is bound, used or not, so one still holding a texture
    // this pass renders to would make the pass both write and sample it,
    // which WebGPU rejects. No shader samples its own target; such a
    // slot is stale.
    if (t && (t == m_framebuffer->GetColorAttachment() ||
              t == m_framebuffer->GetDepthAttachment()))
    {
      t = nullptr;
    }
    if (t && t->IsDepth())
    {
      // A float texture binding cannot take a depth texture; the depth
      // paths are M2's (docs/PHASE3-WEBGPU.md).
      return false;
    }
    textures[i] = t;
    key[i] = t ? t->GetId() : 0;
    key[8 + i] = m_samplers[i].Hex();
  }
  std::array<u32, 3> uniforms;
  for (u32 i = 0; i < 3; ++i)
    uniforms[i] = static_cast<u32>(m_pipeline->IsUtility() ? m_utility_uniform : m_gx_uniform[i]);

  BeginPass();
  if (!m_pass)
    return false;
  const bool fast = g_context.fast_path;
  const bool state_set = fast && m_pass_state_set;

  if (!fast || m_pass_pipeline != m_pipeline)
  {
    m_pass.SetPipeline(m_pipeline->GetPipeline());
    m_pass_pipeline = m_pipeline;
  }

  if (fast)
  {
    if (!state_set || uniforms != m_pass_uniforms)
    {
      m_pass.SetBindGroup(0, m_uniform_group, 3, uniforms.data());
      g_stats_wgpu.bind_group_sets++;
    }
  }
  else
  {
    wgpu::BindGroupEntry entries[3] = {};
    for (u32 i = 0; i < 3; ++i)
    {
      entries[i].binding = i;
      entries[i].buffer = m_uniforms.buffer;
      entries[i].offset = uniforms[i];
      entries[i].size = kUniformWindow;
    }
    wgpu::BindGroupDescriptor desc{};
    desc.layout = g_context.uniform_layout;
    desc.entryCount = 3;
    desc.entries = entries;
    const u32 zero[3] = {};
    m_pass.SetBindGroup(0, g_context.device.CreateBindGroup(&desc), 3, zero);
    g_stats_wgpu.bind_groups++;
    g_stats_wgpu.bind_group_sets++;
  }

  if (!state_set || key != m_pass_textures)
  {
    wgpu::BindGroup group;
    auto it = fast ? m_texture_groups.find(key) : m_texture_groups.end();
    if (it != m_texture_groups.end())
    {
      group = it->second;
    }
    else
    {
      wgpu::BindGroupEntry entries[16] = {};
      for (u32 i = 0; i < 8; ++i)
      {
        entries[i].binding = i;
        entries[i].textureView = (textures[i] ? textures[i] : m_null_texture.get())->GetSampleView();
        entries[8 + i].binding = kSamplerBase + i;
        entries[8 + i].sampler = GetSampler(m_samplers[i]);
      }
      wgpu::BindGroupDescriptor desc{};
      desc.layout = g_context.texture_layout;
      desc.entryCount = 16;
      desc.entries = entries;
      group = g_context.device.CreateBindGroup(&desc);
      g_stats_wgpu.bind_groups++;
      if (fast)
      {
        // Bounded: a group holds its textures alive.
        if (m_texture_groups.size() >= 4096)
        {
          m_texture_groups.clear();
          g_stats_wgpu.texture_group_clears++;
        }
        m_texture_groups.emplace(key, group);
      }
    }
    m_pass.SetBindGroup(1, group);
    g_stats_wgpu.bind_group_sets++;
  }

  if (!state_set)
  {
    m_pass.SetVertexBuffer(0, m_vertices.buffer);
    m_pass.SetIndexBuffer(m_indices.buffer, wgpu::IndexFormat::Uint16);
  }
  m_pass_uniforms = uniforms;
  m_pass_textures = key;
  m_pass_state_set = true;
  g_stats_wgpu.draws++;
  return true;
}

void Gfx::Draw(u32 base_vertex, u32 num_vertices)
{
  if (g_context.no_draws)
    return;
  if (PrepareDraw())
    m_pass.Draw(num_vertices, 1, base_vertex, 0);
}

void Gfx::DrawIndexed(u32 base_index, u32 num_indices, u32 base_vertex)
{
  if (g_context.no_draws)
    return;
  if (PrepareDraw())
    m_pass.DrawIndexed(num_indices, 1, base_index, static_cast<s32>(base_vertex), 0);
}

bool Gfx::BindBackbuffer(const ClearColor& clear_color)
{
  SetAndClearFramebuffer(m_backbuffer.get(), clear_color);
  return true;
}

void Gfx::PresentBackbuffer()
{
  EndPass();
  wgpu::SurfaceTexture current;
  if (!g_context.no_present)
    g_context.surface.GetCurrentTexture(&current);
  if (current.texture)
  {
    EnsureEncoder();
    wgpu::TexelCopyTextureInfo from{};
    from.texture = m_backbuffer_texture->GetTexture();
    wgpu::TexelCopyTextureInfo to{};
    to.texture = current.texture;
    const wgpu::Extent3D size = {g_context.surface_width, g_context.surface_height, 1};
    m_encoder.CopyTextureToTexture(&from, &to, &size);
  }
  Flush();
  // The frame reaches the canvas once this worker's task ends.
  const double t0 = emscripten_get_now();
  if (!g_context.no_present)
    YieldToBrowser();
  Stats& st = g_stats_wgpu;
  st.yield_ms += emscripten_get_now() - t0;
  if (++st.presents % 600 == 0)
  {
    const double f = 600.0;
    WGPU_LOG("stats/frame: maps {:.2f} ({:.3f} ms wait), submits {:.1f}, writeBuffer {:.0f} "
             "({:.0f} KB), bind groups {:.0f} made {:.0f} set ({:.2f} cache clears), draws {:.0f}, "
             "yield {:.3f} ms",
             st.maps / f, st.map_ms / f, st.submits / f, st.write_buffers / f,
             st.write_bytes / f / 1024, st.bind_groups / f, st.bind_group_sets / f,
             st.texture_group_clears / f, st.draws / f, st.yield_ms / f);
    const u64 presents = st.presents;
    st = {};
    st.presents = presents;
  }
}

SurfaceInfo Gfx::GetSurfaceInfo() const
{
  return {g_context.surface_width, g_context.surface_height, 1.0f,
          FromWGPU(g_context.surface_format)};
}
}  // namespace WebGPU
