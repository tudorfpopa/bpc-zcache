"""
check_stats.py — Post-simulation assertion checker for CDZCache functional verification.

Reads m5out_verify_{A,B,C}/stats.txt and asserts expected conditions for each scenario.
No gem5 imports required — plain Python 3.

Usage:
  python3 check_stats.py                  # check all scenarios found
  python3 check_stats.py --scenario A     # check one scenario
  python3 check_stats.py --scenario B
  python3 check_stats.py --scenario C

Exit code: 0 = all checked scenarios passed, 1 = any failure.
"""

import argparse
import math
import os
import sys

SCRIPT_DIR = os.path.dirname(os.path.abspath(__file__))

SCENARIO_OUTDIRS = {
    "A": os.path.join(SCRIPT_DIR, "m5out_verify_A"),
    "B": os.path.join(SCRIPT_DIR, "m5out_verify_B"),
    "C": os.path.join(SCRIPT_DIR, "m5out_verify_C"),
}

# ── Stats parser ──────────────────────────────────────────────────────────────

def parse_stats(stats_path):
    """Return dict: stat_name -> float.  'nan' in file becomes float('nan')."""
    stats = {}
    with open(stats_path) as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#") or line.startswith("---"):
                continue
            parts = line.split()
            if len(parts) < 2:
                continue
            key = parts[0]
            try:
                stats[key] = float(parts[1])
            except ValueError:
                stats[key] = float("nan")
    return stats


def get(stats, key):
    """Return stat value or float('nan') with a friendly KeyError on miss."""
    if key not in stats:
        raise AssertionError(f"Stat not found in stats.txt: {key}")
    return stats[key]


# ── Per-scenario checkers ─────────────────────────────────────────────────────

def check_scenario_A(stats):
    """
    Scenario A: Normal CDZCache — BPC granularity + baseline correctness.
    pool_bytes=256KiB, sweep=320KiB, cache=256KiB/4-way.
    Zero-filled data → all planes ALL_ZEROS → 96 bits raw → pads to 128 bits (16B).
    Pool never exhausted → no fragmentation evictions.
    """
    hits   = int(get(stats, "system.l2cache.overallHits::total"))
    misses = int(get(stats, "system.l2cache.overallMisses::total"))
    fills  = int(get(stats, "system.glue.statFillsWithData"))
    padded = int(get(stats, "system.glue.statTotalPaddedBytes"))
    raw    = int(get(stats, "system.glue.statTotalRawBytes"))
    frag_evictions  = int(get(stats, "system.glue.statFragEvictions"))
    res_requests    = int(get(stats, "system.glue.statReservoirRequests"))
    res_hits        = int(get(stats, "system.glue.statReservoirHits"))
    res_hit_rate    = get(stats, "system.glue.statReservoirHitRate")
    avg_frag        = get(stats, "system.glue.statAvgInternalFragmentation")
    relocations     = get(stats, "system.glue.zcache_tags.statRelocationsTotal")
    walk_depth_avg  = get(stats, "system.glue.zcache_tags.statWalkDepthAverage")

    # Traffic: 320KiB / 64B = 5120 blocks × 2 passes = 10240 total accesses
    assert hits + misses == 10240, \
        f"Total accesses should be 10240, got hits={hits} + misses={misses} = {hits+misses}"

    # ZCache cuckoo is deterministic for this fixed workload + config
    assert hits == 467, \
        f"Expected 467 hits (ZCache reference), got {hits}"
    assert misses == 9773, \
        f"Expected 9773 misses, got {misses}"

    # Every cache miss results in a fill with data
    assert fills == misses, \
        f"statFillsWithData ({fills}) should equal overallMisses ({misses})"

    # BPC granularity: zero-filled data → 32×3 = 96 bits = 12B raw → pads to 16B
    assert padded == misses * 16, \
        f"Expected statTotalPaddedBytes={misses * 16} (misses×16B), got {padded}"
    assert padded % 8 == 0, \
        f"statTotalPaddedBytes={padded} must be a multiple of 8 (buddy granularity)"

    # BPC pads before setSizeBits(), so glue-level rawBytes == paddedBytes
    assert raw == padded, \
        f"statTotalRawBytes ({raw}) should equal statTotalPaddedBytes ({padded})"

    # Formula stat: (paddedBytes - rawBytes) / paddedBytes = 0
    assert avg_frag == 0.0, \
        f"statAvgInternalFragmentation should be 0.0, got {avg_frag}"

    # Pool not exhausted → no evictions or reservoir calls
    assert frag_evictions == 0, \
        f"Expected 0 frag evictions (pool never exhausted), got {frag_evictions}"
    assert res_requests == 0, \
        f"Expected 0 reservoir requests, got {res_requests}"
    assert res_hits == 0, \
        f"Expected 0 reservoir hits, got {res_hits}"

    # Formula stat: 0/0 = NaN when reservoir never called
    assert math.isnan(res_hit_rate), \
        f"statReservoirHitRate should be NaN (0/0), got {res_hit_rate}"

    # Cuckoo is active (ZCache relocation working)
    assert relocations > 0, \
        f"Expected > 0 cuckoo relocations, got {relocations}"
    assert walk_depth_avg >= 1.0, \
        f"Expected walk depth average >= 1.0, got {walk_depth_avg}"


