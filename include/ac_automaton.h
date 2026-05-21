#ifndef AC_AUTOMATON_H
#define AC_AUTOMATON_H

#include "ac_common.h"

/* ---- Read-only Aho-Corasick automaton ----------------------------------
 * After ac_automaton_build() returns AC_OK the structure is strictly
 * read-only and is safe to share between an unbounded number of threads
 * without any synchronization. The construction phase (single-threaded,
 * "master") fills in the transition table, failure links and output
 * chains; the search phase (sequential or parallel) only reads them.
 *
 * Memory layout choices:
 *   - `goto_tbl` is a flat int32_t[num_states * 256] storing the full
 *     deterministic AC transition function (delta). One step is one
 *     indexed load -> excellent cache behaviour on hot inner loops.
 *   - Outputs are stored as a chain. `own_out_head[s]` is the head of
 *     pattern IDs whose terminal node is s. `dict_suffix[s]` is the
 *     nearest ancestor in the failure chain that has its own outputs
 *     (or AC_NIL); walking it lets a search emit every pattern that
 *     ends at the current position without retraversing fail links per
 *     character.
 * ----------------------------------------------------------------------- */

typedef struct {
    int32_t pattern_id;
    int32_t next;            /* next entry in chain, AC_NIL = end */
} ac_output_entry_t;

typedef struct {
    char   *text;            /* not NUL-terminated, owned */
    int32_t length;
} ac_pattern_t;

typedef struct ac_automaton {
    /* Transition table: goto_tbl[state * 256 + c]. Always valid (root has
     * a self-loop on every byte not starting a pattern). */
    int32_t          *goto_tbl;
    int32_t           num_states;

    /* Per-state output information. */
    int32_t          *own_out_head;   /* size: num_states */
    int32_t          *dict_suffix;    /* size: num_states; AC_NIL or state */

    /* Output entry arena (linked-list nodes). */
    ac_output_entry_t *outputs;
    int32_t            num_outputs;

    /* Pattern dictionary (used for reporting matches). */
    ac_pattern_t      *patterns;
    int32_t            num_patterns;

    /* Cached scalars. */
    int32_t            max_pattern_len;
    int32_t            min_pattern_len;

    /* ---- Idea 5: eager dict_suffix flattening ------------------------
     * Per-state flat arena of pattern_ids that AC would emit on arrival
     * at state s (own outputs first, then the dict_suffix ancestor
     * chain in ancestor order -- same multiset and same order as the
     * (own_out_head, dict_suffix, outputs) chain walk). Populated at
     * the end of ac_automaton_build(); strictly read-only thereafter,
     * just like every other field on this struct.
     *
     * Searchers that read this layout (sequential_flat,
     * pthread_chunked_flat, ...) replace two levels of dependent
     * pointer chases with one contiguous linear scan over flat_pids.
     * Chain-walking searchers ignore these fields and keep working
     * unchanged; both layouts are kept in the struct so the
     * dissertation can A/B them on the same automaton.
     *
     *   flat_pids[ flat_offset[s] .. flat_offset[s] + flat_count[s] )
     *     == { pid emitted by sequential AC on arrival at state s }
     */
    int32_t           *flat_offset;     /* size: num_states           */
    int32_t           *flat_count;      /* size: num_states           */
    int32_t           *flat_pids;       /* size: total_flat_pids      */
    int32_t            total_flat_pids;
} ac_automaton_t;

/* Construct from an array of (pattern, length) pairs. Patterns may be
 * arbitrary byte sequences; duplicates are accepted and produce one
 * output per occurrence. */
int  ac_automaton_build(ac_automaton_t *aut,
                        const char *const *patterns,
                        const size_t *lengths,
                        size_t num_patterns);

/* Parallel-build alternative to ac_automaton_build: trie insertion
 * stays sequential (it is small), but the BFS that computes
 * fail/goto_tbl/dict_suffix runs level-synchronously across
 * `num_threads` worker threads. The produced ac_automaton_t is
 * byte-identical to the one returned by ac_automaton_build under all
 * patterns/threads combinations exercised by tests/test_correctness.c.
 *
 *   num_threads <= 1  delegates to ac_automaton_build verbatim, so the
 *                     degenerate path is exactly the sequential build.
 *
 * Idea 4 from docs/proposals/idea_4.md; see also
 * docs/architecture/parallel-build.md. Selected at the CLI by setting
 * the AC_BUILD_PARALLEL=1 environment variable. */
int  ac_automaton_build_par(ac_automaton_t *aut,
                            const char *const *patterns,
                            const size_t *lengths,
                            size_t num_patterns,
                            int num_threads);

void ac_automaton_destroy(ac_automaton_t *aut);

/* Single deterministic transition step. Inlined for hot loops. */
AC_INLINE int32_t ac_step(const ac_automaton_t *AC_RESTRICT aut,
                          int32_t state, uint8_t c)
{
    return aut->goto_tbl[(size_t)state * AC_ALPHABET_SIZE + c];
}

/* Simple text-file pattern loader: one pattern per line, blank lines and
 * leading '#' lines ignored. Returns AC_OK or an AC_E_* code. */
int  ac_patterns_load_file(const char *path,
                           char ***out_patterns,
                           size_t **out_lengths,
                           size_t *out_count);

void ac_patterns_free(char **patterns, size_t *lengths, size_t count);

/* Diagnostics. */
size_t ac_automaton_memory_bytes(const ac_automaton_t *aut);

#endif /* AC_AUTOMATON_H */
