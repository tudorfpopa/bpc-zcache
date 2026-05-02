import m5
import os
from gem5.components.boards.x86_board import X86Board
from gem5.components.memory.single_channel import SingleChannelDDR4_2400
from gem5.components.processors.simple_processor import SimpleProcessor
from gem5.components.processors.cpu_types import CPUTypes
from gem5.components.cachehierarchies.classic.private_l1_private_l2_cache_hierarchy import (
    PrivateL1PrivateL2CacheHierarchy,
)
from gem5.isas import ISA
from gem5.resources.resource import obtain_resource
from gem5.simulate.simulator import Simulator

cache_hierarchy = PrivateL1PrivateL2CacheHierarchy(
    l1d_size="256KiB",
    l1i_size="256KiB",
    l2_size="2MiB",
)

memory = SingleChannelDDR4_2400(size="3GiB")

processor = SimpleProcessor(
    cpu_type=CPUTypes.ATOMIC,
    isa=ISA.X86,
    num_cores=2,
)

board = X86Board(
    clk_freq="3.5GHz",
    processor=processor,
    memory=memory,
    cache_hierarchy=cache_hierarchy,
)

board.set_workload(obtain_resource("x86-ubuntu-24.04-boot-no-systemd"))

checkpoint_dir = os.path.join(os.getcwd(), "checkpoints", "linux_boot")
os.makedirs(checkpoint_dir, exist_ok=True)

simulator = Simulator(board=board)

print("==> Starting simulation...")
simulator.run()

print("==> Boot complete — saving checkpoint...")
m5.checkpoint(checkpoint_dir)

print(f"\n==> Checkpoint saved to: {checkpoint_dir}")
print(f"==> Files: {os.listdir(checkpoint_dir)}")