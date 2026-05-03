#include "mem/cache/tags/decoupled_data_store.hh"

#include <algorithm>
#include <cassert>
#include <cstring>

#include "base/logging.hh"

namespace gem5 {

DecoupledDataStore::DecoupledDataStore(const DecoupledDataStoreParams &p)
    : SimObject(p),
      poolBytes(p.pool_bytes),
      pool(p.pool_bytes, 0)
{
    panic_if(poolBytes == 0 || (poolBytes % 64) != 0,
        "DecoupledDataStore: pool_bytes must be a positive multiple of 64");

    // Populate free list for 64-byte blocks (order 3).
    const uint32_t n64 = poolBytes / 64;
    for (uint32_t i = 0; i < n64; i++)
        freeLists[3].push_back(i * 8); // segment index = byte_offset / 8
}

int DecoupledDataStore::sizeToOrder(uint32_t s)
{
    switch (s) {
        case  8: return 0;
        case 16: return 1;
        case 32: return 2;
        case 64: return 3;
        default:
            panic("DecoupledDataStore: invalid size %u (must be 8/16/32/64)", s);
    }
}

void DecoupledDataStore::splitOrder(int order)
{
    assert(order > 0 && order <= MAX_ORDER);
    assert(!freeLists[order].empty());

    uint32_t ptr = freeLists[order].front();
    freeLists[order].pop_front();

    // Two children of order-1 occupy the lower and upper halves.
    freeLists[order - 1].push_back(ptr);
    freeLists[order - 1].push_back(ptr + (1u << (order - 1)));
}

uint32_t DecoupledDataStore::allocate(uint32_t sizeBytes)
{
    int order = sizeToOrder(sizeBytes);

    // Find the smallest available order >= requested.
    int avail = -1;
    for (int k = order; k <= MAX_ORDER; k++) {
        if (!freeLists[k].empty()) { avail = k; break; }
    }
    if (avail < 0) return ALLOC_FAIL;

    // Split down to the requested order.
    while (avail > order) splitOrder(avail--);

    uint32_t ptr = freeLists[order].front();
    freeLists[order].pop_front();
    return ptr;
}

void DecoupledDataStore::deallocate(uint32_t ptr, uint32_t sizeBytes)
{
    int order = sizeToOrder(sizeBytes);

    // Coalesce with buddy while possible.
    while (order < MAX_ORDER) {
        uint32_t buddy = buddyOf(ptr, order);
        auto &fl = freeLists[order];
        auto  it = std::find(fl.begin(), fl.end(), buddy);
        if (it == fl.end()) break;
        fl.erase(it);
        ptr = std::min(ptr, buddy);
        order++;
    }
    freeLists[order].push_back(ptr);
}

void DecoupledDataStore::write(uint32_t ptr, const uint8_t *src, uint32_t len)
{
    assert(static_cast<size_t>(ptr) * SEGMENT_SIZE + len <= poolBytes);
    memcpy(pool.data() + ptr * SEGMENT_SIZE, src, len);
}

void DecoupledDataStore::read(uint32_t ptr, uint8_t *dst, uint32_t len) const
{
    assert(static_cast<size_t>(ptr) * SEGMENT_SIZE + len <= poolBytes);
    memcpy(dst, pool.data() + ptr * SEGMENT_SIZE, len);
}

} // namespace gem5
