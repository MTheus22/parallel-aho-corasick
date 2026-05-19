/* pthread_prefetch — pthread_chunked with software prefetching.
 *
 * Motivation:
 *   The hot inner loop has two streams of memory loads:
 *     a) text[i]            — sequential single-byte streaming load.
 *     b) goto_tbl[state*256 + c]  — random-ish 4-byte load in a 256-row
 *        table of ~ num_states rows. For Snort+Enron the goto table is
 *        ~56 MiB, well past L2.
 *
 *   The hardware prefetcher handles (a) gracefully — pure forward
 *   stride. (b) is the harder target: state changes per character and
 *   the row it lands on is unpredictable. But the *next text byte* IS
 *   knowable, and the row of goto_tbl that will be addressed `Δ`
 *   characters from now can also be estimated (best case: state hasn't
 *   changed in Δ steps; worst case: the prefetch is wasted but free).
 *
 *   This searcher issues an explicit __builtin_prefetch on text two
 *   cache lines ahead. The cost is one extra instruction per byte; the
 *   benefit, when it lands, is a hidden DRAM round-trip on the streaming
 *   load. Prefetching the goto-table row is harder to justify (we'd
 *   have to guess the *next state*) and is left out.
 *
 *   Structurally this is pthread_chunked_v2 (split loops) with a
 *   prefetch hint inside the owned-region loop. */

#include "ac_searcher.h"
#include "benchmark.h"

#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Prefetch this many bytes ahead. Tuned for x86 cache-line = 64 B; two
 * lines ahead gives a single in-flight prefetch most of the time. */
#define PF_DISTANCE 128

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

static void *worker_main(void *arg)
{
    worker_t *w = (worker_t *)arg;
    const ac_automaton_t *aut = w->aut;

    const int32_t *AC_RESTRICT goto_tbl    = aut->goto_tbl;
    const int32_t *AC_RESTRICT own_head    = aut->own_out_head;
    const int32_t *AC_RESTRICT dict_suffix = aut->dict_suffix;
    const ac_output_entry_t *AC_RESTRICT outputs = aut->outputs;
    const char    *AC_RESTRICT text        = w->text;

    ac_match_list_reserve(&w->local, 4096);

    uint64_t t0 = bench_now_ns();

    int32_t state = 0;
    const size_t scan_start = w->scan_start;
    const size_t core_start = w->core_start;
    const size_t core_end   = w->core_end;

    /* Warm-up (no emission, no prefetch — short region). */
    for (size_t i = scan_start; i < core_start; i++) {
        uint8_t c = (uint8_t)text[i];
        state = goto_tbl[(size_t)state * AC_ALPHABET_SIZE + c];
    }

    /* Owned region with prefetching. We split into two segments so the
     * prefetch never reads past the end of the buffer:
     *   - First N - PF_DISTANCE iterations: prefetch text[i + PF_DISTANCE].
     *   - Last PF_DISTANCE iterations: no prefetch (still inside core_end). */
    size_t pf_end = (core_end > core_start + PF_DISTANCE)
                    ? core_end - PF_DISTANCE : core_start;

    for (size_t i = core_start; i < pf_end; i++) {
        /* T0 locality: bring into L1. The "0" rw flag means read. */
        __builtin_prefetch(text + i + PF_DISTANCE, 0, 0);

        uint8_t c = (uint8_t)text[i];
        state = goto_tbl[(size_t)state * AC_ALPHABET_SIZE + c];

        if (AC_UNLIKELY(own_head[state] != AC_NIL || dict_suffix[state] != AC_NIL)) {
            int32_t l = (own_head[state] != AC_NIL) ? state : dict_suffix[state];
            while (l != AC_NIL) {
                for (int32_t o = own_head[l]; o != AC_NIL; o = outputs[o].next) {
                    ac_match_t m = {
                        .end_pos    = (int64_t)i,
                        .pattern_id = outputs[o].pattern_id,
                    };
                    int rc = ac_match_list_push(&w->local, m);
                    if (rc != AC_OK) { w->rc = rc; goto done; }
                }
                l = dict_suffix[l];
            }
        }
    }

    /* Tail: identical to the prefetched loop minus the __builtin_prefetch. */
    for (size_t i = pf_end; i < core_end; i++) {
        uint8_t c = (uint8_t)text[i];
        state = goto_tbl[(size_t)state * AC_ALPHABET_SIZE + c];

        if (AC_UNLIKELY(own_head[state] != AC_NIL || dict_suffix[state] != AC_NIL)) {
            int32_t l = (own_head[state] != AC_NIL) ? state : dict_suffix[state];
            while (l != AC_NIL) {
                for (int32_t o = own_head[l]; o != AC_NIL; o = outputs[o].next) {
                    ac_match_t m = {
                        .end_pos    = (int64_t)i,
                        .pattern_id = outputs[o].pattern_id,
                    };
                    int rc = ac_match_list_push(&w->local, m);
                    if (rc != AC_OK) { w->rc = rc; goto done; }
                }
                l = dict_suffix[l];
            }
        }
    }
    w->rc = AC_OK;
done:
    w->seconds = (double)(bench_now_ns() - t0) / 1e9;
    return NULL;
}

