/* Data-parallel pthreads searcher.
 *
 * Strategy:
 *   - Split text into N "core" ranges of equal size, one per worker.
 *   - Each worker actually scans [core_start - overlap, core_end), where
 *     overlap = max_pattern_len - 1. The leading `overlap` bytes are a
 *     warm-up: they bring the AC-DFA state at position core_start into
 *     agreement with what a global scan from position 0 would yield, so
 *     no pattern that straddles a chunk boundary is lost. Matches whose
 *     end positions fall within the warm-up region are owned by the
 *     previous worker and are skipped here.
 *   - Each worker stores its hits in a thread-local ac_match_list_t. The
 *     master merges them after pthread_join, so no locks are taken on
 *     the hot path. The automaton is read-only and shared by pointer.
 *
 * Why overlap = L - 1 is sufficient:
 *   The AC-DFA state after consuming a string sigma is determined by the
 *   longest pattern-prefix that is a suffix of sigma. That suffix has
 *   length <= L. Starting from root and consuming overlap = L-1 chars
 *   before core_start puts the worker into a state that, after the next
 *   character at core_start, matches the global state at core_start.
 *   From that point onward the state evolves deterministically, so every
 *   match ending at position p >= core_start is reported correctly. */

#include "ac_searcher.h"
#include "benchmark.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct {
    int                    thread_id;
    const ac_automaton_t  *aut;
    const char            *text;
    size_t                 scan_start;   /* inclusive: where AC scan begins */
    size_t                 core_start;   /* inclusive: first owned position */
    size_t                 core_end;     /* exclusive: end of owned region  */
    ac_match_list_t        local;        /* THREAD-LOCAL match list         */
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

    /* Pre-size the local list a bit to amortise initial growth. Tuned to
     * keep the first few realloc()s out of the timed region. */
    ac_match_list_reserve(&w->local, 1024);

    uint64_t t0 = bench_now_ns();

    int32_t state = 0;
    const size_t scan_start = w->scan_start;
    const size_t core_start = w->core_start;
    const size_t core_end   = w->core_end;

    for (size_t i = scan_start; i < core_end; i++) {
        uint8_t c = (uint8_t)text[i];
        state = goto_tbl[(size_t)state * AC_ALPHABET_SIZE + c];

        /* Suppress reports during warm-up; the previous worker owns them. */
        if (AC_UNLIKELY(i < core_start)) continue;

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

static int pt_search(const ac_automaton_t *aut,
                     const char *text, size_t text_len,
                     const ac_searcher_config_t *cfg,
                     ac_match_list_t *out,
                     ac_thread_metric_t **out_metrics,
                     size_t *out_num_metrics)
{
    int nthreads = cfg->num_threads > 0 ? cfg->num_threads : default_thread_count();
    if (nthreads < 1) nthreads = 1;

    /* Degenerate cases: short text or very few cores -> sequential. */
    size_t overlap = (aut->max_pattern_len > 0) ? (size_t)(aut->max_pattern_len - 1) : 0;
    if (nthreads == 1 || text_len <= overlap * 2 || text_len < (size_t)nthreads * 64) {
        const ac_searcher_t *seq = ac_searcher_find("sequential");
        if (!seq) return AC_E_NOT_FOUND;
        return seq->search(aut, text, text_len, cfg, out, out_metrics, out_num_metrics);
    }

    /* Compute core ranges. We use ceiling division so the last worker
     * never gets a larger-than-fair slice. */
    size_t core_size = (text_len + (size_t)nthreads - 1) / (size_t)nthreads;
    if (core_size == 0) core_size = 1;

    worker_t *workers = calloc((size_t)nthreads, sizeof(*workers));
    pthread_t *tids   = calloc((size_t)nthreads, sizeof(*tids));
    if (!workers || !tids) {
        free(workers); free(tids);
        return AC_E_NOMEM;
    }

    int spawned = 0;
    for (int i = 0; i < nthreads; i++) {
        size_t cs = (size_t)i * core_size;
        size_t ce = cs + core_size;
        if (ce > text_len) ce = text_len;
        if (cs >= text_len) { /* nothing left -- shrink the pool */
            spawned = i;
            break;
        }
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

    /* Spawn (after all worker_t fields are initialised, so the children
     * never observe a torn state). */
    for (int i = 0; i < spawned; i++) {
        int err = pthread_create(&tids[i], NULL, worker_main, &workers[i]);
        if (err != 0) {
            /* Best-effort recovery: run the rest in this thread. */
            for (int j = 0; j < i; j++) pthread_join(tids[j], NULL);
            for (int j = 0; j < spawned; j++) ac_match_list_free(&workers[j].local);
            free(workers); free(tids);
            return AC_E_THREAD;
        }
    }
    for (int i = 0; i < spawned; i++) pthread_join(tids[i], NULL);

    /* Capture metrics BEFORE consuming local lists (consume zeroes them). */
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
            *out_metrics     = NULL;
            *out_num_metrics = 0;
        }
    }

    /* Merge thread-local lists in deterministic order (thread_id ascending,
     * which corresponds to ascending end_pos ranges). */
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

static const ac_searcher_t k_pthread_chunked = {
    .name        = "pthread_chunked",
    .description = "Pthreads, fixed-size chunks with overlap, thread-local lists",
    .search      = pt_search,
};

__attribute__((constructor))
static void pt_register(void) { ac_searcher_register(&k_pthread_chunked); }
