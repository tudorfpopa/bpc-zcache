"""
cdzcache_test.py — Compressed Decoupled ZCache micro-benchmark.

Topology: PyTrafficGen → NoncoherentCache (ZCacheTagsNew + ZCacheGlue) → SimpleMemory

Same 320 KiB linear sweep as bpc_test.py / zcache_test.py.

Usage:
  ./gem5/build/X86/gem5.opt --outdir=m5out_cdzcache cdzcache_test.py
  ./gem5/build/X86/gem5.opt --outdir=m5out_lru      cdzcache_test.py --use-standard-cache
"""

import argparse
import os

import m5
from m5.objects import (
    Root, System, SrcClockDomain, VoltageDomain,
    SimpleMemory, AddrRange, IOXBar,
    NoncoherentCache, LRURP,
    PyTrafficGen,
    BPC, ZCacheIndexingPolicy, ZCacheRP,
    TaggedSetAssociative,
    ZCacheTagsNew, ZCacheGlue, DecoupledDataStore, FragReservoir,
)
import m5.core

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))

def set_outdir(name):
    path = os.path.join(SCRIPT_DIR, name)
    m5.options.outdir = path
    m5.core.setOutputDir(path)

parser = argparse.ArgumentParser()
parser.add_argument("--use-standard-cache", action="store_true",
                    help="Use plain 4-way LRU as baseline")
parser.add_argument("--outdir", default=None)
args, _ = parser.parse_known_args()

if args.outdir:
    set_outdir(args.outdir)
elif args.use_standard_cache:
    set_outdir("m5out_lru")
else:
    set_outdir("m5out_cdzcache")

# ── System ────────────────────────────────────────────────────────────────
system = System(membus=IOXBar(width=128))
system.clk_domain = SrcClockDomain(
    clock="1GHz",
    voltage_domain=VoltageDomain(),
)
system.mem_ranges    = [AddrRange("512MB")]
system.mem_ctrl      = SimpleMemory(bandwidth="32GiB/s", latency="10ns")
system.mem_ctrl.range = system.mem_ranges[0]
system.mem_ctrl.port  = system.membus.mem_side_ports

CACHE_SIZE  = "256KiB"
ASSOC       = 4
BLOCK_SIZE  = 64        # bytes
TAG_LAT     = 4         # cycles
SWEEP_BYTES = 320 * 1024
NUM_SWEEPS  = 2

# ── Cache ─────────────────────────────────────────────────────────────────
if args.use_standard_cache:
    system.l2cache = NoncoherentCache(
        size=CACHE_SIZE, assoc=ASSOC,
        tag_latency=TAG_LAT, data_latency=TAG_LAT, response_latency=TAG_LAT,
        mshrs=16, tgts_per_mshr=8,
        replacement_policy=LRURP(),
    )
else:
    # All cache-derived params specified explicitly because ZCacheTagsNew
    # is parented under ZCacheGlue (not a Cache), so Parent.xxx proxies
    # in BaseTags / BaseSetAssoc cannot resolve.
    zcache_tags = ZCacheTagsNew(
        size=CACHE_SIZE,
        assoc=ASSOC,
        block_size=BLOCK_SIZE,
        entry_size=BLOCK_SIZE,
        tag_latency=TAG_LAT,
        warmup_percentage=0,
        sequential_access=False,
        walk_levels=3,
        num_candidates=16,
        replacement_policy=ZCacheRP(bucket_size_fraction=0.05),
        zcache_indexing_policy=ZCacheIndexingPolicy(
            size=CACHE_SIZE, assoc=ASSOC, entry_size=BLOCK_SIZE,
        ),
        indexing_policy=TaggedSetAssociative(
            size=CACHE_SIZE, assoc=ASSOC, entry_size=BLOCK_SIZE,
        ),
    )
    bpc = BPC(max_compression_ratio=0)

    system.glue = ZCacheGlue(
        zcache_tags=zcache_tags,
        data_store=DecoupledDataStore(pool_bytes=CACHE_SIZE),
        reservoir=FragReservoir(sample_interval_ticks=10000),
        compressor=bpc,
    )

    system.l2cache = NoncoherentCache(
        size=CACHE_SIZE, assoc=ASSOC,
        tag_latency=TAG_LAT, data_latency=TAG_LAT, response_latency=TAG_LAT,
        mshrs=16, tgts_per_mshr=8,
        tags=zcache_tags,
    )

# ── Traffic ───────────────────────────────────────────────────────────────
# Topology: tgen → l2cache → membus → mem_ctrl
system.tgen = PyTrafficGen()
system.tgen.port        = system.l2cache.cpu_side
system.l2cache.mem_side = system.membus.cpu_side_ports

# ── Simulate ──────────────────────────────────────────────────────────────
root = Root(full_system=False, system=system)
m5.instantiate()

PERIOD = 100  # ticks between accesses

warm_up = system.tgen.createLinear(
    50_000_000, 0, SWEEP_BYTES, BLOCK_SIZE, PERIOD, PERIOD, 100, SWEEP_BYTES
)
measure = system.tgen.createLinear(
    50_000_000, 0, SWEEP_BYTES, BLOCK_SIZE, PERIOD, PERIOD, 100, SWEEP_BYTES
)
exit_gen = system.tgen.createExit(0)
system.tgen.start([warm_up, measure, exit_gen])
ev = m5.simulate()
print(f"Exiting @ tick {m5.curTick()} because: {ev.getCause()}")
