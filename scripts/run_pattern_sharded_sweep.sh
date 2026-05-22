#!/usr/bin/env bash
# Sweep for idea 1 (pattern sharding). Three axes:
#   - K (number of shards / worker threads) ∈ {1, 2, 4, 8, NPROC}
#   - sharding policy ∈ {round-robin, lpt, prefix}
#   - dictionary size ∈ {snort_100, snort_1k, snort, et_32, et}
# Corpus is fixed (default: data/simplewiki.txt; override with TEXT=).
#
# The headline metric is throughput (MB/s) and speedup vs. `sequential`.
# For each (dict, K, policy) tuple we also print:
#   - the aggregate sub-automaton state count vs. the unified
#     `aut->num_states`,
#   - the aggregate footprint (sum of ac_automaton_memory_bytes per shard),
# both of which come from the "# pattern_sharded*: K=..." diagnostic the
# searcher emits when the cfg.verbose flag is set. We use stderr to keep
# them out of the benchmark table.
#
# Hypothesis to falsify (per idea_1.md, "Metrics to report"):
#   "With K large enough that each sub_aut fits in L2, ET-32 should
#   recover the Snort-100 throughput regime."
#
# Use:
#   bash scripts/run_pattern_sharded_sweep.sh
#   TEXT=data/enron_corpus.txt bash scripts/run_pattern_sharded_sweep.sh
#   DICTS="snort_100 snort" bash scripts/run_pattern_sharded_sweep.sh

set -euo pipefail
cd "$(dirname "$0")/.."

BIN=build/aclab
[[ -x "$BIN" ]] || make -j

TEXT="${TEXT:-data/simplewiki.txt}"
WARMUP="${WARMUP:-1}"
ITERS="${ITERS:-3}"
NPROC=${NPROC:-$(nproc)}

# Sharding policies registered by src/searchers/pattern_sharded.c
POLICIES=("pattern_sharded" "pattern_sharded_lpt" "pattern_sharded_prefix")

# Default dictionary set. Skip any that do not exist.
DICTS_DEFAULT=("snort_100" "snort_1k" "snort" "et_32" "et")
read -ra DICTS <<< "${DICTS:-${DICTS_DEFAULT[*]}}"

# K sweep. The K=1 path delegates to sequential_flat, so it acts as the
# in-process baseline for the sharded code path. K=NPROC is "saturate
# all cores with one shard per core".
K_SWEEP=(1 2 4 8 "$NPROC")
# De-duplicate (NPROC may be 8 already).
mapfile -t K_SWEEP < <(printf "%s\n" "${K_SWEEP[@]}" | awk '!seen[$0]++')

[[ -f "$TEXT" ]] || { echo "missing corpus: $TEXT" >&2; exit 1; }

OUT_DIR="runs/pattern_sharded_$(date +%Y%m%d_%H%M%S)"
mkdir -p "$OUT_DIR"
echo "# Output: $OUT_DIR"
echo "# Corpus: $TEXT ($(stat -c%s "$TEXT") bytes)"
echo "# Threads: K ∈ ${K_SWEEP[*]}  Policies: ${POLICIES[*]}"
echo

run_one () {
    local dict_name="$1"
    local searcher="$2"
    local K="$3"
    local pat_path="data/patterns_${dict_name}.txt"
    local out="$OUT_DIR/${dict_name}_${searcher}_K${K}.txt"
    if [[ ! -f "$pat_path" ]]; then
        echo "  skip: $pat_path not found"
        return
    fi
    echo "  ${dict_name}  ${searcher}  K=${K}  -> $out"
    # Build phase is non-trivial for ET-* dicts, so use the parallel
    # builder. Search phase is what we are measuring.
    AC_BUILD_PARALLEL=1 "$BIN" \
        --patterns "$pat_path" \
        --input "$TEXT" \
        --searcher "$searcher" \
        --threads "$K" \
        --warmup "$WARMUP" \
        --iters "$ITERS" \
        > "$out" 2>&1 || echo "    (run failed; see $out)"
}

# 1. Sequential baseline per dictionary -- the "1.00×" anchor every
# sharded result is divided by.
echo "## Sequential baseline"
for d in "${DICTS[@]}"; do
    pat_path="data/patterns_${d}.txt"
    [[ -f "$pat_path" ]] || { echo "  skip $d (no $pat_path)"; continue; }
    out="$OUT_DIR/${d}_sequential.txt"
    echo "  ${d}  sequential        -> $out"
    "$BIN" --patterns "$pat_path" --input "$TEXT" --searcher sequential \
        --warmup "$WARMUP" --iters "$ITERS" > "$out" 2>&1 \
        || echo "    (run failed; see $out)"
done

# 2. Sharded sweep
echo
echo "## Sharded sweep"
for d in "${DICTS[@]}"; do
    for s in "${POLICIES[@]}"; do
        for K in "${K_SWEEP[@]}"; do
            run_one "$d" "$s" "$K"
        done
    done
done

# 3. Synthesise a one-page summary in CSV form. Easy to paste into the
# dissertation results table.
SUMMARY="$OUT_DIR/summary.csv"
echo "dict,searcher,K,bytes,mean_ms,mbps,matches" > "$SUMMARY"
for f in "$OUT_DIR"/*.txt; do
    [[ -f "$f" ]] || continue
    base=$(basename "$f" .txt)
    # Extract dict + searcher + K from filename.
    # Naming: <dict>_<searcher>[_K<K>].txt
    if [[ "$base" == *_K* ]]; then
        K_val="${base##*_K}"
        rest="${base%_K*}"
    else
        K_val=""
        rest="$base"
    fi
    # rest is "<dict>_<searcher>". Searcher names contain underscores
    # too (e.g. pattern_sharded_lpt); peel off known-good searchers.
    searcher=""
    for cand in sequential pattern_sharded_lpt pattern_sharded_prefix pattern_sharded; do
        if [[ "$rest" == *"_${cand}" ]]; then
            searcher="$cand"
            dict="${rest%_${cand}}"
            break
        fi
    done
    if [[ -z "$searcher" ]]; then continue; fi
    # Pull the timing line. Skip the header line (column "thr").
    line=$(grep -E "^${searcher}[[:space:]]" "$f" | head -1 || true)
    if [[ -z "$line" ]]; then continue; fi
    # Columns from main.c bench_result_print:
    # searcher thr bytes min(ms) mean(ms) max(ms) MB/s matches
    awk -v d="$dict" -v s="$searcher" -v k="$K_val" '
        { print d","s","k","$3","$5","$7","$8 }
    ' <<< "$line" >> "$SUMMARY"
done

echo
echo "## Summary CSV: $SUMMARY"
echo "  $(wc -l < "$SUMMARY") rows (incl. header)"
