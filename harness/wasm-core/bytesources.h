// Byte sources for RangeBlobReader.
//
// RangeBlobReader deliberately does not know where its bytes come from. These
// are the two that matter for this project:
//
//   FileByteSource  reads from a local file. Native uses it directly; it is
//                   also the reference a ranged transport is checked against.
//
//   HttpByteSource  issues HTTP Range requests. This is what a browser does,
//                   and the reason docs/PLAN.md Bet 05 works at all -- the
//                   3.8 GB disc never has to be resident.
//
// Both report how much they fetched, because the premise of streaming is that
// a game touches a small fraction of its disc and the only way to know is to
// count.

#pragma once

#include <atomic>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <functional>
#include <vector>

#include "Common/CommonTypes.h"

#include "rangeblob.h"

#if defined(__EMSCRIPTEN__)
#include <emscripten/emscripten.h>
#include <emscripten/fetch.h>
#endif

namespace Harness
{
// Shared counters. A ByteSource is copied into every reader DiscIO clones, so
// the statistics have to outlive any one copy.
struct SourceStats
{
  u64 requests = 0;
  u64 bytes = 0;
  // Time spent in reads, in microseconds; the HTTP source's synchronous
  // fetches are what can stall the thread that reads. Atomic because
  // --frame-times reads it from the CPU thread while DiscIO's reader may
  // be another.
  std::atomic<u64> us{0};
  // Called with every read's raw disc offset and length, when set. The
  // harness uses it to name the file a read lands in, which is the only
  // cheap signal of what the game is doing at a given moment.
  std::function<void(u64, u64)> on_read;
};

inline ByteSource FileByteSource(const std::string& path, std::shared_ptr<SourceStats> stats)
{
  // One handle shared by every copy of the source. DiscIO clones readers to
  // read from several threads; opening per request would be correct but slow,
  // and opening per copy would multiply file descriptors.
  auto fp = std::shared_ptr<std::FILE>(std::fopen(path.c_str(), "rb"),
                                       [](std::FILE* f) { if (f) std::fclose(f); });

  return [fp, stats](u64 offset, u64 size) -> std::vector<u8> {
    if (!fp)
      return {};
    if (std::fseek(fp.get(), static_cast<long>(offset), SEEK_SET) != 0)
      return {};
    std::vector<u8> out(size);
    const size_t got = std::fread(out.data(), 1, size, fp.get());
    out.resize(got);
    stats->requests++;
    stats->bytes += got;
    if (stats->on_read)
      stats->on_read(offset, got);
    return out;
  };
}

#if defined(__EMSCRIPTEN__)
// Synchronous ranged fetch. Legal because this runs on a worker, not the main
// thread -- the same constraint Spike C established for Atomics.wait, and the
// reason PROXY_TO_PTHREAD is on.
//
// Every request carries a Range header, so the server must honour it. A server
// that ignores Range returns the whole file for each request, which still
// "works" and silently defeats the entire point; the caller checks the
// returned length against what it asked for.
inline ByteSource HttpByteSource(const std::string& url, std::shared_ptr<SourceStats> stats)
{
  return [url, stats](u64 offset, u64 size) -> std::vector<u8> {
    emscripten_fetch_attr_t attr;
    emscripten_fetch_attr_init(&attr);
    std::strcpy(attr.requestMethod, "GET");
    attr.attributes = EMSCRIPTEN_FETCH_LOAD_TO_MEMORY | EMSCRIPTEN_FETCH_SYNCHRONOUS |
                      EMSCRIPTEN_FETCH_REPLACE;

    const std::string range =
        "bytes=" + std::to_string(offset) + "-" + std::to_string(offset + size - 1);
    const char* headers[] = {"Range", range.c_str(), nullptr};
    attr.requestHeaders = headers;

    const double t0 = emscripten_get_now();
    emscripten_fetch_t* fetch = emscripten_fetch(&attr, url.c_str());
    stats->us.fetch_add(static_cast<u64>((emscripten_get_now() - t0) * 1000.0),
                        std::memory_order_relaxed);
    std::vector<u8> out;
    // 206 Partial Content is the expected answer. 200 means the server ignored
    // the Range header and sent everything, which is a server bug rather than
    // a transport failure -- treated as failure so it cannot pass unnoticed.
    if (fetch && fetch->status == 206 && fetch->numBytes == size)
    {
      out.assign(fetch->data, fetch->data + fetch->numBytes);
      stats->requests++;
      stats->bytes += out.size();
      if (stats->on_read)
        stats->on_read(offset, out.size());
    }
    else if (fetch)
    {
      std::printf("  http: status=%d got=%llu want=%llu\n", fetch->status,
                  static_cast<unsigned long long>(fetch->numBytes),
                  static_cast<unsigned long long>(size));
    }
    if (fetch)
      emscripten_fetch_close(fetch);
    return out;
  };
}
#endif
}  // namespace Harness
