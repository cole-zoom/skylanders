// Read the actual Skylanders disc through RangeBlobReader.
//
// Everything before this used a synthetic image. This opens the real thing --
// a 4.7 GB Wii disc, encrypted and partitioned -- and asks DiscIO for the game
// id, the partitions, and a file out of the filesystem, counting every byte
// fetched along the way.
//
// The point is the count. docs/PLAN.md Bet 05 claims the browser never needs
// the whole disc resident because a game touches a small fraction of it. That
// is either true or it is not, and the only way to find out is to read a real
// disc and measure.
//
// Native reads the file directly. wasm32 cannot -- MEMFS is not the host
// filesystem -- so it fetches ranges over HTTP from a server that honours the
// Range header, which is exactly the shape a browser will use.
//
// Usage:
//   realdisc <path-or-url>

#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "Common/CommonTypes.h"
#include "DiscIO/Blob.h"
#include "DiscIO/DiscExtractor.h"
#include "DiscIO/DiscUtils.h"
#include "DiscIO/Filesystem.h"
#include "DiscIO/Volume.h"
#include "DiscIO/VolumeDisc.h"

#include "bytesources.h"
#include "rangeblob.h"

static int g_failures = 0;

static void check(bool ok, const char* what)
{
  std::printf("  %-52s %s\n", what, ok ? "ok" : "FAIL");
  if (!ok)
    ++g_failures;
}

static std::string Human(u64 bytes)
{
  char buf[64];
  if (bytes >= 1024 * 1024)
    std::snprintf(buf, sizeof(buf), "%.1f MB", bytes / (1024.0 * 1024.0));
  else if (bytes >= 1024)
    std::snprintf(buf, sizeof(buf), "%.1f KB", bytes / 1024.0);
  else
    std::snprintf(buf, sizeof(buf), "%llu B", static_cast<unsigned long long>(bytes));
  return buf;
}

int main(int argc, char** argv)
{
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  const std::string location = argc > 1 ? argv[1] : "/tmp/ssa.iso";

  auto stats = std::make_shared<Harness::SourceStats>();
  Harness::ByteSource source;
  u64 disc_size = 0;

#if defined(__EMSCRIPTEN__)
  std::printf("target: wasm32 (emscripten)  source: %s\n\n", location.c_str());
  // A Wii disc is exactly this size; asking the server for it would cost a
  // request and the value is fixed by the format.
  disc_size = 4699979776ull;
  source = Harness::HttpByteSource(location, stats);
#else
  std::printf("target: native  source: %s\n\n", location.c_str());
  {
    std::FILE* f = std::fopen(location.c_str(), "rb");
    if (!f)
    {
      std::printf("  disc image not found at %s\n", location.c_str());
      std::printf("  convert one with:\n");
      std::printf("    dolphin-tool convert -i <disc>.rvz -o /tmp/ssa.iso -f iso\n");
      return 77;  // distinct from a test failure
    }
    std::fseek(f, 0, SEEK_END);
    disc_size = static_cast<u64>(std::ftell(f));
    std::fclose(f);
  }
  source = Harness::FileByteSource(location, stats);
#endif

  std::printf("  disc size %s\n", Human(disc_size).c_str());

  auto reader = std::make_unique<Harness::RangeBlobReader>(source, disc_size);
  auto volume = DiscIO::CreateDisc(std::move(reader));
  check(volume != nullptr, "DiscIO opened the disc");
  if (!volume)
  {
    std::printf("\nFAILED (%d failures)\n", ++g_failures);
    return 1;
  }

  std::printf("  game id   %s\n", volume->GetGameID().c_str());
  std::printf("  name      %s\n", volume->GetInternalName().c_str());
  std::printf("  revision  %u\n", volume->GetRevision().value_or(0));
  check(volume->GetGameID() == "SSPE52", "game id is SSPE52 (Skylanders, NTSC-U)");
  check(volume->GetInternalName().find("Skylanders") != std::string::npos,
        "internal name says Skylanders");
  check(volume->GetVolumeType() == DiscIO::Platform::WiiDisc, "disc is a Wii disc");

  // Wii discs are partitioned and encrypted. Reaching the filesystem means the
  // partition table parsed and the AES path worked -- neither of which the
  // synthetic GameCube image exercised at all.
  const auto partitions = volume->GetPartitions();
  std::printf("  partitions %zu\n", partitions.size());
  check(!partitions.empty(), "partition table parsed");

  // GetGamePartition returns a Partition by value, not an optional --
  // PARTITION_NONE stands in for "none" rather than nullopt.
  const DiscIO::Partition game_partition = volume->GetGamePartition();
  check(game_partition != DiscIO::PARTITION_NONE, "game partition identified");

  {
    const DiscIO::FileSystem* fs = volume->GetFileSystem(game_partition);
    check(fs != nullptr, "filesystem opened inside the encrypted partition");
    if (fs)
    {
      // Walk the root directory. This is the FST, parsed out of the
      // encrypted partition.
      const DiscIO::FileInfo& root = fs->GetRoot();
      u32 entries = 0;
      std::printf("  root entries:");
      for (const DiscIO::FileInfo& child : root)
      {
        if (entries < 6)
          std::printf(" %s%s", child.GetName().c_str(), child.IsDirectory() ? "/" : "");
        ++entries;
      }
      std::printf("  (%u total)\n", entries);
      check(entries > 0, "filesystem root lists entries");
    }
  }

  // The executable is NOT a file in the FST on Wii -- it lives at a fixed
  // offset in the partition, referenced by the partition header. Looking for
  // "main.dol" in the filesystem finds nothing, which is correct behaviour and
  // not a missing file.
  const auto dol_offset = DiscIO::GetBootDOLOffset(*volume, game_partition);
  check(dol_offset.has_value(), "boot DOL offset found in the partition header");
  if (dol_offset)
  {
    const auto dol_size = DiscIO::GetBootDOLSize(*volume, game_partition, *dol_offset);
    check(dol_size.has_value() && *dol_size > 0, "boot DOL has a size");
    std::printf("  boot DOL  offset=0x%llx size=%s\n",
                static_cast<unsigned long long>(*dol_offset),
                Human(dol_size.value_or(0)).c_str());

    // Read its header. A DOL starts with 7 text-section file offsets; the
    // first is 0x100 for essentially every GameCube/Wii executable.
    std::vector<u8> head(0x20);
    const bool ok = volume->Read(*dol_offset, head.size(), head.data(), game_partition);
    check(ok, "read the boot DOL header through the ranged source");
    if (ok)
    {
      const u32 first_section = (head[0] << 24) | (head[1] << 16) | (head[2] << 8) | head[3];
      std::printf("  DOL text0 file offset=0x%x\n", first_section);
      check(first_section == 0x100, "DOL header looks like a real executable");
    }
  }

  std::printf("\n  fetched %s in %llu requests (%.4f%% of the disc)\n", Human(stats->bytes).c_str(),
              static_cast<unsigned long long>(stats->requests),
              100.0 * static_cast<double>(stats->bytes) / static_cast<double>(disc_size));
  check(stats->bytes < disc_size / 100, "read under 1% of the disc");

  std::printf("\n%s (%d failures)\n", g_failures ? "FAILED" : "PASSED", g_failures);
  return g_failures ? 1 : 0;
}
