// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

// The WebGPU backend's device, owned by the thread that runs the GPU: with
// dual core, Dolphin's GPU thread, an Emscripten pthread. WebGPU objects
// cannot move between workers, so everything here is created and used on
// that thread only.
//
// The browser's WebGPU is promise-based and the GPU thread runs a blocking
// loop. Each wait below suspends the wasm stack through JSPI
// (wgpu::Instance::WaitAny) and lets the worker's event loop resolve the
// promise; see spikes/webgpu-thread/FINDINGS.md.

#pragma once

#include <atomic>
#include <cstdio>
#include <optional>
#include <string>

#include <emscripten.h>
#include <fmt/format.h>

#include <webgpu/webgpu_cpp.h>

#include "Common/CommonTypes.h"

// The backend's diagnostics, to the harness's output. Through Dolphin's log
// they depend on which log types the host enabled; while the backend is
// being brought up, every refusal has to be seen.
#define WGPU_LOG(...)                                                                    \
  do                                                                                     \
  {                                                                                      \
    std::printf("  webgpu    %s\n", fmt::format(__VA_ARGS__).c_str());                  \
  } while (0)

namespace WebGPU
{
struct Context
{
  wgpu::Instance instance;
  wgpu::Adapter adapter;
  wgpu::Device device;
  wgpu::Queue queue;
  wgpu::Surface surface;
  wgpu::TextureFormat surface_format = wgpu::TextureFormat::Undefined;
  u32 surface_width = 0;
  u32 surface_height = 0;
  bool has_unorm16 = false;
  // docs/PHASE3-WEBGPU.md M4: bind groups made once and reused, uniform
  // blocks bound at dynamic offsets, state a pass already has not set
  // again, and buffer writes gathered into one per buffer per submit.
  // False is M1's path -- everything per draw -- kept to measure against.
  bool fast_path = true;
  // Measurement only: no buffer writes at all, so the draws read whatever
  // the buffers hold. What the uploads cost, on either thread.
  bool no_upload = false;
  // Measurement only: draws are not recorded (nothing reaches the GPU
  // process but passes, copies and the present).
  bool no_draws = false;
  // Measurement only: nothing reaches the canvas -- no surface texture, no
  // copy to it, no yield to the browser -- so Chrome has no animating
  // canvas to composite. The frames are still drawn and submitted.
  bool no_present = false;
  // Skip GX uniform uploads equal to the slot's last (Gfx::SetGXUniforms).
  bool uniform_dedup = true;
  // How long a draw waits for a pipeline the browser is still building
  // before the draw is skipped (Pipeline::GetPipeline). The browser
  // answers in 1-4 ms for 95% of pipelines when its Metal cache has met
  // the shaders, and in 25-50 ms when it has not; a fight brings dozens
  // of new ones a second, and waiting for every one froze the game.
  // UINT64_MAX waits for every pipeline, which is what a replay that
  // compares frames needs.
  u64 pipeline_wait_ns = 4'000'000;

  // The one layout every render pipeline shares (WGPUShader.h has the
  // numbering): group 0 the three uniform blocks, group 1 eight textures
  // and their eight samplers. Explicit rather than each pipeline's "auto"
  // layout, which drops whatever a shader declares but does not use -- and
  // Tint declares all eight textures in every TEV shader.
  wgpu::BindGroupLayout uniform_layout;
  wgpu::BindGroupLayout texture_layout;
  wgpu::PipelineLayout pipeline_layout;

  // Adapter, device (with the features docs/PHASE3-WEBGPU.md lists), and a
  // surface on the canvas this thread was handed. Blocks until done.
  bool Create();

  // Blocks until `future` completes.
  bool Wait(wgpu::Future future) const;

