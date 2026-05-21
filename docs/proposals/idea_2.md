# 2 · Idea: SIMD Gather DFA Traversal

> **Phase touched.** Search (hot loop, intra-thread).
> **Composes with.** Every existing chunked / dynamic / sharded searcher.
> **Status.** Not implemented.

## The idea in one paragraph

Walk `V` independent byte streams **in parallel within a single thread**,
one per SIMD lane (`V = 8` on AVX2, `V = 16` on AVX-512). Each lane keeps
its own `(state, byte_index)`; one fused vector-gather instruction
(`_mm256_i32gather_epi32` / `vpgatherdd`) loads `V` next-states from
`goto_tbl` in one go. The `V` streams are simply `V` adjacent
sub-ranges carved out of the worker's chunk, glued back together at the
end. Match emission stays scalar on the rare path (`AC_UNLIKELY`),
exactly as in `src/searchers/sequential.c`.

The motivation is purely algorithmic: the byte-by-byte AC inner loop has
a single dependent load per character (`state = goto_tbl[state*256+c]`)
that limits each core to one transition per memory dependency. SIMD
gather breaks that serial chain by running `V` independent state
trajectories that share no data dependencies. Sitaridi et al. (DaMoN'16,
*"SIMD-Accelerated Regular Expression Matching"*) report 1.7×–2× over
scalar on a single core using exactly this construction, and they also
show the **gain grows when the DFA spills out of cache** — useful here:
ideas 1 (sharding) and 3 (δ²) respond to cache *size*, while this idea
responds to per-byte *latency* regardless of where the DFA sits.

## Why it complements the existing searchers

| Existing axis                              | What it parallelizes |
|--------------------------------------------|----------------------|
| `pthread_chunked*`, `pthread_dynamic`      | across cores         |
| `pthread_block_cyclic`, `pthread_affinity` | across cores         |
| this idea                                  | **within** a core    |

Every byte still ends up touched by exactly one thread (the chunk owner)
— but that thread now does ≈ `V` transitions per gather instead of one
per load. The two parallelism axes multiply: a `T`-thread chunked
searcher equipped with `V`-wide SIMD has a theoretical ceiling of
`T × V × (scalar throughput)`. The dissertation question is how close to
that ceiling a real implementation gets and at what point memory
bandwidth saturates the product.

## Implementation hooks

Drop a single new file: `src/searchers/pthread_chunked_simd.c`.

```c
#include "ac_searcher.h"
#include "benchmark.h"
#include <immintrin.h>          /* guarded by __AVX2__ */
#include <pthread.h>

/* Lane count = AVX2 32-bit gather width. AVX-512 doubles this to 16
 * (use _mm512_i32gather_epi32). Keep V a compile-time constant so the
 * inner loop unrolls cleanly. */
#define V 8

typedef struct {
    int                    thread_id;
    const ac_automaton_t  *aut;
    const char            *text;
    size_t                 scan_start;
    size_t                 core_start;
    size_t                 core_end;
    ac_match_list_t        local;
    double                 seconds;
    int                    rc;
} worker_t;

static void *worker_main(void *arg);

/* Inside worker_main, after the scalar warm-up phase, run V sub-streams
 * in lockstep. Carve the owned range [core_start, core_end) into V
 * slices of ~equal size. Each slice has its own initial DFA state,
 * obtained by scalar warm-up over its first L-1 bytes (where
 * L = aut->max_pattern_len).
 *
 *   __m256i state_v = _mm256_loadu_si256((const __m256i*)init_states);
 *   __m256i pos_v   = _mm256_loadu_si256((const __m256i*)slice_starts);
 *
 *   for (size_t k = 0; k < slice_steps; k++) {
 *       // Load one byte per lane from text[pos_v[lane]] into a 32-bit lane.
 *       // (no native vpgather for byte loads -- use V scalar loads + insert,
 *       //  OR pre-stripe text into V parallel arrays at workspace time.)
 *       __m256i byte_v = load_one_byte_per_lane(text, pos_v);
 *
 *       // index_v = state_v * 256 + byte_v
 *       __m256i idx_v = _mm256_add_epi32(_mm256_slli_epi32(state_v, 8), byte_v);
 *
 *       // gather V next-states from goto_tbl
 *       state_v = _mm256_i32gather_epi32((const int *)aut->goto_tbl, idx_v, 4);
 *
 *       // detect any lane whose new state has outputs or a dict_suffix
 *       __m256i flag_v = lane_has_output(state_v, has_out_bitmap);
 *       int mask = _mm256_movemask_epi8(flag_v);
 *       if (AC_UNLIKELY(mask != 0)) {
 *           emit_for_lanes(&w->local, aut, state_v, pos_v, mask);
 *       }
 *
 *       pos_v = _mm256_add_epi32(pos_v, _mm256_set1_epi32(1));
 *   }
 *
 *   // drain the tail of each slice scalarly so we don't miss the last
 *   // (slice_size mod step) bytes per lane.
 */
```

Critical micro-decisions an implementer must make:

- **Lane data layout.** Two viable options; benchmark both:
  1. *Per-step scatter-load.* Issue `V` scalar `text[pos[lane]]` loads
     each step and pack into a vector. Simplest, lets you share the
     existing text buffer untouched, but every step costs `V` cache
     accesses.
  2. *Pre-striped text.* Once, at worker start-up, copy the owned
     range into `V` parallel byte arrays so lane `i` reads
     `striped[i][k]` linearly. Removes per-step scatter cost at the
     price of one extra pass over the text. Wins on large chunks.
