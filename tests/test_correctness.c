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

    if (all_ok) { printf("\nAll correctness tests PASSED.\n"); return 0; }
    printf("\nSome correctness tests FAILED.\n");
    return 1;
}
