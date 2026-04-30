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
} ac_automaton_t;

/* Construct from an array of (pattern, length) pairs. Patterns may be
 * arbitrary byte sequences; duplicates are accepted and produce one
 * output per occurrence. */
int  ac_automaton_build(ac_automaton_t *aut,
                        const char *const *patterns,
                        const size_t *lengths,
                        size_t num_patterns);

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
