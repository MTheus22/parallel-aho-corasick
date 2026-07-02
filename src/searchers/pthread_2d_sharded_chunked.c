/* pthread_2d_sharded_chunked -- 2-D parallelism: K (dictionary shards)
 * x N (text chunks). Implements idea 6 of the roadmap.
 *
 * Why
 * ---
 * Idea 1 (pattern sharding, prefix policy) demonstrated that splitting
 * the *dictionary* into K sub-automata recovers a slice of the cache
 * footprint that the unified DFA loses (~1.21x vs `sequential` on
 * Snort full + enron at K=8, saturating at K=2). It loses on RAM
 * bandwidth though: each worker scans the entire text, so total text
 * traffic is multiplied by K. At K=12 on a 1.32 GiB corpus the channel
 * saturates and the gain disappears.
 *
 * Idea 5 / pthread_chunked_flat demonstrated that splitting the
 * *text* into N chunks (with the standard L-1 overlap) extracts most
 * of the throughput available on the machine -- 3.64x on Snort full
 * at T=12 in the same regime. It does nothing about the DFA cache
 * blowout: every worker still walks the same gigantic goto_tbl.
 *
 * 2-D combines them. Build K sub-automata (prefix policy, the only
 * single-dim policy that actually wins on large dictionaries); for
 * each shard, spawn N workers that each scan one chunk of the text
 * through that shard's sub-automaton. Total workers = K * N = T.
 *
 *   - K cuts state-load DRAM latency (smaller DFA per worker).
 *   - N keeps text DRAM traffic at ~text_len * K, not text_len * T.
 *
 * The headline hypothesis from `docs/proposals/idea_6.md`: at
 * T=12 on i5-1235U, K=2 x N=6 beats `pthread_chunked_flat` (T=12)
 * on large dictionaries by trading some parallel text bandwidth for
 * localised state-transition cache hits.
 *
 * Strategy
 * --------
 * - K is preferred to be 2 (idea 1 saturated there for the prefix
 *   policy). Override at runtime with `AC_2D_K=<N>`. If T % K != 0
 *   or K > num_patterns we drop K to 1 and delegate to
 *   `pthread_chunked_flat`: a sub-automaton equal to the unified
 *   one would just double memory and do nothing useful.
 * - Sharding policy fixed at PREFIX (first-byte bucket). The other
 *   two policies registered by idea 1 lose on every dictionary size
 *   we measured; no reason to revisit them here.
 * - Each worker `(k, n)` runs sub_aut[k] over text chunk n with
 *   warm-up overlap = sub_aut[k].max_pattern_len - 1. The per-shard
 *   overlap is strictly tighter than the unified `aut->max_pattern_len`
 *   because shards never see patterns from other shards.
 * - Emission goes through the flat output table on each sub_aut
 *   (idea 5); local pid -> global pid via the shard's `pid_remap`.
 *
 * Correctness
 * -----------
 * Shards have disjoint global pid sets (built by bucketing the
 * unified pattern list once). For every text position p and every
 * global pid g, exactly one shard owns g and exactly one chunk owns
 * the byte position p -- so the (end_pos, pattern_id) pair is
 * reported by exactly one worker. Merge is plain concatenation.
 * Validated by `make test` across thread counts {1, 2, 3, 4, 7, 8}.
 *
 * Invariants (cf. docs/architecture/parallelism.md)
 * -------------------------------------------------
 * 1. Sub-automata are built BEFORE pthread_create and are immutable
 *    while workers run. The happens-before of pthread_create makes
 *    the build writes visible to workers.
 * 2. Each worker emits only matches with end_pos in [core_start,
 *    core_end). Matches discovered during the warm-up region
 *    [scan_start, core_start) belong to the previous chunk; the
 *    split-loop avoids enumerating them at all.
 * 3. No locks/mutexes/atomics on the hot path. Match list per worker;
 *    merge after pthread_join in the master.
 */

#include "ac_automaton.h"
#include "ac_searcher.h"
#include "benchmark.h"

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define TWOD_CACHE_LINE 64
#define TWOD_MAX_T      256
#define TWOD_DEFAULT_K  2

/* One built shard. Same shape as pattern_sharded.c::shard_t -- duplicated
 * here on purpose to keep this searcher self-contained (the other file's
 * cache is file-scope static and not reachable from here, and the spec
 * forbids modifying the shared headers just to expose it). */