def check_scenario_B(stats):
    """
    Scenario B: Small Pool / Fragmentation — reservoir eviction path.
    pool_bytes=64KiB = 4096 × 16B slots.  After ~4096 fills the pool is full;
    every subsequent fill triggers ALLOC_FAIL → FragReservoir → eviction.
    """
    hits   = int(get(stats, "system.l2cache.overallHits::total"))
    misses = int(get(stats, "system.l2cache.overallMisses::total"))
    fills  = int(get(stats, "system.glue.statFillsWithData"))
    padded = int(get(stats, "system.glue.statTotalPaddedBytes"))
    raw    = int(get(stats, "system.glue.statTotalRawBytes"))
    frag_evictions = int(get(stats, "system.glue.statFragEvictions"))
    res_requests   = int(get(stats, "system.glue.statReservoirRequests"))
    res_hits       = int(get(stats, "system.glue.statReservoirHits"))
    res_hit_rate   = get(stats, "system.glue.statReservoirHitRate")
    avg_frag       = get(stats, "system.glue.statAvgInternalFragmentation")
    relocations    = get(stats, "system.glue.zcache_tags.statRelocationsTotal")
    walk_depth_avg = get(stats, "system.glue.zcache_tags.statWalkDepthAverage")

    # Traffic: 320KiB / 64B × 2 passes = 10240 total
    assert hits + misses == 10240, \
        f"Total accesses should be 10240, got {hits}+{misses}={hits+misses}"

    # Fills > 0 (simulation ran and processed cache misses)
    assert fills > 0, \
        f"Expected fills > 0, got {fills}"

    # BPC granularity invariant (must hold regardless of evictions)
    assert padded % 8 == 0, \
        f"statTotalPaddedBytes={padded} must be a multiple of 8 (buddy granularity)"

    # BPC pads internally, so glue raw == padded
    assert raw == padded, \
        f"statTotalRawBytes ({raw}) should equal statTotalPaddedBytes ({padded})"

    # Formula stat = 0 since raw==padded
    assert avg_frag == 0.0, \
        f"statAvgInternalFragmentation should be 0.0, got {avg_frag}"

    # Primary assertion: fragmentation eviction path was exercised
    assert frag_evictions > 0, \
        f"Expected > 0 frag evictions (pool too small → ALLOC_FAIL), got {frag_evictions}"

    # Reservoir was queried and returned at least one valid candidate
    assert res_requests > 0, \
        f"Expected > 0 reservoir requests, got {res_requests}"
    assert res_hits > 0, \
        f"Expected > 0 reservoir hits, got {res_hits}"

    # Hit rate is a proper ratio (denominator > 0, value in (0, 1])
    assert not math.isnan(res_hit_rate), \
        f"statReservoirHitRate should not be NaN (reservoir was called)"
    assert 0.0 < res_hit_rate <= 1.0, \
        f"statReservoirHitRate={res_hit_rate:.4f} should be in (0, 1]"

    # Every eviction is caused by a fill → fills >= evictions
    assert fills >= frag_evictions, \
        f"statFillsWithData ({fills}) must be >= statFragEvictions ({frag_evictions})"

    # Cuckoo is still active
    assert relocations > 0, \
        f"Expected > 0 cuckoo relocations, got {relocations}"
    assert walk_depth_avg >= 1.0, \
        f"Expected walk depth average >= 1.0, got {walk_depth_avg}"


