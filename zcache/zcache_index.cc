/*
 * ZCache Indexing Policy implementation — H3 universal hashing.
 *
 * H3 hashing: for each output bit b of way w,
 *   bit_b(hash_w(addr)) = parity( h3Matrix[w][b] & (addr >> setShift) )
 * This gives W statistically independent set indices for the same address.
 */

#include "mem/cache/tags/indexing_policies/zcache_index.hh"

#include "base/intmath.hh"
#include "base/logging.hh"
#include "mem/cache/replacement_policies/replaceable_entry.hh"

namespace gem5
{

uint64_t
ZCacheIndexingPolicy::lcgNext(uint64_t &state)
{
    // Knuth multiplicative LCG
    state = state * 6364136223846793005ULL + 1442695040888963407ULL;
    return state;
}

ZCacheIndexingPolicy::ZCacheIndexingPolicy(const Params &p)
    : BaseIndexingPolicy(p,
                         p.size / p.entry_size,
                         floorLog2(p.entry_size)),
      h3Seed(p.h3_seed),
      logNumSets(floorLog2(numSets)),
      h3Matrix(assoc, std::vector<uint64_t>(floorLog2(numSets), 0))
{
    fatal_if(numSets <= 2,
             "ZCacheIndexingPolicy: numSets must be > 2");
    fatal_if(!isPowerOf2(numSets),
             "ZCacheIndexingPolicy: numSets must be a power of 2");
    fatal_if(assoc < 2,
             "ZCacheIndexingPolicy: need at least 2 ways");

    uint64_t rng = static_cast<uint64_t>(h3Seed) | 1ULL;
    for (uint32_t w = 0; w < assoc; w++) {
        for (int b = 0; b < logNumSets; b++) {
            h3Matrix[w][b] = lcgNext(rng);
        }
    }
}

uint32_t
ZCacheIndexingPolicy::h3Hash(Addr addr, uint32_t way) const
{
    // Input: address bits above block offset
    const Addr input = addr >> setShift;
    uint32_t result = 0;
    for (int b = 0; b < logNumSets; b++) {
        if (__builtin_parityll(h3Matrix[way][b] & input)) {
            result |= (1u << b);
        }
    }
    return result;
}

uint32_t
ZCacheIndexingPolicy::extractSet(Addr addr, uint32_t way) const
{
    return h3Hash(addr, way) & setMask;
}

std::vector<ReplaceableEntry*>
ZCacheIndexingPolicy::getPossibleEntries(const Addr &addr) const
{
    std::vector<ReplaceableEntry*> entries;
    entries.reserve(assoc);
    for (uint32_t w = 0; w < assoc; w++) {
        entries.push_back(sets[extractSet(addr, w)][w]);
    }
    return entries;
}

Addr
ZCacheIndexingPolicy::extractTag(const Addr addr) const
{
    // Tag = block-aligned address.  Applying extractTag to a tag is idempotent,
    // which is required for CacheBlk::operator=(CacheBlk&&) to work correctly.
    return (addr >> setShift) << setShift;
}

Addr
ZCacheIndexingPolicy::regenerateAddr(const Addr &tag,
                                     const ReplaceableEntry *entry) const
{
    // Tag already IS the block-aligned address.
    return tag;
}

std::vector<std::pair<uint32_t, uint32_t>>
ZCacheIndexingPolicy::getAlternativeLocations(uint32_t curWay,
                                               Addr storedAddr) const
{
    std::vector<std::pair<uint32_t, uint32_t>> alts;
    alts.reserve(assoc - 1);
    for (uint32_t w = 0; w < assoc; w++) {
        if (w != curWay) {
            alts.push_back({extractSet(storedAddr, w), w});
        }
    }
    return alts;
}

} // namespace gem5
