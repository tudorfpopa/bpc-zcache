# Copyright (c) 2021 The Regents of the University of California.
# All rights reserved.
#
# Modified to use ZCache (H3 hashing + cuckoo BFS replacement)
# without BPC compression.

import argparse
import time

import m5
from m5.objects import Root, NULL
from m5.objects.Tags import ZCacheTags
from m5.objects.ReplacementPolicies import ZCacheRP
from m5.objects.IndexingPolicies import ZCacheIndexingPolicy

from gem5.components.boards.x86_board import X86Board
from gem5.components.memory import DualChannelDDR4_2400
from gem5.components.processors.cpu_types import CPUTypes
from gem5.components.processors.simple_switchable_processor import (
    SimpleSwitchableProcessor,
)
from gem5.isas import ISA
from gem5.resources.resource import obtain_resource
from gem5.simulate.exit_event import ExitEvent
from gem5.simulate.simulator import Simulator
from gem5.utils.requires import requires

# ── Build checks ──────────────────────────────────────────────────────────────
requires(
    isa_required=ISA.X86,
    kvm_required=True,
)

# ── Benchmark args ────────────────────────────────────────────────────────────
benchmark_choices = [
    "blackscholes", "bodytrack", "canneal", "dedup", "facesim",
    "ferret", "fluidanimate", "freqmine", "raytrace", "streamcluster",
    "swaptions", "vips", "x264",
]
size_choices = ["simsmall", "simmedium", "simlarge"]

parser = argparse.ArgumentParser(
    description="Run PARSEC benchmarks with ZCache (no BPC)."
)
parser.add_argument("--benchmark", type=str, required=True,
                    choices=benchmark_choices,
                    help="Benchmark program to execute.")
parser.add_argument("--size", type=str, required=True,
                    choices=size_choices,
                    help="Simulation size.")
args = parser.parse_args()

# ── ZCache components ─────────────────────────────────────────────────────────

# H3 universal hash indexing policy
zcache_indexing = ZCacheIndexingPolicy(
    size="256KiB",
    entry_size=64,      # cache line / block size in bytes
    h3_seed=42,         # seed for H3 random matrices
)

# Global bucketed-LRU replacement policy
zcache_rp = ZCacheRP(
    bucket_size_fraction=0.1,
)

# ZCacheTags: BFS walk + cuckoo relocation
zcache_tags = ZCacheTags(
    zcache_indexing_policy=zcache_indexing,
    walk_levels=3,          # L — BFS levels to expand candidate pool
    num_candidates=16,      # R — max candidates evaluated per miss
    replacement_policy=zcache_rp,
)

# ── Cache hierarchy ───────────────────────────────────────────────────────────
from gem5.components.cachehierarchies.classic.private_l1_shared_l2_cache_hierarchy import (
    PrivateL1SharedL2CacheHierarchy,
)

class ZCacheCacheHierarchy(PrivateL1SharedL2CacheHierarchy):
    """PrivateL1SharedL2 with ZCacheTags injected into the L2."""
    def incorporate_cache(self, board):
        super().incorporate_cache(board)
        # Find correct L2 attribute name
        l2_attrs = [a for a in dir(self) if 'l2' in a.lower() or 'cache' in a.lower()]
        print("DEBUG cache attrs:", l2_attrs)
        # Try common names
        for attr in ['l2cache', 'l2_cache', '_l2cache', '_l2_cache', 'l2']:
            if hasattr(self, attr):
                print(f"DEBUG: found L2 as self.{attr}")
                getattr(self, attr).tags = zcache_tags
                break

cache_hierarchy = ZCacheCacheHierarchy(
    l1d_size="32KiB",
    l1d_assoc=8,
    l1i_size="32KiB",
    l1i_assoc=8,
    l2_size="256KiB",
    l2_assoc=16,
)

# ── Memory ────────────────────────────────────────────────────────────────────
memory = DualChannelDDR4_2400(size="3GiB")

# ── Processor: KVM boot → Timing for ROI ─────────────────────────────────────
processor = SimpleSwitchableProcessor(
    starting_core_type=CPUTypes.KVM,
    switch_core_type=CPUTypes.TIMING,
    isa=ISA.X86,
    num_cores=2,
)

# ── Board ─────────────────────────────────────────────────────────────────────
board = X86Board(
    clk_freq="3GHz",
    processor=processor,
    memory=memory,
    cache_hierarchy=cache_hierarchy,
)

# ── Workload ──────────────────────────────────────────────────────────────────
command = (
    f"cd /home/gem5/parsec-benchmark;"
    + "source env.sh;"
    + f"parsecmgmt -a run -p {args.benchmark} -c gcc-hooks -i {args.size} -n 2;"
    + "sleep 5;"
    + "m5 exit;"
)

board.set_kernel_disk_workload(
    kernel=obtain_resource("x86-linux-kernel-4.19.83", resource_version="1.0.0"),
    disk_image=obtain_resource("x86-parsec", resource_version="1.0.0"),
    readfile_contents=command,
)

# ── Exit event handlers ───────────────────────────────────────────────────────
def handle_workbegin():
    print("Done booting Linux")
    print("Resetting stats at the start of ROI!")
    m5.stats.reset()
    processor.switch()
    yield False


def handle_workend():
    print("Dump stats at the end of the ROI!")
    m5.stats.dump()
    yield True


simulator = Simulator(
    board=board,
    on_exit_event={
        ExitEvent.WORKBEGIN: handle_workbegin(),
        ExitEvent.WORKEND: handle_workend(),
    },
)

# ── Run ───────────────────────────────────────────────────────────────────────
globalStart = time.time()

print("Running the simulation")
print("Using KVM cpu")
print(f"Cache: ZCache only (L2: 256KiB, assoc=16, walk_levels=3, num_candidates=16)")

m5.stats.reset()
simulator.run()

print("All simulation events were successful.")
print("Done with the simulation")
print()
print("Performance statistics:")
print("Simulated time in ROI: " + str(simulator.get_roi_ticks()[0]))
print("Ran a total of", simulator.get_current_tick() / 1e12, "simulated seconds")
print(
    "Total wallclock time: %.2fs, %.2f min"
    % (time.time() - globalStart, (time.time() - globalStart) / 60)
)