#include "benchmark.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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

int bench_run(const ac_searcher_t *s,
              const ac_automaton_t *aut,
              const char *text, size_t text_len,
              const ac_searcher_config_t *cfg,
              int warmup, int iters,
              bench_result_t *out_result,
              ac_match_list_t *out_last_matches)
{
    if (!s || !aut || !text || !cfg || iters < 1 || !out_result) return AC_E_INVAL;

    ac_match_list_t matches;
    ac_match_list_init(&matches);

    /* Warm-up runs prime caches and let the kernel scheduler settle.
     * Their results are discarded. */
    for (int i = 0; i < warmup; i++) {
        matches.count = 0;
        int rc = s->search(aut, text, text_len, cfg, &matches, NULL, NULL);
        if (rc != AC_OK) { ac_match_list_free(&matches); return rc; }
    }

    double sum = 0.0, smin = 0.0, smax = 0.0;
    size_t last_match_count = 0;
    for (int i = 0; i < iters; i++) {
        matches.count = 0;
        uint64_t t0 = bench_now_ns();
        int rc = s->search(aut, text, text_len, cfg, &matches, NULL, NULL);
        uint64_t t1 = bench_now_ns();
        if (rc != AC_OK) { ac_match_list_free(&matches); return rc; }
        double sec = (double)(t1 - t0) / 1e9;
        if (i == 0 || sec < smin) smin = sec;
        if (i == 0 || sec > smax) smax = sec;
        sum += sec;
        last_match_count = matches.count;
    }

    out_result->searcher_name        = s->name;
    out_result->num_threads          = cfg->num_threads;
    out_result->bytes_scanned        = text_len;
    out_result->num_matches          = last_match_count;
    out_result->build_seconds        = 0.0;
    out_result->search_seconds_min   = smin;
    out_result->search_seconds_max   = smax;
    out_result->search_seconds_mean  = sum / iters;
    out_result->throughput_mbps_mean = (double)text_len /
                                       (out_result->search_seconds_mean * (double)(1u << 20));
    out_result->iterations           = iters;

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
    printf("%-22s %4s %12s %14s %14s %14s %12s %10s\n",
           "searcher", "thr", "bytes", "min(ms)", "mean(ms)", "max(ms)",
           "MB/s", "matches");
}

void bench_result_print(const bench_result_t *r, int show_header)
{
    if (show_header) bench_result_print_header();
    printf("%-22s %4d %12zu %14.3f %14.3f %14.3f %12.2f %10zu\n",
           r->searcher_name,
           r->num_threads,
           r->bytes_scanned,
           r->search_seconds_min  * 1000.0,
           r->search_seconds_mean * 1000.0,
           r->search_seconds_max  * 1000.0,
           r->throughput_mbps_mean,
           r->num_matches);
}
