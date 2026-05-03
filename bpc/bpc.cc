#include "mem/cache/compressors/bpc.hh"

#include <cassert>
#include <cstring>

#include "debug/CacheComp.hh"
#include "params/BPC.hh"

namespace gem5::compression {

BPC::BPC(const BPCParams& p)
    : Base(p), maxCompressionRatio(p.max_compression_ratio)
{
}

uint32_t
BPC::extractPlane(const uint32_t* words, int bit) const
{
    uint32_t plane = 0;
    for (int w = 0; w < WORDS_PER_LINE; w++) {
        if ((words[w] >> bit) & 1u)
            plane |= (1u << w);
    }
    return plane;
}

void
BPC::reconstructWords(const std::vector<uint32_t>& planes,
                      uint32_t* words) const
{
    assert((int)planes.size() == BITS_PER_WORD);
    memset(words, 0, WORDS_PER_LINE * sizeof(uint32_t));
    for (int bit = 0; bit < BITS_PER_WORD; bit++) {
        for (int w = 0; w < WORDS_PER_LINE; w++) {
            if ((planes[bit] >> w) & 1u)
                words[w] |= (1u << bit);
        }
    }
}

std::unique_ptr<Base::CompressionData>
BPC::compress(const std::vector<Base::Chunk>& chunks,
              Cycles& comp_lat,
              Cycles& decomp_lat)
{
    // The total size of the chunks must match the block size
    if (chunks.size() * sizeof(Base::Chunk) != (std::size_t)blkSize) {
        // If there's a mismatch, it might be because the compressor is 
        // receiving more chunks than expected. We'll truncate or warn.
        // For now, let's be strict but correct about the byte count.
        if (chunks.size() * sizeof(Base::Chunk) < (std::size_t)blkSize) {
            panic("BPC::compress: not enough data! chunks=%lu, chunk_size=%lu, blkSize=%lu\n",
                  (unsigned long)chunks.size(), (unsigned long)sizeof(Base::Chunk),
                  (unsigned long)blkSize);
        }
    }

    uint32_t words[WORDS_PER_LINE];
    // Copy exactly blkSize bytes from chunks.data()
    memcpy(words, chunks.data(), blkSize);

    auto comp_data = std::make_unique<BPCData>();

    uint32_t prev_plane = 0;
    int compressed_bits = 0;
    // 3-bit tag per plane
    constexpr int TAG_BITS = 3;
    // A plane has WORDS_PER_LINE valid bits, not 32 — extractPlane only
    // populates bits 0..WORDS_PER_LINE-1.
    constexpr uint32_t PLANE_MASK =
        (WORDS_PER_LINE == 32) ? 0xFFFFFFFFu
                               : ((1u << WORDS_PER_LINE) - 1u);

    for (int bit = 0; bit < BITS_PER_WORD; bit++) {
        uint32_t plane = extractPlane(words, bit);
        PlaneEncoding tag;

        if (plane == 0x00000000u) {
            tag = ALL_ZEROS;
            compressed_bits += TAG_BITS;
        } else if (plane == PLANE_MASK) {
            tag = ALL_ONES;
            compressed_bits += TAG_BITS;
        } else if (plane == prev_plane) {
            tag = SAME_AS_PREV;
            compressed_bits += TAG_BITS;
        } else if (plane == ((~prev_plane) & PLANE_MASK)) {
            tag = COMP_OF_PREV;
            compressed_bits += TAG_BITS;
        } else {
            tag = UNCOMPRESSED;
            comp_data->rawPlanes.push_back(plane);
            compressed_bits += TAG_BITS + WORDS_PER_LINE;
        }

        comp_data->tags.push_back(tag);
        prev_plane = plane;
    }

    if (maxCompressionRatio > 0) {
        const int floor_bits = (blkSize * 8) / maxCompressionRatio;
        if (compressed_bits < floor_bits)
            compressed_bits = floor_bits;
    }

    // Pad reported size to nearest power-of-2 byte boundary in [8B, blkSize].
    // This aligns with the buddy-allocator granularities in DecoupledDataStore.
    {
        const int rawBytes = (compressed_bits + 7) / 8;
        int paddedBytes = 8;
        while (paddedBytes < rawBytes && paddedBytes < blkSize)
            paddedBytes <<= 1;
        if (paddedBytes > blkSize) paddedBytes = blkSize;
        compressed_bits = paddedBytes * 8;
    }

    comp_data->setSizeBits(compressed_bits);

    // Simple latency model — 1 cycle each
    comp_lat   = Cycles(1);
    decomp_lat = Cycles(1);

    DPRINTF(CacheComp, "BPC: compressed %d bits (uncompressed %d bits)\n",
            compressed_bits, WORDS_PER_LINE * BITS_PER_WORD);

    return comp_data;
}

void
BPC::decompress(const CompressionData* comp_data, uint64_t* cache_line)
{
    const BPCData* bpc_data = static_cast<const BPCData*>(comp_data);
    assert((int)bpc_data->tags.size() == BITS_PER_WORD);

    std::vector<uint32_t> planes(BITS_PER_WORD);
    uint32_t prev_plane = 0;
    int raw_idx = 0;
    constexpr uint32_t PLANE_MASK =
        (WORDS_PER_LINE == 32) ? 0xFFFFFFFFu
                               : ((1u << WORDS_PER_LINE) - 1u);

    for (int bit = 0; bit < BITS_PER_WORD; bit++) {
        switch (bpc_data->tags[bit]) {
          case ALL_ZEROS:
            planes[bit] = 0x00000000u;
            break;
          case ALL_ONES:
            planes[bit] = PLANE_MASK;
            break;
          case SAME_AS_PREV:
            planes[bit] = prev_plane;
            break;
          case COMP_OF_PREV:
            planes[bit] = (~prev_plane) & PLANE_MASK;
            break;
          case UNCOMPRESSED:
            planes[bit] = bpc_data->rawPlanes[raw_idx++];
            break;
          default:
            panic("BPC: unknown plane encoding tag %d", bpc_data->tags[bit]);
        }
        prev_plane = planes[bit];
    }

    uint32_t words[WORDS_PER_LINE];
    reconstructWords(planes, words);
    memcpy(cache_line, words, WORDS_PER_LINE * sizeof(uint32_t));
}

} // namespace gem5::compression
