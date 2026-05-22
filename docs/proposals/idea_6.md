# Idea 6: 2-D Parallelism (KxN Sharding × Chunking)

## Context
Prior experiments revealed two opposing bottlenecks:
1. **Cache Blowout**: Large dictionaries (e.g., ET-32, 515MB) exceed L3 cache (12MB), dropping throughput by ~79% due to `goto_tbl` memory bandwidth saturation.
2. **RAM Read Saturation**: Idea 1 (Pattern Sharding) improved cache locality via `prefix` policy by splitting the dictionary into K shards. However, standalone sharding regressed because each worker scanned the *entire text*, multiplying text DRAM reads by K. 

## Goal
Implement a 2-D parallel searcher (`pthread_2d_sharded_chunked`) combining:
- **K (Dictionary Shards)**: Use `prefix` policy to split patterns into K sub-automatons, improving L2/L3 cache hit rates. Empirically, K=2 yields the best locality gain.
- **N (Text Chunks)**: Divide the text into N chunks (like `pthread_chunked_v3`) to distribute text scanning across threads without multiplying text DRAM traffic.
- **Flat Layout**: Inherit Idea 5 (`flat_outputs`) to avoid pointer-chasing during match emission.

## Target Architecture
Given 12 hardware threads (i5-1235U):
- **T = K × N**. Example: K=2 (shards) × N=6 (text chunks) = 12 workers.
- Each worker `w_{k,n}` runs shard `k` on text chunk `n`.
- Standard warm-up phase `(max_pattern_len - 1)` must be performed per chunk `n`, using shard `k`'s automaton.

## Agent Instructions
- **To Study**: Review `src/searchers/pthread_chunked_v3.c` (text partitioning/overlap logic) and `src/searchers/pattern_sharded.c` (dictionary prefix-sharding logic).
- **To Implement**: A searcher that builds K sub-automatons, then launches `K * N` pthreads. Each thread receives a reference to sub-automaton `k` and text bounds `[start_n, end_n)`.
- **Merge Logic**: The master thread must sort and merge thread-local matches to match the deterministic sequential output.
- **Hypothesis to Validate**: `K=2 × N=6` beats `pthread_chunked_flat` (T=12) on large dictionaries by trading off parallel text bandwidth for localized state-transition cache hits.
