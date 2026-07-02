/* pattern_sharded_prefix -- dictionary-level parallelism (idea 1).
 *
 * Strategy
 * --------
 * Partition the *dictionary* (not the text). Build K independent
 * Aho-Corasick sub-automata over disjoint pattern subsets and spawn K
 * workers; each worker scans the entire input with its own much smaller
 * DFA, emits only the matches belonging to its shard, and pushes them
 * into a thread-local ac_match_list_t. After pthread_join the master
 * concatenates the lists. No inter-chunk overlap is needed (every byte
 * is touched by every worker, just through a smaller DFA), no locks
 * appear on the hot path, and the unified automaton passed via `aut`
 * is read but never re-used by the workers -- the workers see only
 * their own sub_aut.
 *
 * Sharding policy: bucket by first byte mod K. States branch heavily
 * near the root; this tends to produce sub-automata of more uniform
 * footprint when the dictionary is biased toward English text (Snort).
 * The only policy retained after empirical evaluation showed it is the
 * only one to achieve speedup in the target regime (large automaton,
 * DRAM-bound).
 *
 * Each registration owns a private static cache keyed by a fingerprint
 * of the unified automaton + K + policy, so the warmup + iters loop in
 * bench_run pays the sub-automaton build cost exactly once and then
 * runs `iters` search-only iterations on the cached sub-automata.
 *
 * Correctness argument
 * --------------------
 * Pattern IDs are disjoint across shards by construction, so no two
 * shards can produce the same (end_pos, global_pid) pair -- the merge
 * is a plain concatenation. Each sub-automaton is the canonical AC
 * automaton over its shard's patterns and behaves identically to a
 * single-threaded scan over the same input with that pattern subset.
 * The union of shard match sets equals the unified match set, so
 * after ac_match_list_sort the output is bytewise identical to
 * `sequential`'s on the unified automaton. Validated by make test
 * across thread counts {1, 2, 3, 4, 7, 8}.
 *
 * Invariants (cf. docs/architecture/parallelism.md)
 * -------------------------------------------------
 * 1. Sub-automata are built BEFORE pthread_create. They are immutable
 *    while workers run; the happens-before edge supplied by
 *    pthread_create makes the build writes visible to the workers.
 * 2. No locks, mutexes, atomics or shared mutable state on the hot
 *    path. Each worker mutates only its own match list and its own
 *    DFA-state register.
 * 3. The pid_remap[k] table is allocated and written only during the
 *    build phase; workers read it via const pointers.
 *
 * See docs/proposals/idea_1.md for the full design rationale.
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

/* Cache-line size. Same convention as the other pthread searchers. */
#define SHARD_CACHE_LINE 64

typedef enum {
    SHARD_POLICY_PREFIX = 0,
} shard_policy_t;

/* One built shard: a self-contained sub-automaton plus a local-pid -> global-pid
 * remap table. Sized to the number of patterns in this shard. */
typedef struct {
    ac_automaton_t  sub_aut;        /* read-only after build              */
    int32_t        *pid_remap;      /* size: sub_aut.num_patterns         */
    int32_t         num_patterns;   /* mirrors sub_aut.num_patterns       */
} shard_t;

/* Process-global cache. One per registered policy, so the three
 * sharded searchers do not collide. Lazily filled on the first call
 * for a given (aut, K, policy) and reused across bench_run iterations. */
typedef struct {
    /* Fingerprint. Multi-field trick to avoid false cache hits when
     * caching by aut pointer alone is unsafe because test_correctness.c
     * declares ac_automaton_t on the stack and reuses the address. */
    const int32_t *goto_tbl_ptr;
    int32_t        num_states;
    int32_t        num_outputs;
    int32_t        num_patterns;
    int            K;               /* fingerprint of effective shard count */

    shard_t       *shards;          /* size: K (NULL if cache empty)        */
    /* Aggregate diagnostics computed at build time; printed once per
     * (aut, K, policy) on the first call. The metrics row in
     * `tcc_notes/tldr_metricas.md` is fed from these numbers. */
    size_t         total_sub_states;
    size_t         total_sub_bytes;
    int            diag_printed;
} shard_cache_t;

