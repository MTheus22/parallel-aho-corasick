/* Correctness harness: every registered searcher must produce the same
 * (sorted) match set as the sequential baseline on a battery of inputs.
 * If any future parallel variant fails this test, the laboratory will
 * refuse to recommend it. */

#include "ac_automaton.h"
#include "ac_match.h"
#include "ac_searcher.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int build_aut(ac_automaton_t *aut, const char *const *pats, size_t n)
{
    size_t *lens = malloc(n * sizeof(size_t));
    for (size_t i = 0; i < n; i++) lens[i] = strlen(pats[i]);
    int rc = ac_automaton_build(aut, pats, lens, n);
    free(lens);
    return rc;
}

static int build_aut_par(ac_automaton_t *aut, const char *const *pats, size_t n, int nthreads)
{
    size_t *lens = malloc(n * sizeof(size_t));
    for (size_t i = 0; i < n; i++) lens[i] = strlen(pats[i]);
    int rc = ac_automaton_build_par(aut, pats, lens, n, nthreads);
    free(lens);
    return rc;
}

/* Byte-equality of two automata. Used to validate that
 * ac_automaton_build_par (idea 4) produces an automaton identical to
 * ac_automaton_build under all thread counts. The parallel BFS uses
 * atomic_fetch_add to reserve frontier slots, so the order in which
 * states appear inside frontier_next is non-deterministic -- but the
 * resulting (goto_tbl, fail-derived dict_suffix, outputs, own_out_head)
 * values are completely determined by the trie and depth ordering,
 * which IS deterministic. Hence the byte compare must succeed. */
static int automata_byte_equal(const ac_automaton_t *a, const ac_automaton_t *b)
{
    if (a->num_states      != b->num_states)      return 0;
    if (a->num_outputs     != b->num_outputs)     return 0;
    if (a->num_patterns    != b->num_patterns)    return 0;
    if (a->max_pattern_len != b->max_pattern_len) return 0;
    if (a->min_pattern_len != b->min_pattern_len) return 0;
    if (a->total_flat_pids != b->total_flat_pids) return 0;
    size_t n = (size_t)a->num_states;
    if (memcmp(a->goto_tbl,     b->goto_tbl,     n * AC_ALPHABET_SIZE * sizeof(int32_t)) != 0) return 0;
    if (memcmp(a->own_out_head, b->own_out_head, n * sizeof(int32_t)) != 0) return 0;
    if (memcmp(a->dict_suffix,  b->dict_suffix,  n * sizeof(int32_t)) != 0) return 0;
    if (memcmp(a->flat_offset,  b->flat_offset,  n * sizeof(int32_t)) != 0) return 0;
    if (memcmp(a->flat_count,   b->flat_count,   n * sizeof(int32_t)) != 0) return 0;
    if (a->total_flat_pids > 0 &&
        memcmp(a->flat_pids, b->flat_pids,
               (size_t)a->total_flat_pids * sizeof(int32_t)) != 0) return 0;
    if (a->num_outputs > 0 &&
        memcmp(a->outputs, b->outputs,
               (size_t)a->num_outputs * sizeof(ac_output_entry_t)) != 0) return 0;
    return 1;
}

static int lists_equal(ac_match_list_t *a, ac_match_list_t *b)
{
    if (a->count != b->count) return 0;
    ac_match_list_sort(a);
    ac_match_list_sort(b);
    for (size_t i = 0; i < a->count; i++) {
        if (a->data[i].end_pos    != b->data[i].end_pos ||
            a->data[i].pattern_id != b->data[i].pattern_id) return 0;
    }
    return 1;
}

