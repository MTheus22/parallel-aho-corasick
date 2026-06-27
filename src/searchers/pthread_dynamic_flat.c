/* pthread_dynamic_flat — pthread_dynamic's bag-of-tasks dispatch combined
 * with the flat output table from idea 5.
 *
 * Motivation:
 *   On homogeneous many-core CPUs (e.g. AMD Zen 5, 16 identical cores)
 *   the right load-balancer is not static topology-aware weighting (that
 *   only pays off on hybrid P/E parts) but *dynamic* dispatch: slice the
 *   text into K = 4·N tasks and let workers pull from a shared atomic
 *   queue, so faster cores naturally consume more tasks. This searcher
 *   keeps that scheduling discipline and replaces the per-match emission
 *   chain-walk with the flat output table, composing the two orthogonal
 *   wins — bag-of-tasks balancing (idea, dynamic) and flat emission
 *   (idea 5) — into a single candidate champion for homogeneous cores.
 *
 *   It is to pthread_dynamic what pthread_chunked_flat is to
 *   pthread_chunked_v2: identical control flow, only the owned-region
 *   emission differs.
 *
 * Correctness:
 *   Task semantics are identical to pthread_dynamic: each task scans
 *   [scan_start, core_end) with an L-1 warm-up and emits only matches
 *   whose end_pos falls in [core_start, core_end). The union of emitted
 *   matches equals the sequential baseline; thread assignment of any
 *   individual match is non-deterministic but the set is invariant.
 *
 * The only difference vs pthread_dynamic is the emission code: instead of
 * walking (own_out_head, dict_suffix, outputs), the owned loop reads
 * flat_count[state] and copies flat_pids[flat_offset[state] .. +cnt)
 * verbatim. */

#include "ac_searcher.h"
#include "benchmark.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Tunables. tasks-per-thread = number of dynamic tasks per thread; larger
 * gives finer balancing but more atomic ops and more warm-up work. 4 matches
 * pthread_dynamic (a common work-stealing literature value) and is the default.
 *
 * Configurable at runtime (no recompile) so a granularity sweep can be
 * automated. Resolution order: cfg->tasks_per_thread (CLI --tasks-per-thread)
 * if > 0, else env AC_DYN_TASKS_PER_THREAD if set/positive, else the default. */
#define K_PER_THREAD_DEFAULT 4

static int resolve_tasks_per_thread(const ac_searcher_config_t *cfg)
{
    if (cfg && cfg->tasks_per_thread > 0) return cfg->tasks_per_thread;
    const char *env = getenv("AC_DYN_TASKS_PER_THREAD");
    if (env && env[0]) {
        int v = atoi(env);
        if (v > 0) return v;
    }
    return K_PER_THREAD_DEFAULT;
}

typedef struct {
    size_t core_start;   /* inclusive, first byte the task owns */
    size_t core_end;     /* exclusive, end of owned region */
    size_t scan_start;   /* inclusive, where warm-up begins */
} task_t;

