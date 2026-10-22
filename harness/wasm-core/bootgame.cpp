// Boot Skylanders.
//
// Everything in Phase 1 has been building toward this: guest memory, the
// interpreter, address translation, exceptions, IOS, and a disc that streams.
// This puts them together and asks the emulator to boot the real game, then
// steps the CPU and traces what it executes.
//
// The Phase 1 gate (docs/PLAN.md §5) is "the game's boot sequence executes
// under the interpreter and CPU state matches a captured native trace
// instruction for instruction". This is the first half: get it executing.
// harness/wasm-core/difftrace.py is the second.
//
// Usage:
//   bootgame <disc-path-or-url> [steps] [--trace]

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <algorithm>
#include <map>
#include <unordered_map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#if defined(__EMSCRIPTEN__)
#include <emscripten.h>
#endif

#include "Common/CommonTypes.h"
#include "Common/Config/Config.h"
#include "Common/FileUtil.h"
#include "Common/IOFile.h"
#include "Core/Boot/Boot.h"
#include "Core/Config/MainSettings.h"
#include "Core/ConfigLoaders/BaseConfigLoader.h"
#include "Core/ConfigManager.h"
#include "Core/Core.h"
#include "Core/CoreTiming.h"
#include "AudioCommon/AudioCommon.h"
#include "Common/Logging/LogManager.h"
#include "Common/WindowSystemInfo.h"
#include "VideoCommon/AsyncRequests.h"
#include "VideoCommon/Fifo.h"
#include "VideoCommon/CommandProcessor.h"
#include "VideoCommon/VideoBackendBase.h"
#include "Core/HW/GCKeyboard.h"
#include "Core/HW/GCPad.h"
#include "Core/HW/Wiimote.h"
#include "InputCommon/ControllerEmu/ControllerEmu.h"
#include "InputCommon/ControllerInterface/ControllerInterface.h"
#include "InputCommon/InputConfig.h"
#include "Core/HW/EXI/EXI_Device.h"
#include "Core/HW/CPU.h"
#include "Core/DSPEmulator.h"
#include "Core/HW/DSP.h"
#include "Core/HW/HW.h"
#include "Core/HW/VideoInterface.h"
#include "Core/HW/Memmap.h"
#include "Core/IOS/IOS.h"
#include "Core/IOS/USB/Emulated/Skylanders/Skylander.h"
#include "Core/IOS/USB/Emulated/Skylanders/SkylanderFigure.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/System.h"
#include "Core/WiiRoot.h"
#include "DiscIO/Volume.h"
#include "DiscIO/VolumeDisc.h"
#include "UICommon/UICommon.h"

#include "bytesources.h"
#include "../../ppc2wasm/src/block.h"
#include "rangeblob.h"
#include "trace.h"

// Progress markers that bypass stdout.
//
// Under PROXY_TO_PTHREAD, printf from the proxied main thread sends each write
// to the real browser main thread. If that thread is blocked, printf blocks
// too -- so a stall observed through printf may be a stall in *printing*
// rather than in the code being traced. The original symptom was output
// stopping MID-WORD, which a line-buffered stdout cannot otherwise produce.
//
// console.log is no better: with PROXY_TO_PTHREAD main() runs on a worker, so
// its console output lands in a context the page never sees and page-scoped
// CDP never reports.
//
// So the marker goes through memory instead. pthreads means the wasm heap is
// a SharedArrayBuffer, and the page holds the same buffer -- the worker stores
// a string, the page reads it with no proxying, no event loop, and no
// cooperation from the stalled thread. That is the one channel that still
// works when the thing being diagnosed is the output path itself.
extern "C" {
volatile char g_harness_stage[96] = "start";
#if defined(__EMSCRIPTEN__)
EMSCRIPTEN_KEEPALIVE const char* harness_stage() { return (const char*)g_harness_stage; }
#endif
}

#if defined(__EMSCRIPTEN__)
// Install a compiled block and return its table index, which under Emscripten
// IS a callable function pointer.
EM_JS(int, JitInstall, (const uint8_t* ptr, int len), {
  try {
    const bytes = HEAPU8.slice(ptr, ptr + len);
    const mod = new WebAssembly.Module(bytes);
    // A linked block imports the function table so it can tail-call its
    // successor. Omitting it fails instantiation with a link error, which
    // this catch turns into a silent fallback -- every block interpreted,
    // and the only symptom a run that is SLOWER than no JIT at all.
    const inst = new WebAssembly.Instance(mod, {
      env: { memory: wasmMemory, __indirect_function_table: wasmTable },
    });
    const idx = wasmTable.length;
    wasmTable.grow(1);
    wasmTable.set(idx, inst.exports.f);
    return idx;
  } catch (e) {
    // Report once. A JIT that silently compiles nothing looks like a slow
    // JIT rather than a broken one.
    if (!Module.__jitReported) {
      Module.__jitReported = 1;
      out('  jit install failed: ' + e);
    }
    return -1;
  }
});
// Install a module holding several blocks, returning the table index of the
// FIRST of them; the rest follow consecutively.
//
// Installation costs about 22 us per module almost regardless of what is in
// it (harness/wasm-core/jitbatch.cpp measures 13.7 us for one block and
// 1.44 us per block at sixteen), so this is where the price of compiling
// drops by an order of magnitude.
EM_JS(int, JitInstallBatch, (const uint8_t* ptr, int len, int nfuncs), {
  try {
    const bytes = HEAPU8.slice(ptr, ptr + len);
    const mod = new WebAssembly.Module(bytes);
    const inst = new WebAssembly.Instance(mod, {
      env: { memory: wasmMemory, __indirect_function_table: wasmTable },
    });
    const base = wasmTable.length;
    wasmTable.grow(nfuncs);
    for (let i = 0; i < nfuncs; i++)
      wasmTable.set(base + i, inst.exports['f' + i]);
    return base;
  } catch (e) {
    if (!Module.__jitReported) {
      Module.__jitReported = 1;
      out('  jit batch install failed: ' + e);
    }
    return -1;
  }
});

EM_JS(int, JitMemoryPages, (), { return wasmMemory.buffer.byteLength >>> 16; });
EM_JS(double, JitNow, (), { return performance.now(); });
#endif

// ── GX command stream capture ──
//
// Core/HW/GPFifo.cpp calls this for every 32 bytes the game writes to the
// write-gather pipe, which is the whole command stream in order. Accumulated
// here and dumped as base64 so it survives the trip out of a browser, where
// there is no filesystem to write to.
//
// Capped rather than unbounded: the stream grows without limit while the game
// runs, and the point is to decode a representative slice, not to mirror a
// session into memory.
static std::vector<u8> g_fifo_bytes;
static size_t g_fifo_limit = 0;

extern "C" void harness_gp_bytes(const u8* data, size_t size)
{
  if (g_fifo_limit == 0 || g_fifo_bytes.size() >= g_fifo_limit)
    return;
  g_fifo_bytes.insert(g_fifo_bytes.end(), data, data + size);
}

// The EXECUTED command stream, with display lists expanded.
//
// Two different things are worth capturing and they are not the same stream:
//
//   gather pipe   what the CPU wrote. Contains CALL_DL pointers and none of
//                 the geometry they refer to, because a display list's body
//                 lives in guest memory and never crosses the FIFO. A 256 KB
//                 capture this way held 400 display lists and ZERO draws.
//
//   commands      what the GPU executed, display lists followed. This is the
//                 same shape Dolphin's FifoRecorder writes to a .dff, so the
//                 existing decoder reads it unchanged.
//
// Keeping both is deliberate: the first is the transport, the second is the
// content, and the difference between them is exactly how much of this game's
// geometry is hiding in display lists.
static std::vector<u8> g_gx_bytes;
static size_t g_gx_limit = 0;

extern "C" void harness_gx_command(const u8* data, u32 size)
{
  if (g_gx_limit == 0 || g_gx_bytes.size() >= g_gx_limit)
    return;
  g_gx_bytes.insert(g_gx_bytes.end(), data, data + size);
}

// base64, so the capture costs 1.33 bytes per byte through the CDP channel
// instead of 2 for hex. Written out rather than pulled from a library because
// the harness links no base64 and this is twenty lines.
static void EmitBase64(const std::vector<u8>& v)
{
  static const char* T = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string line;
  for (size_t i = 0; i < v.size(); i += 3)
  {
    const u32 a = v[i];
    const u32 b = i + 1 < v.size() ? v[i + 1] : 0;
    const u32 c = i + 2 < v.size() ? v[i + 2] : 0;
    const u32 w = (a << 16) | (b << 8) | c;
    line += T[(w >> 18) & 63];
    line += T[(w >> 12) & 63];
    line += i + 1 < v.size() ? T[(w >> 6) & 63] : '=';
    line += i + 2 < v.size() ? T[w & 63] : '=';
    // Bounded line length: one enormous line is harder to move and harder to
    // read back than many short ones.
    if (line.size() >= 76)
    {
      std::printf("F %s\n", line.c_str());
      line.clear();
    }
  }
  if (!line.empty())
    std::printf("F %s\n", line.c_str());
}

// ── CoreTiming event log ──
//
// Answers the one question a register trace cannot. At step 64,453,122 native
// and wasm hold IDENTICAL GPRs, but native has taken an external interrupt and
// wasm has not. Nothing about the guest's computed state differs -- what
// differs is WHEN a scheduled event fired, and that lives in the event queue,
// not in the register file.
//
// Gated on the same --from window as tracing, because events fire constantly
// (SyncGPUCallback alone is every 1000 ticks) and logging 64 million steps of
// them is neither transportable out of a browser nor necessary.
static bool g_event_log_on = false;

// ── synthetic input ──
//
// The game draws an attract loop and stops loading from the disc: 454 disc
// requests at 3 billion instructions and still 454 at ten billion, while it
// renders 2,839 two-dimensional quads and polls the Bluetooth adapter. It is
// waiting to be told to continue, and nothing in this harness ever presses
// anything.
//
// Dolphin exposes SetInputOverrideFunction on an emulated controller, so this
// needs no patch -- the override is consulted wherever the controller would
// otherwise read a real device.
//
// The schedule is a pure function of the instruction count, which is what
// makes it usable in a differential: both targets press at exactly the same
// instruction. Anything driven by wall-clock would make the two runs
// incomparable, which is the mistake that took the network offline.
// The button currently held, as a (group, control) pair the override matches
// against. Empty means nothing is pressed.
static const char* g_press_group = "";
static const char* g_press_control = "";

// The sequence to cycle through, one entry per press window.
static std::vector<std::pair<const char*, const char*>> g_input_sequence;

// One emulated second is about 729 million instructions (the Wii CPU clock,
// and this harness advances one tick per instruction). Press for ~0.2s every
// ~2s: long enough to register, with gaps long enough for a menu to act on it.
static constexpr u64 PRESS_PERIOD = 1458000000ull;  // ~2.0s
static constexpr u64 PRESS_LENGTH = 150000000ull;   // ~0.2s

// Names from WiimoteEmu.h and ControllerEmu.h. A D-pad direction and a face
// button live in different GROUPS, which is why both halves are carried.
static bool LookupButton(const std::string& name,
                         std::pair<const char*, const char*>* out)
{
  static const struct { const char* name; const char* group; const char* control; } kMap[] = {
      {"A", "Buttons", "A"},       {"B", "Buttons", "B"},
      {"1", "Buttons", "1"},       {"2", "Buttons", "2"},
      {"+", "Buttons", "+"},       {"-", "Buttons", "-"},
      {"Home", "Buttons", "Home"}, {"Up", "D-Pad", "Up"},
      {"Down", "D-Pad", "Down"},   {"Left", "D-Pad", "Left"},
      {"Right", "D-Pad", "Right"},
  };
  for (const auto& e : kMap)
  {
    if (name == e.name)
    {
      *out = {e.group, e.control};
      return true;
    }
  }
  return false;
}
// The step the CPU loop is on, so an event can be tied to an instruction
// count. Ticks and steps are NOT the same clock: CoreTiming advances by
// slice_length - downcount, and the apploader runs before stepping begins, so
// the two run at different offsets and potentially different rates. When the
// event ticks match but the guest diverges, the mapping between them is the
// only place left to look.
static u64 g_cur_step = 0;

