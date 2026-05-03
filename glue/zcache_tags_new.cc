#include "mem/cache/tags/zcache_tags_new.hh"
#include "mem/cache/tags/zcache_glue.hh"

#include <cassert>

#include "base/logging.hh"
#include "mem/cache/tags/decoupled_data_store.hh"

namespace gem5 {

ZCacheTagsNew::ZCacheTagsNew(const Params &p)
    : ZCacheTags(p)
{}

void ZCacheTagsNew::tagsInit()
{
    ZCacheTags::tagsInit();

    // Size parallel metadata arrays alongside the inherited blks vector.
    blkDataPtr.assign(blks.size(), DecoupledDataStore::ALLOC_FAIL);
    blkAllocSize.assign(blks.size(), 0);
}

// ---------------------------------------------------------------------------
// insertBlock — run glue handleFill after the standard ZCache insertion.
// ---------------------------------------------------------------------------

void ZCacheTagsNew::insertBlock(const PacketPtr pkt, CacheBlk *blk)
{
    ZCacheTags::insertBlock(pkt, blk);

    if (glue && pkt->hasData()) {
        glue->handleFill(pkt, blk);
    }
}

// ---------------------------------------------------------------------------
// forceInvalidate — bypass cuckoo walk; used by FragReservoir evictions.
// ---------------------------------------------------------------------------

void ZCacheTagsNew::forceInvalidate(CacheBlk *blk)
{
    if (!blk->isValid()) return;

    // Direct invalidation without any eviction/cuckoo logic.
    BaseTags::invalidate(blk);
    zcacheRP->invalidate(blk->replacementData);

    // Clear compressed metadata.
    uint32_t idx = getBlockIndex(blk);
    blkDataPtr[idx]   = DecoupledDataStore::ALLOC_FAIL;
    blkAllocSize[idx] = 0;
}

// ---------------------------------------------------------------------------
// doMoveBlock — extend parent to also migrate compressed metadata.
// ---------------------------------------------------------------------------

void ZCacheTagsNew::doMoveBlock(CacheBlk *src, CacheBlk *dst)
{
    uint32_t si = getBlockIndex(src);
    uint32_t di = getBlockIndex(dst);

    blkDataPtr[di]   = blkDataPtr[si];
    blkAllocSize[di] = blkAllocSize[si];
    blkDataPtr[si]   = DecoupledDataStore::ALLOC_FAIL;
    blkAllocSize[si] = 0;

    ZCacheTags::doMoveBlock(src, dst);
}

} // namespace gem5