def check_scenario_C(stats):
    """
    Scenario C: ZCache Cuckoo Stress — doMoveBlock metadata migration path.
    pool_bytes=256KiB, sweep=640KiB (2.5x), cache=256KiB/4-way.
    Heavy oversubscription forces many more cuckoo chains than Scenario A.
    Pool never exhausted (256KiB / 16B = 16384 slots > 10240 blocks).
    """
    hits   = int(get(stats, "system.l2cache.overallHits::total"))
    misses = int(get(stats, "system.l2cache.overallMisses::total"))
    fills  = int(get(stats, "system.glue.statFillsWithData"))
    padded = int(get(stats, "system.glue.statTotalPaddedBytes"))
    raw    = int(get(stats, "system.glue.statTotalRawBytes"))
    frag_evictions = int(get(stats, "system.glue.statFragEvictions"))
    res_requests   = int(get(stats, "system.glue.statReservoirRequests"))
    res_hit_rate   = get(stats, "system.glue.statReservoirHitRate")
    avg_frag       = get(stats, "system.glue.statAvgInternalFragmentation")
    relocations    = get(stats, "system.glue.zcache_tags.statRelocationsTotal")
    walk_depth_avg = get(stats, "system.glue.zcache_tags.statWalkDepthAverage")

    # Traffic: 640KiB / 64B × 2 passes = 20480 total
    assert hits + misses == 20480, \
        f"Total accesses should be 20480, got {hits}+{misses}={hits+misses}"

    # At 2.5x oversubscription, misses dominate
    assert misses > hits, \
        f"Expected misses ({misses}) > hits ({hits}) at 2.5x oversubscription"

    # Cross-layer invariant: every miss generates a fill with data
    assert fills == misses, \
        f"statFillsWithData ({fills}) should equal overallMisses ({misses}) — " \
        f"divergence indicates insertBlock is skipping the glue callback or " \
        f"doMoveBlock corrupted metadata causing early handleFill returns"

    # BPC granularity (still must hold — sizeToOrder() panics on non-{8,16,32,64})
    assert padded % 8 == 0, \
        f"statTotalPaddedBytes={padded} must be a multiple of 8 (buddy granularity)"

    # BPC pads internally → raw == padded
    assert raw == padded, \
        f"statTotalRawBytes ({raw}) should equal statTotalPaddedBytes ({padded})"

    # Formula stat = 0
    assert avg_frag == 0.0, \
        f"statAvgInternalFragmentation should be 0.0, got {avg_frag}"

    # Pool not exhausted (256KiB / 16B = 16384 slots, we fill at most ~10240 blocks)
    assert frag_evictions == 0, \
        f"Expected 0 frag evictions (pool has headroom), got {frag_evictions}"
    assert res_requests == 0, \
        f"Expected 0 reservoir requests, got {res_requests}"
    assert math.isnan(res_hit_rate), \
        f"statReservoirHitRate should be NaN (0/0), got {res_hit_rate}"

    # Cuckoo must be MORE active than Scenario A (ref: 4853 relocations)
    # If doMoveBlock silently drops metadata, the simulation still runs but
    # pool slots leak, visible as statTotalPaddedBytes != misses*16.
    assert relocations > 4853, \
        f"Expected > 4853 cuckoo relocations (more than Scenario A), got {relocations}"
    assert walk_depth_avg >= 1.0, \
        f"Expected walk depth average >= 1.0, got {walk_depth_avg}"

    # Additional: padded == misses * 16 confirms no pool slot leaks from
    # doMoveBlock corruption (each fill allocates exactly one 16B slot)
    assert padded == misses * 16, \
        f"Expected statTotalPaddedBytes={misses * 16} (misses×16B), got {padded}. " \
        f"Mismatch may indicate doMoveBlock is dropping blkDataPtr metadata."


# ── Main ──────────────────────────────────────────────────────────────────────

CHECKERS = {
    "A": check_scenario_A,
    "B": check_scenario_B,
    "C": check_scenario_C,
}

LABELS = {
    "A": "Normal CDZCache (BPC granularity path)",
    "B": "Small Pool / Fragmentation (reservoir eviction path)",
    "C": "ZCache Cuckoo Stress (doMoveBlock migration path)",
}


def main():
    parser = argparse.ArgumentParser(
        description="Assert expected CDZCache stats for functional verification scenarios.")
    parser.add_argument("--scenario", choices=["A", "B", "C"],
                        help="Check only this scenario (default: all found)")
    args = parser.parse_args()

    scenarios_to_check = [args.scenario] if args.scenario else ["A", "B", "C"]
    failed  = []
    skipped = []

    for s in scenarios_to_check:
        outdir     = SCENARIO_OUTDIRS[s]
        stats_path = os.path.join(outdir, "stats.txt")
        label      = LABELS[s]

        if not os.path.exists(stats_path):
            print(f"[SKIP] Scenario {s} ({label}): {stats_path} not found — run the sim first")
            skipped.append(s)
            continue

        print(f"[CHECK] Scenario {s}: {label}")
        try:
            stats = parse_stats(stats_path)
            CHECKERS[s](stats)
            print(f"  [PASS] Scenario {s}")
        except AssertionError as e:
            print(f"  [FAIL] Scenario {s}: {e}")
            failed.append(s)
        except Exception as e:
            print(f"  [ERROR] Scenario {s}: unexpected error — {e}")
            failed.append(s)

    print()
    if skipped:
        print(f"Skipped (stats.txt not found): {skipped}")
    if failed:
        print(f"FAILED scenarios: {failed}")
        sys.exit(1)
    elif skipped and not (set(scenarios_to_check) - set(skipped)):
        print("No scenarios could be checked (all skipped).")
        sys.exit(0)
    else:
        checked = [s for s in scenarios_to_check if s not in skipped]
        print(f"All checked scenarios PASSED: {checked}")


if __name__ == "__main__":
    main()
