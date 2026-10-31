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

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <list>
#include <memory>
#include <mutex>
#include <string>
#include <functional>
#include <thread>
#include <unordered_map>
#include <unordered_set>
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
// the statistics have to outlive any one copy. Atomic throughout: the HTTP
// source's read-ahead thread counts its fetches beside the reading thread's,
// and --frame-times reads the waits from the CPU thread while DiscIO's reader
// may be another.
struct SourceStats
{
  std::atomic<u64> requests{0};  // fetches (or file reads), by any thread
  std::atomic<u64> bytes{0};
  std::atomic<u64> ahead{0};   // of the requests, chunks fetched before any read asked
  std::atomic<u64> stalls{0};  // reads that waited for a fetch
  // Time the game's reads waited for the transport, in microseconds. The HTTP
  // source's synchronous fetches are what stall the thread that reads.
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
//
// `chunk`, when non-zero, is the unit the source fetches and keeps: a read is
// served from whole chunks aligned to that size, held in a cache of about
// 32 MB, most recently used first. The game reads the disc in pieces of
// ~31 KB on average (a 400-second walk: 3,914 requests for 121 MB), each a
// round trip the emulator waits out; from a server across the internet that
// is 40-80 ms a piece and thousands of them at every load, where the local
// server answered in a fraction of a millisecond. One request a megabyte
// turns that into dozens. Zero keeps one request per read.
//
// `ahead`, when non-zero, is how many chunks past a read's last one a helper
// thread fetches before the game asks for them. A chunk still costs a round
// trip -- an object store answers a range in 200-500 ms whatever its size --
// and the game streams its level and its music in order, so without this it
// crosses into a chunk nobody has every ten or twenty seconds and the CPU
// thread waits out that round trip: a half-second stall, the lag spike the
// player saw as 50% on the readout. The helper waits instead, on its own
// worker, where nobody notices.
inline ByteSource HttpByteSource(const std::string& url, std::shared_ptr<SourceStats> stats,
                                 u64 chunk = 0, unsigned ahead = 0)
{
  // One fetch of [offset, offset + size). `exact` demands the whole range
  // back (a read the game asked for); a chunk may end short at the disc's
  // end, which is the last chunk's size, not a failure.
  auto fetch_range = [url, stats](u64 offset, u64 size, bool exact) -> std::vector<u8> {
    emscripten_fetch_attr_t attr;
    emscripten_fetch_attr_init(&attr);
    std::strcpy(attr.requestMethod, "GET");
    attr.attributes = EMSCRIPTEN_FETCH_LOAD_TO_MEMORY | EMSCRIPTEN_FETCH_SYNCHRONOUS |
                      EMSCRIPTEN_FETCH_REPLACE;

    const std::string range =
        "bytes=" + std::to_string(offset) + "-" + std::to_string(offset + size - 1);
    const char* headers[] = {"Range", range.c_str(), nullptr};
    attr.requestHeaders = headers;

    emscripten_fetch_t* fetch = emscripten_fetch(&attr, url.c_str());
    std::vector<u8> out;
    // 206 Partial Content is the expected answer. 200 means the server ignored
    // the Range header and sent everything, which is a server bug rather than
    // a transport failure -- treated as failure so it cannot pass unnoticed.
    const bool whole = fetch && fetch->status == 206 &&
                       (exact ? fetch->numBytes == size
                              : (fetch->numBytes > 0 && fetch->numBytes <= size));
    if (whole)
    {
      out.assign(fetch->data, fetch->data + fetch->numBytes);
      stats->requests.fetch_add(1, std::memory_order_relaxed);
      stats->bytes.fetch_add(out.size(), std::memory_order_relaxed);
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
  if (!chunk)
  {
    return [fetch_range, stats](u64 offset, u64 size) -> std::vector<u8> {
      const double t0 = emscripten_get_now();
      std::vector<u8> out = fetch_range(offset, size, true);
      stats->us.fetch_add(static_cast<u64>((emscripten_get_now() - t0) * 1000.0),
                          std::memory_order_relaxed);
      stats->stalls.fetch_add(1, std::memory_order_relaxed);
      if (!out.empty() && stats->on_read)
        stats->on_read(offset, out.size());
      return out;
    };
  }

  // The chunk cache, shared by every copy of the source (DiscIO clones its
  // readers; several threads may read) and by the helper, so one lock around
  // it. Nobody holds the lock across a fetch: a chunk being fetched is in
  // `fetching`, and a reader that needs it waits for that fetch to end
  // instead of starting another.
  struct Cache
  {
    std::mutex lock;
    std::condition_variable changed;  // a fetch ended, or the helper has work
    std::list<u64> order;             // most recently used first
    std::unordered_map<u64, std::pair<std::vector<u8>, std::list<u64>::iterator>> chunks;
    std::unordered_set<u64> fetching;
    std::deque<u64> wanted;  // the helper's queue, nearest first
  };
  auto cache = std::make_shared<Cache>();
  const size_t max_chunks = static_cast<size_t>(std::max<u64>(1, (u64{32} << 20) / chunk));

  // Fetches one chunk into the cache. Entered with the lock held, which it
  // gives up for the fetch; on return the lock is held again and the chunk
  // is in the cache, unless the fetch failed.
  auto fetch_chunk = [fetch_range, cache, chunk, max_chunks](
                         u64 index, std::unique_lock<std::mutex>& guard) {
    cache->fetching.insert(index);
    guard.unlock();
    std::vector<u8> data = fetch_range(index * chunk, chunk, false);
    guard.lock();
    cache->fetching.erase(index);
    if (!data.empty())
    {
      cache->order.push_front(index);
      cache->chunks[index] = std::make_pair(std::move(data), cache->order.begin());
      while (cache->chunks.size() > max_chunks)
      {
        cache->chunks.erase(cache->order.back());
        cache->order.pop_back();
      }
    }
    cache->changed.notify_all();
  };

  if (ahead)
  {
    // The helper: fetches what the readers queue, for as long as the program
    // runs. Detached, holding its own share of the cache.
    std::thread([fetch_chunk, cache, stats] {
      std::unique_lock<std::mutex> guard(cache->lock);
      for (;;)
      {
        cache->changed.wait(guard, [&] { return !cache->wanted.empty(); });
        const u64 index = cache->wanted.front();
        cache->wanted.pop_front();
        if (cache->chunks.count(index) || cache->fetching.count(index))
          continue;
        stats->ahead.fetch_add(1, std::memory_order_relaxed);
        fetch_chunk(index, guard);
      }
    }).detach();
  }

  return [fetch_chunk, stats, cache, chunk, ahead](u64 offset, u64 size) -> std::vector<u8> {
    std::vector<u8> out;
    out.reserve(static_cast<size_t>(size));
    const double t0 = emscripten_get_now();
    bool waited = false;
    std::unique_lock<std::mutex> guard(cache->lock);
    u64 pos = offset, last = offset / chunk;
    while (pos < offset + size)
    {
      const u64 index = pos / chunk;
      auto it = cache->chunks.find(index);
      if (it == cache->chunks.end())
      {
        waited = true;
        if (cache->fetching.count(index))
          cache->changed.wait(guard, [&] { return !cache->fetching.count(index); });
        else
          fetch_chunk(index, guard);
        it = cache->chunks.find(index);
        if (it == cache->chunks.end())
          break;  // the fetch failed
      }
      else if (it->second.second != cache->order.begin())
      {
        cache->order.erase(it->second.second);
        cache->order.push_front(index);
        it->second.second = cache->order.begin();
      }
      const std::vector<u8>& data = it->second.first;
      const u64 in = pos - index * chunk;
      if (in >= data.size())
        break;  // past the disc's end
      const u64 n = std::min<u64>(offset + size - pos, data.size() - in);
      out.insert(out.end(), data.begin() + static_cast<size_t>(in),
                 data.begin() + static_cast<size_t>(in + n));
      pos += n;
      last = index;
      if (data.size() < chunk && in + n == data.size())
        break;  // a short last chunk: the disc ended inside it
    }
    if (ahead)
    {
      // The chunks after this read's last one, replacing whatever was queued:
      // the newest read is the best guess of where the game goes next.
      cache->wanted.clear();
      for (unsigned k = 1; k <= ahead; ++k)
        if (!cache->chunks.count(last + k) && !cache->fetching.count(last + k))
          cache->wanted.push_back(last + k);
      if (!cache->wanted.empty())
        cache->changed.notify_all();
    }
    guard.unlock();
    if (waited)
    {
      stats->stalls.fetch_add(1, std::memory_order_relaxed);
      stats->us.fetch_add(static_cast<u64>((emscripten_get_now() - t0) * 1000.0),
                          std::memory_order_relaxed);
    }
    if (stats->on_read)
      stats->on_read(offset, out.size());
    return out;
  };
}
#endif
}  // namespace Harness
