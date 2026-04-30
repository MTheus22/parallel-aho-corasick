/* Baseline sequential Aho-Corasick searcher.
 *
 * One thread, one pass over the text. This is the reference implementation
 * every parallel searcher is benchmarked against. Keep it small and
 * straightforward -- correctness here is load-bearing. */

#include "ac_searcher.h"

#include <stdint.h>
#include <stdlib.h>

static int seq_search(const ac_automaton_t *aut,
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
    const int32_t *AC_RESTRICT own_head    = aut->own_out_head;
    const int32_t *AC_RESTRICT dict_suffix = aut->dict_suffix;
    const ac_output_entry_t *AC_RESTRICT outputs = aut->outputs;

    int32_t state = 0;
    for (size_t i = 0; i < text_len; i++) {
        uint8_t c = (uint8_t)text[i];
        state = goto_tbl[(size_t)state * AC_ALPHABET_SIZE + c];

        /* Fast path: most states have no own outputs and no dict suffix. */
        if (AC_UNLIKELY(own_head[state] != AC_NIL || dict_suffix[state] != AC_NIL)) {
            int32_t l = (own_head[state] != AC_NIL) ? state : dict_suffix[state];
            while (l != AC_NIL) {
                for (int32_t o = own_head[l]; o != AC_NIL; o = outputs[o].next) {
                    ac_match_t m = {
                        .end_pos    = (int64_t)i,
                        .pattern_id = outputs[o].pattern_id,
                    };
                    int rc = ac_match_list_push(out, m);
                    if (rc != AC_OK) return rc;
                }
                l = dict_suffix[l];
            }
        }
    }
    return AC_OK;
}

static const ac_searcher_t k_seq_searcher = {
    .name        = "sequential",
    .description = "Single-threaded baseline AC scan",
    .search      = seq_search,
};

__attribute__((constructor))
static void seq_register(void) { ac_searcher_register(&k_seq_searcher); }
