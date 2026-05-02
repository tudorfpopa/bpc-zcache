"""
BPC test script — mirrors zcache_test.py but for the BPC compressor.

Topology
--------
PyTrafficGen
    └── L2 cache  (NoncoherentCache, CompressedTags + BPC, or plain LRU)
         └── IOXBar
              └── SimpleMemory

No CPU is needed: PyTrafficGen drives the cache directly in timing mode.

Workload
--------
Same 320 KiB linear sweep used by zcache_test.py. With the standard 4-way LRU
this aliases 5 lines into every set and thrashes. With BPC + CompressedTags the
*tag* count is unchanged, but each superblock holds up to `max_compression_ratio`
compressed sub-blocks, so the effective data capacity grows when lines compress.

Note: PyTrafficGen's read-only sweep does not seed memory with a particular
content pattern, so the absolute compression ratio you observe here is a
function of whatever SimpleMemory returns by default. The point of this script
is to wire BPC end-to-end and produce comparable hit/miss + compression stats,
not to claim a workload-realistic compression number.

Run:
    ./gem5/build/X86/gem5.opt --outdir=m5out_bpc bpc_test.py
    ./gem5/build/X86/gem5.opt --outdir=m5out_lru bpc_test.py --use-standard-cache

Key stats to look at:
    system.l2cache.overallHits::total
    system.l2cache.overallMisses::total
    system.l2cache.compressor.*  (compression-size histogram, latencies)
"""

import argparse
import os

import m5
from m5.objects import *

# Anchor output to the directory containing this script, so results land in
# the repo root no matter where gem5.opt was invoked from. The user can still
# override with --outdir on the gem5 CLI before this runs.
_SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))

# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------
parser = argparse.ArgumentParser(
    description="BPC vs. standard LRU cache micro-benchmark",
    formatter_class=argparse.ArgumentDefaultsHelpFormatter,
)
parser.add_argument(
    "--use-standard-cache",
    action="store_true",
    default=False,
    help="Replace BPC+CompressedTags with a plain LRU cache for comparison",
)
parser.add_argument("--l2-size", default="256KiB", help="Cache capacity")
parser.add_argument("--l2-assoc", type=int, default=4, help="Physical ways")
parser.add_argument(
    "--max-compression-ratio",
    type=int,
    default=2,
    help="Maximum compressed blocks per superblock (CompressedTags)",
)
parser.add_argument(
    "--outdir",
    default=None,
    help=(
        "Output directory for stats/config (relative paths resolved against "
        "the script's directory, not gem5's CWD). Defaults to "
        "<script_dir>/m5out_bpc or m5out_lru."
    ),
)
args = parser.parse_args()

# Resolve outdir to an absolute path next to this script, then push it into
# gem5's core so all subsequent output goes there.
_default_outdir = "m5out_lru" if args.use_standard_cache else "m5out_bpc"
_outdir = args.outdir or _default_outdir
if not os.path.isabs(_outdir):
    _outdir = os.path.join(_SCRIPT_DIR, _outdir)
os.makedirs(_outdir, exist_ok=True)
m5.options.outdir = _outdir
m5.core.setOutputDir(_outdir)
print(f"[bpc_test] Writing output to {_outdir}")

# ---------------------------------------------------------------------------
# System
# ---------------------------------------------------------------------------
system = System(membus=IOXBar(width=128))
system.clk_domain = SrcClockDomain(
    clock="1GHz", voltage_domain=VoltageDomain()
)

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
        f"[bpc_test] Using STANDARD {args.l2_assoc}-way LRU "
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
        # CompressedTags can't take 0; treat 0 as "uncapped on both sides"
        # (BPC reports its real size, tags stay at the default of 2).
        tags=(
            CompressedTags()
            if args.max_compression_ratio == 0
            else CompressedTags(max_compression_ratio=args.max_compression_ratio)
        ),
        compressor=BPC(max_compression_ratio=args.max_compression_ratio),
        replacement_policy=LRURP(),
    )
    print(
        f"[bpc_test] Using BPC: {args.l2_assoc} ways, {args.l2_size}, "
        f"max_compression_ratio={args.max_compression_ratio} "
        f"(applied to both CompressedTags and BPC)"
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

BLOCK_SIZE = 64
PERIOD     = 250

warm_up = system.tgen.createLinear(
    50_000_000, 0, REGION_SIZE, BLOCK_SIZE, PERIOD, PERIOD, 100, REGION_SIZE
)

measure = system.tgen.createLinear(
    50_000_000, 0, REGION_SIZE, BLOCK_SIZE, PERIOD, PERIOD, 100, REGION_SIZE
)

exit_gen = system.tgen.createExit(0)
system.tgen.start([warm_up, measure, exit_gen])

print("[bpc_test] Starting simulation …")
exit_event = m5.simulate()
print(
    f"[bpc_test] Exiting @ tick {m5.curTick()} "
    f"because: {exit_event.getCause()}"
)

print("\n--- Cache Statistics ---")
print("  (see <outdir>/stats.txt for full details)")
print("  Hit/miss:")
for stat in [
    "system.l2cache.overallHits::total",
    "system.l2cache.overallMisses::total",
]:
    print(f"    {stat}")
if not args.use_standard_cache:
    print("  Compression:")
    print("    system.l2cache.compressor.compressionSize")
    print("    system.l2cache.compressor.compressions")
    print("    system.l2cache.compressor.decompressions")