// Take the Wii network offline, on BOTH targets.
//
// /dev/net/ip/top performs real host socket operations. Native leaves a socket
// op pending; Emscripten's socket emulation answers immediately. That showed
// up as a divergence at 531 million instructions -- but the deeper problem is
// that it makes the ORACLE depend on host network state, so native would not
// even reproduce against itself on a different network.
//
// A browser has no raw sockets, so "no Wii network" is also what the real
// target looks like. Refusing the device symmetrically is both deterministic
// and honest; refusing it on one target only would be the exact mistake that
// caused the Wiimote divergence.
extern "C" void harness_usb_device(unsigned vid, unsigned pid, const char* name)
{
  // Each distinct device once, ever.
  //
  // The USB scanner rescans every 50ms for the lifetime of the process, so an
  // unconditional print here emits the same line thousands of times -- and
  // keeps emitting after the step loop has finished, because the scanner is
  // on its own thread. A browser run never terminated: the step marker sat at
  // "stepping 1996M" while the line count climbed past 19,000.
  //
  // Not gated on the event window, because device presence is exactly the
  // kind of per-target difference that is invisible until it costs hundreds
  // of millions of instructions, and it is established before any window
  // would open.
  static std::set<std::pair<unsigned, unsigned>> seen;
  if (!seen.insert({vid, pid}).second)
    return;
  std::printf("  usb device %04x:%04x %s\n", vid, pid, name);
}

extern "C" int harness_block_device(const char* path)
{
  return std::strncmp(path, "/dev/net/", 9) == 0 ? 1 : 0;
}

extern "C" void harness_ios_reply(unsigned command, unsigned fd, int return_value,
                                  long long cycles, const char* device)
{
  if (!g_event_log_on)
    return;
  std::printf("R cmd=%u fd=%u ret=%d cycles=%lld dev=%s step=%llu\n", command, fd, return_value,
              cycles, device, static_cast<unsigned long long>(g_cur_step));
}

extern "C" void harness_vi_state(unsigned half_lines, unsigned hlw, unsigned vct0,
                                 unsigned hct0, unsigned mask0, unsigned int0)
{
  if (!g_event_log_on)
    return;
  std::printf("V hl=%u hlw=%u vct0=%u hct0=%u mask0=%u int0=%u step=%llu\n", half_lines, hlw,
              vct0, hct0, mask0, int0, static_cast<unsigned long long>(g_cur_step));
}

extern "C" void harness_pi_interrupt(unsigned cause, int set, unsigned cause_now,
                                     unsigned mask_now)
{
  if (!g_event_log_on)
    return;
  std::printf("I cause=%08x set=%d now=%08x mask=%08x step=%llu\n", cause, set, cause_now,
              mask_now, static_cast<unsigned long long>(g_cur_step));
}

extern "C" void harness_core_event(const char* name, long long scheduled, long long now)
{
  if (!g_event_log_on)
    return;
  std::printf("E %s sched=%lld now=%lld step=%llu\n", name, scheduled, now,
              static_cast<unsigned long long>(g_cur_step));
}

static void InitMark(const char* msg)
{
  std::size_t i = 0;
  for (; msg[i] && i < sizeof(g_harness_stage) - 1; ++i)
    g_harness_stage[i] = msg[i];
  g_harness_stage[i] = '\0';
#if !defined(__EMSCRIPTEN__)
  std::printf("  [init] %s\n", msg);
#endif
}
#define INIT_MARK(msg) InitMark(msg)

// Satisfies the weak hook in Core/HW/HW.cpp so HW::Init reports which
// sub-initialiser it is in. Without this the stall is only localisable to
// "somewhere in HW::Init", which is twenty separate subsystems.
extern "C" void harness_hw_stage(const char* name)
{
  char buf[96];
  std::snprintf(buf, sizeof(buf), "HW::Init -> %s", name);
  InitMark(buf);
}

static int g_failures = 0;

static void check(bool ok, const char* what)
{
  std::printf("  %-52s %s\n", what, ok ? "ok" : "FAIL");
  if (!ok)
    ++g_failures;
}

// ── framebuffer capture ──
//
// The XFB is YUYV 4:2:2 in GUEST memory: the buffer the Video Interface scans
// out. Reading it needs no cooperation from the video backend, which matters
// because headless has no presentation path at all.
//
// 640x480 is this title's NTSC framebuffer. No VI register is consulted, so
// another video mode would come out skewed with no warning.
static constexpr u32 XFB_W = 640, XFB_H = 480;

static std::vector<u8> ReadXfbYuv(Core::System& system, u32 xfb)
{
  std::vector<u8> yuv(static_cast<size_t>(XFB_W) * XFB_H * 2);
  auto& mem = system.GetMemory();
  for (size_t off = 0; off < yuv.size(); off += 4)
  {
    const u32 word = mem.Read_U32(xfb + static_cast<u32>(off));
    yuv[off + 0] = static_cast<u8>(word >> 24);
    yuv[off + 1] = static_cast<u8>(word >> 16);
    yuv[off + 2] = static_cast<u8>(word >> 8);
    yuv[off + 3] = static_cast<u8>(word);
  }
  return yuv;
}

static std::vector<u8> YuvToRgb(const std::vector<u8>& yuv)
{
  std::vector<u8> rgb(static_cast<size_t>(XFB_W) * XFB_H * 3);
  auto clamp8 = [](double x) { return static_cast<u8>(x < 0 ? 0 : (x > 255 ? 255 : x)); };
  for (size_t i = 0, p = 0; i < yuv.size(); i += 4, p += 6)
  {
    const double y0 = yuv[i], u = yuv[i + 1] - 128.0, y1 = yuv[i + 2], v = yuv[i + 3] - 128.0;
    rgb[p + 0] = clamp8(y0 + 1.371 * v);
    rgb[p + 1] = clamp8(y0 - 0.698 * v - 0.336 * u);
    rgb[p + 2] = clamp8(y0 + 1.732 * u);
    rgb[p + 3] = clamp8(y1 + 1.371 * v);
    rgb[p + 4] = clamp8(y1 - 0.698 * v - 0.336 * u);
    rgb[p + 5] = clamp8(y1 + 1.732 * u);
  }
  return rgb;
}

// A fingerprint, not an image.
//
// A full frame is 900 KB, which cannot be emitted thousands of times out of a
// browser. A hash per frame is eight bytes and answers the question that
// actually matters across two targets -- WHICH frame first differs -- after
// which one full frame can be dumped natively and looked at.
static u64 HashFrame(const std::vector<u8>& bytes)
{
  u64 h = 0xcbf29ce484222325ULL;
  for (const u8 b : bytes)
  {
    h ^= b;
    h *= 0x100000001b3ULL;
  }
  return h;
}

static void WritePpm(const std::string& path, const std::vector<u8>& rgb)
{
  std::FILE* f = std::fopen(path.c_str(), "wb");
  if (!f)
    return;
  std::fprintf(f, "P6\n%u %u\n255\n", XFB_W, XFB_H);
  std::fwrite(rgb.data(), 1, rgb.size(), f);
  std::fclose(f);
}

