/* pthread_chunked_v3 — Topology-aware affinity + frequency-weighted chunks.
 *
 * Builds on pthread_chunked_v2 (split warm-up/owned loops, cache-padded
 * worker_t) and addresses two limitations of pthread_affinity:
 *
 *   1. Naive affinity pins worker i to logical CPU i. On hybrid CPUs
 *      such as Alder Lake (i5-1235U) the logical-CPU layout is
 *        CPUs 0,1 = P-core 0 SMT siblings
 *        CPUs 2,3 = P-core 1 SMT siblings
 *        CPUs 4..N = E-cores (one thread each)
 *      So with only 2 threads, pthread_affinity puts both on CPUs 0,1
 *      — the same physical P-core — and they contend for L1/L2 and
 *      execution ports. We instead build a topology-aware order that
 *      fills physical-core leaders first (sorted by max-frequency
 *      descending), and only spills into SMT siblings when nthreads
 *      exceeds the number of physical cores.
 *
 *   2. Equal-sized chunks assume every worker scans bytes at the same
 *      rate. On heterogeneous silicon that's wrong: at 4.4 GHz a P-core
 *      processes ~1.3–1.9× more bytes/second than an E-core at 3.3 GHz.
 *      We weight each worker's chunk by its CPU's `cpuinfo_max_freq`
 *      and apply a 0.55× multiplier when an SMT sibling pair is active
 *      (both threads on the same physical core share execution ports).
 *
 * Both adjustments are best-effort: missing or unreadable sysfs entries
 * degrade gracefully to pthread_chunked_v2 behaviour (equal-sized
 * chunks, identity-order affinity). The overlap rule, ownership rule,
 * shared read-only automaton and thread-local match lists are inherited
 * unchanged from v2 — `make test` validates that v3 produces an
 * identical multiset of matches versus the `sequential` baseline.
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

#define V3_CACHE_LINE  64
#define V3_MAX_CPUS    256

/* SMT-pair compensation. Measured per-thread throughput on the i5-1235U
 * shows P-core HT-paired-per-thread rate ≈ 1.33–1.40× the solo E-core
 * rate — almost exactly the freq ratio 4400/3300 = 1.33. So a raw
 * freq-weighted distribution is already optimal at t=12 on this CPU
 * and the HT factor degenerates to 1.0 (no derate).
 *
 * Kept as a tunable: on CPUs where the per-thread-paired rate is lower
 * than freq alone predicts (older Intel SMT, AMD without execution-port
 * superscalar parity), values in the 0.65–0.85 range become useful. */
#define V3_HT_FACTOR_NUM 100
#define V3_HT_FACTOR_DEN 100

/* Per-CPU topology info gathered from sysfs. Layout chosen for qsort. */
typedef struct {
    int       cpu_id;          /* logical CPU id (matches /sys/.../cpu<id>) */
    uint32_t  max_freq_khz;    /* 0 if cpufreq unreadable */
    int       sibling_leader;  /* lowest CPU id in this SMT group; == cpu_id when solo */
} v3_cpu_info_t;

/* Worker state. Padded to a full cache line so adjacent workers in the
 * aligned_alloc()ed array can never share a line. */
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
} __attribute__((aligned(V3_CACHE_LINE))) worker_t;

static uint32_t v3_read_max_freq_khz(int cpu_id)
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

static int v3_read_sibling_leader(int cpu_id)
{
    char path[160];
    snprintf(path, sizeof(path),
             "/sys/devices/system/cpu/cpu%d/topology/thread_siblings_list", cpu_id);
    FILE *f = fopen(path, "r");
    if (!f) return cpu_id;
    int leader = -1;
    /* The siblings list is "lo-hi" or "lo,hi" or "lo"; the first integer
     * is always the lowest sibling id, which we treat as the canonical
     * leader of the SMT group. */
    if (fscanf(f, "%d", &leader) != 1) leader = cpu_id;
    fclose(f);
    return leader >= 0 ? leader : cpu_id;
}

/* qsort comparator. Sort key, in priority order:
 *   1. SMT leaders before SMT siblings (so we fill physical cores first)
 *   2. Higher max-frequency first (P-cores before E-cores)
 *   3. Lower cpu_id first (deterministic tiebreak)
 */
static int v3_cpu_cmp(const void *a, const void *b)
{
    const v3_cpu_info_t *x = (const v3_cpu_info_t *)a;
    const v3_cpu_info_t *y = (const v3_cpu_info_t *)b;
    int x_leader = (x->cpu_id == x->sibling_leader) ? 1 : 0;
    int y_leader = (y->cpu_id == y->sibling_leader) ? 1 : 0;
    if (x_leader != y_leader) return y_leader - x_leader;
    if (x->max_freq_khz != y->max_freq_khz)
        return (int)((int64_t)y->max_freq_khz - (int64_t)x->max_freq_khz);
    return x->cpu_id - y->cpu_id;
}

/* Populate `info[0..n)` with topology-aware ordering of online CPUs.
 * Returns the number of CPUs populated (capped at max_n). */
static int v3_build_cpu_info(v3_cpu_info_t *info, int max_n)
{
    long online = sysconf(_SC_NPROCESSORS_ONLN);
    if (online < 1) online = 1;
    if (online > max_n) online = max_n;
    for (int i = 0; i < (int)online; i++) {
        info[i].cpu_id         = i;
        info[i].max_freq_khz   = v3_read_max_freq_khz(i);
        info[i].sibling_leader = v3_read_sibling_leader(i);
    }
    qsort(info, (size_t)online, sizeof(info[0]), v3_cpu_cmp);
    return (int)online;
}

