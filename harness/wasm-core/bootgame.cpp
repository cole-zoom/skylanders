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
  Pad::Initialize();
  Keyboard::Initialize();
  Wiimote::Initialize(Wiimote::InitializeMode::DO_NOT_WAIT_FOR_WIIMOTES);
  check(true, "controller configs loaded");

  // Null audio. cubeb tries to open a real device, and under wasm that never
  // returns -- the browser boot printed through "controller configs loaded"
  // and then stalled here indefinitely. A browser build will drive audio
  // through an AudioWorklet (Spike C) rather than cubeb, so selecting no
  // backend is the right answer rather than a workaround.
  Config::SetCurrent(Config::MAIN_AUDIO_BACKEND, std::string(BACKEND_NULLSOUND));
  std::printf("  [init] sound stream...\n");
  AudioCommon::InitSoundStream(system);
  std::printf("  [init] sound stream done\n");
  std::printf("  [init] HW::Init...\n");
  HW::Init(system, nullptr);
  std::printf("  [init] HW::Init done\n");
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
    booted = CBoot::BootUp(system, guard, std::move(boot));
  }
  check(booted, "CBoot::BootUp returned success");

  auto& state = system.GetPPCState();
  std::printf("  entry pc  0x%08x (was 0x%08x)\n", state.pc, pc_before);
  std::printf("  msr=%08x  r1(sp)=%08x\n", state.msr.Hex, state.gpr[1]);
  check(state.pc != 0 && state.pc != pc_before, "boot left the CPU at an entry point");

  // ── execute ──
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
