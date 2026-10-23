// Spike S1 (docs/PHASE3-WEBGPU.md §5): can Dolphin's GPU thread own a WebGPU
// device in a browser?
//
// The GPU thread is an Emscripten pthread that runs a blocking loop and
// never returns to its worker's event loop. Three questions:
//
//   1. Can that thread acquire an adapter and a device? Both are promises.
//   2. Can it draw into a canvas the page transferred to it, and do frames
//      reach the screen while it does not return to the event loop?
//   3. Can it BLOCK on a readback (mapAsync) -- what an EFB peek needs --
//      and what does one cost?
//
// Blocking on a promise from wasm is JSPI (-sJSPI): the wasm stack is
// suspended, the worker's event loop runs, the promise resolves, the stack
// resumes. instance.WaitAny is where emdawnwebgpu does that.
#include <cstdio>
#include <cstring>
#include <pthread.h>
#include <vector>

#include <emscripten.h>
#include <emscripten/threading.h>
#include <webgpu/webgpu_cpp.h>

static wgpu::Instance g_instance;

template <typename F>
static bool Wait(F future)
{
  return g_instance.WaitAny(future, UINT64_MAX) == wgpu::WaitStatus::Success;
}

// Lines go to the page's #out on the main thread, synchronously: the main
// thread is idle (main() has returned), and a pthread's own postMessage
// would land in Emscripten's worker-message handler.
static void Report(const char* line)
{
  MAIN_THREAD_EM_ASM({ document.getElementById('out').textContent += UTF8ToString($0) + '\n'; },
                     line);
}

static void Say(const char* fmt, double a = 0, double b = 0, double c = 0)
{
  char line[256];
  std::snprintf(line, sizeof(line), fmt, a, b, c);
  Report(line);
}

// Yield to the worker's event loop for one turn, through JSPI: a promise
// that resolves on the next task. If frames only reach the canvas while the
// thread is suspended, this is what the GPU loop would have to do per
// presented frame.
EM_ASYNC_JS(void, YieldOnce, (int how), {
  if (how == 0) {
    await new Promise(r => setTimeout(r, 0));          // a timer task (clamped after nesting)
  } else if (how == 1) {
    if (!globalThis.__mc) {                              // a message task: not clamped
      globalThis.__mc = new MessageChannel();
      globalThis.__mcq = [];
      globalThis.__mc.port1.onmessage = () => globalThis.__mcq.shift()();
    }
    await new Promise(r => { globalThis.__mcq.push(r); globalThis.__mc.port2.postMessage(0); });
  } else {
    await Promise.resolve();                             // a microtask: not a task at all
  }
});