  // Runs `fn` with WebGPU validation errors captured, and returns the
  // first one's message, or an empty string. Blocks for the answer: for
  // object creation, where a failure must be known before it is used.
  template <typename F>
  std::string CaptureValidation(F&& fn) const
  {
    device.PushErrorScope(wgpu::ErrorFilter::Validation);
    fn();
    std::string message;
    Wait(device.PopErrorScope(wgpu::CallbackMode::WaitAnyOnly,
                              [&](wgpu::PopErrorScopeStatus, wgpu::ErrorType type,
                                  wgpu::StringView text) {
                                if (type != wgpu::ErrorType::NoError)
                                  message.assign(text.data, text.length);
                              }));
    return message;
  }
};

extern Context g_context;

// Counters for docs/PHASE3-WEBGPU.md M4, printed every 600 presents.
struct Stats
{
  u64 presents = 0;
  u64 maps = 0;          // staging readbacks: EFB peeks, copies to RAM
  double map_ms = 0;     // time the GPU thread spent blocked on them
  u64 submits = 0;
  u64 write_buffers = 0;
  u64 write_bytes = 0;
  u64 bind_groups = 0;   // created
  u64 bind_group_sets = 0;
  u64 texture_group_clears = 0;
  u64 draws = 0;
  u64 uniform_skips = 0;  // GX uniform uploads skipped: same bytes as the last
  double yield_ms = 0;
   // time in the per-present yield
};
extern Stats g_stats_wgpu;

// Time the GPU thread spends making shaders (glslang, Tint, the browser's
// compile) and pipelines, in microseconds. Atomic because the harness's
// --frame-times reads them from the CPU thread to say what a slow frame
// was waiting on.
inline std::atomic<u64> g_compile_us{0};
inline std::atomic<u64> g_compiles{0};
// The same time by phase: glslang (GLSL to SPIR-V), Tint (SPIR-V to WGSL),
// CreateShaderModule and CreateRenderPipeline -- the last two including
// CaptureValidation's wait for the browser's answer.
enum CompilePhase { kGlslang, kTint, kModule, kPipeline, kCompilePhases };
inline std::atomic<u64> g_phase_us[kCompilePhases];
inline std::atomic<u64> g_phase_n[kCompilePhases];
// The kPipeline phase is a draw's wait for its pipeline (Pipeline::
// GetPipeline), bounded by Context::pipeline_wait_ns. Alongside: the
// pipelines a draw asked for before the background got to them, the draws
// skipped because the wait ran out, and how long the waits that ended
// with a pipeline took, by doubling milliseconds (<1, <2, ... <64, more),
// for choosing the bound.
inline std::atomic<u64> g_pipelines_at_draw{0};
inline std::atomic<u64> g_draws_skipped{0};
constexpr int kWaitBuckets = 8;
inline std::atomic<u64> g_pipeline_wait_hist[kWaitBuckets];

// Adds the time from construction to destruction to g_compile_us, and to
// a phase's counter when given one.
struct CompileTimer
{
  int phase = -1;
  double t0 = emscripten_get_now();
  CompileTimer() = default;
  explicit CompileTimer(int phase_) : phase(phase_) {}
  ~CompileTimer()
  {
    const u64 us = static_cast<u64>((emscripten_get_now() - t0) * 1000.0);
    if (phase >= 0)
    {
      g_phase_us[phase].fetch_add(us, std::memory_order_relaxed);
      g_phase_n[phase].fetch_add(1, std::memory_order_relaxed);
      return;
    }
    g_compile_us.fetch_add(us, std::memory_order_relaxed);
    g_compiles.fetch_add(1, std::memory_order_relaxed);
  }
};

// Let the browser have this worker for one turn of its event loop. A
// canvas owned by a worker shows a frame only once the worker's task ends,
// and the GPU thread's task never does, so the present path calls this
// once a frame. A MessageChannel task, 21 us: a timer is clamped to ~5 ms
// and a microtask presents nothing (spikes/webgpu-thread/FINDINGS.md).
void YieldToBrowser();
}  // namespace WebGPU
