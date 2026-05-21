# 5 · Idea: Eager Dictionary-Suffix Flattening (Flat Output Table)

> **Phase touched.** Build (algorithmic transformation of the automaton
> layout) and Search (hot loop benefits).
> **Composes with.** Every existing and proposed searcher; the win is
> *multiplicative* with the per-thread / per-lane gains of ideas 1, 2, 3.
> **Status.** Not implemented.

## The idea in one paragraph

Today, when the hot loop reaches a state `s` and discovers a match
(`own_head[s] != NIL || dict_suffix[s] != NIL`), emitting matches
requires walking **two nested chains**: the outer chain follows
`dict_suffix[…]` from `s` toward the root, and the inner chain walks the
linked list `outputs[own_head[l]]` at each visited state. Both chains
are stored as indices into `int32_t[]` arrays in `ac_automaton_t`; the
walks are pointer-chasing with cold-cache behaviour on large automata.

Replace the chains with a **single flat array of pattern IDs per state**,
computed once at build time:

```text
flat_pids[ flat_offset[s] .. flat_offset[s] + flat_count[s] )
  == { pid : pid is emitted by sequential AC when arriving at state s }
```

The search-phase emission becomes one contiguous loop over `flat_count[s]`
`int32_t`s, with no pointer chasing and predictable cache behaviour.
Every existing searcher (sequential, all `pthread_*`) gets the speed-up
for free once it adopts the new layout. The technique itself is
algorithmic — it is a layout transformation of the automaton — and is
therefore TCC-defensible independently of any threading work.

## Why it complements the existing searchers

The current hot loop (e.g. `src/searchers/sequential.c`, lines 33–47;
also `src/searchers/pthread_chunked.c`, lines 76–89) is:

```c
if (AC_UNLIKELY(own_head[state] != AC_NIL || dict_suffix[state] != AC_NIL)) {
    int32_t l = (own_head[state] != AC_NIL) ? state : dict_suffix[state];
    while (l != AC_NIL) {
        for (int32_t o = own_head[l]; o != AC_NIL; o = outputs[o].next) {
            /* emit (i, outputs[o].pattern_id) */
        }
        l = dict_suffix[l];
    }
}
```

The hot-path branch (`AC_UNLIKELY`) is well-predicted *most of the
time* — but every time it fires, the loops above incur:

- A chain of dependent loads on `dict_suffix[l]` (random access into a
  `num_states`-sized array — cold once the automaton spills out of L2).
- A second chain of dependent loads on `outputs[o].next` (random access
  into a `num_outputs`-sized arena — also cold for large dictionaries).

After flattening, the same emission costs **one** load from
`flat_count[state]` plus a contiguous run through `flat_pids[…]`. Both
arrays can be made small: `flat_count` is `int32_t[num_states]`
(identical footprint to `own_out_head`), and `flat_pids` is one
`int32_t` per *(state, emitted_pid)* pair, i.e. roughly the same total
size as the existing `outputs` arena (often smaller, because
dict_suffix walks no longer duplicate pids across the chain — see
"De-duplication" below).

Crucially, this idea is **orthogonal** to ideas 1–4:

| Idea                   | Per-thread benefit | Per-lane (SIMD) benefit | Build-time benefit |
|------------------------|--------------------|-------------------------|--------------------|
| 1 (pattern sharding)   | yes (cache)        | —                       | —                  |
| 2 (SIMD gather)        | yes                | yes                     | —                  |
| 3 (δ² multi-step)      | yes (compute)      | —                       | —                  |
| 4 (parallel BFS build) | —                  | —                       | yes                |
| **5 (flat outputs)**   | **yes (emit)**     | **yes (emit)**          | **slight (one extra pass over states)** |

In particular, idea 2 (SIMD gather) **needs** this idea — or something
like it — to vectorise emission cleanly. With the chain-walking layout,
SIMD lanes that hit a match must each independently chase a chain of
arbitrary length, which is exactly what SIMD cannot do efficiently.
With flattening, the lane just reads
`(flat_offset[lane_state], flat_count[lane_state])` and emits in a
short scalar loop with predictable size.

