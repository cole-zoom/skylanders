// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <webgpu/webgpu_cpp.h>

#include "VideoCommon/AbstractShader.h"

namespace WebGPU
{
// A resource a WGSL module declares, as Tint wrote it.
struct Binding
{
  enum class Kind
  {
    Uniform,
    Texture,
    Sampler,
    Storage,
    Other,
  };
  u32 group;
  u32 binding;
  Kind kind;
};

// The binding layout the shaders end up with -- Metal's dialect, as
// glslang and Tint carry it into WGSL:
//
//   group 0  uniform blocks: 0 pixel, 1 vertex, 2 geometry/utility
//   group 1  textures 0-7, and the samplers Tint splits out of the GLSL
//            combined samplers, moved to kSamplerBase + i
//   group 2  storage buffers
//   group 3  compute images
constexpr u32 kSamplerBase = 32;
constexpr u32 kNumTextures = 8;

// Starts every cached WGSL. Dolphin's cache files carry Dolphin's revision,
// not this backend's: bump the number whenever the WGSL a shader becomes
// changes (kHeader, RewriteForWebGPU, Tint's options, a Tint update), or
// players keep the old translation.
constexpr std::string_view kBinaryTag = "dolphin-web wgsl 1\n";

class Shader final : public AbstractShader
{
public:
  Shader(ShaderStage stage, wgpu::ShaderModule module, std::vector<Binding> bindings,
         std::string wgsl);
  ~Shader() override;

  const wgpu::ShaderModule& GetModule() const { return m_module; }
  const std::vector<Binding>& GetBindings() const { return m_bindings; }
  // The WGSL, after kBinaryTag: Dolphin keeps it in its shader cache
  // (Cache/Shaders, which the page keeps in IndexedDB), so a shader met in
  // an earlier session skips glslang and Tint.
  BinaryData GetBinary() const override;

  // GLSL from VideoCommon's generators to a WebGPU module: glslang to
  // SPIR-V (VideoCommon/Spirv.cpp, as the Metal backend does), Tint to
  // WGSL, then the browser's compiler. Null, with the reason logged, if
  // any of the three refuses it.
  static std::unique_ptr<Shader> Compile(ShaderStage stage, std::string_view source,
                                         std::string_view name);
  // A GetBinary from an earlier session; null if its tag is not this
  // build's, and the shader is compiled from source instead.
  static std::unique_ptr<Shader> FromBinary(ShaderStage stage, const void* data, size_t size,
                                            std::string_view name);

private:
  // The browser's module from the WGSL, and the bindings it declares.
  static std::unique_ptr<Shader> FromWgsl(ShaderStage stage, std::string wgsl,
                                          std::string_view name);

  wgpu::ShaderModule m_module;
  std::vector<Binding> m_bindings;
  std::string m_wgsl;
};
}  // namespace WebGPU
