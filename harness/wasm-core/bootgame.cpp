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
#include <atomic>
#include <chrono>
#include <functional>
#include <future>
#include <thread>
#include <map>
#include <mutex>
#include <unordered_set>
#include <tuple>
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
#include "Common/GekkoDisassembler.h"
#include "Common/IOFile.h"
#include "Core/Boot/Boot.h"
#include "Core/Config/MainSettings.h"
#include "Core/ConfigLoaders/BaseConfigLoader.h"
#include "Core/ConfigManager.h"
#include "Common/Thread.h"
#include "Core/Core.h"
#include "Core/CoreTiming.h"
#include "AudioCommon/AudioCommon.h"
#include "AudioCommon/Mixer.h"
#include "AudioCommon/SoundStream.h"
#include "Common/Logging/LogManager.h"
#include "Common/WindowSystemInfo.h"
#include "VideoCommon/AsyncRequests.h"
#include "VideoCommon/Fifo.h"
#include "VideoCommon/CPMemory.h"
#include "VideoCommon/CommandProcessor.h"
#include "VideoCommon/VideoBackendBase.h"
#include "VideoCommon/VideoConfig.h"
#if defined(HARNESS_WEBGPU)
#include <emscripten/threading.h>
#include <pthread.h>
#include "VideoBackends/WebGPU/WGPUBackend.h"
#include "VideoBackends/WebGPU/WGPUContext.h"
#endif
#include "Core/Config/GraphicsSettings.h"
#include "Core/HW/GCKeyboard.h"
#include "Core/HW/GCPad.h"
#include "Core/HW/Wiimote.h"
#include "Core/HW/WiimoteEmu/Extension/Nunchuk.h"
#include "Core/HW/WiimoteEmu/ExtensionPort.h"
#include "Core/HW/WiimoteEmu/WiimoteEmu.h"
#include "InputCommon/ControllerEmu/ControlGroup/Attachments.h"
#include "InputCommon/ControllerEmu/ControllerEmu.h"
#include "InputCommon/ControllerInterface/ControllerInterface.h"
#include "InputCommon/InputConfig.h"
#include "Core/HW/EXI/EXI_Device.h"
#include "Core/HW/CPU.h"
#include "Core/FifoPlayer/FifoDataFile.h"
#include "Core/FifoPlayer/FifoPlayer.h"
#include "Core/FifoPlayer/FifoRecorder.h"
#include "Core/DSPEmulator.h"
#include "Core/HW/DSP.h"
#include "Core/HW/HW.h"
#include "Core/HW/VideoInterface.h"
#include "Core/HW/Memmap.h"
#include "Core/IOS/IOS.h"
#include "Core/IOS/USB/Emulated/Skylanders/Skylander.h"
#include "Core/IOS/USB/Emulated/Skylanders/SkylanderFigure.h"
#include "Core/PowerPC/Interpreter/Interpreter.h"
#include "Core/PowerPC/Interpreter/Interpreter_FPUtils.h"
#include "Core/PowerPC/PowerPC.h"
#include "Core/System.h"
#include "Core/WiiRoot.h"
#include "DiscIO/Filesystem.h"
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

// ── the interpreter, called from compiled code ──
//
// What a block calls for an instruction it has no translation for; see
// StateLayout::interp_hatch. This is Interpreter::SingleStepInner with the
// fetch and the floating-point-availability test taken out -- the block
// supplies the instruction word and has made that test -- and
// Interpreter::SingleStep's exception check folded in after it. Returns
// non-zero when the PC did not simply advance, so the block leaves with the
// PC the interpreter chose.
static Interpreter* g_hatch_interp = nullptr;
static PowerPC::PowerPCManager* g_hatch_ppc = nullptr;
static PowerPC::PowerPCState* g_hatch_state = nullptr;
// What goes through the hatch, keyed as the interpreter statistics key it
// (primary opcode, and the extended opcode for 4, 19, 31, 59 and 63), so
// the two rankings read side by side. Only kept with --jit-stats.
static bool g_hatch_stats = false;
static u64 g_hatch_calls = 0;
static std::map<u32, u64> g_hatch_hist;

#if !defined(__EMSCRIPTEN__)
#define EMSCRIPTEN_KEEPALIVE
#endif
extern "C" EMSCRIPTEN_KEEPALIVE u32 ppc_interp_op(u32 /*state*/, u32 ins, u32 pc)
{
  auto& st = *g_hatch_state;
  ++g_hatch_calls;
  if (g_hatch_stats)
  {
    const u32 primary = ins >> 26;
    const u32 key = (primary == 31 || primary == 19 || primary == 63 || primary == 59 || primary == 4)
                        ? (primary << 16) | (((ins >> 1) & 0x3FF) << 1)
                        : (primary << 16);
    ++g_hatch_hist[key];
  }
  st.pc = pc;
  st.npc = pc + 4;
  Interpreter::RunInterpreterOp(*g_hatch_interp, UGeckoInstruction{ins});
  // A DSI is checked before the PC advances (SingleStepInner), everything
  // else after it (SingleStep). Both end with pc = npc.
  if ((st.Exceptions & EXCEPTION_DSI) != 0)
  {
    g_hatch_ppc->CheckExceptions();
    st.pc = st.npc;
    return 1;
  }
  st.pc = st.npc;
  if (st.Exceptions != 0)
  {
    g_hatch_ppc->CheckExceptions();
    st.pc = st.npc;
  }
  return st.pc != pc + 4 ? 1u : 0u;
}

#include "slowpaths.h"

#if defined(__EMSCRIPTEN__)
// The import object every block module is instantiated with: the heap, the
// table, and the interpreter hatch, which is this module's own export.
EM_JS(void, JitImportsInit, (), {
  Module.__jitImports = {
    env: {
      memory: wasmMemory,
      __indirect_function_table: wasmTable,
      ppc_interp_op: wasmExports['ppc_interp_op'],
      ppc_hatch_exit: wasmExports['ppc_hatch_exit'],
      ppc_mem_slow: wasmExports['ppc_mem_slow'],
    },
  };
});

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
    const inst = new WebAssembly.Instance(mod, Module.__jitImports);
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
    const inst = new WebAssembly.Instance(mod, Module.__jitImports);
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
// Capturing is ARMED by the step loop, immediately or at --gx-from, so a
// capture can be a window late in the run rather than the boot.
//
// With --dual-core the opcode decoder, and so this hook, runs on the GPU
// thread while the step loop runs on the CPU thread. The flag and the
// byte count cross that boundary as atomics; the vector itself is only
// read once the GPU thread has been stopped.
static std::atomic<bool> g_gx_armed{false};
static std::atomic<size_t> g_gx_size{0};

