// Link stubs for the native build of the smoke test.
//
// SkylanderFigure.cpp references list_skylanders and NormalizeSkylanderType
// from GetData(). Pulling in the real Skylander.cpp would drag the whole
// emulated USB device stack and ~400 more symbols with it, so these are stubbed
// instead.
//
// This is only sound because the smoke test calls Create() and DecryptFigure()
// and never GetData(). If that stops being true the stub must not quietly
// answer wrong -- so NormalizeSkylanderType aborts rather than guessing, and
// the empty map below would make any real lookup fail visibly.
//
// The wasm link does not need these: at -O2 wasm-ld eliminates the unreached
// reference, while ld64 does not. Same source, different link behaviour.

#include <cstdio>
#include <cstdlib>
#include <map>
#include <utility>

#include "Common/CommonTypes.h"
#include "Core/IOS/USB/Emulated/Skylanders/Skylander.h"

namespace IOS::HLE::USB
{
const std::map<const std::pair<const u16, const u16>, SkyData> list_skylanders = {};

Type NormalizeSkylanderType(Type type)
{
  std::fprintf(stderr,
               "native_stubs: NormalizeSkylanderType() was called, but the smoke test "
               "stubs it out. The figure-data path is not covered here.\n");
  std::abort();
}
}  // namespace IOS::HLE::USB
