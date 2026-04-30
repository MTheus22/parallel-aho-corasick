#include "ac_automaton.h"
#include "ac_match.h"
#include "ac_searcher.h"
#include "benchmark.h"

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

static void usage(const char *p)
{
    fprintf(stderr,
"Usage: %s --patterns FILE --input FILE [options]\n"
"\n"
"  --patterns FILE       Pattern dictionary (one pattern per line, # comments)\n"
"  --input    FILE       Input text/binary to scan\n"
"  --searcher NAME       Searcher to run (default: all registered)\n"
"  --threads  N          Threads for parallel searchers (default: nproc)\n"
"  --chunk    BYTES      Suggested chunk size (0 = auto)\n"
"  --warmup   N          Warm-up iterations (default 1)\n"
"  --iters    N          Timed iterations (default 5)\n"
"  --print-matches       Print every match (sorted)\n"
"  --per-thread          Print per-thread metrics\n"
"  --list                List registered searchers and exit\n"
"  --help                This message\n",
        p);
}

static int slurp(const char *path, char **out, size_t *out_len)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) { perror(path); return AC_E_NOT_FOUND; }
    struct stat st;
    if (fstat(fd, &st) < 0) { perror("fstat"); close(fd); return AC_E_INVAL; }
    size_t len = (size_t)st.st_size;
    char *buf = malloc(len ? len : 1);
    if (!buf) { close(fd); return AC_E_NOMEM; }
    size_t got = 0;
    while (got < len) {
        ssize_t n = read(fd, buf + got, len - got);
        if (n <= 0) { free(buf); close(fd); return AC_E_INVAL; }
        got += (size_t)n;
    }
    close(fd);
    *out = buf;
    *out_len = len;
    return AC_OK;
}

static void print_matches(const ac_automaton_t *aut, ac_match_list_t *l)
{
    ac_match_list_sort(l);
    for (size_t i = 0; i < l->count; i++) {
        const ac_match_t *m = &l->data[i];
        const ac_pattern_t *p = &aut->patterns[m->pattern_id];
        int64_t start = m->end_pos - p->length + 1;
        printf("  match: pat=%-3d start=%-10lld end=%-10lld len=%-4d \"",
               m->pattern_id,
               (long long)start, (long long)m->end_pos, p->length);
        for (int k = 0; k < p->length; k++) {
            unsigned char c = (unsigned char)p->text[k];
            if (c >= 32 && c < 127) putchar(c);
            else printf("\\x%02x", c);
        }
        printf("\"\n");
    }
}

