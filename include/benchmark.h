#ifndef BENCHMARK_H
#define BENCHMARK_H

#include "ac_common.h"
#include "ac_searcher.h"

/* High-precision wall clock (CLOCK_MONOTONIC). Always returns ns. */
uint64_t bench_now_ns(void);

/* Lightweight timer marker pair. Cheap enough to embed in inner phases. */
typedef struct {
    const char *label;
    uint64_t    start_ns;
    uint64_t    end_ns;
} bench_marker_t;

void   bench_marker_start(bench_marker_t *m, const char *label);
void   bench_marker_end(bench_marker_t *m);
double bench_marker_seconds(const bench_marker_t *m);

/* End-to-end benchmark result for one searcher run. */
typedef struct {
    const char *searcher_name;
    int         num_threads;
    size_t      bytes_scanned;
    size_t      num_matches;
    double      build_seconds;
    double      search_seconds_min;
    double      search_seconds_mean;
    double      search_seconds_max;
    double      throughput_mbps_mean;   /* MB = 1<<20 */
    int         iterations;
} bench_result_t;

/* Run the searcher `iters` times after `warmup` warm-up runs. The text,
 * automaton and matches list are reused across iterations; the matches
 * list is reset before each iteration so accumulated allocations are
 * reused (capturing realistic steady-state behaviour). */
int    bench_run(const ac_searcher_t *s,
                 const ac_automaton_t *aut,
                 const char *text, size_t text_len,
                 const ac_searcher_config_t *cfg,
                 int warmup, int iters,
                 bench_result_t *out_result,
                 ac_match_list_t *out_last_matches);

void   bench_result_print(const bench_result_t *r, int show_header);
void   bench_result_print_header(void);

#endif /* BENCHMARK_H */
