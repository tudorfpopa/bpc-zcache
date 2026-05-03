#include "mem/cache/tags/frag_reservoir.hh"
#include "mem/cache/tags/zcache_tags_new.hh"

#include <cassert>
#include <limits>

#include "base/logging.hh"

namespace gem5 {

FragReservoir::FragReservoir(const FragReservoirParams &p)
    : SimObject(p),
      sampleIntervalTicks(p.sample_interval_ticks),
      tagsRef(nullptr),
      lfsrState(0xACE1u), // non-zero initial state required for LFSR
      sampleEvent([this]{ processSampleEvent(); }, name())
{}

void FragReservoir::startup()
{
    schedule(sampleEvent, curTick() + sampleIntervalTicks);
}

uint32_t FragReservoir::lfsrNext()
{
    // 16-bit Fibonacci LFSR, taps x^16 + x^14 + x^13 + x^11 + 1.
    // Produces the full period of 65535 before repeating.
    lfsrState = (lfsrState >> 1)
              ^ (-(lfsrState & 1u) & 0xB400u);
    return lfsrState;
}

int FragReservoir::sizeToIdx(uint32_t sz)
{
    if (sz <=  8) return 0;
    if (sz <= 16) return 1;
    if (sz <= 32) return 2;
    return 3;
}

void FragReservoir::processSampleEvent()
{
    if (tagsRef) {
        const uint32_t numBlocks = tagsRef->getNumBlocks();
        if (numBlocks > 0) {
            uint32_t idx = lfsrNext() % numBlocks;
            CacheBlk *blk = tagsRef->getBlockByIndex(idx);
            if (blk && blk->isValid()) {
                uint32_t sz    = tagsRef->getBlockAllocSize(blk);
                Tick     touch = tagsRef->getBlockLastTouch(blk);
                int      b     = sizeToIdx(sz);

                // Find a free slot, or the slot with the highest (warmest) touch.
                int  replSlot    = -1;
                Tick maxTouch    = 0;
                bool hasFreeSlot = false;

                for (int s = 0; s < 4; s++) {
                    if (!bins[b][s].valid) {
                        replSlot    = s;
                        hasFreeSlot = true;
                        break;
                    }
                    if (bins[b][s].lastTouch > maxTouch) {
                        maxTouch = bins[b][s].lastTouch;
                        replSlot = s;
                    }
                }

                // Keep only cold candidates: insert if slot free or new is colder.
                if (hasFreeSlot || touch < maxTouch) {
                    bins[b][replSlot] = {blk, sz, touch, true};
                }
            }
        }
    }

    schedule(sampleEvent, curTick() + sampleIntervalTicks);
}

ReplaceableEntry *FragReservoir::getVictim(uint32_t sizeBytes)
{
    // Walk from the bin matching sizeBytes upward (larger blocks can satisfy
    // a smaller request by evicting more capacity than strictly needed).
    for (int b = sizeToIdx(sizeBytes); b < 4; b++) {
        int  coldSlot = -1;
        Tick minTouch = std::numeric_limits<Tick>::max();

        for (int s = 0; s < 4; s++) {
            if (bins[b][s].valid && bins[b][s].lastTouch < minTouch) {
                minTouch = bins[b][s].lastTouch;
                coldSlot = s;
            }
        }
        if (coldSlot >= 0) {
            ReplaceableEntry *victim = bins[b][coldSlot].entry;
            bins[b][coldSlot] = {}; // clear slot so it can be repopulated
            return victim;
        }
    }
    return nullptr;
}

} // namespace gem5
