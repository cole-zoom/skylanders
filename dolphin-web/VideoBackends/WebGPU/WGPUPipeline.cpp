// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "WGPUPipeline.h"

#include <algorithm>

#include "Common/Logging/Log.h"
#include "VideoCommon/VertexShaderGen.h"
#include "VideoCommon/VideoConfig.h"

#include "WGPUContext.h"
#include "WGPUTexture.h"

namespace WebGPU
{
namespace
{
// WebGPU has no three-component 8- or 16-bit vertex formats; the
// four-component one reads one more component, which the shader ignores.
wgpu::VertexFormat Convert(ComponentFormat type, int count, bool integer)
{
  const int n = count == 3 ? 4 : count;
  using VF = wgpu::VertexFormat;
  switch (type)
  {
  case ComponentFormat::UByte:
    return integer ? (n == 1 ? VF::Uint8 : n == 2 ? VF::Uint8x2 : VF::Uint8x4) :
                     (n == 1 ? VF::Unorm8 : n == 2 ? VF::Unorm8x2 : VF::Unorm8x4);
  case ComponentFormat::Byte:
    return integer ? (n == 1 ? VF::Sint8 : n == 2 ? VF::Sint8x2 : VF::Sint8x4) :
                     (n == 1 ? VF::Snorm8 : n == 2 ? VF::Snorm8x2 : VF::Snorm8x4);
  case ComponentFormat::UShort:
    return integer ? (n == 1 ? VF::Uint16 : n == 2 ? VF::Uint16x2 : VF::Uint16x4) :
                     (n == 1 ? VF::Unorm16 : n == 2 ? VF::Unorm16x2 : VF::Unorm16x4);
  case ComponentFormat::Short:
    return integer ? (n == 1 ? VF::Sint16 : n == 2 ? VF::Sint16x2 : VF::Sint16x4) :
                     (n == 1 ? VF::Snorm16 : n == 2 ? VF::Snorm16x2 : VF::Snorm16x4);
  default:  // Float and the invalid formats treated as float
    return count == 1 ? VF::Float32 :
           count == 2 ? VF::Float32x2 :
           count == 3 ? VF::Float32x3 :
                        VF::Float32x4;
  }
}

wgpu::CompareFunction Convert(CompareMode mode)
{
  // As the Metal backend: without a reversed depth range the comparisons
  // are inverted.
  const bool invert = !g_backend_info.bSupportsReversedDepthRange;
  switch (mode)
  {
  case CompareMode::Never:
    return wgpu::CompareFunction::Never;
  case CompareMode::Less:
    return invert ? wgpu::CompareFunction::Greater : wgpu::CompareFunction::Less;
  case CompareMode::Equal:
    return wgpu::CompareFunction::Equal;
  case CompareMode::LEqual:
    return invert ? wgpu::CompareFunction::GreaterEqual : wgpu::CompareFunction::LessEqual;
  case CompareMode::Greater:
    return invert ? wgpu::CompareFunction::Less : wgpu::CompareFunction::Greater;
  case CompareMode::NEqual:
    return wgpu::CompareFunction::NotEqual;
  case CompareMode::GEqual:
    return invert ? wgpu::CompareFunction::LessEqual : wgpu::CompareFunction::GreaterEqual;
  case CompareMode::Always:
  default:
    return wgpu::CompareFunction::Always;
  }
}

wgpu::BlendFactor Convert(SrcBlendFactor f, bool dual)
{
  switch (f)
  {
  case SrcBlendFactor::Zero:
    return wgpu::BlendFactor::Zero;
  case SrcBlendFactor::One:
    return wgpu::BlendFactor::One;
  case SrcBlendFactor::DstClr:
    return wgpu::BlendFactor::Dst;
  case SrcBlendFactor::InvDstClr:
    return wgpu::BlendFactor::OneMinusDst;
  case SrcBlendFactor::SrcAlpha:
    return dual ? wgpu::BlendFactor::Src1Alpha : wgpu::BlendFactor::SrcAlpha;
  case SrcBlendFactor::InvSrcAlpha:
    return dual ? wgpu::BlendFactor::OneMinusSrc1Alpha : wgpu::BlendFactor::OneMinusSrcAlpha;
  case SrcBlendFactor::DstAlpha:
    return wgpu::BlendFactor::DstAlpha;
  case SrcBlendFactor::InvDstAlpha:
  default:
    return wgpu::BlendFactor::OneMinusDstAlpha;
  }
}

wgpu::BlendFactor Convert(DstBlendFactor f, bool dual)
{
  switch (f)
  {
  case DstBlendFactor::Zero:
    return wgpu::BlendFactor::Zero;
  case DstBlendFactor::One:
    return wgpu::BlendFactor::One;
  case DstBlendFactor::SrcClr:
    return wgpu::BlendFactor::Src;
  case DstBlendFactor::InvSrcClr:
    return wgpu::BlendFactor::OneMinusSrc;
  case DstBlendFactor::SrcAlpha:
    return dual ? wgpu::BlendFactor::Src1Alpha : wgpu::BlendFactor::SrcAlpha;
  case DstBlendFactor::InvSrcAlpha:
    return dual ? wgpu::BlendFactor::OneMinusSrc1Alpha : wgpu::BlendFactor::OneMinusSrcAlpha;
  case DstBlendFactor::DstAlpha:
    return wgpu::BlendFactor::DstAlpha;
  case DstBlendFactor::InvDstAlpha:
  default:
    return wgpu::BlendFactor::OneMinusDstAlpha;
  }
}
}  // namespace

VertexFormat::VertexFormat(const PortableVertexDeclaration& decl) : NativeVertexFormat(decl)
{
  auto add = [&](ShaderAttrib location, const AttributeFormat& format) {
    if (!format.enable)
      return;
    m_attributes.push_back({.format = Convert(format.type, format.components, format.integer),
                            .offset = static_cast<u64>(format.offset),
                            .shaderLocation = static_cast<u32>(location)});
  };
  add(ShaderAttrib::Position, decl.position);
  for (u32 i = 0; i < decl.normals.size(); ++i)
    add(ShaderAttrib::Normal + i, decl.normals[i]);
  for (u32 i = 0; i < decl.colors.size(); ++i)
    add(ShaderAttrib::Color0 + i, decl.colors[i]);
  for (u32 i = 0; i < decl.texcoords.size(); ++i)
    add(ShaderAttrib::TexCoord0 + i, decl.texcoords[i]);
  add(ShaderAttrib::PositionMatrix, decl.posmtx);
  m_layout.stepMode = wgpu::VertexStepMode::Vertex;
  m_layout.arrayStride = static_cast<u64>(decl.stride);
  m_layout.attributeCount = m_attributes.size();
  m_layout.attributes = m_attributes.data();
}

Pipeline::Pipeline(const AbstractPipelineConfig& config, wgpu::RenderPipeline pipeline,
                   std::vector<Binding> bindings)
    : AbstractPipeline(config), m_pipeline(std::move(pipeline)), m_bindings(std::move(bindings))
{
}

Pipeline::~Pipeline() = default;

std::unique_ptr<Pipeline> Pipeline::Create(const AbstractPipelineConfig& config)
{
  const CompileTimer timer;
  const Shader* vs = static_cast<const Shader*>(config.vertex_shader);
  const Shader* ps = static_cast<const Shader*>(config.pixel_shader);
  if (!vs)
    return nullptr;

  // The shared layout covers groups 0 and 1; storage buffers and compute
  // images (groups 2 and 3) belong to paths this backend does not report.
  for (const Shader* s : {vs, ps})
  {
    if (!s)
      continue;
    for (const Binding& b : s->GetBindings())
    {
      if (b.group > 1)
      {
        WGPU_LOG("WebGPU: pipeline needs group {} binding {}; not supported yet", b.group,
                 b.binding);
        return nullptr;
      }
    }
  }

  wgpu::RenderPipelineDescriptor desc{};
  desc.layout = g_context.pipeline_layout;
  desc.vertex.module = vs->GetModule();
  desc.vertex.entryPoint = "main";
  if (config.vertex_format)
  {
    desc.vertex.bufferCount = 1;
    desc.vertex.buffers = &static_cast<const VertexFormat*>(config.vertex_format)->GetLayout();
  }

  switch (config.rasterization_state.primitive)
  {
  case PrimitiveType::Points:
    desc.primitive.topology = wgpu::PrimitiveTopology::PointList;
    break;
  case PrimitiveType::Lines:
    desc.primitive.topology = wgpu::PrimitiveTopology::LineList;
    break;
  case PrimitiveType::Triangles:
    desc.primitive.topology = wgpu::PrimitiveTopology::TriangleList;
    break;
  case PrimitiveType::TriangleStrip:
    // Restart on 0xFFFF, which IndexGenerator writes between strips.
    desc.primitive.topology = wgpu::PrimitiveTopology::TriangleStrip;
    desc.primitive.stripIndexFormat = wgpu::IndexFormat::Uint16;
    break;
  }
  // Clockwise front faces, as Metal's default winding.
  desc.primitive.frontFace = wgpu::FrontFace::CW;
  switch (config.rasterization_state.cull_mode)
  {
  case CullMode::Back:
    desc.primitive.cullMode = wgpu::CullMode::Back;
    break;
  case CullMode::Front:
    desc.primitive.cullMode = wgpu::CullMode::Front;
    break;
  default:  // None, and All, which VertexLoaderManager handles by not drawing
    desc.primitive.cullMode = wgpu::CullMode::None;
    break;
  }

  wgpu::DepthStencilState depth{};
  const AbstractTextureFormat depth_format = config.framebuffer_state.depth_texture_format;
  if (depth_format != AbstractTextureFormat::Undefined)
  {
    depth.format = ToWGPU(depth_format);
    depth.depthWriteEnabled =
        config.depth_state.test_enable && config.depth_state.update_enable ?
            wgpu::OptionalBool::True :
            wgpu::OptionalBool::False;
    depth.depthCompare = config.depth_state.test_enable ? Convert(config.depth_state.func) :
                                                          wgpu::CompareFunction::Always;
    desc.depthStencil = &depth;
  }

  wgpu::FragmentState fragment{};
  std::vector<wgpu::ColorTargetState> targets;
  wgpu::BlendState blend{};
  const BlendingState& bs = config.blending_state;
  if (ps)
  {
    fragment.module = ps->GetModule();
    fragment.entryPoint = "main";
    const AbstractTextureFormat color_format = config.framebuffer_state.color_texture_format;
    const u32 count = (color_format != AbstractTextureFormat::Undefined ? 1 : 0) +
                      config.framebuffer_state.additional_color_attachment_count;
    if (bs.blend_enable)
    {
      const bool dual = bs.use_dual_src;
      blend.color = {bs.subtract ? wgpu::BlendOperation::ReverseSubtract : wgpu::BlendOperation::Add,
                     Convert(bs.src_factor, dual), Convert(bs.dst_factor, dual)};
      blend.alpha = {bs.subtract_alpha ? wgpu::BlendOperation::ReverseSubtract :
                                         wgpu::BlendOperation::Add,
                     Convert(bs.src_factor_alpha, dual), Convert(bs.dst_factor_alpha, dual)};
    }
    wgpu::ColorWriteMask mask = wgpu::ColorWriteMask::None;
    if (bs.color_update)
      mask = mask | wgpu::ColorWriteMask::Red | wgpu::ColorWriteMask::Green |
             wgpu::ColorWriteMask::Blue;
    if (bs.alpha_update)
      mask = mask | wgpu::ColorWriteMask::Alpha;
    for (u32 i = 0; i < count; ++i)
    {
      wgpu::ColorTargetState target{};
      target.format = ToWGPU(color_format);
      target.blend = bs.blend_enable ? &blend : nullptr;
      target.writeMask = mask;
      targets.push_back(target);
    }
    fragment.targetCount = targets.size();
    fragment.targets = targets.data();
    desc.fragment = &fragment;
  }

  wgpu::RenderPipeline pipeline;
  std::optional<CompileTimer> phase(kPipeline);
  const std::string error = g_context.CaptureValidation(
      [&] { pipeline = g_context.device.CreateRenderPipeline(&desc); });
  phase.reset();
  if (!error.empty())
  {
    WGPU_LOG("WebGPU: pipeline rejected: {}", error);
    return nullptr;
  }

  std::vector<Binding> bindings = vs->GetBindings();
  if (ps)
  {
    for (const Binding& b : ps->GetBindings())
    {
      if (std::none_of(bindings.begin(), bindings.end(), [&](const Binding& have) {
            return have.group == b.group && have.binding == b.binding;
          }))
      {
        bindings.push_back(b);
      }
    }
  }
  return std::make_unique<Pipeline>(config, std::move(pipeline), std::move(bindings));
}
}  // namespace WebGPU