typedef struct {
    ac_automaton_t  sub_aut;
    int32_t        *pid_remap;     /* size: sub_aut.num_patterns           */
    int32_t         num_patterns;
} twod_shard_t;

/* Process-global cache of built shards. Fingerprinted by the unified
 * automaton plus K -- both must match for a cache hit. Same multi-field
 * fingerprint trick as `pattern_sharded.c`:
 * caching by `aut` pointer alone is unsafe because the test harness
 * stack-allocates an ac_automaton_t and reuses the address. */
typedef struct {
    const int32_t *goto_tbl_ptr;
    int32_t        num_states;
    int32_t        num_outputs;
    int32_t        num_patterns;
    int            K;

    twod_shard_t  *shards;          /* size: K (NULL if cache empty)    */
    size_t         total_sub_states;
    size_t         total_sub_bytes;
    int            diag_printed;
} twod_cache_t;

/* Worker state. Padded to a full cache line so adjacent worker_t's in
 * the posix_memalign'd array never share one. */
typedef struct {
    int                    thread_id;
    int                    shard_idx;
    int                    chunk_idx;
    const ac_automaton_t  *sub_aut;
    const int32_t         *pid_remap;
    const char            *text;
    size_t                 scan_start;
    size_t                 core_start;
    size_t                 core_end;
    ac_match_list_t        local;
    double                 seconds;
    int                    rc;
    int                    cpu;
} __attribute__((aligned(TWOD_CACHE_LINE))) worker_t;

static int default_thread_count(void)
{
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    if (n < 1) n = 1;
    if (n > TWOD_MAX_T) n = TWOD_MAX_T;
    return (int)n;
}

/* ---- Cache management ------------------------------------------------- */

static void twod_release_cache(twod_cache_t *c)
{
    if (c->shards) {
        for (int k = 0; k < c->K; k++) {
            if (c->shards[k].num_patterns > 0)
                ac_automaton_destroy(&c->shards[k].sub_aut);
            free(c->shards[k].pid_remap);
        }
        free(c->shards);
    }
    c->shards            = NULL;
    c->goto_tbl_ptr      = NULL;
    c->num_states        = 0;
    c->num_outputs       = 0;
    c->num_patterns      = 0;
    c->K                 = 0;
    c->total_sub_states  = 0;
    c->total_sub_bytes   = 0;
    c->diag_printed      = 0;
}

static int twod_cache_matches(const twod_cache_t *c,
                              const ac_automaton_t *aut,
                              int K)
{
    return c->shards != NULL
        && c->goto_tbl_ptr == aut->goto_tbl
        && c->num_states   == aut->num_states
        && c->num_outputs  == aut->num_outputs
        && c->num_patterns == aut->num_patterns
        && c->K            == K;
}

/* ---- Shard build (prefix policy) -------------------------------------- */

/* PREFIX bucketing: `shard = pattern[0] % K`. The only single-dim
 * sharding policy that empirically wins on large dictionaries
 * (see `docs/searchers/pattern_sharded.md` headline numbers). */
static void twod_assign_prefix(const ac_automaton_t *aut, int K,
                               int32_t *assignment)
{
    int32_t n = aut->num_patterns;
    for (int32_t i = 0; i < n; i++) {
        uint8_t first = (uint8_t)aut->patterns[i].text[0];
        assignment[i] = (int32_t)((unsigned)first % (unsigned)K);
    }
}

