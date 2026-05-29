#include "benchmark.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifndef AC_GIT_HASH
#  define AC_GIT_HASH "unknown"
#endif

uint64_t bench_now_ns(void)
{
    struct timespec ts;
    /* CLOCK_MONOTONIC is unaffected by NTP slew steps and walltime jumps,
     * making it the right choice for short, repeatable measurements. */
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

void bench_marker_start(bench_marker_t *m, const char *label)
{
    m->label = label;
    m->start_ns = bench_now_ns();
    m->end_ns = 0;
}

void bench_marker_end(bench_marker_t *m)
{
    m->end_ns = bench_now_ns();
}

double bench_marker_seconds(const bench_marker_t *m)
{
    return (double)(m->end_ns - m->start_ns) / 1e9;
}

static int dbl_cmp(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return (x < y) ? -1 : (x > y) ? 1 : 0;
}

void bench_result_free(bench_result_t *r)
{
    if (!r) return;
    free(r->iter_seconds);
    r->iter_seconds = NULL;
}

int bench_run(const ac_searcher_t *s,
              const ac_automaton_t *aut,
              const char *text, size_t text_len,
              const ac_searcher_config_t *cfg,
              int warmup, int iters,
              bench_result_t *out_result,
              ac_match_list_t *out_last_matches)
{
    if (!s || !aut || !text || !cfg || iters < 1 || !out_result) return AC_E_INVAL;

    memset(out_result, 0, sizeof(*out_result));

    double *samples = malloc((size_t)iters * sizeof(*samples));
    if (!samples) return AC_E_NOMEM;

    ac_match_list_t matches;
    ac_match_list_init(&matches);

    /* Warm-up runs prime caches and let the kernel scheduler settle.
     * Their results are discarded. The last warm-up run doubles as a free
     * probe of the effective worker count: we ask the searcher for its
     * per-thread metrics (whose length is the number of workers actually
     * launched) on a run we were going to throw away anyway, so no timed
     * iteration is perturbed. Searchers that run single-threaded leave the
     * metrics empty, which we map to 1 worker below. */
    int effective_workers = 0;
    for (int i = 0; i < warmup; i++) {
        matches.count = 0;
        int want_metrics = (i == warmup - 1);
        ac_thread_metric_t *tm = NULL;
        size_t ntm = 0;
        int rc = s->search(aut, text, text_len, cfg, &matches,
                           want_metrics ? &tm : NULL,
                           want_metrics ? &ntm : NULL);
        if (rc != AC_OK) { free(tm); ac_match_list_free(&matches); free(samples); return rc; }
        if (want_metrics) {
            if (ntm > 0) effective_workers = (int)ntm;
            free(tm);
        }
    }

    size_t last_match_count = 0;
    for (int i = 0; i < iters; i++) {
        matches.count = 0;
        uint64_t t0 = bench_now_ns();
        int rc = s->search(aut, text, text_len, cfg, &matches, NULL, NULL);
        uint64_t t1 = bench_now_ns();
        if (rc != AC_OK) { ac_match_list_free(&matches); free(samples); return rc; }
        samples[i] = (double)(t1 - t0) / 1e9;
        last_match_count = matches.count;
    }

    /* Order statistics over the per-iteration wall times. */
    double *sorted = malloc((size_t)iters * sizeof(*sorted));
    if (!sorted) { ac_match_list_free(&matches); free(samples); return AC_E_NOMEM; }
    memcpy(sorted, samples, (size_t)iters * sizeof(*sorted));
    qsort(sorted, (size_t)iters, sizeof(*sorted), dbl_cmp);

    double smin = sorted[0];
    double smax = sorted[iters - 1];
    double median = (iters & 1) ? sorted[iters / 2]
                                : 0.5 * (sorted[iters / 2 - 1] + sorted[iters / 2]);

    double sum = 0.0;
    for (int i = 0; i < iters; i++) sum += samples[i];
    double mean = sum / iters;

    /* Sample standard deviation (n-1). Undefined for a single iteration;
     * report 0 there rather than divide by zero. */
    double stdev = 0.0;
    if (iters > 1) {
        double sq = 0.0;
        for (int i = 0; i < iters; i++) { double d = samples[i] - mean; sq += d * d; }
        stdev = sqrt(sq / (double)(iters - 1));
    }
    free(sorted);

    const double MiB = (double)(1u << 20);
    out_result->searcher_name          = s->name;
    out_result->num_threads            = cfg->num_threads;
    out_result->effective_workers      = effective_workers > 0 ? effective_workers : 1;
    out_result->bytes_scanned          = text_len;
    out_result->num_matches            = last_match_count;
    out_result->build_seconds          = 0.0;
    out_result->search_seconds_min     = smin;
    out_result->search_seconds_max     = smax;
    out_result->search_seconds_mean    = mean;
    out_result->search_seconds_median  = median;
    out_result->search_seconds_stdev   = stdev;
    out_result->cv_pct                 = (mean > 0.0) ? 100.0 * stdev / mean : 0.0;
    out_result->throughput_mbps_mean   = (mean   > 0.0) ? (double)text_len / (mean   * MiB) : 0.0;
    out_result->throughput_mbps_median = (median > 0.0) ? (double)text_len / (median * MiB) : 0.0;
    out_result->throughput_gbps_mean   = (mean   > 0.0) ? (double)text_len * 8.0 / (mean * 1e9) : 0.0;
    out_result->ns_per_byte_mean       = (text_len > 0) ? mean * 1e9 / (double)text_len : 0.0;
    out_result->iterations             = iters;
    out_result->iter_seconds           = samples;   /* ownership transfers to result */

    if (out_last_matches) {
        ac_match_list_free(out_last_matches);
        *out_last_matches = matches;
    } else {
        ac_match_list_free(&matches);
    }
    return AC_OK;
}

void bench_result_print_header(void)
{
    /* The first 8 columns are FROZEN: run_overnight_sweep.sh extracts them
     * positionally ($1..$8) and detects completion via `grep "^<name>\s"`.
     * Everything after `matches` is appended and safe to evolve. */
    printf("%-22s %4s %12s %14s %14s %14s %12s %10s %14s %8s %10s %6s\n",
           "searcher", "thr", "bytes", "min(ms)", "mean(ms)", "max(ms)",
           "MB/s", "matches",
           "median(ms)", "cv%", "Gbps", "wrk");
}

void bench_result_print(const bench_result_t *r, int show_header)
{
    if (show_header) bench_result_print_header();
    printf("%-22s %4d %12zu %14.3f %14.3f %14.3f %12.2f %10zu %14.3f %8.2f %10.4f %6d\n",
           r->searcher_name,
           r->num_threads,
           r->bytes_scanned,
           r->search_seconds_min  * 1000.0,
           r->search_seconds_mean * 1000.0,
           r->search_seconds_max  * 1000.0,
           r->throughput_mbps_mean,
           r->num_matches,
           r->search_seconds_median * 1000.0,
           r->cv_pct,
           r->throughput_gbps_mean,
           r->effective_workers);

    /* Per-iteration wall times: lets post-processing detect thermal drift
     * (a monotone climb iteration-over-iteration) and recompute any
     * statistic. Printed as a `#` comment so the sweep's grep/awk skip it. */
    if (r->iter_seconds && r->iterations > 0) {
        printf("#   iter_ms:");
        for (int i = 0; i < r->iterations; i++)
            printf(" %.3f", r->iter_seconds[i] * 1000.0);
        printf("\n");
    }
    printf("#   workers=%d  median_MB/s=%.2f  ns/byte=%.4f  stdev_ms=%.3f\n",
           r->effective_workers,
           r->throughput_mbps_median,
           r->ns_per_byte_mean,
           r->search_seconds_stdev * 1000.0);
}

void bench_result_print_csv_header(void)
{
    printf("git_hash,searcher,threads_req,effective_workers,bytes,matches,"
           "min_ms,mean_ms,median_ms,max_ms,stdev_ms,cv_pct,"
           "mbps_mean,mbps_median,gbps_mean,ns_per_byte,iters,iter_ms\n");
}

void bench_result_print_csv(const bench_result_t *r, int show_header)
{
    if (show_header) bench_result_print_csv_header();
    printf("%s,%s,%d,%d,%zu,%zu,%.6f,%.6f,%.6f,%.6f,%.6f,%.4f,%.4f,%.4f,%.6f,%.6f,%d,",
           AC_GIT_HASH,
           r->searcher_name,
           r->num_threads,
           r->effective_workers,
           r->bytes_scanned,
           r->num_matches,
           r->search_seconds_min    * 1000.0,
           r->search_seconds_mean   * 1000.0,
           r->search_seconds_median * 1000.0,
           r->search_seconds_max    * 1000.0,
           r->search_seconds_stdev  * 1000.0,
           r->cv_pct,
           r->throughput_mbps_mean,
           r->throughput_mbps_median,
           r->throughput_gbps_mean,
           r->ns_per_byte_mean,
           r->iterations);
    /* iter_ms as a ';'-joined field so the row stays one CSV record. */
    for (int i = 0; i < r->iterations; i++)
        printf("%s%.3f", i ? ";" : "", r->iter_seconds ? r->iter_seconds[i] * 1000.0 : 0.0);
    printf("\n");
}
