/* pthread_chunked_v3_flat — Idea 7. Composition of `pthread_chunked_v3`
 * (topology-aware affinity + frequency-weighted chunks) with the flat
 * output table from idea 5 (already proven in pthread_chunked_flat).
 *
 * Hypothesis (from `docs/proposals/parallelism-roadmap.md` / synthesis
 * doc, future-work card F1): the two optimisations target orthogonal
 * bottlenecks and should compose multiplicatively.
 *
 *   - v3 redistributes the *bytes of text* across hybrid P/E cores so
 *     no worker finishes ahead of the slowest one. The win shows up
 *     mostly at low T on Alder Lake (t=2 / t=3) and as a 3–5 % bonus
 *     at the topology-saturating thread count.
 *
 *   - flat outputs eliminate the chain-walk through
 *     (own_out_head, dict_suffix, outputs) and replace it with one
 *     contiguous scan of `flat_pids[off, off+cnt)`. The win shows up
 *     wherever the dictionary is large enough that the auxiliary
 *     arrays escape L2.
 *
 * Concretely this file is v3 verbatim (topology + weighted chunks +
 * try_pin per worker) with the owned-loop emission swapped for the
 * flat-table emission used by `pthread_chunked_flat`. The hot loop is
 * branch-light: `flat_count[state]` is 0 for the vast majority of
 * states and the AC_UNLIKELY guard keeps the slow path off the
 * pipeline. The warm-up region is bytewise identical to v3 (state-only,
 * no emission, overlap = max_pattern_len - 1).
 *
 * Tiny-input fallback prefers `sequential_flat` over `sequential` so
 * the layout change is still exercised when chunking degenerates.
 *
 * Correctness inheritance: every invariant satisfied by v3 and by
 * pthread_chunked_flat is satisfied here, since this is a code-level
 * composition. `make test` validates against the sequential baseline
 * for `{1,2,3,4,7,8}` threads; `make tsan` validates absence of races.
 */

#include "ac_searcher.h"
#include "benchmark.h"

#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define V3F_CACHE_LINE  64
#define V3F_MAX_CPUS    256

/* SMT-pair compensation. Kept identical to pthread_chunked_v3 so the
 * comparison stays apples-to-apples: any throughput delta between
 * v3 and v3_flat must come from the emission path, not from chunk
 * sizing. */
#define V3F_HT_FACTOR_NUM 100
#define V3F_HT_FACTOR_DEN 100

typedef struct {
    int       cpu_id;
    uint32_t  max_freq_khz;
    int       sibling_leader;
} v3f_cpu_info_t;

typedef struct {
    int                       thread_id;
    int                       cpu_id;
    uint32_t                  weight;
    const ac_automaton_t     *aut;
    const char               *text;
    size_t                    scan_start;
    size_t                    core_start;
    size_t                    core_end;
    ac_match_list_t           local;
    double                    seconds;
    int                       rc;
    int                       cpu;
} __attribute__((aligned(V3F_CACHE_LINE))) worker_t;

static uint32_t v3f_read_max_freq_khz(int cpu_id)
{
    char path[160];
    snprintf(path, sizeof(path),
             "/sys/devices/system/cpu/cpu%d/cpufreq/cpuinfo_max_freq", cpu_id);
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    uint32_t khz = 0;
    if (fscanf(f, "%u", &khz) != 1) khz = 0;
    fclose(f);
    return khz;
}

static int v3f_read_sibling_leader(int cpu_id)
{
    char path[160];
    snprintf(path, sizeof(path),
             "/sys/devices/system/cpu/cpu%d/topology/thread_siblings_list", cpu_id);
    FILE *f = fopen(path, "r");
    if (!f) return cpu_id;
    int leader = -1;
    if (fscanf(f, "%d", &leader) != 1) leader = cpu_id;
    fclose(f);
    return leader >= 0 ? leader : cpu_id;
}

static int v3f_cpu_cmp(const void *a, const void *b)
{
    const v3f_cpu_info_t *x = (const v3f_cpu_info_t *)a;
    const v3f_cpu_info_t *y = (const v3f_cpu_info_t *)b;
    int x_leader = (x->cpu_id == x->sibling_leader) ? 1 : 0;
    int y_leader = (y->cpu_id == y->sibling_leader) ? 1 : 0;
    if (x_leader != y_leader) return y_leader - x_leader;
    if (x->max_freq_khz != y->max_freq_khz)
        return (int)((int64_t)y->max_freq_khz - (int64_t)x->max_freq_khz);
    return x->cpu_id - y->cpu_id;
}

static int v3f_build_cpu_info(v3f_cpu_info_t *info, int max_n)
{
    long online = sysconf(_SC_NPROCESSORS_ONLN);
    if (online < 1) online = 1;
    if (online > max_n) online = max_n;
    for (int i = 0; i < (int)online; i++) {
        info[i].cpu_id         = i;
        info[i].max_freq_khz   = v3f_read_max_freq_khz(i);
        info[i].sibling_leader = v3f_read_sibling_leader(i);
    }
    qsort(info, (size_t)online, sizeof(info[0]), v3f_cpu_cmp);
    return (int)online;
}

static void v3f_try_pin(int cpu_id)
{
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu_id, &set);
    (void)pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
}

