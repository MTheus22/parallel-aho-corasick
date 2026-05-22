# Parallel Aho–Corasick: Roadmap of Promising Ideas

> **Audience.** AI coding agents (and human reviewers) tasked with
> extending the laboratory. This file is intentionally in English
> because the codebase, identifiers and the dissertation text already
> use English in the implementation layer; reuse it verbatim in
> `src/searchers/*.c` and `src/ac_automaton.c` comments.
>
> **Scope.** Forward-looking. Nothing here is implemented. The eight
> searchers currently registered already cover the *most obvious* axis
> of parallelism — data parallelism with static/dynamic chunking on a
> unified read-only DFA during the **search** phase. The ideas below
> cover the axes the existing work does **not**: dictionary-level
> parallelism, intra-thread vectorisation, multi-step transitions,
> parallel **automaton construction**, and a cross-cutting layout
> change that benefits every searcher at once. The framing is purely
> algorithmic — concrete hardware details (cache sizes, P/E-core
> asymmetry, NUMA topology) belong in the measurement plan of each
> idea, not in the design.
>
> **How to use this file.** Pick one `idea_<N>.md`, read its
> *Implementation hooks* block, drop the indicated files,
> validate with `make test`, `make tsan`, and the benchmark protocol
> in `docs/architecture/benchmark-protocol.md`. Each idea page is
> self-contained: an agent should not need to read the others to
> implement one.

---

## 0 · Phases of Aho–Corasick the existing code covers vs. does not

```
+-------------------------------------------------------------------+
|  Phase                              | Current state              |
+-------------------------------------------------------------------+
|  1. Pattern loading                 | Sequential, I/O bound.     |
|                                     | Not interesting.           |
|  2. Trie insertion                  | Sequential (master).       |
|                                     | Not parallelised.          |
|  3. BFS: fail + goto + dict_suffix  | Sequential (master).       |
|                                     | Not parallelised.          |
|  4. Search (hot loop)               | Parallelised across 8      |
|                                     | searchers; many variants.  |
|  5. Match emission (per state)      | Chain-walk over            |
|                                     | dict_suffix + outputs;     |
|                                     | unchanged since baseline.  |
|  6. Merge of thread-local lists     | Sequential, < 1 % total.   |
|                                     | Skipped intentionally.     |
+-------------------------------------------------------------------+
```

Phase 4 is *saturated* with existing searchers. The roadmap is built
to attack phases **2 + 3** (build), **4** (with axes nobody has
tried), and **5** (a layout change that lifts every search-phase
variant simultaneously).

---

## 1 · Non-negotiable invariants

Every new file proposed in the roadmap must respect:

1. **Read-only automaton during search.** `goto_tbl`, `own_out_head`,
   `dict_suffix`, `outputs`, `patterns`, and any new flat arrays
   added by idea 5 are written **only** inside `ac_automaton_build()`
   (or its replacement) and never thereafter. The build phase may
   freely mutate; the search phase may not.
2. **Build-phase atomics are tolerated; hot-loop atomics are not.**
   Idea 4 (parallel BFS) uses an `atomic_size_t` to reserve frontier
   slots — that is fine because it fires once per real-child
   discovery during build, not per byte. The search hot loop remains
   strictly free of `pthread_mutex_*`, `pthread_rwlock_*`, atomics,
   and any mutable shared state.
3. **Matches are thread-local until `pthread_join`.** Search workers
   each own a private `ac_match_list_t`. The master concatenates
   after the join. No exception.
4. **Overlap ≥ `max_pattern_len - 1` for any chunked search.**
   Skipping it loses matches that straddle boundaries
   (`docs/architecture/parallelism.md` §“Why L−2 or smaller does not
   work”). Ideas 2, 3 and any composition of 1+chunking inherit this
   rule.
5. **Match ownership is disjoint.** A match with `end_pos = p` is
   reported by **exactly one** thread/task. Pattern sharding (idea
   1) handles this by making shards disjoint pid-sets; data-chunking
   handles it via `[core_start, core_end)`.