static int run_case(const char *label,
                    const char *const *pats, size_t npats,
                    const char *text, size_t text_len)
{
    ac_automaton_t aut;
    if (build_aut(&aut, pats, npats) != AC_OK) {
        fprintf(stderr, "[%s] build failed\n", label);
        return 0;
    }

    /* Idea 4 equivalence check: ac_automaton_build_par must produce a
     * byte-identical automaton at every thread count we care about. A
     * mismatch is a build-phase bug; surface it before running any
     * searcher (otherwise downstream divergences would obscure the
     * real cause). Skip nthreads <= 1 because that path delegates to
     * the sequential build and the comparison is trivially true. */
    {
        int par_threads[] = {2, 3, 4, 7, 8};
        size_t npt = sizeof(par_threads) / sizeof(par_threads[0]);
        int build_par_ok = 1;
        for (size_t i = 0; i < npt; i++) {
            ac_automaton_t aut_par;
            int rc_par = build_aut_par(&aut_par, pats, npats, par_threads[i]);
            if (rc_par != AC_OK) {
                fprintf(stderr, "[%s] build_par(t=%d) failed: %d\n",
                        label, par_threads[i], rc_par);
                build_par_ok = 0;
                ac_automaton_destroy(&aut_par);
                break;
            }
            if (!automata_byte_equal(&aut, &aut_par)) {
                fprintf(stderr,
                        "[%s] build_par(t=%d) diverged from sequential build\n",
                        label, par_threads[i]);
                build_par_ok = 0;
                ac_automaton_destroy(&aut_par);
                break;
            }
            ac_automaton_destroy(&aut_par);
        }
        if (!build_par_ok) {
            ac_automaton_destroy(&aut);
            return 0;
        }
        printf("[%s] build_par OK across t={2,3,4,7,8}\n", label);
    }

    ac_match_list_t baseline;
    ac_match_list_init(&baseline);
    const ac_searcher_t *seq = ac_searcher_find("sequential");
    assert(seq);
    ac_searcher_config_t cfg = (ac_searcher_config_t){0};
    if (seq->search(&aut, text, text_len, &cfg, &baseline, NULL, NULL) != AC_OK) {
        fprintf(stderr, "[%s] sequential failed\n", label);
        ac_automaton_destroy(&aut);
        return 0;
    }

    int ok = 1;
    /* Compare every other searcher across a few thread counts. */
    int thread_counts[] = {1, 2, 3, 4, 7, 8};
    size_t ntc = sizeof(thread_counts)/sizeof(thread_counts[0]);

    for (size_t i = 0; i < ac_searcher_count(); i++) {
        const ac_searcher_t *s = ac_searcher_at(i);
        if (s == seq) continue;
        for (size_t j = 0; j < ntc; j++) {
            ac_searcher_config_t pc = cfg;
            pc.num_threads = thread_counts[j];
            ac_match_list_t got;
            ac_match_list_init(&got);
            int rc = s->search(&aut, text, text_len, &pc, &got, NULL, NULL);
            if (rc != AC_OK) {
                fprintf(stderr, "[%s] %s (t=%d) returned %d\n",
                        label, s->name, thread_counts[j], rc);
                ok = 0;
            } else if (!lists_equal(&got, &baseline)) {
                fprintf(stderr,
                        "[%s] %s (t=%d) diverged: got %zu matches vs baseline %zu\n",
                        label, s->name, thread_counts[j], got.count, baseline.count);
                ok = 0;
            } else {
                printf("[%s] %s t=%-2d OK (%zu matches)\n",
                       label, s->name, thread_counts[j], got.count);
            }
            ac_match_list_free(&got);
        }
    }

    ac_match_list_free(&baseline);
    ac_automaton_destroy(&aut);
    return ok;
}

static char *make_repeated(const char *unit, size_t reps)
{
    size_t L = strlen(unit);
    char *buf = malloc(L * reps + 1);
    for (size_t i = 0; i < reps; i++) memcpy(buf + i*L, unit, L);
    buf[L*reps] = 0;
    return buf;
}