typedef struct {
    int                    thread_id;
    const ac_automaton_t  *aut;
    const char            *text;
    const task_t          *tasks;
    size_t                 num_tasks;
    atomic_size_t         *next_task;     /* shared task counter */
    ac_match_list_t        local;
    size_t                 tasks_done;
    double                 seconds;
    int                    rc;
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

    ac_match_list_reserve(&w->local, 4096);

    uint64_t t0 = bench_now_ns();

    for (;;) {
        /* memory_order_relaxed is sufficient: the counter is the only
         * shared mutable state, and reads of `tasks[]` are race-free
         * because tasks[] is filled before pthread_create. */
        size_t idx = atomic_fetch_add_explicit(w->next_task, 1, memory_order_relaxed);
        if (idx >= w->num_tasks) break;

        const task_t *t = &w->tasks[idx];
        int32_t state = 0;

        /* Warm-up: state-only scan over [scan_start, core_start). */
        for (size_t i = t->scan_start; i < t->core_start; i++) {
            uint8_t c = (uint8_t)text[i];
            state = goto_tbl[(size_t)state * AC_ALPHABET_SIZE + c];
        }

        /* Owned region: flat emission, no boundary branch. */
        for (size_t i = t->core_start; i < t->core_end; i++) {
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
        w->tasks_done++;
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

static int dyn_flat_search(const ac_automaton_t *aut,
                           const char *text, size_t text_len,
                           const ac_searcher_config_t *cfg,
                           ac_match_list_t *out,
                           ac_thread_metric_t **out_metrics,
                           size_t *out_num_metrics)
{
    int nthreads = cfg->num_threads > 0 ? cfg->num_threads : default_thread_count();
    if (nthreads < 1) nthreads = 1;

    size_t overlap = (aut->max_pattern_len > 0) ? (size_t)(aut->max_pattern_len - 1) : 0;

    /* Degenerate: short text or single thread → sequential_flat so the
     * flat layout is still exercised in the trivial path. */
    if (nthreads == 1 || text_len <= overlap * 2 || text_len < (size_t)nthreads * 64) {
        const ac_searcher_t *fb = ac_searcher_find("sequential_flat");
        if (!fb) fb = ac_searcher_find("sequential");
        if (!fb) return AC_E_NOT_FOUND;
        return fb->search(aut, text, text_len, cfg, out, out_metrics, out_num_metrics);
    }

    /* Build the task list. K = nthreads * tasks_per_thread chunks, sized
     * roughly equally. Last chunk eats the remainder. */
    int k_per_thread = resolve_tasks_per_thread(cfg);
    size_t num_tasks = (size_t)nthreads * (size_t)k_per_thread;
    if (num_tasks > text_len / 64) {
        num_tasks = text_len / 64;
        if (num_tasks < (size_t)nthreads) num_tasks = (size_t)nthreads;
    }
    size_t task_size = (text_len + num_tasks - 1) / num_tasks;
    if (task_size == 0) task_size = 1;

    task_t *tasks = calloc(num_tasks, sizeof(*tasks));
    if (!tasks) return AC_E_NOMEM;
    size_t real_tasks = 0;
    for (size_t i = 0; i < num_tasks; i++) {
        size_t cs = i * task_size;
        size_t ce = cs + task_size;
        if (ce > text_len) ce = text_len;
        if (cs >= text_len) break;
        tasks[real_tasks].core_start = cs;
        tasks[real_tasks].core_end   = ce;
        tasks[real_tasks].scan_start = (i == 0 || cs < overlap) ? 0 : cs - overlap;
        real_tasks++;
    }

    atomic_size_t next_task;
    atomic_init(&next_task, 0);

    worker_t *workers = calloc((size_t)nthreads, sizeof(*workers));
    pthread_t *tids   = calloc((size_t)nthreads, sizeof(*tids));
    if (!workers || !tids) {
        free(workers); free(tids); free(tasks);
        return AC_E_NOMEM;
    }

    for (int i = 0; i < nthreads; i++) {
        workers[i].thread_id  = i;
        workers[i].aut        = aut;
        workers[i].text       = text;
        workers[i].tasks      = tasks;
        workers[i].num_tasks  = real_tasks;
        workers[i].next_task  = &next_task;
        workers[i].rc         = AC_OK;
        ac_match_list_init(&workers[i].local);
    }

    for (int i = 0; i < nthreads; i++) {
        if (pthread_create(&tids[i], NULL, worker_main, &workers[i]) != 0) {
            for (int j = 0; j < i; j++) pthread_join(tids[j], NULL);
            for (int j = 0; j < nthreads; j++) ac_match_list_free(&workers[j].local);
            free(workers); free(tids); free(tasks);
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
                /* For dynamic dispatch the "bytes scanned" reading is
                 * approximate — sum across all tasks this worker grabbed,
                 * ignoring overlap (small contribution). */
                tm[i].bytes_scanned = workers[i].tasks_done * task_size;
                tm[i].matches_found = workers[i].local.count;
            }
            *out_metrics     = tm;
            *out_num_metrics = (size_t)nthreads;
        } else {
            *out_metrics = NULL;
            *out_num_metrics = 0;
        }
    }

    int rc = AC_OK;
    for (int i = 0; i < nthreads; i++) {
        if (workers[i].rc != AC_OK) { rc = workers[i].rc; break; }
        rc = ac_match_list_extend_consume(out, &workers[i].local);
        if (rc != AC_OK) break;
    }

    for (int i = 0; i < nthreads; i++) ac_match_list_free(&workers[i].local);
    free(workers);
    free(tids);
    free(tasks);
    return rc;
}

static const ac_searcher_t k_pthread_dynamic_flat = {
    .name        = "pthread_dynamic_flat",
    .description = "Pthreads dynamic dispatch (bag of tasks) + flat output table (idea 5)",
    .search      = dyn_flat_search,
};

__attribute__((constructor))
static void dyn_flat_register(void) { ac_searcher_register(&k_pthread_dynamic_flat); }
