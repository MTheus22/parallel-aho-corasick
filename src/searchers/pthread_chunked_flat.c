/* pthread_chunked_flat — pthread_chunked_v2's chunking discipline
 * (warm-up / owned split-loop, cache-padded worker_t, thread-local
 * match lists) combined with the flat output table from idea 5.
 *
 * Everything that matters for correctness is identical to v2:
 *
 *   - overlap = max_pattern_len - 1 between adjacent chunks;
 *   - warm-up region [scan_start, core_start) updates the DFA state
 *     only -- no emission at all;
 *   - owned region [core_start, core_end) emits matches with no
 *     boundary branch (i.e. no `i < core_start` test in the hot loop);
 *   - matches are pushed into a thread-local ac_match_list_t and only
 *     concatenated into the global output after pthread_join.
 *
 * The only difference is the emission code: instead of walking the
 * (own_out_head, dict_suffix, outputs) chain, the owned loop reads
 * flat_count[state] and copies flat_pids[flat_offset[state] .. +cnt)
 * verbatim. This composes the chunked-pthread search-phase win with
 * the layout win, which is the headline number for idea 5.
 */

#include "ac_searcher.h"
#include "benchmark.h"

#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Cache-line size. Hard-coded so the worker_t pad is a compile-time
 * constant -- matches pthread_chunked_v2.c verbatim. */
#define FLAT_CACHE_LINE 64

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
    /* Pad to a full cache line so adjacent worker_t structs in the
     * posix_memalign'd array never share one (false-sharing guard). */
    char _pad[FLAT_CACHE_LINE - ((sizeof(int) * 2
                              + sizeof(const ac_automaton_t *)
                              + sizeof(const char *)
                              + sizeof(size_t) * 3
                              + sizeof(ac_match_list_t)
                              + sizeof(double)) % FLAT_CACHE_LINE)];
} worker_t;

static void *worker_main(void *arg)
{
    worker_t *w = (worker_t *)arg;
    const ac_automaton_t *aut = w->aut;

    const int32_t *AC_RESTRICT goto_tbl    = aut->goto_tbl;
    const int32_t *AC_RESTRICT flat_offset = aut->flat_offset;
    const int32_t *AC_RESTRICT flat_count  = aut->flat_count;
    const int32_t *AC_RESTRICT flat_pids   = aut->flat_pids;
    const char    *AC_RESTRICT text        = w->text;

    /* Same first-realloc-avoidance constant as v2. */
    ac_match_list_reserve(&w->local, 4096);

    uint64_t t0 = bench_now_ns();

    int32_t state = 0;
    const size_t scan_start = w->scan_start;
    const size_t core_start = w->core_start;
    const size_t core_end   = w->core_end;

    /* ---- Phase 1: warm-up. State-only, no emission. -------------------- */
    for (size_t i = scan_start; i < core_start; i++) {
        uint8_t c = (uint8_t)text[i];
        state = goto_tbl[(size_t)state * AC_ALPHABET_SIZE + c];
    }

    /* ---- Phase 2: owned region. Flat emission, no boundary branch. ----- */
    for (size_t i = core_start; i < core_end; i++) {
        uint8_t c = (uint8_t)text[i];
        state = goto_tbl[(size_t)state * AC_ALPHABET_SIZE + c];

        int32_t cnt = flat_count[state];
        if (AC_UNLIKELY(cnt > 0)) {
            int32_t off = flat_offset[state];
            for (int32_t k = 0; k < cnt; k++) {
                ac_match_t m = {
                    .end_pos    = (int64_t)i,
                    .pattern_id = flat_pids[off + k],
                };
                int rc = ac_match_list_push(&w->local, m);
                if (rc != AC_OK) { w->rc = rc; goto done; }
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

static int flat_search(const ac_automaton_t *aut,
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
        /* Tiny-input fallback: prefer the flat sequential variant so
         * the layout change is exercised even in the degenerate path;
         * fall back to plain `sequential` if it has not been linked. */
        const ac_searcher_t *fb = ac_searcher_find("sequential_flat");
        if (!fb) fb = ac_searcher_find("sequential");
        if (!fb) return AC_E_NOT_FOUND;
        return fb->search(aut, text, text_len, cfg, out, out_metrics, out_num_metrics);
    }

    size_t core_size = (text_len + (size_t)nthreads - 1) / (size_t)nthreads;
    if (core_size == 0) core_size = 1;

    worker_t *workers = NULL;
    if (posix_memalign((void **)&workers, FLAT_CACHE_LINE,
                       (size_t)nthreads * sizeof(*workers)) != 0) {
        return AC_E_NOMEM;
    }
    memset(workers, 0, (size_t)nthreads * sizeof(*workers));

    pthread_t *tids = calloc((size_t)nthreads, sizeof(*tids));
    if (!tids) { free(workers); return AC_E_NOMEM; }

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
        int err = pthread_create(&tids[i], NULL, worker_main, &workers[i]);
        if (err != 0) {
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
            *out_metrics     = NULL;
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

static const ac_searcher_t k_pthread_chunked_flat = {
    .name        = "pthread_chunked_flat",
    .description = "Pthreads chunks (v2 layout) + flat output table (idea 5)",
    .search      = flat_search,
};

__attribute__((constructor))
static void flat_register(void) { ac_searcher_register(&k_pthread_chunked_flat); }
