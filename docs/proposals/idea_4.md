# 4 · Idea: Parallel BFS Construction of the Automaton

> **Phase touched.** Build (currently 100 % sequential, in
> `src/ac_automaton.c::ac_automaton_build`).
> **Composes with.** Every existing searcher (search-phase code does
> not change; only the wall-clock build cost shrinks).
> **Status.** Not implemented.

## The idea in one paragraph

Today the automaton is built single-threaded by the master: the trie is
inserted byte-by-byte, then **a single BFS** (`src/ac_automaton.c`,
lines ≈ 144–192) sweeps every state to compute `fail[v]`, the
deterministic transition row `goto_tbl[u]`, and the dictionary suffix
link `dict_suffix[u]`. This BFS dominates construction time and is
embarrassingly parallel: at BFS depth `d`, every state can be processed
**independently** because the only inputs it needs are
`fail[parent]` and `goto_tbl[fail[parent]]`, both at depth `≤ d − 1` and
therefore already finalized. Replace the sequential queue with a
**level-synchronous parallel BFS**: at each depth, partition the
frontier among `T` workers, process in parallel, barrier, repeat.

This is the only idea in the roadmap that attacks the build phase.
Every other idea inherits the slower build cost; this one removes it.

## Why it complements the existing searchers

Build cost is currently invisible in `bench_run` (which times only
search), but it appears in the `# build time:` line that
`src/main.c::main` prints, and it matters in two regimes:

1. **Short scans / many runs.** When the same dictionary is reused
   against many small inputs (the typical IDS *streaming* regime),
   amortized build time enters the critical path. Halving build time
   directly improves user-visible latency.
2. **Large dictionaries (Snort ET, ~44 k rules).** Construction for
   `num_states` in the tens of thousands runs the BFS inner loop
   `num_states × 256` times. With 30 k states that is 7.7 M iterations
   per build; on a single core this is single-digit seconds. With
   `T = 8` cores it should drop to sub-second territory — a result
   none of the surveyed papers reports for software AC.

It also opens a new dimension for the dissertation: **report build
throughput** (`states/s` or `MiB-of-automaton/s`) alongside search
throughput. The combined picture of "K patterns → ready-to-scan
automaton in time T" is more honest about the cost of large IDS
rulesets than search-only numbers.

## Why this is safe (correctness sketch)

Define `depth(v)` as the BFS depth of state `v` in the trie (root is
depth 0, its children are depth 1, etc.). The construction recurrences,
copied from `ac_automaton.c`, are:

```text
fail[v]                                                  for v at depth 1: = 0
fail[v]    = goto_tbl[fail[parent(v)], byte_into_v]       for v at depth ≥ 2
goto_tbl[u, c]  = raw_child[u, c]    if raw_child[u,c] != NIL
                | goto_tbl[fail[u], c]   otherwise
dict_suffix[u]  = fail[u]   if own_out_head[fail[u]] != NIL
                | dict_suffix[fail[u]]   otherwise
```

For every state `u` at depth `d`, **all right-hand sides reference
states at depth strictly less than `d`** (the relation
`depth(fail[u]) < depth(u)` is a textbook invariant of Aho–Corasick).
Therefore a level-synchronous BFS that fully finalizes depth `d − 1`
before starting depth `d` produces the same `(fail, goto_tbl,
dict_suffix)` as the sequential queue-based BFS. The barrier between
levels is the only synchronization required.

## Implementation hooks

Modify `src/ac_automaton.c` (the search code is untouched). The
sequential BFS loop:

```c
while (qhead < qtail) {
    int32_t u = queue[qhead++];
    int32_t f = b.fail[u];
    /* ...dict_suffix[u]... */
    for (int c = 0; c < AC_ALPHABET_SIZE; c++) {
        /* ...goto_tbl[u][c], possibly enqueue child v... */
    }
}
```

becomes:

```c
/* Level-synchronous variant.
 *   frontier_curr[]  : states at depth d, finalized for all parents
 *   frontier_next[]  : children of frontier_curr, will be processed at depth d+1
 *   next_count       : atomic_size_t -- monotonically grows as level d
 *                      workers append to frontier_next; reset between levels.
 */

int32_t *frontier_curr = /* root's children, populated as before */;
int32_t  curr_count    = /* number of root children */;
int32_t *frontier_next = malloc(num_states * sizeof(int32_t));
atomic_size_t next_count;

while (curr_count > 0) {
    atomic_store_explicit(&next_count, 0, memory_order_relaxed);

    /* Spawn T workers; each takes a slice of frontier_curr[0..curr_count).
     * Workers DO NOT share writes to goto_tbl rows (each writes its own
     * states' rows), DO NOT share writes to fail[] (each writes its own
     * children's fail entries), and append to frontier_next via
     * atomic_fetch_add(&next_count, k) followed by k scalar writes into
     * the reserved slice. */
    spawn_level_workers(frontier_curr, curr_count, &next_count, ...);
    barrier();   /* implicit via pthread_join when the level ends */

    /* Swap for next level */
    int32_t *tmp   = frontier_curr;
    frontier_curr  = frontier_next;
    frontier_next  = tmp;
    curr_count     = (int32_t)atomic_load_explicit(&next_count, memory_order_acquire);
}
```

Critical decisions an implementer must make:

