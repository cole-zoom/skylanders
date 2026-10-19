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
#include "VideoCommon/AsyncRequests.h"
#include "VideoCommon/CommandProcessor.h"
#include "VideoCommon/VideoBackendBase.h"
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
  // --until-fifo: treat the step count as a cap and stop at the first FIFO
  // write instead of running it out. Reaching the FIFO takes far more
  // instructions than a fixed differential run, and every step past the
  // milestone is browser time spent learning nothing.
  bool stop_at_fifo = false;
  u64 checkpoint = 0;
  u64 trace_from = 0;
  u64 dump_at = 0;
  for (int i = 1; i < argc; ++i)
  {
    if (std::strcmp(argv[i], "--trace") == 0)
      want_trace = true;
    else if (std::strcmp(argv[i], "--until-fifo") == 0)
      stop_at_fifo = true;
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
  INIT_MARK("HW::Init...");
  HW::Init(system, nullptr);
  INIT_MARK("HW::Init done");
  check(system.GetMemory().GetRAM() != nullptr, "hardware up, guest memory allocated");

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

  check(stop_at_fifo ? state.pc != 0 : executed == steps,
        stop_at_fifo ? "ran until the FIFO write (or the cap) without falling off"
                     : "ran the full step count without falling off");
  check(state.pc != 0, "pc is still valid");

  std::printf("\n%s (%d failures)\n", g_failures ? "FAILED" : "PASSED", g_failures);

  // _exit rather than return: a booted emulator leaves threads, timing events
  // and device state live, and unwinding them from here terminates in
  // libc++abi after the result has already been printed. Exiting directly
  // keeps the status meaningful instead of turning a pass into a crash.
  std::fflush(stdout);
  _exit(g_failures ? 1 : 0);
}
