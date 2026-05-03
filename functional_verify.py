"""
functional_verify.py — CDZCache full functional verification sim config.

Three scenarios exercising distinct data-flow paths:

  A  Normal operation    — BPC granularity path; pool never exhausted
  B  Small pool          — fragmentation eviction path; reservoir hit rate > 0
  C  Cuckoo stress       — 2.5x oversubscription; exercises doMoveBlock migration

Usage:
  ./gem5/build/X86/gem5.opt --outdir=m5out_verify_A functional_verify.py --scenario A
  ./gem5/build/X86/gem5.opt --outdir=m5out_verify_B functional_verify.py --scenario B
  ./gem5/build/X86/gem5.opt --outdir=m5out_verify_C functional_verify.py --scenario C

Then run:
  python3 check_stats.py
"""

import argparse
import os

import m5
from m5.objects import (
    Root, System, SrcClockDomain, VoltageDomain,
    SimpleMemory, AddrRange, IOXBar,
    NoncoherentCache,
    PyTrafficGen,
    BPC, ZCacheIndexingPolicy, ZCacheRP,
    TaggedSetAssociative,
    ZCacheTagsNew, ZCacheGlue, DecoupledDataStore, FragReservoir,
)
import m5.core

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))

# ── Scenario parameter table ──────────────────────────────────────────────────
SCENARIOS = {
    "A": {
        "label":                 "Normal CDZCache (BPC granularity path)",
        "pool_bytes":            "256KiB",     # never exhausted
        "sweep_bytes":           320 * 1024,   # 320 KiB = 5/4 of cache capacity
        "cache_size":            "256KiB",
        "assoc":                 4,
        "walk_levels":           3,
        "num_candidates":        16,
        "sample_interval_ticks": 10000,
        "period":                100,          # ticks between requests
        "sim_ticks":             50_000_000,   # ticks per phase (matches cdzcache_test.py)
        "outdir":                "m5out_verify_A",
    },
    "B": {
        "label":                 "Small Pool / Fragmentation (reservoir eviction path)",
        "pool_bytes":            "16KiB",      # 1024 × 16B slots = 25% of cache → forces ALLOC_FAIL
        "sweep_bytes":           320 * 1024,
        "cache_size":            "256KiB",
        "assoc":                 4,
        "walk_levels":           3,
        "num_candidates":        16,
        "sample_interval_ticks": 10000,
        "period":                100,
        "sim_ticks":             50_000_000,
        "outdir":                "m5out_verify_B",
    },
    "C": {
        "label":                 "ZCache Cuckoo Stress (doMoveBlock metadata migration path)",
        "pool_bytes":            "256KiB",     # never exhausted
        "sweep_bytes":           640 * 1024,   # 640 KiB = 2.5x cache → heavy cuckoo chaining
        "cache_size":            "256KiB",
        "assoc":                 4,
        "walk_levels":           3,
        "num_candidates":        16,
        "sample_interval_ticks": 10000,
        # PERIOD=15000 ticks > miss latency (~12000 ticks) → serialises misses so the
        # class-level ZCacheTags::pendingSwapChain is never overwritten between
        # findVictim and insertBlock.  phase_duration=200M ticks gives the sweep
        # 153.6M ticks to complete (10240 accesses × 15000 ticks each).
        "period":                15000,
        "sim_ticks":             200_000_000,
        "outdir":                "m5out_verify_C",
    },
}

# ── Helpers ───────────────────────────────────────────────────────────────────

def set_outdir(name):
    path = os.path.join(SCRIPT_DIR, name)
    m5.options.outdir = path
    m5.core.setOutputDir(path)

# ── Argument parsing ──────────────────────────────────────────────────────────

parser = argparse.ArgumentParser()
parser.add_argument("--scenario", choices=["A", "B", "C"], required=True,
                    help="Which verification scenario to run")
parser.add_argument("--outdir", default=None,
                    help="Override output directory (absolute path preferred)")
args, _ = parser.parse_known_args()

cfg = SCENARIOS[args.scenario]

if args.outdir:
    set_outdir(args.outdir)
