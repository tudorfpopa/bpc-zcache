from gem5.components.boards.x86_board import X86Board
from gem5.components.memory import SingleChannelDDR3_1600
from gem5.components.processors.simple_processor import SimpleProcessor
from gem5.components.processors.cpu_types import CPUTypes
from gem5.isas import ISA
from gem5.resources.resource import BinaryResource
from gem5.simulate.simulator import Simulator

from m5.objects import CompressedTags, BPC, LRURP
from gem5.components.cachehierarchies.classic.private_l1_shared_l2_cache_hierarchy import PrivateL1SharedL2CacheHierarchy

# Custom hierarchy to inject BPC into L2
class BPCClassicCacheHierarchy(PrivateL1SharedL2CacheHierarchy):
    def __init__(self):
        super().__init__(
            l1d_size="32KiB",
            l1i_size="32KiB",
            l2_size="256KiB",
            l1d_assoc=8,
            l1i_assoc=8,
            l2_assoc=16,
        )

    def incorporate_cache(self, board):
        super().incorporate_cache(board)
        # After super().incorporate_cache(board), self.l2cache is created
        # We override its tags and compressor
        # BPC requires CompressedTags
        self.l2cache.tags = CompressedTags(max_compression_ratio=2)
        self.l2cache.compressor = BPC()
        # LRURP is the default, but we can be explicit
        self.l2cache.replacement_policy = LRURP()

# 1. Setup the processor (Dual core X86 Timing CPU)
processor = SimpleProcessor(cpu_type=CPUTypes.TIMING, isa=ISA.X86, num_cores=2)

# 2. Setup the cache hierarchy
cache_hierarchy = BPCClassicCacheHierarchy()

# 3. Setup the memory
memory = SingleChannelDDR3_1600(size="2GiB")

# 4. Setup the board
board = X86Board(
    clk_freq="3GHz",
    processor=processor,
    memory=memory,
    cache_hierarchy=cache_hierarchy,
)

# 5. Setup the workload (using a local hello binary as the online client is failing)
local_hello = BinaryResource(
    local_path="tests/test-progs/hello/bin/x86/linux/hello"
)
board.set_se_binary_workload(local_hello)



# 6. Run the simulation
simulator = Simulator(board=board)
print("Starting simulation with BPC L2 Cache...")
simulator.run()
print("Simulation finished.")
