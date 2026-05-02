/*
 * ZCache Bucketed-LRU replacement policy.
 *
 * Instead of a per-set LRU counter, a single global logical timestamp is used
 * so that getVictim() can compare candidates drawn from many different sets.
 * The timestamp advances every bucketSize accesses, grouping nearby accesses
 * into the same "bucket" (approximation of LRU).
 */

#ifndef __MEM_CACHE_REPLACEMENT_POLICIES_ZCACHE_RP_HH__
#define __MEM_CACHE_REPLACEMENT_POLICIES_ZCACHE_RP_HH__

#include <cstdint>
#include <memory>

#include "mem/cache/replacement_policies/base.hh"

namespace gem5
{

struct ZCacheRPParams;

namespace replacement_policy
{

class ZCacheRP : public Base
{
  protected:
    struct ZCacheRPData : ReplacementData
    {
        uint64_t lastTouchTimestamp;
        ZCacheRPData() : lastTouchTimestamp(0) {}
    };

    const float bucketSizeFraction;

    // mutable: modified inside const touch()/reset()
    mutable uint64_t globalTimestamp;
    mutable uint64_t accessCount;
    mutable uint64_t bucketSize;   // K — computed lazily from entryCount
    mutable unsigned entryCount;   // incremented in instantiateEntry()

  public:
    typedef ZCacheRPParams Params;
    ZCacheRP(const Params &p);
    ~ZCacheRP() = default;

    // Bring two-arg Base::touch/reset into scope (C++ name-hiding fix)
    using Base::touch;
    using Base::reset;

    void invalidate(const std::shared_ptr<ReplacementData>& replacement_data)
        override;

    void touch(const std::shared_ptr<ReplacementData>& replacement_data)
        const override;

    void reset(const std::shared_ptr<ReplacementData>& replacement_data)
        const override;

    ReplaceableEntry* getVictim(const ReplacementCandidates& candidates)
        const override;

    std::shared_ptr<ReplacementData> instantiateEntry() override;
};

} // namespace replacement_policy
} // namespace gem5

#endif // __MEM_CACHE_REPLACEMENT_POLICIES_ZCACHE_RP_HH__