static int twod_shard_build(twod_cache_t *c, const ac_automaton_t *aut, int K)
{
    int32_t n = aut->num_patterns;

    int32_t *assignment  = malloc((size_t)n * sizeof(*assignment));
    int32_t *shard_count = calloc((size_t)K, sizeof(*shard_count));
    if (!assignment || !shard_count) {
        free(assignment); free(shard_count);
        return AC_E_NOMEM;
    }
    twod_assign_prefix(aut, K, assignment);
    for (int32_t i = 0; i < n; i++) shard_count[assignment[i]]++;

    twod_shard_t *shards = calloc((size_t)K, sizeof(*shards));
    if (!shards) {
        free(assignment); free(shard_count);
        return AC_E_NOMEM;
    }
    for (int k = 0; k < K; k++) {
        shards[k].num_patterns = shard_count[k];
        if (shard_count[k] > 0) {
            shards[k].pid_remap = malloc((size_t)shard_count[k] * sizeof(int32_t));
            if (!shards[k].pid_remap) {
                for (int j = 0; j < k; j++) free(shards[j].pid_remap);
                free(shards);
                free(assignment); free(shard_count);
                return AC_E_NOMEM;
            }
        }
    }

    int32_t *cursor = calloc((size_t)K, sizeof(*cursor));
    if (!cursor) {
        for (int k = 0; k < K; k++) free(shards[k].pid_remap);
        free(shards);
        free(assignment); free(shard_count);
        return AC_E_NOMEM;
    }
    for (int32_t i = 0; i < n; i++) {
        int32_t k = assignment[i];
        shards[k].pid_remap[cursor[k]++] = i;
    }
    free(cursor);

    size_t total_states = 0;
    size_t total_bytes  = 0;
    for (int k = 0; k < K; k++) {
        if (shard_count[k] == 0) continue;
        const char **pats = malloc((size_t)shard_count[k] * sizeof(*pats));
        size_t      *lens = malloc((size_t)shard_count[k] * sizeof(*lens));
        if (!pats || !lens) {
            free(pats); free(lens);
            for (int j = 0; j <= k; j++) {
                if (shards[j].num_patterns > 0)
                    ac_automaton_destroy(&shards[j].sub_aut);
                free(shards[j].pid_remap);
            }
            free(shards);
            free(assignment); free(shard_count);
            return AC_E_NOMEM;
        }
        for (int32_t l = 0; l < shard_count[k]; l++) {
            int32_t gid = shards[k].pid_remap[l];
            pats[l] = aut->patterns[gid].text;
            lens[l] = (size_t)aut->patterns[gid].length;
        }
        int rc = ac_automaton_build(&shards[k].sub_aut, pats, lens,
                                    (size_t)shard_count[k]);
        free(pats);
        free(lens);
        if (rc != AC_OK) {
            for (int j = 0; j < k; j++) {
                if (shards[j].num_patterns > 0)
                    ac_automaton_destroy(&shards[j].sub_aut);
                free(shards[j].pid_remap);
            }
            free(shards[k].pid_remap);
            for (int j = k + 1; j < K; j++) free(shards[j].pid_remap);
            free(shards);
            free(assignment); free(shard_count);
            return rc;
        }
        total_states += (size_t)shards[k].sub_aut.num_states;
        total_bytes  += ac_automaton_memory_bytes(&shards[k].sub_aut);
    }

    free(assignment);
    free(shard_count);

    c->goto_tbl_ptr     = aut->goto_tbl;
    c->num_states       = aut->num_states;
    c->num_outputs      = aut->num_outputs;
    c->num_patterns     = aut->num_patterns;
    c->K                = K;
    c->shards           = shards;
    c->total_sub_states = total_states;
    c->total_sub_bytes  = total_bytes;
    c->diag_printed     = 0;
    return AC_OK;
}

static int twod_ensure_built(twod_cache_t *c, const ac_automaton_t *aut, int K)
{
    if (twod_cache_matches(c, aut, K)) return AC_OK;
    twod_release_cache(c);
    return twod_shard_build(c, aut, K);
}

/* ---- Worker hot loop -------------------------------------------------- */

