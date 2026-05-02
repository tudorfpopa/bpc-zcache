#include "mem/cache/replacement_policies/zcache_rp.hh"

#include <algorithm>
#include <cassert>
#include <memory>

#include "params/ZCacheRP.hh"

namespace gem5
{
namespace replacement_policy
{

ZCacheRP::ZCacheRP(const Params &p)
    : Base(p),
      bucketSizeFraction(p.bucket_size_fraction),
      globalTimestamp(0),
      accessCount(0),
      bucketSize(0),
      entryCount(0)
{}

void
ZCacheRP::invalidate(const std::shared_ptr<ReplacementData>& replacement_data)
{
    std::static_pointer_cast<ZCacheRPData>(replacement_data)
        ->lastTouchTimestamp = 0;
}

void
ZCacheRP::touch(const std::shared_ptr<ReplacementData>& replacement_data)
    const
{
    // Lazily compute bucket size after all entries are allocated.
    if (bucketSize == 0 && entryCount > 0) {
        bucketSize = std::max(uint64_t(1),
            static_cast<uint64_t>(bucketSizeFraction * entryCount));
    }

    ++accessCount;
    if (bucketSize > 0 && (accessCount % bucketSize) == 0) {
        ++globalTimestamp;
    } else if (bucketSize == 0) {
        ++globalTimestamp;
    }

    std::static_pointer_cast<ZCacheRPData>(replacement_data)
        ->lastTouchTimestamp = globalTimestamp;
}

void
ZCacheRP::reset(const std::shared_ptr<ReplacementData>& replacement_data)
    const
{
    touch(replacement_data);
}

ReplaceableEntry*
ZCacheRP::getVictim(const ReplacementCandidates& candidates) const
{
    assert(!candidates.empty());

    ReplaceableEntry* victim = candidates[0];
    uint64_t minTs = std::static_pointer_cast<ZCacheRPData>(
                         candidates[0]->replacementData)->lastTouchTimestamp;

    for (const auto& cand : candidates) {
        uint64_t ts = std::static_pointer_cast<ZCacheRPData>(
                          cand->replacementData)->lastTouchTimestamp;
        if (ts < minTs) {
            minTs = ts;
            victim = cand;
        }
    }
    return victim;
}

std::shared_ptr<ReplacementData>
ZCacheRP::instantiateEntry()
{
    ++entryCount;
    return std::make_shared<ZCacheRPData>();
}

} // namespace replacement_policy
} // namespace gem5
