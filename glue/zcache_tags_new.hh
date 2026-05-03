#ifndef __MEM_CACHE_TAGS_ZCACHE_TAGS_NEW_HH__
#define __MEM_CACHE_TAGS_ZCACHE_TAGS_NEW_HH__

#include <cstdint>
#include <vector>

#include "mem/cache/tags/zcache_tags.hh"
#include "params/ZCacheTagsNew.hh"

namespace gem5 {

class ZCacheGlue;

/*
 * ZCacheTagsNew — ZCacheTags extended for decoupled compressed data storage.
 *
 * Two modifications from ZCacheTags:
 *  1. Parallel arrays track per-block (dataPtr, allocSize) into the
 *     DecoupledDataStore so the glue can manage compressed capacity.
 *  2. forceInvalidate() bypasses the cuckoo walk and directly invalidates a
 *     block — required when the FragReservoir evicts a victim to recover space.
 *
 * The normal CacheBlk.data array is kept intact for gem5 simulation correctness
 * (data is still served uncompressed through the normal cache path).  The glue
 * layer models compressed capacity separately via the data store.
 */
class ZCacheTagsNew : public ZCacheTags
{
  public:
    typedef ZCacheTagsNewParams Params;

    ZCacheTagsNew(const Params &p);
    ~ZCacheTagsNew() = default;

    // --- Initialisation ---

    // Extends ZCacheTags::tagsInit() to size the parallel metadata arrays.
    void tagsInit() override;

    // --- Glue integration ---

    // Set by ZCacheGlue::init() after all objects are constructed.
    void setGlue(ZCacheGlue *g) { glue = g; }

    // Call ZCacheGlue::handleFill() with the incoming packet after inserting.
    void insertBlock(const PacketPtr pkt, CacheBlk *blk) override;

    // Standard invalidation path — called by BaseCache::handleEvictions for
    // normal LRU-style evictions.  Frees the block's pool slot before clearing
    // the tag; prevents the pool-slot leak that occurs when doMoveBlock
    // overwrites blkDataPtr[dst] without freeing the victim's existing slot.
    void invalidate(CacheBlk *blk) override;

    // Invalidate blk directly, without triggering a cuckoo walk.
    // Called by ZCacheGlue when the FragReservoir selects this block as victim.
    void forceInvalidate(CacheBlk *blk);

    // --- Per-block compressed metadata accessors ---

    uint32_t getBlockIndex(const CacheBlk *blk) const
    { return static_cast<uint32_t>(blk - &blks[0]); }

    void setBlockDataPtr(CacheBlk *blk, uint32_t ptr, uint32_t allocSz)
    {
        uint32_t i = getBlockIndex(blk);
        blkDataPtr[i]   = ptr;
        blkAllocSize[i] = allocSz;
    }

    uint32_t getBlockDataPtr(const CacheBlk *blk) const
    { return blkDataPtr[getBlockIndex(blk)]; }

    uint32_t getBlockAllocSize(const CacheBlk *blk) const
    { return blkAllocSize[getBlockIndex(blk)]; }

    Tick getBlockLastTouch(const CacheBlk *blk) const
    { return zcacheRP->getLastTouchTimestamp(blk->replacementData); }

    // --- Block enumeration for FragReservoir ---

    uint32_t getNumBlocks() const
    { return static_cast<uint32_t>(blks.size()); }

    CacheBlk *getBlockByIndex(uint32_t idx)
    { return (idx < blks.size()) ? &blks[idx] : nullptr; }

  protected:
    // Override to also migrate dataPtr/allocSize during cuckoo moves.
    void doMoveBlock(CacheBlk *src, CacheBlk *dst) override;

  private:
    ZCacheGlue *glue = nullptr;

    // Per-block compressed-data pointers (segment index into DecoupledDataStore).
    // Initialised to UINT32_MAX (ALLOC_FAIL) — indicates no compressed copy.
    std::vector<uint32_t> blkDataPtr;
    std::vector<uint32_t> blkAllocSize;
};

} // namespace gem5
#endif // __MEM_CACHE_TAGS_ZCACHE_TAGS_NEW_HH__