else:
    set_outdir(cfg["outdir"])

print(f"\n=== Scenario {args.scenario}: {cfg['label']} ===")
print(f"    pool_bytes={cfg['pool_bytes']}  "
      f"sweep_bytes={cfg['sweep_bytes'] // 1024} KiB  "
      f"cache_size={cfg['cache_size']}\n")

# ── System ────────────────────────────────────────────────────────────────────

BLOCK_SIZE = 64       # bytes
TAG_LAT    = 4        # cycles

system = System(membus=IOXBar(width=128))
system.clk_domain = SrcClockDomain(
    clock="1GHz",
    voltage_domain=VoltageDomain(),
)
system.mem_ranges   = [AddrRange("512MB")]   # large enough for 640 KiB sweep
system.mem_ctrl     = SimpleMemory(bandwidth="32GiB/s", latency="10ns")
system.mem_ctrl.range = system.mem_ranges[0]
system.mem_ctrl.port  = system.membus.mem_side_ports

CACHE_SIZE = cfg["cache_size"]
ASSOC      = cfg["assoc"]

# ── CDZCache ──────────────────────────────────────────────────────────────────
# All ZCacheTagsNew params must be explicit: Parent.xxx proxies cannot resolve
# when the parent SimObject is ZCacheGlue (not a Cache or NoncoherentCache).

zcache_tags = ZCacheTagsNew(
    size=CACHE_SIZE,
    assoc=ASSOC,
    block_size=BLOCK_SIZE,
    entry_size=BLOCK_SIZE,
    tag_latency=TAG_LAT,
    warmup_percentage=0,
    sequential_access=False,
    walk_levels=cfg["walk_levels"],
    num_candidates=cfg["num_candidates"],
    replacement_policy=ZCacheRP(bucket_size_fraction=0.05),
    zcache_indexing_policy=ZCacheIndexingPolicy(
        size=CACHE_SIZE, assoc=ASSOC, entry_size=BLOCK_SIZE,
    ),
    indexing_policy=TaggedSetAssociative(
        size=CACHE_SIZE, assoc=ASSOC, entry_size=BLOCK_SIZE,
    ),
)

bpc = BPC(max_compression_ratio=0)

# NOTE: do NOT pass block_size explicitly to ZCacheGlue — it uses
# Parent.cache_line_size which resolves from System, matching cdzcache_test.py.
system.glue = ZCacheGlue(
    zcache_tags=zcache_tags,
    data_store=DecoupledDataStore(pool_bytes=cfg["pool_bytes"]),
    reservoir=FragReservoir(sample_interval_ticks=cfg["sample_interval_ticks"]),
    compressor=bpc,
)

system.l2cache = NoncoherentCache(
    size=CACHE_SIZE, assoc=ASSOC,
    tag_latency=TAG_LAT, data_latency=TAG_LAT, response_latency=TAG_LAT,
    mshrs=16, tgts_per_mshr=8,
    tags=zcache_tags,
)

# ── Traffic ───────────────────────────────────────────────────────────────────
# Topology: tgen → l2cache → membus → mem_ctrl

system.tgen = PyTrafficGen()
system.tgen.port        = system.l2cache.cpu_side
system.l2cache.mem_side = system.membus.cpu_side_ports

# ── Simulate ──────────────────────────────────────────────────────────────────

root = Root(full_system=False, system=system)
m5.instantiate()

PERIOD    = cfg["period"]
SIM_TICKS = cfg["sim_ticks"]
SWEEP     = cfg["sweep_bytes"]

warm_up  = system.tgen.createLinear(
    SIM_TICKS, 0, SWEEP, BLOCK_SIZE, PERIOD, PERIOD, 100, SWEEP)
measure  = system.tgen.createLinear(
    SIM_TICKS, 0, SWEEP, BLOCK_SIZE, PERIOD, PERIOD, 100, SWEEP)
exit_gen = system.tgen.createExit(0)

system.tgen.start([warm_up, measure, exit_gen])
ev = m5.simulate()
print(f"Exiting @ tick {m5.curTick()} because: {ev.getCause()}")
