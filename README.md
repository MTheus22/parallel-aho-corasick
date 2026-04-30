# Parallel Aho–Corasick Research Laboratory

A modular C laboratory for designing, plugging in, and rigorously
benchmarking parallel implementations of the **Aho–Corasick** multi-pattern
search algorithm against a sequential baseline. Targets multi-core CPUs in
shared memory, using the **POSIX Threads** API for the first parallel
variant.

## Layout

```
include/             # public headers (automaton, match list, searcher API,
                     # benchmark harness)
src/                 # core (automaton + match list + bench + registry)
src/searchers/       # one file per searcher implementation; plug-in slot
                     # for new variants
tests/               # correctness test (every searcher must agree with the
                     # sequential baseline)
scripts/             # benchmark sweeps
```

## Architecture

### 1. Automaton construction (sequential, master thread)

`ac_automaton_build()` builds the trie, the failure function, and a
**deterministic transition table** `goto_tbl[state * 256 + byte]` so each
scan step is a single indexed load. Output reporting uses a per-state
`own_out_head` chain plus a `dict_suffix` link to the nearest
output-bearing ancestor, giving O(1)+|reported| work per character.

After `ac_automaton_build()` returns the structure is **strictly
read-only**: the goto table, output arena, and pattern storage are never
written again. Multiple threads share the automaton by pointer with no
locks.

### 2. Pluggable searcher interface

Every implementation exposes one `ac_searcher_t`:

```c
typedef struct ac_searcher {
    const char *name;
    const char *description;
    int (*search)(const ac_automaton_t *aut,
                  const char *text, size_t text_len,
                  const ac_searcher_config_t *cfg,
                  ac_match_list_t *out_matches,
                  ac_thread_metric_t **out_thread_metrics,
                  size_t *out_num_thread_metrics);
} ac_searcher_t;
```

To add a new variant, drop a new file in `src/searchers/` and register
yourself from a constructor:

```c
static const ac_searcher_t k_my_searcher = {
    .name = "my_variant", .description = "...", .search = my_search,
};
__attribute__((constructor))
static void my_register(void) { ac_searcher_register(&k_my_searcher); }
```

The `Makefile` picks up the file automatically.

### 3. Searchers shipped

| Name              | Description                                                                  |
|-------------------|------------------------------------------------------------------------------|
| `sequential`      | Single-threaded baseline reference                                           |
| `pthread_chunked` | Pthreads, fixed-size chunks with `(max_pattern_len-1)` overlap, thread-local match lists, read-only shared automaton |

`pthread_chunked` partitions the input into **N core ranges**, one per
worker. Worker *i* actually scans `[core_start_i - overlap, core_end_i)`;
the leading `overlap = max_pattern_len - 1` bytes are warm-up so that the
DFA state at `core_start_i` agrees with a global scan, and any pattern
straddling a chunk boundary is detected by exactly one worker. Matches
ending in the warm-up region belong to the previous worker and are
dropped. Each worker writes into a **thread-local `ac_match_list_t`**;
the master merges them after `pthread_join`.

### 4. Benchmarking

`benchmark.h` provides:

- `bench_now_ns()` — `CLOCK_MONOTONIC` nanosecond timestamps.
- `bench_marker_t` — simple start/end markers for ad-hoc phases.
- `bench_run()` — warm-up + N iterations, captures min/mean/max wall
  time, throughput in MiB/s, and match counts.
- `--per-thread` CLI flag to print per-worker time, bytes, and matches.

## Build

```sh
make            # release (-O3 -march=native)
make debug      # asserts on, -O0
make asan       # AddressSanitizer + UBSan
make tsan       # ThreadSanitizer (data-race verifier for parallel scans)
make test       # run correctness suite (every searcher vs. baseline)
make bench      # synthetic sweep across thread counts
```

Requires a C11 compiler, pthreads, POSIX 2008.

## Quick start

```sh
make
./build/aclab --list                           # show registered searchers
./build/aclab --patterns tests/data/patterns_small.txt \
              --input    /path/to/large.txt \
              --searcher pthread_chunked \
              --threads  8 \
              --warmup 1 --iters 5 \
              --per-thread
```

Omit `--searcher` to sweep every registered searcher in one run.

## Verifying parallel correctness

`make test` builds an independent test binary that:

1. Constructs the automaton from a battery of pattern sets.
2. Computes the baseline match set with `sequential`.
3. Runs **every other registered searcher** at thread counts
   `{1,2,3,4,7,8}` over inputs explicitly designed to put matches on
   every chunk boundary.
4. Sorts both lists and compares element-wise. Any divergence fails.

Combine with `make tsan` to verify there are no data races on the
read-only automaton or on the per-thread match lists.

## Adding the next parallel variant

The expected workflow:

1. Drop `src/searchers/<your_variant>.c`.
2. Implement the searcher and register it from `__attribute__((constructor))`.
3. `make test` — must agree with the baseline.
4. `make bench` — compare throughput.
5. (Optional) `make tsan` — verify no races.

No other file needs to change.
