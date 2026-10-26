// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "WGPUShader.h"

#include <cstdlib>

#include "src/tint/api/tint.h"
#include "src/tint/lang/core/ir/module.h"
#include "src/tint/lang/spirv/reader/reader.h"
#include "src/tint/lang/wgsl/program/program.h"
#include "src/tint/lang/wgsl/writer/writer.h"

#include "Common/Logging/Log.h"
#include "VideoCommon/Spirv.h"

#include "WGPUContext.h"

namespace WebGPU
{
namespace
{
// The Metal backend's SHADER_HEADER (VideoBackends/Metal/MTLUtil.mm) with
// what WGSL cannot take removed: the 8- and 16-bit integer extensions,
// and forced early fragment tests, which this backend reports it lacks
// (bSupportsEarlyZ = false) -- the macro stays, as a harmless declaration,
// because the generators emit it as a statement.
constexpr std::string_view kHeader = R"(
#version 450 core
#define ATTRIBUTE_LOCATION(x) layout(location = x)
#define FRAGMENT_OUTPUT_LOCATION(x) layout(location = x)
#define FRAGMENT_OUTPUT_LOCATION_INDEXED(x, y) layout(location = x, index = y)
#define UBO_BINDING(packing, x) layout(packing, set = 0, binding = (x - 1))
#define SAMPLER_BINDING(x) layout(set = 1, binding = x)
#define TEXEL_BUFFER_BINDING(x) layout(set = 1, binding = (x + 8))
#define SSBO_BINDING(x) layout(std430, set = 2, binding = x)
#define INPUT_ATTACHMENT_BINDING(x, y, z) layout(set = x, binding = y, input_attachment_index = z)
#define VARYING_LOCATION(x) layout(location = x)
#define FORCE_EARLY_Z const int dolphin_no_forced_early_z = 0
#define FB_FETCH_VALUE subpassLoad(in_ocol0)
#define API_METAL 1
#define float2 vec2
#define float3 vec3
#define float4 vec4
#define uint2 uvec2
#define uint3 uvec3
#define uint4 uvec4
#define int2 ivec2
#define int3 ivec3
#define int4 ivec4
#define frac fract
#define lerp mix
#define gl_VertexID gl_VertexIndex
#define gl_InstanceID gl_InstanceIndex
)";

constexpr std::string_view kComputeHeader = R"(
#version 450 core
#define UBO_BINDING(packing, x) layout(packing, set = 0, binding = (x - 1))
#define SAMPLER_BINDING(x) layout(set = 1, binding = x)
#define SSBO_BINDING(x) layout(std430, set = 2, binding = x)
#define IMAGE_BINDING(format, x) layout(format, set = 3, binding = x)
#define API_METAL 1
#define float2 vec2
#define float3 vec3
#define float4 vec4
#define uint2 uvec2
#define uint3 uvec3
#define uint4 uvec4
#define int2 ivec2
#define int3 ivec3
#define int4 ivec4
#define frac fract
#define lerp mix
)";

void Replace(std::string& s, std::string_view from, std::string_view to)
{
  for (size_t at = s.find(from); at != std::string::npos; at = s.find(from, at + to.size()))
    s.replace(at, from.size(), to);
}

// What VideoCommon's generators will emit for WebGPU, applied to their
// text until they do (docs/PHASE3-WEBGPU.md M2; the same rewrites the S2
// spike measured, spikes/webgpu-shaders/glsl2spv.cpp):
//
//   isnan -> a bit test. Tint's SPIR-V reader has no OpIsNan and does not
//   fail on it gracefully: it aborts, which takes the GPU thread down.
//
//   samp[8] -> samp0..samp7. Core WebGPU has no arrays of textures. The one
//   indexing site, sampleTextureWrapper, is always called with a literal,
//   so a switch over the eight folds away.
std::string RewriteForWebGPU(std::string s)
{
  Replace(s, "#define dolphin_isnan(f) isnan(f)\n",
          "bool dolphin_isnan(float f) { return (floatBitsToInt(f) & 0x7FFFFFFF) > 0x7F800000; }\n");
  if (s.find("uniform sampler2DArray samp[8];") != std::string::npos)
  {
    std::string decls;
    for (int i = 0; i < 8; ++i)
    {
      decls += "SAMPLER_BINDING(" + std::to_string(i) + ") uniform sampler2DArray samp" +
               std::to_string(i) + ";\n";
    }
    Replace(s, "SAMPLER_BINDING(0) uniform sampler2DArray samp[8];\n", decls);
    std::string wrapper = "int4 sampleTextureWrapper(uint texmap, int2 uv, int layer) {\n"
                          "  switch (texmap) {\n";
    for (int i = 0; i < 8; ++i)
    {
      wrapper += "  case " + std::to_string(i) + "u: return sampleTexture(texmap, samp" +
                 std::to_string(i) + ", uv, layer);\n";
    }
    wrapper += "  }\n  return int4(0);\n}\n";
    Replace(s,
            "#define sampleTextureWrapper(texmap, uv, layer) sampleTexture(texmap, samp[texmap], "
            "uv, layer)\n",
            wrapper);
  }
  return s;
}