int main(int argc, char **argv)
{
    const char *patterns_path = NULL;
    const char *input_path    = NULL;
    const char *searcher_name = NULL;
    int   threads     = 0;
    size_t chunk_size = 0;
    int   warmup      = 1;
    int   iters       = 5;
    int   print_match = 0;
    int   per_thread  = 0;
    int   list_only   = 0;

    static struct option opts[] = {
        {"patterns",      required_argument, 0, 'p'},
        {"input",         required_argument, 0, 'i'},
        {"searcher",      required_argument, 0, 's'},
        {"threads",       required_argument, 0, 't'},
        {"chunk",         required_argument, 0, 'c'},
        {"warmup",        required_argument, 0, 'w'},
        {"iters",         required_argument, 0, 'n'},
        {"print-matches", no_argument,       0, 'm'},
        {"per-thread",    no_argument,       0, 'T'},
        {"list",          no_argument,       0, 'l'},
        {"help",          no_argument,       0, 'h'},
        {0,0,0,0}
    };
    int o;
    while ((o = getopt_long(argc, argv, "p:i:s:t:c:w:n:mTlh", opts, NULL)) != -1) {
        switch (o) {
        case 'p': patterns_path = optarg; break;
        case 'i': input_path    = optarg; break;
        case 's': searcher_name = optarg; break;
        case 't': threads       = atoi(optarg); break;
        case 'c': chunk_size    = strtoull(optarg, NULL, 10); break;
        case 'w': warmup        = atoi(optarg); break;
        case 'n': iters         = atoi(optarg); break;
        case 'm': print_match   = 1; break;
        case 'T': per_thread    = 1; break;
        case 'l': list_only     = 1; break;
        case 'h': usage(argv[0]); return 0;
        default:  usage(argv[0]); return 2;
        }
    }

    if (list_only) {
        printf("Registered searchers (%zu):\n", ac_searcher_count());
        for (size_t i = 0; i < ac_searcher_count(); i++) {
            const ac_searcher_t *s = ac_searcher_at(i);
            printf("  %-22s  %s\n", s->name, s->description ? s->description : "");
        }
        return 0;
    }

    if (!patterns_path || !input_path) { usage(argv[0]); return 2; }

    /* ---- Phase 1: build automaton (sequential / master) ----------------- */
    char  **pats = NULL;
    size_t *plens = NULL, npats = 0;
    int rc = ac_patterns_load_file(patterns_path, &pats, &plens, &npats);
    if (rc != AC_OK) { fprintf(stderr, "Failed to load patterns: %d\n", rc); return 1; }
    if (npats == 0) { fprintf(stderr, "No patterns in %s\n", patterns_path); return 1; }

    char *text = NULL; size_t text_len = 0;
    rc = slurp(input_path, &text, &text_len);
    if (rc != AC_OK) { ac_patterns_free(pats, plens, npats); return 1; }

    ac_automaton_t aut;
    bench_marker_t build_m;
    bench_marker_start(&build_m, "build");
    rc = ac_automaton_build(&aut, (const char *const *)pats, plens, npats);
    bench_marker_end(&build_m);
    if (rc != AC_OK) {
        fprintf(stderr, "automaton build failed: %d\n", rc);
        free(text); ac_patterns_free(pats, plens, npats);
        return 1;
    }

    printf("# Aho-Corasick laboratory\n");
    printf("# patterns:   %zu (max len %d, min len %d)\n",
           npats, aut.max_pattern_len, aut.min_pattern_len);
    printf("# automaton:  %d states, %.2f KiB\n",
           aut.num_states, ac_automaton_memory_bytes(&aut) / 1024.0);
    printf("# input:      %zu bytes (%.2f MiB)\n",
           text_len, text_len / (double)(1u << 20));
    printf("# build time: %.3f ms\n", bench_marker_seconds(&build_m) * 1000.0);
    printf("# warmup=%d iters=%d threads=%d\n", warmup, iters, threads);
    printf("\n");

    /* ---- Phase 2: run searcher(s) -------------------------------------- */
    ac_searcher_config_t cfg = (ac_searcher_config_t){
        .num_threads = threads,
        .chunk_size  = chunk_size,
        .verbose     = 0,
        .user        = NULL,
    };

    bench_result_print_header();

    int header_done = 1;
    int run_count = 0;
    int any_failed = 0;

    /* If a searcher was explicitly named, run only that one; otherwise
     * sweep every registered searcher (handy as a smoke test). */
    size_t total = ac_searcher_count();
    for (size_t idx = 0; idx < total; idx++) {
        const ac_searcher_t *s = ac_searcher_at(idx);
        if (searcher_name && strcmp(s->name, searcher_name) != 0) continue;

        ac_match_list_t last;
        ac_match_list_init(&last);
        bench_result_t r;
        rc = bench_run(s, &aut, text, text_len, &cfg, warmup, iters, &r, &last);
        if (rc != AC_OK) {
            fprintf(stderr, "%s: search failed (%d)\n", s->name, rc);
            ac_match_list_free(&last);
            any_failed = 1;
            continue;
        }
        bench_result_print(&r, !header_done);
        header_done = 1;
        run_count++;

        /* Per-thread metrics: re-run once with the metrics hook. */
        if (per_thread) {
            ac_thread_metric_t *tm = NULL; size_t ntm = 0;
            ac_match_list_t throwaway;
            ac_match_list_init(&throwaway);
            s->search(&aut, text, text_len, &cfg, &throwaway, &tm, &ntm);
            for (size_t k = 0; k < ntm; k++) {
                printf("    [t%02d] %.3f ms, %zu bytes, %zu matches (%.2f MB/s)\n",
                       tm[k].thread_id, tm[k].seconds * 1000.0,
                       tm[k].bytes_scanned, tm[k].matches_found,
                       tm[k].bytes_scanned / (tm[k].seconds * (double)(1u << 20)));
            }
            free(tm);
            ac_match_list_free(&throwaway);
        }

        if (print_match) print_matches(&aut, &last);
        ac_match_list_free(&last);
    }

    if (run_count == 0) {
        fprintf(stderr, "No searcher matched %s\n",
                searcher_name ? searcher_name : "(any)");
        any_failed = 1;
    }

    ac_automaton_destroy(&aut);
    free(text);
    ac_patterns_free(pats, plens, npats);
    return any_failed ? 1 : 0;
}