static int default_thread_count(void)
{
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    if (n < 1) n = 1;
    if (n > 256) n = 256;
    return (int)n;
}

static int pf_search(const ac_automaton_t *aut,
                     const char *text, size_t text_len,
                     const ac_searcher_config_t *cfg,
                     ac_match_list_t *out,
                     ac_thread_metric_t **out_metrics,
                     size_t *out_num_metrics)
{
    int nthreads = cfg->num_threads > 0 ? cfg->num_threads : default_thread_count();
    if (nthreads < 1) nthreads = 1;

    size_t overlap = (aut->max_pattern_len > 0) ? (size_t)(aut->max_pattern_len - 1) : 0;
    if (nthreads == 1 || text_len <= overlap * 2 || text_len < (size_t)nthreads * 64) {
        const ac_searcher_t *seq = ac_searcher_find("sequential");
        if (!seq) return AC_E_NOT_FOUND;
        return seq->search(aut, text, text_len, cfg, out, out_metrics, out_num_metrics);
    }

    size_t core_size = (text_len + (size_t)nthreads - 1) / (size_t)nthreads;
    if (core_size == 0) core_size = 1;

    worker_t *workers = calloc((size_t)nthreads, sizeof(*workers));
    pthread_t *tids   = calloc((size_t)nthreads, sizeof(*tids));
    if (!workers || !tids) { free(workers); free(tids); return AC_E_NOMEM; }

    int spawned = 0;
    for (int i = 0; i < nthreads; i++) {
        size_t cs = (size_t)i * core_size;
        size_t ce = cs + core_size;
        if (ce > text_len) ce = text_len;
        if (cs >= text_len) { spawned = i; break; }
        workers[i].thread_id  = i;
        workers[i].aut        = aut;
        workers[i].text       = text;
        workers[i].core_start = cs;
        workers[i].core_end   = ce;
        workers[i].scan_start = (i == 0 || cs < overlap) ? 0 : cs - overlap;
        workers[i].rc         = AC_OK;
        ac_match_list_init(&workers[i].local);
        spawned = i + 1;
    }

    for (int i = 0; i < spawned; i++) {
        if (pthread_create(&tids[i], NULL, worker_main, &workers[i]) != 0) {
            for (int j = 0; j < i; j++) pthread_join(tids[j], NULL);
            for (int j = 0; j < spawned; j++) ac_match_list_free(&workers[j].local);
            free(workers); free(tids);
            return AC_E_THREAD;
        }
    }
    for (int i = 0; i < spawned; i++) pthread_join(tids[i], NULL);

    if (out_metrics && out_num_metrics) {
        ac_thread_metric_t *tm = calloc((size_t)spawned, sizeof(*tm));
        if (tm) {
            for (int i = 0; i < spawned; i++) {
                tm[i].thread_id     = workers[i].thread_id;
                tm[i].seconds       = workers[i].seconds;
                tm[i].bytes_scanned = workers[i].core_end - workers[i].scan_start;
                tm[i].matches_found = workers[i].local.count;
            }
            *out_metrics     = tm;
            *out_num_metrics = (size_t)spawned;
        } else {
            *out_metrics = NULL;
            *out_num_metrics = 0;
        }
    }

    int rc = AC_OK;
    for (int i = 0; i < spawned; i++) {
        if (workers[i].rc != AC_OK) { rc = workers[i].rc; break; }
        rc = ac_match_list_extend_consume(out, &workers[i].local);
        if (rc != AC_OK) break;
    }

    for (int i = 0; i < spawned; i++) ac_match_list_free(&workers[i].local);
    free(workers);
    free(tids);
    return rc;
}

static const ac_searcher_t k_pthread_prefetch = {
    .name        = "pthread_prefetch",
    .description = "Pthreads chunks + __builtin_prefetch(text + Δ)",
    .search      = pf_search,
};

__attribute__((constructor))
static void pf_register(void) { ac_searcher_register(&k_pthread_prefetch); }