// The resources a WGSL module declares, read from Tint's own text:
//   @group(1u) @binding(2u) var x_5 : texture_2d_array<f32>;
// Tint writes every declaration on one line in this form.
std::vector<Binding> ParseBindings(const std::string& wgsl)
{
  std::vector<Binding> out;
  size_t at = 0;
  while ((at = wgsl.find("@group(", at)) != std::string::npos)
  {
    const size_t line_end = wgsl.find('\n', at);
    const std::string line = wgsl.substr(at, line_end - at);
    at = line_end;
    const u32 group = static_cast<u32>(std::strtoul(line.c_str() + 7, nullptr, 10));
    const size_t b = line.find("@binding(");
    if (b == std::string::npos)
      continue;
    const u32 binding = static_cast<u32>(std::strtoul(line.c_str() + b + 9, nullptr, 10));
    Binding::Kind kind = Binding::Kind::Other;
    if (line.find("var<uniform>") != std::string::npos)
      kind = Binding::Kind::Uniform;
    else if (line.find("var<storage") != std::string::npos)
      kind = Binding::Kind::Storage;
    else if (line.find(": sampler") != std::string::npos)
      kind = Binding::Kind::Sampler;
    else if (line.find(": texture") != std::string::npos)
      kind = Binding::Kind::Texture;
    out.push_back({group, binding, kind});
  }
  return out;
}
}  // namespace

Shader::Shader(ShaderStage stage, wgpu::ShaderModule module, std::vector<Binding> bindings,
               std::string wgsl)
    : AbstractShader(stage), m_module(std::move(module)), m_bindings(std::move(bindings)),
      m_wgsl(std::move(wgsl))
{
}

Shader::~Shader() = default;

std::unique_ptr<Shader> Shader::Compile(ShaderStage stage, std::string_view source,
                                        std::string_view name)
{
  if (stage == ShaderStage::Geometry)
    return nullptr;
  const CompileTimer timer;
  std::string full(stage == ShaderStage::Compute ? kComputeHeader : kHeader);
  full.append(RewriteForWebGPU(std::string(source)));
  // Tint aborts, rather than failing, on a SPIR-V instruction it does not
  // implement. isnan was the one the game's shaders hit; refuse anything
  // that still calls it rather than lose the thread.
  if (full.find("isnan(") != std::string::npos &&
      full.find("bool dolphin_isnan") == std::string::npos)
  {
    WGPU_LOG("WebGPU: {} still calls isnan; not compiled", name);
    return nullptr;
  }

  std::optional<SPIRV::CodeVector> spv;
  std::optional<CompileTimer> phase(kGlslang);
  switch (stage)
  {
  case ShaderStage::Vertex:
    spv = SPIRV::CompileVertexShader(full, APIType::Metal, glslang::EShTargetSpv_1_3, nullptr);
    break;
  case ShaderStage::Pixel:
    spv = SPIRV::CompileFragmentShader(full, APIType::Metal, glslang::EShTargetSpv_1_3, nullptr);
    break;
  case ShaderStage::Compute:
    spv = SPIRV::CompileComputeShader(full, APIType::Metal, glslang::EShTargetSpv_1_3, nullptr);
    break;
  default:
    break;
  }
  phase.reset();
  if (!spv)
    return nullptr;

  phase.emplace(kTint);
  static bool tint_initialized = false;
  if (!tint_initialized)
  {
    tint::Initialize();
    tint_initialized = true;
  }
  tint::spirv::reader::Options ropts;
  for (u32 i = 0; i < 16; ++i)
    ropts.sampler_mappings[tint::BindingPoint{1, i}] = tint::BindingPoint{1, kSamplerBase + i};
  auto ir = tint::spirv::reader::ReadIR(std::vector<uint32_t>(spv->begin(), spv->end()), ropts);
  if (ir != tint::Success)
  {
    WGPU_LOG("WebGPU: Tint rejected {}: {}", name, ir.Failure().reason);
    return nullptr;
  }
  tint::wgsl::writer::Options wopts;
  wopts.allowed_features = tint::wgsl::AllowedFeatures::Everything();
  auto program = tint::wgsl::writer::ProgramFromIR(ir.Get(), wopts);
  if (program != tint::Success)
  {
    WGPU_LOG("WebGPU: Tint could not lower {}: {}", name, program.Failure().reason);
    return nullptr;
  }
  auto out = tint::wgsl::writer::Generate(program.Get(), wopts);
  if (out != tint::Success)
  {
    WGPU_LOG("WebGPU: Tint could not write {}: {}", name, out.Failure().reason);
    return nullptr;
  }
  std::string wgsl = std::move(out->wgsl);
  phase.emplace(kModule);

  wgpu::ShaderSourceWGSL wsrc{};
  wsrc.code = {wgsl.data(), wgsl.size()};
  wgpu::ShaderModuleDescriptor mdesc{};
  mdesc.nextInChain = &wsrc;
  wgpu::ShaderModule module;
  const std::string error =
      g_context.CaptureValidation([&] { module = g_context.device.CreateShaderModule(&mdesc); });
  if (!error.empty())
  {
    WGPU_LOG("WebGPU: the browser rejected {}: {}", name, error);
    return nullptr;
  }
  phase.reset();
  std::vector<Binding> bindings = ParseBindings(wgsl);
  return std::make_unique<Shader>(stage, std::move(module), std::move(bindings), std::move(wgsl));
}
}  // namespace WebGPU