6. **The `ac_searcher_t::search` signature does not change.** Extra
   precomputed structures (sub-automata for idea 1, δ² for idea 3,
   flat outputs for idea 5) are either built lazily inside
   `search()` on first call and cached in a `static`, or attached to
   `ac_automaton_t` itself (idea 5 does this).

A new component that violates any of (1)–(6) will either fail
`make test` on the boundary / dense-match cases or produce TSan
warnings on `make tsan`.

---

## 2 · The five proposed ideas

> **All ideas implemented.** Idea 1 was the last; see the row below
> for its searcher doc. The line above used to point to the next
> idea to implement; it is now retired.

| #  | Idea                                              | Phase          | Page                     | Order | Status                              |
|----|---------------------------------------------------|----------------|--------------------------|-------|-------------------------------------|
| 1  | Pattern sharding (dictionary-level parallelism)   | Search         | [`idea_1.md`](idea_1.md) | 4th   | ✅ Implemented 2026-05-21 (see [`../searchers/pattern_sharded.md`](../searchers/pattern_sharded.md)) |
| 2  | SIMD gather DFA traversal                         | Search         | [`idea_2.md`](idea_2.md) | —     | ❌ Descartada (ver §6)              |
| 3  | Multi-step (k-byte) transition tables — δᵏ        | Search         | [`idea_3.md`](idea_3.md) | 3rd   | ✅ Implemented 2026-05-21 (5353383) |
| 4  | Parallel BFS construction of fail/goto/dict_suffix| Build          | [`idea_4.md`](idea_4.md) | 2nd   | ✅ Implemented 2026-05-21 (82c0f10) |
| 5  | Eager dict_suffix flattening (flat output table)  | Build + Search | [`idea_5.md`](idea_5.md) | 1st   | ✅ Implemented 2026-05-20 (e78045c) |

**Brief one-paragraph summaries** (the full agent-ready spec lives in
each `idea_<N>.md`):

### 1 · Pattern sharding
Partition the **dictionary** (not the text). Build `K` independent
sub-automata over disjoint pattern subsets; spawn `K` workers, each
scanning the entire input with its own sub-automaton; merge match
lists. No inter-chunk overlap is needed — every byte is touched by
every worker, just through a much smaller DFA. The win comes from
sub-automata that individually fit in cache when the unified one
does not.

### 3 · Multi-step (k-byte) transition tables — δᵏ
Precompute `δ²[state][byte_pair]` so one indexed load consumes two
input bytes instead of one. Halves the dependent-load chain length.
Only buildable when `num_states ≲ 4 000` (storage limit); a clean
cross-over experiment by dictionary size makes this a publishable
chapter on its own.

### 4 · Parallel BFS construction of fail / goto / dict_suffix
Replace the sequential BFS in `ac_automaton_build` with a level-
synchronous parallel BFS. At depth `d`, every state depends only on
states at depth `< d`, so the level is embarrassingly parallel given
a barrier between levels. This is the only roadmap idea that attacks
the **build phase**, which is currently 100 % sequential. No
surveyed paper does this for software AC.

### 5 · Eager dict_suffix flattening (flat output table)
At the end of build, precompute for every state `s` the flat array
of pattern IDs that AC would emit on arrival at `s`. The search hot
loop replaces a nested chain-walk
(`while (l != NIL) for (o = …; …) {} l = dict_suffix[l]`) with one
contiguous linear scan of `flat_pids[…]`. This is a **layout
transformation** that every existing and proposed searcher
inherits without code changes.

---

## 3 · Recommended implementation order

The ordering minimises rework: each step either has zero
dependencies on the others or relies on the algorithmic / data-layout
changes already in place.

1. **Idea 5 (flat outputs).** ✅ Done. Layout change; every other idea inherits it.
2. **Idea 4 (parallel BFS construction).** ✅ Done. Attacks the build phase.
3. **Idea 3 (δ² sequential).** ✅ Done. Algorithmic acceleration with a clean
   applicability ceiling (`num_states ≲ 4 000`); useful as a comparison point
   that demonstrates why single-thread tricks have limited reach on large dicts.