int main(void)
{
    int all_ok = 1;

    /* ---- Case 1: classic AC textbook example -------------------------- */
    {
        const char *pats[] = {"he", "she", "his", "hers"};
        const char *text = "ushers say she is hers and his";
        all_ok &= run_case("classic", pats, 4, text, strlen(text));
    }

    /* ---- Case 2: overlapping patterns -------------------------------- */
    {
        const char *pats[] = {"abc", "bca", "cab", "abcabc", "bcabca"};
        const char *text = "xxabcabcabcxxbcabcabcabcyy";
        all_ok &= run_case("overlap", pats, 5, text, strlen(text));
    }

    /* ---- Case 3: pattern at boundary (long pattern, many threads) ----
     * Stresses the overlap logic: the long pattern must be detected even
     * when split across many tiny core ranges. */
    {
        const char *pats[] = {"NEEDLE_IN_THE_HAYSTACK_PATTERN", "the", "in"};
        char *text = make_repeated("xxNEEDLE_IN_THE_HAYSTACK_PATTERNyy in the haystack ", 50);
        all_ok &= run_case("boundary", pats, 3, text, strlen(text));
        free(text);
    }

    /* ---- Case 4: dense matches on small alphabet --------------------- */
    {
        const char *pats[] = {"a", "aa", "aaa", "aaaa"};
        char *text = make_repeated("a", 1000);
        all_ok &= run_case("dense_a", pats, 4, text, strlen(text));
        free(text);
    }

    /* ---- Case 6: deep dict_suffix chain (idea 5 stress test) ---------
     * Each pattern is a 1-byte left-extension of the previous, so the
     * terminal of the longest pattern has a dict_suffix chain of depth
     * 5. Arriving at it emits 5 pids in one step. Exercises both
     * layouts (chain-walk and flat) under maximum chain depth -- any
     * ordering or ownership bug in the flat-output build pass shows
     * up here as a divergence vs. `sequential`. */
    {
        const char *pats[] = {"a", "ba", "cba", "dcba", "edcba"};
        char *text = make_repeated("xxedcbaxx", 200);
        all_ok &= run_case("dict_chain", pats, 5, text, strlen(text));
        free(text);
    }

    /* ---- Case 5: large text, many patterns --------------------------- */
    {
        const char *pats[] = {
            "lorem", "ipsum", "dolor", "sit", "amet", "consectetur",
            "elit", "sed", "tempor", "incididunt", "ut", "labore",
        };
        const char *unit =
            "Lorem ipsum dolor sit amet, consectetur adipiscing elit, "
            "sed do eiusmod tempor incididunt ut labore et dolore magna. ";
        char *text = make_repeated(unit, 5000);
        all_ok &= run_case("lorem_5k", pats,
                           sizeof(pats)/sizeof(pats[0]),
                           text, strlen(text));
        free(text);
    }

    /* ---- Case 7: mixed pattern length classes (idea 1 stress) --------
     * Pattern sharding policies behave differently across length
     * distributions. This case mixes 1-byte, 8-byte, 64-byte and
     * 256-byte patterns so that:
     *   - round-robin produces shards with skewed total length;
     *   - LPT (length-balanced greedy) must successfully balance
     *     pattern length across shards even when sizes span 8
     *     orders of binary magnitude;
     *   - prefix-byte bucketing must handle the case where the
     *     long-pattern bucket contains all the state-heavy patterns.
     * Each policy must still produce the same match multiset as
     * `sequential` for every thread count {1, 2, 3, 4, 7, 8}. */
    {
        /* 1-byte */
        const char *p1 = "z";
        /* 8-byte */
        const char *p2 = "needle__";
        /* 64-byte */
        const char *p3 =
            "0123456789abcdef0123456789abcdef"
            "0123456789abcdef0123456789abcdef";
        /* 256-byte */
        char *p4 = malloc(257);
        for (int i = 0; i < 256; i++) p4[i] = (char)('A' + (i % 26));
        p4[256] = 0;
        const char *pats[] = { p1, p2, p3, p4, "the", "and", "of" };
        const char *unit =
            "zzz the needle__ in the haystack 0123456789abcdef0123456789abcdef"
            "0123456789abcdef0123456789abcdef of zz and the ";
        char *text = make_repeated(unit, 100);
        all_ok &= run_case("mixed_lengths", pats,
                           sizeof(pats)/sizeof(pats[0]),
                           text, strlen(text));
        free(text);
        free(p4);
    }

    /* ---- Case 8: more patterns than threads, prefix-bucket bias ------
     * All patterns start with the same first byte ('h'), so
     * `pattern_sharded_prefix` collapses every pattern into a single
     * shard. The other shards are empty and their workers must do
     * zero work without corrupting output. Forces an asymmetric
     * pattern_sharded_prefix execution path. */
    {
        const char *pats[] = {
            "he", "hat", "have", "his", "her", "hello", "history",
            "html", "head", "hand", "hold", "hash", "host", "hex",
        };
        const char *unit =
            "his hat had hello there; hers were hashed in the head "
            "of his history of html hosts. ";
        char *text = make_repeated(unit, 200);
        all_ok &= run_case("prefix_bias_h", pats,
                           sizeof(pats)/sizeof(pats[0]),
                           text, strlen(text));
        free(text);
    }

    /* ---- Case 9: more patterns than threads, varied prefix bytes -----
     * 16 patterns with diverse first bytes; exercises every sharding
     * policy with K ∈ {1, 2, 3, 4, 7, 8} producing distinct
     * partitions. Validates that the local->global pid remap survives
     * shard counts both smaller and larger than commonly-seen K. */
    {
        const char *pats[] = {
            "alpha", "bravo", "charlie", "delta",
            "echo", "foxtrot", "golf", "hotel",
            "india", "juliet", "kilo", "lima",
            "mike", "november", "oscar", "papa",
        };
        const char *unit =
            "alpha bravo charlie delta echo foxtrot golf hotel "
            "india juliet kilo lima mike november oscar papa ";
        char *text = make_repeated(unit, 300);
        all_ok &= run_case("phonetic_16", pats,
                           sizeof(pats)/sizeof(pats[0]),
                           text, strlen(text));
        free(text);
    }

    if (all_ok) { printf("\nAll correctness tests PASSED.\n"); return 0; }
    printf("\nSome correctness tests FAILED.\n");
    return 1;
}