static void *worker_main(void *arg)
{
    worker_t *w = (worker_t *)arg;
    v3f_try_pin(w->cpu_id);

    const ac_automaton_t *aut = w->aut;
    const int32_t *AC_RESTRICT goto_tbl    = aut->goto_tbl;
    const int32_t *AC_RESTRICT flat_offset = aut->flat_offset;
    const int32_t *AC_RESTRICT flat_count  = aut->flat_count;
    const int32_t *AC_RESTRICT flat_pids   = aut->flat_pids;
    const char    *AC_RESTRICT text        = w->text;

    ac_match_list_reserve(&w->local, 4096);

    uint64_t t0 = bench_now_ns();

    int32_t state = 0;
    const size_t scan_start = w->scan_start;
    const size_t core_start = w->core_start;
    const size_t core_end   = w->core_end;

    /* Phase 1: warm-up (state-only, identical to v3). */
    for (size_t i = scan_start; i < core_start; i++) {
        uint8_t c = (uint8_t)text[i];
        state = goto_tbl[(size_t)state * AC_ALPHABET_SIZE + c];
    }
    /* Phase 2: owned region with flat emission. */
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
    w->cpu = ac_current_cpu();
    return NULL;
}

static int v3f_default_thread_count(void)
{
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    if (n < 1) n = 1;
    if (n > V3F_MAX_CPUS) n = V3F_MAX_CPUS;
    return (int)n;
}

static int v3f_search(const ac_automaton_t *aut,
                      const char *text, size_t text_len,
                      const ac_searcher_config_t *cfg,
                      ac_match_list_t *out,
                      ac_thread_metric_t **out_metrics,
                      size_t *out_num_metrics)
{
    int nthreads = cfg->num_threads > 0 ? cfg->num_threads : v3f_default_thread_count();
    if (nthreads < 1) nthreads = 1;

    size_t overlap = (aut->max_pattern_len > 0) ? (size_t)(aut->max_pattern_len - 1) : 0;
    if (nthreads == 1 || text_len <= overlap * 2 || text_len < (size_t)nthreads * 64) {
        /* Prefer sequential_flat so the flat layout is still exercised
         * in the degenerate path. Fall back to sequential if it's not
         * linked into the binary. */
        const ac_searcher_t *fb = ac_searcher_find("sequential_flat");
        if (!fb) fb = ac_searcher_find("sequential");
        if (!fb) return AC_E_NOT_FOUND;
        return fb->search(aut, text, text_len, cfg, out, out_metrics, out_num_metrics);
    }

    v3f_cpu_info_t info[V3F_MAX_CPUS];
    int n_cpus = v3f_build_cpu_info(info, V3F_MAX_CPUS);
    if (n_cpus < 1) n_cpus = 1;

    int have_freq_info = 0;
    for (int i = 0; i < n_cpus; i++) {
        if (info[i].max_freq_khz > 0) { have_freq_info = 1; break; }
    }

    int group_count[V3F_MAX_CPUS];
    memset(group_count, 0, sizeof(group_count));
    for (int i = 0; i < nthreads; i++) {
        int leader = info[i % n_cpus].sibling_leader;
        if (leader >= 0 && leader < V3F_MAX_CPUS) group_count[leader]++;
    }

    uint64_t total_weight = 0;
    uint32_t weights[V3F_MAX_CPUS];
    for (int i = 0; i < nthreads; i++) {
        v3f_cpu_info_t ci = info[i % n_cpus];
        uint32_t w;
        if (have_freq_info) {
            w = (ci.max_freq_khz > 0) ? ci.max_freq_khz : 1000000u;
        } else {
            w = 1u;
        }
        if (have_freq_info
            && ci.sibling_leader >= 0 && ci.sibling_leader < V3F_MAX_CPUS
            && group_count[ci.sibling_leader] > 1) {
            w = (uint32_t)((uint64_t)w * V3F_HT_FACTOR_NUM / V3F_HT_FACTOR_DEN);
            if (w == 0) w = 1;
        }
        weights[i]    = w;
        total_weight += w;
    }
    if (total_weight == 0) total_weight = (uint64_t)nthreads;

    worker_t *workers = NULL;
    if (posix_memalign((void **)&workers, V3F_CACHE_LINE,
                       (size_t)nthreads * sizeof(*workers)) != 0) {
        return AC_E_NOMEM;
    }
    memset(workers, 0, (size_t)nthreads * sizeof(*workers));

    pthread_t *tids = calloc((size_t)nthreads, sizeof(*tids));
    if (!tids) { free(workers); return AC_E_NOMEM; }

    size_t cumulative = 0;
    int spawned = 0;
    for (int i = 0; i < nthreads; i++) {
        size_t cs = cumulative;
        if (cs >= text_len) { spawned = i; break; }

        size_t bytes;
        if (i == nthreads - 1) {
            bytes = text_len - cs;
        } else {
            uint64_t share = (uint64_t)text_len * weights[i] / total_weight;
            share = (share + 63u) & ~(uint64_t)63u;
            if (share < 64u) share = 64u;
            if (cs + share > text_len) share = text_len - cs;
            bytes = (size_t)share;
        }
        size_t ce = cs + bytes;
        cumulative = ce;

        workers[i].thread_id  = i;
        workers[i].cpu_id     = info[i % n_cpus].cpu_id;
        workers[i].weight     = weights[i];
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
                tm[i].cpu           = workers[i].cpu;
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

static const ac_searcher_t k_pthread_chunked_v3_flat = {
    .name        = "pthread_chunked_v3_flat",
    .description = "Pthreads chunks; topology-aware affinity + freq-weighted chunks + flat output table",
    .search      = v3f_search,
};

__attribute__((constructor))
static void v3f_register(void) { ac_searcher_register(&k_pthread_chunked_v3_flat); }
