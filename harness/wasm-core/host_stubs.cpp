// Host and frontend boundaries for a headless core.
//
// Dolphin's core calls out to a frontend for a handful of things. A browser
// build will eventually provide real implementations; this supplies the
// minimum needed to bring the core up in a test.
//
// The libusb stubs are different in kind and deliberately ABORT. Phase 1 wants
// the EMULATED Skylander portal reached and host USB never touched. If any
// libusb entry point is called, that invariant is broken, and the test must
// die saying so rather than quietly enumerating nothing.

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "Core/Host.h"

// ── frontend callbacks ──
// A headless core needs these to exist; none of them need to do anything.
std::vector<std::string> Host_GetPreferredLocales()
{
  return {};
}
void Host_Message(HostMessageID)
{
}
void Host_UpdateTitle(const std::string&)
{
}
void Host_UpdateDiscordClientID(const std::string&)
{
}
bool Host_UIBlocksControllerState()
{
  return false;
}
bool Host_UpdateDiscordPresenceRaw(const std::string&, const std::string&, const std::string&,
                                   const std::string&, const std::string&, const std::string&,
                                   long long, long long, int, int)
{
  return false;
}
void Host_UpdateDisasmDialog()
{
}
void Host_JitCacheInvalidation()
{
}
void Host_JitProfileDataWiped()
{
}
void Host_UpdateMainFrame()
{
}
void Host_RequestRenderWindowSize(int, int)
{
}
bool Host_RendererHasFocus()
{
  return false;
}
bool Host_RendererHasFullFocus()
{
  return false;
}
bool Host_RendererIsFullscreen()
{
  return false;
}
bool Host_TASInputHasFocus()
{
  return false;
}
void Host_YieldToUI()
{
}
void Host_TitleChanged()
{
}
void Host_PPCSymbolsChanged()
{
}
void Host_PPCBreakpointsChanged()
{
}
void Host_RefreshDSPDebuggerWindow()
{
}

// Emscripten's pthread implementation does not provide pthread_setname_np.
// Dolphin names its threads for debugging only, so a no-op is correct -- the
// names simply do not appear.
// wasm only. Emscripten's pthread implementation does not provide
// pthread_setname_np, and Dolphin calls the two-argument form
// (pthread_t, const char*). Defining it unconditionally breaks the native
// build, where macOS declares the single-argument form in <pthread.h> and
// supplies a real implementation.
//
// wasm-ld only WARNS on a signature mismatch here rather than erroring, so the
// wrong arity links cleanly and corrupts the call at runtime -- the warning is
// the only signal.
#if defined(__EMSCRIPTEN__)
extern "C" int pthread_setname_np(unsigned long, const char*)
{
  return 0;
}
#endif

// ── host USB: must never be reached ──
//
// Only for wasm. The native differential build links the real libusb (it has
// __LIBUSB__ defined, since only Emscripten excludes it), and these would
// collide with it. The invariant being protected -- "the browser build must
// use the EMULATED portal" -- is a wasm invariant anyway.
#if defined(__EMSCRIPTEN__)
extern "C" {
#if defined(__EMSCRIPTEN__)
[[noreturn]] static void no_host_usb(const char* fn)
{
  std::fprintf(stderr,
               "host_stubs: %s was called.\n"
               "This build must use the EMULATED Skylander portal "
               "(Main.EmulatedUSBDevices.EmulateSkylanderPortal), never host USB.\n",
               fn);
  std::abort();
}
#endif
int libusb_init(void**) { no_host_usb("libusb_init"); }
void libusb_exit(void*) { no_host_usb("libusb_exit"); }
int libusb_handle_events_timeout_completed(void*, void*, int*) {
  no_host_usb("libusb_handle_events_timeout_completed");
}
void libusb_interrupt_event_handler(void*) { no_host_usb("libusb_interrupt_event_handler"); }
const char* libusb_error_name(int) { no_host_usb("libusb_error_name"); }
const char* libusb_strerror(int) { no_host_usb("libusb_strerror"); }
int libusb_interrupt_transfer(void*, unsigned char, unsigned char*, int, int*, unsigned int) {
  no_host_usb("libusb_interrupt_transfer");
}
}
#endif  // __EMSCRIPTEN__
