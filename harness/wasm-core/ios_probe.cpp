// How far does IOS come up, and what is still missing?
//
// IOS is the Wii's operating system, HLE'd by Dolphin, and it is the layer the
// game reaches the Skylander portal through. PR #9 identified it as the one
// uncovered layer in the Skylanders stack: figure crypto and the portal's data
// path are both verified byte-for-byte against native, but every access the
// game actually makes goes through an IOS device node.
//
// This is deliberately NOT part of the coreboot differential. Both targets
// currently fail here, at different points, which means the setup below is
// incomplete rather than that wasm is broken:
//
//   wasm32  the kernel constructs and every device node is present, then the
//           DESTRUCTOR traps (EmulationKernel::~EmulationKernel removes its
//           CoreTiming events, clears the device map, and resets the socket
//           manager -- one of those three)
//   native  SIGSEGV during IOS::HLE::Init, before the kernel is constructed
//
// Two targets failing differently is the signature of missing initialisation,
// not a port bug. Dolphin normally reaches IOS through Core::Init, which brings
// up far more than this file does. Keeping the probe separate keeps the
// differential honest: coreboot.cpp stays green on both targets and this
// records exactly how far IOS gets.
//
// Build:  harness/wasm-core/link.sh harness/wasm-core/ios_probe.cpp /tmp/ios.js
//         harness/wasm-core/link-native.sh harness/wasm-core/ios_probe.cpp /tmp/ios-native

#include <cstdio>
#include <string>

#include "Common/CommonTypes.h"
#include "Common/Config/Config.h"
#include "Common/FileUtil.h"
#include "Core/Config/MainSettings.h"
#include "Core/ConfigManager.h"
#include "Core/CoreTiming.h"
#include "Core/HW/Memmap.h"
#include "Core/IOS/IOS.h"
#include "Core/System.h"
#include "Core/WiiRoot.h"
#include "UICommon/UICommon.h"

static int g_failures = 0;

static void check(bool ok, const char* what)
{
  std::printf("  %-52s %s\n", what, ok ? "ok" : "FAIL");
  if (!ok)
    ++g_failures;
}

int main()
{
  std::setvbuf(stdout, nullptr, _IONBF, 0);
#if defined(__EMSCRIPTEN__)
  std::printf("target: wasm32 (emscripten)\n\n");
#else
  std::printf("target: native\n\n");
#endif

  auto& system = Core::System::GetInstance();
  Config::Init();
  SConfig::Init();
  system.Initialize();

  system.SetIsWii(true);
  system.GetMemory().Init();
  check(system.GetMemory().GetRAM() != nullptr, "guest memory up");

  // FS::MakeFileSystem asserts on an empty root path. SetUserDirectory alone
  // leaves D_SESSION_WIIROOT_IDX empty; InitializeWiiRoot(true) fills it in
  // with a temporary NAND, which is what a test wants.
  const std::string user_dir = "/tmp/dolphin-ios-probe/";
  File::CreateFullPath(user_dir);
  UICommon::SetUserDirectory(user_dir);
  Core::InitializeWiiRoot(true);
  check(!File::GetUserPath(D_SESSION_WIIROOT_IDX).empty(), "session NAND root is set");

  system.GetCoreTiming().Init();
  check(true, "core timing initialised");

  std::printf("  [about to call IOS::HLE::Init -- native SIGSEGVs here]\n");
  IOS::HLE::Init(system);

  auto* ios = system.GetIOS();
  check(ios != nullptr, "IOS kernel constructed");
  if (ios)
  {
    // /dev/usb/oh0 is the node the game opens to reach the portal, so its
    // presence is the point of the whole exercise.
    for (const char* node : {"/dev/aes", "/dev/sha", "/dev/es", "/dev/usb/oh0"})
      check(ios->GetDeviceByName(node) != nullptr, node);
  }

  std::printf("  [about to destroy the kernel -- wasm32 traps here]\n");
  system.SetIOS(nullptr);
  check(true, "kernel destroyed");

  system.GetCoreTiming().Shutdown();
  Core::ShutdownWiiRoot();
  system.GetMemory().Shutdown();

  std::printf("\n%s (%d failures)\n", g_failures ? "FAILED" : "PASSED", g_failures);
  return g_failures ? 1 : 0;
}