4. **Idea 1 (pattern sharding).** ✅ Done. The only idea that directly
   attacks the cache-blowout regime (large dictionaries >> L3) that is the
   central performance problem for IDS-scale workloads. Three policies
   registered (`pattern_sharded`, `pattern_sharded_lpt`, `pattern_sharded_prefix`).
   On Snort full + simplewiki (12-core x86_64), `pattern_sharded_prefix` at
   K=8 delivers **1.19×** speedup vs. `sequential`; round-robin and LPT
   policies regress on every dictionary size tested. The headline cross-
   over is empirically dictionary-size + policy dependent — see
   [`../searchers/pattern_sharded.md`](../searchers/pattern_sharded.md)
   "Headline benchmark" for the full table.

The dissertation arc is: layout optimisation → parallel build → algorithmic
single-thread ceiling → dictionary-level parallelism that defeats the cache wall.
Each stage is a complete, independently-measurable result. Idea 2 was removed
from this order; see §6.

---

## 4 · Cross-cutting evaluation harness checklist

For each new file produced (whether a new searcher or a build-phase
change), produce these artifacts before claiming success:

- `make` — release build clean under the existing warning set
  (`-Wall -Wextra -Wpedantic -Wshadow -Wstrict-prototypes
  -Wmissing-prototypes -Wpointer-arith -Wcast-align`).
- `make test` — green on **every** registered searcher across
  thread counts `{1, 2, 3, 4, 7, 8}`. Add a new test case if the
  technique has corner conditions not exercised by the existing
  five (e.g. a long-chain `dict_suffix` test for idea 5, a fixed
  random-seed differential test for idea 2).
- `make tsan` — no warnings under the same test set. Mandatory for
  anything threaded; mandatory for ideas 4 and 5 even though the
  search phase is read-only, because the **build phase** now has
  threading.
- One row added to `tcc_notes/tldr_metricas.md` (or equivalent
  results table) with: name, dictionary, corpus, threads, build
  time, mean search time, MB/s, Gbps, speedup vs. `sequential`.
  Plus a "phase" column distinguishing search/build wins.
- One sweep script committed under `scripts/` matching the
  protocol in `docs/architecture/benchmark-protocol.md`
  (`--warmup ≥ 1`, `--iters ≥ 5`, CPU governor pinned to
  `performance`, taskset-pinned threads, large enough input to
  amortise build).
- One `docs/searchers/<name>.md` page (in Portuguese, per
  `CLAUDE.md` §"Convenções de código") describing the technique,
  the invariants it newly relies on, and the headline numbers.

For ideas that touch the build phase (4, 5), the result table must
include build seconds, not only search seconds — otherwise the
contribution disappears in the aggregate.

---

## 5 · Branch model and cumulative integration

**All five ideas land on a single branch as additive commits — no
worktrees, no per-idea branches.** This is a deliberate architectural
choice, not a convenience. Three rules make it work:

1. **Search-phase ideas (1, 3) only add new files under
   `src/searchers/`.** They never modify existing searcher files.
   The plug-in registry (`__attribute__((constructor))` →
   `ac_searcher_register`) picks them up automatically. Every prior
   searcher remains in the registry and remains testable by
   `make test` after every new commit.
2. **Idea 5 (flat outputs) extends `ac_automaton_t` additively.**
   New fields (`flat_offset`, `flat_count`, `flat_pids`,
   `total_flat_pids`) are appended to the struct; existing fields
   (`own_out_head`, `dict_suffix`, `outputs`) stay in place. Old
   searchers continue to work because they read what they already
   read; new searchers read the flat arrays. The build pass that
   fills the flat arrays runs unconditionally at the end of
   `ac_automaton_build()` — it is cheap (`O(num_outputs)`).
3. **Idea 4 (parallel BFS construction) is opt-in.** Add a new
   function `ac_automaton_build_par()` alongside the existing
   `ac_automaton_build()`; let `src/main.c` choose between them via
   an environment variable (e.g. `AC_BUILD_PARALLEL=1`) or a new
   CLI flag. **Do not delete or replace the sequential build** —
   the dissertation needs the sequential build path to remain
   callable for before/after build-time comparison.

The consequence is that after every commit on this branch:

- `make test` runs **all** registered searchers, including every
  previously-implemented one, against `sequential`. Any regression
  in idea N caused by idea N+1's commit fails the test suite
  immediately. Additivity is enforced by the test harness, not by
  policy.
