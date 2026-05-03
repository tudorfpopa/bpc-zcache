# bpc-zcache

Research project on bit-plane compression caches (BPC) and ZCaches, simulated with [gem5](https://www.gem5.org). We do not fork gem5; we keep our sources outside the gem5 tree and overlay them into the right `gem5/src/...` directories with `init.sh`.

## Repo layout

- `gem5/` — pristine gem5 submodule. Do **not** keep edits here; they are not tracked and will be clobbered the next time `init.sh` runs.
- `bpc/` — BPC compressor: `bpc.{hh,cc,test.cc}`, `Compressors.py` (SimObject registration), `SConscript`, plus `compressed_tags.cc` (a patched copy of gem5's file — see "Gotchas").
- `zcache/` — ZCache tags / indexing policy / replacement policy, with their `Sconscript` and Python registration files.
- `glue/` — **Compressed Decoupled ZCache** glue layer (new). See "Compressed Decoupled ZCache" section below.
- `init.sh` — copies everything in `bpc/`, `zcache/`, and `glue/` into the matching gem5 source directories.
- `run_bpc_sanity.py` — gem5 stdlib config that runs an x86 hello-world with BPC in the L2.
- `bpc_test.py`, `zcache_test.py` — CPU-less micro-benchmarks (`PyTrafficGen` → `NoncoherentCache` → `IOXBar` → `SimpleMemory`). Same workload (320 KiB linear sweep, twice). `bpc_test.py` swaps in `CompressedTags + BPC`; `zcache_test.py` swaps in `ZCacheTags`. Each takes `--use-standard-cache` for an LRU baseline.
- `x86_boot_checkpoint.py` — full-system x86 checkpoint helper.
- `m5out_*/` — per-run output directories (one per script + variant). See "Output directories" below.

Anything you want git to track must live in `bpc/`, `zcache/`, or the repo root — not under `gem5/`.

## Build & run

```bash
./init.sh                                          # overlay sources into gem5/
cd gem5
scons build/X86/gem5.opt -j$(sysctl -n hw.ncpu)    # ~30 min from clean
cd ..
./gem5/build/X86/gem5.opt run_bpc_sanity.py
```

Re-run `./init.sh` whenever you edit anything under `bpc/` or `zcache/`. Then re-build (scons only recompiles what changed).

Paths inside `run_bpc_sanity.py` (e.g. the hello binary) are relative to gem5's checkout root, so either `cd gem5 && ./build/X86/gem5.opt ../run_bpc_sanity.py` or use absolute paths in the config.

## Gotchas

- **`SConscript` capitalization.** scons looks for `SConscript` (capital C). Some files in this repo were originally named `Sconscript`; on macOS's case-insensitive APFS that silently breaks the overlay (the gem5 `SConscript` is not actually replaced, so our SimObjects are never registered, and you get errors like `'params/ZCacheIndexingPolicy.hh' file not found` at build time). Keep the capital `C`.
- **Read-only overlays.** gem5 marks copied files read-only after the first overlay. `init.sh` uses `cp -f` so subsequent overlays succeed; without `-f` you get `Permission denied`.
- **Upstream gem5 25.1.0.1 bug — patched.** `gem5/src/mem/cache/tags/compressed_tags.cc::tagsInit()` builds the superblock / sub-block arrays but forgets to call `registerTagExtractor` on them, the way `SectorTags::tagsInit` does. The first `match()` (which fires during binary load via a functional access through the L2) then asserts on `extractTag` being null:
  ```
  Assertion failed: (extractTag), function match, file tagged_entry.hh, line 159.
  ```
  Our fix lives at `bpc/compressed_tags.cc` and is overlaid into gem5 by `init.sh`. If a future gem5 bump fixes this upstream, drop our copy.

## Output directories

gem5 writes stats/config to its CWD's `m5out/` by default. If you `cd gem5` to launch the simulator, results land in `gem5/m5out/` — inside the submodule, where they're easy to lose. Two ways to keep output in the repo root:

1. **Pass `--outdir` to `gem5.opt`** (before the script path), e.g. `./gem5/build/X86/gem5.opt --outdir=m5out_bpc bpc_test.py`.
2. **Anchor outdir in the script.** `bpc_test.py` does this — it sets `m5.options.outdir` and `m5.core.setOutputDir()` to `<script_dir>/m5out_bpc` (or `m5out_lru` with `--use-standard-cache`) on startup, so output goes to the repo root regardless of CWD. The script's `--outdir` flag overrides this; relative paths there resolve against the script directory, not gem5's CWD. Worth copying this pattern into other configs.

Note: gem5 still creates an empty `m5out/stats.txt` in CWD before the script runs (the redirect happens inside the Python script). It's harmless — the real output is in the redirected dir — and it's easy to `rm -rf gem5/m5out` if it bothers you.

## Micro-benchmark caveat

The `bpc_test.py` / `zcache_test.py` workload uses `PyTrafficGen` reading from `SimpleMemory`, which returns zero-filled lines. So:

- **BPC** trivially compresses everything (all-zero planes), the 320 KiB footprint fits in 256 KiB, and the second pass becomes a pure hit (50% overall hit rate). This validates the wiring; it does **not** characterise BPC on real workloads.
- **ZCache** (~4.6% hit rate on the same sweep) is exercising its actual mechanism — H3 placement + cuckoo relocation against a hard-aliasing access pattern.
- **LRU baseline** thrashes completely (0 hits / 10 240 misses).

Reference numbers from the current scripts (256 KiB, 4-way, 320 KiB sweep ×2):

| Cache | Hits | Misses | Hit rate | Notes |
| --- | ---: | ---: | ---: | --- |
| LRU | 0 | 10 240 | 0% | 5-way aliasing per set, complete thrash |
| ZCache (L=3, R=16) | 467 | 9 773 | 4.6% | Real mechanism gain |
| BPC (max ratio = 2) | 5 120 | 5 120 | 50% | Workload artifact — zero-filled memory |

Don't quote the BPC number as a performance result without a content-aware traffic source (e.g. a real binary trace or a generator that writes structured data before reading).

## BPC

### What it is
Bit-Plane Compression. A 64-byte cache line is viewed as 16 × 32-bit words. For each of the 32 bit positions we extract one bit from each word, giving a 16-bit "plane". Adjacent planes tend to look alike (long runs of zeros, long runs of ones, or near-duplicates of the previous plane), so each plane is tagged with one of:

```
ALL_ZEROS | ALL_ONES | SAME_AS_PREV | COMP_OF_PREV | UNCOMPRESSED
```

Only `UNCOMPRESSED` planes contribute raw bits; the rest cost only their tag.

### Files

| File | Role |
| --- | --- |
| `bpc/bpc.hh` | `gem5::compression::BPC` class, `BPCData` payload, `PlaneEncoding` enum |
| `bpc/bpc.cc` | `compress()` / `decompress()` implementation |
| `bpc/bpc.test.cc` | gtest unit tests |
| `bpc/Compressors.py` | SimObject `class BPC(BaseCacheCompressor)` — `type='BPC'`, `cxx_class='gem5::compression::BPC'`, `cxx_header='mem/cache/compressors/bpc.hh'` |
| `bpc/SConscript` | Adds `bpc.cc` to the build and registers the `BPC` SimObject |
| `bpc/compressed_tags.cc` | Patched gem5 file (see Gotchas) |

After `init.sh`, all of these land in `gem5/src/mem/cache/compressors/` (except `compressed_tags.cc`, which goes to `gem5/src/mem/cache/tags/`). The build picks them up via the overlaid `SConscript`, and `from m5.objects import BPC` then works in any gem5 config.

### Wiring into a cache

BPC requires `CompressedTags` (which is `SectorTags` plus per-block compression metadata). A plain `BaseSetAssoc` will not work. The pattern in `run_bpc_sanity.py`:

```python
from m5.objects import CompressedTags, BPC, LRURP
from gem5.components.cachehierarchies.classic.private_l1_shared_l2_cache_hierarchy \
    import PrivateL1SharedL2CacheHierarchy

class BPCClassicCacheHierarchy(PrivateL1SharedL2CacheHierarchy):
    def incorporate_cache(self, board):
        super().incorporate_cache(board)
        self.l2cache.tags = CompressedTags(max_compression_ratio=2)
        self.l2cache.compressor = BPC()
        self.l2cache.replacement_policy = LRURP()
```

`max_compression_ratio=2` means each superblock holds up to 2 sub-blocks, so a perfectly compressed line costs half a tag's worth of data space.

### Path through gem5

- **Fill**: `BaseCache` calls `compressor->compress()` to size the new line, then `CompressedTags::findVictim()` decides whether the (possibly smaller) block can co-allocate inside an existing superblock or evicts one.
- **Hit**: the stored `BPCData` is handed to `decompress()`, which rebuilds the 64-byte line from the per-plane tags + raw planes.
- **Latency**: `comp_extra_latency` / `decomp_extra_latency` come from `BaseCacheCompressor` defaults; override on the `BPC()` SimObject if you want different numbers.

### `max_compression_ratio` (cap)

`BPC` exposes a `max_compression_ratio` Python parameter that clamps the *reported* compressed size to a floor of `blkSize / max_compression_ratio` (in bits, computed in `bpc/bpc.cc`). The actual stored `BPCData` is unaffected — `decompress()` still reconstructs the line from the real per-plane tags. The cap only changes the size the cache uses for fit / eviction accounting in `CompressedTags`.

- **Default `0` = uncapped** → BPC reports its real compressed size. Backward-compatible with the original implementation.
- **`N ≥ 1`** → reported size is at least `blkSize / N` bits. Set this to match `CompressedTags(max_compression_ratio=N)` so storage and reported size agree.

`bpc_test.py` threads its `--max-compression-ratio` flag to both sides at once. Sweep on the 320 KiB / 256 KiB workload (zero-filled memory caveat still applies):

| `--max-compression-ratio` | Hits | Misses | Notes |
| ---: | ---: | ---: | --- |
| 0 (uncapped) | 5 120 | 5 120 | BPC reports tiny size, tags = default 2/superblock |
| 1 | 0 | 10 240 | 1:1 cap ⇒ no compression ⇒ same as plain LRU |
| 2 | 5 120 | 5 120 | 2 sub-blocks/superblock — fits the 320 KiB sweep |
| 4 | 5 120 | 5 120 | More slack, but tag count limits effective capacity |
| 8 | 2 560 | 7 680 | Aggressive sub-blocking shrinks the tag array further |

## ZCache

### What it is
A skew-associative cache that gets effectively higher associativity from a small number of physical ways via three pieces working together:

1. **Per-way H3 universal hashing** — each of the W ways uses an independent H3 hash, so a single address maps to W different sets (one per way). Adjacent-set conflicts in one way scatter randomly across the others.
2. **Multi-level BFS walk** — on a miss, expand a candidate pool by walking from the W Level-1 slots through their *alternative locations* in other ways. Depth `L` (`walk_levels`) and total candidates capped at `R` (`num_candidates`).
3. **Cuckoo relocation** — pick the victim from the entire pool (not just the Level-1 slots), then shift the chain of intermediate blocks to free a Level-1 slot for the incoming line.

Net effect: for a workload that aliases hard in a regular set-associative cache (e.g., a 5/4-capacity sweep with stride = set-stride), ZCache rescues a meaningful fraction of the hits a 4-way LRU loses entirely to thrashing.

### Files

| File | Role |
| --- | --- |
| `zcache/zcache_index.{hh,cc}` | `ZCacheIndexingPolicy` — H3 matrices, `extractSet(addr, way)`, `getAlternativeLocations(curWay, addr)` |
| `zcache/zcache_rp.{hh,cc}` | `ZCacheRP` — global bucketed-LRU comparable across sets (a per-set LRU timestamp would not be) |
| `zcache/zcache_tags.{hh,cc}` | `ZCacheTags` — overrides `tagsInit/findBlock/accessBlock/findVictim/insertBlock/regenerateBlkAddr/findBlockBySetAndWay` |
| `zcache/IndexingPolicies.py` | Registers `ZCacheIndexingPolicy` (params: `size`, `entry_size`, `h3_seed`) |
| `zcache/ReplacementPolicies.py` | Registers `ZCacheRP` (param: `bucket_size_fraction`) |
| `zcache/Tags.py` | Registers `ZCacheTags` (params: `zcache_indexing_policy`, `walk_levels`, `num_candidates`; default RP = `ZCacheRP`) |
| `zcache/index/SConscript`, `zcache/repl/SConscript`, `zcache/tags/SConscript` | Adds the `.cc` files and registers the SimObjects |

After `init.sh` these land in `gem5/src/mem/cache/tags/indexing_policies/`, `gem5/src/mem/cache/replacement_policies/`, and `gem5/src/mem/cache/tags/` respectively.

### Key design points (from `ai_work.txt`)

- **Idempotent tag encoding.** `extractTag(addr) = addr & ~blkMask` (block-aligned address), *not* the usual `addr >> tagShift`. Required because `CacheBlk::operator=(CacheBlk&&)` calls `insert({other.getTag(), secure})` — i.e., it re-applies `extractTag` on a value already produced by `extractTag`. The block-aligned scheme is idempotent under this; the shift scheme would corrupt the tag on every move.
- **Free-slot preference in `findVictim`.** Before calling `getVictim()`, scan the candidate pool for any invalid slot. Without this, freshly loaded blocks share `lastTouchTimestamp = 0` with empty slots and the RP can't tell them apart, so warm-up causes spurious evictions.
- **Split between `findVictim` and `insertBlock`.** `findVictim` records the cuckoo chain into `pendingSwapChain` and returns the Level-1 root slot. `insertBlock` executes the chain *after* `Cache.cc` has invalidated the victim — required for writeback safety.
- **`doMoveBlock` copies data explicitly.** `BaseTags::moveBlock` (via `CacheBlk::operator=(&&)`) only moves metadata; the data array must be `memcpy`'d separately ("an entry cannot move its data").
- **C++ name-hiding fix in `ZCacheRP`.** `using Base::touch; using Base::reset;` brings the two-arg PacketPtr wrappers back into scope alongside the overridden one-arg versions.
- **Stats idiom.** `statistics::Average` has no `.sample()` method, so per-event averages are done as `Scalar` accumulators paired with a `Formula` ratio — see `statWalkDepthAverage` and `statCandidatesEvaluatedAverage`.

### Wiring into a cache

The teammate's test (`zcache_test.py` at the repo root) attaches `ZCacheTags` to a `NoncoherentCache` driven by `PyTrafficGen` — no CPU, no ISA. The relevant snippet:

```python
from m5.objects import NoncoherentCache, ZCacheTags, ZCacheRP

system.l2cache = NoncoherentCache(
    size="256KiB", assoc=4,
    tag_latency=4, data_latency=4, response_latency=4,
    mshrs=16, tgts_per_mshr=8,
    tags=ZCacheTags(
        walk_levels=3,
        num_candidates=16,
        replacement_policy=ZCacheRP(bucket_size_fraction=0.05),
    ),
)
```

The same pattern works for any `Cache` / `NoncoherentCache` — drop `tags=ZCacheTags(...)` in. Knobs:

- `walk_levels` (L) — BFS depth.
- `num_candidates` (R) — cap on total candidates evaluated by the RP per miss.
- `bucket_size_fraction` — granularity of the global LRU bucket; smaller = finer-grained victim ranking, larger = more "ties" the RP can break by other criteria.

### Running the test

The test is CPU-less, so the X86 build runs it without modification:

```bash
./gem5/build/X86/gem5.opt --outdir=m5out_zcache zcache_test.py
./gem5/build/X86/gem5.opt --outdir=m5out_lru   zcache_test.py --use-standard-cache
```

Stats to compare in `stats.txt`:
- `system.l2cache.overallHits::total` / `overallMisses::total`
- `system.l2cache.tags.statWalkDepthAverage`
- `system.l2cache.tags.statCandidatesEvaluatedAverage`
- `system.l2cache.tags.statRelocationsTotal`

Reference numbers on the 320 KiB sweep into a 256 KiB / 4-way cache: ZCache (L=3, R=16) gets 467 hits / 9 773 misses; standard 4-way LRU thrashes completely (0 / 10 240).

---

## Compressed Decoupled ZCache (CDZCache)

### Goal

Combines ZCache's logical associativity with BPC compression and a buddy-allocated data store to maximise effective LLC capacity while minimising conflict misses.

### Architecture

```
CPU / Traffic Generator
        │
        ▼
NoncoherentCache / Cache
  tags = ZCacheTagsNew  ──(on every fill)──▶ ZCacheGlue
                                                  │
                         ┌────────────────────────┤
                         │                        │
                   DecoupledDataStore       FragReservoir
                   (buddy allocator)        (LFSR + best-of-4
                                             reservoir)
```

### New files (`glue/`)

| File | Role |
| --- | --- |
| `glue/decoupled_data_store.hh/.cc` | `DecoupledDataStore` SimObject — buddy allocator (8 B segments, 8/16/32/64 B blocks, coalescing on free) |
| `glue/frag_reservoir.hh/.cc` | `FragReservoir` ClockedObject — 16-bit LFSR sampler, 4 bins × 4 slots (best-of-4 LRU-ish), self-rescheduling gem5 Event every N cycles |
| `glue/zcache_tags_new.hh/.cc` | `ZCacheTagsNew` — extends `ZCacheTags` with per-block `(dataPtr, allocSize)` parallel arrays; adds `forceInvalidate()` (bypasses cuckoo walk); overrides `doMoveBlock()` to also migrate compressed metadata during cuckoo chains |
| `glue/zcache_glue.hh/.cc` | `ZCacheGlue` SimObject — coordinates compress → allocate → frag-evict loop → store on every `insertBlock`; exposes `totalFragEvictions`, `avgInternalFragmentation`, `reservoirHitRate` stats |
| `glue/ZCacheGlue.py` | Python SimObject registrations for all four classes above |
| `glue/SConscript` | Reference (contents merged into `zcache/tags/SConscript` by maintainer) |

### Modifications to existing files

| File | Change |
| --- | --- |
| `zcache/zcache_tags.hh` | `private:` → `protected:` so `ZCacheTagsNew` can access `zcacheIP`, `zcacheRP`, walk params, `pendingSwapChain`, `getBlkAt()`; `doMoveBlock` made `virtual` for subclass override |
| `zcache/zcache_rp.hh` | Added `getLastTouchTimestamp(rd)` public accessor (reads `ZCacheRPData::lastTouchTimestamp` for FragReservoir without exposing the protected struct) |
| `zcache/tags/SConscript` | Registers `ZCacheGlue.py` SimObjects and adds the four glue `.cc` sources |
| `init.sh` | Copies `glue/*.hh`, `glue/*.cc`, `glue/ZCacheGlue.py` to `gem5/src/mem/cache/tags/` |

### Fill flow (ZCacheGlue::handleFill)

1. **Compress** — call `BPC::compress(pkt_chunks)` → get `compressedBits`; round up to `paddedSize` ∈ {8, 16, 32, 64} B.
2. **Allocate** — `DecoupledDataStore::allocate(paddedSize)`.
3. **Fragment-evict loop** — while `ptr == ALLOC_FAIL`: call `FragReservoir::getVictim(paddedSize)`; call `ZCacheTagsNew::forceInvalidate(victim)` + `DecoupledDataStore::deallocate(victim->ptr, victim->size)`; retry allocate.
4. **Store** — `DecoupledDataStore::write(ptr, data, paddedSize)`; call `ZCacheTagsNew::setBlockDataPtr(blk, ptr, paddedSize)`.

### Stats

```
system.glue.statFragEvictions           — reservoir-eviction events
system.glue.statAvgInternalFragmentation — (paddedSize - rawSize) / paddedSize
system.glue.statReservoirHitRate        — fraction of reservoir lookups satisfied
```

### Running the test

```bash
./init.sh
cd gem5 && scons build/X86/gem5.opt -j$(sysctl -n hw.ncpu) && cd ..

# CDZCache
./gem5/build/X86/gem5.opt --outdir=m5out_cdzcache cdzcache_test.py

# LRU baseline
./gem5/build/X86/gem5.opt --outdir=m5out_lru cdzcache_test.py --use-standard-cache
```

### Wiring into a config script

```python
from m5.objects import (
    NoncoherentCache, ZCacheTagsNew, ZCacheGlue,
    DecoupledDataStore, FragReservoir,
    ZCacheIndexingPolicy, ZCacheRP, BPC,
)

zcache_tags = ZCacheTagsNew(
    walk_levels=3, num_candidates=16,
    replacement_policy=ZCacheRP(bucket_size_fraction=0.05),
    zcache_indexing_policy=ZCacheIndexingPolicy(),
)
bpc = BPC(max_compression_ratio=0)

system.glue = ZCacheGlue(
    zcache_tags=zcache_tags,
    data_store=DecoupledDataStore(),
    reservoir=FragReservoir(sample_interval=10),
    compressor=bpc,
)

system.l2cache = NoncoherentCache(
    size="256KiB", assoc=4,
    tag_latency=4, data_latency=4, response_latency=4,
    mshrs=16, tgts_per_mshr=8,
    tags=zcache_tags,
)
```

`ZCacheGlue.startup()` calls `zcache_tags->setGlue(this)` and `reservoir->setTagsRef(zcache_tags)` so the circular wiring completes before simulation starts.
