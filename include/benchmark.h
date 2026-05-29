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

/* End-to-end benchmark result for one searcher run.
 *
 * The first block of fields (through throughput_mbps_mean / iterations) is
 * kept in its original order on purpose: the text table emitted by
 * bench_result_print() preserves the historical 8-column layout
 * (searcher, thr, bytes, min, mean, max, MB/s, matches) so that
 * scripts/run_overnight_sweep.sh -- which detects a completed run with
 * `grep "^<searcher>\s"` and extracts positional columns $1..$8 -- keeps
 * working unchanged. New metrics are appended as trailing table columns
 * and as `#`-prefixed comment lines, both of which the sweep ignores. */
typedef struct {
    const char *searcher_name;
    int         num_threads;            /* requested (cfg->num_threads)      */
    size_t      bytes_scanned;
    size_t      num_matches;
    double      build_seconds;
    double      search_seconds_min;
    double      search_seconds_mean;
    double      search_seconds_max;
    double      throughput_mbps_mean;   /* MB = 1<<20                        */
    int         iterations;

    /* ---- Extended metrics (appended; never reorder the block above) ---- */
    int         effective_workers;      /* worker threads actually launched  */
    double      search_seconds_median;  /* robust central tendency           */
    double      search_seconds_stdev;   /* sample stdev (n-1); 0 if iters<2  */
    double      cv_pct;                  /* 100 * stdev / mean (noise gauge)  */
    double      throughput_gbps_mean;   /* bits/s in 1e9 units (IDS lit.)    */
    double      throughput_mbps_median; /* MB/s from the median time         */
    double      ns_per_byte_mean;       /* normalised cost per input byte    */

    /* Per-iteration wall times (seconds), length = iterations. Owned by the
     * result; release with bench_result_free(). NULL if not captured. */
    double     *iter_seconds;
} bench_result_t;

/* Run the searcher `iters` times after `warmup` warm-up runs. The text,
 * automaton and matches list are reused across iterations; the matches
 * list is reset before each iteration so accumulated allocations are
 * reused (capturing realistic steady-state behaviour).
 *
 * The effective worker count is sampled once on the last warm-up run (so
 * it costs nothing extra and never perturbs a timed iteration). With
 * warmup == 0 it is not sampled and defaults to 1. */
int    bench_run(const ac_searcher_t *s,
                 const ac_automaton_t *aut,
                 const char *text, size_t text_len,
                 const ac_searcher_config_t *cfg,
                 int warmup, int iters,
                 bench_result_t *out_result,
                 ac_match_list_t *out_last_matches);

/* Release heap owned by a bench_result_t (currently iter_seconds). Safe to
 * call on a zero-initialised or already-freed result. */
void   bench_result_free(bench_result_t *r);

/* Human-readable table. Backward-compatible 8-column prefix + appended
 * columns; emits `# iter_ms:` / `# workers:` comment lines after the row. */
void   bench_result_print(const bench_result_t *r, int show_header);
void   bench_result_print_header(void);

/* Machine-readable CSV (one header row, one row per result). Opt-in via the
 * CLI `--format csv`; never emitted by default, so the sweep's text logs
 * are unaffected. */
void   bench_result_print_csv(const bench_result_t *r, int show_header);
void   bench_result_print_csv_header(void);

#endif /* BENCHMARK_H */