static void* GpuThread(void*)
{
  const wgpu::InstanceFeatureName features[] = {wgpu::InstanceFeatureName::TimedWaitAny};
  wgpu::InstanceDescriptor idesc{};
  idesc.requiredFeatureCount = 1;
  idesc.requiredFeatures = features;
  g_instance = wgpu::CreateInstance(&idesc);
  Say("instance %g", g_instance ? 1 : 0);

  const double t0 = emscripten_get_now();
  wgpu::Adapter adapter;
  Wait(g_instance.RequestAdapter(
      nullptr, wgpu::CallbackMode::WaitAnyOnly,
      [&](wgpu::RequestAdapterStatus status, wgpu::Adapter a, wgpu::StringView) {
        if (status == wgpu::RequestAdapterStatus::Success)
          adapter = std::move(a);
      }));
  Say("adapter %g after %.1f ms", adapter ? 1 : 0, emscripten_get_now() - t0);
  if (!adapter)
    return nullptr;

  wgpu::DeviceDescriptor ddesc{};
  ddesc.SetUncapturedErrorCallback(
      [](const wgpu::Device&, wgpu::ErrorType, wgpu::StringView message) {
        char line[512];
        std::snprintf(line, sizeof(line), "uncaptured error: %.*s",
                      static_cast<int>(message.length), message.data);
        Report(line);
      });
  wgpu::Device device;
  Wait(adapter.RequestDevice(
      &ddesc, wgpu::CallbackMode::WaitAnyOnly,
      [&](wgpu::RequestDeviceStatus status, wgpu::Device d, wgpu::StringView) {
        if (status == wgpu::RequestDeviceStatus::Success)
          device = std::move(d);
      }));
  Say("device %g after %.1f ms", device ? 1 : 0, emscripten_get_now() - t0);
  if (!device)
    return nullptr;
  wgpu::Queue queue = device.GetQueue();

  // The canvas the page transferred to this thread.
  wgpu::EmscriptenSurfaceSourceCanvasHTMLSelector source{};
  source.selector = "#canvas";
  wgpu::SurfaceDescriptor sdesc{};
  sdesc.nextInChain = &source;
  wgpu::Surface surface = g_instance.CreateSurface(&sdesc);
  Say("surface %g", surface ? 1 : 0);
  wgpu::SurfaceCapabilities caps;
  surface.GetCapabilities(adapter, &caps);
  const wgpu::TextureFormat format = caps.formats[0];
  wgpu::SurfaceConfiguration config{};
  config.device = device;
  config.format = format;
  config.usage = wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::CopySrc;
  config.width = 640;
  config.height = 528;
  surface.Configure(&config);

  // The readback target: one 640x528 RGBA8 frame, as an EFB would be.
  constexpr uint32_t W = 640, H = 528, ROW = W * 4;
  wgpu::TextureDescriptor tdesc{};
  tdesc.size = {W, H, 1};
  tdesc.format = wgpu::TextureFormat::RGBA8Unorm;
  tdesc.usage = wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::CopySrc;
  wgpu::Texture efb = device.CreateTexture(&tdesc);
  wgpu::BufferDescriptor bdesc{};
  bdesc.size = ROW * H;
  bdesc.usage = wgpu::BufferUsage::MapRead | wgpu::BufferUsage::CopyDst;
  wgpu::Buffer staging = device.CreateBuffer(&bdesc);

  auto clear_pass = [&](wgpu::CommandEncoder& enc, const wgpu::TextureView& view, double r,
                        double g, double bl) {
    wgpu::RenderPassColorAttachment att{};
    att.view = view;
    att.loadOp = wgpu::LoadOp::Clear;
    att.storeOp = wgpu::StoreOp::Store;
    att.clearValue = {r, g, bl, 1.0};
    wgpu::RenderPassDescriptor rp{};
    rp.colorAttachmentCount = 1;
    rp.colorAttachments = &att;
    enc.BeginRenderPass(&rp).End();
  };

  // (3) Blocking readbacks: clear the EFB to a known colour, copy one
  // pixel's row (what a peek needs) or the whole frame to the staging
  // buffer, map, read, unmap -- the round trip a peek pays.
  for (int whole = 0; whole < 2; ++whole)
  {
    const int reps = 50;
    double total = 0, worst = 0;
    bool ok = true;
    for (int i = 0; i < reps; ++i)
    {
      const double t = emscripten_get_now();
      wgpu::CommandEncoder enc = device.CreateCommandEncoder();
      clear_pass(enc, efb.CreateView(), (i % 7) / 7.0, 0.5, 0.25);
      wgpu::TexelCopyTextureInfo src{};
      src.texture = efb;
      wgpu::TexelCopyBufferInfo dst{};
      dst.buffer = staging;
      dst.layout.bytesPerRow = ROW;
      const wgpu::Extent3D extent = whole ? wgpu::Extent3D{W, H, 1} : wgpu::Extent3D{W, 1, 1};
      enc.CopyTextureToBuffer(&src, &dst, &extent);
      wgpu::CommandBuffer cb = enc.Finish();
      queue.Submit(1, &cb);
      bool mapped = false;
      Wait(staging.MapAsync(wgpu::MapMode::Read, 0, whole ? ROW * H : ROW,
                            wgpu::CallbackMode::WaitAnyOnly,
                            [&](wgpu::MapAsyncStatus s, wgpu::StringView) {
                              mapped = s == wgpu::MapAsyncStatus::Success;
                            }));
      if (mapped)
      {
        const uint8_t* p =
            static_cast<const uint8_t*>(staging.GetConstMappedRange(0, whole ? ROW * H : ROW));
        const int want_r = static_cast<int>((i % 7) / 7.0 * 255.0 + 0.5);
        if (!p || std::abs(p[0] - want_r) > 1 || std::abs(p[1] - 128) > 1)
          ok = false;
        staging.Unmap();
      }
      else
      {
        ok = false;
      }
      const double dt = emscripten_get_now() - t;
      total += dt;
      worst = dt > worst ? dt : worst;
    }
    Say(whole ? "readback whole frame: %.3f ms mean, %.3f ms worst" :
                "readback one row:    %.3f ms mean, %.3f ms worst",
        total / reps, worst);
    Report(ok ? "  values ok" : "  values WRONG");
  }

  // (2) Present. 60 frames per phase, each phase a different colour:
  // red never leaves the wasm stack (the GPU loop as it is today); green
  // yields through a timer task, blue through a message task, yellow
  // through a microtask. Then the thread holds, busy, for 300 ms, and the
  // page samples the screen.
  struct Phase { const char* name; int how; double r, g, b; };
  const Phase phases[] = {{"red", -1, 1, 0, 0}, {"green", 0, 0, 1, 0},
                          {"blue", 1, 0, 0, 1}, {"yellow", 2, 1, 1, 0}};
  for (const Phase& ph : phases)
  {
    const double t = emscripten_get_now();
    for (int f = 0; f < 60; ++f)
    {
      wgpu::SurfaceTexture st;
      surface.GetCurrentTexture(&st);
      wgpu::CommandEncoder enc = device.CreateCommandEncoder();
      clear_pass(enc, st.texture.CreateView(), ph.r, ph.g, ph.b);
      wgpu::CommandBuffer cb = enc.Finish();
      queue.Submit(1, &cb);
      if (ph.how >= 0)
        YieldOnce(ph.how);
    }
    char line[128];
    std::snprintf(line, sizeof(line), "%s: 60 frames in %.1f ms (%.3f ms each)", ph.name,
                  emscripten_get_now() - t, (emscripten_get_now() - t) / 60);
    Report(line);
    std::snprintf(line, sizeof(line), "SAMPLE %s", ph.name);
    Report(line);
    const double hold = emscripten_get_now();
    while (emscripten_get_now() - hold < 300)
    {
    }
  }
  Report("DONE");
  return nullptr;
}

int main()
{
  pthread_attr_t attr;
  pthread_attr_init(&attr);
  emscripten_pthread_attr_settransferredcanvases(&attr, "#canvas");
  pthread_t thread;
  pthread_create(&thread, &attr, GpuThread, nullptr);
  pthread_detach(thread);
  return 0;
}
