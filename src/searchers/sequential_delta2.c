/* sequential_delta2 -- 2-byte multi-step transition table (idea 3).
 *
 * Precomputes delta2[s * 65536 + ab] = goto_tbl[goto_tbl[s*256+a]*256+b],
 * so the search hot loop consumes TWO input bytes per indexed load.
 * This halves the per-byte dependent-load chain that limits a scalar
 * core -- a clean attack on the same bottleneck SIMD addresses
 * obliquely. Composes with the flat output table from idea 5: when an
 * emission is necessary, the inner loop reads flat_count/flat_offset
 * exactly as sequential_flat does.
 *
 * The table is huge: num_states * 65536 * 4 bytes for delta2 plus
 * num_states * 65536 * 1 byte for the emission mask. The footprint
 * grows linearly in num_states and exceeds 1 GiB at roughly 3300
 * states. Beyond that, this searcher transparently falls back to
 * `sequential` (chain walk) and prints one diagnostic line, so
 * benchmarks against large IDS dictionaries keep working without
 * special-casing the call site.
 *
 * The table is cached in a static keyed by `aut` pointer. Successive
 * calls with the same automaton (e.g. across benchmark iterations)
 * pay zero rebuild cost; a call with a different automaton (e.g. the
 * test harness moving on to the next case) frees the previous cache
 * and rebuilds. This is safe because the test harness invokes every
 * searcher single-threaded against any given automaton at a time. */

#include "ac_searcher.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 2^16 entries per state -- one for every (a, b) pair. Same byte
 * alphabet (AC_ALPHABET_SIZE = 256) the rest of the lab uses. */
#define D2_PAIRS (AC_ALPHABET_SIZE * AC_ALPHABET_SIZE)

/* Maximum acceptable footprint for delta2 + emit_mask combined. At
 * 1 GiB the table covers roughly num_states <= 3300. Dictionaries
 * larger than that fall back to the chain-walk sequential baseline,
 * which is correct (and the dissertation's cross-over point). */
#define D2_FOOTPRINT_LIMIT ((size_t)1u << 30)

/* Emission mask bits per (state, pair). When non-zero, the hot loop
 * leaves the pair-stepping fast path and emits matches for the
 * indicated positions before resuming. */
#define D2_EMIT_FIRST  ((uint8_t)1)   /* intermediate state s1 emits */
#define D2_EMIT_SECOND ((uint8_t)2)   /* final state    s2 emits     */

/* Cache fingerprint. Caching by the ac_automaton_t pointer alone is
 * unsafe in this codebase: the test harness declares the automaton
 * on the stack inside run_case() and the same stack address is
 * reused across cases, so two completely different automata share
 * an "aut" pointer at successive calls. The fields below
 * (goto_tbl pointer + state/output counts) form a multi-value
 * fingerprint that catches any rebuild: ac_automaton_build mallocs
 * a fresh goto_tbl per automaton, num_states/num_outputs both move
 * with the dictionary. */
static struct {
    const int32_t *goto_tbl_ptr;   /* aut->goto_tbl identity        */
    int32_t        num_states;     /* extra fingerprint bit         */
    int32_t        num_outputs;    /* extra fingerprint bit         */
    int32_t       *delta2;         /* size: num_states * D2_PAIRS   */
    uint8_t       *emit_mask;      /* size: num_states * D2_PAIRS   */
    int            disabled;       /* footprint > limit -> fallback */
} g_d2;

static int d2_cache_matches(const ac_automaton_t *aut)
{
    return g_d2.goto_tbl_ptr == aut->goto_tbl
        && g_d2.num_states   == aut->num_states
        && g_d2.num_outputs  == aut->num_outputs;
}

static void d2_release_cache(void)
{
    free(g_d2.delta2);
    free(g_d2.emit_mask);
    g_d2.delta2       = NULL;
    g_d2.emit_mask    = NULL;
    g_d2.goto_tbl_ptr = NULL;
    g_d2.num_states   = 0;
    g_d2.num_outputs  = 0;
    g_d2.disabled     = 0;
}

