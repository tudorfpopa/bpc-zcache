from m5.objects.Tags import ZCacheTags
from m5.objects.Compressors import BPC
from m5.params import *
from m5.proxy import *
from m5.SimObject import SimObject


class DecoupledDataStore(SimObject):
    type = "DecoupledDataStore"
    cxx_class = "gem5::DecoupledDataStore"
    cxx_header = "mem/cache/tags/decoupled_data_store.hh"

    # Physical data pool size.  Set this to match the parent cache's size
    # so the uncompressed footprint never exceeds hardware capacity.
    # No proxy default — must be specified explicitly in the config script.
    pool_bytes = Param.MemorySize("256KiB", "Total bytes in the buddy-managed data pool")


class FragReservoir(SimObject):
    type = "FragReservoir"
    cxx_class = "gem5::FragReservoir"
    cxx_header = "mem/cache/tags/frag_reservoir.hh"

    # Tick-based interval avoids requiring a ClockDomain when parented under
    # a plain SimObject (ZCacheGlue).  10 000 ticks ≈ 10 cycles @ 1 GHz.
    sample_interval_ticks = Param.Tick(10000, "Sampling interval in ticks")


class ZCacheTagsNew(ZCacheTags):
    """ZCacheTags extended with per-block compressed-data pointer tracking
    and a forceInvalidate() path for reservoir-driven evictions.

    NOTE: When used standalone under ZCacheGlue (not inline in a Cache),
    proxy params like size/assoc/block_size/tag_latency must be passed
    explicitly — ZCacheGlue does not forward these from a parent cache.
    """

    type = "ZCacheTagsNew"
    cxx_class = "gem5::ZCacheTagsNew"
    cxx_header = "mem/cache/tags/zcache_tags_new.hh"

    # Override partitioning_manager to NULL so the Parent.partitioning_manager
    # proxy (in BaseTags) is not triggered when the parent is ZCacheGlue.
    partitioning_manager = Param.PartitionManager(
        NULL, "Partition manager (disabled; not needed for CDZCache)"
    )


class ZCacheGlue(SimObject):
    """Coordinator that connects BPC, DecoupledDataStore, FragReservoir,
    and ZCacheTagsNew to implement the Compressed Decoupled ZCache."""

    type = "ZCacheGlue"
    cxx_class = "gem5::ZCacheGlue"
    cxx_header = "mem/cache/tags/zcache_glue.hh"

    # Sub-module wiring — must all be set in the config script.
    data_store = Param.DecoupledDataStore("Buddy-allocator data pool")
    reservoir = Param.FragReservoir("Pseudo-random fragmentation reservoir")
    zcache_tags = Param.ZCacheTagsNew("ZCacheTagsNew instance to coordinate")
    compressor = Param.BPC("BPC compressor for fill sizing")

    block_size = Param.Int(Parent.cache_line_size, "cache line size in bytes")
