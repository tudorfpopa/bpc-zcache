"""
Full-system PARSEC sweep across cache variants:

    --variant lru       baseline 4-way LRU L2
    --variant bpc       CompressedTags + BPC
    --variant zcache    ZCacheTags + ZCacheRP
    --variant cdzcache  ZCacheTagsNew + ZCacheGlue + BPC + DecoupledDataStore + FragReservoir

Boots Linux under KVM, switches to TIMING at ROI workbegin, dumps stats at workend.

Usage:
    ./gem5/build/X86/gem5.opt --outdir=m5out_<bench>_<variant> \
        cdzcache_parsec.py --benchmark dedup --size simsmall --variant cdzcache
"""

import argparse
import time

import m5
from m5.objects import (
    Root,
    LRURP,
    CompressedTags,
    BPC,
    ZCacheTags,
    ZCacheTagsNew,
    ZCacheRP,
    ZCacheIndexingPolicy,
    ZCacheGlue,
    DecoupledDataStore,
    FragReservoir,
)

from gem5.components.boards.x86_board import X86Board
from gem5.components.cachehierarchies.classic.private_l1_shared_l2_cache_hierarchy import (
    PrivateL1SharedL2CacheHierarchy,
)
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

benchmark_choices = [
    "blackscholes", "bodytrack", "canneal", "dedup", "facesim", "ferret",
    "fluidanimate", "freqmine", "raytrace", "streamcluster", "swaptions",
    "vips", "x264",
]
size_choices = ["simsmall", "simmedium", "simlarge"]
variant_choices = ["lru", "bpc", "zcache", "cdzcache"]

parser = argparse.ArgumentParser(description="PARSEC sweep with BPC/ZCache/CDZCache L2 variants.")
parser.add_argument("--benchmark", required=True, choices=benchmark_choices)
parser.add_argument("--size", required=True, choices=size_choices)
parser.add_argument("--variant", required=True, choices=variant_choices)
parser.add_argument("--max-compression-ratio", type=int, default=2,
                    help="BPC cap; only used by --variant bpc.")
parser.add_argument("--atomic-boot", action="store_true",
                    help="Boot under ATOMIC instead of KVM. Slower, but avoids "
                         "KVM+CompressedTags panic for --variant bpc.")
args = parser.parse_args()

requires(isa_required=ISA.X86, kvm_required=not args.atomic_boot)


class LRUHierarchy(PrivateL1SharedL2CacheHierarchy):
    def incorporate_cache(self, board):
        super().incorporate_cache(board)
        self.l2cache.replacement_policy = LRURP()


class BPCHierarchy(PrivateL1SharedL2CacheHierarchy):
    def incorporate_cache(self, board):
        super().incorporate_cache(board)
        ratio = args.max_compression_ratio
        self.l2cache.tags = CompressedTags(max_compression_ratio=ratio)
        self.l2cache.compressor = BPC(max_compression_ratio=ratio)
        self.l2cache.replacement_policy = LRURP()


class ZCacheHierarchy(PrivateL1SharedL2CacheHierarchy):
    def incorporate_cache(self, board):
        super().incorporate_cache(board)
        self.l2cache.tags = ZCacheTags(
            walk_levels=3,
            num_candidates=16,
            replacement_policy=ZCacheRP(bucket_size_fraction=0.05),
            zcache_indexing_policy=ZCacheIndexingPolicy(),
        )
        self.l2cache.replacement_policy = self.l2cache.tags.replacement_policy


class CDZCacheHierarchy(PrivateL1SharedL2CacheHierarchy):
    def incorporate_cache(self, board):
        super().incorporate_cache(board)
        self.membus.snoop_filter.max_capacity = "32MiB"
        zcache_tags = ZCacheTagsNew(
            walk_levels=3,
            num_candidates=16,
            replacement_policy=ZCacheRP(bucket_size_fraction=0.05),
            zcache_indexing_policy=ZCacheIndexingPolicy(),
        )
        self.l2cache.tags = zcache_tags
        self.l2cache.replacement_policy = zcache_tags.replacement_policy
        # Glue is attached to the board after construction (see below).
        self._zcache_tags = zcache_tags


HIERARCHY = {
    "lru": LRUHierarchy,
    "bpc": BPCHierarchy,
    "zcache": ZCacheHierarchy,
    "cdzcache": CDZCacheHierarchy,
}[args.variant]

cache_hierarchy = HIERARCHY(
    l1d_size="32KiB", l1i_size="32KiB", l2_size="256KiB",
)

memory = DualChannelDDR4_2400(size="3GiB")

processor = SimpleSwitchableProcessor(
    starting_core_type=CPUTypes.ATOMIC if args.atomic_boot else CPUTypes.KVM,
    switch_core_type=CPUTypes.TIMING,
    isa=ISA.X86,
    num_cores=2,
)

board = X86Board(
    clk_freq="3GHz",
    processor=processor,
    memory=memory,
    cache_hierarchy=cache_hierarchy,
)

if args.variant == "cdzcache":
    board.glue = ZCacheGlue(
        zcache_tags=cache_hierarchy._zcache_tags,
        data_store=DecoupledDataStore(),
        reservoir=FragReservoir(sample_interval=10),
        compressor=BPC(max_compression_ratio=0),
    )

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


def handle_workbegin():
    print(f"[{args.variant}] ROI start — switching to TIMING and resetting stats")
    m5.stats.reset()
    processor.switch()
    yield False


def handle_workend():
    print(f"[{args.variant}] ROI end — dumping stats")
    m5.stats.dump()
    yield True


simulator = Simulator(
    board=board,
    on_exit_event={
        ExitEvent.WORKBEGIN: handle_workbegin(),
        ExitEvent.WORKEND: handle_workend(),
    },
)

globalStart = time.time()
print(f"[{args.variant}] booting under KVM, benchmark={args.benchmark} size={args.size}")
m5.stats.reset()
simulator.run()

elapsed = time.time() - globalStart
print(f"[{args.variant}] done — ROI ticks={simulator.get_roi_ticks()[0]}, "
      f"sim_seconds={simulator.get_current_tick() / 1e12:.3f}, "
      f"wallclock={elapsed:.1f}s ({elapsed/60:.1f} min)")