/* Build delta2 + emit_mask once for `aut`, or no-op if already cached.
 * Returns AC_OK on success including the footprint-guarded "disabled"
 * outcome (callers must check g_d2.disabled). Returns AC_E_NOMEM on
 * allocation failure. */
static int d2_ensure_built(const ac_automaton_t *aut)
{
    if (d2_cache_matches(aut)) return AC_OK;

    /* New automaton -- discard whatever was cached for the previous one. */
    d2_release_cache();

    const size_t n      = (size_t)aut->num_states;
    const size_t cells  = n * (size_t)D2_PAIRS;
    /* Overflow guard: cells * (4 + 1) bytes. */
    if (n > 0 && cells / n != (size_t)D2_PAIRS) {
        g_d2.goto_tbl_ptr = aut->goto_tbl;
        g_d2.num_states   = aut->num_states;
        g_d2.num_outputs  = aut->num_outputs;
        g_d2.disabled     = 1;
        fprintf(stderr, "# sequential_delta2: state count %zu overflows -- "
                        "delta2 disabled, falling back to sequential\n", n);
        return AC_OK;
    }
    const size_t footprint = cells * sizeof(int32_t) + cells * sizeof(uint8_t);

    if (footprint > D2_FOOTPRINT_LIMIT) {
        g_d2.goto_tbl_ptr = aut->goto_tbl;
        g_d2.num_states   = aut->num_states;
        g_d2.num_outputs  = aut->num_outputs;
        g_d2.disabled     = 1;
        fprintf(stderr, "# sequential_delta2: %.1f MiB for %zu states exceeds "
                        "%.1f MiB limit -- falling back to sequential\n",
                footprint / (double)(1u << 20), n,
                D2_FOOTPRINT_LIMIT / (double)(1u << 20));
        return AC_OK;
    }

    int32_t *delta2 = malloc(cells * sizeof(int32_t));
    uint8_t *emask  = malloc(cells * sizeof(uint8_t));
    if (!delta2 || !emask) {
        free(delta2); free(emask);
        return AC_E_NOMEM;
    }

    /* Build pass: for each state s and each pair (a, b),
     *   s1 = goto_tbl[s * 256 + a];
     *   s2 = goto_tbl[s1 * 256 + b];
     *   delta2[s, ab] = s2;
     *   emit_mask[s, ab] = (flat_count[s1] > 0 ? FIRST : 0)
     *                    | (flat_count[s2] > 0 ? SECOND : 0);
     * The double loop on (a, b) is structured to lift the s1 lookup
     * out of the inner loop -- s1 depends only on (s, a). */
    const int32_t *AC_RESTRICT goto_tbl   = aut->goto_tbl;
    const int32_t *AC_RESTRICT flat_count = aut->flat_count;
    const int32_t              num_states = aut->num_states;

    for (int32_t s = 0; s < num_states; s++) {
        const size_t base_s = (size_t)s * (size_t)D2_PAIRS;
        for (int a = 0; a < AC_ALPHABET_SIZE; a++) {
            int32_t  s1       = goto_tbl[(size_t)s * AC_ALPHABET_SIZE + a];
            uint8_t  emit_s1  = (flat_count[s1] > 0) ? D2_EMIT_FIRST : 0;
            const size_t base_sa = base_s + (size_t)a * AC_ALPHABET_SIZE;
            for (int b = 0; b < AC_ALPHABET_SIZE; b++) {
                int32_t s2 = goto_tbl[(size_t)s1 * AC_ALPHABET_SIZE + b];
                delta2[base_sa + (size_t)b] = s2;
                emask[base_sa + (size_t)b]  =
                    emit_s1 | (uint8_t)((flat_count[s2] > 0) ? D2_EMIT_SECOND : 0);
            }
        }
    }

    g_d2.goto_tbl_ptr = aut->goto_tbl;
    g_d2.num_states   = aut->num_states;
    g_d2.num_outputs  = aut->num_outputs;
    g_d2.delta2       = delta2;
    g_d2.emit_mask    = emask;
    g_d2.disabled     = 0;
    return AC_OK;
}

/* Emit all pids that the flat output table associates with `state`,
 * pinned to absolute text position `end_pos`. Pulled into a helper
 * to keep the pair-stepping loop readable. */
