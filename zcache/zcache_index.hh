/*
 * ZCache Indexing Policy — H3 skewed associativity
 *
 * Each of the W physical ways hashes an incoming address with an independent
 * H3 matrix, so the W candidate slots for a given address are spread across
 * different sets.  This is the indexing half of the ZCache design.
 */

#ifndef __MEM_CACHE_INDEXING_POLICIES_ZCACHE_INDEX_HH__
#define __MEM_CACHE_INDEXING_POLICIES_ZCACHE_INDEX_HH__

#include <cstdint>
#include <utility>
#include <vector>

#include "mem/cache/tags/indexing_policies/base.hh"
#include "params/ZCacheIndexingPolicy.hh"

namespace gem5
{

class ZCacheIndexingPolicy : public BaseIndexingPolicy
{
  private:
    const uint32_t h3Seed;
    const int      logNumSets;

    // h3Matrix[way][bit] — 64-bit random mask for output bit 'bit' of way 'way'
    std::vector<std::vector<uint64_t>> h3Matrix;

    static uint64_t lcgNext(uint64_t &state);
    uint32_t        h3Hash(Addr addr, uint32_t way) const;

  public:
    typedef ZCacheIndexingPolicyParams Params;

    ZCacheIndexingPolicy(const Params &p);
    ~ZCacheIndexingPolicy() = default;

    // H3 set extraction for a specific way
    uint32_t extractSet(Addr addr, uint32_t way) const;

    // BaseIndexingPolicy interface (returns one entry per way)
    std::vector<ReplaceableEntry*>
    getPossibleEntries(const Addr &addr) const override;

    // Tag = block-aligned address (idempotent under extractTag, simplifies moveBlock)
    Addr extractTag(const Addr addr) const override;

    // Reconstruct address from tag (tag IS the block-aligned address)
    Addr regenerateAddr(const Addr &tag,
                        const ReplaceableEntry *entry) const override;

    // Returns {altSet, altWay} pairs for where entry's block could be relocated.
    // storedAddr must be the block-aligned address stored in the entry.
    std::vector<std::pair<uint32_t, uint32_t>>
    getAlternativeLocations(uint32_t curWay, Addr storedAddr) const;

    int getSetShift() const { return setShift; }
};

} // namespace gem5

#endif // __MEM_CACHE_INDEXING_POLICIES_ZCACHE_INDEX_HH__