int main(int argc, char** argv)
{
  std::setvbuf(stdout, nullptr, _IONBF, 0);

  const std::string location = argc > 1 ? argv[1] : "/tmp/ssa.iso";
  const u64 steps = argc > 2 ? std::strtoull(argv[2], nullptr, 0) : 2000;
  bool want_trace = false;
  // --until-fifo: treat the step count as a cap and stop at the first FIFO
  // write instead of running it out. Reaching the FIFO takes far more
  // instructions than a fixed differential run, and every step past the
  // milestone is browser time spent learning nothing.
  bool stop_at_fifo = false;
  bool want_events = false;
  bool want_input = false;
  std::string input_seq = "A";
  bool want_skylander = false;
  u16 sky_id = 0, sky_var = 0;
  u64 checkpoint = 0;
  u64 trace_from = 0;
  u64 dump_at = 0;
  u32 watch_addr = 0;
  bool want_coverage = false;
  bool want_jit = false;
  bool want_link = true;
  bool want_jit_stats = false;
  int jit_probe = 0;
  bool want_render = false;
  std::string xfb_path;
  u64 frame_every = 0;
  for (int i = 1; i < argc; ++i)
  {
    if (std::strcmp(argv[i], "--trace") == 0)
      want_trace = true;
    else if (std::strcmp(argv[i], "--until-fifo") == 0)
      stop_at_fifo = true;
    else if (std::strcmp(argv[i], "--events") == 0)
      want_events = true;
    // --input: drive the A button on a fixed, instruction-count schedule.
    // Opt-in so that every existing differential baseline keeps its meaning.
    // --input[=SEQ]: cycle a comma-separated button sequence, one entry per
    // press window. Defaults to "A" so every existing baseline keeps meaning.
    else if (std::strncmp(argv[i], "--input", 7) == 0)
    {
      want_input = true;
      if (argv[i][7] == '=')
        input_seq = argv[i] + 8;
    }
    // --skylander[=ID:VAR]: create a figure and place it on the portal.
    // Defaults to Whirlwind (0, 0x0000), one of the figures that shipped with
    // Spyro's Adventure, so the id/var pair is known-good for this disc.
    else if (std::strncmp(argv[i], "--skylander", 11) == 0)
    {
      want_skylander = true;
      if (argv[i][11] == '=')
        std::sscanf(argv[i] + 12, "%hu:%hx", &sky_id, &sky_var);
    }
    // --checkpoint=N: emit a trace line every N steps instead of every step.
    // A full trace to the first FIFO write would be 63 million lines, which is
    // not transportable out of a browser. Checkpoints every 100k give ~630
    // lines and localise a divergence to a 100k window; a second run with
    // --trace over that window pinpoints the instruction. The line format is
    // identical to --trace, so difftrace.py compares either without changes.
    else if (std::strncmp(argv[i], "--checkpoint=", 13) == 0)
      checkpoint = std::strtoull(argv[i] + 13, nullptr, 0);
    // --from=N: skip emitting until step N. Pairs with --trace to capture a
    // bounded window deep into the run.
    else if (std::strncmp(argv[i], "--from=", 7) == 0)
      trace_from = std::strtoull(argv[i] + 7, nullptr, 0);
    // --dump-at=N: full register state at step N, plus the instruction words
    // around the PC. This is the last step of localising a divergence: the
    // checkpoint pass finds the 100k window, a traced window finds the step,
    // and this says which register and which opcode.
    else if (std::strncmp(argv[i], "--dump-at=", 10) == 0)
      dump_at = std::strtoull(argv[i] + 10, nullptr, 0);
    // --watch=ADDR: report every change to the u32 at ADDR, with the PC that
    // was executing. A spin loop reveals WHAT it polls but not WHO satisfies
    // it; this closes that gap without a full memory trace.
    // --render: use the Software backend instead of Null, so EFB copies
    // actually produce pixels in the XFB.
    else if (std::strcmp(argv[i], "--render") == 0)
      want_render = true;
    // --dump-xfb=PATH: decode the external framebuffer to a PPM.
    else if (std::strncmp(argv[i], "--dump-xfb=", 11) == 0)
      xfb_path = argv[i] + 11;
    // --frame-every=N: fingerprint the framebuffer every N instructions, and
    // natively also write each one as PREFIX-NNNN.ppm. The fingerprints are
    // what cross the browser boundary; the images are for looking at.
    else if (std::strncmp(argv[i], "--frame-every=", 14) == 0)
      frame_every = std::strtoull(argv[i] + 14, nullptr, 0);
    // --jit-coverage: tally the opcodes the game actually executes and report
    // which of them ppc2wasm can translate. The instruction list in the
    // recompiler says what it CAN do; this says what the game NEEDS, and the
    // gap between those is what to build next.
    else if (std::strcmp(argv[i], "--jit-coverage") == 0)
      want_coverage = true;
    // --jit: execute through compiled blocks where possible, interpreting
    // whatever will not compile.
    else if (std::strcmp(argv[i], "--jit") == 0)
      want_jit = true;
    // --jit-no-link: make blocks return to the dispatcher instead of
    // tail-calling their successor.
    //
    // Linking is ON by default now that it pays: 54.0 MIPS against 42.1
    // without. It did not pay until two other things were true -- every
    // block gets compiled, so a successor exists to link to, and blocks
    // that always refuse get retired, so a chain is not routed through one.
    else if (std::strcmp(argv[i], "--jit-stats") == 0)
      // Per-dispatch and per-interpreted-instruction accounting. OFF by
      // default: it costs about 4% of the run, and the speed line is the
      // number this project steers by. A measurement that quietly taxes the
      // thing it measures is worse than no measurement.
      want_jit_stats = true;
    else if (std::strncmp(argv[i], "--jit-probe=", 12) == 0)
      // 1 = cache lookup, 2 = CoreTiming::Advance, 3 = one indirect call.
      // Each repeats work the dispatcher already does, once more per
      // round-trip; the cost is the delta against a baseline run.
      jit_probe = std::atoi(argv[i] + 12);
    else if (std::strcmp(argv[i], "--jit-no-link") == 0)
      want_link = false;
    else if (std::strncmp(argv[i], "--watch=", 8) == 0)
      watch_addr = static_cast<u32>(std::strtoul(argv[i] + 8, nullptr, 0));
    // --capture-fifo=N: capture N bytes of GX command stream and dump them as
    // base64. Implies running until the stream is full rather than to a step
    // count, because the interesting thing is the commands, not the distance.
    else if (std::strncmp(argv[i], "--capture-fifo=", 15) == 0)
      g_fifo_limit = std::strtoull(argv[i] + 15, nullptr, 0);
    // --capture-gx=N: capture N bytes of EXECUTED command stream, display
    // lists followed. This is the one that contains geometry.
    else if (std::strncmp(argv[i], "--capture-gx=", 13) == 0)
      g_gx_limit = std::strtoull(argv[i] + 13, nullptr, 0);
  }

#if defined(__EMSCRIPTEN__)
  std::printf("target: wasm32 (emscripten)  disc: %s\n\n", location.c_str());
#else
  std::printf("target: native  disc: %s\n\n", location.c_str());
#endif

  auto& system = Core::System::GetInstance();

  // Same ordering the differential established: NAND paths before the config
  // base layer, because the loader touches the IOS filesystem.
  Config::Init();
  const std::string user_dir = "/tmp/dolphin-boot-user/";
  File::CreateFullPath(user_dir);
  UICommon::SetUserDirectory(user_dir);
  Core::InitializeWiiRoot(true);
  Config::AddLayer(ConfigLoaders::GenerateBaseConfigLoader());
  SConfig::Init();

  // Logging comes AFTER the user directory: LogManager opens a log file under
  // it, and initialising it first aborts before anything useful is printed.
  Common::Log::LogManager::Init();
  auto* logm = Common::Log::LogManager::GetInstance();
  logm->SetConfigLogLevel(Common::Log::LogLevel::LINFO);
  logm->SetEnable(Common::Log::LogType::BOOT, true);
  logm->SetEnable(Common::Log::LogType::DVDINTERFACE, true);
  logm->SetEnable(Common::Log::LogType::IOS, true);
  logm->SetEnable(Common::Log::LogType::IOS_ES, true);
  logm->SetEnable(Common::Log::LogType::DISCIO, true);
  logm->SetEnable(Common::Log::LogType::CORE, true);
  logm->SetEnable(Common::Log::LogType::IOS_FS, true);
  logm->SetEnable(Common::Log::LogType::COMMON, true);
  logm->SetEnable(Common::Log::LogType::MEMMAP, true);
  logm->SetEnable(Common::Log::LogType::AUDIO, true);
  logm->EnableListener(Common::Log::LogListener::CONSOLE_LISTENER, true);



  // The emulated portal, not host USB. This is the whole point of the project
  // and costs one config line.
  Config::SetCurrent(Config::MAIN_EMULATE_SKYLANDER_PORTAL, true);

  // The CPU core is chosen through config, not by calling PowerPC::Init:
  // HW::Init does that itself from MAIN_CPU_CORE. Initialising it again
  // re-registers the "invalidateEmulatedCache" timing event and trips the same
  // duplicate-event assertion that the duplicate IOS init did.
  //
  // The wasm build is ENABLE_GENERIC and offers nothing else, but the native
  // differential build would default to the AArch64 recompiler, and the two
  // must run the same core to be comparable.
  Config::SetCurrent(Config::MAIN_CPU_CORE, PowerPC::CPUCore::Interpreter);
  system.Initialize();

  system.SetIsWii(true);

  // HW::Init rather than Memory::Init by hand. The earlier tests only needed
  // guest RAM, but booting touches hardware: the first attempt crashed in
  // MMIO::ReadHandler<u32>::Read with a null handler table, because
  // Memory::Init allocates the map while InitMMIO registers the devices
  // behind it -- and the latter is part of hardware bring-up.
  //
  // HW::Init also subsumes CoreTiming::Init, the CPU, DVD interface, the
  // GPFifo and WiiIPC, so the hand-rolled subset below it is gone.
  Core::DeclareAsCPUThread();

  // The sound stream must exist before HW::Init: AudioInterface::Init calls
  // Mixer::SetStreamInputSampleRateDivisor on it, and Core::Init creates it
  // one line earlier for exactly this reason. Skipping it crashes inside
  // AudioInterface with a null dereference that names neither audio nor
  // ordering.
  // The boot reads controller state (to check for held buttons), and
  // ControllerInterface::UpdateInput asserts if it was never initialised.
  // Headless is a first-class WindowSystemType, so this needs no window.
  WindowSystemInfo wsi;
  wsi.type = WindowSystemType::Headless;
  g_controller_interface.Initialize(wsi);
  check(g_controller_interface.IsInit(), "controller interface up (headless)");

  // Controller configs must be loaded, not just the interface initialised.
  // The Wii boot asks for controller state and InputConfig::GetController
  // ABORTS -- not returns null -- when no controllers were created. That
  // aborts with no message and no Dolphin frames in the default backtrace;
  // breaking on abort() is what found it.
  INIT_MARK("Pad::Initialize");
  Pad::Initialize();
  INIT_MARK("Keyboard::Initialize");
  Keyboard::Initialize();
  // Wiimote::Initialize is NOT optional, and skipping it was a mistake.
  //
  // It was skipped under Emscripten in #24 as a "precaution" -- it starts a
  // real-Wiimote scanning thread, and init-time thread creation is the
  // deadlock hazard on this port. It was explicitly recorded as making no
  // difference at the time, because the stall it was suspected of was
  // ExpansionInterface.
  //
  // It made a difference 477 MILLION instructions later. Without it, IOS's
  // emulated Bluetooth adapter /dev/usb/oh1/57e/305 does not answer an
  // IOCTLV, so no IPC reply is queued, the game keeps polling IPC_PPCCTRL at
  // 0xCD000004, and the two targets diverge at an MMIO read.
  //
  // The lesson is about the precaution, not the Wiimote: disabling a
  // subsystem "just in case" on one target of a differential makes the two
  // targets different by construction, and the cost surfaces nowhere near the
  // change.
  Wiimote::Initialize(Wiimote::InitializeMode::DO_NOT_WAIT_FOR_WIIMOTES);

  if (want_input)
  {
    for (size_t pos = 0; pos <= input_seq.size();)
    {
      const size_t comma = input_seq.find(',', pos);
      const std::string name = input_seq.substr(pos, comma - pos);
      std::pair<const char*, const char*> entry;
      if (!name.empty() && LookupButton(name, &entry))
        g_input_sequence.push_back(entry);
      else if (!name.empty())
        std::printf("  unknown button '%s' ignored\n", name.c_str());
      if (comma == std::string::npos)
        break;
      pos = comma + 1;
    }
    check(!g_input_sequence.empty(), "input sequence parsed");

    // The override is consulted wherever the emulated Wiimote would read a
    // real device, so no Dolphin patch is needed and no actual input backend
    // has to exist -- which matters in a browser, where there is none.
    if (InputConfig* cfg = Wiimote::GetConfig())
    {
      if (auto* controller = cfg->GetController(0))
      {
        controller->SetInputOverrideFunction(
            [](std::string_view group, std::string_view control,
               ControlState state) -> std::optional<ControlState> {
              // Only the groups this harness drives are overridden. Returning
              // nullopt elsewhere leaves motion, IR and extensions to their
              // normal (neutral) handling rather than forcing them to zero.
              if (group != "Buttons" && group != "D-Pad")
                return std::nullopt;
              const bool held = group == g_press_group && control == g_press_control;
              return held ? 1.0 : 0.0;
            });
        std::printf("  input sequence: %s (%zu entries)\n", input_seq.c_str(),
                    g_input_sequence.size());
      }
    }
  }
  check(true, "controller configs loaded");

  // Three config choices below, all about threads.
  //
  // The browser boot stalled inside HW::Init, and the cause is a single
  // Emscripten property: a pthread created from the proxied main thread is
  // actually spawned BY the browser main thread. If the creator then waits on
  // the new thread while the main thread is busy inside a proxied call,
  // neither side can advance. Anything starting a thread during init is
  // therefore a hazard, and these are the three that do.
  //
  // Only the EXI one was the real stall. The other two are the same hazard
  // class, disabled so they cannot become the next one.

  // 1. No EXI devices. THIS is what stalled. ExpansionInterface::Init builds
  //    the devices configured for each slot, and a memory card constructs a
  //    MemoryCard, whose constructor starts a flush thread
  //    (Core/HW/GCMemcard/GCMemcardRaw.cpp:81).
  //
  //    A Wii saves to NAND, not to GameCube memory cards, so empty slots cost
  //    this title nothing.
  Config::SetCurrent(Config::MAIN_SLOT_A, ExpansionInterface::EXIDeviceType::None);
  Config::SetCurrent(Config::MAIN_SLOT_B, ExpansionInterface::EXIDeviceType::None);
  Config::SetCurrent(Config::MAIN_SERIAL_PORT_1, ExpansionInterface::EXIDeviceType::None);

  // 1b. Single core. Dolphin's dual-core mode runs the GPU on its own thread
  //     (Core.cpp spawns it in GetInitializedVideoGuard), which is the same
  //     init-time thread hazard again. Single core runs the GPU inline on the
  //     CPU thread, and also makes the run deterministic -- a differential
  //     against a native oracle cannot tolerate a second thread deciding when
  //     the FIFO is consumed.
  Config::SetCurrent(Config::MAIN_CPU_THREAD, false);

  // 1c. Null video backend. It registers the CP/PE MMIO the game writes to
  //     and consumes the FIFO, but rasterises nothing -- which is exactly the
  //     Phase 1 shape in docs/PLAN.md: correctness of the command stream, no
  //     pixels.
  //
  // "Software Renderer" rasterises on the CPU, which is slow but needs no GPU
  // and no window -- the only way to get actual pixels out of a headless run,
  // and the only way to answer what is on screen. Null remains the default
  // because every differential baseline so far was measured with it.
  Config::SetCurrent(Config::MAIN_GFX_BACKEND,
                     std::string(want_render ? "Software Renderer" : "Null"));

  // 1d. Pin the emulated clock. SystemTimers.cpp:253 seeds the RTC from
  //     Common::Timer::GetLocalTimeSinceJan1970() unless a custom RTC is set,
  //     so every run starts the guest at a different wall-clock time. That is
  //     invisible for 20,000 instructions and very visible at 63 million: the
  //     step index of the first FIFO write drifted by a few hundred between
  //     native runs, which makes step indices useless as a comparator and
  //     would read as a wasm divergence when it is not one.
  //
  //     The value is arbitrary but fixed. What matters is that both targets
  //     and every repeat run see the same one.
  Config::SetCurrent(Config::MAIN_CUSTOM_RTC_ENABLE, true);
  Config::SetCurrent(Config::MAIN_CUSTOM_RTC_VALUE, u32{0x5D000000});

  // 2. DSP HLE. HW::Init passes MAIN_DSP_HLE to DSPManager::Init and the LLE
  //    path spawns a dedicated DSP thread; HLE runs the DSP on the caller.
  //    HLE is already the default, so this guards against a stray config.
  Config::SetCurrent(Config::MAIN_DSP_HLE, true);

  // 3. Null audio. InitSoundStream was never the problem -- the stage markers
  //    show it returning normally, and the printf evidence that pointed here
  //    was an artefact of the output channel wedging first. A browser build
  //    drives audio through an AudioWorklet (Spike C) rather than cubeb, so
  //    no backend is what this harness wants regardless: it boots and steps
  //    the CPU, it does not play anything.
  Config::SetCurrent(Config::MAIN_AUDIO_BACKEND, std::string(BACKEND_NULLSOUND));
  INIT_MARK("sound stream...");
  AudioCommon::InitSoundStream(system);
  INIT_MARK("sound stream done");
  // ── disc and game metadata, BEFORE HW::Init ──
  //
  // Order is load-bearing, and getting it wrong cost a real divergence.
  // VideoInterface::Init reads SConfig::m_region (VideoInterface.cpp:159) to
  // pick the VI clock: m_clock = IsNTSC(region), selecting 27MHz or 54MHz.
  // VI::Init runs inside HW::Init, so the region has to be set BEFORE that --
  // which is exactly what BootManager::BootCore does, calling
  // SetPathsAndGameMetadata at BootManager.cpp:56, long before Core::Init
  // reaches HW::Init.
  //
  // With the metadata set afterwards instead, VI read an uninitialised region.
  // Native happened to read zeroed memory (NTSC_J -- still NTSC, so
  // IsNTSC gave 1 and the clock was right by luck); wasm read reused heap and
  // got a value that is not a Region at all, so IsNTSC gave 0 and VI ran at
  // half the tick rate. Ticks per half-line came out 23166 instead of 11583,
  // VI fell exactly one half-line behind, and 64 MILLION instructions later a
  // vertical-retrace interrupt fired on one target and not the other.

  // ── the disc ──
  auto stats = std::make_shared<Harness::SourceStats>();
  Harness::ByteSource source;
  u64 disc_size = 0;
#if defined(__EMSCRIPTEN__)
  disc_size = 4699979776ull;
  source = Harness::HttpByteSource(location, stats);
#else
  {
    std::FILE* f = std::fopen(location.c_str(), "rb");
    if (!f)
    {
      std::printf("  no disc at %s\n", location.c_str());
      return 77;
    }
    std::fseek(f, 0, SEEK_END);
    disc_size = static_cast<u64>(std::ftell(f));
    std::fclose(f);
  }
  source = Harness::FileByteSource(location, stats);
#endif

  INIT_MARK("DiscIO::CreateDisc");
  auto volume = DiscIO::CreateDisc(
      std::make_unique<Harness::RangeBlobReader>(source, disc_size));
  check(volume != nullptr, "disc opened");
  if (!volume)
    return 1;
  std::printf("  booting %s (%s)\n", volume->GetGameID().c_str(),
              volume->GetInternalName().c_str());

  // ── boot ──
  // CBoot::BootUp runs the apploader out of the disc, which copies the game's
  // executable into guest memory and leaves the CPU at its entry point. This
  // is the real boot path, not a shortcut: the same call the GUI makes.
  auto boot = std::make_unique<BootParameters>(
      BootParameters::Disc{location, std::move(volume), {}});

  // Game metadata BEFORE boot. BootManager::BootCore does this at
  // BootManager.cpp:56, and calling CBoot::BootUp directly skips it -- which
  // leaves SConfig::m_region (ConfigManager.h:53, declared with no
  // initialiser) holding indeterminate memory.
  //
  // That is not a cosmetic omission. Boot_BS2Emu.cpp:512 writes
  // IsNTSC(m_region) ? 0 : 1 to 0x800000CC, so an unset region picks the wrong
  // video standard, and the game reads it back 59.7 MILLION instructions later
  // and takes a different branch. Native happened to read zeroed memory
  // (NTSC_J -- wrong for a USA disc, but NTSC, so the write matched by luck);
  // wasm read reused heap and got 4606276, which is not a Region at all.
  //
  // This is exactly the bug class the differential exists to catch: identical
  // code, identical input, and a divergence that only appears because one
  // target's uninitialised memory happens to be zero.
  check(SConfig::GetInstance().SetPathsAndGameMetadata(system, *boot),
        "game metadata set (region, paths)");

  // Hand SYSCONF to the guest, as BootManager.cpp:163 does.
  //
  // While this is unset, ConfigLoaders::SaveToSYSCONF builds a STANDALONE
  // IOS::HLE::Kernel to reach the NAND (BaseConfigLoader.cpp:90) -- and the
  // Kernel constructor asserts that no IOS already exists (IOS.cpp:285,
  // "using more than one IOS instance at a time is not supported"). Any config
  // write once emulation is up then kills the run mid-flight.
  //
  // This is the THIRD thing found missing because the harness calls
  // CBoot::BootUp directly instead of BootManager::BootCore, after the region
  // and the FIFO sync. The pattern is consistent: BootCore establishes global
  // state that something much later depends on, and skipping it fails far from
  // the cause.
  ConfigLoaders::TransferSYSCONFControlToGuest();

  INIT_MARK("HW::Init...");
  HW::Init(system, nullptr);
  INIT_MARK("HW::Init done");
  check(system.GetMemory().GetRAM() != nullptr, "hardware up, guest memory allocated");

  // ── the Portal of Power ──
  //
  // This is the point of the whole project: Skylanders reads figures over USB
  // from a toy portal, and a browser has no USB. Dolphin emulates the portal
  // (VID 0x1430, PID 0x0150) and knows 517 figure variants, so "every
  // Skylander available" is a matter of generating the right 1 KB of NFC data
  // rather than owning the toys.
  //
  // A figure is file-backed: Create() writes the encrypted MIFARE image, and
  // LoadSkylander() places it in a portal slot. MEMFS is a real filesystem as
  // far as this code is concerned, so the browser creates figures the same way
  // native does -- no special case.
  if (want_skylander)
  {
    const std::string sky_path = user_dir + "figure.sky";
    {
      IOS::HLE::USB::SkylanderFigure figure(sky_path);
      check(figure.Create(sky_id, sky_var), "Skylander figure data created");
      figure.Close();
    }
    File::IOFile sky_file(sky_path, "r+b");
    const u8 slot = system.GetSkylanderPortal().LoadSkylander(
        std::make_unique<IOS::HLE::USB::SkylanderFigure>(std::move(sky_file)));
    check(slot != 0xFF, "Skylander placed on the portal");
    std::printf("  skylander id=%u var=0x%04x -> portal slot %u\n", sky_id, sky_var, slot);
  }

  // ── video ──
  // After HW::Init, matching Core.cpp: HW::Init runs at Core.cpp:580 and the
  // video guard at :601. The order matters because Memory::InitMMIO (the last
  // thing HW::Init does) is what wires the CP and PE register blocks, and the
  // backend expects that to have happened.
  INIT_MARK("video backend");
  VideoBackendBase::ActivateBackend(Config::Get(Config::MAIN_GFX_BACKEND));
  check(g_video_backend != nullptr, "Null video backend activated");

  // Single core, so the GPU runs on this thread. Both of these are what
  // Core.cpp does inside its init_video lambda; passthrough means AsyncRequests
  // executes work inline instead of queueing it for a GPU thread that does not
  // exist here.
  Core::DeclareAsGPUThread();
  AsyncRequests::GetInstance()->SetPassthrough(true);
  const bool video_up = g_video_backend && g_video_backend->Initialize(wsi);
  check(video_up, "video backend initialised");
  INIT_MARK("video backend done");

  // IOS is NOT initialised here. HW::Init does it (HW.cpp:58, "Depends on
  // Memory"). Calling it again re-registers the IPCEvent and trips a
  // CoreTiming assertion about duplicate event names -- which is how this was
  // discovered.
  check(system.GetIOS() != nullptr, "IOS up (via HW::Init)");

  auto& ppc = system.GetPowerPC();
  check(ppc.GetMode() == PowerPC::CoreMode::Interpreter, "running the interpreter");


  // The DSP emulator has to be Initialize()d, not just constructed.
  //
  // HW::Init calls DSPManager::Init, which CREATES the emulator; Core.cpp:613
  // then calls Initialize() on it, and the harness skipped that. The guest's
  // DSPInit performs a mailbox handshake with the DSP and spins until it
  // answers, so an uninitialised DSP hangs the game -- which is exactly what
  // happened: GX set-up completed, two EFB copies went out, and then the game
  // sat in a three-instruction loop at 0x803873a0 waiting on a flag, for ten
  // billion instructions.
  //
  // The second argument is the DSP THREAD, forced false. Core.cpp decides it
  // from the host core count; here it is another init-time thread and so
  // another Emscripten deadlock, and a second thread would break determinism
  // besides.
  check(system.GetDSP().GetDSPEmulator() != nullptr, "DSP emulator exists");
  check(system.GetDSP().GetDSPEmulator()->Initialize(system.IsWii(), false),
        "DSP emulator initialised");
  AudioCommon::PostInitSoundStream(system);

  // Known starting execution state, as Core.cpp:620 does.
  system.GetCPU().Break();
  system.GetPowerPC().SetMode(PowerPC::CoreMode::Interpreter);


  // Logging starts BEFORE the boot, not at the first stepped instruction.
  // CBoot::BootUp runs the apploader as real guest code, which advances
  // CoreTiming -- so by the time the step loop begins, VI has already counted
  // hundreds of half-lines. An offset introduced there is invisible to any
  // window that starts at step 0.
  g_event_log_on = want_events;
  // The emulated clock at the moment boot begins. If the two targets are
  // already at different ticks here, everything scheduled afterwards is
  // offset, and no amount of CPU-state comparison downstream will show why.
  std::printf("  ticks at boot = %lld\n",
              static_cast<long long>(system.GetCoreTiming().GetTicks()));

  const u32 pc_before = system.GetPPCState().pc;
  bool booted = false;
  {
    const Core::CPUThreadGuard guard(system);
    INIT_MARK("CBoot::BootUp");
    booted = CBoot::BootUp(system, guard, std::move(boot));
  }
  check(booted, "CBoot::BootUp returned success");

  // Arm GPU/CPU syncing. Core.cpp:656 does this immediately after boot, and it
  // is not optional: FifoManager::Prepare() both registers the SyncGPUCallback
  // CoreTiming event and sets m_syncing_suspended = true. The member defaults
  // to FALSE, and RunGpu() only schedules the callback when it is true -- so
  // without this call RunGpu() does nothing at all, the GPU never consumes the
  // FIFO, and the FIFO just fills up.
  //
  // That was the stall: the game wrote 2,336 bytes of GX set-up, the read
  // pointer never moved off CPBase (rwdist grew, rd == base), and the game sat
  // in a three-instruction wait loop at 0x803873a0 for ten billion
  // instructions. A default-false flag and a missing one-line call.
  system.GetFifo().Prepare();

  auto& state = system.GetPPCState();
  std::printf("  entry pc  0x%08x (was 0x%08x)\n", state.pc, pc_before);
  std::printf("  msr=%08x  r1(sp)=%08x\n", state.msr.Hex, state.gpr[1]);

  // Region and the low-memory video-mode global.
  //
  // Boot_BS2Emu.cpp:512 writes IsNTSC(region) ? 0 : 1 to 0x800000CC ("Fake the
  // VI Init of the IPL"), and the game reads it back with `lwz r24, 204(r3)`
  // at pc 0x803dfd00. If the two targets disagree about region they diverge
  // there -- 59.7 million instructions after boot, which is a long way to
  // travel to discover a configuration difference. Printing it here makes that
  // class of mismatch visible immediately.
  std::printf("  region    %d  lowmem[0x800000CC]=%08x\n",
              static_cast<int>(SConfig::GetInstance().m_region),
              system.GetMemory().Read_U32(0x800000CC));
  check(state.pc != 0 && state.pc != pc_before, "boot left the CPU at an entry point");

  // ── execute ──
  INIT_MARK("stepping");
  std::printf("\n  stepping %llu instructions...\n", static_cast<unsigned long long>(steps));
#if defined(__EMSCRIPTEN__)
  const double step_t0 = JitNow();
#endif
  u64 executed = 0;
  u32 watch_last = 0;
  u64 frame_index = 0;
  std::map<u32, std::pair<u64, u32>> cov_counts;
  u64 cov_total = 0;
  Memory::MemoryManager* mem_for_cov = &system.GetMemory();
  const u32 first_pc = state.pc;
  u32 min_pc = state.pc, max_pc = state.pc;

  // ── the block cache ──
  //
  // One compiled block per guest PC, compiled the first time that PC is
  // reached. Entries with a negative index are PCs where nothing worth
  // compiling starts, and are remembered so the compiler is not re-run on
  // every visit to the same spot.
  struct JitEntry
  {
    int index = -1;       // table index, which is a callable function pointer
    u32 instructions = 0;
    u8 stop = 0;          // 0 branch, 1 unsupported instruction, 2 hit the limit
    bool retired = false; // compiled, then observed to refuse its first instruction
  };
  std::unordered_map<u32, JitEntry> jit_cache;
  u64 jit_blocks = 0, jit_insts = 0, jit_interp = 0, jit_bails = 0;

  // ── where the time goes ──
  //
  // Translation and installation happen once per UNIQUE block, not once per
  // round-trip, so timing them individually costs nothing measurable and
  // needs no sampling. They are also the two costs the ranked plan in
  // docs/PHASE2-HANDOFF.md does not list, which is reason enough to measure
  // them before the three it does.
  double jit_t_translate = 0.0, jit_t_install = 0.0;
  u64 jit_translates = 0, jit_installs = 0, jit_batched = 0;
  // How many blocks each region actually gathered, bucketed 1,2,3,4,5-8,
  // 9-15,16. "Blocks per module" is an average and averages hide the
  // shape: a handful of large regions and a flood of singletons reads the
  // same as everything being mid-sized, and only one of those has a fix.
  u64 region_hist[7] = {0, 0, 0, 0, 0, 0, 0};
  u64 succ_edges = 0, succ_sibling = 0;
  // A sink the optimiser cannot see through, for probes whose result is
  // otherwise unused.
  volatile u64 jit_probe_sink = 0;

  // ── why blocks are short, and what the interpreter is left holding ──
  //
  // Blocks average 2.82 instructions against a branch-density ceiling of
  // about 4.5, so something other than branches is ending them. Counting
  // the reason separates "compile more instruction forms" from "compile
  // across branches", which are different projects.
  //
  // Static (per distinct block) and dynamic (per dispatch) are both kept.
  // A reason that ends thousands of cold blocks and a reason that ends one
  // block entered ten million times need different answers, and a single
  // count cannot tell them apart.
  u64 stop_static[3] = {0, 0, 0}, stop_insts[3] = {0, 0, 0}, stop_dyn[3] = {0, 0, 0};
  // What the interpreter runs, keyed the way --jit-coverage keys it, split
  // by WHY it is here. The two causes have different fixes and the totals
  // alone cannot tell them apart:
  //
  //   never compiled  the form is not translated at all
  //   retired         it compiled, then refused its first instruction every
  //                   time, so the dispatcher dropped it -- which is what a
  //                   MEM2 access looks like, and why MEM2 shows up as a
  //                   small bail count rather than as the cost it is
  std::map<u32, std::pair<u64, u32>> interp_counts, interp_retired;
  u64 jit_retired_blocks = 0, interp_from_retired = 0;
#if defined(__EMSCRIPTEN__)
  ppc2wasm::StateLayout jit_layout;
  jit_layout.gpr_offset = static_cast<u32>(offsetof(PowerPC::PowerPCState, gpr));
  jit_layout.cr_offset = static_cast<u32>(offsetof(PowerPC::PowerPCState, cr));
  jit_layout.xer_so_ov_offset = static_cast<u32>(offsetof(PowerPC::PowerPCState, xer_so_ov));
  jit_layout.spr_offset = static_cast<u32>(offsetof(PowerPC::PowerPCState, spr));
  jit_layout.cr_table_base =
      static_cast<u32>(reinterpret_cast<uintptr_t>(PowerPC::ConditionRegister::s_crTable.data()));
  jit_layout.ram_base =
      static_cast<u32>(reinterpret_cast<uintptr_t>(system.GetMemory().GetRAM()));
  jit_layout.memory_pages = static_cast<u32>(JitMemoryPages());
  jit_layout.msr_offset = static_cast<u32>(offsetof(PowerPC::PowerPCState, msr));
  jit_layout.downcount_offset = static_cast<u32>(offsetof(PowerPC::PowerPCState, downcount));

  // ── the link table ──
  //
  // A direct-mapped cache from guest PC to table index, read by emitted code
  // so a block can jump to its successor without returning here. Entries are
  // {pc, index}; a slot collision is a miss, which is correct and cheap,
  // because the emitted code compares the stored PC before jumping.
  // Sized by the ADDRESS SPAN of guest code, not by the number of blocks.
  //
  // That distinction is the whole of this constant. The slot is
  // (pc >> 2) & mask, so a table of N entries separates N*4 bytes of code
  // and everything further apart than that aliases onto it. This game's
  // code spans 5.80 MB, which is 1,450,029 distinct (pc >> 2) values; at
  // 1<<19 the table covered 2.1 MB, so up to three hot PCs shared every
  // slot and evicted each other forever. 1<<21 covers 8.4 MB, and the
  // aliasing disappears rather than being reduced.
  //
  // Measured over 800M instructions, of 9,580,876 missed exits only 3.1%
  // now have a successor that was compiled -- down from 58.1%.
  //
  // The cost is 16 MB of the host heap, up from 4 MB. The alternative --
  // keeping the table small and mixing high bits into the hash -- costs two
  // wasm instructions in an epilogue executed 277 million times, which is
  // the more expensive side of the trade by a wide margin.
  //
  // A game with more than 8.4 MB of hot code would alias again. The counter
  // that detects it is miss_but_cached, below: if that climbs back toward
  // half, this number is why.
  constexpr u32 kLinkEntries = 1u << 21;
  std::vector<u32> link_table(kLinkEntries * 2, 0);
  jit_layout.link_base = static_cast<u32>(reinterpret_cast<uintptr_t>(link_table.data()));
  jit_layout.link_mask = kLinkEntries - 1;
  // hits, attempts, budget exits, misses
  u32 link_stats[4] = {0, 0, 0, 0};
  jit_layout.link_stats_addr = static_cast<u32>(reinterpret_cast<uintptr_t>(link_stats));
  // Of the exits that missed, how many had a compiled successor anyway? That
  // separates a publishing or lookup fault from a successor that genuinely
  // was not compiled yet.
  u64 miss_but_cached = 0, miss_total = 0;
  jit_layout.enable_linking = want_link;

  // A block that does nothing but return its argument, for probe 3. It
  // isolates the indirect call itself from anything the call does.
  int jit_probe_fn = -1;
  if (jit_probe == 3)
  {
    ppc2wasm::ModuleBuilder pmb;
    pmb.ImportMemory(jit_layout.memory_pages, jit_layout.memory_pages, true);
    ppc2wasm::Bytes pbody;
    pbody.push_back(ppc2wasm::kLocalGet);
    ppc2wasm::PutU32(pbody, 0);
    pmb.SetFunction({ppc2wasm::kI32}, {ppc2wasm::kI32}, pbody, "f", 0);
    const auto pm = pmb.Build();
    jit_probe_fn = JitInstall(pm.data(), static_cast<int>(pm.size()));
  }

  // How many guest instructions a chain may run before handing control back
  // so the emulated clock can advance. Short enough that scheduled events are
  // not noticeably late, long enough that dispatch is amortised.
  constexpr int kSlice = 1024;
  const u32 jit_state_ptr = static_cast<u32>(reinterpret_cast<uintptr_t>(&state));
  auto& jit_timing = system.GetCoreTiming();

  // Every block gets compiled, however short.
  //
  // The threshold used to be four, on the reasoning that installing a module
  // costs more than interpreting a handful of instructions. That is true in
  // isolation and wrong in aggregate: a block that is not compiled is not a
  // LINK TARGET either, so every chain reaching it has to stop. Measured,
  // 99.7% of link misses were successors that had simply never been
  // compiled -- not collisions, not a stale table, not the budget.
  constexpr u32 kMinBlock = 1;
  constexpr u32 kMaxBlock = 64;
#endif

  // ── region compilation ──
  //
  // On a cache miss, compile the block at that PC AND the blocks its static
  // successors start, and install all of them as ONE module.
  //
  // Installation costs about 22 us per module and almost nothing per block
  // inside it (harness/wasm-core/jitbatch.cpp). One block per module made
  // that 27.8% of an 800M-instruction run -- the second largest cost in the
  // profile. Sixteen blocks per module pays it once.
  //
  // Following successors rather than queueing blocks as they are discovered
  // is what keeps this from costing latency: every block in the batch is
  // callable the moment it is installed, so nothing hot waits behind
  // unrelated discoveries to become compiled.
  constexpr u32 kRegion = 16;
  auto compile_region = [&](u32 seed) {
    // Candidate PCs, bounded to MEM1 with room for a full block read.
    //
    // A successor is a GUESS about reachability, and a wrong one points at
    // data or at nothing. Reading guest memory outside the mapped range
    // makes Dolphin's CopyFromEmu panic, so the bound is a precondition
    // here and not a nicety.
    u32 queue[kRegion * 3];
    u32 qn = 0, qi = 0;
    auto push = [&](u32 p) {
      if (qn >= kRegion * 3)
        return;
      if ((p & 0xFE000000) != 0x80000000)
        return;
      if ((p & 0x01FFFFFF) + kMaxBlock * 4 > 0x1800000)
        return;
      // Stay near the seed.
      //
      // There is no block invalidation anywhere in this recompiler, so a
      // translation made now is used forever. Compiling a block the
      // instant before it runs kept that hazard narrow; compiling ahead
      // widens it, because the guest has longer to overwrite the code --
      // a DMA'd overlay being the case that would actually do it.
      //
      // This bound does not fix that and is not pretending to. It keeps
      // speculation inside code the guest is demonstrably executing right
      // now, which is the part least likely to be swapped out from under
      // us, and it costs nothing: a megabyte is far wider than any
      // function's own branches.
      const u32 d = p > seed ? p - seed : seed - p;
      if (d > (1u << 20))
        return;
      for (u32 i = 0; i < qn; ++i)
        if (queue[i] == p)
          return;
      queue[qn++] = p;
    };
    push(seed);

    ppc2wasm::ModuleBuilder mb;
    mb.ImportMemory(jit_layout.memory_pages, jit_layout.memory_pages, jit_layout.memory_shared);
    if (want_link)
      mb.ImportTable();
    mb.SetSignature({ppc2wasm::kI32}, {ppc2wasm::kI32});

    struct Pending
    {
      u32 pc;
      u32 instructions;
      u8 stop;
      u32 succ[2];
      u32 nsucc;
    };
    std::vector<Pending> pending;

    // When the successor walk runs dry before the batch is full, keep
    // going LINEARLY from the far end of what the region covered.
    //
    // Following successors alone averaged 4.3 blocks per module rather
    // than sixteen: by the time a region is compiled its neighbours
    // usually are too, so later misses are isolated PCs reached by an
    // indirect branch whose static successors are all known already.
    // Guest code is laid out contiguously, so the next block in memory is
    // very likely the next block to run.
    //
    // Guessing wrong costs translation time and module bytes and nothing
    // else: a PC that is really data refuses on its first instruction and
    // is recorded as "interpret this", which is the same verdict the
    // compiler would reach if the PC were reached for real.
    u32 frontier = 0;
    const double t_translate0 = JitNow();
    while (pending.size() < kRegion)
    {
      if (qi >= qn)
      {
        if (!frontier)
          break;
        const u32 f = frontier;
        frontier = 0;
        const u32 before = qn;
        push(f);
        if (qn == before)
          break;  // out of bounds, or already queued
      }
      const u32 bpc = queue[qi++];
      if (jit_cache.count(bpc))
        continue;
      u32 code[kMaxBlock];
      for (u32 k = 0; k < kMaxBlock; ++k)
        code[k] = system.GetMemory().Read_U32(bpc + k * 4);
      // Bodies only: this packs them into a shared module itself, so the
      // single-function module CompileBlock would otherwise build is
      // scaffolding around a copy of code that is about to be copied
      // again.
      auto r = ppc2wasm::CompileBlock(code, bpc, kMaxBlock, jit_layout, false);
      ++jit_translates;
      const u8 stop = (std::strcmp(r.stopped_on, "branch") == 0) ? 0
                      : (std::strcmp(r.stopped_on, "unsupported instruction") == 0)
                          ? 1
                          : 2;
      ++stop_static[stop];
      stop_insts[stop] += r.instructions;
      if (r.instructions >= kMinBlock && !r.body.empty())
      {
        mb.AddFunction(r.body, "f" + std::to_string(pending.size()), r.locals);
        pending.push_back({bpc, r.instructions, stop, {r.succ[0], r.succ[1]}, r.nsucc});
        for (u32 i = 0; i < r.nsucc; ++i)
          push(r.succ[i]);
        // The next address in memory after this block, which is past the
        // refused instruction when that is why it stopped.
        const u32 linear = r.end_pc + (stop == 1 ? 4u : 0u);
        if (linear > frontier)
          frontier = linear;
      }
      else
      {
        // Nothing compiled here. Remembered so the compiler is not re-run
        // on every visit, exactly as before -- the only difference is that
        // this PC may have been reached by guessing rather than by running.
        JitEntry none;
        none.stop = stop;
        jit_cache.emplace(bpc, none);
      }
    }
    jit_t_translate += JitNow() - t_translate0;

    // The seed MUST end up in the cache, compiled or not. Everything
    // downstream does jit_cache.find(pc) and dereferences the result, and
    // the walk can decline to look at the seed at all -- a PC outside MEM1
    // is rejected by the bound above before anything reads it. A negative
    // entry there means "interpret this", which is the right answer and
    // the one the old one-block-at-a-time path produced by accident.
    auto ensure_seed = [&]() {
      if (!jit_cache.count(seed))
        jit_cache.emplace(seed, JitEntry{});
    };

    if (pending.empty())
    {
      ensure_seed();
      return;
    }

    const double t_install0 = JitNow();
    const auto bytes = mb.Build();
    const int base =
        JitInstallBatch(bytes.data(), static_cast<int>(bytes.size()),
                        static_cast<int>(pending.size()));
    jit_t_install += JitNow() - t_install0;
    ++jit_installs;
    jit_batched += pending.size();
    {
      const size_t n = pending.size();
      const int bucket = n <= 4 ? static_cast<int>(n) - 1 : (n <= 8 ? 4 : (n < 16 ? 5 : 6));
      ++region_hist[bucket];
    }

    // ── how much direct linking could reach ──
    //
    // A statically known successor can be called DIRECTLY, with no hash,
    // no table load and no PC compare, only when it is a sibling in the
    // same module. Counting that before building it, because the prize is
    // bounded by it and regions average under four blocks.
    //
    // Static, not dynamic: a tail call never passes through C++, so this
    // counts successor EDGES rather than executions of them.
    for (const Pending& pb : pending)
    {
      for (u32 i = 0; i < pb.nsucc; ++i)
      {
        ++succ_edges;
        for (const Pending& other : pending)
          if (other.pc == pb.succ[i])
          {
            ++succ_sibling;
            break;
          }
      }
    }

    for (size_t k = 0; k < pending.size(); ++k)
    {
      JitEntry e;
      e.index = (base < 0) ? -1 : base + static_cast<int>(k);
      e.instructions = pending[k].instructions;
      e.stop = pending[k].stop;
      if (e.index >= 0)
      {
        // Publish it so other blocks can jump here directly.
        const u32 slot = (pending[k].pc >> 2) & jit_layout.link_mask;
        link_table[slot * 2 + 0] = pending[k].pc;
        link_table[slot * 2 + 1] = static_cast<u32>(e.index);
      }
      jit_cache.emplace(pending[k].pc, e);
    }
    ensure_seed();
  };

  // Run one step, through a compiled block when there is one. Returns how
  // many guest instructions were executed.
  auto step_once = [&]() -> u32 {
#if defined(__EMSCRIPTEN__)
    if (!want_jit)
    {
      ppc.SingleStep();
      return 1;
    }
    const u32 pc = state.pc;
    auto it = jit_cache.find(pc);
    if (it == jit_cache.end())
    {
      compile_region(pc);
      it = jit_cache.find(pc);
    }
    if (it->second.index < 0)
    {
      ++jit_interp;
      // What the interpreter is left holding. Keyed as --jit-coverage keys
      // it so the two rankings can be read side by side -- the interesting
      // comparison is an instruction that is RARE in the stream and common
      // here, which is what a missing form looks like.
      if (want_jit_stats)
      {
        const u32 ins = system.GetMemory().Read_U32(state.pc);
        const u32 primary = ins >> 26;
        const u32 key = (primary == 31 || primary == 19 || primary == 63)
                            ? (primary << 16) | (((ins >> 1) & 0x3FF) << 1) | (ins & 1)
                            : (primary << 16);
        auto& slot = (it->second.retired ? interp_retired : interp_counts)[key];
        ++slot.first;
        slot.second = ins;
        if (it->second.retired)
          ++interp_from_retired;
      }
      ppc.SingleStep();
      return 1;
    }
    if (want_jit_stats)
      ++stop_dyn[it->second.stop];

    // The translation guard, once per chain rather than once per block.
    //
    // Compiled code maps guest addresses with ram_base + (ea & mask),
    // which is only right while MSR[DR] is set. That used to be a prologue
    // on every memory-touching block; MSR cannot change inside a chain --
    // mtmsr ends its block and returns here -- so one check before
    // entering is exactly as strong, and it is a C++ branch instead of
    // five wasm instructions at 261 million block entries.
    //
    // Checked for every block, not just the ones that touch memory,
    // because a chain can tail-call from a block that does not into one
    // that does, and the dispatcher cannot see where a chain will go.
    if (!state.msr.DR)
    {
      ++jit_interp;
      ppc.SingleStep();
      return 1;
    }

    using BlockFn = u32 (*)(u32);
    BlockFn f = reinterpret_cast<BlockFn>(it->second.index);

    // How far the call got is measured differently depending on whether
    // blocks link.
    //
    // A LINKED block decrements the downcount itself and may tail-call
    // several successors, so what returns is the end of a chain and the only
    // record of its length is what the budget lost. An UNLINKED block never
    // touches the downcount, so reading it would always say zero -- which is
    // exactly what happened: every block fell back to the interpreter and
    // the JIT reported 0% while quietly costing a compile per block.
    // Only a linked chain needs a budget. Writing the downcount for an
    // unlinked block perturbs the emulated clock for no reason -- and
    // measurably changed what the game did, which is the sort of thing a
    // dispatcher must never do by accident.
    if (want_link)
      state.downcount = kSlice;
    const u32 ret = f(jit_state_ptr);

    const bool bailed = (ret & 1u) != 0;
    const bool msr_changed = (ret & 2u) != 0;
    const u32 next_pc = ret & ~3u;
    state.pc = state.npc = next_pc;
    if (bailed)
      ++jit_bails;

    int consumed;
    if (want_link)
    {
      consumed = kSlice - state.downcount;
    }
    else
    {
      // One block, so the PC says how much of it ran: a bail lands inside the
      // block, anything else is the whole of it.
      consumed = bailed ? static_cast<int>((next_pc - pc) / 4)
                        : (msr_changed ? static_cast<int>((next_pc - pc) / 4)
                                       : static_cast<int>(it->second.instructions));
    }
    if (consumed < 0)
      consumed = 0;

    if (consumed == 0)
    {
      // The block refused on its very first instruction and did nothing.
      //
      // Compiling every block, however short, is what makes a successor
      // exist to link to -- but it also creates blocks whose first
      // instruction always refuses, usually a memory access outside MEM1.
      // Those cost a call and a chain step and never execute anything, and
      // with linking on they end a chain that was otherwise running.
      //
      // Retire it: drop the entry and clear its link slot so chains stop
      // routing through a block that cannot make progress. The decision is
      // made from OBSERVED behaviour, which is the only way -- whether an
      // address lands in MEM1 is not knowable when the block is compiled.
      it->second.index = -1;
      it->second.retired = true;
      ++jit_retired_blocks;
      const u32 slot = (pc >> 2) & jit_layout.link_mask;
      if (link_table[slot * 2 + 0] == pc)
      {
        link_table[slot * 2 + 0] = 0;
        link_table[slot * 2 + 1] = 0;
      }
      ++jit_interp;
      ppc.SingleStep();
      return 1;
    }

    // Advance the emulated clock by what the chain actually ran. Without
    // this nothing scheduled ever fires: no vertical retrace, no IPC reply,
    // and the game waits forever on a flag an interrupt was meant to set.
    //
    // Events that would have fired mid-chain fire at its end, so the slice
    // size sets timing granularity. That is a real behavioural difference
    // from the interpreter, not an implementation detail.
    if (want_link && !bailed && !msr_changed)
    {
      // This exit came back to the dispatcher, so the chain stopped here.
      // Was its successor something we had already compiled?
      ++miss_total;
      auto nx = jit_cache.find(next_pc);
      if (nx != jit_cache.end() && nx->second.index >= 0)
        ++miss_but_cached;
    }

    if (msr_changed)
    {
      // The block wrote MSR and stopped. Everything the interpreter does
      // after that write happens here: translation state is recomputed, and
      // any interrupt that was already pending is taken -- which is the
      // whole reason enabling MSR[EE] cannot be a plain store.
      ppc.MSRUpdated();
      ppc.CheckExceptions();
      state.pc = state.npc;
    }

    jit_timing.GetGlobals().slice_length = consumed;
    state.downcount = 0;
    jit_timing.Advance();

    // ── per-round-trip probes ──
    //
    // performance.now() resolves to 5 us even in a cross-origin-isolated
    // page, and a round-trip is on the order of 100 ns, so these cannot be
    // timed one at a time. They are timed by DOING ONE TWICE: run the boot
    // with a probe on, subtract the baseline, divide by the round-trip
    // count. The clock is only read at the ends of the run, where 5 us
    // against four seconds does not matter.
    //
    // Each probe repeats work that is already happening, so none of them
    // changes what the guest does -- which is the one thing a dispatcher
    // must never do by accident (see the landmine in the handoff).
    switch (jit_probe)
    {
    case 1:  // the cache lookup
      jit_probe_sink += (jit_cache.find(next_pc) != jit_cache.end()) ? 1 : 0;
      break;
    case 2:  // CoreTiming::Advance, with no time to advance by
      jit_timing.GetGlobals().slice_length = 0;
      state.downcount = 0;
      jit_timing.Advance();
      break;
    case 3:  // one indirect call through the table, into a block that returns
      if (jit_probe_fn >= 0)
        jit_probe_sink += reinterpret_cast<u32 (*)(u32)>(jit_probe_fn)(jit_state_ptr);
      break;
    default:
      break;
    }

    ++jit_blocks;
    jit_insts += static_cast<u64>(consumed);
    return static_cast<u32>(consumed);
#else
    ppc.SingleStep();
    return 1;
#endif
  };

  // ── FIFO milestones ──
  // The Phase 1 gate is the first rendered frame's FIFO write, so the run has
  // to report when the game first touches the graphics FIFO rather than only
  // how far the PC got.
  //
  // Two distinct events, recorded separately because they answer different
  // questions:
  //
  //   configured  CPBase/CPEnd become non-zero -- the game has told the
  //               Command Processor where its FIFO lives. This proves the CP
  //               register block is wired and the writes are landing.
  //
  //   written     CPWritePointer moves -- actual command data reached the
  //               FIFO. This is the gate's event.
  //
  // CommandProcessor::GetFifo() is public, so neither needs a Dolphin patch.
  // The gather pipe itself would be the earliest possible signal, but
  // GPFifo::GetGatherPipeCount() is private and a flush lands here anyway.
  auto& fifo = system.GetCommandProcessor().GetFifo();
  const u32 wp_initial = fifo.CPWritePointer.load();
  s64 fifo_configured_at = -1, fifo_written_at = -1;
  u32 fifo_configured_pc = 0, fifo_written_pc = 0;

  for (u64 i = 0; i < steps; ++i)
  {
    // Liveness, not logging. The stepping phase produces NO output for
    // minutes at a time -- a 256 KB capture takes ~4 billion instructions --
    // so a watchdog based on output alone cannot tell a working run from a
    // wedged one. Folding the step count into the stage marker gives the
    // browser-side poller something that advances on its own, which is what
    // makes a stall timeout safe to apply at all.
    //
    // Every 2^22 steps (~4M) so the cost is nothing and the marker still
    // moves several times a minute under the wasm interpreter.
    if ((i & 0x3FFFFF) == 0)
    {
      char buf[96];
      std::snprintf(buf, sizeof(buf), "stepping %lluM",
                    static_cast<unsigned long long>(i / 1000000));
      InitMark(buf);
    }

    // Events are logged over the same window as the trace. --events turns
    // logging on from the first step, which --from cannot express: a window
    // starting at zero is indistinguishable from "no window".
    g_cur_step = i;
    if (want_input && !g_input_sequence.empty())
    {
      if ((i % PRESS_PERIOD) < PRESS_LENGTH)
      {
        const auto& e = g_input_sequence[(i / PRESS_PERIOD) % g_input_sequence.size()];
        g_press_group = e.first;
        g_press_control = e.second;
      }
      else
      {
        g_press_group = "";
        g_press_control = "";
      }
    }
    if (i >= trace_from && (want_events || trace_from))
      g_event_log_on = true;

    if (i >= trace_from && (want_trace || (checkpoint && i % checkpoint == 0)))
      Trace::EmitLine(i, state);
    if (frame_every && i && (i % frame_every) == 0)
    {
      const u32 xfb = system.GetVideoInterface().GetXFBAddressTop();
      if (xfb)
      {
        const auto yuv = ReadXfbYuv(system, xfb);
        std::printf("X frame=%llu step=%llu addr=%08x hash=%016llx\n",
                    static_cast<unsigned long long>(frame_index),
                    static_cast<unsigned long long>(i), xfb,
                    static_cast<unsigned long long>(HashFrame(yuv)));
#if !defined(__EMSCRIPTEN__)
        if (!xfb_path.empty())
        {
          char name[512];
          std::snprintf(name, sizeof(name), "%s-%04llu.ppm", xfb_path.c_str(),
                        static_cast<unsigned long long>(frame_index));
          WritePpm(name, YuvToRgb(yuv));
        }
#endif
        ++frame_index;
      }
    }

    if (want_coverage)
    {
      // Key by primary opcode, and for the extended-opcode space by its XO
      // too -- "opcode 31" spans everything from add to the cache hints, so a
      // tally that lumps them together says nothing useful.
      const u32 ins = mem_for_cov->Read_U32(state.pc);
      const u32 primary = ins >> 26;
      // The Rc bit is part of the key. Without it `add` and `add.` share a
      // bucket, one example encoding stands for both, and an opcode whose
      // dot form is refused is reported as translated.
      // mfspr and mtspr are keyed by WHICH register, because whether they
      // can be translated depends entirely on that and not on the opcode.
      const u32 xo_now = (ins >> 1) & 0x3FF;
      if (primary == 31 && (xo_now == 339 || xo_now == 467))
      {
        const u32 sprn = ((ins >> 16) & 0x1F) | (((ins >> 11) & 0x1F) << 5);
        auto& sl = cov_counts[(31u << 16) | (0xF000u) | (xo_now == 339 ? 0u : 0x800u) | sprn];
        ++sl.first;
        sl.second = ins;
        ++cov_total;
        goto cov_done;
      }
      {
      const u32 key = (primary == 31 || primary == 19 || primary == 63)
                          ? (primary << 16) | (((ins >> 1) & 0x3FF) << 1) | (ins & 1)
                          : (primary << 16);
      auto& slot = cov_counts[key];
      ++slot.first;
      // Keep a REAL encoding, not a reconstructed one. Whether an instruction
      // translates can depend on fields outside the opcode -- a bc with BO
      // bit 2 clear is a CTR form and refused, so a synthetic BO=0 probe
      // reports every conditional branch as unsupported.
      slot.second = ins;
      ++cov_total;
      }
    cov_done:;
    }

    if (watch_addr)
    {
      const u32 now = system.GetMemory().Read_U32(watch_addr);
      if (now != watch_last)
      {
        std::printf("W step=%llu addr=%08x %08x -> %08x pc=%08x\n",
                    static_cast<unsigned long long>(i), watch_addr, watch_last, now, state.pc);
        watch_last = now;
      }
    }

    if (dump_at && i == dump_at)
    {
      Trace::EmitFullState(i, state);
      // The instruction words either side of the PC. Code comes from the disc
      // and is identical on both targets, so this only needs reading once --
      // but printing it on both proves that assumption rather than trusting it.
      auto& mem = system.GetMemory();
      for (u32 a = state.pc - 16; a <= state.pc + 16; a += 4)
        std::printf("D   code %08x: %08x%s\n", a, mem.Read_U32(a),
                    a == state.pc ? "   <- pc" : "");
      // Paired singles too: the float-contraction risk in docs/PLAN.md lands
      // here, and a GPR-only dump would miss it.
      for (int r = 0; r < 32; ++r)
      {
        if (state.ps[r].PS0AsU64() != 0 || state.ps[r].PS1AsU64() != 0)
          std::printf("D   ps%-2d=%016llx %016llx\n", r,
                      static_cast<unsigned long long>(state.ps[r].PS0AsU64()),
                      static_cast<unsigned long long>(state.ps[r].PS1AsU64()));
      }
    }
    const u32 ran_now = step_once();
    i += ran_now - 1;  // the for-loop's own ++i accounts for the first
    executed += ran_now;
    min_pc = std::min(min_pc, state.pc);
    max_pc = std::max(max_pc, state.pc);

    if (fifo_configured_at < 0 && fifo.CPBase.load() != 0)
    {
      fifo_configured_at = static_cast<s64>(i);
      fifo_configured_pc = state.pc;
    }
    if (fifo_written_at < 0 && fifo.CPWritePointer.load() != wp_initial)
    {
      fifo_written_at = static_cast<s64>(i);
      fifo_written_pc = state.pc;
    }

    // A PC of zero means execution fell off into nothing, which is a failure
    // worth stopping on rather than tracing thousands of lines of it.
    if (state.pc == 0)
      break;
    // Stop as soon as the gate's event happens, unless a fixed step count was
    // asked for. Running past it wastes minutes of browser time.
    if (stop_at_fifo && fifo_written_at >= 0)
      break;
    if (g_fifo_limit && g_fifo_bytes.size() >= g_fifo_limit)
      break;
    if (g_gx_limit && g_gx_bytes.size() >= g_gx_limit)
      break;
  }

#if defined(__EMSCRIPTEN__)
  {
    // Wall time for the stepping phase only. This is the number the whole
    // recompiler exists to move, and it is reported whether or not --jit is
    // on so the two runs are directly comparable.
    const double ms = JitNow() - step_t0;
    std::printf("  speed     %.0f ms, %.1f MIPS  (%s)\n", ms,
                executed / (ms / 1000.0) / 1e6, want_jit ? "jit" : "interpreter");
  }
#endif
  std::printf("  executed  %llu instructions\n", static_cast<unsigned long long>(executed));
  std::printf("  pc range  0x%08x .. 0x%08x (started 0x%08x, now 0x%08x)\n", min_pc, max_pc,
              first_pc, state.pc);
#if defined(__EMSCRIPTEN__)
  if (want_jit)
  {
    const double covered = executed ? 100.0 * static_cast<double>(jit_insts) /
                                          static_cast<double>(executed)
                                    : 0.0;
    std::printf("  jit       %llu blocks cached, %llu compiled-block entries\n",
                static_cast<unsigned long long>(jit_cache.size()),
                static_cast<unsigned long long>(jit_blocks));
    std::printf("  jit       %llu instructions in compiled code (%.1f%% of executed)\n",
                static_cast<unsigned long long>(jit_insts), covered);
    {
      // How much of the link table is live, and how long chains actually run.
      // A chain that averages one block is not linking, and the difference
      // between "published" and "hit" is the whole mechanism.
      u64 live = 0;
      for (u32 i = 0; i < kLinkEntries; ++i)
        if (link_table[i * 2 + 1] != 0)
          ++live;
      std::printf("  jit       link hits %u, attempts %u, budget exits %u, misses %u\n",
                  link_stats[0], link_stats[1], link_stats[2], link_stats[3]);
      std::printf("  jit       of %llu missed exits, %llu had a compiled successor (%.1f%%)\n",
                  static_cast<unsigned long long>(miss_total),
                  static_cast<unsigned long long>(miss_but_cached),
                  miss_total ? 100.0 * static_cast<double>(miss_but_cached) /
                                   static_cast<double>(miss_total)
                             : 0.0);
      std::printf("  jit       translate %.0f ms over %llu blocks (%.1f us each)\n",
                  jit_t_translate, static_cast<unsigned long long>(jit_translates),
                  jit_translates ? jit_t_translate * 1000.0 /
                                       static_cast<double>(jit_translates)
                                 : 0.0);
      std::printf("  jit       install   %.0f ms over %llu modules holding %llu blocks "
                  "(%.1f blocks each, %.2f us per block)\n",
                  jit_t_install, static_cast<unsigned long long>(jit_installs),
                  static_cast<unsigned long long>(jit_batched),
                  jit_installs ? static_cast<double>(jit_batched) /
                                     static_cast<double>(jit_installs)
                               : 0.0,
                  jit_batched ? jit_t_install * 1000.0 / static_cast<double>(jit_batched)
                              : 0.0);
      if (jit_probe)
        std::printf("  jit       probe %d active -- subtract a baseline run\n", jit_probe);
      // Instructions per BLOCK ENTRY, which is a different number from
      // instructions per chain and a more important one. A chain is how far
      // control runs before the dispatcher sees it again; a block entry is
      // how often the fixed cost of entering compiled code -- function
      // prologue, the MSR[DR] guard, the whole link epilogue -- is paid.
      // Linking lengthens chains without changing this at all.
      const u64 entries = jit_blocks + static_cast<u64>(link_stats[0]);
      {
        static const char* kLbl[7] = {"1", "2", "3", "4", "5-8", "9-15", "16"};
        std::printf("  jit       region sizes:");
        for (int k = 0; k < 7; ++k)
          std::printf("  %s=%llu", kLbl[k], static_cast<unsigned long long>(region_hist[k]));
        std::printf("\n");
      }
      std::printf("  jit       %llu static successor edges, %llu are siblings (%.1f%%)\n",
                  static_cast<unsigned long long>(succ_edges),
                  static_cast<unsigned long long>(succ_sibling),
                  succ_edges ? 100.0 * static_cast<double>(succ_sibling) /
                                   static_cast<double>(succ_edges)
                             : 0.0);
      std::printf("  jit       %llu block entries, %.2f instructions per entry\n",
                  static_cast<unsigned long long>(entries),
                  entries ? static_cast<double>(jit_insts) / static_cast<double>(entries) : 0.0);
      if (want_jit_stats)
      {
        static const char* kStop[3] = {"branch", "unsupported", "limit"};
        for (int k = 0; k < 3; ++k)
          std::printf("  jit       ends on %-12s %7llu blocks (%.2f insts avg), "
                      "%llu dispatches\n",
                      kStop[k], static_cast<unsigned long long>(stop_static[k]),
                      stop_static[k] ? static_cast<double>(stop_insts[k]) /
                                           static_cast<double>(stop_static[k])
                                     : 0.0,
                      static_cast<unsigned long long>(stop_dyn[k]));
      }
      if (want_jit_stats)
      {
        // The top of what the interpreter runs. 2.1% of the stream costing
        // 13% of the time is only worth acting on if it concentrates.
        std::printf("  jit       %llu blocks retired, %llu instructions interpreted "
                    "through them (%.1f%% of all interpreted)\n",
                    static_cast<unsigned long long>(jit_retired_blocks),
                    static_cast<unsigned long long>(interp_from_retired),
                    jit_interp ? 100.0 * static_cast<double>(interp_from_retired) /
                                     static_cast<double>(jit_interp)
                               : 0.0);
        const char* label[2] = {"never-compiled", "retired      "};
        std::map<u32, std::pair<u64, u32>>* maps[2] = {&interp_counts, &interp_retired};
        for (int m = 0; m < 2; ++m)
        {
          std::vector<std::pair<u64, u32>> top;
          for (const auto& kv : *maps[m])
            top.push_back({kv.second.first, kv.first});
          std::sort(top.begin(), top.end(), std::greater<>());
          u64 tot = 0;
          for (const auto& t : top)
            tot += t.first;
          for (size_t k = 0; k < top.size() && k < 8; ++k)
            std::printf("  jit       %s %zu: primary %2u xo %4u  %10llu  %5.1f%%  (%08x)\n",
                        label[m], k + 1, top[k].second >> 16, (top[k].second & 0xFFFF) >> 1,
                        static_cast<unsigned long long>(top[k].first),
                        tot ? 100.0 * static_cast<double>(top[k].first) /
                                  static_cast<double>(tot)
                            : 0.0,
                        (*maps[m])[top[k].second].second);
        }
      }
      std::printf("  jit       link table %llu/%u slots live, %.2f instructions per chain\n",
                  static_cast<unsigned long long>(live), kLinkEntries,
                  jit_blocks ? static_cast<double>(jit_insts) / static_cast<double>(jit_blocks)
                             : 0.0);
    }
    std::printf("  jit       %llu interpreted singly, %llu block bails\n",
                static_cast<unsigned long long>(jit_interp),
                static_cast<unsigned long long>(jit_bails));
  }
#endif
  std::printf("  disc      %llu requests, %.2f MB fetched\n",
              static_cast<unsigned long long>(stats->requests),
              stats->bytes / (1024.0 * 1024.0));

  // Printed in a fixed format on both targets so a mismatch is visible without
  // reading the trace.
  if (fifo_configured_at >= 0)
    std::printf("  fifo cfg  step %lld, pc 0x%08x, base=0x%08x end=0x%08x\n",
                static_cast<long long>(fifo_configured_at), fifo_configured_pc,
                fifo.CPBase.load(), fifo.CPEnd.load());
  else
    std::printf("  fifo cfg  not reached in %llu steps\n",
                static_cast<unsigned long long>(executed));

  if (fifo_written_at >= 0)
    std::printf("  fifo wr   step %lld, pc 0x%08x, wp=0x%08x (was 0x%08x)\n",
                static_cast<long long>(fifo_written_at), fifo_written_pc,
                fifo.CPWritePointer.load(), wp_initial);
  else
    std::printf("  fifo wr   not reached in %llu steps\n",
                static_cast<unsigned long long>(executed));

  // Both --until-fifo and --capture-fifo stop early ON PURPOSE, so the step
  // count is not the success condition for them -- staying on valid code is.
  const bool early_stop_ok = stop_at_fifo || g_fifo_limit || g_gx_limit;
  // A compiled block runs to its own end, so the step limit is a floor
  // rather than an exact count -- the last block can carry execution a few
  // instructions past it. Requiring equality was right for an interpreter
  // stepping one at a time and is simply wrong now.
  check(early_stop_ok ? state.pc != 0 : executed >= steps,
        early_stop_ok ? "ran until the stop condition without falling off"
                      : "ran at least the full step count without falling off");
  check(state.pc != 0, "pc is still valid");

  // Is anything draining the FIFO? If CPReadWriteDistance only grows, the GPU
  // side never ran and a game waiting for FIFO space would stall -- a very
  // different diagnosis from a game waiting on an interrupt.
  std::printf("  fifo state  rwdist=%u rd=%08x wr=%08x base=%08x end=%08x gpread=%u\n",
              fifo.CPReadWriteDistance.load(), fifo.CPReadPointer.load(),
              fifo.CPWritePointer.load(), fifo.CPBase.load(), fifo.CPEnd.load(),
              fifo.bFF_GPReadEnable.load());

  if (g_fifo_limit)
  {
    std::printf("  fifo cap  %zu bytes of gather-pipe stream\n", g_fifo_bytes.size());
    check(g_fifo_bytes.size() >= g_fifo_limit, "captured the requested gather-pipe stream");
    EmitBase64(g_fifo_bytes);
  }
  if (g_gx_limit)
  {
    std::printf("  gx cap    %zu bytes of executed command stream\n", g_gx_bytes.size());
    // A short capture is only a failure if the capture limit was what should
    // have stopped the run. With a step cap and a generous byte limit -- the
    // shape used to compare two targets over a FIXED instruction count -- the
    // steps run out first, and that is the intended outcome, not a failure.
    check(g_gx_bytes.size() >= g_gx_limit || executed >= steps,
          "captured the requested stream, or ran out of steps first");
    EmitBase64(g_gx_bytes);
  }

  // ── the frame ──
  //
  // The XFB is what the Video Interface scans out: a YUYV 4:2:2 buffer in
  // GUEST memory, written there by the EFB copies the game issues. Reading it
  // needs no cooperation from the video backend at all -- but it is only
  // populated if something actually rasterised, which is why this wants
  // --render.
  if (!xfb_path.empty())
  {
    auto& vi = system.GetVideoInterface();
    const u32 xfb = vi.GetXFBAddressTop();
    std::printf("  xfb addr  0x%08x\n", xfb);
    if (xfb == 0)
    {
      check(false, "VI has an XFB address");
    }
    else
    {
      // 640x480 is the standard NTSC framebuffer this title uses. The stride
      // is two bytes per pixel: YUYV packs two pixels into four bytes.
      constexpr u32 W = 640, H = 480;
      std::vector<u8> yuv(static_cast<size_t>(W) * H * 2);
      auto& mem = system.GetMemory();
      for (size_t off = 0; off < yuv.size(); off += 4)
      {
        const u32 word = mem.Read_U32(xfb + static_cast<u32>(off));
        yuv[off + 0] = static_cast<u8>(word >> 24);
        yuv[off + 1] = static_cast<u8>(word >> 16);
        yuv[off + 2] = static_cast<u8>(word >> 8);
        yuv[off + 3] = static_cast<u8>(word);
      }

      std::vector<u8> rgb(static_cast<size_t>(W) * H * 3);
      for (size_t i = 0, p = 0; i < yuv.size(); i += 4, p += 6)
      {
        const double y0 = yuv[i], u = yuv[i + 1] - 128.0, y1 = yuv[i + 2], v = yuv[i + 3] - 128.0;
        auto clamp8 = [](double x) {
          return static_cast<u8>(x < 0 ? 0 : (x > 255 ? 255 : x));
        };
        rgb[p + 0] = clamp8(y0 + 1.371 * v);
        rgb[p + 1] = clamp8(y0 - 0.698 * v - 0.336 * u);
        rgb[p + 2] = clamp8(y0 + 1.732 * u);
        rgb[p + 3] = clamp8(y1 + 1.371 * v);
        rgb[p + 4] = clamp8(y1 - 0.698 * v - 0.336 * u);
        rgb[p + 5] = clamp8(y1 + 1.732 * u);
      }

      // PPM, because it is six lines of code and every tool reads it. A
      // browser has no filesystem worth writing to, so wasm prints base64 and
      // the caller reassembles -- the same channel the GX capture uses.
#if defined(__EMSCRIPTEN__)
      std::printf("  xfb %u x %u, emitting as base64\n", W, H);
      std::vector<u8> ppm;
      {
        char hdr[64];
        const int n = std::snprintf(hdr, sizeof(hdr), "P6\n%u %u\n255\n", W, H);
        ppm.assign(hdr, hdr + n);
        ppm.insert(ppm.end(), rgb.begin(), rgb.end());
      }
      EmitBase64(ppm);
#else
      std::FILE* f = std::fopen(xfb_path.c_str(), "wb");
      if (f)
      {
        std::fprintf(f, "P6\n%u %u\n255\n", W, H);
        std::fwrite(rgb.data(), 1, rgb.size(), f);
        std::fclose(f);
        std::printf("  xfb wrote  %s (%u x %u)\n", xfb_path.c_str(), W, H);
      }
#endif
      // A framebuffer of one flat colour means nothing rasterised.
      size_t distinct = 0;
      std::set<u32> colours;
      for (size_t p = 0; p + 2 < rgb.size() && colours.size() < 64; p += 3)
        colours.insert((rgb[p] << 16) | (rgb[p + 1] << 8) | rgb[p + 2]);
      distinct = colours.size();
      std::printf("  xfb has %zu distinct colours in the first sample\n", distinct);
      check(distinct > 1, "the framebuffer is not a single flat colour");
    }
  }

  if (want_coverage && cov_total)
  {
    // Ask the real decoder, not a hand-maintained list: compile a
    // single-instruction block and see whether it took.
    ppc2wasm::StateLayout probe;
    probe.gpr_offset = static_cast<u32>(offsetof(PowerPC::PowerPCState, gpr));
    probe.memory_pages = 1;

    struct Row { u64 count; u32 key; u32 example; };
    std::vector<Row> ranked;
    for (const auto& [key, v] : cov_counts)
      ranked.push_back({v.first, key, v.second});
    std::sort(ranked.begin(), ranked.end(),
              [](const Row& a, const Row& b) { return a.count > b.count; });

    u64 supported = 0;
    for (const Row& r : ranked)
    {
      if (ppc2wasm::CompileBlock(&r.example, 0x8000'0000, 1, probe).instructions == 1)
        supported += r.count;
    }

    std::printf("\n  ── ppc2wasm coverage over %llu executed instructions ──\n",
                static_cast<unsigned long long>(cov_total));
    std::printf("  translatable today: %.1f%%\n\n",
                100.0 * static_cast<double>(supported) / static_cast<double>(cov_total));
    std::printf("  %-10s %-8s %12s  %6s  %s\n", "opcode", "xo", "count", "share", "status");
    int shown = 0;
    for (const Row& r : ranked)
    {
      if (shown++ >= 24)
        break;
      const bool ok = ppc2wasm::CompileBlock(&r.example, 0x8000'0000, 1, probe).instructions == 1;
      std::printf("  %-10u %-8u %12llu  %5.2f%%  %s\n", r.key >> 16, (r.key & 0xFFFF) >> 1,
                  static_cast<unsigned long long>(r.count),
                  100.0 * static_cast<double>(r.count) / static_cast<double>(cov_total),
                  ok ? "translated" : "NOT translated");
    }
  }

  std::printf("\n%s (%d failures)\n", g_failures ? "FAILED" : "PASSED", g_failures);

  // _exit rather than return: a booted emulator leaves threads, timing events
  // and device state live, and unwinding them from here terminates in
  // libc++abi after the result has already been printed. Exiting directly
  // keeps the status meaningful instead of turning a pass into a crash.
  std::fflush(stdout);
  _exit(g_failures ? 1 : 0);
}