## Where it lives in `ac_automaton_t`

Extend the public struct (`include/ac_automaton.h`) with three fields:

```c
typedef struct ac_automaton {
    /* ...existing fields... */

    /* Flat per-state pattern_id arena.
     * For state s, the matches emitted on arrival are
     *   flat_pids[ flat_offset[s] .. flat_offset[s] + flat_count[s] ).
     * Populated at the end of ac_automaton_build(); read-only thereafter. */
    int32_t          *flat_offset;     /* size: num_states */
    int32_t          *flat_count;      /* size: num_states */
    int32_t          *flat_pids;       /* size: total_flat_pids */
    int32_t           total_flat_pids;
} ac_automaton_t;
```

These are computed **after** the existing `(own_out_head, dict_suffix,
outputs)` are finalised, by one extra pass over the automaton. The
existing fields stay in place (they are still useful for diagnostics
and for any searcher that has not migrated). The data is read-only
after build, so it satisfies invariant 1 of the roadmap automatically.

## Implementation hooks

Two atomic units of work, in this order:

### A · Construction (`src/ac_automaton.c`)

At the end of `ac_automaton_build`, after the BFS finalises
`(goto_tbl, fail, dict_suffix, own_out_head, outputs)`:

```c
/* Pass 1: count, per state, how many pids the chain-walk would emit. */
int32_t total = 0;
for (int32_t s = 0; s < aut->num_states; s++) {
    int32_t l = (own_head[s] != AC_NIL) ? s : dict_suffix[s];
    int32_t c = 0;
    while (l != AC_NIL) {
        for (int32_t o = own_head[l]; o != AC_NIL; o = outputs[o].next) c++;
        l = dict_suffix[l];
    }
    flat_count[s] = c;
    flat_offset[s] = total;
    total += c;
}
flat_pids = malloc((size_t)total * sizeof(int32_t));
total_flat_pids = total;

/* Pass 2: fill flat_pids. */
for (int32_t s = 0; s < aut->num_states; s++) {
    int32_t l = (own_head[s] != AC_NIL) ? s : dict_suffix[s];
    int32_t pos = flat_offset[s];
    while (l != AC_NIL) {
        for (int32_t o = own_head[l]; o != AC_NIL; o = outputs[o].next) {
            flat_pids[pos++] = outputs[o].pattern_id;
        }
        l = dict_suffix[l];
    }
}
```

This pass is `O(Σ chain_length(s))`, in practice ~linear in
`num_outputs`. It is trivially parallel over `s` if combined with
idea 4 (each level worker can compute its states' counts and pids
locally, then the offset prefix sum is a serial post-pass).

### B · Search migration (one new file per migrated searcher)

Pick the searchers that benefit most (`sequential` and
`pthread_chunked_v2` are good first targets) and add a *new* variant
that uses the flat layout:

```c
/* src/searchers/sequential_flat.c  (or a flag flipped inside the
 * existing sequential.c, behind a compile-time switch -- but a new
 * file keeps the comparison clean for benchmarks). */

int32_t state = 0;
for (size_t i = 0; i < text_len; i++) {
    state = goto_tbl[(size_t)state * AC_ALPHABET_SIZE + (uint8_t)text[i]];

    int32_t cnt = flat_count[state];
    if (AC_UNLIKELY(cnt > 0)) {
        int32_t off = flat_offset[state];
        for (int32_t k = 0; k < cnt; k++) {
            ac_match_t m = {
                .end_pos    = (int64_t)i,
                .pattern_id = flat_pids[off + k],
            };
            int rc = ac_match_list_push(out, m);
            if (rc != AC_OK) return rc;
        }
    }
}
```

The change is minimal — one chained walk replaced with one linear
loop — and intentionally trivial so an agent can verify it by reading
the diff.

## Critical decisions an implementer must make

