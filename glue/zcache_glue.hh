#ifndef __MEM_CACHE_TAGS_ZCACHE_GLUE_HH__
#define __MEM_CACHE_TAGS_ZCACHE_GLUE_HH__

#include <cstdint>
#include <memory>
#include <vector>

#include "base/statistics.hh"
#include "mem/cache/compressors/bpc.hh"
#include "mem/packet.hh"
#include "params/ZCacheGlue.hh"
#include "sim/sim_object.hh"

namespace gem5 {

class ZCacheTagsNew;
class DecoupledDataStore;
class FragReservoir;
class CacheBlk;

/*
 * ZCacheGlue — coordinates BPC compression, buddy-allocated data storage,
 * and fragmentation eviction for the Compressed Decoupled ZCache.
 *
 * handleFill() is called by ZCacheTagsNew::insertBlock() after each fill.
 * It implements the compress → allocate → (fragment-evict loop) → store flow
 * described in the design document.
 *
 * Stats exposed:
 *   totalFragEvictions        — times FragReservoir was invoked.
 *   avgInternalFragmentation  — mean (paddedSize - rawSize) / paddedSize.
 *   reservoirHitRate          — fraction of getVictim() calls that succeeded.
 */
class ZCacheGlue : public SimObject
{
  public:
    ZCacheGlue(const ZCacheGlueParams &p);
    ~ZCacheGlue() = default;

    // Called once at simulation startup to wire the circular reference.
    void startup() override;

    // Main fill handler — compress pkt data, allocate in data store,
    // evict via reservoir if fragmented, store pointer in blk's metadata.
    void handleFill(const PacketPtr pkt, CacheBlk *blk);

  private:
    ZCacheTagsNew       *zcacheTags;
    DecoupledDataStore  *dataStore;
    FragReservoir       *reservoir;
    compression::BPC    *compressor;

    const uint32_t blkSize; // cache line size in bytes

    // Compute padded size: round compressed bytes up to next power of 2, min 8B.
    static uint32_t computePaddedSize(uint32_t compressedBits);

    // Stats group
    struct GlueStats : public statistics::Group {
        GlueStats(ZCacheGlue *parent);

        statistics::Scalar statFragEvictions;
        statistics::Scalar statFillsWithData;
        statistics::Scalar statTotalPaddedBytes;
        statistics::Scalar statTotalRawBytes;
        statistics::Scalar statReservoirRequests;
        statistics::Scalar statReservoirHits;

        statistics::Formula statAvgInternalFragmentation;
        statistics::Formula statReservoirHitRate;
    } glueStats;
};

} // namespace gem5
#endif // __MEM_CACHE_TAGS_ZCACHE_GLUE_HH__