- **Output detection.** Build a small **1-bit-per-state** bitmap at
  search start (`has_out[state] = (own_head[state] != AC_NIL ||
  dict_suffix[state] != AC_NIL)`) so the lane-output test is a single
  gather + non-zero compare. Cost: `num_states / 8` bytes, computed
  once per `search()` call.
- **Sub-slice warm-up cost.** Each of the `V` sub-slices needs its
  own `L−1` warm-up bytes. Total warm-up per chunk is now
  `V × (L−1)` bytes (vs. `L−1` for scalar chunked searchers). For
  tiny chunks this can swamp the gain, so add the same `text_len <
  threshold → fallback to scalar` guard you see at the top of
  `src/searchers/pthread_chunked.c::pt_search`.
- **Match ownership at lane boundaries.** Lanes inside a chunk are
  *not* like inter-chunk boundaries: every match found by every lane
  belongs to the owning worker (because all `V` lanes live inside
  `[core_start, core_end)`). The only ownership filter is the same
  inter-chunk one already in place. Do not double-filter.
- **Compile-time gating.** Compile this object file with `-mavx2` (a
  per-target Makefile rule, not a global flag bump). Guard with
  `#if defined(__AVX2__)`; provide a scalar fallback that is
  bit-identical to `sequential.c` so the searcher always links and
  always passes `make test`, even when AVX2 is absent.

## Correctness validation

- **`make test`** — must remain green for thread counts `{1, 2, 3, 4, 7,
  8}` across all five existing cases. The *dense matches on small
  alphabet* case (`make_repeated("a", 1000)`) is the SIMD stressor:
  every byte emits and exercises the scalar fallback path. The
  *boundary* case stresses sub-slice warm-up alignment.
- **`make tsan`** — required for the multi-threaded form. SIMD itself
  is single-threaded; TSan only catches inter-thread races. As long as
  each worker's own `worker_t` is correctly cache-padded and the
  automaton stays read-only, this idea introduces no new race surface.
- **Per-lane differential test.** Add a deterministic micro-test that
  runs the SIMD searcher with `V = 1` (i.e. emulated via the scalar
  fallback) and confirms byte-identical match lists vs. `sequential.c`.
  Then run with `V = 8` and confirm `lists_equal`.
- **Bitmap consistency assertion.** Once per call (debug build only),
  walk every state and verify `has_out[state]` agrees with
  `own_head[state] != AC_NIL || dict_suffix[state] != AC_NIL`.
- **Bounds sanity.** Add an assertion (debug only) that
  `0 <= state_v[lane] < aut->num_states` after every gather, before
  use. A miscalculated index quietly reads garbage from `goto_tbl`
  and produces wrong matches that TSan cannot detect.

## Metrics to report

- **Per-core throughput sweep.** Fix `T = 1` and run with
  `V ∈ {1 (scalar), 4, 8}` (and 16 if AVX-512 is available). Isolates
  the SIMD effect from threading. Report MB/s and cycles per byte
  (`perf stat -e cycles,instructions` divided by `bytes_scanned`).
- **Composition curve.** Run `pthread_chunked_simd` at `T ∈
  {1, …, nproc}` against `pthread_chunked_v2` at the same `T` on the
  same dictionary. Hypothesis to confirm or refute: throughput stays
  roughly `(scalar curve) × V` until memory bandwidth saturates.
- **DFA cache-residency sensitivity.** Repeat for {Snort-100,
  Snort-1k, Snort full, ET full}. The literature predicts the SIMD
  gain *grows* as the DFA spills to DRAM (because gathers hide
  latency better than serial dependent loads). Report the gain ratio
  per dictionary size to support or refute this on the test platform.
- **Build cost of the bitmap.** Report milliseconds to compute
  `has_out[]`. Should be ≤ 1 ms even for the full ET dictionary, so
  it is safe to rebuild per `search()` call.

## Failure modes / when it loses

- **Heavy match density.** Every emission falls back to scalar. With
  per-step fallback rates above ~30 %, the SIMD win evaporates because
  the lane-mask test, the fallback dispatch, and the scalar emission
  loop together cost more than the gather saves. Document the
  cross-over with the dense-`a` test case.
- **Tiny chunks.** When `core_end - core_start` is small enough that
  `V × (L−1)` warm-up dominates, the scalar fallback at the top of
  the worker is the correct choice. Make the threshold a tunable
  constant near the top of the file.
- **Hosts without AVX2.** Falls back to scalar; the searcher still
  registers, just at the speed of `pthread_chunked_v2`. The TCC can
  present AVX2-host numbers as the contribution and the AVX2-less
  case as a free-lunch fallback.

## Related work pointer

Sitaridi, Polychroniou & Ross, *"SIMD-Accelerated Regular Expression
Matching"*, DaMoN '16 (co-located with ACM SIGMOD). Section 6 (short-
circuit / non-lockstep gather) is the closest direct precedent. The
paper formally notes (§3) that Aho–Corasick reduces to minimal-DFA
traversal once failure links are encoded into `goto_tbl` — which is
exactly what `src/ac_automaton.c::ac_automaton_build` already does — so
its results transfer cleanly.

---
