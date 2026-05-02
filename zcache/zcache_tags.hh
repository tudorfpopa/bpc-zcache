/*
 * ZCacheTags — tag store implementing the Z-Cache architecture.
 *
 * Overview
 * --------
 * On a hit  : W parallel H3-hashed lookups, identical to skewed-associative.
 * On a miss : BFS walk of up to L levels expands the candidate set from W to R
 *             blocks.  The globally-oldest candidate (ZCacheRP) is evicted.
 *             A cuckoo chain is then built so that the incoming block can be
 *             placed at a Level-1 position (directly reachable by H3 hash).
 *             The actual cuckoo moves are executed in insertBlock(), after the
 *             victim has been written back and invalidated by the cache ctrl.
 *
 * Tag encoding
 * ------------
 * Tags store the block-aligned address (addr & ~blkMask) rather than the
 * conventional (addr >> tagShift).  This is idempotent under extractTag(),
 * which is required for CacheBlk::operator=(CacheBlk&&) (used by cuckoo
 * moves) to preserve tags correctly.
 */

#ifndef __MEM_CACHE_TAGS_ZCACHE_TAGS_HH__
#define __MEM_CACHE_TAGS_ZCACHE_TAGS_HH__

#include <cstdint>
#include <vector>

#include "base/statistics.hh"
#include "mem/cache/replacement_policies/zcache_rp.hh"
#include "mem/cache/tags/base_set_assoc.hh"
#include "mem/cache/tags/indexing_policies/zcache_index.hh"
#include "params/ZCacheTags.hh"

namespace gem5
{

class ZCacheTags : public BaseSetAssoc
{
  private:
    // ZCache-specific indexing (H3 hash per way)
    ZCacheIndexingPolicy* const zcacheIP;

    // Bucketed-LRU replacement policy (cast of the inherited replacementPolicy)
    replacement_policy::ZCacheRP* const zcacheRP;

    // BFS walk parameters
    const uint32_t walkLevels;    // L — max BFS depth
    const uint32_t numCandidates; // R — max replacement candidates
    const uint32_t numWays;       // W = assoc
    const uint32_t numSets_;      // total sets = numBlocks / numWays

    // Pending cuckoo chain set by findVictim(), executed by insertBlock().
    // chain[0] = evicted victim (invalidated by Cache.cc before insertBlock),
    // chain.back() = root (Level-1 slot, returned as insertion destination).
    std::vector<CacheBlk*> pendingSwapChain;

    // Internal helpers
    CacheBlk* getBlkAt(uint32_t set, uint32_t way)
    { return &blks[set * numWays + way]; }
    const CacheBlk* getBlkAt(uint32_t set, uint32_t way) const
    { return &blks[set * numWays + way]; }

    // Move src's metadata + data bytes to dst; dst must be invalid.
    // Does NOT call BaseSetAssoc::invalidate (avoids double-decrement of
    // tagsInUse) — replacement data is handled inline.
    void doMoveBlock(CacheBlk* src, CacheBlk* dst);

    // BFS walk info node
    struct ZWalkInfo
    {
        uint32_t set;
        uint32_t way;
        int32_t  parentIdx; // -1 for Level-1 seeds
    };

    // ZCache-specific statistics
    struct ZCacheTagStats : public statistics::Group
    {
        ZCacheTagStats(ZCacheTags *parent);

        statistics::Scalar  statWalkDepthTotal;
        statistics::Scalar  statMissCount;
        statistics::Scalar  statCandidatesTotal;
        statistics::Scalar  statRelocationsTotal;
        statistics::Formula statWalkDepthAverage;
        statistics::Formula statCandidatesEvaluatedAverage;
    } zcacheStats;

  public:
    typedef ZCacheTagsParams Params;

    ZCacheTags(const Params &p);
    ~ZCacheTags() = default;

    // Override tagsInit to re-register tag extractors for ZCache's tag scheme.
    void tagsInit() override;

    // H3-based cache lookup (W parallel hashed probes).
    CacheBlk* findBlock(const CacheBlk::KeyType &key) const override;

    // Access block and update replacement data.
    CacheBlk* accessBlock(const PacketPtr pkt, Cycles &lat) override;

    // BFS walk + LRU victim selection.  Sets pendingSwapChain for insertBlock.
    CacheBlk* findVictim(const CacheBlk::KeyType &key,
                         const std::size_t size,
                         std::vector<CacheBlk*>& evict_blks,
                         const uint64_t partition_id=0) override;

    // Execute pending cuckoo chain, then insert new block.
    void insertBlock(const PacketPtr pkt, CacheBlk* blk) override;

    // Reconstruct block address from tag (tag IS the block-aligned address).
    Addr regenerateBlkAddr(const CacheBlk* blk) const override
    { return blk->getTag(); }

    // Look up by physical (set, way) using the ZCache block layout.
    ReplaceableEntry* findBlockBySetAndWay(int set, int way) const override
    { return const_cast<CacheBlk*>(getBlkAt(set, way)); }
};

} // namespace gem5

#endif // __MEM_CACHE_TAGS_ZCACHE_TAGS_HH__
