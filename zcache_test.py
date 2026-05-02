"""
ZCache test script — configs/example/zcache_test.py

Topology
--------
PyTrafficGen
    └── L2 cache  (ZCache or standard LRU, 256 KiB, 4-way)
         └── IOXBar
              └── SimpleMemory

No CPU is needed: PyTrafficGen drives the cache directly in timing mode.

Workload — conflict-miss stress test
--------------------------------------
A 256 KiB, 4-way, 1024-set cache has a "set stride" of 1024 × 64 B = 64 KiB.
Any two addresses that differ by a multiple of 64 KiB alias to the same set in
a standard set-associative cache.

The test sweeps a 320 KiB region (= 5/4 × 256 KiB) with 64-byte blocks:
  • 5120 distinct cache lines contend for 4096 physical slots.
  • In each set of the standard LRU, 5 lines compete for 4 ways → the 5th
    line always evicts the 1st, producing ~1024 conflict misses per pass.
  • ZCache's H3 hash maps each (address, way) pair to a statistically
    independent set, so the 5 conflicting lines can live in different sets
    simultaneously → fewer conflict misses.

Run:
    build/NULL/gem5.opt configs/example/zcache_test.py
    build/NULL/gem5.opt configs/example/zcache_test.py --use-standard-cache

Key statistics to compare:
    system.l2cache.tags.statWalkDepthAverage
    system.l2cache.tags.statRelocationsTotal
    system.l2cache.tags.statCandidatesEvaluatedAverage
    system.l2cache.overallHits::total
    system.l2cache.overallMisses::total
"""

import argparse

import m5
from m5.objects import *

# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------
parser = argparse.ArgumentParser(
    description="ZCache vs. standard LRU cache micro-benchmark",
    formatter_class=argparse.ArgumentDefaultsHelpFormatter,
)
parser.add_argument(
    "--use-standard-cache",
    action="store_true",
    default=False,
    help="Replace ZCache with a standard 4-way LRU cache for comparison",
)
parser.add_argument("--l2-size", default="256KiB", help="Cache capacity")
parser.add_argument("--l2-assoc", type=int, default=4, help="Physical ways")
parser.add_argument(
    "--walk-levels", type=int, default=3, help="ZCache BFS walk depth (L)"
)
parser.add_argument(
    "--num-candidates",
    type=int,
    default=16,
    help="ZCache replacement candidates per miss (R)",
)
args = parser.parse_args()

# ---------------------------------------------------------------------------
# System
# ---------------------------------------------------------------------------
system = System(membus=IOXBar(width=128))
system.clk_domain = SrcClockDomain(
    clock="1GHz", voltage_domain=VoltageDomain()
)

# 320 KiB region = 5/4 × 256 KiB: 5 cache lines per set in a standard
# 4-way 1024-set cache → one conflicting line always evicted per set.
REGION_SIZE = 320 * 1024
system.mem_ctrl = SimpleMemory(bandwidth="32GiB/s", latency="10ns")
system.mem_ctrl.range = AddrRange(REGION_SIZE * 4)
system.mem_ctrl.port = system.membus.mem_side_ports

# ---------------------------------------------------------------------------
# Cache under test
# ---------------------------------------------------------------------------
if args.use_standard_cache:
    system.l2cache = NoncoherentCache(
        size=args.l2_size,
        assoc=args.l2_assoc,
        tag_latency=4,
        data_latency=4,
        response_latency=4,
        mshrs=16,
        tgts_per_mshr=8,
        replacement_policy=LRURP(),
    )
    print(
        f"[zcache_test] Using STANDARD {args.l2_assoc}-way LRU "
        f"({args.l2_size})"
    )
else:
    system.l2cache = NoncoherentCache(
        size=args.l2_size,
        assoc=args.l2_assoc,
        tag_latency=4,
        data_latency=4,
        response_latency=4,
        mshrs=16,
        tgts_per_mshr=8,
        tags=ZCacheTags(
            walk_levels=args.walk_levels,
            num_candidates=args.num_candidates,
            replacement_policy=ZCacheRP(bucket_size_fraction=0.05),
        ),
    )
    print(
        f"[zcache_test] Using ZCACHE: {args.l2_assoc} ways, "
        f"{args.l2_size}, L={args.walk_levels}, R={args.num_candidates}"
    )

system.l2cache.mem_side = system.membus.cpu_side_ports

# ---------------------------------------------------------------------------
# Traffic generator
# ---------------------------------------------------------------------------
system.tgen = PyTrafficGen()
system.tgen.port = system.l2cache.cpu_side

root = Root(full_system=False, system=system)
root.system.mem_mode = "timing"
m5.instantiate()

# ---------------------------------------------------------------------------
# Traffic pattern: two sequential sweeps over REGION_SIZE bytes.
# First pass warms the cache; second pass measures hit rate.
# With standard 4-way LRU, many addresses alias the same set → conflict misses.
# ZCache's BFS + cuckoo relocation dramatically reduces those misses.
# ---------------------------------------------------------------------------
BLOCK_SIZE = 64
PERIOD     = 250  # ticks between requests (250 ps @ 1 GHz = reasonable spacing)

# createLinear(duration, start_addr, end_addr, block_size,
#              min_period, max_period, read_percent, data_limit)
warm_up = system.tgen.createLinear(
    50_000_000, 0, REGION_SIZE, BLOCK_SIZE, PERIOD, PERIOD, 100, REGION_SIZE
)

measure = system.tgen.createLinear(
    50_000_000, 0, REGION_SIZE, BLOCK_SIZE, PERIOD, PERIOD, 100, REGION_SIZE
)

exit_gen = system.tgen.createExit(0)
system.tgen.start([warm_up, measure, exit_gen])

print("[zcache_test] Starting simulation …")
exit_event = m5.simulate()
print(
    f"[zcache_test] Exiting @ tick {m5.curTick()} "
    f"because: {exit_event.getCause()}"
)

print("\n--- Cache Statistics ---")
print("  (see m5out/stats.txt for full details)")
if not args.use_standard_cache:
    print("  ZCache-specific stats:")
    for stat in [
        "system.l2cache.tags.statWalkDepthAverage",
        "system.l2cache.tags.statRelocationsTotal",
        "system.l2cache.tags.statCandidatesEvaluatedAverage",
    ]:
        print(f"    {stat}")
print("  Hit/miss:")
for stat in [
    "system.l2cache.overallHits::total",
    "system.l2cache.overallMisses::total",
]:
    print(f"    {stat}")