/* Sub-automata bind to a single worker thread, which scans the full
 * text and emits matches with global pids. */
typedef struct {
    int                    thread_id;
    const shard_t         *shard;
    const char            *text;
    size_t                 text_len;
    ac_match_list_t        local;       /* THREAD-LOCAL match list         */
    double                 seconds;
    int                    rc;
    int                    cpu;
    /* Pad so adjacent worker_t's never share a cache line (false-sharing
     * guard). Computed at compile time to keep the rest of the layout
     * deterministic across compilers. */
    char _pad[SHARD_CACHE_LINE - ((sizeof(int) * 3
                              + sizeof(const shard_t *)
                              + sizeof(const char *)
                              + sizeof(size_t)
                              + sizeof(ac_match_list_t)
                              + sizeof(double)) % SHARD_CACHE_LINE)];
} worker_t;

static int default_thread_count(void)
{
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    if (n < 1) n = 1;
    if (n > 256) n = 256;
    return (int)n;
}

/* ---- Cache management -------------------------------------------------- */

static void shard_release_cache(shard_cache_t *c)
{
    if (!c->shards) {
        c->goto_tbl_ptr = NULL;
        c->num_states = c->num_outputs = c->num_patterns = 0;
        c->K = 0;
        c->total_sub_states = 0;
        c->total_sub_bytes  = 0;
        c->diag_printed     = 0;
        return;
    }
    for (int k = 0; k < c->K; k++) {
        ac_automaton_destroy(&c->shards[k].sub_aut);
        free(c->shards[k].pid_remap);
    }
    free(c->shards);
    c->shards          = NULL;
    c->goto_tbl_ptr    = NULL;
    c->num_states      = 0;
    c->num_outputs     = 0;
    c->num_patterns    = 0;
    c->K               = 0;
    c->total_sub_states = 0;
    c->total_sub_bytes  = 0;
    c->diag_printed     = 0;
}

static int shard_cache_matches(const shard_cache_t *c,
                               const ac_automaton_t *aut,
                               int K)
{
    return c->goto_tbl_ptr == aut->goto_tbl
        && c->num_states   == aut->num_states
        && c->num_outputs  == aut->num_outputs
        && c->num_patterns == aut->num_patterns
        && c->K            == K;
}

/* ---- Sharding policy -------------------------------------------------- */

/* Bucket by first byte mod K. Empty patterns are not possible here
 * (ac_automaton_build rejects them with AC_E_PATTERN_EMPTY before this
 * code runs). */
static void policy_prefix(const ac_automaton_t *aut, int K, int32_t *assignment)
{
    int32_t n = aut->num_patterns;
    for (int32_t i = 0; i < n; i++) {
        uint8_t first = (uint8_t)aut->patterns[i].text[0];
        assignment[i] = (int32_t)((unsigned)first % (unsigned)K);
    }
}

/* ---- Sub-automaton build ---------------------------------------------- */

/* Build K sub-automata. On success, the cache is populated and
 * AC_OK is returned. On failure, partial state is torn down via
 * shard_release_cache() before returning. */
