#ifndef AC_SEARCHER_H
#define AC_SEARCHER_H

#include "ac_automaton.h"
#include "ac_match.h"

/* ---- Pluggable searcher interface -------------------------------------
 * Each implementation (sequential, pthread-chunked, future SIMD/OpenMP/
 * lock-free variants...) supplies one ac_searcher_t and registers it
 * once at program load time. The benchmark harness and the CLI select
 * implementations by name -- nothing else needs to change to plug in a
 * new variant.
 * ----------------------------------------------------------------------- */

/* Per-thread observability. Optional output of a search call. */
typedef struct {
    int    thread_id;
    double seconds;
    size_t bytes_scanned;
    size_t matches_found;
} ac_thread_metric_t;

typedef struct {
    int    num_threads;        /* honoured by parallel searchers; 0 = auto */
    size_t chunk_size;         /* 0 = derive from text/num_threads        */
    int    verbose;
    void  *user;               /* implementation-specific extension hook  */
} ac_searcher_config_t;

typedef struct ac_searcher {
    const char *name;
    const char *description;

    /* Run a full search of `text[0..text_len)` against `aut`, appending
     * results to `out_matches`. If `out_thread_metrics` is non-NULL the
     * implementation may set *out_thread_metrics to a malloc()ed array
     * of length *out_num_thread_metrics owned by the caller. Sequential
     * searchers may leave them NULL/0. */
    int (*search)(const ac_automaton_t *aut,
                  const char *text, size_t text_len,
                  const ac_searcher_config_t *cfg,
                  ac_match_list_t *out_matches,
                  ac_thread_metric_t **out_thread_metrics,
                  size_t *out_num_thread_metrics);
} ac_searcher_t;

/* Registry. Implementations call ac_searcher_register() from a
 * __attribute__((constructor)) so that simply linking the .o into the
 * binary makes them available. */
int                 ac_searcher_register(const ac_searcher_t *s);
const ac_searcher_t *ac_searcher_find(const char *name);
size_t              ac_searcher_count(void);
const ac_searcher_t *ac_searcher_at(size_t i);

#endif /* AC_SEARCHER_H */