- `git checkout <commit-hash>` lets the human re-measure any
  intermediate state without rebuilding history.
- No merge conflicts, no rebase-related hash churn, no per-idea
  branch bookkeeping.

**When would worktrees be justified?** Only if two agents had to
develop two ideas in parallel and the project's structure forced
them to touch the same file. Neither condition holds here — the
ideas are additive and the loop runs one agent at a time. Worktrees
would add complexity for no benefit.

**Commit cadence.** One atomic commit per idea (after build, test,
TSan, measurement and doc-page are all done). The commit body
references the corresponding `idea_<N>.md`. If an idea naturally
splits into independent sub-steps (e.g. idea 5: first the
build-phase changes, then the migrated searcher), two commits are
acceptable, but both go on the same branch and the second is the
one that flips the §2 status to "Implemented".

---

## 6 · Out of scope (deliberately, with one-line justification)

| Idea                                | Why not in scope                                                                                          |
|-------------------------------------|-----------------------------------------------------------------------------------------------------------|
| SIMD gather DFA traversal (idea 2)  | Intra-thread vectorisation, not multi-core parallelism. Gain peaks when the DFA fits in L1/L2 — exactly the regime that is *not* the TCC's focus. At IDS scale (ET ≫ L3), memory bandwidth is the bottleneck; gathers do not increase bandwidth. High implementation cost for a result that regresses in the most important benchmark. |
| Adaptive/meta strategy selector     | A dispatcher does not accelerate AC; it just picks among accelerators. Useful as future engineering, not algorithm. |
| Parallel trie insertion             | Insertion cost is `O(total_pattern_length)`; BFS cost is `O(num_states × 256)`. BFS dominates → idea 4 instead. |
| NUMA-aware text replication         | Pure environment tuning; the dissertation's contribution should be portable across machine classes.       |
| Hierarchical / tree merge of matches| Final merge is < 1 % of total time. Even a perfect parallel merge buys < 1 %.                             |
| Work-stealing deques                | Equivalent in practice to `pthread_dynamic` for a static, monotonic workload. No new theoretical room.    |
| PFAC (failure-less) on CPU          | Designed for GPU SIMT; per-byte overhead of K independent traversals is prohibitive on CPU. Cite in related work as contrast. |
| Compressed automaton (ACISM)        | Storage optimisation, not a parallelism contribution. Cite Regéciová 2021 in related work.                |
| Replacing AC with Shift-Or / FDR    | Not Aho–Corasick parallelism; out of TCC scope. Cite Harry (Xu et al.) as contrast.                       |

---

## 7 · References (mapped to `tcc_notes/related_work_summaries/`)

- **Pattern sharding (idea 1)** has no direct precedent in the
  surveyed papers. Closest in spirit to the head/body split of
  HBM / FHBM (Lee & Yang 2017), but applied to threads rather than
  automaton tiers.
- **SIMD gather DFA traversal (idea 2)** — descartada (ver §6).
  Referência de contraste para trabalhos relacionados: Sitaridi,
  Polychroniou & Ross, DaMoN '16, *"SIMD-Accelerated Regular
  Expression Matching"*.
- **Multi-step transition tables (idea 3)** — folklore in
  finite-state engines; the clearest treatment in the lab's reading
  list is Sitaridi et al. §4 (DFA representation trade-offs).
- **Parallel BFS construction (idea 4)** — level-synchronous BFS
  is canonical in graph processing (Beamer, Buluç & Patterson
  SC '12), but applying it to AC construction is, as far as the
  systematic review found, novel for software AC: not in Lee 2017,
  Qu 2016, Thambawita 2018, or Lin 2010.
- **Flat output tables (idea 5)** — folklore in production engines
  (Hyperscan, RE2). Closest precedent in the reading list is the
  ACISM transition representation in Regéciová, Kolář & Milkovič
  2021 (*"Pattern Matching in YARA"*).

---

*End of roadmap. Treat this file as living — once an idea is
implemented, replace its row in §2 with a one-line pointer to the new
`docs/searchers/<name>.md` or to the relevant `src/` change, and
update the "Phases" table in §0.*
