#ifndef __MEM_CACHE_TAGS_DECOUPLED_DATA_STORE_HH__
#define __MEM_CACHE_TAGS_DECOUPLED_DATA_STORE_HH__

#include <cstdint>
#include <list>
#include <vector>

#include "params/DecoupledDataStore.hh"
#include "sim/sim_object.hh"

namespace gem5 {

/*
 * DecoupledDataStore — physical data array managed by a buddy allocator.
 *
 * Segments are 8 bytes.  Supported allocation sizes: 8, 16, 32, 64 bytes
 * (orders 0–3).  Buddy coalescing is performed on deallocation.
 *
 * The "pointer" returned by allocate() is a segment index
 * (byte_offset / SEGMENT_SIZE); this fits in a uint16_t for pools ≤ 512 KiB.
 */
class DecoupledDataStore : public SimObject
{
  public:
    static constexpr uint32_t ALLOC_FAIL   = UINT32_MAX;
    static constexpr uint32_t SEGMENT_SIZE = 8;   // bytes
    static constexpr int      MAX_ORDER    = 3;   // 64 B = 8 B << 3

    DecoupledDataStore(const DecoupledDataStoreParams &p);
    ~DecoupledDataStore() = default;

    // Allocate sizeBytes (8, 16, 32, or 64).
    // Returns segment index or ALLOC_FAIL on exhaustion / fragmentation.
    uint32_t allocate(uint32_t sizeBytes);

    // Return a previously allocated segment back to the pool.
    void deallocate(uint32_t ptr, uint32_t sizeBytes);

    // Copy len bytes from src into pool at segment index ptr.
    void write(uint32_t ptr, const uint8_t *src, uint32_t len);

    // Copy len bytes from pool at segment index ptr into dst.
    void read(uint32_t ptr, uint8_t *dst, uint32_t len) const;

    uint32_t getPoolBytes() const { return poolBytes; }

  private:
    const uint32_t poolBytes;
    std::vector<uint8_t> pool;

    // freeLists[k] contains segment indices for free blocks of size (8 << k).
    std::list<uint32_t> freeLists[4];

    static int      sizeToOrder(uint32_t sizeBytes);
    static uint32_t orderToSize(int order) { return SEGMENT_SIZE << order; }
    uint32_t        buddyOf(uint32_t ptr, int order) const
    { return ptr ^ (1u << order); }
    void            splitOrder(int order);
};

} // namespace gem5
#endif // __MEM_CACHE_TAGS_DECOUPLED_DATA_STORE_HH__
