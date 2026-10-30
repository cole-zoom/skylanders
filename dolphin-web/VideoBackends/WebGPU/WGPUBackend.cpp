// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "WGPUBackend.h"

#include <span>
#include <vector>

#include "Common/Logging/Log.h"
#include "VideoCommon/BoundingBox.h"
#include "VideoCommon/PerfQueryBase.h"
#include "VideoCommon/VideoConfig.h"

#include "WGPUContext.h"
#include "WGPUGfx.h"
#include "WGPUVertexManager.h"

namespace WebGPU
{
namespace
{
// Occlusion queries and the bounding box are not implemented; the backend
// reports bSupportsBBox = false, and a query reads zero.
class PerfQuery final : public PerfQueryBase
{
public:
  void EnableQuery(PerfQueryGroup) override {}
  void DisableQuery(PerfQueryGroup) override {}
  void ResetQuery() override {}
  u32 GetQueryResult(PerfQueryType) override { return 0; }
  void FlushResults() override {}
  bool IsFlushed() const override { return true; }
};

class NoBoundingBox final : public BoundingBox
{
public:
  bool Initialize() override { return true; }

protected:
  std::vector<BBoxType> Read(u32, u32 length) override { return std::vector<BBoxType>(length); }
  void Write(u32, std::span<const BBoxType>) override {}
};
}  // namespace

void VideoBackend::InitBackendInfo(const WindowSystemInfo&)
{
  // Metal's (VideoBackends/Metal/MTLUtil.mm), where WebGPU has the same;
  // less where it has less (spikes/webgpu-shaders/FINDINGS.md): no
  // geometry shaders, logic ops, framebuffer fetch, forced early-Z or
  // dynamic sampler indexing. And, for M1 (docs/PHASE3-WEBGPU.md), not yet
  // the paths that need storage or texel buffers: GPU texture decoding,
  // palette conversion, the dynamic vertex loader, line and point
  // expansion, the bounding box.
  BackendInfo& bi = g_backend_info;
  bi.api_type = APIType::Metal;  // the GLSL dialect the shaders start in
  bi.MaxTextureSize = 8192;
  bi.bUsesLowerLeftOrigin = false;
  bi.bSupportsExclusiveFullscreen = false;
  bi.bSupportsDualSourceBlend = true;
  bi.bSupportsPrimitiveRestart = true;
  bi.bSupportsGeometryShaders = false;
  bi.bSupportsComputeShaders = false;
  bi.bSupports3DVision = false;
  bi.bSupportsEarlyZ = false;
  bi.bSupportsBindingLayout = true;
  bi.bSupportsBBox = false;
  bi.bSupportsGSInstancing = false;
  bi.bSupportsPostProcessing = false;
  bi.bSupportsPaletteConversion = false;
  bi.bSupportsClipControl = true;
  bi.bSupportsSSAA = false;
  bi.bSupportsFragmentStoresAndAtomics = false;
  bi.bSupportsReversedDepthRange = false;
  bi.bSupportsLogicOp = false;
  bi.bSupportsMultithreading = false;
  bi.bSupportsGPUTextureDecoding = false;
  bi.bSupportsST3CTextures = true;
  bi.bSupportsBPTCTextures = true;
  bi.bSupportsCopyToVram = true;
  bi.bSupportsBitfield = true;
  bi.bSupportsDynamicSamplerIndexing = false;
  bi.bSupportsFramebufferFetch = false;
  bi.bSupportsBackgroundCompiling = false;
  bi.bSupportsLargePoints = false;
  bi.bSupportsPartialDepthCopies = false;
  bi.bSupportsDepthReadback = true;
  bi.bSupportsShaderBinaries = true;  // the WGSL; see Shader::GetBinary
  bi.bSupportsPipelineCacheData = false;
  bi.bSupportsCoarseDerivatives = false;
  bi.bSupportsTextureQueryLevels = true;
  bi.bSupportsLodBiasInSampler = false;
  bi.bSupportsSettingObjectNames = false;
  bi.bSupportsPartialMultisampleResolve = false;
  bi.bSupportsDynamicVertexLoader = false;
  bi.bSupportsVSLinePointExpand = false;
  bi.bSupportsDepthClamp = false;
  bi.bSupportsHDROutput = false;
  bi.Adapters.clear();
  bi.AAModes = {1};
}

bool VideoBackend::Initialize(const WindowSystemInfo&)
{
  // On the GPU thread: everything WebGPU is created here and stays here.
  if (!g_context.Create())
  {
    WGPU_LOG("WebGPU: no device");
    return false;
  }
  return InitializeShared(std::make_unique<Gfx>(), std::make_unique<VertexManager>(),
                          std::make_unique<PerfQuery>(), std::make_unique<NoBoundingBox>());
}

void VideoBackend::Shutdown()
{
  ShutdownShared();
  g_context = {};
}
}  // namespace WebGPU