static int d2_emit_state(const ac_automaton_t *aut,
                         int32_t state, int64_t end_pos,
                         ac_match_list_t *out)
{
    int32_t cnt = aut->flat_count[state];
    if (cnt == 0) return AC_OK;
    int32_t off = aut->flat_offset[state];
    const int32_t *AC_RESTRICT pids = aut->flat_pids;
    for (int32_t k = 0; k < cnt; k++) {
        ac_match_t m = { .end_pos = end_pos, .pattern_id = pids[off + k] };
        int rc = ac_match_list_push(out, m);
        if (rc != AC_OK) return rc;
    }
    return AC_OK;
}

static int d2_search(const ac_automaton_t *aut,
                     const char *text, size_t text_len,
                     const ac_searcher_config_t *cfg,
                     ac_match_list_t *out,
                     ac_thread_metric_t **out_metrics,
                     size_t *out_num_metrics)
{
    if (out_metrics)     *out_metrics     = NULL;
    if (out_num_metrics) *out_num_metrics = 0;

    int rc = d2_ensure_built(aut);
    if (rc != AC_OK) return rc;

    if (g_d2.disabled) {
        /* Footprint above limit -- chain-walk sequential is the
         * authoritative fallback. The flat layout would also work,
         * but `sequential` is the dissertation's baseline and the
         * one whose behaviour is most thoroughly characterised. */
        const ac_searcher_t *fb = ac_searcher_find("sequential");
        if (!fb) return AC_E_NOT_FOUND;
        return fb->search(aut, text, text_len, cfg, out, out_metrics, out_num_metrics);
    }

    const int32_t *AC_RESTRICT goto_tbl   = aut->goto_tbl;
    const int32_t *AC_RESTRICT delta2     = g_d2.delta2;
    const uint8_t *AC_RESTRICT emit_mask  = g_d2.emit_mask;

    int32_t state = 0;
    size_t  i     = 0;

    /* Pair-stepping fast path: consume two input bytes per indexed
     * load into delta2. The emit_mask is the AC_UNLIKELY branch --
     * for IDS-style scans against a small Snort dictionary most
     * pairs are non-emitting, so the inner emit block never fires
     * on the typical byte. */
    while (i + 1 < text_len) {
        uint8_t a = (uint8_t)text[i];
        uint8_t b = (uint8_t)text[i + 1];
        size_t  key = (size_t)state * (size_t)D2_PAIRS
                    + (size_t)a * AC_ALPHABET_SIZE + (size_t)b;
        int32_t next_state = delta2[key];
        uint8_t emask      = emit_mask[key];

        if (AC_UNLIKELY(emask != 0)) {
            if (emask & D2_EMIT_FIRST) {
                /* The intermediate state after only the first byte
                 * (s1) emits. Reconstruct s1 from the single-byte
                 * goto -- cheaper than caching a third table. */
                int32_t s1 = goto_tbl[(size_t)state * AC_ALPHABET_SIZE + a];
                int err = d2_emit_state(aut, s1, (int64_t)i, out);
                if (err != AC_OK) return err;
            }
            if (emask & D2_EMIT_SECOND) {
                int err = d2_emit_state(aut, next_state, (int64_t)(i + 1), out);
                if (err != AC_OK) return err;
            }
        }
        state = next_state;
        i += 2;
    }

    /* Tail: at most one byte remains when text_len is odd. Falls back
     * to the scalar step / flat emission so the last byte's matches
     * are not lost. */
    if (i < text_len) {
        uint8_t c = (uint8_t)text[i];
        state = goto_tbl[(size_t)state * AC_ALPHABET_SIZE + c];
        int err = d2_emit_state(aut, state, (int64_t)i, out);
        if (err != AC_OK) return err;
    }

    return AC_OK;
}

static const ac_searcher_t k_d2_searcher = {
    .name        = "sequential_delta2",
    .description = "Sequential AC scan with 2-byte multi-step delta2 table (idea 3)",
    .search      = d2_search,
};

__attribute__((constructor))
static void d2_register(void) { ac_searcher_register(&k_d2_searcher); }