- **Per-level worker spawn vs persistent pool.** Two viable shapes:
  1. *Spawn + join per level.* Simplest, no shared state outside the
     frontier arrays. Overhead is `T × pthread_create` + `T ×
     pthread_join` per level. The trie depth for Snort is ~30, so the
     spawn cost can dominate for shallow dictionaries.
  2. *Persistent pool with `pthread_barrier_t` per level.* Workers stay
     alive across levels; one barrier wait per level. Lower per-level
     overhead, more code. Recommended for the published results.
- **Frontier append.** Use `atomic_size_t` (relaxed for the increment,
  acquire on the read after `pthread_join`/barrier) to reserve a slice
  of `frontier_next`. **This is the only atomic in the build phase**,
  touched once per real-child discovery, which is far less than per
  byte. It does **not** violate roadmap invariant 2 (no atomics in
  search hot loop) because build is not the search hot loop.
- **Work granularity.** With `curr_count = 10`, partitioning across 8
  workers is wasteful (most workers idle). Add a *guard*: if
  `curr_count < min_par_threshold`, run that level sequentially.
  Typical threshold: 64 states.
- **Atomic-free alternative for the frontier.** Each worker can write
  its discovered children into a thread-local buffer and the master
  concatenates after the barrier. Trades atomics for buffer overhead;
  benchmark both.
- **Pre-allocate `frontier_curr` and `frontier_next` once** at
  `num_states` capacity (their union is bounded by it). Reusing them
  across all levels avoids per-level mallocs.

## Correctness validation

- **`make test`** — the existing test suite **already** exercises this
  if you run after the change. Every searcher (sequential and parallel)
  consumes the resulting `ac_automaton_t`, so any divergence between
  the parallel-build automaton and the sequential-build automaton
  surfaces as a `lists_equal` failure on the first test case. No new
  test cases are strictly required, but add one targeted assertion
  (see next bullet).
- **Build-result equivalence assertion.** Run both the sequential
  `ac_automaton_build` and the parallel one (renamed e.g.
  `ac_automaton_build_par`) on the same patterns, then byte-compare
  the produced `goto_tbl`, `dict_suffix`, `own_out_head`, and
  `outputs`. They must be identical. Add this as `tests/test_build_par.c`.
- **`make tsan`** — required, even though the build phase is not the
  hot path. The `atomic_size_t` reservation is the only race-prone
  step; TSan should remain silent. Any other write race is a bug.
- **Level-by-level invariant check (debug only).** After level `d`,
  assert that for every state `u` finalized at this level,
  `depth(fail[u]) <= d - 1`. This catches BFS-order violations early.

## Metrics to report

- **Build time vs. number of patterns.** Fix the corpus, sweep
  dictionary size: `{Snort-100, Snort-1k, Snort-10k, Snort full,
  ET full}`. Plot wall-clock build time (`bench_marker(build)` in
  `main.c`) for sequential and `T ∈ {2, 4, 8, nproc}` parallel.
  Report:
  - absolute build seconds
  - parallel speedup `T_seq / T_par`
  - efficiency `speedup / T`
- **Per-level histogram.** For a fixed large dictionary, log
  `(level, frontier_size, level_seconds)` so the dissertation can
  show *which* BFS levels actually parallelize (shallow levels
  often have so few states that they stay sequential).
- **Build vs. scan, relative weight.** Report
  `build_seconds / (build_seconds + scan_seconds)` for both
  sequential and parallel builds on a single-pass workload. The
  contribution is most defensible when the parallel build moves the
  ratio meaningfully (e.g. from 12 % to 4 %).
- **Total memory pressure.** Verify peak RSS during the parallel
  build is bounded by `2 × num_states × sizeof(int32_t)` extra (the
  two frontier arrays). It should not blow up.

## Failure modes / when it loses

- **Very shallow tries** (a handful of long, non-overlapping
  patterns) where the BFS has < `nproc` states per level for most
  levels. The parallel guard skips these; the build then runs at
  sequential speed, no regression.
- **Very small dictionaries** where the entire build is sub-
  millisecond. Parallelism overhead dominates; the guard returns
  early to the sequential path.
- **Memory bandwidth ceilings.** The BFS writes one `int32_t` row
  per state (1 KiB per state, dense). Beyond a certain `T` the bus
  saturates and adding workers stops helping. This is the same
  ceiling search-phase hits; it is fine to acknowledge it as a
  bound rather than a defect.

## Out of scope (deliberate)

- *Parallel trie insertion* (the loop in
  `ac_automaton_build` that walks `b.raw_child` for each pattern).
  The trie-insertion cost is `O(total_pattern_length)`, which is
  typically ≪ the BFS cost `O(num_states × 256)`. Parallelizing
  insertion would require atomics on `raw_child[u*256+c]` (because
  multiple patterns may discover the same child slot simultaneously)
  and on `aut->num_states` (for state allocation). The
  cost/complexity ratio is poor compared to parallel BFS. Mention
  it in the dissertation as future work.

## Related work pointer

Level-synchronous parallel BFS is folklore in graph processing; the
canonical reference is Beamer, Buluç & Patterson, *"Direction-
Optimizing Breadth-First Search"* (SC '12), though the AC build does
not benefit from the "bottom-up" direction (the AC graph is too
shallow). The novelty for the TCC is in **applying** the technique to
the AC automaton construction, not in inventing the BFS shape.

Within the laboratory's reading list, no surveyed paper (Lee 2017, Qu
2016, Thambawita 2018, Lin 2010) parallelizes the construction phase
of AC. This makes parallel construction the *only* algorithmic axis in
the roadmap that has no published direct precedent for software AC —
a clean dissertation contribution.

---
