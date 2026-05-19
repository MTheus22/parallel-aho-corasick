/* pthread_block_cyclic — static round-robin block dispatch.
 *
 * Motivation:
 *   Static contiguous chunking gives every worker one huge region. If a
 *   worker stalls (page fault, scheduler preemption, slow core), its
 *   region is gated and the join waits. Fully dynamic dispatch
 *   (pthread_dynamic) fixes that but adds an atomic per task and gives
 *   up cache locality on the prefetch stream.
 *
 *   Block-cyclic sits in the middle: split the text into B-byte blocks
 *   (B small enough to interleave but large enough to amortise per-block
 *   warm-up). Worker i statically owns blocks { i, i+N, i+2N, ... }.
 *
 *   - No atomic on the hot path. Distribution is decided at start.
 *   - If one worker is 2x slower than the average, the other workers
 *     simply burn through their own stride and finish; the only loss is
 *     N-1 idle workers waiting for that one stride to complete.
 *   - Adjacent blocks of any one worker are NOT contiguous; the OS
 *     prefetcher loses the long stream pattern. We compensate by
 *     keeping B fairly large (default 1 MiB) so each block is a long
 *     enough stream of its own.
 *
 * Each block carries its own L-1 warm-up region, identical to the
 * per-chunk ownership rule in pthread_chunked. */

#include "ac_searcher.h"
#include "benchmark.h"

#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define BC_BLOCK_BYTES (1u << 20)   /* 1 MiB per cyclic block */

typedef struct {
    int                    thread_id;
    int                    nthreads;
    const ac_automaton_t  *aut;
    const char            *text;
    size_t                 text_len;
    size_t                 block_bytes;
    size_t                 overlap;
    ac_match_list_t        local;
    double                 seconds;
    int                    rc;
    size_t                 blocks_done;
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
    const size_t block_bytes = w->block_bytes;
    const size_t overlap     = w->overlap;
    const size_t text_len    = w->text_len;
    const size_t nthreads    = (size_t)w->nthreads;
    const size_t tid         = (size_t)w->thread_id;

    ac_match_list_reserve(&w->local, 4096);

    uint64_t t0 = bench_now_ns();

    /* Block index k → byte range [k*B, (k+1)*B), iterate k = tid, tid+N, ... */
    size_t total_blocks = (text_len + block_bytes - 1) / block_bytes;
    for (size_t k = tid; k < total_blocks; k += nthreads) {
        size_t core_start = k * block_bytes;
        size_t core_end   = core_start + block_bytes;
        if (core_end > text_len) core_end = text_len;
        size_t scan_start = (k == 0 || core_start < overlap) ? 0 : core_start - overlap;

        int32_t state = 0;

        /* Warm-up: state-only. */
        for (size_t i = scan_start; i < core_start; i++) {
            uint8_t c = (uint8_t)text[i];
            state = goto_tbl[(size_t)state * AC_ALPHABET_SIZE + c];
        }

        /* Owned region: emit. */
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
        w->blocks_done++;
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

static int bc_search(const ac_automaton_t *aut,
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

    /* Block size must be >> overlap so the warm-up is amortised. We
     * accept the caller's chunk_size hint when it is reasonable;
     * otherwise pick 1 MiB by default. */
    size_t block_bytes = cfg->chunk_size > 0 ? cfg->chunk_size : BC_BLOCK_BYTES;
    if (block_bytes < overlap * 4) block_bytes = overlap * 4;
    if (block_bytes < 65536) block_bytes = 65536;

    worker_t *workers = calloc((size_t)nthreads, sizeof(*workers));
    pthread_t *tids   = calloc((size_t)nthreads, sizeof(*tids));
    if (!workers || !tids) { free(workers); free(tids); return AC_E_NOMEM; }

    for (int i = 0; i < nthreads; i++) {
        workers[i].thread_id   = i;
        workers[i].nthreads    = nthreads;
        workers[i].aut         = aut;
        workers[i].text        = text;
        workers[i].text_len    = text_len;
        workers[i].block_bytes = block_bytes;
        workers[i].overlap     = overlap;
        workers[i].rc          = AC_OK;
        ac_match_list_init(&workers[i].local);
    }

    for (int i = 0; i < nthreads; i++) {
        if (pthread_create(&tids[i], NULL, worker_main, &workers[i]) != 0) {
            for (int j = 0; j < i; j++) pthread_join(tids[j], NULL);
            for (int j = 0; j < nthreads; j++) ac_match_list_free(&workers[j].local);
            free(workers); free(tids);
            return AC_E_THREAD;
        }
    }
    for (int i = 0; i < nthreads; i++) pthread_join(tids[i], NULL);

    if (out_metrics && out_num_metrics) {
        ac_thread_metric_t *tm = calloc((size_t)nthreads, sizeof(*tm));
        if (tm) {
            for (int i = 0; i < nthreads; i++) {
                tm[i].thread_id     = workers[i].thread_id;
                tm[i].seconds       = workers[i].seconds;
                tm[i].bytes_scanned = workers[i].blocks_done * block_bytes;
                tm[i].matches_found = workers[i].local.count;
            }
            *out_metrics     = tm;
            *out_num_metrics = (size_t)nthreads;
        } else {
            *out_metrics = NULL;
            *out_num_metrics = 0;
        }
    }

    /* Important: matches were produced in cyclic block order. We merge
     * by thread_id (i.e. block-stride groups) and let
     * ac_match_list_sort() on the final list handle canonicalisation. */
    int rc = AC_OK;
    for (int i = 0; i < nthreads; i++) {
        if (workers[i].rc != AC_OK) { rc = workers[i].rc; break; }
        rc = ac_match_list_extend_consume(out, &workers[i].local);
        if (rc != AC_OK) break;
    }

    for (int i = 0; i < nthreads; i++) ac_match_list_free(&workers[i].local);
    free(workers);
    free(tids);
    return rc;
}

static const ac_searcher_t k_pthread_block_cyclic = {
    .name        = "pthread_block_cyclic",
    .description = "Pthreads static round-robin blocks (1 MiB), thread-local lists",
    .search      = bc_search,
};

__attribute__((constructor))
static void bc_register(void) { ac_searcher_register(&k_pthread_block_cyclic); }
