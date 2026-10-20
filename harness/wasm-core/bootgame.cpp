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
static bool g_press_a = false;

// One emulated second is about 729 million instructions (the Wii CPU clock,
// and this harness advances one tick per instruction). Press for ~0.2s every
// ~2s: long enough to register, with gaps long enough for a menu to act on it.
static constexpr u64 PRESS_PERIOD = 1458000000ull;  // ~2.0s
static constexpr u64 PRESS_LENGTH = 150000000ull;   // ~0.2s
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
  bool want_skylander = false;
  u16 sky_id = 0, sky_var = 0;
  u64 checkpoint = 0;
  u64 trace_from = 0;
  u64 dump_at = 0;
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
    else if (std::strcmp(argv[i], "--input") == 0)
      want_input = true;
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
              if (group == "Buttons" && control == "A")
                return g_press_a ? 1.0 : 0.0;
              return std::nullopt;
            });
        check(true, "synthetic input installed (A on a fixed schedule)");
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
  Config::SetCurrent(Config::MAIN_GFX_BACKEND, std::string("Null"));

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
  u64 executed = 0;
  const u32 first_pc = state.pc;
  u32 min_pc = state.pc, max_pc = state.pc;

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
    if (want_input)
      g_press_a = (i % PRESS_PERIOD) < PRESS_LENGTH;
    if (i >= trace_from && (want_events || trace_from))
      g_event_log_on = true;

    if (i >= trace_from && (want_trace || (checkpoint && i % checkpoint == 0)))
      Trace::EmitLine(i, state);
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
    ppc.SingleStep();
    ++executed;
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

  std::printf("  executed  %llu instructions\n", static_cast<unsigned long long>(executed));
  std::printf("  pc range  0x%08x .. 0x%08x (started 0x%08x, now 0x%08x)\n", min_pc, max_pc,
              first_pc, state.pc);
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
  check(early_stop_ok ? state.pc != 0 : executed == steps,
        early_stop_ok ? "ran until the stop condition without falling off"
                      : "ran the full step count without falling off");
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

  std::printf("\n%s (%d failures)\n", g_failures ? "FAILED" : "PASSED", g_failures);

  // _exit rather than return: a booted emulator leaves threads, timing events
  // and device state live, and unwinding them from here terminates in
  // libc++abi after the result has already been printed. Exiting directly
  // keeps the status meaningful instead of turning a pass into a crash.
  std::fflush(stdout);
  _exit(g_failures ? 1 : 0);
}
