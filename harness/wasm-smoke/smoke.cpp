// First execution of Dolphin code compiled to WebAssembly.
//
// Two static libraries linking says nothing about whether they run. This
// project has found that gap four times already -- indexed vertex arrays, a
// devirtualized benchmark, a wrong struct stride, and MemArenaUnix itself all
// compiled or ran cleanly while being wrong.
//
// So this exercises the two things most likely to be silently broken, and
// prints a result that can be diffed between native and wasm32:
//
//   1. MemArenaEmscripten -- does the replacement arena actually work, and
//      does it refuse the fastmem reservation as designed?
//
//   2. SkylanderFigure::Create -- does the MIFARE crypto produce byte-identical
//      figure data under wasm32? This is the most load-bearing untested thing
//      in the project. An endianness or integer-width difference here yields
//      figures the game rejects with no obvious cause.
//
// Build both targets and compare stdout. They must match exactly.

#include <array>
#include <cstdio>
#include <cstring>
#include <string>

#include "Common/CommonTypes.h"
#include "Common/MemArena.h"
#include "Core/IOS/USB/Emulated/Skylanders/SkylanderFigure.h"

static int g_failures = 0;

static void check(bool ok, const char* what)
{
  std::printf("  %-52s %s\n", what, ok ? "ok" : "FAIL");
  if (!ok)
    ++g_failures;
}

// FNV-1a. Small, dependency-free, and stable across platforms -- which is the
// whole point: the hash must differ only if the bytes differ.
static u64 fnv1a(const u8* data, size_t len)
{
  u64 h = 0xcbf29ce484222325ULL;
  for (size_t i = 0; i < len; ++i)
  {
    h ^= data[i];
    h *= 0x100000001b3ULL;
  }
  return h;
}

static void test_mem_arena()
{
  std::printf("MemArena\n");
  Common::MemArena arena;

  const size_t size = 24 * 1024 * 1024;  // MEM1
  arena.GrabSHMSegment(size, "smoke");

  u8* view = static_cast<u8*>(arena.CreateView(0, size));
  check(view != nullptr, "CreateView returns a pointer");
  if (!view)
    return;

  // Guest memory must start zeroed.
  bool zeroed = true;
  for (size_t i = 0; i < size; i += 4096)
    zeroed &= (view[i] == 0);
  check(zeroed, "segment is zero-initialised");

  // Round-trip through the view, including the last byte, which catches an
  // allocation that is short of the requested size.
  view[0] = 0xAB;
  view[size - 1] = 0xCD;
  check(view[0] == 0xAB && view[size - 1] == 0xCD, "first and last byte round-trip");

  // Dolphin's actual contract, from Memory::Init: one view per physical
  // region, at distinct non-overlapping offsets, each independently usable.
  //
  // Two earlier versions of this check asserted things Dolphin never relies on
  // -- first that a second view sits at base+offset (true only on Emscripten),
  // then that overlapping views alias (true on neither). Both times the test
  // was wrong, not the arena. The contract is non-overlapping views, so that
  // is what gets tested.
  arena.ReleaseView(view, size);
  arena.ReleaseSHMSegment();

  const size_t a_size = 16 * 1024 * 1024, b_size = 8 * 1024 * 1024;
  arena.GrabSHMSegment(a_size + b_size, "smoke2");
  u8* a = static_cast<u8*>(arena.CreateView(0, a_size));
  u8* b = static_cast<u8*>(arena.CreateView(a_size, b_size));
  check(a && b, "two non-overlapping views are created");
  if (a && b)
  {
    std::memset(a, 0x11, a_size);
    std::memset(b, 0x22, b_size);
    bool clean = (a[0] == 0x11 && a[a_size - 1] == 0x11 && b[0] == 0x22 &&
                  b[b_size - 1] == 0x22);
    check(clean, "regions hold their own data without overlapping");
    arena.ReleaseView(b, b_size);
    arena.ReleaseView(a, a_size);
  }
  view = nullptr;

  // Deliberately asymmetric. On wasm the point of MemArenaEmscripten is to
  // REFUSE fastmem rather than fake it; on a native host the reservation is
  // expected to succeed. A test that demanded the same answer on both would be
  // testing the wrong thing.
  u8* fastmem = arena.ReserveMemoryRegion(0x4000'0000);
#if defined(__EMSCRIPTEN__)
  check(fastmem == nullptr, "ReserveMemoryRegion refuses (fastmem unavailable)");
  check(arena.GetPageSize() == 65536, "page size is the wasm page size");
#else
  check(fastmem != nullptr, "ReserveMemoryRegion succeeds (native fastmem)");
  if (fastmem)
    arena.ReleaseMemoryRegion();
  check(arena.GetPageSize() > 0, "page size is sane");
#endif

  arena.ReleaseSHMSegment();
  std::printf("\n");
}

static void test_skylander_crypto()
{
  std::printf("Skylander figure\n");

  // A fixed NUID makes Create() deterministic. Without it the figure carries a
  // random tag id and nothing could be compared across targets.
  const std::array<u8, 4> nuid{0x01, 0x23, 0x45, 0x67};

  struct Case
  {
    u16 id, var;
    const char* name;
  };
  // Spyro is the figure record-gameplay.sh puts on the portal. The others
  // cover a different element and a variant with a non-zero var field.
  const Case cases[] = {
      {16, 0x0000, "Spyro"},
      {14, 0x0000, "Gill Grunt"},
      {0, 0x1801, "Whirlwind (S2)"},
  };

  for (const Case& c : cases)
  {
    const std::string path = "/tmp/smoke_" + std::to_string(c.id) + "_" +
                             std::to_string(c.var) + ".sky";
    std::remove(path.c_str());

    IOS::HLE::USB::SkylanderFigure fig(path);
    if (!fig.Create(c.id, c.var, nuid))
    {
      std::printf("  %-52s FAIL (Create returned false)\n", c.name);
      ++g_failures;
      continue;
    }

    std::array<u8, IOS::HLE::USB::FIGURE_SIZE> decrypted{};
    fig.DecryptFigure(&decrypted);
    fig.Close();

    // The decrypted image is what the game reads back through the portal, so
    // hashing it covers both the figure layout and the MIFARE key derivation.
    std::printf("  %-20s id=%-3u var=0x%04X  decrypted fnv1a=%016llx\n", c.name, c.id, c.var,
                static_cast<unsigned long long>(fnv1a(decrypted.data(), decrypted.size())));
  }
  std::printf("\n");
}

int main()
{
#if defined(__EMSCRIPTEN__)
  std::printf("target: wasm32 (emscripten)\n");
#else
  std::printf("target: native\n");
#endif
  std::printf("pointer size: %zu bytes\n\n", sizeof(void*));

  test_mem_arena();
  test_skylander_crypto();

  std::printf("%s (%d failures)\n", g_failures ? "FAILED" : "PASSED", g_failures);
  return g_failures ? 1 : 0;
}
