#include "mem/cache/tags/zcache_glue.hh"

#include <algorithm>
#include <cassert>
#include <cstring>
#include <vector>

#include "base/logging.hh"
#include "mem/cache/cache_blk.hh"
#include "mem/cache/tags/decoupled_data_store.hh"
#include "mem/cache/tags/frag_reservoir.hh"
#include "mem/cache/tags/zcache_tags_new.hh"

namespace gem5 {

// ---------------------------------------------------------------------------
// Stats
// ---------------------------------------------------------------------------

ZCacheGlue::GlueStats::GlueStats(ZCacheGlue *parent)
    : statistics::Group(parent),
      ADD_STAT(statFragEvictions,
               statistics::units::Count::get(),
               "Total fragmentation evictions via FragReservoir"),
      ADD_STAT(statFillsWithData,
               statistics::units::Count::get(),
               "Total fills carrying data that were processed"),
      ADD_STAT(statTotalPaddedBytes,
               statistics::units::Count::get(),
               "Sum of padded compressed sizes across all fills"),
      ADD_STAT(statTotalRawBytes,
               statistics::units::Count::get(),
               "Sum of raw (unpadded) compressed sizes across all fills"),
      ADD_STAT(statReservoirRequests,
               statistics::units::Count::get(),
               "Total getVictim() calls to FragReservoir"),
      ADD_STAT(statReservoirHits,
               statistics::units::Count::get(),
               "Times FragReservoir returned a valid candidate"),
      ADD_STAT(statAvgInternalFragmentation,
               statistics::units::Ratio::get(),
               "Average internal fragmentation per fill "
               "(paddedSize - rawSize) / paddedSize"),
      ADD_STAT(statReservoirHitRate,
               statistics::units::Ratio::get(),
               "Fraction of reservoir lookups that found a valid candidate")
{
    statAvgInternalFragmentation =
        (statTotalPaddedBytes - statTotalRawBytes) / statTotalPaddedBytes;
    statReservoirHitRate = statReservoirHits / statReservoirRequests;
}

// ---------------------------------------------------------------------------
// Constructor
// ---------------------------------------------------------------------------

ZCacheGlue::ZCacheGlue(const ZCacheGlueParams &p)
    : SimObject(p),
      zcacheTags(p.zcache_tags),
      dataStore(p.data_store),
      reservoir(p.reservoir),
      compressor(p.compressor),
      blkSize(p.block_size),
      glueStats(this)
{}

void ZCacheGlue::startup()
{
    // Wire the circular reference so ZCacheTagsNew calls us on fills.
    zcacheTags->setGlue(this);
    // Also let FragReservoir query tag metadata.
    reservoir->setTagsRef(zcacheTags);
}

// ---------------------------------------------------------------------------
// computePaddedSize
// ---------------------------------------------------------------------------

uint32_t ZCacheGlue::computePaddedSize(uint32_t compressedBits)
{
    // BPC already pads to power-of-2 bytes in [8B, blkSize]; just convert.
    return (compressedBits + 7) / 8;
}

// ---------------------------------------------------------------------------
// handleFill — compress → allocate → fragment-evict loop → store
// ---------------------------------------------------------------------------

void ZCacheGlue::handleFill(const PacketPtr pkt, CacheBlk *blk)
{
    assert(pkt->hasData());

    // ------------------------------------------------------------------
    // 1. Compress the incoming cache line with BPC.
    // ------------------------------------------------------------------
    const uint8_t *rawData = pkt->getConstPtr<uint8_t>();

    // Build the chunk vector BPC::compress expects.
    // Base::Chunk is typedef uint64_t (protected), so we use uint64_t directly.
    const size_t chunkSize = sizeof(uint64_t);
    std::vector<uint64_t> chunks(blkSize / chunkSize);
    memcpy(chunks.data(), rawData, blkSize);

    Cycles compLat, decompLat;
    auto compData = compressor->compress(chunks, compLat, decompLat);

    const uint32_t paddedSize = computePaddedSize(compData->getSizeBits());
    const uint32_t rawBytes   = (compData->getSizeBits() + 7) / 8;

    glueStats.statFillsWithData++;
    glueStats.statTotalPaddedBytes += paddedSize;
    glueStats.statTotalRawBytes    += rawBytes;

    // ------------------------------------------------------------------
    // 2. Initial allocation attempt.
    // ------------------------------------------------------------------
    uint32_t ptr = dataStore->allocate(paddedSize);

    // ------------------------------------------------------------------
    // 3. Fragmentation-eviction loop — runs only if allocation failed.
    // ------------------------------------------------------------------
    while (ptr == DecoupledDataStore::ALLOC_FAIL) {
        glueStats.statReservoirRequests++;

        ReplaceableEntry *victimEntry = reservoir->getVictim(paddedSize);
        if (!victimEntry) {
            // Reservoir empty; skip storing compressed copy this fill.
            return;
        }

        glueStats.statReservoirHits++;
        glueStats.statFragEvictions++;

        CacheBlk *victim = static_cast<CacheBlk *>(victimEntry);

        // Retrieve and release the victim's data-store slot.
        uint32_t victimPtr  = zcacheTags->getBlockDataPtr(victim);
        uint32_t victimSize = zcacheTags->getBlockAllocSize(victim);

        zcacheTags->forceInvalidate(victim); // clears tag + compressed metadata

        if (victimPtr != DecoupledDataStore::ALLOC_FAIL && victimSize > 0) {
            dataStore->deallocate(victimPtr, victimSize);
        }

        ptr = dataStore->allocate(paddedSize);
    }

    // ------------------------------------------------------------------
    // 4. Store compressed data and record pointer in ZCacheTagsNew.
    // ------------------------------------------------------------------
    // Write the first paddedSize bytes of the uncompressed line as a proxy
    // for the compressed payload (the full decompressed data lives in blk->data
    // and is served through the normal gem5 data path).
    dataStore->write(ptr, rawData, paddedSize);
    zcacheTags->setBlockDataPtr(blk, ptr, paddedSize);
}

} // namespace gem5