extern "C" void harness_gx_command(const u8* data, u32 size)
{
  if (!g_gx_armed.load(std::memory_order_relaxed) || g_gx_limit == 0 ||
      g_gx_bytes.size() >= g_gx_limit)
    return;
  g_gx_bytes.insert(g_gx_bytes.end(), data, data + size);
  g_gx_size.store(g_gx_bytes.size(), std::memory_order_release);
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

// Presses are scheduled by EMULATED TIME -- CoreTiming ticks, 729 million
// per second -- not by instruction count. They used to be scheduled by
// instruction count, which was the same thing while every instruction cost
// one tick; with idle loops skipped the two come apart, and a schedule in
// instructions lands at a different emulated second on every build. Press
// for 0.2 s every 2 s by default: long enough to register, with gaps long
// enough for a menu to act on it. --period-ms and --press-ms change that.
static u64 g_press_period = 1458000000ull;  // 2.0 s
static u64 g_press_length = 145800000ull;   // 0.2 s

// The Nunchuk's stick, as a deflection the override hands back for the X
// and Y axes while a stick entry is held.
static double g_stick_x = 0.0, g_stick_y = 0.0;
// Where the Wii Remote points, in the cursor's -1..1 space with (0, 0) the
// centre of the screen. --pointer=X,Y sets it; left unset, the cursor is
// not overridden and reports whatever the unmapped controls give, which is
// the centre as well. Menus in this game are pointed at.
static bool g_pointer_set = false;
static double g_pointer_x = 0.0, g_pointer_y = 0.0;

// --live-input: the player's keyboard, mouse and gamepad, for the browser
// shell in web/ (M5). The page writes this struct straight into wasm memory
// -- the heap is a SharedArrayBuffer, so the CPU thread sees the stores --
// and the input overrides read it whenever the emulated Wiimote polls. No
// lock: each field is one aligned word, and a read that races a write sees
// a button one poll late. The layout is web/input.js's; change both.
struct LiveInput
{
  u32 buttons;             // bit i held = kLiveButtons[i]
  float stick_x, stick_y;  // Nunchuk stick, -1..1, y up
  float pointer_x, pointer_y;  // Wii Remote pointer, -1..1, y up
  u32 pointer_on;          // 0: not overridden (the cursor reads the centre)
};
static volatile LiveInput g_live{};
static bool g_live_on = false;
static const char* const kLiveButtons[] = {"A",    "B",  "1",    "2",     "+", "-", "Home",
                                           "Up",   "Down", "Left", "Right", "C", "Z", "Shake",
                                           "Thrust"};
static constexpr int kLiveShakeBit = 13;
static constexpr int kLiveThrustBit = 14;
extern "C" {
#if defined(__EMSCRIPTEN__)
EMSCRIPTEN_KEEPALIVE volatile LiveInput* harness_live_input() { return &g_live; }
#endif
// Emulated seconds since stepping began, refreshed with the stage marker,
// so the page can show the speed (emulated seconds per wall second).
volatile double g_harness_emulated_s = 0.0;
#if defined(__EMSCRIPTEN__)
EMSCRIPTEN_KEEPALIVE volatile double* harness_emulated_seconds() { return &g_harness_emulated_s; }
#endif
}

// Names from WiimoteEmu.h, Nunchuk.h and ControllerEmu.h. A D-pad direction
// and a face button live in different GROUPS, which is why both halves are
// carried. Entries whose group begins with "Nunchuk " belong to the
// extension's own override; the stick entries are deflections, not buttons.
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
      {"C", "Nunchuk Buttons", "C"}, {"Z", "Nunchuk Buttons", "Z"},
      {"SX+", "Nunchuk Stick", "X+"}, {"SX-", "Nunchuk Stick", "X-"},
      {"SY+", "Nunchuk Stick", "Y+"}, {"SY-", "Nunchuk Stick", "Y-"},
      {"none", "", ""},
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

// Whether the page holds the control the Wiimote or Nunchuk is reading.
static bool LiveHeld(std::string_view group, std::string_view control)
{
  const u32 buttons = g_live.buttons;
  for (size_t i = 0; i < std::size(kLiveButtons); ++i)
  {
    std::pair<const char*, const char*> e;
    if ((buttons >> i & 1) && LookupButton(kLiveButtons[i], &e) && group == e.first &&
        control == e.second)
      return true;
  }
  return false;
}
// The Wii Remote's motions, held while the page's bits are: Shake (all
// three axes) and Thrust (the Swing group's Forward: the remote pushed
// away from the player, as the game's "Move!" prompts ask for). Dolphin's
// motion groups take no input override: they read their controls' mapping
// expressions. So those controls are mapped to the variables $shake and
// $thrust and the bits set the variables. Checked on every poll, because
// Wiimote::LoadConfig (the boot's title change) puts the default mappings
// back after the override is installed.
static void LiveMotion(WiimoteEmu::Wiimote* wiimote)
{
  const auto map = [wiimote](ControllerEmu::Control& c, const char* expression) {
    if (c.control_ref->GetExpression() != expression)
    {
      c.control_ref->SetExpression(expression);
      wiimote->UpdateSingleControlReference(g_controller_interface, c.control_ref.get());
    }
  };
  for (auto& c : wiimote->GetWiimoteGroup(WiimoteEmu::WiimoteGroup::Shake)->controls)
    map(*c, "$shake");
  // Force's inputs: Up, Down, Left, Right, Forward, Backward.
  map(*wiimote->GetWiimoteGroup(WiimoteEmu::WiimoteGroup::Swing)->controls[4], "$thrust");
  const auto& vars = wiimote->GetExpressionVariables();
  const auto set = [&vars](const char* name, int bit) {
    if (const auto it = vars.find(name); it != vars.end() && it->second)
      *it->second = (g_live.buttons >> bit & 1) ? 1.0 : 0.0;
  };
  set("shake", kLiveShakeBit);
  set("thrust", kLiveThrustBit);
}

// --audio: the game's sound, for the page (web/audio.js). Dolphin's Null
// sound stream sets the mixer's output rate to 0, which drops every sample
// the DSP pushes; with --audio the rate goes back to 48 kHz and a thread of
// our own does what a native backend's callback would: it pulls stereo
// from the mixer into a ring in wasm memory, which the page's AudioWorklet
// reads. The page owns `read`, this side `write`; both count frames and
// only ever grow, so `write - read` is the fill.
struct AudioRing
{
  std::atomic<u32> write;
  std::atomic<u32> read;
  u32 frames;     // capacity
  u32 underruns;  // the worklet's count of quanta it found short
  s16 data[2 * 8192];
};
static AudioRing g_audio{{0}, {0}, 8192, 0, {}};
extern "C" {
#if defined(__EMSCRIPTEN__)
EMSCRIPTEN_KEEPALIVE AudioRing* harness_audio() { return &g_audio; }
#endif
}

// Keeps the ring about 43 ms full (2,048 frames) in 256-frame pulls.
// Nothing waits on this thread and it waits on nothing but its own sleep:
// a creator blocking on a new thread is the deadlock docs/PLAN.md records.
// When the game runs slow the mixer runs dry and pads with its last
// sample, so the ring stays fed by wall time and a slow stretch is heard as
// a stretch of held sound, not as the page falling behind.
static void StartAudioPump(Mixer* mixer)
{
  mixer->SetSampleRate(48000);
  std::thread([mixer] {
    constexpr u32 kTarget = 2048, kChunk = 256;
    s16 chunk[2 * kChunk];
    while (true)
    {
      u32 w = g_audio.write.load(std::memory_order_relaxed);
      while (w - g_audio.read.load(std::memory_order_acquire) < kTarget)
      {
        mixer->Mix(chunk, kChunk);
        for (u32 i = 0; i < kChunk; ++i)
        {
          const u32 at = (w + i) % g_audio.frames;
          g_audio.data[2 * at] = chunk[2 * i];
          g_audio.data[2 * at + 1] = chunk[2 * i + 1];
        }
        w += kChunk;
        g_audio.write.store(w, std::memory_order_release);
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
  }).detach();
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

// The texture cache's hashes, by call site (see the hook's comment in
// TextureCacheBase.cpp for the five sites). Counted only with --jit-stats.
//
// With --dual-core the hook runs on the GPU thread and the report on the CPU
// thread, so the counters are atomics; the window snapshot is taken while the
// GPU thread runs, which is exact to within the hashes of one draw.
//
// "work" is what GetMurmurHash3, the hash a wasm build uses, actually reads:
// it steps through len/8 eight-byte blocks (len/4)/samples at a time, plus a
// tail under eight bytes. With samples at 128 that caps a hash near 64
// blocks however large the texture; with samples at 0 (forced safe hashing)
// it reads every byte.
static constexpr int kTexHashSites = 5;
static bool g_texhash_on = false;
struct TexHashCount
{
  std::atomic<u64> calls{0}, rows{0}, bytes{0}, work{0}, full{0};
};
static TexHashCount g_texhash[kTexHashSites];

static u64 MurmurWork(u32 len, u32 samples)
{
  const u32 nblocks = len / 8;
  u32 step = len / 4;
  if (samples == 0)
    samples = std::max(step, 1u);
  step = std::max(step / samples, 1u);
  return 8ull * ((nblocks + step - 1) / step) + (len & 7);
}

// The GPU loop's counters (dolphin-web/patches/0018-fifo-loop-hooks.patch):
// runs of its payload, those that found nothing (spins), the 32-byte
// chunks run and the opcode-decoder runs they took, and the CP's enables
// seen. --fifo-batch=N is what harness_fifo_batch returns.
static std::atomic<u64> g_fifo_payloads{0}, g_fifo_spins{0}, g_fifo_chunks{0},
    g_fifo_batches{0};
static std::atomic<u32> g_fifo_enables{0};
static u32 g_fifo_batch = 1;
static bool g_skip_vertex_convert = false;
extern "C" bool harness_skip_vertex_convert()
{
  return g_skip_vertex_convert;
}
// --vertex-stats: vertices per vertex format over the run (GPU thread),
// printed at the end. The key is the format's VCD and VAT words.
static bool g_vertex_stats = false;
struct VertexFormatTally
{
  u64 vertices = 0, runs = 0;
  u32 raw_size = 0, stride = 0;
};
static std::mutex g_vertex_tally_lock;
static std::map<std::array<u32, 5>, VertexFormatTally> g_vertex_tally;
extern "C" void harness_vertex_run(u32 desc_low, u32 desc_high, u32 g0, u32 g1, u32 g2,
                                   u32 raw_size, u32 stride, int count)
{
  if (!g_vertex_stats)
    return;
  std::lock_guard<std::mutex> lk(g_vertex_tally_lock);
  VertexFormatTally& t = g_vertex_tally[{desc_low, desc_high, g0, g1, g2}];
  t.vertices += static_cast<u64>(count);
  ++t.runs;
  t.raw_size = raw_size;
  t.stride = stride;
}
// harness/wasm-core/vertexjit.cpp: the wasm vertex-loader JIT.
// --vertex-loader=software|jit|compare (0, 1, 2).
void VertexJitSetMode(int mode);
void VertexJitReport();

static void PrintVertexTally()
{
  std::lock_guard<std::mutex> lk(g_vertex_tally_lock);
  std::vector<std::pair<std::array<u32, 5>, VertexFormatTally>> rows(g_vertex_tally.begin(),
                                                                     g_vertex_tally.end());
  std::sort(rows.begin(), rows.end(),
            [](const auto& a, const auto& b) { return a.second.vertices > b.second.vertices; });
  u64 total = 0;
  for (const auto& r : rows)
    total += r.second.vertices;
  std::printf("  vertices  %llu over the run in %zu formats; the top ones (index mode per "
              "attribute: - none, D direct, 8/6 index 8/16; VAT formats as Dolphin's enums)\n",
              static_cast<unsigned long long>(total), rows.size());
  auto mode = [](u32 v) { return "-D86"[v & 3]; };
  int shown = 0;
  for (const auto& [key, t] : rows)
  {
    if (shown++ == 12)
      break;
    TVtxDesc desc;
    desc.low.Hex = key[0];
    desc.high.Hex = key[1];
    VAT vat;
    vat.g0.Hex = key[2];
    vat.g1.Hex = key[3];
    vat.g2.Hex = key[4];
    std::printf("  vertices  %5.1f%%  %10llu in %7llu runs  raw %2u -> %2u bytes  pos %c "
                "fmt %u x%u  nrm %c fmt %u  col %c%c comp %u/%u  tex %c%c%c%c fmt %u x%u  "
                "vcd %08x %08x vat %08x %08x %08x\n",
                100.0 * t.vertices / total, static_cast<unsigned long long>(t.vertices),
                static_cast<unsigned long long>(t.runs), t.raw_size, t.stride,
                mode(static_cast<u32>(desc.low.Position.Value())),
                static_cast<u32>(vat.g0.PosFormat.Value()),
                static_cast<u32>(vat.g0.PosElements.Value()) + 2,
                mode(static_cast<u32>(desc.low.Normal.Value())),
                static_cast<u32>(vat.g0.NormalFormat.Value()),
                mode(static_cast<u32>(desc.low.Color0.Value())),
                mode(static_cast<u32>(desc.low.Color1.Value())),
                static_cast<u32>(vat.g0.Color0Comp.Value()),
                static_cast<u32>(vat.g0.Color1Comp.Value()),
                mode(static_cast<u32>(desc.high.Tex0Coord.Value())),
                mode(static_cast<u32>(desc.high.Tex1Coord.Value())),
                mode(static_cast<u32>(desc.high.Tex2Coord.Value())),
                mode(static_cast<u32>(desc.high.Tex3Coord.Value())),
                static_cast<u32>(vat.g0.Tex0CoordFormat.Value()),
                static_cast<u32>(vat.g0.Tex0CoordElements.Value()) + 1, key[0], key[1], key[2],
                key[3], key[4]);
  }
}
extern "C" void harness_fifo_loop(u32 chunks, u32 batches, u32 enables)
{
  g_fifo_payloads.fetch_add(1, std::memory_order_relaxed);
  if (chunks == 0)
    g_fifo_spins.fetch_add(1, std::memory_order_relaxed);
  g_fifo_chunks.fetch_add(chunks, std::memory_order_relaxed);
  g_fifo_batches.fetch_add(batches, std::memory_order_relaxed);
  if (enables)
    g_fifo_enables.fetch_or(enables, std::memory_order_relaxed);
}
extern "C" u32 harness_fifo_batch()
{
  return g_fifo_batch;
}
static u32 g_fifo_spin_limit = 0;
extern "C" u32 harness_fifo_spin_limit()
{
  return g_fifo_spin_limit;
}

// The CPU thread's idle syncs (CoreTiming::Idle -> FlushGpu): how many,
// and how long each waited for the GPU thread, in buckets of microseconds.
// Read at the window's edges by main.
// --idle-stats turns the timing on (two performance.now() calls per sync,
// 38,000 a second in gameplay: about half a percent).
static bool g_idle_stats = false;
static u64 g_idle_n = 0;
static double g_idle_ms = 0, g_idle_longest_ms = 0;
static u64 g_idle_bucket[5] = {};  // <= 5 us (no wait), <= 50, <= 500, <= 2000, more
// For the waits over 2 ms: the FIFO's backlog (CPReadWriteDistance) as
// the CPU thread entered them.
static u64 g_idle_long_n = 0, g_idle_long_backlog = 0, g_idle_long_backlog_max = 0;
static void NoteIdleWait(double ms, u32 backlog)
{
  ++g_idle_n;
  g_idle_ms += ms;
  g_idle_longest_ms = std::max(g_idle_longest_ms, ms);
  const double us = ms * 1000.0;
  ++g_idle_bucket[us <= 5 ? 0 : us <= 50 ? 1 : us <= 500 ? 2 : us <= 2000 ? 3 : 4];
  if (us > 2000)
  {
    ++g_idle_long_n;
    g_idle_long_backlog += backlog;
    g_idle_long_backlog_max = std::max<u64>(g_idle_long_backlog_max, backlog);
  }
}

// --frame-times: every frame's wall time against its emulated time, and
// what a slow one waited on. The gameplay window's average said 101%; a
// player in a visible Chrome saw the readout at ~80%, dips to ~28% and a
// few seconds at 0% while Eon spoke. This is the measurement that sees
// those. A frame is a change of the scanned-out XFB, as for `frames`.
struct FrameTime
{
  double t_emu;       // emulated s at the frame's end
  double wall_ms;     // wall time the frame took
  double emu_ms;      // emulated time it covered
  double compile_ms;  // GPU thread making shaders and pipelines meanwhile
  double disc_ms;     // in synchronous disc reads meanwhile
  double idle_ms;     // the CPU thread waiting for the GPU thread meanwhile
};
static std::vector<FrameTime> g_frame_times;

static u64 CompileUs()
{
#if defined(HARNESS_WEBGPU)
  return WebGPU::g_compile_us.load(std::memory_order_relaxed);
#else
  return 0;
#endif
}

// Speed per second of wall time -- what the page's readout shows -- and the
// slow frames, over the frames from `from_s` emulated seconds on.
static void ReportFrameTimes(const char* label, double from_s)
{
  std::vector<double> per_second, walls;
  double wall = 0, emu = 0;
  double hitch_ms = 0, hitch_compile = 0, hitch_disc = 0, hitch_idle = 0;
  u64 hitches = 0;
  std::vector<const FrameTime*> worst;
  for (const FrameTime& f : g_frame_times)
  {
    if (f.t_emu < from_s)
      continue;
    walls.push_back(f.wall_ms);
    wall += f.wall_ms;
    emu += f.emu_ms;
    if (wall >= 1000.0)
    {
      per_second.push_back(100.0 * emu / wall);
      wall = emu = 0;
    }
    if (f.wall_ms > 100.0)
    {
      ++hitches;
      hitch_ms += f.wall_ms;
      hitch_compile += f.compile_ms;
      hitch_disc += f.disc_ms;
      hitch_idle += f.idle_ms;
      worst.push_back(&f);
    }
  }
  if (per_second.empty())
    return;
  std::sort(per_second.begin(), per_second.end());
  std::sort(walls.begin(), walls.end());
  const auto pct = [](const std::vector<double>& v, double p) {
    return v[std::min(v.size() - 1, static_cast<size_t>(p * v.size()))];
  };
  size_t below90 = 0;
  for (double v : per_second)
    below90 += v < 90.0;
  std::printf("  seconds   %s: %zu wall seconds; speed in each: median %.0f%%, p25 %.0f%%, "
              "p10 %.0f%%, p1 %.0f%%, worst %.0f%%; %zu below 90%%\n",
              label, per_second.size(), pct(per_second, 0.5), pct(per_second, 0.25),
              pct(per_second, 0.10), pct(per_second, 0.01), per_second.front(), below90);
  std::printf("  frametime %s: %zu frames; wall ms median %.1f, p90 %.1f, p99 %.1f, max %.0f\n",
              label, walls.size(), pct(walls, 0.5), pct(walls, 0.9), pct(walls, 0.99),
              walls.back());
  std::printf("  hitches   %s: %llu frames over 100 ms, %.0f ms in all; during them the GPU "
              "thread compiled for %.0f ms, disc reads took %.0f ms, the CPU thread waited on "
              "the GPU thread %.0f ms\n",
              label, static_cast<unsigned long long>(hitches), hitch_ms, hitch_compile,
              hitch_disc, hitch_idle);
  std::sort(worst.begin(), worst.end(),
            [](const FrameTime* a, const FrameTime* b) { return a->wall_ms > b->wall_ms; });
  for (size_t i = 0; i < std::min<size_t>(worst.size(), 25); ++i)
    std::printf("  hitch     t=%.2f s: %.0f ms for %.0f ms emulated; compile %.0f, disc %.0f, "
                "idle-wait %.0f ms\n",
                worst[i]->t_emu, worst[i]->wall_ms, worst[i]->emu_ms, worst[i]->compile_ms,
                worst[i]->disc_ms, worst[i]->idle_ms);
}

extern "C" void harness_texture_hash(int site, u32 len, u32 samples, u32 rows)
{
  if (!g_texhash_on || site < 0 || site >= kTexHashSites)
    return;
  TexHashCount& c = g_texhash[site];
  c.calls.fetch_add(1, std::memory_order_relaxed);
  if (rows == 0)
    return;
  c.rows.fetch_add(rows, std::memory_order_relaxed);
  c.bytes.fetch_add(static_cast<u64>(len) * rows, std::memory_order_relaxed);
  c.work.fetch_add(MurmurWork(len, samples) * rows, std::memory_order_relaxed);
  if (samples == 0)
    c.full.fetch_add(1, std::memory_order_relaxed);
}

struct TexHashSnapshot
{
  u64 calls[kTexHashSites], rows[kTexHashSites], bytes[kTexHashSites], work[kTexHashSites],
      full[kTexHashSites];
};

static TexHashSnapshot TexHashNow()
{
  TexHashSnapshot s;
  for (int i = 0; i < kTexHashSites; ++i)
  {
    s.calls[i] = g_texhash[i].calls.load(std::memory_order_relaxed);
    s.rows[i] = g_texhash[i].rows.load(std::memory_order_relaxed);
    s.bytes[i] = g_texhash[i].bytes.load(std::memory_order_relaxed);
    s.work[i] = g_texhash[i].work.load(std::memory_order_relaxed);
    s.full[i] = g_texhash[i].full.load(std::memory_order_relaxed);
  }
  return s;
}

// One line per site, per frame and per emulated second, over a span that
// started at snapshot `from`.
static void PrintTexHash(const char* label, const TexHashSnapshot& from, u64 frames,
                         double emu_s)
{
  static const char* names[kTexHashSites] = {"load: TMEM reuse, no hash",
                                             "load: bound re-check",
                                             "GetTexture: data", "GetTexture: palette",
                                             "CalculateHash"};
  const TexHashSnapshot now = TexHashNow();
  const double f = frames ? static_cast<double>(frames) : 1.0;
  const double e = emu_s > 0 ? emu_s : 1.0;
  std::printf("  texhash   %s: %llu frames, %.3f emulated s\n", label,
              static_cast<unsigned long long>(frames), emu_s);
  u64 hashes = 0, work = 0;
  for (int i = 0; i < kTexHashSites; ++i)
  {
    const u64 calls = now.calls[i] - from.calls[i];
    const u64 rows = now.rows[i] - from.rows[i];
    const u64 bytes = now.bytes[i] - from.bytes[i];
    const u64 w = now.work[i] - from.work[i];
    const u64 full = now.full[i] - from.full[i];
    if (i == 2 || i == 3 || i == 4)
    {
      hashes += rows;
      work += w;
    }
    std::printf("  texhash   %-26s %8.1f /frame %10.0f /s  rows %8.1f /frame  "
                "len %8.0f B/frame  read %8.0f B/frame  unsampled %llu\n",
                names[i], calls / f, calls / e, rows / f, bytes / f, w / f,
                static_cast<unsigned long long>(full));
  }
  std::printf("  texhash   GetHash64 calls %.1f /frame, %.0f /s; %.0f B read /frame, %.1f MB/s\n",
              hashes / f, hashes / e, work / f, work / e / 1e6);
}

// Every distinct shader VideoCommon created (--dump-shaders), as
// (stage, source), from the Null backend's hook. Under --dual-core the hook
// runs on the GPU thread; the list is read after that thread has stopped.
static bool g_dump_shaders = false;
static std::mutex g_shader_mutex;
static std::unordered_set<std::string> g_shader_seen;
static std::vector<std::pair<int, std::string>> g_shaders;

extern "C" void harness_shader_source(int stage, const char* source, unsigned long size)
{
  if (!g_dump_shaders)
    return;
  std::string text(source, size);
  std::lock_guard<std::mutex> lock(g_shader_mutex);
  if (g_shader_seen.insert(std::to_string(stage) + text).second)
    g_shaders.emplace_back(stage, std::move(text));
}

// The GPU thread: a std::thread, or -- for the WebGPU backend -- a pthread
// created with the page's canvas transferred to it, because a WebGPU
// surface belongs to the worker that owns the canvas and std::thread has
// no way to say which canvases a new thread should take.
struct HarnessThread
{
  std::thread thread;
#if defined(HARNESS_WEBGPU)
  pthread_t pthread{};
  bool is_pthread = false;
#endif
  bool joinable() const
  {
#if defined(HARNESS_WEBGPU)
    if (is_pthread)
      return true;
#endif
    return thread.joinable();
  }
  void join()
  {
#if defined(HARNESS_WEBGPU)
    if (is_pthread)
    {
      pthread_join(pthread, nullptr);
      is_pthread = false;
      return;
    }
#endif
    thread.join();
  }
};

static HarnessThread SpawnGpuThread(std::function<void()> body, bool with_canvas)
{
  HarnessThread t;
#if defined(HARNESS_WEBGPU)
  if (with_canvas)
  {
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    emscripten_pthread_attr_settransferredcanvases(&attr, "#canvas");
    auto* heap_body = new std::function<void()>(std::move(body));
    pthread_create(
        &t.pthread, &attr,
        [](void* arg) -> void* {
          std::unique_ptr<std::function<void()>> fn(static_cast<std::function<void()>*>(arg));
          (*fn)();
          return nullptr;
        },
        heap_body);
    pthread_attr_destroy(&attr);
    t.is_pthread = true;
    return t;
  }
#endif
  (void)with_canvas;
  t.thread = std::thread(std::move(body));
  return t;
}

// --record-fifo: set from the recorder's callback, which may run on the GPU
// thread.
static bool g_fifo_recording_started = false;
static std::atomic<bool> g_fifo_recording_done{false};
static std::atomic<bool> g_fifo_recorded_ok{false};

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
  u64 steps = argc > 2 ? std::strtoull(argv[2], nullptr, 0) : 2000;
  bool want_trace = false;
  // --until-fifo: treat the step count as a cap and stop at the first FIFO
  // write instead of running it out. Reaching the FIFO takes far more
  // instructions than a fixed differential run, and every step past the
  // milestone is browser time spent learning nothing.
  bool stop_at_fifo = false;
  bool want_events = false;
  bool want_input = false;
  std::string input_seq = "A";
  float emulation_speed = 0.0f;  // --speed=F; 0 = unlimited
  bool want_frame_times = false;
  u64 walk_from = 0;
  bool want_dump_uidcache = false;
  std::string uidcache_url;
  bool want_skylander = false;
  bool want_audio = false;
  u16 sky_id = 0, sky_var = 0;
  u64 checkpoint = 0;
  u64 trace_from = 0;
  u64 dump_at = 0;
  u32 watch_addr = 0;
  bool want_coverage = false;
  bool want_jit = false;
  bool want_link = true;
  bool want_jit_stats = false;
  bool want_idle_skip = true;
  u32 jit_threshold = 16;
  bool want_jit_names = false;
  bool want_probe_pad = false;
  bool want_compact = true;
  bool want_fast_fp = true;
  bool want_short_convert = true;
  bool want_fprf = false;  // or Config::MAIN_FPRF; see below
  std::vector<u32> jit_dump_pcs;
  bool want_hatch = true;
  bool want_phys = true;
  bool want_gp = true;
  bool want_psq = true;
  bool want_madd = true;
  bool want_crdiff = true;
  bool want_l1 = true;
  bool want_dual_core = false;
  int tex_samples = -1;  // --tex-samples=N: the texture cache's hash samples, -1 = untouched
  bool want_dolphin_video_config = true;
  bool want_dump_shaders = false;
  bool want_webgpu = false;
  bool want_webgpu_per_draw = false;
  bool want_webgpu_no_upload = false;
  bool want_webgpu_no_draws = false;
  bool want_webgpu_no_present = false;
  bool want_webgpu_uniform_dedup = true;
  bool want_osd = true;
  std::string fifo_path;
  u64 record_fifo_at = 0;  // --record-fifo=TICKS
  bool want_xfb_to_ram = false;
  bool want_true_color = true;
  bool want_sync_idle = true;
  int sync_gpu_ticks = 0;
  bool want_nunchuk = true;
  bool want_disc_files = false;
  u64 gx_from = 0;      // --gx-from=TICKS: arm the GX capture at this emulated time
  u64 xfb_every = 0;    // --xfb-every=TICKS: a quarter-size framebuffer thumbnail this often
  u64 speed_from = 0;   // --speed-from=TICKS: a second stopwatch from this emulated time
  std::vector<std::pair<u32, u32>> disasm_ranges;  // --disasm=ADDR,N
  bool want_mtmsr_continue = true;
  u64 tick_cap = 0;
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
    // --frame-times: per-frame wall time, the speed in each wall second,
    // and what the slow frames waited on (see FrameTime). Turns on
    // --idle-stats, which it reads.
    else if (std::strcmp(argv[i], "--frame-times") == 0)
      want_frame_times = g_idle_stats = true;
    // --walk-from=TICKS: with --input, from this emulated time on also hold
    // the Nunchuk stick, a quarter turn every 2 s (up, right, down, left):
    // walking while the sequence keeps pressing, closer to play than
    // tapping A in place.
    else if (std::strncmp(argv[i], "--walk-from=", 12) == 0)
      walk_from = std::strtoull(argv[i] + 12, nullptr, 0);
    // --dump-uidcache: at the end, print the shader cache's pipeline UID
    // file (Dolphin writes one per game, Cache/<game id>.uidcache, a record
    // of every pipeline the run made) as base64, for web/make-uidcache.sh.
    else if (std::strcmp(argv[i], "--dump-uidcache") == 0)
      want_dump_uidcache = true;
    // --uidcache-url=URL: fetch a UID file before the video backend starts
    // and put it in Cache/ under the URL's file name, which must be
    // <game id>.uidcache. Dolphin's shader cache then compiles every
    // pipeline in it at startup (on the GPU thread, before the game runs:
    // the backend has no background compiling), instead of the first time
    // the game draws with each -- the hitches --frame-times found.
    else if (std::strncmp(argv[i], "--uidcache-url=", 15) == 0)
      uidcache_url = argv[i] + 15;
    // --live-input: take buttons, stick and pointer from the page (web/)
    // instead of a script. Replaces --input.
    else if (std::strcmp(argv[i], "--live-input") == 0)
      g_live_on = true;
    // --audio: play the game's sound through the page (see AudioRing).
    else if (std::strcmp(argv[i], "--audio") == 0)
      want_audio = true;
    // --speed=F: limit emulation to F times real time, as Dolphin does when
    // played (1 = real time). Measuring runs leave it unlimited; see 1e.
    else if (std::strncmp(argv[i], "--speed=", 8) == 0)
      emulation_speed = std::strtof(argv[i] + 8, nullptr);
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
    // --jit-no-idle: spin through busy-wait loops instead of handing the
    // rest of the slice to CoreTiming::Idle. For measuring what skipping
    // them is worth, and for a differential that wants every instruction.
    else if (std::strcmp(argv[i], "--jit-no-idle") == 0)
      want_idle_skip = false;
    // --jit-no-mtmsr: make every mtmsr leave the chain, as it used to.
    else if (std::strcmp(argv[i], "--jit-no-mtmsr") == 0)
      want_mtmsr_continue = false;
    // --ticks=N: stop after N emulated ticks rather than N instructions.
    // With idle skipping, instructions and emulated time come apart, and a
    // fixed amount of emulated time is the comparison that means something.
    else if (std::strncmp(argv[i], "--ticks=", 8) == 0)
      tick_cap = std::strtoull(argv[i] + 8, nullptr, 0);
    // --jit-threshold=N: compile a block on its Nth visit, interpreting it
    // until then. 1 compiles everything on sight, as the dispatcher used to.
    // --jit-names: give every compiled block a wasm name of the form
    // b_<guest pc>, so a CPU profile attributes time to guest code rather
    // than to wasm-function[k] in a module identified by hash.
    else if (std::strcmp(argv[i], "--jit-names") == 0)
      want_jit_names = true;
    // --jit-dump=PC[,PC...]: print the module holding the block at each PC
    // (JITMOD <pc> <function> <bytes>, then base64 "F " lines), so the
    // emitted code can be read and run through V8 outside the game
    // (harness/wasm-core/jit-asm.mjs prints the machine code).
    else if (std::strncmp(argv[i], "--jit-dump=", 11) == 0)
      for (const char* q = argv[i] + 11; *q;)
      {
        char* e;
        jit_dump_pcs.push_back(static_cast<u32>(std::strtoul(q, &e, 16)));
        q = *e == ',' ? e + 1 : e;
      }
    // --jit-no-hatch: an untranslated instruction ends its block and goes
    // through the dispatcher to the interpreter, as it used to.
    else if (std::strcmp(argv[i], "--jit-no-hatch") == 0)
      want_hatch = false;
    // --jit-no-phys: interpret code that runs with translation off, as
    // the dispatcher used to, instead of compiling it in its own tier.
    else if (std::strcmp(argv[i], "--jit-no-phys") == 0)
      want_phys = false;
    // --jit-no-gp: every store to the write-gather pipe goes through the
    // hatch, instead of the ones that do not fill the buffer being
    // written to it inline.
    else if (std::strcmp(argv[i], "--jit-no-gp") == 0)
      want_gp = false;
    // --jit-no-psq: the quantized paired-single loads and stores go
    // through the hatch, instead of being specialised on the GQRs.
    else if (std::strcmp(argv[i], "--jit-no-psq") == 0)
      want_psq = false;
    // --jit-no-madd: the paired fused multiply-adds go through the hatch,
    // as they did before EmitPsMadd.
    else if (std::strcmp(argv[i], "--jit-no-madd") == 0)
      want_madd = false;
    // --jit-no-crdiff: compares index s_crTable, as they did before they
    // stored the operands' difference.
    else if (std::strcmp(argv[i], "--jit-no-crdiff") == 0)
      want_crdiff = false;
    // --jit-no-l1: accesses to the locked L1 cache bail to the interpreter
    // (and retire the block when first), as they used to.
    else if (std::strcmp(argv[i], "--jit-no-l1") == 0)
      want_l1 = false;
    // --jit-fprf: the inline floating-point forms write FPSCR[FPRF], as the
    // interpreter does. Off by default because Config::MAIN_FPRF is, which
    // is what Dolphin's own JITs follow (StateLayout::fprf).
    else if (std::strcmp(argv[i], "--jit-fprf") == 0)
      want_fprf = true;
    // --jit-exact-fp: floating-point arithmetic exact to the interpreter,
    // FPSCR exception bits and all, instead of at Dolphin's JIT accuracy
    // (StateLayout::fast_fp).
    else if (std::strcmp(argv[i], "--jit-exact-fp") == 0)
      want_fast_fp = false;
    // --jit-no-compact: the slow paths inline in each block, as they were
    // before StateLayout::hatch_exit_index.
    else if (std::strcmp(argv[i], "--jit-no-compact") == 0)
      want_compact = false;
    // --jit-no-short-convert: float loads and stores convert bit by bit, as
    // before StateLayout::short_convert. Both ways are exact.
    else if (std::strcmp(argv[i], "--jit-no-short-convert") == 0)
      want_short_convert = false;
    // --jit-probe-pad: every RAM access carries a dead copy of its miss
    // branch (StateLayout::probe_cold_pad), to price inline cold code.
    else if (std::strcmp(argv[i], "--jit-probe-pad") == 0)
      want_probe_pad = true;
    // --dual-core: Dolphin's dual-core mode. The GPU -- VideoCommon's
    // command processing, vertex loading and texture cache, which is 42%
    // of the emulation thread's time in gameplay with the Null backend --
    // runs on its own thread, as it does in Dolphin itself. The run is
    // then not deterministic, so this is for speed measurements, not for
    // differentials against the native oracle.
    else if (std::strcmp(argv[i], "--dual-core") == 0)
      want_dual_core = true;
    // --tex-samples=N: how many points of each texture the texture cache
    // hashes; 0 hashes every byte. Dolphin's default, and so the harness's,
    // is 128.
    else if (std::strncmp(argv[i], "--tex-samples=", 14) == 0)
      tex_samples = std::atoi(argv[i] + 14);
    // --no-dolphin-video-config: the harness as it was before video was set
    // up the way Core.cpp does it -- ActivateBackend alone, g_Config and
    // g_backend_info left at their C++ member defaults, which hash every
    // byte of every texture. For A/B runs only: 70% of real time in the
    // gameplay window against 88%, with the same frames and draws.
    else if (std::strcmp(argv[i], "--no-dolphin-video-config") == 0)
      want_dolphin_video_config = false;
    // --dump-shaders: report a WebGPU backend's capabilities instead of the
    // Null backend's, so VideoCommon generates the shaders that backend
    // would get, and print every distinct one at the end as "SHADER <stage>
    // <base64>". For docs/PHASE3-WEBGPU.md spike S2; the run still draws
    // nothing.
    else if (std::strcmp(argv[i], "--dump-shaders") == 0)
      want_dump_shaders = true;
    // --webgpu: draw with the WebGPU backend (dolphin-web/VideoBackends/
    // WebGPU; docs/PHASE3-WEBGPU.md) into the page's canvas, instead of the
    // Null backend. Needs a WEBGPU=1 build.
    else if (std::strcmp(argv[i], "--webgpu") == 0)
      want_webgpu = true;
    // --webgpu-per-draw: the WebGPU backend's M1 path, a bind group made
    // and every buffer write sent per draw, to measure M4's against
    // (docs/PHASE3-WEBGPU.md M4).
    else if (std::strcmp(argv[i], "--webgpu-per-draw") == 0)
      want_webgpu_per_draw = true;
    // --webgpu-no-upload: the backend writes nothing to its buffers (the
    // draws read stale data). What the uploads cost -- on the GPU thread,
    // and on the CPU thread through the caches (docs/PHASE3-WEBGPU.md M4).
    else if (std::strcmp(argv[i], "--webgpu-no-upload") == 0)
      want_webgpu_no_upload = true;
    // --webgpu-no-uniform-dedup: upload every dirty uniform block, changed
    // or not, as before (Gfx::SetGXUniforms), for A/B.
    else if (std::strcmp(argv[i], "--webgpu-no-uniform-dedup") == 0)
      want_webgpu_uniform_dedup = false;
    // --webgpu-no-draws: the backend records no draws (frames stay blank).
    // What the draws cost, and what Chrome's GPU-side work costs the CPU
    // thread.
    else if (std::strcmp(argv[i], "--webgpu-no-draws") == 0)
      want_webgpu_no_draws = true;
    // --webgpu-no-present: frames are drawn and submitted but never reach
    // the canvas (no copy, no yield to the browser). Bisects the CPU
    // thread's slowdown under this backend (docs/PHASE3-WEBGPU.md M4).
    else if (std::strcmp(argv[i], "--webgpu-no-present") == 0)
      want_webgpu_no_present = true;
    // --fifo=FILE: replay a FIFO capture (.dff) with Dolphin's FifoPlayer
    // instead of running the game: the same GX commands and memory, frame
    // for frame, whatever the backend -- what docs/PHASE3-WEBGPU.md M3
    // compares backends on. In the browser FILE is fetched from the page's
    // server. The disc still opens, for the console type and the memory
    // map; nothing from it runs.
    else if (std::strncmp(argv[i], "--fifo=", 7) == 0)
      fifo_path = argv[i] + 7;
    // --record-fifo=TICKS: at this emulated time, record three frames with
    // Dolphin's FifoRecorder -- what nogui's --fifo-record does, at a point
    // this harness reaches deterministically -- and emit the .dff as base64
    // on "D " lines, then stop.
    else if (std::strncmp(argv[i], "--record-fifo=", 14) == 0)
      record_fifo_at = std::strtoull(argv[i] + 14, nullptr, 0);
    // --xfb-to-ram: XFB copies are written to guest memory even by a
    // backend that keeps them in VRAM, so --dump-xfb sees what it drew.
    else if (std::strcmp(argv[i], "--xfb-to-ram") == 0)
      want_xfb_to_ram = true;
    // --no-true-color: Dolphin's "Force 24-bit color" off, so a hardware
    // backend truncates to the EFB's 6-bit format and dithers, as the GX --
    // and the Software renderer -- do. On by default in Dolphin.
    else if (std::strcmp(argv[i], "--no-true-color") == 0)
      want_true_color = false;
    // --no-osd: no on-screen messages over the presented frame.
    else if (std::strcmp(argv[i], "--no-osd") == 0)
      want_osd = false;
    // --no-sync-idle: with --dual-core, the CPU does not wait for the GPU
    // thread to drain the FIFO each time an idle loop is skipped
    // (Dolphin's "Sync on skip idle", on by default). The game still
    // synchronises where it asks to -- draw-done, tokens -- and the FIFO
    // bounds how far ahead the CPU can get.
    else if (std::strcmp(argv[i], "--no-sync-idle") == 0)
      want_sync_idle = false;
    // --fifo-batch=N: the GPU thread runs up to N 32-byte FIFO chunks
    // through the opcode decoder per CP status update, instead of
    // Dolphin's one (dolphin-web/patches/0018-fifo-loop-hooks.patch; for
    // docs/PHASE3-WEBGPU.md M4).
    else if (std::strncmp(argv[i], "--fifo-batch=", 13) == 0)
      g_fifo_batch = static_cast<u32>(std::strtoul(argv[i] + 13, nullptr, 0));
    // --gpu-spin=N: the GPU loop sleeps after N runs that found nothing
    // (patch 0018), instead of spinning until SystemTimers' 1 kHz tick.
    // The spin holds a core; this measures what that costs the CPU thread.
    else if (std::strncmp(argv[i], "--gpu-spin=", 11) == 0)
      g_fifo_spin_limit = static_cast<u32>(std::strtoul(argv[i] + 11, nullptr, 0));
    // --idle-stats: time every idle sync with the GPU thread and report
    // them for the window (the "idle" line).
    else if (std::strcmp(argv[i], "--idle-stats") == 0)
      g_idle_stats = true;
    // --skip-vertex-convert: the vertex loader converts nothing (patch
    // 0019); draws go out with stale vertex data. For measuring what the
    // loader costs, with a backend that draws nothing.
    else if (std::strcmp(argv[i], "--skip-vertex-convert") == 0)
      g_skip_vertex_convert = true;
    // --vertex-stats: vertices per vertex format over the run, at the end.
    else if (std::strcmp(argv[i], "--vertex-stats") == 0)
      g_vertex_stats = true;
    // --vertex-loader=software|jit|compare: Dolphin's generic vertex loader,
    // the wasm vertex-loader JIT (vertexjit.cpp), or both on every call with
    // the differences counted (the vtxjit line at the end).
    else if (std::strncmp(argv[i], "--vertex-loader=", 16) == 0)
    {
      const char* v = argv[i] + 16;
      VertexJitSetMode(std::strcmp(v, "jit") == 0 ? 1 : std::strcmp(v, "compare") == 0 ? 2 : 0);
    }
    // --sync-gpu=TICKS: Dolphin's "SyncGPU" (MAIN_SYNC_GPU): the CPU thread
    // may run at most TICKS of emulated time ahead of the GPU thread's
    // progress, as the opcode decoder estimates it, and blocks there --
    // instead of, or as well as, stopping at every skipped idle loop. For
    // docs/PHASE3-WEBGPU.md M4; with --no-sync-idle.
    else if (std::strncmp(argv[i], "--sync-gpu=", 11) == 0)
      sync_gpu_ticks = static_cast<int>(std::strtol(argv[i] + 11, nullptr, 0));
    // --no-nunchuk: leave the Wiimote without an extension. The game asks
    // for a Nunchuk before it will go anywhere, so one is attached by
    // default; this is for comparing against baselines recorded without.
    else if (std::strcmp(argv[i], "--no-nunchuk") == 0)
      want_nunchuk = false;
    // --disc-files: print the game partition's file table at startup.
    else if (std::strcmp(argv[i], "--disc-files") == 0)
      want_disc_files = true;
    else if (std::strncmp(argv[i], "--period-ms=", 12) == 0)
      g_press_period = std::strtoull(argv[i] + 12, nullptr, 0) * 729000ull;
    else if (std::strncmp(argv[i], "--press-ms=", 11) == 0)
      g_press_length = std::strtoull(argv[i] + 11, nullptr, 0) * 729000ull;
    // --gx-from=TICKS: with --capture-gx=N, start capturing only once this
    // much emulated time has passed, so the N bytes are a window late in
    // the run -- the first seconds of gameplay rather than the boot.
    else if (std::strncmp(argv[i], "--gx-from=", 10) == 0)
      gx_from = std::strtoull(argv[i] + 10, nullptr, 0);
    // --speed-from=TICKS: report the speed of everything after this
    // emulated time as well as of the whole run. The number that matters
    // for play is the one measured once the game is being played.
    else if (std::strncmp(argv[i], "--speed-from=", 13) == 0)
      speed_from = std::strtoull(argv[i] + 13, nullptr, 0);
    // --xfb-every=TICKS: with --render, emit a 160x120 thumbnail of the
    // framebuffer every N emulated ticks, as base64 PPM. A full frame is
    // 900 KB; a thumbnail is 57 KB, which is what makes a dozen of them
    // transportable out of a browser. For seeing which screen the game is
    // on at each point of a long run.
    else if (std::strncmp(argv[i], "--xfb-every=", 12) == 0)
      xfb_every = std::strtoull(argv[i] + 12, nullptr, 0);
    // --pointer=X,Y: hold the Wii Remote's pointer at this screen position,
    // -1..1 on each axis, (0,0) the centre.
    else if (std::strncmp(argv[i], "--pointer=", 10) == 0)
    {
      g_pointer_set = std::sscanf(argv[i] + 10, "%lf,%lf", &g_pointer_x, &g_pointer_y) == 2;
    }
    // --disasm=ADDR,N: at the end of the run, disassemble N guest
    // instructions at ADDR. For reading what a hot block in a profile is.
    else if (std::strncmp(argv[i], "--disasm=", 9) == 0)
    {
      unsigned addr = 0, count = 16;
      std::sscanf(argv[i] + 9, "%x,%u", &addr, &count);
      disasm_ranges.push_back({addr, count});
    }
    else if (std::strncmp(argv[i], "--jit-threshold=", 16) == 0)
      jit_threshold = static_cast<u32>(std::strtoul(argv[i] + 16, nullptr, 0));
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
  // The WebGPU backend reports why it refuses a shader, a pipeline or a
  // texture through VIDEO; with Null there is nothing to report.
  if (want_webgpu)
    logm->SetEnable(Common::Log::LogType::VIDEO, true);
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
  // System::Initialize reads MAIN_CPU_THREAD here, once; the later setting
  // in the configuration block below is what the rest of Dolphin reads.
  Config::SetCurrent(Config::MAIN_CPU_THREAD, want_dual_core);
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
              if (group == "IR")
              {
                if (!g_pointer_set)
                  return std::nullopt;
                if (control == "X")
                  return g_pointer_x;
                if (control == "Y")
                  return g_pointer_y;
                return std::nullopt;
              }
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
  else if (g_live_on)
  {
    if (InputConfig* cfg = Wiimote::GetConfig())
    {
      if (auto* controller = cfg->GetController(0))
      {
        auto* wiimote = static_cast<WiimoteEmu::Wiimote*>(controller);
        controller->SetInputOverrideFunction(
            [wiimote](std::string_view group, std::string_view control,
                      ControlState state) -> std::optional<ControlState> {
              if (group == "IR")
              {
                if (!g_live.pointer_on)
                  return std::nullopt;
                if (control == "X")
                  return g_live.pointer_x;
                if (control == "Y")
                  return g_live.pointer_y;
                return std::nullopt;
              }
              if (group != "Buttons" && group != "D-Pad")
                return std::nullopt;
              // Once per poll. The motion is read just before the buttons,
              // so a motion starts a poll (5 ms) late.
              if (group == "Buttons" && control == "A")
                LiveMotion(wiimote);
              return LiveHeld(group, control) ? 1.0 : 0.0;
            });
        std::printf("  input     live, from the page\n");
      }
    }
  }
  check(true, "controller configs loaded");

  // ── the Nunchuk ──
  //
  // Skylanders stops at "Connect Nunchuk to the Wii Remote" and will not
  // leave the title screen without one (scripts/record-gameplay.sh found
  // this the hard way). The extension is an attachment on the emulated
  // Wiimote with its own controls and its own input override, so the stick
  // and the C and Z buttons are driven through a second override function
  // installed on it.
  if (want_nunchuk)
  {
    bool attached = false;
    if (InputConfig* cfg = Wiimote::GetConfig())
    {
      if (auto* controller = cfg->GetController(0))
      {
        auto* wiimote = static_cast<WiimoteEmu::Wiimote*>(controller);
        auto* attachments = static_cast<ControllerEmu::Attachments*>(
            wiimote->GetWiimoteGroup(WiimoteEmu::WiimoteGroup::Attachments));
        attachments->SetSelectedAttachment(WiimoteEmu::ExtensionNumber::NUNCHUK);
        auto& nunchuk = *attachments->GetAttachmentList()[WiimoteEmu::ExtensionNumber::NUNCHUK];
        nunchuk.SetInputOverrideFunction(
            [](std::string_view group, std::string_view control,
               ControlState state) -> std::optional<ControlState> {
              if (group == "Stick")
              {
                if (control == "X")
                  return g_live_on ? g_live.stick_x : g_stick_x;
                if (control == "Y")
                  return g_live_on ? g_live.stick_y : g_stick_y;
                return std::nullopt;
              }
              if (group == "Buttons" && g_live_on)
                return LiveHeld("Nunchuk Buttons", control) ? 1.0 : 0.0;
              if (group == "Buttons")
              {
                const bool held = std::string_view(g_press_group) == "Nunchuk Buttons" &&
                                  control == g_press_control;
                return held ? 1.0 : 0.0;
              }
              return std::nullopt;
            });
        // Selected, not yet active: the emulated Wiimote plugs the new
        // extension in on a later update, as a real one would. The active
        // number is reported at the end of the run.
        attached = attachments->GetAttachmentSetting().GetValue() ==
                   static_cast<int>(WiimoteEmu::ExtensionNumber::NUNCHUK);
      }
    }
    check(attached, "Nunchuk selected on the Wii Remote");
  }

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
  Config::SetCurrent(Config::MAIN_CPU_THREAD, want_dual_core);
  Config::SetCurrent(Config::MAIN_SYNC_ON_SKIP_IDLE, want_sync_idle);
  if (sync_gpu_ticks > 0)
  {
    Config::SetCurrent(Config::MAIN_SYNC_GPU, true);
    Config::SetCurrent(Config::MAIN_SYNC_GPU_MAX_DISTANCE, sync_gpu_ticks);
    Config::SetCurrent(Config::MAIN_SYNC_GPU_MIN_DISTANCE, -sync_gpu_ticks);
    Config::SetCurrent(Config::MAIN_SYNC_GPU_OVERCLOCK, 1.0f);
  }

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

  // 1e. No speed limit. Dolphin throttles emulation to real time by
  //     default (CoreTiming::Throttle sleeps when the guest clock runs
  //     ahead of the wall clock), which is right for playing and wrong for
  //     measuring: at 95% of real time it was already costing 184 ms of a
  //     2.2 s run in clock reads and sleeps, and past 100% it would hold
  //     the speed line at exactly 100% whatever the recompiler did. Zero
  //     means unlimited (CoreTiming::IsSpeedUnlimited). --speed=1 puts the
  //     limit back, for playing (web/).
  Config::SetCurrent(Config::MAIN_EMULATION_SPEED, emulation_speed);
  // The page's input is gated by the browser already: keys only reach a
  // focused tab. Dolphin's own gate asks the host whether the render window
  // has focus, which host_stubs.cpp answers "no", and a closed gate reads
  // every MAPPED input as 0 -- the shake and the thrust, which are mapped
  // (LiveMotion), while the buttons are overrides and pass. So for live
  // input, Dolphin's "background input" setting: the gate stays open.
  if (g_live_on)
    Config::SetCurrent(Config::MAIN_INPUT_BACKGROUND_INPUT, true);

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

  // ── the pipeline UID file ──
  // Dolphin's shader cache records every pipeline it makes in
  // Cache/<game id>.uidcache -- but only if Cache/ exists, which nothing
  // in the harness made, so no run kept one.
  File::CreateFullPath(File::GetUserPath(D_CACHE_IDX));
#if defined(__EMSCRIPTEN__)
  if (!uidcache_url.empty())
  {
    emscripten_fetch_attr_t attr;
    emscripten_fetch_attr_init(&attr);
    std::strcpy(attr.requestMethod, "GET");
    attr.attributes = EMSCRIPTEN_FETCH_LOAD_TO_MEMORY | EMSCRIPTEN_FETCH_SYNCHRONOUS;
    emscripten_fetch_t* fetch = emscripten_fetch(&attr, uidcache_url.c_str());
    const std::string name = uidcache_url.substr(uidcache_url.find_last_of('/') + 1);
    const std::string path = File::GetUserPath(D_CACHE_IDX) + name;
    bool ok = false;
    if (fetch && fetch->status == 200)
    {
      File::CreateFullPath(path);
      ok = File::WriteStringToFile(path, std::string(fetch->data, fetch->numBytes));
    }
    std::printf("  uidcache  %s: %s (%llu bytes)\n", uidcache_url.c_str(),
                ok ? "loaded" : "NOT loaded",
                fetch ? static_cast<unsigned long long>(fetch->numBytes) : 0ull);
    if (fetch)
      emscripten_fetch_close(fetch);
  }
#endif

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

  // ── which file is the game reading? ──
  //
  // The disc's file table, walked once, so that every read the harness
  // serves can be named. A Wii partition's data is encrypted in 0x8000-byte
  // clusters of which 0x7C00 are payload, so a raw disc offset maps to a
  // partition offset through that geometry and then to a file through the
  // FST. Printed only when the file changes, with the emulated time: a
  // compact timeline of what the game is doing (its title screen, a movie,
  // a level) that needs no renderer.
  //
  // --disc-files prints the table itself.
  static std::vector<std::tuple<u64, u64, std::string>> disc_files;  // offset, size, path
  static u64 disc_data_start = 0;
  static const char* disc_last_file = "";
  {
    const DiscIO::Partition part = volume->GetGamePartition();
    if (const DiscIO::FileSystem* fs = volume->GetFileSystem(part); fs && fs->IsValid())
    {
      disc_data_start = volume->PartitionOffsetToRawOffset(0, part) - 0x400;
      std::function<void(const DiscIO::FileInfo&)> walk = [&](const DiscIO::FileInfo& f) {
        if (f.IsDirectory())
        {
          for (const DiscIO::FileInfo& c : f)
            walk(c);
        }
        else
        {
          disc_files.emplace_back(f.GetOffset(), f.GetSize(), f.GetPath());
        }
      };
      walk(fs->GetRoot());
      std::sort(disc_files.begin(), disc_files.end());
      if (want_disc_files)
        for (const auto& [off, size, path] : disc_files)
          std::printf("  file %012llx %9llu %s\n", static_cast<unsigned long long>(off),
                      static_cast<unsigned long long>(size), path.c_str());
      std::printf("  disc      %zu files in the game partition\n", disc_files.size());
    }
  }
  stats->on_read = [](u64 raw, u64 /*len*/) {
    if (raw < disc_data_start || disc_files.empty())
      return;
    const u64 rel = raw - disc_data_start;
    const u64 within = rel % 0x8000;
    const u64 part_off = (rel / 0x8000) * 0x7C00 + (within > 0x400 ? within - 0x400 : 0);
    // The last file starting at or before the offset.
    auto it = std::upper_bound(disc_files.begin(), disc_files.end(),
                               std::make_tuple(part_off, ~0ull, std::string()));
    if (it == disc_files.begin())
      return;
    --it;
    const auto& [off, size, path] = *it;
    if (part_off >= off + size)
      return;
    if (path.c_str() == disc_last_file || path == disc_last_file)
      return;
    disc_last_file = path.c_str();
    const auto& ct = Core::System::GetInstance().GetCoreTiming();
    std::printf("  reading   %8.1f s  %s\n", static_cast<double>(ct.GetTicks()) / 729e6,
                path.c_str());
  };

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
  // As Core.cpp does it: PopulateBackendInfo is g_Config.Refresh() from the
  // config system, ActivateBackend, the backend's InitBackendInfo and
  // VerifyValidity. ActivateBackend alone -- what the harness did until
  // --no-dolphin-video-config became the exception -- leaves g_Config and
  // g_backend_info at their C++ member defaults, among them hash samples 0:
  // a whole-texture hash on every bind, half the GPU thread in gameplay.
  //
  // Dolphin also layers the game's GameSettings INI over the base config
  // (SConfig::SetRunningGameMetadata). The harness has no Sys directory to
  // load it from, so this game's one line is set here by hand.
  if (want_xfb_to_ram)
    Config::SetCurrent(Config::GFX_HACK_SKIP_XFB_COPY_TO_RAM, false);
  if (!want_true_color)
    Config::SetCurrent(Config::GFX_ENHANCE_FORCE_TRUE_COLOR, false);
  if (!fifo_path.empty())
  {
    // Play the capture once, through, and stop: no looping, and memory
    // updates at the point in the frame where they were recorded.
    Config::SetCurrent(Config::MAIN_FIFOPLAYER_LOOP_REPLAY, false);
    Config::SetCurrent(Config::MAIN_FIFOPLAYER_EARLY_MEMORY_UPDATES, false);
  }
#if defined(HARNESS_WEBGPU)
  static WebGPU::VideoBackend webgpu_backend;
  if (want_webgpu)
  {
    // PopulateBackendInfo's steps (VideoBackendBase.cpp), with this backend
    // in place of the one the config names: it is not in Dolphin's backend
    // list, which ActivateBackend picks from.
    Config::SetCurrent(Config::GFX_HACK_EFB_EMULATE_FORMAT_CHANGES, true);
    if (!want_osd)
      Config::SetCurrent(Config::MAIN_OSD_MESSAGES, false);
    g_Config.Refresh();
    g_backend_info = {};
    g_video_backend = &webgpu_backend;
    WebGPU::g_context.fast_path = !want_webgpu_per_draw;
    WebGPU::g_context.no_upload = want_webgpu_no_upload;
    WebGPU::g_context.no_draws = want_webgpu_no_draws;
    WebGPU::g_context.no_present = want_webgpu_no_present;
    WebGPU::g_context.uniform_dedup = want_webgpu_uniform_dedup;
    g_backend_info.DisplayName = g_video_backend->GetDisplayName();
    g_video_backend->InitBackendInfo(wsi);
    g_Config.VerifyValidity();
  }
  else
#endif
  if (want_dolphin_video_config)
  {
    // GameSettings/SSP.ini: "Properly emulates the blur in the pause screen."
    Config::SetCurrent(Config::GFX_HACK_EFB_EMULATE_FORMAT_CHANGES, true);
    VideoBackendBase::PopulateBackendInfo(wsi);
  }
  else
  {
    VideoBackendBase::ActivateBackend(Config::Get(Config::MAIN_GFX_BACKEND));
  }
  check(g_video_backend != nullptr, want_webgpu ? "WebGPU video backend activated" :
                                                  "Null video backend activated");
  if (want_dump_shaders)
  {
    // What a WebGPU backend can honestly claim, written against Metal's
    // PopulateBackendInfo (MTLUtil.mm) -- the template backend, whose GLSL
    // dialect (APIType::Metal: Vulkan-style bindings, through glslang to
    // SPIR-V) is the one the translation starts from. Different from Metal
    // where core WebGPU has less: no dynamic indexing of sampler arrays,
    // no subgroup operations, and no bounding box until it is shown to be
    // needed.
    BackendInfo& bi = g_backend_info;
    bi.api_type = APIType::Metal;
    bi.bUsesLowerLeftOrigin = false;
    bi.bSupportsDualSourceBlend = true;
    bi.bSupportsPrimitiveRestart = true;
    bi.bSupportsGeometryShaders = false;
    bi.bSupportsComputeShaders = true;
    bi.bSupportsEarlyZ = true;
    bi.bSupportsBindingLayout = true;
    bi.bSupportsBBox = false;
    bi.bSupportsGSInstancing = false;
    bi.bSupportsPaletteConversion = true;
    bi.bSupportsClipControl = true;
    bi.bSupportsFragmentStoresAndAtomics = true;
    bi.bSupportsReversedDepthRange = false;
    bi.bSupportsLogicOp = false;
    bi.bSupportsGPUTextureDecoding = true;
    bi.bSupportsCopyToVram = true;
    bi.bSupportsBitfield = true;
    bi.bSupportsDynamicSamplerIndexing = false;
    bi.bSupportsFramebufferFetch = false;
    bi.bSupportsLargePoints = false;
    bi.bSupportsPartialDepthCopies = true;
    bi.bSupportsDepthReadback = true;
    bi.bSupportsTextureQueryLevels = true;
    bi.bSupportsLodBiasInSampler = false;
    bi.bSupportsDynamicVertexLoader = true;
    bi.bSupportsVSLinePointExpand = true;
    g_dump_shaders = true;
  }
  if (tex_samples >= 0)
    g_Config.iSafeTextureCache_ColorSamples = tex_samples;
  // InitializeShared copies g_Config to g_ActiveConfig, so this is what the
  // backend will run with.
  std::printf("  video     %s; hash samples %d, copy to VRAM %d, EFB format changes %d, "
              "geometry shaders %d, bbox %d\n",
              want_dolphin_video_config ? "Dolphin's config" : "member defaults",
              g_Config.iSafeTextureCache_ColorSamples, g_backend_info.bSupportsCopyToVram,
              g_Config.bEFBEmulateFormatChanges, g_backend_info.bSupportsGeometryShaders,
              g_backend_info.bSupportsBBox);

  // Single core: the GPU runs on this thread. Both of these are what
  // Core.cpp does inside its init_video lambda; passthrough means AsyncRequests
  // executes work inline instead of queueing it for a GPU thread that does not
  // exist here.
  //
  // Dual core (--dual-core): the GPU thread is spawned here exactly as
  // Core.cpp's GetInitializedVideoGuard spawns it -- it initialises the
  // backend, reports back, and runs the FIFO loop until ExitGpuLoop. This
  // thread stays the CPU thread.
  bool video_up = false;
  HarnessThread gpu_thread;
  if (want_dual_core)
  {
    std::promise<bool> init_from_thread;
    gpu_thread = SpawnGpuThread([&] {
      Common::SetCurrentThreadName("Video thread");
      Core::DeclareAsGPUThread();
      AsyncRequests::GetInstance()->SetPassthrough(false);
      const bool ok = g_video_backend && g_video_backend->Initialize(wsi);
      init_from_thread.set_value(ok);
      if (!ok)
        return;
      system.GetFifo().RunGpuLoop();
    }, want_webgpu);
    video_up = init_from_thread.get_future().get();
  }
  else
  {
    Core::DeclareAsGPUThread();
    AsyncRequests::GetInstance()->SetPassthrough(true);
    video_up = g_video_backend && g_video_backend->Initialize(wsi);
  }
  check(video_up, "video backend initialised");
  std::printf("  threads   %s\n", want_dual_core ? "dual core: the GPU on its own thread" :
                                                   "single core: the GPU on this thread");
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
  if (want_audio)
  {
    StartAudioPump(system.GetSoundStream()->GetMixer());
    std::printf("  audio     48 kHz to the page\n");
  }

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
  if (fifo_path.empty())
  {
    const Core::CPUThreadGuard guard(system);
    INIT_MARK("CBoot::BootUp");
    booted = CBoot::BootUp(system, guard, std::move(boot));
    check(booted, "CBoot::BootUp returned success");
  }

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
  // The GPU loop does nothing until told the emulator is running --
  // Core::SetState(Running) does this in Dolphin. Without it the FIFO
  // filled to 2,336 bytes and stayed there while the game's wait loop was
  // skipped as idle: 400 emulated seconds in four seconds and no frame.
  if (want_dual_core)
    system.GetFifo().EmulatorState(true);

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
  if (fifo_path.empty())
    check(state.pc != 0 && state.pc != pc_before, "boot left the CPU at an entry point");

  // ── a FIFO capture instead of the game ──
  if (!fifo_path.empty())
  {
    std::string local = fifo_path;
#if defined(__EMSCRIPTEN__)
    // Fetched whole from the page's server into the in-memory filesystem.
    {
      emscripten_fetch_attr_t attr;
      emscripten_fetch_attr_init(&attr);
      std::strcpy(attr.requestMethod, "GET");
      attr.attributes = EMSCRIPTEN_FETCH_LOAD_TO_MEMORY | EMSCRIPTEN_FETCH_SYNCHRONOUS |
                        EMSCRIPTEN_FETCH_REPLACE;
      emscripten_fetch_t* fetch =
          emscripten_fetch(&attr, ("http://127.0.0.1:8102/" + fifo_path).c_str());
      const bool got = fetch && fetch->status == 200;
      check(got, "FIFO capture fetched");
      local = "/tmp/replay.dff";
      if (got)
      {
        std::FILE* f = std::fopen(local.c_str(), "wb");
        std::fwrite(fetch->data, 1, fetch->numBytes, f);
        std::fclose(f);
        std::printf("  fifo      %s, %llu bytes\n", fifo_path.c_str(),
                    static_cast<unsigned long long>(fetch->numBytes));
      }
      if (fetch)
        emscripten_fetch_close(fetch);
    }
#endif
    auto& player = system.GetFifoPlayer();
    check(player.Open(local), "FIFO capture opened");
    std::unique_ptr<CPUCoreBase> core = player.GetCPUCore();
    check(core != nullptr, "FIFO capture has frames");
    if (core)
    {
      // FifoPlayer's own CPU core: Init loads the capture's memory and
      // registers; Run writes every frame into the FIFO and returns when
      // the last has been drawn (it powers the CPU down).
      core->Init();
      const auto t0 = std::chrono::steady_clock::now();
      core->Run();
      const double ms =
          std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
      std::printf("  fifo      %u frames replayed in %.0f ms\n", player.GetFile()->GetFrameCount(),
                  ms);
      core->Shutdown();
    }
    // The step loop has nothing to run.
    steps = 0;
  }

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
    // How many times the dispatcher has reached this PC uncompiled. A block
    // is compiled on its jit_threshold-th visit; code that runs once --
    // and a boot is mostly code that runs once -- is interpreted instead,
    // which at a few hundred nanoseconds a block is far cheaper than the
    // 24 us a module costs to install.
    u8 visits = 0;
    // Whether anything has been decided here: false means the PC has been
    // seen but neither compiled nor refused, and compile_region may still
    // compile it.
    bool decided = false;
  };
  std::unordered_map<u32, JitEntry> jit_cache;
  u64 jit_cold_runs = 0, jit_cold_insts = 0;
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
  // Direct-call holes reserved, and how many were filled with a sibling.
  u64 jit_direct_holes = 0, jit_direct_patched = 0;
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
  std::map<u32, u32> interp_where;  // the last PC seen for each key
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
  jit_layout.xer_ca_offset = static_cast<u32>(offsetof(PowerPC::PowerPCState, xer_ca));
  jit_layout.xer_stringctrl_offset =
      static_cast<u32>(offsetof(PowerPC::PowerPCState, xer_stringctrl));
  jit_layout.ps_offset = static_cast<u32>(offsetof(PowerPC::PowerPCState, ps));
  jit_layout.interp_hatch = want_hatch;
  jit_layout.hatch_mmio = want_hatch;
  // Function 0 is ppc_interp_op; 1 and 2 the compact helpers, imported
  // after it whenever the hatch is.
  if (want_hatch && want_compact)
  {
    jit_layout.hatch_exit_index = 1;
    jit_layout.mem_slow_index = 2;
    jit_layout.exit_value_addr = static_cast<u32>(reinterpret_cast<uintptr_t>(&g_jit_exit_value));
  }
  jit_layout.pc_offset = static_cast<u32>(offsetof(PowerPC::PowerPCState, pc));
  g_hatch_interp = &system.GetInterpreter();
  g_hatch_ppc = &ppc;
  g_hatch_state = &state;
  g_hatch_stats = want_jit_stats;
  g_texhash_on = want_jit_stats;
  JitImportsInit();
  jit_layout.reserve_offset = static_cast<u32>(offsetof(PowerPC::PowerPCState, reserve));
  jit_layout.reserve_address_offset =
      static_cast<u32>(offsetof(PowerPC::PowerPCState, reserve_address));
  jit_layout.fpscr_offset = static_cast<u32>(offsetof(PowerPC::PowerPCState, fpscr));
  // The time base is computed from CoreTiming's globals, which is only
  // exact at an overclock factor of 1.0; see StateLayout::tb_*.
  if (!Config::Get(Config::MAIN_OVERCLOCK_ENABLE))
  {
    auto& g = system.GetCoreTiming().GetGlobals();
    jit_layout.tb_global_timer_addr = static_cast<u32>(reinterpret_cast<uintptr_t>(&g.global_timer));
    jit_layout.tb_slice_length_addr = static_cast<u32>(reinterpret_cast<uintptr_t>(&g.slice_length));
    jit_layout.tb_start_value_addr =
        static_cast<u32>(reinterpret_cast<uintptr_t>(&g.fake_TB_start_value));
    jit_layout.tb_start_ticks_addr =
        static_cast<u32>(reinterpret_cast<uintptr_t>(&g.fake_TB_start_ticks));
  }
  jit_layout.dcache_enabled = state.m_enable_dcache;
  jit_layout.low_dcbz_hack = Config::Get(Config::MAIN_LOW_DCBZ_HACK);
  // The GQRs to specialise psq_l/psq_st on, read as each block is
  // compiled; see StateLayout::gqr_values. The emitted guard keeps the
  // result exact if the game changes one.
  if (want_psq && want_hatch)
    jit_layout.gqr_values = &state.spr[SPR_GQR0];
  jit_layout.paired_madd = want_madd && want_hatch;
  jit_layout.cr_diff = want_crdiff;
  jit_layout.probe_cold_pad = want_probe_pad;
  jit_layout.fprf = want_fprf || Config::Get(Config::MAIN_FPRF);
  jit_layout.fast_fp = want_fast_fp;
  jit_layout.short_convert = want_short_convert;
  // The write-gather pipe, when the DBATs map it where every Wii game
  // has it; see StateLayout::gather_pipe_ea. The physical tier runs the
  // exception handlers, which issue no GX commands, and keeps the hatch.
  if (want_gp && want_hatch && system.GetMMU().IsOptimizableGatherPipeWrite(0xCC008000u))
  {
    jit_layout.gather_pipe_ea = 0xCC008000u;
    jit_layout.gather_pipe_ptr_offset =
        static_cast<u32>(offsetof(PowerPC::PowerPCState, gather_pipe_ptr));
    jit_layout.gather_pipe_base_offset =
        static_cast<u32>(offsetof(PowerPC::PowerPCState, gather_pipe_base_ptr));
  }

  // ── the region table ──
  //
  // 128 host bases, one per 32 MB of guest address space, read by every
  // compiled load and store; see StateLayout::region_table. MEM1 and its
  // uncached mirror, MEM2 (two granules) and its mirror; everything else
  // -- MMIO at 0xCC000000, the EFB, the locked cache, unmapped space --
  // stays zero and bails to the interpreter, which knows what to do with
  // it. The physical addresses the guest uses with translation off are
  // not here either: compiled code only runs with MSR[DR] set.
  std::vector<u32> region_table(128, 0);
  {
    const u32 ram = static_cast<u32>(reinterpret_cast<uintptr_t>(system.GetMemory().GetRAM()));
    const u32 exram = static_cast<u32>(reinterpret_cast<uintptr_t>(system.GetMemory().GetEXRAM()));
    region_table[0x80000000u >> 25] = ram;
    region_table[0xC0000000u >> 25] = ram;
    if (exram)
    {
      region_table[0x90000000u >> 25] = exram;
      region_table[0x92000000u >> 25] = exram + 0x02000000u;
      region_table[0xD0000000u >> 25] = exram;
      region_table[0xD2000000u >> 25] = exram + 0x02000000u;
    }
  }
  jit_layout.region_table = static_cast<u32>(reinterpret_cast<uintptr_t>(region_table.data()));

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
  // hits, attempts, budget exits, misses -- OFF unless --jit-stats.
  //
  // Each counter is six wasm instructions including a load and a store,
  // and the epilogue bumps two of them on the path every chain step takes.
  // Twelve instructions per block entry, at 261 million block entries, to
  // count something nobody reads during a timing run.
  //
  // The numbers are worth having and they are not worth having by
  // default, which is the same conclusion the per-dispatch counters
  // reached. --jit-stats turns both on together.
  // 0..3 hits/attempts/budget/misses; 4..7 which kind of exit a block
  // entry left by: unconditional b, bl, a taken conditional branch, and
  // the fall-through off the end of a block.
  u32 link_stats[8] = {0, 0, 0, 0, 0, 0, 0, 0};
  if (want_jit_stats)
    jit_layout.link_stats_addr = static_cast<u32>(reinterpret_cast<uintptr_t>(link_stats));
  // Of the exits that missed, how many had a compiled successor anyway? That
  // separates a publishing or lookup fault from a successor that genuinely
  // was not compiled yet.
  u64 miss_but_cached = 0, miss_total = 0;
  jit_layout.enable_linking = want_link;
  // A hole is only worth reserving because compile_region fills it. Tied
  // to linking because a direct tail call IS the link.
  jit_layout.allow_direct = want_link;
  // Both OFF. Each was implemented, measured, and left off:
  //
  //   trace_uncond    follows b and bl. Removed 65.6M exits and created
  //                   52.7M new ones in their place -- the block simply
  //                   ends further along -- for 13M fewer entries and no
  //                   measurable speed change.
  //   trace_backward  follows the taken edge of a backward conditional
  //                   branch, unrolling loops into the block. Removed
  //                   94.2M block entries, 36% of them, and the run got
  //                   very slightly SLOWER.
  //
  // The second is the useful one. Removing a third of all block entries
  // for no gain says the cost does not scale with entries; it scales with
  // instructions. See docs/PHASE2-PROFILE.md.
  jit_layout.trace_uncond = false;
  jit_layout.trace_backward = false;

  // mtmsr stays in the chain unless the write has a consequence; see
  // StateLayout::mtmsr_continue. 24.4M of 32.5M round-trips were these.
  jit_layout.exceptions_offset = static_cast<u32>(offsetof(PowerPC::PowerPCState, Exceptions));
  jit_layout.pagetable_pending_offset =
      static_cast<u32>(offsetof(PowerPC::PowerPCState, pagetable_update_pending));
  jit_layout.mtmsr_continue = want_link && want_mtmsr_continue;

  // Idle loops hand the rest of their slice to CoreTiming::Idle; see
  // StateLayout::idle_flag_addr. The flag lives here, read and cleared by
  // the dispatcher after every chain.
  u32 jit_idle_flag = 0;
  if (want_link && want_idle_skip)
    jit_layout.idle_flag_addr = static_cast<u32>(reinterpret_cast<uintptr_t>(&jit_idle_flag));
  u64 jit_idle_exits = 0, jit_idled_ticks = 0, jit_mtmsr_exits = 0, jit_slices = 0;
  u64 jit_fast_lookups = 0;

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

  // The downcount is CoreTiming's, not the dispatcher's.
  //
  // It used to be reset to a fixed 1024 before every chain and the clock
  // advanced by whatever the chain consumed when it returned -- so every
  // chain end, 32.5 million of them over the boot, paid for
  // CoreTiming::Advance. That is not how Dolphin runs compiled code. Its
  // dispatcher lets the downcount run from one Advance to the next: Advance
  // sets it to the distance to the next scheduled event (capped at
  // MAX_SLICE_LENGTH), blocks decrement it as they run, and only when it
  // reaches zero does the clock advance and events fire. A chain that ends
  // early for any other reason -- a link miss, a bail, an mtmsr that needs
  // the interpreter -- simply dispatches again with whatever budget is left.
  //
  // Measured over an 800M-instruction boot the dispatcher loop and Advance
  // were about 40% of the run between them, against 25% for the compiled
  // code itself. This is the change that addresses that.
  const u32 jit_state_ptr = static_cast<u32>(reinterpret_cast<uintptr_t>(&state));
  auto& jit_timing = system.GetCoreTiming();
  auto& jit_interpreter = system.GetInterpreter();
  bool jit_started = false;

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

  // ── two tiers: translation on, translation off ──
  //
  // Compiled code maps guest addresses through a fixed table, so a block is
  // only right for one setting of MSR[DR]. Code that runs with translation
  // off -- the OS's exception prologue and epilogue at physical 0x500 and
  // friends, on every interrupt -- used to go to the interpreter on the
  // dispatcher's DR guard: measured past the boot, most of what was still
  // interpreted. It gets its own cache, link table and layout, whose
  // region table maps the PHYSICAL addresses 0x00000000 (MEM1) and
  // 0x10000000 (MEM2). A chain cannot change DR without leaving (mtmsr
  // flips it as a sensitive bit; rfi goes through the hatch and reports a
  // new PC), so a chain is wholly one tier or the other, and the
  // dispatcher picks the tier by DR at every entry.
  struct Tier
  {
    std::unordered_map<u32, JitEntry>& cache;
    std::vector<u32>& link_table;
    ppc2wasm::StateLayout& layout;
    bool physical;
  };
  std::unordered_map<u32, JitEntry> jit_cache_phys;
  constexpr u32 kLinkEntriesPhys = 1u << 16;  // the handlers are a few kilobytes of code
  std::vector<u32> link_table_phys(kLinkEntriesPhys * 2, 0);
  std::vector<u32> region_table_phys(128, 0);
  {
    const u32 ram = static_cast<u32>(reinterpret_cast<uintptr_t>(system.GetMemory().GetRAM()));
    const u32 exram = static_cast<u32>(reinterpret_cast<uintptr_t>(system.GetMemory().GetEXRAM()));
    region_table_phys[0x00000000u >> 25] = ram;
    if (exram)
    {
      region_table_phys[0x10000000u >> 25] = exram;
      region_table_phys[0x12000000u >> 25] = exram + 0x02000000u;
    }
  }
  ppc2wasm::StateLayout jit_layout_phys = jit_layout;
  jit_layout_phys.gather_pipe_ea = 0;
  jit_layout_phys.link_base = static_cast<u32>(reinterpret_cast<uintptr_t>(link_table_phys.data()));
  jit_layout_phys.link_mask = kLinkEntriesPhys - 1;
  jit_layout_phys.region_table =
      static_cast<u32>(reinterpret_cast<uintptr_t>(region_table_phys.data()));
  Tier tier_virt{jit_cache, link_table, jit_layout, false};
  Tier tier_phys{jit_cache_phys, link_table_phys, jit_layout_phys, true};
  u64 jit_phys_dispatches = 0;

  auto compile_region = [&](Tier& tier, u32 seed) {
    // The tier's state, under the names the rest of this lambda uses.
    auto& jit_cache = tier.cache;
    auto& link_table = tier.link_table;
    auto& jit_layout = tier.layout;
    // The locked L1 cache, for blocks compiled while the DBATs map it;
    // see StateLayout::l1_cache_base. Decided here, per compile, because
    // the game's OS init sets that BAT long after this harness starts.
    // The physical tier runs with translation off and keeps the bail.
    if (want_l1 && !tier.physical && system.GetMemory().GetL1Cache())
    {
      jit_layout.l1_cache_base =
          system.GetMMU().IsOptimizableRAMAddress(0xE0000000u, 32) ?
              static_cast<u32>(reinterpret_cast<uintptr_t>(system.GetMemory().GetL1Cache())) :
              0;
    }
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
      // Code in MEM1 or MEM2, with room for a full block read. A Wii game
      // runs relocatable modules out of MEM2, and before this accepted
      // them every block there was interpreted forever.
      const bool in_mem1 = tier.physical
                               ? p + kMaxBlock * 4 <= 0x1800000
                               : (p & 0xFE000000) == 0x80000000 &&
                                     (p & 0x01FFFFFF) + kMaxBlock * 4 <= 0x1800000;
      const bool in_mem2 = (p & 0xFC000000) == (tier.physical ? 0x10000000u : 0x90000000u) &&
                           (p & 0x03FFFFFF) + kMaxBlock * 4 <= 0x4000000;
      if (!in_mem1 && !in_mem2)
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
    if (want_hatch)
    {
      mb.ImportFunction("env", "ppc_interp_op", {ppc2wasm::kI32, ppc2wasm::kI32, ppc2wasm::kI32},
                        {ppc2wasm::kI32});
      // Imported whether or not they are used, so the indices are fixed.
      mb.ImportFunction("env", "ppc_hatch_exit",
                        {ppc2wasm::kI32, ppc2wasm::kI32, ppc2wasm::kI32, ppc2wasm::kI32},
                        {ppc2wasm::kI32});
      mb.ImportFunction("env", "ppc_mem_slow",
                        {ppc2wasm::kI32, ppc2wasm::kI32, ppc2wasm::kI32, ppc2wasm::kI32,
                         ppc2wasm::kI32},
                        {ppc2wasm::kI32});
    }
    mb.SetSignature({ppc2wasm::kI32}, {ppc2wasm::kI32});

    struct Pending
    {
      u32 pc;
      u32 instructions;
      u8 stop;
      u32 succ[2];
      u32 nsucc;
      ppc2wasm::Bytes body;
      u32 locals;
      u32 locals_i64;
      std::vector<ppc2wasm::DirectFixup> fixups;
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
    // performance.now() is not free: 21,000 calls measured at 175 ms of
    // self time in a 2.2 s profile. Timed only when the statistics are on.
    const double t_translate0 = want_jit_stats ? JitNow() : 0.0;
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
      {
        auto seen = jit_cache.find(bpc);
        if (seen != jit_cache.end() && seen->second.decided)
          continue;
      }
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
        // The body is held rather than added to the module now. A direct
        // call needs the function index of its target, and that is not
        // known until the whole region has been discovered -- the target
        // may be a block this walk has not reached yet.
        pending.push_back({bpc, r.instructions, stop, {r.succ[0], r.succ[1]}, r.nsucc,
                           std::move(r.body), r.locals, r.locals_i64, std::move(r.fixups)});
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
        JitEntry& none = jit_cache[bpc];
        none.stop = stop;
        none.decided = true;
      }
    }
    if (want_jit_stats)
      jit_t_translate += JitNow() - t_translate0;

    // The seed MUST end up in the cache, compiled or not. Everything
    // downstream does jit_cache.find(pc) and dereferences the result, and
    // the walk can decline to look at the seed at all -- a PC outside MEM1
    // is rejected by the bound above before anything reads it. A negative
    // entry there means "interpret this", which is the right answer and
    // the one the old one-block-at-a-time path produced by accident.
    auto ensure_seed = [&]() {
      jit_cache[seed].decided = true;
    };

    if (pending.empty())
    {
      ensure_seed();
      return;
    }

    // ── fill the direct-call holes ──
    //
    // Function index k is pending[k], because that is the order they are
    // added to the module below. A target that is not in this region
    // leaves its hole as nops and falls back to the table lookup.
    //
    // Linear search over at most kRegion entries, which beats building a
    // map for sixteen items.
    // Imported functions take the low indices, so a sibling's function
    // index is its position in `pending` plus the import count.
    const int import_count = static_cast<int>(mb.ImportCount());
    auto index_of = [&pending, import_count](u32 target) -> int {
      for (size_t k = 0; k < pending.size(); ++k)
        if (pending[k].pc == target)
          return static_cast<int>(k) + import_count;
      return -1;
    };
    for (size_t k = 0; k < pending.size(); ++k)
    {
      jit_direct_holes += pending[k].fixups.size();
      jit_direct_patched += ppc2wasm::PatchDirectCalls(pending[k].body, pending[k].fixups,
                                                       index_of);
      mb.AddFunction(pending[k].body, "f" + std::to_string(k), pending[k].locals,
                     pending[k].locals_i64);
      if (want_jit_names)
      {
        char nm[16];
        std::snprintf(nm, sizeof(nm), "b_%08x", pending[k].pc);
        mb.SetFunctionName(static_cast<uint32_t>(k), nm);
      }
    }

    const double t_install0 = want_jit_stats ? JitNow() : 0.0;
    const auto bytes = mb.Build();
    for (size_t k = 0; k < pending.size(); ++k)
      if (std::find(jit_dump_pcs.begin(), jit_dump_pcs.end(), pending[k].pc) !=
          jit_dump_pcs.end())
      {
        std::printf("JITMOD %08x f%zu %zu\n", pending[k].pc, k, bytes.size());
        EmitBase64(bytes);
      }
    const int base =
        JitInstallBatch(bytes.data(), static_cast<int>(bytes.size()),
                        static_cast<int>(pending.size()));
    if (want_jit_stats)
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
      if (!want_jit_stats)
        break;
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
      JitEntry& e = jit_cache[pending[k].pc];
      e.index = (base < 0) ? -1 : base + static_cast<int>(k);
      e.instructions = pending[k].instructions;
      e.stop = pending[k].stop;
      e.decided = true;
      if (e.index >= 0)
      {
        // Publish it so other blocks can jump here directly.
        const u32 slot = (pending[k].pc >> 2) & jit_layout.link_mask;
        link_table[slot * 2 + 0] = pending[k].pc;
        link_table[slot * 2 + 1] = static_cast<u32>(e.index);
      }
    }
    ensure_seed();
  };
#endif

  // Run one step, through a compiled block when there is one. Returns how
  // many guest instructions were executed.
  auto step_once = [&]() -> u32 {
#if defined(__EMSCRIPTEN__)
    if (!want_jit)
    {
      ppc.SingleStep();
      return 1;
    }

    // ── the slice ──
    //
    // Advance opens a slice: it fires every event that is due, sets the
    // downcount to the distance to the next one, and delivers any external
    // interrupt that became deliverable -- which may move state.pc, so the
    // PC is read AFTER this and not before.
    if (!jit_started || state.downcount <= 0)
    {
      jit_started = true;
      ++jit_slices;
      jit_timing.Advance();
    }

    const u32 pc = state.pc;

    // One instruction through the interpreter, the way Interpreter::Run's
    // fast loop does it: no CoreTiming::Advance per instruction. (The same
    // lambda is defined again below, after the lookup, for the paths there;
    // this copy serves the one early exit above it.)
    auto interpret_one_outer = [&]() -> u32 {
      ++jit_interp;
      jit_interpreter.SingleStepInner();
      if (state.Exceptions != 0)
      {
        ppc.CheckExceptions();
        state.pc = state.npc;
      }
      state.downcount -= 1;
      return 1;
    };

    // The tier is chosen by MSR[DR]: compiled code maps guest addresses
    // for exactly one setting of it. See Tier above.
    if (!state.msr.DR && !want_phys)
      return interpret_one_outer();
    Tier& tier = state.msr.DR ? tier_virt : tier_phys;
    auto& jit_cache = tier.cache;
    auto& link_table = tier.link_table;
    auto& jit_layout = tier.layout;
    if (tier.physical)
      ++jit_phys_dispatches;

    // ── find the block ──
    //
    // The link table first. It is a direct-mapped {pc, index} cache that
    // emitted code already consults on every chain step, so for a PC that
    // has been compiled and published it answers in one load and one
    // compare -- against a hash, a bucket walk and a compare for the
    // unordered_map, which is now only consulted when the table misses:
    // an uncompiled PC, a retired one, or a collision.
    int index = -1;
    JitEntry* entry = nullptr;
    {
      const u32 slot = (pc >> 2) & jit_layout.link_mask;
      if (link_table[slot * 2 + 0] == pc && link_table[slot * 2 + 1] != 0)
        index = static_cast<int>(link_table[slot * 2 + 1]);
    }
    if (index < 0)
    {
      entry = &jit_cache[pc];
      if (!entry->decided)
      {
        if (++entry->visits < jit_threshold)
        {
          // Not hot yet. Interpret the straight-line run from here -- up to
          // the first taken branch or exception -- and come back when the
          // PC is reached again. Only the START of the run gets a cache
          // entry, so a cold path does not seed the cache with one entry
          // per instruction.
          //
          // The run stops at a budget boundary too, so the clock advances
          // on time; and it stops if translation is turned off, since the
          // dispatcher's own guard would otherwise be bypassed for the
          // rest of the run.
          u32 ran = 0;
          while (true)
          {
            const u32 at = state.pc;
            ++jit_interp;
            jit_interpreter.SingleStepInner();
            if (state.Exceptions != 0)
            {
              ppc.CheckExceptions();
              state.pc = state.npc;
            }
            state.downcount -= 1;
            ++ran;
            if (state.pc != at + 4 || ran >= kMaxBlock || state.downcount <= 0)
              break;
          }
          ++jit_cold_runs;
          jit_cold_insts += ran;
          return ran;
        }
        compile_region(tier, pc);
        // compile_region may rehash the map; take the entry again.
        entry = &jit_cache[pc];
      }
      index = entry->index;
    }

    // One instruction through the interpreter, the way Interpreter::Run's
    // fast loop does it rather than the way Interpreter::SingleStep does:
    // no CoreTiming::Advance per instruction. SingleStep exists for the
    // debugger and declares a new one-tick slice every call, which is why
    // the harness's interpreter measures 18 MIPS and why 1.4% of
    // instructions used to cost 15% of the run.
    auto interpret_one = [&]() -> u32 {
      ++jit_interp;
      jit_interpreter.SingleStepInner();
      if (state.Exceptions != 0)
      {
        ppc.CheckExceptions();
        state.pc = state.npc;
      }
      // One tick per instruction, the same charge compiled code makes.
      state.downcount -= 1;
      return 1;
    };

    if (index < 0)
    {
      // What the interpreter is left holding. Keyed as --jit-coverage keys
      // it so the two rankings can be read side by side -- the interesting
      // comparison is an instruction that is RARE in the stream and common
      // here, which is what a missing form looks like.
      if (want_jit_stats && entry)
      {
        const u32 ins = system.GetMemory().Read_U32(state.pc);
        const u32 primary = ins >> 26;
        const u32 key = (primary == 31 || primary == 19 || primary == 63)
                            ? (primary << 16) | (((ins >> 1) & 0x3FF) << 1) | (ins & 1)
                            : (primary << 16);
        auto& slot = (entry->retired ? interp_retired : interp_counts)[key];
        ++slot.first;
        slot.second = ins;
        interp_where[key] = state.pc;
        if (entry->retired)
          ++interp_from_retired;
      }
      return interpret_one();
    }
    if (want_jit_stats && entry)
      ++stop_dyn[entry->stop];

    // No translation guard here any more: the tier was chosen by MSR[DR]
    // above, and MSR cannot change inside a chain without the chain
    // leaving -- an mtmsr that flips DR returns here, and rfi reports a
    // new PC through the hatch.

    using BlockFn = u32 (*)(u32);
    BlockFn f = reinterpret_cast<BlockFn>(index);
    if (!entry)
      ++jit_fast_lookups;

    // Blocks decrement the downcount themselves, so what a chain consumed
    // is simply the difference. No chain ever runs with a non-positive
    // budget: the slice check above guarantees it is positive on entry.
    const int dc_before = state.downcount;
    const u32 ret = f(jit_state_ptr);

    const bool bailed = (ret & 1u) != 0;
    const bool msr_changed = (ret & 2u) != 0;
    const u32 next_pc = ret & ~3u;
    state.pc = state.npc = next_pc;
    if (bailed)
      ++jit_bails;

    int consumed = dc_before - state.downcount;
    if (consumed < 0)
      consumed = 0;

    if (consumed == 0 && !state.msr.FP)
    {
      // Refused with floating point turned off. The Wii's OS switches FP
      // contexts lazily: MSR[FP] is cleared on every thread switch and the
      // first FP instruction of the next thread traps, so this is a block
      // that begins with an FP instruction doing exactly what it should.
      // Retiring it would interpret it forever for one expected trap --
      // measured as 64,000 interpreted lfd before this check existed.
      // Interpret the one instruction, which raises the exception, and
      // keep the block.
      return interpret_one();
    }

    if (consumed == 0)
    {
      // The block refused on its very first instruction and did nothing.
      //
      // Compiling every block, however short, is what makes a successor
      // exist to link to -- but it also creates blocks whose first
      // instruction always refuses, usually a memory access outside RAM.
      // Those cost a call and a chain step and never execute anything, and
      // with linking on they end a chain that was otherwise running.
      //
      // Retire it: drop the entry and clear its link slot so chains stop
      // routing through a block that cannot make progress. The decision is
      // made from OBSERVED behaviour, which is the only way -- whether an
      // address lands in RAM is not knowable when the block is compiled.
      //
      // A block found through the link table has no `entry` in hand; it
      // is looked up here, on this cold path, rather than on every step.
      if (!entry)
        entry = &jit_cache[pc];
      entry->index = -1;
      entry->retired = true;
      entry->decided = true;
      ++jit_retired_blocks;
      if (want_jit_stats && jit_retired_blocks <= 64)
      {
        // What refused, and the address it would have touched, for a
        // D-form memory instruction: the usual cause is an address no
        // region covers, and this says which.
        const u32 ins = system.GetMemory().Read_U32(pc);
        const u32 op = ins >> 26;
        const u32 ra = (ins >> 16) & 0x1F;
        s32 disp = 0;
        if (op >= 56 && op <= 61)
          disp = static_cast<s32>(ins << 20) >> 20;
        else
          disp = static_cast<s16>(ins & 0xFFFF);
        const u32 ea = (ra ? state.gpr[ra] : 0) + static_cast<u32>(disp);
        std::printf("  retire    pc %08x ins %08x ea %08x msr %08x hid2 %08x gqr%u %08x\n", pc,
                    ins, ea, state.msr.Hex, state.spr[SPR_HID2], (ins >> 12) & 7,
                    state.spr[SPR_GQR0 + ((ins >> 12) & 7)]);
      }
      const u32 slot = (pc >> 2) & jit_layout.link_mask;
      if (link_table[slot * 2 + 0] == pc)
      {
        link_table[slot * 2 + 0] = 0;
        link_table[slot * 2 + 1] = 0;
      }
      return interpret_one();
    }

    // An idle exit: the block proved nothing it does can change its own
    // outcome before the next event, so the rest of the slice is handed to
    // CoreTiming::Idle -- which is what Dolphin's JIT does with the same
    // loops. The next Advance then fires that event immediately.
    if (jit_idle_flag)
    {
      jit_idle_flag = 0;
      ++jit_idle_exits;
      const int rest = state.downcount;
      if (rest > 0)
      {
#if defined(__EMSCRIPTEN__)
        if (g_idle_stats)
        {
          const u32 backlog = Core::System::GetInstance()
                                  .GetCommandProcessor()
                                  .GetFifo()
                                  .CPReadWriteDistance.load(std::memory_order_relaxed);
          const double idle_t0 = JitNow();
          jit_timing.Idle();
          NoteIdleWait(JitNow() - idle_t0, backlog);
        }
        else
        {
          jit_timing.Idle();
        }
#else
        jit_timing.Idle();
#endif
        jit_idled_ticks += static_cast<u64>(rest);
      }
    }

    if (want_link && !bailed && !msr_changed && jit_idle_flag == 0 && state.downcount > 0)
    {
      // This exit came back to the dispatcher with budget to spare, so the
      // chain stopped on a lookup miss. Was its successor something we had
      // already compiled? Only counted with the statistics on: it is a
      // second hash lookup per chain otherwise.
      if (want_jit_stats)
      {
        ++miss_total;
        auto nx = jit_cache.find(next_pc);
        if (nx != jit_cache.end() && nx->second.index >= 0)
          ++miss_but_cached;
      }
    }

    if (msr_changed)
    {
      // The block wrote MSR and stopped. Everything the interpreter does
      // after that write happens here: translation state is recomputed, and
      // any interrupt that was already pending is taken -- which is the
      // whole reason enabling MSR[EE] cannot be a plain store.
      ++jit_mtmsr_exits;
      ppc.MSRUpdated();
      // Interpreter::mtmsr's next step: an FP exception already
      // summarised in FPSCR[FEX] is taken when FE0 or FE1 enables it.
      // Found by the chain differential, which seeds FPSCR with FEX set.
      CheckFPExceptions(state);
      ppc.CheckExceptions();
      state.pc = state.npc;
    }

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
    {
      const int keep = state.downcount;
      jit_timing.GetGlobals().slice_length = 0;
      state.downcount = 0;
      jit_timing.Advance();
      state.downcount = keep;
      break;
    }
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

  // Emulated time, for the speed figure that actually matters. MIPS says
  // how fast instructions run; with idle loops skipped, instructions and
  // time come apart, and what the player feels is emulated seconds per
  // wall second. The Wii's clock is 729 MHz, so that is ticks / 729e6.
  const s64 ticks_at_step_start = system.GetCoreTiming().GetTicks();
  // Checks that need to run every step only when a flag asks for them.
  // Each is cheap alone; together, every step, they were a measurable part
  // of a dispatcher loop that runs 40 million times.
  const bool slow_checks = want_trace || checkpoint != 0 || trace_from != 0 || want_events ||
                           frame_every != 0 || want_coverage || watch_addr != 0 || dump_at != 0;
  u64 next_mark = 0;
  // The second stopwatch (--speed-from), and a frame counter: the XFB the
  // Video Interface scans out alternates between two buffers, so a change
  // of its address is a frame the game finished. Frames per emulated
  // second says whether the game is running at its own frame rate, which
  // no instruction count can.
  u64 next_thumb = xfb_every;
  bool window_open = false;
  u64 window_idle_n0 = 0, window_idle_bucket0[5] = {}, window_fifo0[4] = {};
  double window_idle_ms0 = 0;
  double window_t0 = 0.0;
  u64 window_exec0 = 0;
  s64 window_ticks0 = 0;
  u64 window_frames0 = 0;
  TexHashSnapshot window_texhash0{};
  const TexHashSnapshot run_texhash0 = TexHashNow();
  u64 frames = 0;
  u32 last_xfb = 0;
  // --frame-times: the previous frame's end.
  double ft_wall0 = 0, ft_idle0 = 0;
  s64 ft_ticks0 = 0;
  u64 ft_compile0 = 0, ft_disc0 = 0;
  g_gx_armed = (gx_from == 0);

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
    // moves several times a minute under the wasm interpreter. A threshold
    // rather than a mask, because a compiled chain advances `i` by dozens
    // at a time and would step over an exact multiple almost every time.
    if (i >= next_mark)
    {
      char buf[96];
      std::snprintf(buf, sizeof(buf), "stepping %lluM",
                    static_cast<unsigned long long>(i / 1000000));
      InitMark(buf);
      g_harness_emulated_s =
          static_cast<double>(system.GetCoreTiming().GetTicks() - ticks_at_step_start) / 729e6;
      next_mark = i + (1u << 22);
    }
    if (tick_cap &&
        static_cast<u64>(system.GetCoreTiming().GetTicks() - ticks_at_step_start) >= tick_cap)
      break;

    // Events are logged over the same window as the trace. --events turns
    // logging on from the first step, which --from cannot express: a window
    // starting at zero is indistinguishable from "no window".
    g_cur_step = i;
    if (want_input && !g_input_sequence.empty())
    {
      const u64 t = static_cast<u64>(system.GetCoreTiming().GetTicks() - ticks_at_step_start);
      g_stick_x = g_stick_y = 0.0;
      if ((t % g_press_period) < g_press_length)
      {
        const auto& e = g_input_sequence[(t / g_press_period) % g_input_sequence.size()];
        g_press_group = e.first;
        g_press_control = e.second;
        if (std::string_view(e.first) == "Nunchuk Stick")
        {
          // Not a button: a deflection on one axis for the whole window.
          const bool x = e.second[0] == 'X';
          const double v = e.second[1] == '+' ? 0.95 : -0.95;
          (x ? g_stick_x : g_stick_y) = v;
        }
      }
      else
      {
        g_press_group = "";
        g_press_control = "";
      }
      if (walk_from && t >= walk_from)
      {
        static constexpr double kDir[4][2] = {{0, 0.95}, {0.95, 0}, {0, -0.95}, {-0.95, 0}};
        const auto& d = kDir[((t - walk_from) / 1458000000ull) % 4];
        g_stick_x = d[0];
        g_stick_y = d[1];
      }
    }
    if (slow_checks)
    {
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
    }  // slow_checks
    const u32 ran_now = step_once();
    i += ran_now - 1;  // the for-loop's own ++i accounts for the first
    executed += ran_now;
    min_pc = std::min(min_pc, state.pc);
    max_pc = std::max(max_pc, state.pc);

    // A frame: the scanned-out XFB moved.
    {
      const u32 xfb = system.GetVideoInterface().GetXFBAddressTop();
      if (xfb != last_xfb)
      {
        last_xfb = xfb;
        ++frames;
#if defined(__EMSCRIPTEN__)
        if (want_frame_times)
        {
          const double now = JitNow();
          const s64 ticks = system.GetCoreTiming().GetTicks();
          const u64 cus = CompileUs();
          const u64 dus = stats->us.load(std::memory_order_relaxed);
          if (ft_wall0 > 0)
            g_frame_times.push_back({(ticks - ticks_at_step_start) / 729e6, now - ft_wall0,
                                     (ticks - ft_ticks0) / 729e3, (cus - ft_compile0) / 1000.0,
                                     (dus - ft_disc0) / 1000.0, g_idle_ms - ft_idle0});
          ft_wall0 = now;
          ft_ticks0 = ticks;
          ft_compile0 = cus;
          ft_disc0 = dus;
          ft_idle0 = g_idle_ms;
        }
#endif
      }
    }
    if (xfb_every &&
        static_cast<u64>(system.GetCoreTiming().GetTicks() - ticks_at_step_start) >= next_thumb)
    {
      next_thumb += xfb_every;
      const u32 xfb = system.GetVideoInterface().GetXFBAddressTop();
      if (xfb)
      {
        const auto yuv = ReadXfbYuv(system, xfb);
        const auto rgb = YuvToRgb(yuv);
        // Every fourth pixel of every fourth line.
        std::vector<u8> ppm;
        {
          char hdr[64];
          const int n = std::snprintf(hdr, sizeof(hdr), "P6\n%u %u\n255\n", XFB_W / 4, XFB_H / 4);
          ppm.assign(hdr, hdr + n);
        }
        for (u32 y = 0; y < XFB_H; y += 4)
          for (u32 x = 0; x < XFB_W; x += 4)
          {
            const size_t p = (static_cast<size_t>(y) * XFB_W + x) * 3;
            ppm.push_back(rgb[p]);
            ppm.push_back(rgb[p + 1]);
            ppm.push_back(rgb[p + 2]);
          }
        std::printf("  xfb thumbnail at %.1f emulated s, emitting as base64\n",
                    static_cast<double>(system.GetCoreTiming().GetTicks() - ticks_at_step_start) /
                        729e6);
        EmitBase64(ppm);
      }
    }
    if (record_fifo_at && !g_fifo_recording_started)
    {
      const u64 t = static_cast<u64>(system.GetCoreTiming().GetTicks() - ticks_at_step_start);
      if (t >= record_fifo_at)
      {
        g_fifo_recording_started = true;
        std::printf("  fifo      recording 3 frames at %.1f emulated s\n", t / 729e6);
        system.GetFifoRecorder().StartRecording(3, [&system] {
          FifoDataFile* file = system.GetFifoRecorder().GetRecordedFile();
          g_fifo_recorded_ok = file && file->Save("/tmp/recorded.dff");
          g_fifo_recording_done = true;
        });
      }
    }
    if (g_fifo_recording_done)
      break;
    if ((gx_from && !g_gx_armed) || (speed_from && !window_open))
    {
      const u64 t = static_cast<u64>(system.GetCoreTiming().GetTicks() - ticks_at_step_start);
      if (gx_from && !g_gx_armed && t >= gx_from)
        g_gx_armed = true;
      if (speed_from && !window_open && t >= speed_from)
      {
        window_open = true;
#if defined(__EMSCRIPTEN__)
        window_t0 = JitNow();
#endif
        window_exec0 = executed;
        window_ticks0 = system.GetCoreTiming().GetTicks();
        window_idle_n0 = g_idle_n;
        window_idle_ms0 = g_idle_ms;
        g_idle_long_n = g_idle_long_backlog = g_idle_long_backlog_max = 0;
        std::copy(std::begin(g_idle_bucket), std::end(g_idle_bucket), window_idle_bucket0);
        g_idle_longest_ms = 0;
        window_fifo0[0] = g_fifo_payloads.load();
        window_fifo0[1] = g_fifo_spins.load();
        window_fifo0[2] = g_fifo_chunks.load();
        window_fifo0[3] = g_fifo_batches.load();
        window_frames0 = frames;
        window_texhash0 = TexHashNow();
      }
    }

    // Two atomic loads a step until the milestone is reached, none after.
    if (fifo_written_at < 0)
    {
      if (fifo_configured_at < 0 && fifo.CPBase.load() != 0)
      {
        fifo_configured_at = static_cast<s64>(i);
        fifo_configured_pc = state.pc;
      }
      if (fifo.CPWritePointer.load() != wp_initial)
      {
        fifo_written_at = static_cast<s64>(i);
        fifo_written_pc = state.pc;
      }
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
    if (g_gx_limit && g_gx_size.load(std::memory_order_acquire) >= g_gx_limit)
      break;
  }

#if defined(__EMSCRIPTEN__)
  {
    // Wall time for the stepping phase only. This is the number the whole
    // recompiler exists to move, and it is reported whether or not --jit is
    // on so the two runs are directly comparable.
    const double ms = JitNow() - step_t0;
    const double ticks =
        static_cast<double>(system.GetCoreTiming().GetTicks() - ticks_at_step_start);
    // 729 MHz is the Wii's clock; one emulated second is that many ticks.
    const double emu_s = ticks / 729e6;
    std::printf("  speed     %.0f ms, %.1f MIPS, %.3f emulated s, %.1f%% of real time  (%s)\n", ms,
                executed / (ms / 1000.0) / 1e6, emu_s, 100.0 * emu_s / (ms / 1000.0),
                want_jit ? "jit" : "interpreter");
    std::printf("  frames    %llu over the run, %.1f per emulated second\n",
                static_cast<unsigned long long>(frames), emu_s > 0 ? frames / emu_s : 0.0);
    if (InputConfig* cfg = Wiimote::GetConfig())
      if (auto* controller = cfg->GetController(0))
        std::printf("  wiimote   active extension %d (1 = Nunchuk)\n",
                    static_cast<int>(static_cast<WiimoteEmu::Wiimote*>(controller)
                                         ->GetActiveExtensionNumber()));
    if (want_dump_uidcache)
    {
      // The shader cache appends through a stdio handle it never closes
      // here (the run ends in _exit); flush it before reading the file.
      std::fflush(nullptr);
      for (const auto& entry : File::ScanDirectoryTree(File::GetUserPath(D_CACHE_IDX), false).children)
      {
        if (!entry.virtualName.ends_with(".uidcache"))
          continue;
        std::string data;
        File::ReadFileToString(entry.physicalName, data);
        std::printf("UIDCACHE %s %zu\n", entry.virtualName.c_str(), data.size());
        EmitBase64(std::vector<u8>(data.begin(), data.end()));
        std::printf("UIDCACHE END\n");
      }
    }
    if (want_frame_times)
    {
#if defined(HARNESS_WEBGPU)
      {
        static const char* const kPhase[] = {"glslang", "tint", "module", "pipeline"};
        std::printf("  compile   whole run:");
        for (int p = 0; p < WebGPU::kCompilePhases; ++p)
          std::printf(" %s %llu in %.0f ms;", kPhase[p],
                      static_cast<unsigned long long>(WebGPU::g_phase_n[p].load()),
                      WebGPU::g_phase_us[p].load() / 1000.0);
        std::printf("\n");
      }
#endif
      ReportFrameTimes("whole run", 0.0);
      if (speed_from)
        ReportFrameTimes("window", speed_from / 729e6);
    }
    if (window_open)
    {
      const double wms = JitNow() - window_t0;
      const double wticks =
          static_cast<double>(system.GetCoreTiming().GetTicks() - window_ticks0);
      const double wemu = wticks / 729e6;
      std::printf("  window    from %.1f emulated s: %.0f ms, %.1f MIPS, %.3f emulated s, "
                  "%.1f%% of real time, %.1f frames per emulated second\n",
                  speed_from / 729e6, wms, (executed - window_exec0) / (wms / 1000.0) / 1e6, wemu,
                  100.0 * wemu / (wms / 1000.0), wemu > 0 ? (frames - window_frames0) / wemu : 0.0);
      const u64 idle_n = g_idle_n - window_idle_n0;
      const double idle_ms = g_idle_ms - window_idle_ms0;
      if (g_idle_stats)
      std::printf("  idle      %llu idle syncs, waited %.0f ms (%.1f%% of the window), longest "
                  "%.2f ms; <=5us %llu, <=50us %llu, <=500us %llu, <=2ms %llu, more %llu\n",
                  static_cast<unsigned long long>(idle_n), idle_ms, 100.0 * idle_ms / wms,
                  g_idle_longest_ms,
                  static_cast<unsigned long long>(g_idle_bucket[0] - window_idle_bucket0[0]),
                  static_cast<unsigned long long>(g_idle_bucket[1] - window_idle_bucket0[1]),
                  static_cast<unsigned long long>(g_idle_bucket[2] - window_idle_bucket0[2]),
                  static_cast<unsigned long long>(g_idle_bucket[3] - window_idle_bucket0[3]),
                  static_cast<unsigned long long>(g_idle_bucket[4] - window_idle_bucket0[4]));
      if (g_idle_stats && g_idle_long_n)
        std::printf("  idle      the %llu waits over 2 ms entered with a FIFO backlog of %.0f KB "
                    "on average, %.0f KB at most\n",
                    static_cast<unsigned long long>(g_idle_long_n),
                    g_idle_long_backlog / 1024.0 / g_idle_long_n, g_idle_long_backlog_max / 1024.0);
      const double fp = (g_fifo_payloads.load() - window_fifo0[0]) / wemu;
      const double fs = (g_fifo_spins.load() - window_fifo0[1]) / wemu;
      const double fc = (g_fifo_chunks.load() - window_fifo0[2]) / wemu;
      const double fb = (g_fifo_batches.load() - window_fifo0[3]) / wemu;
      std::printf("  fifo      per emulated second: GPU loop runs %.0f (%.0f found nothing), "
                  "decoder runs %.0f over %.0f chunks (%.0f KB); CP enables seen %u "
                  "(1 breakpoint, 2 high watermark, 4 low)\n",
                  fp, fs, fb, fc, fc * 32 / 1024, g_fifo_enables.load());
    }
  }
#endif
  if (gpu_thread.joinable())
  {
    // After the clock has been read, before anything below reads what the
    // GPU thread wrote: the capture vector, the FIFO state. ExitGpuLoop
    // returns once the loop has stopped; the FIFO may still hold what the
    // CPU wrote last.
    system.GetFifo().ExitGpuLoop();
    gpu_thread.join();
  }
  if (record_fifo_at)
  {
    check(g_fifo_recorded_ok, "FIFO capture recorded");
    if (g_fifo_recorded_ok)
    {
      std::FILE* f = std::fopen("/tmp/recorded.dff", "rb");
      std::vector<u8> bytes;
      if (f)
      {
        u8 buf[65536];
        size_t n;
        while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0)
          bytes.insert(bytes.end(), buf, buf + n);
        std::fclose(f);
      }
      std::printf("  fifo      %zu bytes recorded, emitting as base64\n", bytes.size());
      static const char* T = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
      std::string line;
      for (size_t k = 0; k < bytes.size(); k += 3)
      {
        const u32 v = (bytes[k] << 16) | (k + 1 < bytes.size() ? bytes[k + 1] << 8 : 0) |
                      (k + 2 < bytes.size() ? bytes[k + 2] : 0);
        line += T[(v >> 18) & 63];
        line += T[(v >> 12) & 63];
        line += k + 1 < bytes.size() ? T[(v >> 6) & 63] : '=';
        line += k + 2 < bytes.size() ? T[v & 63] : '=';
        if (line.size() >= 76)
        {
          std::printf("D %s\n", line.c_str());
          line.clear();
        }
      }
      if (!line.empty())
        std::printf("D %s\n", line.c_str());
    }
  }
  if (g_dump_shaders)
  {
    std::lock_guard<std::mutex> lock(g_shader_mutex);
    static const char* T = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    for (const auto& [stage, text] : g_shaders)
    {
      std::string line = "SHADER " + std::to_string(stage) + " ";
      for (size_t k = 0; k < text.size(); k += 3)
      {
        const u32 v = (u8(text[k]) << 16) | (k + 1 < text.size() ? u8(text[k + 1]) << 8 : 0) |
                      (k + 2 < text.size() ? u8(text[k + 2]) : 0);
        line += T[(v >> 18) & 63];
        line += T[(v >> 12) & 63];
        line += k + 1 < text.size() ? T[(v >> 6) & 63] : '=';
        line += k + 2 < text.size() ? T[v & 63] : '=';
      }
      std::printf("%s\n", line.c_str());
    }
    std::printf("  shaders   %zu distinct\n", g_shaders.size());
  }
  if (g_texhash_on)
  {
    const s64 ticks_now = system.GetCoreTiming().GetTicks();
    PrintTexHash("whole run", run_texhash0, frames,
                 static_cast<double>(ticks_now - ticks_at_step_start) / 729e6);
    if (window_open)
      PrintTexHash("window", window_texhash0, frames - window_frames0,
                   static_cast<double>(ticks_now - window_ticks0) / 729e6);
  }
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
      {
        static const char* kLbl[7] = {"1", "2", "3", "4", "5-8", "9-15", "16"};
        std::printf("  jit       region sizes:");
        for (int k = 0; k < 7; ++k)
          std::printf("  %s=%llu", kLbl[k], static_cast<unsigned long long>(region_hist[k]));
        std::printf("\n");
      }
      if (want_jit_stats)
      {
        std::printf("  jit       exits: uncond-b %u  bl %u  cond-taken %u  fallthrough %u\n",
                    link_stats[4], link_stats[5], link_stats[6], link_stats[7]);
        std::printf("  jit       %llu direct-call holes, %llu filled with a sibling (%.1f%%)\n",
                    static_cast<unsigned long long>(jit_direct_holes),
                    static_cast<unsigned long long>(jit_direct_patched),
                    jit_direct_holes ? 100.0 * static_cast<double>(jit_direct_patched) /
                                           static_cast<double>(jit_direct_holes)
                                     : 0.0);
        std::printf("  jit       %llu static successor edges, %llu are siblings (%.1f%%)\n",
                    static_cast<unsigned long long>(succ_edges),
                    static_cast<unsigned long long>(succ_sibling),
                    succ_edges ? 100.0 * static_cast<double>(succ_sibling) /
                                     static_cast<double>(succ_edges)
                               : 0.0);
        // Derived from the link hit counter, which is itself behind this
        // flag. Printed without it, the sum is the round-trip count
        // wearing the wrong label -- it read 24.02 instructions per entry
        // where the real figure is 3.02.
        const u64 entries = jit_blocks + static_cast<u64>(link_stats[0]);
        std::printf("  jit       %llu block entries, %.2f instructions per entry\n",
                    static_cast<unsigned long long>(entries),
                    entries ? static_cast<double>(jit_insts) / static_cast<double>(entries)
                            : 0.0);
      }
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
            std::printf("  jit       %s %zu: primary %2u xo %4u  %10llu  %5.1f%%  (%08x at %08x)\n",
                        label[m], k + 1, top[k].second >> 16, (top[k].second & 0xFFFF) >> 1,
                        static_cast<unsigned long long>(top[k].first),
                        tot ? 100.0 * static_cast<double>(top[k].first) /
                                  static_cast<double>(tot)
                            : 0.0,
                        (*maps[m])[top[k].second].second, interp_where[top[k].second]);
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
    std::printf("  jit       %llu instructions through the hatch (%.2f%% of executed)\n",
                static_cast<unsigned long long>(g_hatch_calls),
                executed ? 100.0 * static_cast<double>(g_hatch_calls) /
                               static_cast<double>(executed)
                         : 0.0);
    if (want_jit_stats && !g_hatch_hist.empty())
    {
      std::vector<std::pair<u64, u32>> top;
      for (const auto& kv : g_hatch_hist)
        top.push_back({kv.second, kv.first});
      std::sort(top.begin(), top.end(), std::greater<>());
      for (size_t k = 0; k < top.size() && k < 12; ++k)
        std::printf("  jit       hatch %2zu: primary %2u xo %4u  %10llu  %5.1f%%\n", k + 1,
                    top[k].second >> 16, (top[k].second & 0xFFFF) >> 1,
                    static_cast<unsigned long long>(top[k].first),
                    100.0 * static_cast<double>(top[k].first) /
                        static_cast<double>(g_hatch_calls));
    }
    std::printf("  jit       translation-off tier: %zu blocks cached, %llu dispatches\n",
                jit_cache_phys.size(), static_cast<unsigned long long>(jit_phys_dispatches));
    std::printf("  jit       threshold %u: %llu cold runs interpreted, %llu instructions\n",
                jit_threshold, static_cast<unsigned long long>(jit_cold_runs),
                static_cast<unsigned long long>(jit_cold_insts));
    std::printf("  jit       %llu slices, %llu idle exits skipping %llu ticks, %llu mtmsr exits, "
                "%llu of %llu dispatches through the link table\n",
                static_cast<unsigned long long>(jit_slices),
                static_cast<unsigned long long>(jit_idle_exits),
                static_cast<unsigned long long>(jit_idled_ticks),
                static_cast<unsigned long long>(jit_mtmsr_exits),
                static_cast<unsigned long long>(jit_fast_lookups),
                static_cast<unsigned long long>(jit_blocks));
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
  const bool early_stop_ok = stop_at_fifo || g_fifo_limit || g_gx_limit || tick_cap != 0;
  // A compiled block runs to its own end, so the step limit is a floor
  // rather than an exact count -- the last block can carry execution a few
  // instructions past it. Requiring equality was right for an interpreter
  // stepping one at a time and is simply wrong now.
  check(early_stop_ok ? state.pc != 0 : executed >= steps,
        early_stop_ok ? "ran until the stop condition without falling off"
                      : "ran at least the full step count without falling off");
  if (fifo_path.empty())
    check(state.pc != 0, "pc is still valid");

  // Is anything draining the FIFO? If CPReadWriteDistance only grows, the GPU
  // side never ran and a game waiting for FIFO space would stall -- a very
  // different diagnosis from a game waiting on an interrupt.
  if (g_vertex_stats)
    PrintVertexTally();
  VertexJitReport();
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

  for (const auto& [addr, count] : disasm_ranges)
  {
    std::printf("\n  ── guest code at %08x ──\n", addr);
    auto& mem = system.GetMemory();
    for (u32 k = 0; k < count; ++k)
    {
      const u32 a = addr + k * 4;
      const u32 w = mem.Read_U32(a);
      std::printf("  %08x  %08x  %s\n", a, w, Common::GekkoDisassembler::Disassemble(w, a).c_str());
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
