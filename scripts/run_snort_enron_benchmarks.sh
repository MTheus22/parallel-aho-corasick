#!/usr/bin/env bash
# Sweep the registered searchers across a range of thread counts using
# the Snort 3 Community Rules patterns and the Enron Email Dataset corpus.
set -euo pipefail

cd "$(dirname "$0")/.."

BIN=build/aclab
[[ -x "$BIN" ]] || make -j

PATS="data/patterns_snort.txt"
TEXT="data/enron_corpus.txt"

if [[ ! -s "$PATS" || ! -s "$TEXT" ]]; then
  echo "Error: Datasets not found. Please run ./scripts/prepare_datasets.sh first."
  exit 1
fi

NPROC=${NPROC:-$(nproc)}

echo "=========================================================="
echo "    Benchmarking Parallel Aho-Corasick (Snort + Enron)    "
echo "=========================================================="
echo "Patterns: $PATS"
echo "Corpus:   $TEXT"
echo "Threads:  up to $NPROC"
echo "=========================================================="

echo
echo "## Sequential baseline"
"$BIN" --patterns "$PATS" --input "$TEXT" --searcher sequential --warmup 1 --iters 3

echo
echo "## Parallel sweep (Pthreads Chunked)"
for T in 1 2 4 8 16 "$NPROC"; do
  # Avoid running the $NPROC twice if it matches one of the fixed sizes
  if [[ "$T" == "$NPROC" ]] && [[ " 1 2 4 8 16 " =~ " $NPROC " ]]; then
    continue
  fi

  "$BIN" --patterns "$PATS" --input "$TEXT" \
    --searcher pthread_chunked --threads "$T" \
    --warmup 1 --iters 3
done

echo "=========================================================="
echo "Benchmark completed."
