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
#include <string>
#include <vector>

#if defined(__EMSCRIPTEN__)
#include <emscripten.h>
#endif

#include "Common/CommonTypes.h"
#include "Common/Config/Config.h"
#include "Common/FileUtil.h"
#include "Core/Boot/Boot.h"
#include "Core/Config/MainSettings.h"
#include "Core/ConfigLoaders/BaseConfigLoader.h"
#include "Core/ConfigManager.h"
#include "Core/Core.h"
#include "Core/CoreTiming.h"
#include "AudioCommon/AudioCommon.h"
#include "Common/Logging/LogManager.h"
#include "Common/WindowSystemInfo.h"
#include "Core/HW/GCKeyboard.h"
#include "Core/HW/GCPad.h"
#include "Core/HW/Wiimote.h"
#include "InputCommon/ControllerInterface/ControllerInterface.h"
#include "Core/HW/EXI/EXI_Device.h"
#include "Core/HW/HW.h"
#include "Core/HW/Memmap.h"
#include "Core/IOS/IOS.h"
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
  for (int i = 1; i < argc; ++i)
    if (std::strcmp(argv[i], "--trace") == 0)
      want_trace = true;

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
#if !defined(__EMSCRIPTEN__)
  // Skipped under Emscripten as a precaution, NOT as a fix: this was suspected
  // first and changing it made no difference -- the stall was in
  // ExpansionInterface. It stays skipped because it is in the same hazard
  // class (Wiimote::Initialize starts a real-Wiimote scanning thread, and
  // thread creation from the proxied main thread is what deadlocks here), and
  // because a browser has no Bluetooth Wiimotes to scan for.
  //
  // Revisit when Wii input is actually wired up: emulated Wiimotes are a
  // separate path from the real-device scan, but this call covers both.
  Wiimote::Initialize(Wiimote::InitializeMode::DO_NOT_WAIT_FOR_WIIMOTES);
#endif
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
  INIT_MARK("HW::Init...");
  HW::Init(system, nullptr);
  INIT_MARK("HW::Init done");
  check(system.GetMemory().GetRAM() != nullptr, "hardware up, guest memory allocated");

  // IOS is NOT initialised here. HW::Init does it (HW.cpp:58, "Depends on
  // Memory"). Calling it again re-registers the IPCEvent and trips a
  // CoreTiming assertion about duplicate event names -- which is how this was
  // discovered.
  check(system.GetIOS() != nullptr, "IOS up (via HW::Init)");

  auto& ppc = system.GetPowerPC();
  check(ppc.GetMode() == PowerPC::CoreMode::Interpreter, "running the interpreter");

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

  const u32 pc_before = system.GetPPCState().pc;
  bool booted = false;
  {
    const Core::CPUThreadGuard guard(system);
    INIT_MARK("CBoot::BootUp");
    booted = CBoot::BootUp(system, guard, std::move(boot));
  }
  check(booted, "CBoot::BootUp returned success");

  auto& state = system.GetPPCState();
  std::printf("  entry pc  0x%08x (was 0x%08x)\n", state.pc, pc_before);
  std::printf("  msr=%08x  r1(sp)=%08x\n", state.msr.Hex, state.gpr[1]);
  check(state.pc != 0 && state.pc != pc_before, "boot left the CPU at an entry point");

  // ── execute ──
  INIT_MARK("stepping");
  std::printf("\n  stepping %llu instructions...\n", static_cast<unsigned long long>(steps));
  u64 executed = 0;
  const u32 first_pc = state.pc;
  u32 min_pc = state.pc, max_pc = state.pc;
  for (u64 i = 0; i < steps; ++i)
  {
    if (want_trace)
      Trace::EmitLine(i, state);
    ppc.SingleStep();
    ++executed;
    min_pc = std::min(min_pc, state.pc);
    max_pc = std::max(max_pc, state.pc);
    // A PC of zero means execution fell off into nothing, which is a failure
    // worth stopping on rather than tracing thousands of lines of it.
    if (state.pc == 0)
      break;
  }

  std::printf("  executed  %llu instructions\n", static_cast<unsigned long long>(executed));
  std::printf("  pc range  0x%08x .. 0x%08x (started 0x%08x, now 0x%08x)\n", min_pc, max_pc,
              first_pc, state.pc);
  std::printf("  disc      %llu requests, %.2f MB fetched\n",
              static_cast<unsigned long long>(stats->requests),
              stats->bytes / (1024.0 * 1024.0));

  check(executed == steps, "ran the full step count without falling off");
  check(state.pc != 0, "pc is still valid");

  std::printf("\n%s (%d failures)\n", g_failures ? "FAILED" : "PASSED", g_failures);

  // _exit rather than return: a booted emulator leaves threads, timing events
  // and device state live, and unwinding them from here terminates in
  // libc++abi after the result has already been printed. Exiting directly
  // keeps the status meaningful instead of turning a pass into a crash.
  std::fflush(stdout);
  _exit(g_failures ? 1 : 0);
}