static void *worker_main(void *arg)
{
    worker_t *w = (worker_t *)arg;
    const ac_automaton_t *aut = w->sub_aut;

    /* Empty shard (legitimate for the prefix policy when a first-byte
     * bucket is unused): no transitions to follow, no matches to emit.
     * Account zero seconds so the per-thread metric row is consistent. */
    if (aut->num_states <= 1) {
        w->rc      = AC_OK;
        w->seconds = 0.0;
        w->cpu     = ac_current_cpu();
        return NULL;
    }

    const int32_t *AC_RESTRICT goto_tbl    = aut->goto_tbl;
    const int32_t *AC_RESTRICT flat_offset = aut->flat_offset;
    const int32_t *AC_RESTRICT flat_count  = aut->flat_count;
    const int32_t *AC_RESTRICT flat_pids   = aut->flat_pids;
    const int32_t *AC_RESTRICT remap       = w->pid_remap;
    const char    *AC_RESTRICT text        = w->text;

    /* Match a chunked worker's pre-allocation. Heavier sizing was
     * measured at high T on dense workloads and regressed (memory
     * pressure vs. realloc savings). */
    ac_match_list_reserve(&w->local, 4096);

    uint64_t t0 = bench_now_ns();

    int32_t state = 0;
    const size_t scan_start = w->scan_start;
    const size_t core_start = w->core_start;
    const size_t core_end   = w->core_end;

    /* Phase 1: warm-up. Update DFA state only, never emit. */
    for (size_t i = scan_start; i < core_start; i++) {
        uint8_t c = (uint8_t)text[i];
        state = goto_tbl[(size_t)state * AC_ALPHABET_SIZE + c];
    }
    /* Phase 2: owned region. Emit flat (idea 5), with local->global remap. */
    for (size_t i = core_start; i < core_end; i++) {
        uint8_t c = (uint8_t)text[i];
        state = goto_tbl[(size_t)state * AC_ALPHABET_SIZE + c];

        int32_t cnt = flat_count[state];
        if (AC_UNLIKELY(cnt > 0)) {
            int32_t off = flat_offset[state];
            for (int32_t k = 0; k < cnt; k++) {
                ac_match_t m = {
                    .end_pos    = (int64_t)i,
                    .pattern_id = remap[flat_pids[off + k]],
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

/* ---- K selection ------------------------------------------------------ */

/* Return the K we will actually use given T threads and num_patterns
 * patterns. Strategy: prefer K = AC_2D_K (env, default 2). Drop to 1
 * when the chosen K does not divide T or exceeds num_patterns -- at
 * that point a sub-automaton equal to the unified one would just
 * waste memory and the caller should delegate to pthread_chunked_flat. */
static int twod_pick_k(int T, int num_patterns)
{
    if (T < 2) return 1;
    int desired = TWOD_DEFAULT_K;
    const char *env = getenv("AC_2D_K");
    if (env && env[0]) {
        int v = atoi(env);
        if (v >= 1) desired = v;
    }
    if (desired < 2)             return 1;
    if (desired > T)             return 1;
    if (desired > num_patterns)  return 1;
    if (T % desired != 0)        return 1;
    return desired;
}

/* ---- Top-level search ------------------------------------------------- */

static twod_cache_t g_cache;

static int twod_search(const ac_automaton_t *aut,
                       const char *text, size_t text_len,
                       const ac_searcher_config_t *cfg,
                       ac_match_list_t *out,
                       ac_thread_metric_t **out_metrics,
                       size_t *out_num_metrics)
{
    if (out_metrics)     *out_metrics     = NULL;
    if (out_num_metrics) *out_num_metrics = 0;

    int T = cfg->num_threads > 0 ? cfg->num_threads : default_thread_count();
    if (T < 1) T = 1;
    if (aut->num_patterns == 0) return AC_OK;

    /* T == 1: nothing to parallelise. Delegate to sequential_flat so
     * the flat layout (idea 5) is still exercised. */
    if (T == 1) {
        const ac_searcher_t *fb = ac_searcher_find("sequential_flat");
        if (!fb) fb = ac_searcher_find("sequential");
        if (!fb) return AC_E_NOT_FOUND;
        return fb->search(aut, text, text_len, cfg, out, out_metrics, out_num_metrics);
    }

    int K = twod_pick_k(T, aut->num_patterns);
    if (K <= 1) {
        /* No genuine 2-D shape available for this T -- the searcher
         * collapses to the 1-D chunked baseline. Delegate to it
         * directly rather than wrap it in a redundant single-shard
         * sub-automaton. */
        const ac_searcher_t *fb = ac_searcher_find("pthread_chunked_flat");
        if (!fb) fb = ac_searcher_find("sequential_flat");
        if (!fb) fb = ac_searcher_find("sequential");
        if (!fb) return AC_E_NOT_FOUND;
        return fb->search(aut, text, text_len, cfg, out, out_metrics, out_num_metrics);
    }
    int N = T / K;
    if (N < 1) N = 1;

    int rc = twod_ensure_built(&g_cache, aut, K);
    if (rc != AC_OK) return rc;

    /* Conservative tiny-text fallback: if any chunk would be smaller
     * than 64 bytes the chunking math has nothing useful to do.
     * Delegate, but only after the cache is warm so subsequent calls
     * with normal-sized inputs hit. */
    if (text_len < (size_t)N * 64) {
        const ac_searcher_t *fb = ac_searcher_find("sequential_flat");
        if (!fb) fb = ac_searcher_find("sequential");
        if (!fb) return AC_E_NOT_FOUND;
        return fb->search(aut, text, text_len, cfg, out, out_metrics, out_num_metrics);
    }

    if (cfg->verbose && !g_cache.diag_printed) {
        fprintf(stderr,
                "# pthread_2d_sharded_chunked: K=%d N=%d (T=%d), "
                "sum_states=%zu (unified=%d), sum_bytes=%.2f MiB\n",
                K, N, T, g_cache.total_sub_states, aut->num_states,
                g_cache.total_sub_bytes / (double)(1u << 20));
        g_cache.diag_printed = 1;
    }

    int total_workers = K * N;
    worker_t *workers = NULL;
    if (posix_memalign((void **)&workers, TWOD_CACHE_LINE,
                       (size_t)total_workers * sizeof(*workers)) != 0) {
        return AC_E_NOMEM;
    }
    memset(workers, 0, (size_t)total_workers * sizeof(*workers));

    pthread_t *tids = calloc((size_t)total_workers, sizeof(*tids));
    if (!tids) { free(workers); return AC_E_NOMEM; }

    /* Equal-sized text chunks across N. Last chunk absorbs the
     * remainder so total coverage is exact. The K dimension just
     * fans the same chunk layout across the K shards. */
    size_t core_size = (text_len + (size_t)N - 1) / (size_t)N;
    if (core_size == 0) core_size = 1;

    int spawned = 0;
    for (int k = 0; k < K; k++) {
        const twod_shard_t *sh = &g_cache.shards[k];
        const ac_automaton_t *sa = (sh->num_patterns > 0) ? &sh->sub_aut : NULL;
        /* Per-shard overlap. Empty shards have no patterns of their own,
         * so any overlap value works; pick 0. */
        size_t overlap = 0;
        if (sa && sa->max_pattern_len > 0)
            overlap = (size_t)(sa->max_pattern_len - 1);
        for (int n = 0; n < N; n++) {
            int wi = k * N + n;
            size_t cs = (size_t)n * core_size;
            size_t ce = cs + core_size;
            if (ce > text_len) ce = text_len;
            if (cs > text_len) cs = text_len;
            workers[wi].thread_id = wi;
            workers[wi].shard_idx = k;
            workers[wi].chunk_idx = n;
            workers[wi].sub_aut   = sa ? sa : &sh->sub_aut; /* empty shard: still valid pointer */
            workers[wi].pid_remap = sh->pid_remap;
            workers[wi].text      = text;
            workers[wi].core_start = cs;
            workers[wi].core_end   = ce;
            workers[wi].scan_start = (n == 0 || cs < overlap) ? 0 : cs - overlap;
            workers[wi].rc         = AC_OK;
            ac_match_list_init(&workers[wi].local);
            spawned = wi + 1;
        }
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
                /* Workers in the same shard scan disjoint chunks; workers
                 * across shards re-scan the same chunk against a different
                 * sub-automaton. Counting bytes_scanned per worker as the
                 * actually-touched range keeps the per-thread MB/s row
                 * honest. */
                tm[i].bytes_scanned = workers[i].core_end - workers[i].scan_start;
                tm[i].matches_found = workers[i].local.count;
                tm[i].cpu           = workers[i].cpu;
            }
            *out_metrics     = tm;
            *out_num_metrics = (size_t)spawned;
        }
    }

    /* Merge: shards have disjoint global pids and within a shard
     * chunk ownership is disjoint by [core_start, core_end), so no
     * (end_pos, pid) pair can be emitted by two workers. Plain
     * concatenation is correct. */
    rc = AC_OK;
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

static const ac_searcher_t k_pthread_2d_sharded_chunked = {
    .name        = "pthread_2d_sharded_chunked",
    .description = "2-D: K (prefix shards) x N (text chunks); flat emission (idea 6)",
    .search      = twod_search,
};

__attribute__((constructor))
static void twod_register(void)
{
    ac_searcher_register(&k_pthread_2d_sharded_chunked);
}
