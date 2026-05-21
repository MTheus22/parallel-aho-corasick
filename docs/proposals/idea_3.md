# 3 · Idea: Multi-Step (k-byte) Transition Tables — δᵏ

### The idea in one paragraph

Precompute `δ²[state][byte_pair]` such that one indexed load consumes
**two** bytes of input. Storage is `num_states × 65536 × 4` bytes,
which is feasible only when `num_states ≲ 4 000` (≈ 1 GiB ceiling).
The hot loop halves its instruction count and halves the dependent-load
chain length — a direct attack on the per-byte serial bottleneck that
SIMD only mitigates indirectly. δ³ is theoretically possible but the
table explodes (`num_states × 2²⁴`); skip it.

This idea is a *clean algorithmic win* with a well-defined applicability
ceiling, which makes it an excellent dissertation chapter even if it
only beats the baseline on small dictionaries.

### Implementation hooks

New file: `src/searchers/sequential_delta2.c` (sequential first; promote
to threaded after correctness is solid).

```c
/* Build δ²: for each state s and each pair (a, b),
 *     delta2[s * 65536 + a * 256 + b] = goto_tbl[goto_tbl[s*256+a]*256+b];
 *
 * Plus an emission bitmap parallel to delta2:
 *     emit_mask[s * 65536 + a * 256 + b] =
 *         own_head[s1] != NIL || dict_suffix[s1] != NIL  // intermediate
 *       | own_head[s2] != NIL || dict_suffix[s2] != NIL; // final
 *   where s1 = goto_tbl[s*256+a], s2 = delta2[s * 65536 + a*256 + b].
 *
 * When emit_mask is set, drop into the scalar single-byte path for the
 * pair to find *which* byte (or both) emitted, then continue in delta2. */

static int build_delta2(const ac_automaton_t *aut,
                        int32_t **out_delta2, uint8_t **out_emit_mask);
```

Critical decisions:

- **Footprint guard.** Refuse to build if `num_states × 65536 × 5` (4 B
  table + 1 B mask) exceeds, say, 1 GiB. Fall back to `sequential` on
  large dictionaries; print one diagnostic line.
- **Build cost.** O(`num_states × 65536`). For 1 000 states that is
  65 M operations — about 50 ms — well below the per-iteration scan
  time on any sensible corpus.
- **Combination with chunking.** δ² changes the warm-up math. With
  pair-stepping, after `(L−1)/2 + 1` pair-steps the worker is in the
  globally-correct state. Round up for safety; verify with the boundary
  test case.

### Correctness validation

- **`make test`.** Required across all five existing test cases. The
  “overlap” and “boundary” cases will catch any off-by-one in pair
  alignment at chunk start.
- **`make tsan`.** Required only if the searcher is multi-threaded
  (recommended once the sequential variant passes).
- **Direct table check.** Add a unit test asserting that for every
  reachable `s` and every pair `(a, b)`,
  `delta2[s * 65536 + a * 256 + b] == goto_tbl[goto_tbl[s*256+a]*256+b]`.

### Metrics to report

- **Throughput vs. dictionary size.** Same sweep as §1 (Snort-100 →
  Snort-10k → Snort full → ET). Expect a clean cross-over: δ² wins
  while it’s buildable, the footprint guard kicks in beyond ~3 000
  states.
- **Build time vs. search time.** δ² has a non-trivial build cost.
  Report `build_time / total_runtime` for the typical TCC corpus
  (Enron, 1.4 GiB). If build dominates, the technique only wins for
  long-running, repeated scans.

### Failure modes / when it loses

- **Cache footprint.** A 1 000-state automaton already uses
  `1 000 × 65 536 × 4 ≈ 256 MiB` for δ² alone. This is *much* worse
  for cache than the original `goto_tbl` (≈ 1 MiB at the same state
  count). The technique only helps when *the inner loop is
  compute-bound* (small dict) and *not when it’s memory-bound* (large
  dict). The cross-over experiment in §1 will pin this down.