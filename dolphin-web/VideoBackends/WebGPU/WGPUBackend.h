// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>

#include "VideoCommon/VideoBackendBase.h"

namespace WebGPU
{
// Dolphin's video backend for the browser's WebGPU, modelled on
// VideoBackends/Metal. Plan and evidence: docs/PHASE3-WEBGPU.md.
class VideoBackend final : public VideoBackendBase
{
public:
  bool Initialize(const WindowSystemInfo& wsi) override;
  void Shutdown() override;

  std::string GetConfigName() const override { return CONFIG_NAME; }
  std::string GetDisplayName() const override { return "WebGPU"; }
  void InitBackendInfo(const WindowSystemInfo& wsi) override;

  static constexpr const char* CONFIG_NAME = "WebGPU";
};
}  // namespace WebGPU
