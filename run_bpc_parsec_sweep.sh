#!/usr/bin/env bash
# Run the BPC L2 variant across PARSEC benchmarks.
# Priority group runs first; remaining benchmarks run after.
# Each benchmark gets its own m5out_<bench>_bpc/ directory in the repo root.
#
# Parallelism within a group is controlled by JOBS (default 1).
# Example:
#   JOBS=4 ./run_bpc_parsec_sweep.sh
#   SIZE=simmedium ./run_bpc_parsec_sweep.sh

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
GEM5="${REPO_ROOT}/gem5/build/X86/gem5.opt"
CONFIG="${REPO_ROOT}/cdzcache_parsec.py"

SIZE="${SIZE:-simsmall}"
JOBS="${JOBS:-1}"
VARIANT=bpc

PRIORITY=(blackscholes swaptions freqmine streamcluster)
REST=(bodytrack canneal dedup facesim ferret fluidanimate raytrace vips x264)

if [[ ! -x "$GEM5" ]]; then
    echo "error: $GEM5 not found or not executable. Build gem5 first." >&2
    exit 1
fi
if [[ ! -f "$CONFIG" ]]; then
    echo "error: $CONFIG not found." >&2
    exit 1
fi

run_one() {
    local bench="$1"
    local outdir="${REPO_ROOT}/m5out_${bench}_${VARIANT}"
    local log="${outdir}/run.log"
    mkdir -p "$outdir"
    echo "[$(date +%H:%M:%S)] start ${bench} -> ${outdir}"
    if "$GEM5" --outdir="$outdir" "$CONFIG" \
            --benchmark "$bench" --size "$SIZE" --variant "$VARIANT" \
            >"$log" 2>&1; then
        echo "[$(date +%H:%M:%S)] done  ${bench}"
    else
        echo "[$(date +%H:%M:%S)] FAIL  ${bench} (see ${log})" >&2
    fi
}
export -f run_one
export REPO_ROOT GEM5 CONFIG SIZE VARIANT

run_group() {
    local label="$1"; shift
    local benches=("$@")
    echo "=== group: ${label} (${#benches[@]} benchmarks, JOBS=${JOBS}) ==="
    if [[ "$JOBS" -le 1 ]]; then
        for b in "${benches[@]}"; do run_one "$b"; done
    else
        printf '%s\n' "${benches[@]}" | xargs -n1 -P"$JOBS" -I{} bash -c 'run_one "$@"' _ {}
    fi
}

run_group "priority" "${PRIORITY[@]}"
run_group "rest"     "${REST[@]}"

echo "=== all done ==="
