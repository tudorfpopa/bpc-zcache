#ifndef __MEM_CACHE_TAGS_FRAG_RESERVOIR_HH__
#define __MEM_CACHE_TAGS_FRAG_RESERVOIR_HH__

#include <cstdint>

#include "base/types.hh"
#include "mem/cache/replacement_policies/replaceable_entry.hh"
#include "params/FragReservoir.hh"
#include "sim/eventq.hh"
#include "sim/sim_object.hh"

namespace gem5 {

class ZCacheTagsNew;

/*
 * FragReservoir — Best-of-4 pseudo-random reservoir for fragmentation eviction.
 *
 * A gem5 Event fires every sampleIntervalTicks and uses a 16-bit LFSR to
 * sample a random block from ZCacheTagsNew.  Blocks are sorted into four bins
 * (8B/16B/32B/64B) each holding up to four cold candidates so getVictim()
 * runs in O(1) without scanning the full tag array.
 *
 * Uses SimObject (not ClockedObject) with a Tick-based interval to avoid
 * requiring a clock domain when parented under ZCacheGlue.
 */
class FragReservoir : public SimObject
{
  public:
    FragReservoir(const FragReservoirParams &p);
    ~FragReservoir() = default;

    // Wired by ZCacheGlue::startup() after all objects are constructed.
    void setTagsRef(ZCacheTagsNew *tags) { tagsRef = tags; }

    // Return the coldest candidate with allocSize >= sizeBytes.
    // Walks bins from sizeBytes up to 64B.  Returns nullptr if empty.
    ReplaceableEntry *getVictim(uint32_t sizeBytes);

    void startup() override;

  private:
    const Tick sampleIntervalTicks; // ticks between LFSR sampling pulses
    ZCacheTagsNew *tagsRef;
    uint32_t lfsrState;

    struct Candidate {
        ReplaceableEntry *entry    = nullptr;
        uint32_t          allocSize = 0;
        Tick              lastTouch = 0;
        bool              valid     = false;
    };

    // bins[b][0..3]: four slots per size class (0=8B, 1=16B, 2=32B, 3=64B).
    Candidate bins[4][4];

    EventFunctionWrapper sampleEvent;

    void     processSampleEvent();
    uint32_t lfsrNext();
    static int sizeToIdx(uint32_t sizeBytes);
};

} // namespace gem5
#endif // __MEM_CACHE_TAGS_FRAG_RESERVOIR_HH__