static int shard_build(shard_cache_t *c,
                       const ac_automaton_t *aut,
                       int K,
                       shard_policy_t policy)
{
    int32_t n = aut->num_patterns;

    int32_t *assignment = malloc((size_t)n * sizeof(*assignment));
    int32_t *shard_count = calloc((size_t)K, sizeof(*shard_count));
    if (!assignment || !shard_count) {
        free(assignment); free(shard_count);
        return AC_E_NOMEM;
    }

    (void)policy;
    policy_prefix(aut, K, assignment);

    for (int32_t i = 0; i < n; i++) shard_count[assignment[i]]++;

    /* Allocate the shard array + per-shard temporary buffers for
     * building (the per-shard pattern text pointers and lengths plus
     * the pid_remap). */
    shard_t *shards = calloc((size_t)K, sizeof(*shards));
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

    /* Pass 1: fill per-shard pid_remap (local -> global). */
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

    /* Pass 2: build each sub-automaton. We use ac_automaton_build
     * (sequential): sub-automata are by definition smaller than the
     * unified one, and the parallel build's pthread_create overhead
     * does not pay off at this scale. */
    size_t total_states = 0;
    size_t total_bytes  = 0;
    for (int k = 0; k < K; k++) {
        if (shard_count[k] == 0) {
            /* Empty shard -- legitimate when K > num_patterns or when
             * a prefix-bucket happens to be empty. Keep the slot but
             * skip building; the worker will do zero work. */
            continue;
        }
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

    /* Commit to the cache. */
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

/* Ensure the cache holds K sub-automata for `aut` under `policy`. */
static int shard_ensure_built(shard_cache_t *c,
                              const ac_automaton_t *aut,
                              int K,
                              shard_policy_t policy)
{
    if (shard_cache_matches(c, aut, K)) return AC_OK;
    shard_release_cache(c);
    return shard_build(c, aut, K, policy);
}

/* ---- Worker hot loop -------------------------------------------------- */

static void *worker_main(void *arg)
{
    worker_t *w = (worker_t *)arg;
    const ac_automaton_t *aut = &w->shard->sub_aut;
    const int32_t *AC_RESTRICT remap = w->shard->pid_remap;

    const int32_t *AC_RESTRICT goto_tbl    = aut->goto_tbl;
    const int32_t *AC_RESTRICT flat_offset = aut->flat_offset;
    const int32_t *AC_RESTRICT flat_count  = aut->flat_count;
    const int32_t *AC_RESTRICT flat_pids   = aut->flat_pids;
    const char    *AC_RESTRICT text        = w->text;
    const size_t               text_len    = w->text_len;

    /* Empty shard: no work to do. Still account zero time so the
     * per-thread metric row stays consistent. */
    if (aut->num_states <= 1) {
        w->rc = AC_OK;
        w->seconds = 0.0;
        w->cpu = ac_current_cpu();
        return NULL;
    }

    /* Pre-reserve to keep the first realloc()s out of the timed region.
     * Same constant as the other flat-output pthread searchers. */
    ac_match_list_reserve(&w->local, 4096);

    uint64_t t0 = bench_now_ns();

    int32_t state = 0;
    for (size_t i = 0; i < text_len; i++) {
        uint8_t c = (uint8_t)text[i];
        state = goto_tbl[(size_t)state * AC_ALPHABET_SIZE + c];

        int32_t cnt = flat_count[state];
        if (AC_UNLIKELY(cnt > 0)) {
            int32_t off = flat_offset[state];
            for (int32_t k = 0; k < cnt; k++) {
                /* Local pid -> global pid via per-shard remap table.
                 * remap[] is built before pthread_create, read-only
                 * here, so no synchronisation is needed. */
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

/* ---- Top-level search ------------------------------------------------- */

static int shard_search(const ac_automaton_t *aut,
                        const char *text, size_t text_len,
                        const ac_searcher_config_t *cfg,
                        ac_match_list_t *out,
                        ac_thread_metric_t **out_metrics,
                        size_t *out_num_metrics,
                        shard_cache_t *cache,
                        shard_policy_t policy,
                        const char *searcher_name)
{
    if (out_metrics)     *out_metrics     = NULL;
    if (out_num_metrics) *out_num_metrics = 0;

    int requested = cfg->num_threads > 0 ? cfg->num_threads : default_thread_count();
    if (requested < 1) requested = 1;

    /* Degenerate cases:
     *   - empty dictionary  -> nothing to match
     *   - K == 1            -> single sub-automaton equivalent to the
     *                          unified one; delegate to `sequential_flat`
     *                          (or `sequential`) so we do not pay an
     *                          extra pthread create+join for nothing.
     *   - K > num_patterns  -> cap so we never spawn an idle shard.
     */
    if (aut->num_patterns == 0) return AC_OK;

    int K = requested;
    if (K > aut->num_patterns) K = aut->num_patterns;

    if (K <= 1) {
        const ac_searcher_t *fb = ac_searcher_find("sequential_flat");
        if (!fb) fb = ac_searcher_find("sequential");
        if (!fb) return AC_E_NOT_FOUND;
        return fb->search(aut, text, text_len, cfg, out, out_metrics, out_num_metrics);
    }

    int rc = shard_ensure_built(cache, aut, K, policy);
    if (rc != AC_OK) return rc;

    if (cfg->verbose && !cache->diag_printed) {
        fprintf(stderr,
                "# %s: K=%d, sum_states=%zu (unified=%d), sum_bytes=%.2f MiB\n",
                searcher_name, K, cache->total_sub_states, aut->num_states,
                cache->total_sub_bytes / (double)(1u << 20));
        cache->diag_printed = 1;
    }

    /* Allocate worker_t array, each on its own cache line, so adjacent
     * workers never share one. posix_memalign is the portable spelling
     * of "aligned malloc" (calloc inherits the default arena alignment). */
    worker_t *workers = NULL;
    if (posix_memalign((void **)&workers, SHARD_CACHE_LINE,
                       (size_t)K * sizeof(*workers)) != 0) {
        return AC_E_NOMEM;
    }
    memset(workers, 0, (size_t)K * sizeof(*workers));

    pthread_t *tids = calloc((size_t)K, sizeof(*tids));
    if (!tids) { free(workers); return AC_E_NOMEM; }

    for (int i = 0; i < K; i++) {
        workers[i].thread_id = i;
        workers[i].shard     = &cache->shards[i];
        workers[i].text      = text;
        workers[i].text_len  = text_len;
        workers[i].rc        = AC_OK;
        ac_match_list_init(&workers[i].local);
    }

    int spawned = 0;
    for (int i = 0; i < K; i++) {
        int err = pthread_create(&tids[i], NULL, worker_main, &workers[i]);
        if (err != 0) {
            /* Best-effort recovery: join what we have, free what we
             * allocated, return AC_E_THREAD. */
            for (int j = 0; j < i; j++) pthread_join(tids[j], NULL);
            for (int j = 0; j < K; j++) ac_match_list_free(&workers[j].local);
            free(workers); free(tids);
            return AC_E_THREAD;
        }
        spawned = i + 1;
    }
    for (int i = 0; i < spawned; i++) pthread_join(tids[i], NULL);

    if (out_metrics && out_num_metrics) {
        ac_thread_metric_t *tm = calloc((size_t)spawned, sizeof(*tm));
        if (tm) {
            for (int i = 0; i < spawned; i++) {
                tm[i].thread_id     = workers[i].thread_id;
                tm[i].seconds       = workers[i].seconds;
                /* Every worker scans the whole input; that is the
                 * point of pattern sharding. The accounting must
                 * reflect that for a fair throughput-vs-K plot. */
                tm[i].bytes_scanned = text_len;
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

    /* Merge in deterministic order (thread_id asc). Shards have
     * disjoint global pids, so no deduplication is needed and a plain
     * concatenation is correct. The output is sorted by the caller
     * (correctness harness via ac_match_list_sort). */
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

/* ---- Registered searcher ---------------------------------------------- */

static shard_cache_t g_cache_prefix;

static int search_prefix(const ac_automaton_t *aut,
                         const char *text, size_t text_len,
                         const ac_searcher_config_t *cfg,
                         ac_match_list_t *out,
                         ac_thread_metric_t **m, size_t *nm)
{
    return shard_search(aut, text, text_len, cfg, out, m, nm,
                        &g_cache_prefix, SHARD_POLICY_PREFIX,
                        "pattern_sharded_prefix");
}

static const ac_searcher_t k_sharded_prefix = {
    .name        = "pattern_sharded_prefix",
    .description = "Dictionary-level parallelism (idea 1), first-byte prefix shards",
    .search      = search_prefix,
};

__attribute__((constructor))
static void shard_register(void)
{
    ac_searcher_register(&k_sharded_prefix);
}
