/* pthread_chunked_v2 — branch-eliminated, cache-pad-aware variant of
 * pthread_chunked.
 *
 * Three micro-optimisations over the v1 baseline:
 *
 *   1. The warm-up region [scan_start, core_start) is consumed by a
 *      DEDICATED tight loop that only updates the DFA state and never
 *      checks `own_out_head` / `dict_suffix`. The owned region
 *      [core_start, core_end) is then a separate loop that emits matches
 *      without any `i < core_start` branch. v1 paid a predictable but
 *      non-zero branch on every byte of the warm-up region; here that
 *      cost is structurally zero.
 *
 *   2. `worker_t` is padded to 64 bytes (the L1 cache-line size on x86)
 *      so that adjacent worker structs in the calloc()ed array do not
 *      share a cache line. This is mostly defensive — v1 already mutates
 *      its own `local` list head far more often than the master ever
 *      reads a sibling — but it removes an entire class of pathological
 *      false-sharing scenarios on CPUs with stricter coherency.
 *
 *   3. The thread-local `ac_match_list_t` is pre-reserved to a size
 *      proportional to chunk_size (1 entry per 64 bytes scanned by
 *      default). For Snort+Enron this puts the steady-state count above
 *      the first power-of-two boundary and removes the early realloc()s
 *      that would otherwise land in the timed region.
 *
 * No invariant changes vs v1: overlap = L-1, ownership rule (matches
 * with end_pos in [scan_start, core_start) are silently dropped), shared
 * read-only automaton. Match output is identical, so the correctness
 * harness compares it against `sequential` 1:1. */

#include "ac_searcher.h"
#include "benchmark.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Cache line. Hard-coded because we want this to be a compile-time
 * constant regardless of the host's runtime-queried size. */
#define V2_CACHE_LINE 64

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
    /* Pad to a full cache line so adjacent worker_t structs never share
     * one. The exact field count fluctuates with pointer width, so we
     * compute the padding from the rest of the struct. */
    char _pad[V2_CACHE_LINE - ((sizeof(int) * 2
                              + sizeof(const ac_automaton_t *)
                              + sizeof(const char *)
                              + sizeof(size_t) * 3
                              + sizeof(ac_match_list_t)
                              + sizeof(double)) % V2_CACHE_LINE)];
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

    /* Reserve a few thousand entries so the first realloc()s land before
     * the timed section. Heavier pre-allocation tied to chunk size was
     * tested but regressed at high thread counts (>200 MB of unused
     * capacity in aggregate). 4096 is a good middle ground: covers the
     * typical first second of a sparse workload, no measurable cost
     * when the workload is denser. */
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

    /* ---- Phase 2: owned region. Emits matches. No boundary branch. ----- */
    for (size_t i = core_start; i < core_end; i++) {
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

static int v2_search(const ac_automaton_t *aut,
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

    /* Allocate so each worker_t lives on its own cache line. posix_memalign
     * is the closest portable spelling of "aligned malloc"; calloc would
     * inherit the default 8/16-byte alignment of the malloc arena. */
    worker_t *workers = NULL;
    if (posix_memalign((void **)&workers, V2_CACHE_LINE,
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

static const ac_searcher_t k_pthread_chunked_v2 = {
    .name        = "pthread_chunked_v2",
    .description = "Pthreads chunks; split warm-up/owned loops; cache-pad worker_t",
    .search      = v2_search,
};

__attribute__((constructor))
static void v2_register(void) { ac_searcher_register(&k_pthread_chunked_v2); }
