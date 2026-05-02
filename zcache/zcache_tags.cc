#include "mem/cache/tags/zcache_tags.hh"

#include <algorithm>
#include <cassert>
#include <cstring>

#include "base/intmath.hh"
#include "base/logging.hh"
#include "base/statistics.hh"
#include "mem/cache/cache_blk.hh"

namespace gem5
{

// ---------------------------------------------------------------------------
// Constructor
// ---------------------------------------------------------------------------

ZCacheTags::ZCacheTags(const Params &p)
    : BaseSetAssoc(p),
      zcacheIP(p.zcache_indexing_policy),
      zcacheRP(dynamic_cast<replacement_policy::ZCacheRP*>(
                   p.replacement_policy)),
      walkLevels(p.walk_levels),
      numCandidates(p.num_candidates),
      numWays(p.assoc),
      numSets_(p.size / p.block_size / p.assoc),
      zcacheStats(this)
{
    fatal_if(!zcacheIP,
        "ZCacheTags requires a ZCacheIndexingPolicy for zcache_indexing_policy");
    fatal_if(!zcacheRP,
        "ZCacheTags requires ZCacheRP as the replacement_policy");
    fatal_if(numWays < 2,
        "ZCache requires at least 2 ways");
    fatal_if(!isPowerOf2(numSets_),
        "ZCache numSets must be a power of 2");
    fatal_if(numCandidates < numWays,
        "ZCache num_candidates must be >= assoc");
}

// ---------------------------------------------------------------------------
// Stats
// ---------------------------------------------------------------------------

ZCacheTags::ZCacheTagStats::ZCacheTagStats(ZCacheTags *parent)
    : statistics::Group(parent),
      ADD_STAT(statWalkDepthTotal,
               statistics::units::Count::get(),
               "Total BFS walk depth across all misses"),
      ADD_STAT(statMissCount,
               statistics::units::Count::get(),
               "Total cache misses handled by findVictim"),
      ADD_STAT(statCandidatesTotal,
               statistics::units::Count::get(),
               "Total replacement candidates evaluated"),
      ADD_STAT(statRelocationsTotal,
               statistics::units::Count::get(),
               "Total cuckoo block relocations performed"),
      ADD_STAT(statWalkDepthAverage,
               statistics::units::Count::get(),
               "Average BFS walk depth per miss"),
      ADD_STAT(statCandidatesEvaluatedAverage,
               statistics::units::Count::get(),
               "Average replacement candidates evaluated per miss")
{
    statWalkDepthAverage = statWalkDepthTotal / statMissCount;
    statCandidatesEvaluatedAverage = statCandidatesTotal / statMissCount;
}

// ---------------------------------------------------------------------------
// tagsInit — re-register tag extractors for the block-aligned tag scheme
// ---------------------------------------------------------------------------

void
ZCacheTags::tagsInit()
{
    // BaseSetAssoc::tagsInit() assigns physical (set,way) positions, allocates
    // data pointers, instantiates replacement data, and registers standard tag
    // extractors (addr >> tagShift).  We then replace those extractors with
    // our idempotent block-aligned version (addr & ~blkMask).
    BaseSetAssoc::tagsInit();

    const Addr mask = blkMask; // capture by value for lambda
    for (auto& blk : blks) {
        blk.registerTagExtractor([mask](Addr addr) {
            return addr & ~mask;
        });
    }
}

// ---------------------------------------------------------------------------
// findBlock — W parallel H3 lookups
// ---------------------------------------------------------------------------

CacheBlk*
ZCacheTags::findBlock(const CacheBlk::KeyType &key) const
{
    for (uint32_t w = 0; w < numWays; w++) {
        const uint32_t set = zcacheIP->extractSet(key.address, w);
        CacheBlk* blk = const_cast<CacheBlk*>(getBlkAt(set, w));
        if (blk->match(key)) {
            return blk;
        }
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// accessBlock
// ---------------------------------------------------------------------------

CacheBlk*
ZCacheTags::accessBlock(const PacketPtr pkt, Cycles &lat)
{
    CacheBlk* blk = findBlock({pkt->getAddr(), pkt->isSecure()});

    stats.tagAccesses += numWays;
    if (sequentialAccess) {
        if (blk != nullptr) stats.dataAccesses += 1;
    } else {
        stats.dataAccesses += numWays;
    }

    if (blk != nullptr) {
        blk->increaseRefCount();
        zcacheRP->touch(blk->replacementData, pkt);
    }

    lat = lookupLatency;
    return blk;
}

// ---------------------------------------------------------------------------
// findVictim — BFS walk + ZCacheRP victim selection
// ---------------------------------------------------------------------------

CacheBlk*
ZCacheTags::findVictim(const CacheBlk::KeyType &key,
                       const std::size_t        /*size*/,
                       std::vector<CacheBlk*>&  evict_blks,
                       const uint64_t           /*partition_id*/)
{
    const Addr addr = key.address;
    pendingSwapChain.clear();

    // Fast path: if any Level-1 slot is already empty, use it directly.
    for (uint32_t w = 0; w < numWays; w++) {
        const uint32_t set = zcacheIP->extractSet(addr, w);
        CacheBlk* blk = getBlkAt(set, w);
        if (!blk->isValid()) {
            pendingSwapChain.push_back(blk);
            // No eviction needed — blk is already invalid.
            return blk;
        }
    }

    // --- BFS walk ---
    // ZWalkInfo nodes: level-1 seeds have parentIdx == -1.
    // For level k+1, parentIdx points to the node whose block COULD move into
    // this slot (freeing the parent slot for a further move toward level 1).

    std::vector<ZWalkInfo> candidates;
    candidates.reserve(numCandidates + numWays);

    bool allValid = true;

    // Seed with Level-1 conflicts
    for (uint32_t w = 0; w < numWays; w++) {
        const uint32_t set = zcacheIP->extractSet(addr, w);
        CacheBlk* blk = getBlkAt(set, w);
        candidates.push_back({set, w, -1});
        if (!blk->isValid()) allValid = false;
    }

    uint32_t fringeStart = 0;
    uint32_t fringeEnd   = numWays;
    uint32_t level       = 1;

    while (static_cast<uint32_t>(candidates.size()) < numCandidates
           && allValid
           && level < walkLevels)
    {
        const uint32_t prevFringeEnd = fringeEnd;
        for (uint32_t fi = fringeStart;
             fi < prevFringeEnd
             && static_cast<uint32_t>(candidates.size()) < numCandidates;
             ++fi)
        {
            const ZWalkInfo& fc = candidates[fi];
            const CacheBlk*  fb = getBlkAt(fc.set, fc.way);
            if (!fb->isValid()) break;

            // fb's stored (block-aligned) address
            const Addr fringeAddr = fb->getTag();

            auto alts = zcacheIP->getAlternativeLocations(fc.way, fringeAddr);
            for (const auto& [altSet, altWay] : alts) {
                if (static_cast<uint32_t>(candidates.size()) >= numCandidates)
                    break;

                CacheBlk* altBlk = getBlkAt(altSet, altWay);
                // Avoid self-loop: skip if alternative maps back to the same
                // physical block as the fringe node.
                if (altBlk == fb) continue;

                candidates.push_back({altSet, altWay,
                                      static_cast<int32_t>(fi)});
                if (!altBlk->isValid()) allValid = false;
            }
        }
        fringeStart = fringeEnd;
        fringeEnd   = static_cast<uint32_t>(candidates.size());
        ++level;
        if (fringeStart == fringeEnd) break; // no expansion possible
    }

    // Clamp to numCandidates
    const uint32_t numCands =
        std::min(static_cast<uint32_t>(candidates.size()), numCandidates);

    // Update stats
    zcacheStats.statWalkDepthTotal += level;
    zcacheStats.statCandidatesTotal += numCands;
    zcacheStats.statMissCount++;

    // --- Victim selection ---
    // Spec: prefer a free (invalid) slot over evicting a live block.
    // This is critical when the working set fits in the cache: without this
    // preference, empty slots at BFS depth > 1 are invisible to getVictim
    // (timestamp 0 == same as a recently loaded block in the first LRU bucket).
    int32_t victimIdx = -1;
    for (uint32_t i = 0; i < numCands; i++) {
        if (!getBlkAt(candidates[i].set, candidates[i].way)->isValid()) {
            victimIdx = static_cast<int32_t>(i);
            break;
        }
    }

    if (victimIdx < 0) {
        // All candidates are valid — use the replacement policy.
        ReplacementCandidates rpCands;
        rpCands.reserve(numCands);
        for (uint32_t i = 0; i < numCands; i++) {
            rpCands.push_back(getBlkAt(candidates[i].set, candidates[i].way));
        }
        CacheBlk* victim =
            static_cast<CacheBlk*>(zcacheRP->getVictim(rpCands));
        for (uint32_t i = 0; i < numCands; i++) {
            if (getBlkAt(candidates[i].set, candidates[i].way) == victim) {
                victimIdx = static_cast<int32_t>(i);
                break;
            }
        }
    }
    assert(victimIdx >= 0);

    // --- Build cuckoo chain (victim → root) ---
    // chain[0]       = evicted block (victim)
    // chain.back()   = Level-1 insertion slot (root)
    {
        int32_t idx = victimIdx;
        while (idx >= 0) {
            pendingSwapChain.push_back(
                getBlkAt(candidates[idx].set, candidates[idx].way));
            idx = candidates[idx].parentIdx;
        }
        // pendingSwapChain is now [victim, ..., root]
    }

    // Add victim to evict_blks so Cache.cc can handle writeback.
    // Only add if valid — BFS may find an empty slot as the chain terminus.
    // (Must be done BEFORE any data movement.)
    if (pendingSwapChain[0]->isValid()) {
        evict_blks.push_back(pendingSwapChain[0]);
    }

    // Return the root (Level-1 slot) as the insertion destination.
    // It is still VALID at this point; it will be freed by the cuckoo moves
    // executed in insertBlock() after Cache.cc has invalidated the victim.
    return pendingSwapChain.back();
}

// ---------------------------------------------------------------------------
// doMoveBlock — copy metadata + data bytes, then invalidate source
// ---------------------------------------------------------------------------

void
ZCacheTags::doMoveBlock(CacheBlk* src, CacheBlk* dst)
{
    assert(src->isValid());
    assert(!dst->isValid());

    // Copy data bytes first (before BaseTags::moveBlock invalidates src).
    std::memcpy(dst->data, src->data, blkSize);

    // Move metadata (CacheBlk::operator= copies tag/valid/dirty/etc.,
    // then calls src->invalidate()).
    BaseTags::moveBlock(src, dst);

    // Keep replacement data consistent.
    zcacheRP->invalidate(src->replacementData);
    zcacheRP->reset(dst->replacementData);
}

// ---------------------------------------------------------------------------
// insertBlock — execute pending cuckoo chain, then insert new data
// ---------------------------------------------------------------------------

void
ZCacheTags::insertBlock(const PacketPtr pkt, CacheBlk* blk)
{
    // Execute the cuckoo chain built by findVictim.
    //
    // pendingSwapChain = [victim, chain1, ..., root=blk]
    //
    // At this point:
    //   chain[0] (victim) is INVALID  — Cache.cc already invalidated it.
    //   chain[1..k]       are VALID   — still contain their old data.
    //
    // We move chain[i+1] → chain[i] for i = 0..k-1.
    // After the loop chain[k] (= blk = root) is INVALID and ready for
    // the incoming block.

    if (!pendingSwapChain.empty() && pendingSwapChain.back() == blk) {
        const size_t chainLen = pendingSwapChain.size();

        for (size_t i = 1; i < chainLen; i++) {
            CacheBlk* moveSrc = pendingSwapChain[i];     // closer to root
            CacheBlk* moveDst = pendingSwapChain[i - 1]; // closer to victim
            // moveDst should be invalid (either from Cache.cc or previous step)
            if (moveSrc->isValid()) {
                doMoveBlock(moveSrc, moveDst);
            }
        }

        // Count relocations (each move after the first is a cuckoo relocation)
        if (chainLen > 1) {
            zcacheStats.statRelocationsTotal += static_cast<int>(chainLen - 1);
        }

        pendingSwapChain.clear();
    }

    // blk (root) must now be invalid.
    assert(!blk->isValid());

    // Standard insertion (sets tag, requestorId, taskId, partitionId, stats)
    BaseTags::insertBlock(pkt, blk);
    stats.tagsInUse++;

    // Notify partitioning manager if present
    if (partitionManager) {
        auto pid = partitionManager->readPacketPartitionID(pkt);
        partitionManager->notifyAcquire(pid);
    }

    // Update replacement policy for the freshly inserted block
    zcacheRP->reset(blk->replacementData, pkt);
}

} // namespace gem5