- **De-duplication.** In the chain-walk layout, a single pid can be
  emitted multiple times for the same `end_pos` only if it appears in
  multiple dict_suffix-reachable states (it does not, by construction
  of AC). Therefore `flat_pids[s]` should *not* deduplicate. Verify by
  asserting that the multiset of pids emitted for every `end_pos` is
  identical between the chain-walk and flat searchers.
- **Memory bound.** In the pathological case (one state reachable
  via dict_suffix from every other), `total_flat_pids` could
  approach `num_states × num_outputs`. In practice (Snort, ET)
  chain lengths are small and `total_flat_pids ≈ num_outputs`.
  Print `total_flat_pids` in the build header so the dissertation
  can show empirical growth.
- **`ac_automaton_memory_bytes()`.** Update to include the three new
  arrays. Otherwise the cache-footprint plots from
  `tldr_metricas.md` §3 stop being accurate.
- **Match ordering.** The chain-walk emission visits pids in a
  specific order (own outputs first, then dict_suffix chain in
  ancestor order). Preserve this order in `flat_pids[s]` so
  comparison via `ac_match_list_sort` remains insensitive but
  bytewise diff debugging is still useful.

## Correctness validation

- **`make test`** — every existing case must remain green for the
  flat-layout searcher *across all thread counts*. The dense-matches
  case is the stress test because it emits maximally.
- **`make tsan`** — required for any threaded flat searcher. The
  layout is strictly read-only after build, so no races are possible
  unless the migration accidentally writes into one of the new
  arrays.
- **Build-time equivalence check.** Add a unit test that, for every
  state `s`, recomputes the chain-walk pid multiset on the fly and
  asserts equality with `flat_pids[flat_offset[s] : flat_offset[s] +
  flat_count[s]]` interpreted as a multiset. Run this on the entire
  five-case test corpus.
- **Round-trip dissertation sanity.** Run *both* layouts on the same
  benchmarks; the match lists must compare equal after
  `ac_match_list_sort`. Any divergence is a flattening bug.

## Metrics to report

- **Hot-loop microbenchmark.** Run the flat sequential against the
  chain-walk sequential on the densest test case
  (`make_repeated("a", 1000)` × N copies). Report cycles/byte from
  `perf stat`. Expect a measurable drop because every byte emits.
- **Throughput vs. dictionary size.** Sweep
  `{Snort-100, Snort-1k, Snort full, ET full}` and plot MB/s for
  chain-walk vs. flat across `T ∈ {1, …, nproc}`. The dissertation
  argument is that the gain *grows* with dictionary size because the
  chain-walk pointer chasing increasingly misses cache.
- **Combined with idea 2.** Once both ideas are implemented, report
  SIMD-flat vs. SIMD-chain on the same dictionary range. This is the
  cleanest possible demonstration of orthogonality.
- **Memory cost.** Report `total_flat_pids` and
  `flat_offset+flat_count+flat_pids` bytes for each dictionary.
  Compare to the original `outputs` arena size; the flat arena is
  typically within 1.5× of `num_outputs * 4` bytes.

## Failure modes / when it loses

- **Sparse-match workloads.** Where matches are extremely rare
  (e.g. random-bytes input scanned against literal patterns), the
  chain-walk path is taken so seldom that pointer-chasing latency is
  invisible. The flat searcher then ties the chain-walk one (no
  loss either way, but no notable gain). Document this regime
  honestly.
- **Pathological dict_suffix chains.** A constructed pattern set in
  which every node has long dict_suffix chains could blow up
  `total_flat_pids`. Add the print-and-bail diagnostic at build time
  (if `total_flat_pids > 64 × num_outputs`, refuse and fall back to
  chain-walk for that build).

## Related work pointer

This is folklore in finite-state machine engines (Hyperscan, RE2,
PCRE2 all use some variant). Within the laboratory's reading list,
the closest precedent is the ACISM (interleaved transition matrix)
representation described in Regéciová, Kolář & Milkovič 2021,
*"Pattern Matching in YARA: Improved Aho–Corasick Algorithm"* —
ACISM does for `goto_tbl` what flat outputs does for the emission
chain. Citing both in the related-work section frames this idea as a
natural extension of the same line of layout optimizations.

---
