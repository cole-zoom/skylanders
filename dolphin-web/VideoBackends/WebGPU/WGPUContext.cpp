// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "WGPUContext.h"

#include <vector>

#include <emscripten.h>

#include <cstdio>

#include "Common/Logging/Log.h"

namespace WebGPU
{
Context g_context;

EM_ASYNC_JS(void, WebGPUYieldOnce, (), {
  if (!globalThis.__wgpuYield) {
    const channel = new MessageChannel();
    const waiting = [];
    channel.port1.onmessage = () => waiting.shift()();
    globalThis.__wgpuYield = () => new Promise(r => {
      waiting.push(r);
      channel.port2.postMessage(0);
    });
  }
  await globalThis.__wgpuYield();
});

void YieldToBrowser()
{
  WebGPUYieldOnce();
}

bool Context::Wait(wgpu::Future future) const
{
  return instance.WaitAny(future, UINT64_MAX) == wgpu::WaitStatus::Success;
}

bool Context::Create()
{
  const wgpu::InstanceFeatureName instance_features[] = {wgpu::InstanceFeatureName::TimedWaitAny};
  wgpu::InstanceDescriptor idesc{};
  idesc.requiredFeatureCount = 1;
  idesc.requiredFeatures = instance_features;
  instance = wgpu::CreateInstance(&idesc);
  std::printf("  webgpu    instance %d\n", instance ? 1 : 0);
  if (!instance)
    return false;

  wgpu::RequestAdapterOptions aopts{};
  aopts.powerPreference = wgpu::PowerPreference::HighPerformance;
  Wait(instance.RequestAdapter(
      &aopts, wgpu::CallbackMode::WaitAnyOnly,
      [&](wgpu::RequestAdapterStatus status, wgpu::Adapter a, wgpu::StringView message) {
        if (status == wgpu::RequestAdapterStatus::Success)
          adapter = std::move(a);
        else
          WGPU_LOG("WebGPU: no adapter: {}",
                        std::string_view(message.data, message.length));
      }));
  std::printf("  webgpu    adapter %d\n", adapter ? 1 : 0);
  if (!adapter)
    return false;

  // What the backend relies on (spikes/webgpu-shaders/FINDINGS.md): BC
  // textures for CMPR, dual-source blending for the destination-alpha
  // pass, clip distances for 50 of the game's vertex shaders. 16-bit
  // normalised formats when present, for R16.
  std::vector<wgpu::FeatureName> features;
  for (wgpu::FeatureName f :
       {wgpu::FeatureName::TextureCompressionBC, wgpu::FeatureName::DualSourceBlending,
        wgpu::FeatureName::ClipDistances, wgpu::FeatureName::Unorm16TextureFormats})
  {
    if (adapter.HasFeature(f))
      features.push_back(f);
    else
      WGPU_LOG("WebGPU: adapter lacks feature {}", static_cast<u32>(f));
  }
  has_unorm16 = adapter.HasFeature(wgpu::FeatureName::Unorm16TextureFormats);

  wgpu::DeviceDescriptor ddesc{};
  ddesc.requiredFeatureCount = features.size();
  ddesc.requiredFeatures = features.data();
  ddesc.SetUncapturedErrorCallback(
      [](const wgpu::Device&, wgpu::ErrorType type, wgpu::StringView message) {
        WGPU_LOG("WebGPU error {}: {}", static_cast<u32>(type),
                      std::string_view(message.data, message.length));
      });
  ddesc.SetDeviceLostCallback(
      wgpu::CallbackMode::AllowSpontaneous,
      [](const wgpu::Device&, wgpu::DeviceLostReason reason, wgpu::StringView message) {
        WGPU_LOG("WebGPU device lost ({}): {}", static_cast<u32>(reason),
                      std::string_view(message.data, message.length));
      });
  Wait(adapter.RequestDevice(
      &ddesc, wgpu::CallbackMode::WaitAnyOnly,
      [&](wgpu::RequestDeviceStatus status, wgpu::Device d, wgpu::StringView message) {
        if (status == wgpu::RequestDeviceStatus::Success)
          device = std::move(d);
        else
          WGPU_LOG("WebGPU: no device: {}",
                        std::string_view(message.data, message.length));
      }));
  std::printf("  webgpu    device %d\n", device ? 1 : 0);
  if (!device)
    return false;
  queue = device.GetQueue();

  {
    wgpu::BindGroupLayoutEntry entries[3] = {};
    for (u32 i = 0; i < 3; ++i)
    {
      entries[i].binding = i;
      entries[i].visibility = wgpu::ShaderStage::Vertex | wgpu::ShaderStage::Fragment;
      entries[i].buffer.type = wgpu::BufferBindingType::Uniform;
    }
    wgpu::BindGroupLayoutDescriptor desc{};
    desc.entryCount = 3;
    desc.entries = entries;
    uniform_layout = device.CreateBindGroupLayout(&desc);
  }
  {
    wgpu::BindGroupLayoutEntry entries[16] = {};
    for (u32 i = 0; i < 8; ++i)
    {
      entries[i].binding = i;
      entries[i].visibility = wgpu::ShaderStage::Vertex | wgpu::ShaderStage::Fragment;
      entries[i].texture.sampleType = wgpu::TextureSampleType::Float;
      entries[i].texture.viewDimension = wgpu::TextureViewDimension::e2DArray;
      entries[8 + i].binding = 32 + i;
      entries[8 + i].visibility = wgpu::ShaderStage::Vertex | wgpu::ShaderStage::Fragment;
      entries[8 + i].sampler.type = wgpu::SamplerBindingType::Filtering;
    }
    wgpu::BindGroupLayoutDescriptor desc{};
    desc.entryCount = 16;
    desc.entries = entries;
    texture_layout = device.CreateBindGroupLayout(&desc);
  }
  {
    const wgpu::BindGroupLayout layouts[2] = {uniform_layout, texture_layout};
    wgpu::PipelineLayoutDescriptor desc{};
    desc.bindGroupLayoutCount = 2;
    desc.bindGroupLayouts = layouts;
    pipeline_layout = device.CreatePipelineLayout(&desc);
  }

  // The canvas the page handed this thread at creation.
  wgpu::EmscriptenSurfaceSourceCanvasHTMLSelector source{};
  source.selector = "#canvas";
  wgpu::SurfaceDescriptor sdesc{};
  sdesc.nextInChain = &source;
  surface = instance.CreateSurface(&sdesc);
  std::printf("  webgpu    surface %d\n", surface ? 1 : 0);
  if (!surface)
    return false;
  wgpu::SurfaceCapabilities caps;
  surface.GetCapabilities(adapter, &caps);
  surface_format = caps.formats[0];
  surface_width = 640;
  surface_height = 528;
  wgpu::SurfaceConfiguration config{};
  config.device = device;
  config.format = surface_format;
  config.usage = wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::CopyDst;
  config.width = surface_width;
  config.height = surface_height;
  surface.Configure(&config);
  WGPU_LOG("WebGPU: device up, surface format {}", static_cast<u32>(surface_format));
  return true;
}
}  // namespace WebGPU
