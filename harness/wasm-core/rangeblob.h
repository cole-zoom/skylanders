// A DiscIO::BlobReader backed by a pluggable byte-range source.
//
// docs/PLAN.md Bet 05 says the disc is streamed rather than downloaded: the
// browser holds no 3.8 GB file, it asks for the ranges the game touches and
// caches them. Dolphin already has the seam for that -- DiscIO::BlobReader is
// abstract, and DiscIO::CreateDisc accepts one -- so the question is whether
// the rest of DiscIO is happy being fed by something that is not a file.
//
// This separates the two halves of that question. RangeBlobReader implements
// the BlobReader contract over a std::function that returns bytes for a given
// offset and length. Where those bytes come from -- a file, an HTTP range
// request, an OPFS cache -- is the source's problem, not DiscIO's.
//
// Deliberately NOT an RVZ reader. RVZ decoding lives in WIARVZFileReader,
// which takes a File::IOFile and cannot be pointed at an arbitrary source, so
// serving RVZ over HTTP means reimplementing its chunk index on top of this.
// That is a separate piece of work; this is the layer underneath it.

#pragma once

#include <algorithm>
#include <cstring>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "Common/CommonTypes.h"
#include "DiscIO/Blob.h"

namespace Harness
{
// Returns `size` bytes at `offset`, or fewer at end of data. The reader treats
// a short read as a failure, which is what DiscIO expects.
using ByteSource = std::function<std::vector<u8>(u64 offset, u64 size)>;

class RangeBlobReader final : public DiscIO::BlobReader
{
public:
  RangeBlobReader(ByteSource source, u64 total_size)
      : m_source(std::move(source)), m_size(total_size)
  {
  }

  // PLAIN rather than a new blob type: this presents an already-decoded disc
  // image, which is exactly what PLAIN means to the rest of DiscIO. Inventing
  // a type would require teaching every switch over BlobType about it.
  DiscIO::BlobType GetBlobType() const override { return DiscIO::BlobType::PLAIN; }

  // DiscIO copies readers to read from several threads. The source is shared
  // deliberately: an HTTP or OPFS backend wants one cache, not one per copy.
  std::unique_ptr<BlobReader> CopyReader() const override
  {
    return std::make_unique<RangeBlobReader>(m_source, m_size);
  }

  u64 GetRawSize() const override { return m_size; }
  u64 GetDataSize() const override { return m_size; }
  DiscIO::DataSizeType GetDataSizeType() const override { return DiscIO::DataSizeType::Accurate; }

  // Zero means "not block based". A real ranged source will want to report its
  // fetch granularity here so DiscIO aligns reads to it -- for RVZ that is the
  // 128 KB chunk size the capture header reports.
  u64 GetBlockSize() const override { return 0; }
  bool HasFastRandomAccessInBlock() const override { return true; }

  std::string GetCompressionMethod() const override { return {}; }
  std::optional<int> GetCompressionLevel() const override { return std::nullopt; }

  bool Read(u64 offset, u64 size, u8* out_ptr) override
  {
    if (offset + size > m_size)
      return false;

    const std::vector<u8> bytes = m_source(offset, size);
    if (bytes.size() != size)
      return false;

    std::memcpy(out_ptr, bytes.data(), size);
    m_reads++;
    m_bytes_read += size;
    return true;
  }

  // Not part of the contract; used by the test to show that DiscIO reads a
  // small fraction of a disc, which is the premise of streaming it.
  u64 ReadCount() const { return m_reads; }
  u64 BytesRead() const { return m_bytes_read; }

private:
  ByteSource m_source;
  u64 m_size;
  u64 m_reads = 0;
  u64 m_bytes_read = 0;
};
}  // namespace Harness
