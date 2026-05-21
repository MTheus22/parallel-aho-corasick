/* sequential_flat — single-threaded baseline AC scan using the flat
 * output table built by idea 5 (see docs/proposals/idea_5.md and
 * docs/architecture/flat-outputs.md).
 *
 * Identical control flow to `sequential` except the emission step.
 * Where `sequential` walks two nested chains (own_out_head + outputs
 * over the dict_suffix chain) and pays a chain of dependent loads on
 * every match, this variant reads (flat_offset[state], flat_count[state])
 * and runs one contiguous int32_t loop. Same multiset of pids in the
 * same order, so the output match list is bytewise identical to
 * `sequential`'s after ac_match_list_sort.
 *
 * The win grows with dictionary size, because the chain walk
 * increasingly misses cache while flat_pids stays linear. The point
 * of this file is to expose the layout change in isolation; threaded
 * variants build on it (see pthread_chunked_flat.c).
 */

#include "ac_searcher.h"

#include <stdint.h>
#include <stdlib.h>

static int seq_flat_search(const ac_automaton_t *aut,
                           const char *text, size_t text_len,
                           const ac_searcher_config_t *cfg,
                           ac_match_list_t *out,
                           ac_thread_metric_t **out_metrics,
                           size_t *out_num_metrics)
{
    (void)cfg;
    if (out_metrics)     *out_metrics     = NULL;
    if (out_num_metrics) *out_num_metrics = 0;

    const int32_t *AC_RESTRICT goto_tbl    = aut->goto_tbl;
    const int32_t *AC_RESTRICT flat_offset = aut->flat_offset;
    const int32_t *AC_RESTRICT flat_count  = aut->flat_count;
    const int32_t *AC_RESTRICT flat_pids   = aut->flat_pids;

    int32_t state = 0;
    for (size_t i = 0; i < text_len; i++) {
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
                int rc = ac_match_list_push(out, m);
                if (rc != AC_OK) return rc;
            }
        }
    }
    return AC_OK;
}

static const ac_searcher_t k_seq_flat_searcher = {
    .name        = "sequential_flat",
    .description = "Sequential AC scan reading the flat output table (idea 5)",
    .search      = seq_flat_search,
};

__attribute__((constructor))
static void seq_flat_register(void) { ac_searcher_register(&k_seq_flat_searcher); }
