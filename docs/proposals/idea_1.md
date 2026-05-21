## 1 · Idea: Pattern Sharding (dictionary-level parallelism)

### The idea in one paragraph

Instead of partitioning the *text*, partition the *dictionary*. Build `K`
independent Aho–Corasick automata, each over a disjoint subset of the
patterns. Spawn `K` workers; **each worker scans the entire input** using
its own sub-automaton, emitting only the matches belonging to its shard.
A final, sequential merge concatenates the per-shard match lists. No
overlap is needed (every worker sees every byte). Each worker’s
sub-automaton is dramatically smaller than the unified one, so it can fit
in L2/L3 even when the unified automaton blows out.

### Why this is worth a chapter in the TCC

`tcc_notes/tldr_metricas.md` §3 measured a **78.7 % throughput drop**
(1838.9 MB/s → 391.4 MB/s) when the automaton grew from 1.3 MiB to 515 MiB
(42× L3). The existing chunked variants do nothing about this — every
worker still walks the same gigantic DFA. Pattern sharding is the only
proposed technique that **directly attacks the cache-blowout regime**,
which is precisely the IDS regime the TCC is centered on (Snort ET-Open,
~44 k rules).

It is also genuinely orthogonal to data chunking, which lets the TCC
report a 2-D speedup curve (`K × N`, with `K · N ≤ #cores`) — a result
none of the surveyed papers (Lee 2017, Qu 2016, Thambawita 2018) explores.

### Implementation hooks

New file: `src/searchers/pattern_sharded.c`.

```c
/* Build K sub-automata over pattern shards. K defaults to num_threads;
 * a richer policy could choose K based on aut->num_states vs. L2 size. */

typedef struct {
    int                  shard_id;
    ac_automaton_t       sub_aut;          /* read-only after build */
    int32_t              pattern_id_remap[];/* local->global, see below */
} shard_t;

typedef struct {
    int                    thread_id;
    const ac_automaton_t  *sub_aut;
    const int32_t         *id_remap;        /* sub_aut local pid -> global pid */
    const char            *text;
    size_t                 text_len;
    ac_match_list_t        local;
    double                 seconds;
    int                    rc;
} worker_t;

static void *worker_main(void *arg) {
    /* Identical inner loop to sequential.c, but on sub_aut.
     * On match emit:
     *   m.pattern_id = id_remap[outputs[o].pattern_id];
     *   m.end_pos    = (int64_t)i;
     */
}
```

Key decisions an implementer must make:

- **Sharding policy.** Three reasonable choices, evaluate all three:
  1. *Round-robin by index*: `shard = i % K`. Simple, balances pattern
     count but not state count.
  2. *Length-balanced greedy*: sort patterns by length desc, assign each
     to the currently smallest shard (Longest-Processing-Time-first).
     Balances *bytes-per-shard*, a decent proxy for state count.
  3. *Prefix-byte bucketing*: hash patterns by their first byte mod K.
     States branch heavily near the root; this can produce more uniform
     sub-automata sizes when the dictionary is biased.
- **Storage.** The `aut` argument received by `search()` is the
  **unified** automaton. The sharded variant must build its own sub-
  automata internally (the registry passes a single `aut`). Memoize by
  caching shard automata in a `static` keyed on `aut->patterns` pointer,
  or rebuild per call and accept the cost (already amortized over the
  warmup + iters loop in `bench_run`).
- **Match deduplication.** With disjoint shards, no two shards can
  produce the same `(end_pos, global_pattern_id)` pair, so the merge is
  a plain concatenation. Sort with `ac_match_list_sort` before comparing
  against `sequential` in tests.

### Correctness validation

- **`make test`.** Add a test case where the pattern set deliberately
  fragments into many length classes (e.g. 1 byte, 8 bytes, 64 bytes,
  256 bytes) so each sharding policy hits its corner cases.
- **`make tsan`.** Required. The K sub-automata must be built **before**
  `pthread_create` (rule 1 of §0). Sanity check that no worker touches
  `aut` (the unified one) — it should only see its own `sub_aut`.
- **Pid remap test.** A unit assertion: for every match emitted with
  global pid `g`, verify `aut->patterns[g].text` equals
  `sub_aut->patterns[local_pid].text` for the corresponding shard.

### Metrics to report

Run two complementary sweeps against `enron_corpus.txt`:

- **Throughput vs. K.** Fix `T = nproc`, run `K ∈ {1, 2, 4, 8, K=nproc}`.
  Each K assigns `T/K` chunked workers per shard *or* (simpler baseline)
  one worker per shard with the input scanned K times. Report MB/s and
  the *aggregate state count* `Σ sub_aut[k].num_states` vs. the unified
  `aut->num_states`.
- **Cache pressure.** Re-run the §3 experiment from `tldr_metricas.md`
  (Snort-100 vs. ET-32 dictionaries). The hypothesis to falsify: with
  K large enough that each sub_aut fits in L2, ET-32 should recover the
  Snort-100 throughput regime.
- **Footprint.** Print `Σ ac_automaton_memory_bytes(&sub_aut[k])`. It
  will be larger than the unified automaton (each shard re-pays the
  256-byte transition row overhead) — quantify the memory/throughput
  trade-off.

### Failure modes / when it loses

- **Small unified automata** that already fit in L1/L2. Each byte is
  scanned K times; the second-scan cache hit is cheap but not free.
  Expect a regression relative to `pthread_chunked` for the Snort-100
  dictionary; this is fine and worth reporting as a clean
  *cross-over curve* (small dict → chunking wins, large dict → sharding
  wins).
- **Pathological pattern distribution** where one shard absorbs >90 %
  of states. Sharding policy (2) or (3) above is the mitigation.

---