static void v3_try_pin(int cpu_id)
{
    /* Best-effort: ignore failure (e.g. cgroup/taskset constraints). */
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu_id, &set);
    (void)pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
}

static void *worker_main(void *arg)
{
    worker_t *w = (worker_t *)arg;
    v3_try_pin(w->cpu_id);

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

    /* Phase 1: warm-up (state-only, no emission). */
    for (size_t i = scan_start; i < core_start; i++) {
        uint8_t c = (uint8_t)text[i];
        state = goto_tbl[(size_t)state * AC_ALPHABET_SIZE + c];
    }
    /* Phase 2: owned region (emit matches). */
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

static int v3_default_thread_count(void)
{
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    if (n < 1) n = 1;
    if (n > V3_MAX_CPUS) n = V3_MAX_CPUS;
    return (int)n;
}

static int v3_search(const ac_automaton_t *aut,
                     const char *text, size_t text_len,
                     const ac_searcher_config_t *cfg,
                     ac_match_list_t *out,
                     ac_thread_metric_t **out_metrics,
                     size_t *out_num_metrics)
{
    int nthreads = cfg->num_threads > 0 ? cfg->num_threads : v3_default_thread_count();
    if (nthreads < 1) nthreads = 1;

    size_t overlap = (aut->max_pattern_len > 0) ? (size_t)(aut->max_pattern_len - 1) : 0;
    if (nthreads == 1 || text_len <= overlap * 2 || text_len < (size_t)nthreads * 64) {
        const ac_searcher_t *seq = ac_searcher_find("sequential");
        if (!seq) return AC_E_NOT_FOUND;
        return seq->search(aut, text, text_len, cfg, out, out_metrics, out_num_metrics);
    }

    /* Topology discovery. */
    v3_cpu_info_t info[V3_MAX_CPUS];
    int n_cpus = v3_build_cpu_info(info, V3_MAX_CPUS);
    if (n_cpus < 1) n_cpus = 1;

    /* Detect whether we have any frequency data. Without it, all weights
     * collapse to 1 and the chunk distribution becomes equal-sized — the
     * same fallback as pthread_chunked_v2. */
    int have_freq_info = 0;
    for (int i = 0; i < n_cpus; i++) {
        if (info[i].max_freq_khz > 0) { have_freq_info = 1; break; }
    }

    /* Count how many threads will share each SMT group (indexed by the
     * group's leader cpu_id). A group with count > 1 has at least one
     * paired HT slot, so every thread mapped into it gets the HT
     * compensation factor applied. */
    int group_count[V3_MAX_CPUS];
    memset(group_count, 0, sizeof(group_count));
    for (int i = 0; i < nthreads; i++) {
        int leader = info[i % n_cpus].sibling_leader;
        if (leader >= 0 && leader < V3_MAX_CPUS) group_count[leader]++;
    }

    uint64_t total_weight = 0;
    uint32_t weights[V3_MAX_CPUS];
    for (int i = 0; i < nthreads; i++) {
        v3_cpu_info_t ci = info[i % n_cpus];
        uint32_t w;
        if (have_freq_info) {
            w = (ci.max_freq_khz > 0) ? ci.max_freq_khz : 1000000u;
        } else {
            w = 1u;
        }
        /* HT-pair compensation: if this CPU's SMT group has >1 thread
         * mapped into it, both threads on that physical core run at
         * ~0.55× of solo throughput. Halving the weight shrinks both
         * paired workers' chunks so they finish on time with the rest. */
        if (have_freq_info
            && ci.sibling_leader >= 0 && ci.sibling_leader < V3_MAX_CPUS
            && group_count[ci.sibling_leader] > 1) {
            w = (uint32_t)((uint64_t)w * V3_HT_FACTOR_NUM / V3_HT_FACTOR_DEN);
            if (w == 0) w = 1;
        }
        weights[i]    = w;
        total_weight += w;
    }
    if (total_weight == 0) total_weight = (uint64_t)nthreads;

    /* Allocate workers on cache-line boundaries. The struct attribute
     * pads sizeof(worker_t) up to V3_CACHE_LINE, so adjacent elements
     * sit on distinct lines. */
    worker_t *workers = NULL;
    if (posix_memalign((void **)&workers, V3_CACHE_LINE,
                       (size_t)nthreads * sizeof(*workers)) != 0) {
        return AC_E_NOMEM;
    }
    memset(workers, 0, (size_t)nthreads * sizeof(*workers));

    pthread_t *tids = calloc((size_t)nthreads, sizeof(*tids));
    if (!tids) { free(workers); return AC_E_NOMEM; }

    /* Compute per-worker byte ranges proportional to weight. Each chunk
     * boundary is rounded up to 64 B so chunks never share a cache line.
     * The last worker absorbs the remainder so total coverage is exact. */
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

static const ac_searcher_t k_pthread_chunked_v3 = {
    .name        = "pthread_chunked_v3",
    .description = "Pthreads chunks; topology-aware affinity + freq-weighted chunks",
    .search      = v3_search,
};

__attribute__((constructor))
static void v3_register(void) { ac_searcher_register(&k_pthread_chunked_v3); }
