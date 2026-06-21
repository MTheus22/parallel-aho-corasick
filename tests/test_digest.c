/* Digest harness — reproduces the thesis claim that an MD5 digest of the
 * ORDERED match stream agrees between the sequential baseline and every
 * parallel searcher, up to 64 threads.
 *
 * The unit test (tests/test_correctness.c) SORTS both lists before
 * comparing, so it only proves multiset (set-of-matches) equality. This
 * harness instead computes TWO digests per searcher run:
 *
 *   RAW    = MD5 over the matches in the exact order each searcher
 *            appended them to its ac_match_list_t (emission order).
 *   SORTED = MD5 over the matches after ac_match_list_sort
 *            (canonical (end_pos, pattern_id) order = multiset identity).
 *
 * Comparing RAW against the sequential baseline's RAW tells us whether
 * the digest certifies emission ORDER. Comparing SORTED tells us whether
 * it certifies the multiset. The thesis claim is specifically about the
 * ORDERED stream, so RAW is the load-bearing comparison.
 *
 * MD5 is implemented inline below (public-domain, RFC 1321 reference
 * style) so the harness has no external dependency. */

#include "ac_automaton.h"
#include "ac_match.h"
#include "ac_searcher.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ===================================================================== *
 *  MD5 (public domain, RFC 1321 reference implementation, compacted)    *
 * ===================================================================== */

typedef struct {
    uint32_t a, b, c, d;
    uint64_t len;          /* total message length in bytes */
    uint8_t  buf[64];
    size_t   buflen;
} md5_ctx;

static uint32_t md5_rotl(uint32_t x, uint32_t c)
{
    return (x << c) | (x >> (32 - c));
}

static void md5_block(md5_ctx *ctx, const uint8_t *p)
{
    static const uint32_t K[64] = {
        0xd76aa478,0xe8c7b756,0x242070db,0xc1bdceee,0xf57c0faf,0x4787c62a,
        0xa8304613,0xfd469501,0x698098d8,0x8b44f7af,0xffff5bb1,0x895cd7be,
        0x6b901122,0xfd987193,0xa679438e,0x49b40821,0xf61e2562,0xc040b340,
        0x265e5a51,0xe9b6c7aa,0xd62f105d,0x02441453,0xd8a1e681,0xe7d3fbc8,
        0x21e1cde6,0xc33707d6,0xf4d50d87,0x455a14ed,0xa9e3e905,0xfcefa3f8,
        0x676f02d9,0x8d2a4c8a,0xfffa3942,0x8771f681,0x6d9d6122,0xfde5380c,
        0xa4beea44,0x4bdecfa9,0xf6bb4b60,0xbebfbc70,0x289b7ec6,0xeaa127fa,
        0xd4ef3085,0x04881d05,0xd9d4d039,0xe6db99e5,0x1fa27cf8,0xc4ac5665,
        0xf4292244,0x432aff97,0xab9423a7,0xfc93a039,0x655b59c3,0x8f0ccc92,
        0xffeff47d,0x85845dd1,0x6fa87e4f,0xfe2ce6e0,0xa3014314,0x4e0811a1,
        0xf7537e82,0xbd3af235,0x2ad7d2bb,0xeb86d391
    };
    static const uint32_t S[64] = {
        7,12,17,22, 7,12,17,22, 7,12,17,22, 7,12,17,22,
        5, 9,14,20, 5, 9,14,20, 5, 9,14,20, 5, 9,14,20,
        4,11,16,23, 4,11,16,23, 4,11,16,23, 4,11,16,23,
        6,10,15,21, 6,10,15,21, 6,10,15,21, 6,10,15,21
    };

    uint32_t M[16];
    for (int i = 0; i < 16; i++) {
        M[i] =  (uint32_t)p[i*4]
             | ((uint32_t)p[i*4+1] << 8)
             | ((uint32_t)p[i*4+2] << 16)
             | ((uint32_t)p[i*4+3] << 24);
    }

    uint32_t a = ctx->a, b = ctx->b, c = ctx->c, d = ctx->d;
    for (uint32_t i = 0; i < 64; i++) {
        uint32_t f, g;
        if (i < 16)      { f = (b & c) | (~b & d);        g = i; }
        else if (i < 32) { f = (d & b) | (~d & c);        g = (5*i + 1) & 15; }
        else if (i < 48) { f = b ^ c ^ d;                 g = (3*i + 5) & 15; }
        else             { f = c ^ (b | ~d);              g = (7*i) & 15; }
        uint32_t tmp = d;
        d = c;
        c = b;
        b = b + md5_rotl(a + f + K[i] + M[g], S[i]);
        a = tmp;
    }
    ctx->a += a; ctx->b += b; ctx->c += c; ctx->d += d;
}

static void md5_init(md5_ctx *ctx)
{
    ctx->a = 0x67452301; ctx->b = 0xefcdab89;
    ctx->c = 0x98badcfe; ctx->d = 0x10325476;
    ctx->len = 0; ctx->buflen = 0;
}

static void md5_update(md5_ctx *ctx, const void *data, size_t n)
{
    const uint8_t *p = (const uint8_t *)data;
    ctx->len += n;
    while (n > 0) {
        size_t want = 64 - ctx->buflen;
        size_t take = n < want ? n : want;
        memcpy(ctx->buf + ctx->buflen, p, take);
        ctx->buflen += take;
        p += take;
        n -= take;
        if (ctx->buflen == 64) {
            md5_block(ctx, ctx->buf);
            ctx->buflen = 0;
        }
    }
}

static void md5_final(md5_ctx *ctx, uint8_t out[16])
{
    uint64_t bitlen = ctx->len * 8;
    uint8_t pad = 0x80;
    md5_update(ctx, &pad, 1);
    uint8_t zero = 0x00;
    while (ctx->buflen != 56) md5_update(ctx, &zero, 1);
    uint8_t lenbytes[8];
    for (int i = 0; i < 8; i++) lenbytes[i] = (uint8_t)(bitlen >> (8*i));
    md5_update(ctx, lenbytes, 8);
    /* buflen is now 0; output a,b,c,d little-endian */
    uint32_t v[4] = { ctx->a, ctx->b, ctx->c, ctx->d };
    for (int i = 0; i < 4; i++) {
        out[i*4]   = (uint8_t)(v[i]);
        out[i*4+1] = (uint8_t)(v[i] >> 8);
        out[i*4+2] = (uint8_t)(v[i] >> 16);
        out[i*4+3] = (uint8_t)(v[i] >> 24);
    }
}

static void md5_hex(const uint8_t d[16], char out[33])
{
    static const char *h = "0123456789abcdef";
    for (int i = 0; i < 16; i++) {
        out[i*2]   = h[d[i] >> 4];
        out[i*2+1] = h[d[i] & 15];
    }
    out[32] = 0;
}

/* ===================================================================== *
 *  Match-stream digest                                                  *
 * ===================================================================== */

/* Serialize each match deterministically: 8-byte little-endian end_pos
 * followed by 4-byte little-endian pattern_id, then MD5 the buffer.
 * Streamed match-by-match so the corpus size never matters. */
static void digest_list(const ac_match_list_t *l, char hex[33])
{
    md5_ctx ctx;
    md5_init(&ctx);
    for (size_t i = 0; i < l->count; i++) {
        uint8_t rec[12];
        uint64_t e = (uint64_t)l->data[i].end_pos;
        uint32_t p = (uint32_t)l->data[i].pattern_id;
        for (int k = 0; k < 8; k++) rec[k]   = (uint8_t)(e >> (8*k));
        for (int k = 0; k < 4; k++) rec[8+k] = (uint8_t)(p >> (8*k));
        md5_update(&ctx, rec, sizeof rec);
    }
    uint8_t d[16];
    md5_final(&ctx, d);
    md5_hex(d, hex);
}

/* ===================================================================== *
 *  Workload loading                                                     *
 * ===================================================================== */

static int load_corpus(const char *path, char **out, size_t *out_len, size_t cap)
{
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); return -1; }
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return -1; }
    long sz = ftell(f);
    rewind(f);
    if (sz < 0) { fclose(f); return -1; }
    size_t want = (size_t)sz;
    if (cap > 0 && want > cap) want = cap;
    char *buf = malloc(want ? want : 1);
    if (!buf) { fclose(f); return -1; }
    size_t got = fread(buf, 1, want, f);
    fclose(f);
    *out = buf;
    *out_len = got;
    return 0;
}

int main(int argc, char **argv)
{
    /* Defaults: real IDS workload — Snort dictionary + Enron slice. */
    const char *pat_path  = "data/patterns_snort.txt";
    const char *corp_path = "data/enron_corpus.txt";
    size_t corpus_cap = (size_t)256 << 20;   /* 256 MiB slice */

    if (argc > 1) pat_path  = argv[1];
    if (argc > 2) corp_path = argv[2];
    if (argc > 3) corpus_cap = strtoull(argv[3], NULL, 10);

    /* ---- patterns ---- */
    char  **pats = NULL;
    size_t *plens = NULL, npats = 0;
    int rc = ac_patterns_load_file(pat_path, &pats, &plens, &npats);
    if (rc != AC_OK || npats == 0) {
        fprintf(stderr, "failed to load patterns from %s (rc=%d, n=%zu)\n",
                pat_path, rc, npats);
        return 1;
    }

    /* ---- corpus ---- */
    char  *text = NULL;
    size_t text_len = 0;
    if (load_corpus(corp_path, &text, &text_len, corpus_cap) != 0) {
        fprintf(stderr, "failed to load corpus from %s\n", corp_path);
        return 1;
    }

    /* ---- automaton ---- */
    ac_automaton_t aut;
    if (ac_automaton_build(&aut, (const char *const *)pats, plens, npats) != AC_OK) {
        fprintf(stderr, "automaton build failed\n");
        return 1;
    }

    printf("# Digest harness\n");
    printf("# patterns:  %zu  (max len %d) from %s\n",
           npats, aut.max_pattern_len, pat_path);
    printf("# corpus:    %zu bytes (%.2f MiB) from %s\n",
           text_len, text_len / (double)(1u << 20), corp_path);
    printf("# automaton: %d states\n", aut.num_states);

    /* ---- sequential baseline ---- */
    const ac_searcher_t *seq = ac_searcher_find("sequential");
    if (!seq) { fprintf(stderr, "no sequential searcher\n"); return 1; }

    ac_searcher_config_t cfg = (ac_searcher_config_t){0};
    ac_match_list_t base;
    ac_match_list_init(&base);
    if (seq->search(&aut, text, text_len, &cfg, &base, NULL, NULL) != AC_OK) {
        fprintf(stderr, "sequential search failed\n");
        return 1;
    }
    char base_raw[33], base_sorted[33];
    digest_list(&base, base_raw);          /* emission order */
    /* copy then sort for the canonical digest (keep `base` in emission
     * order so the RAW digest above remains the reference). */
    {
        ac_match_list_t tmp;
        ac_match_list_init(&tmp);
        ac_match_list_reserve(&tmp, base.count);
        for (size_t i = 0; i < base.count; i++) ac_match_list_push(&tmp, base.data[i]);
        ac_match_list_sort(&tmp);
        digest_list(&tmp, base_sorted);
        ac_match_list_free(&tmp);
    }

    printf("\n# sequential baseline: %zu matches\n", base.count);
    printf("#   RAW    digest = %s\n", base_raw);
    printf("#   SORTED digest = %s\n\n", base_sorted);

    int thread_counts[] = {1, 2, 4, 8, 12, 16, 32, 64};
    size_t ntc = sizeof(thread_counts)/sizeof(thread_counts[0]);

    printf("%-26s %5s  %-10s  %-10s  %12s\n",
           "searcher", "T", "RAW", "SORTED", "matches");
    printf("--------------------------------------------------------------------------\n");

    int raw_all_agree = 1;          /* RAW agrees for EVERY searcher x T */
    int sorted_all_agree = 1;       /* SORTED agrees (real bug if not)   */
    int raw_fail_count = 0, sorted_fail_count = 0;

    for (size_t i = 0; i < ac_searcher_count(); i++) {
        const ac_searcher_t *s = ac_searcher_at(i);
        if (s == seq) continue;
        for (size_t j = 0; j < ntc; j++) {
            ac_searcher_config_t pc = cfg;
            pc.num_threads = thread_counts[j];

            ac_match_list_t got;
            ac_match_list_init(&got);
            int r = s->search(&aut, text, text_len, &pc, &got, NULL, NULL);
            if (r != AC_OK) {
                printf("%-26s %5d  %-10s  %-10s  (search rc=%d)\n",
                       s->name, thread_counts[j], "ERR", "ERR", r);
                ac_match_list_free(&got);
                raw_all_agree = sorted_all_agree = 0;
                continue;
            }

            char raw[33], sorted[33];
            digest_list(&got, raw);                 /* emission order */
            ac_match_list_sort(&got);
            digest_list(&got, sorted);              /* canonical order */

            int raw_ok    = (strcmp(raw, base_raw) == 0);
            int sorted_ok = (strcmp(sorted, base_sorted) == 0);
            if (!raw_ok)    { raw_all_agree = 0; raw_fail_count++; }
            if (!sorted_ok) { sorted_all_agree = 0; sorted_fail_count++; }

            printf("%-26s %5d  %-10s  %-10s  %12zu\n",
                   s->name, thread_counts[j],
                   raw_ok ? "AGREE" : "MISMATCH",
                   sorted_ok ? "AGREE" : "MISMATCH",
                   got.count);

            ac_match_list_free(&got);
        }
    }

    printf("--------------------------------------------------------------------------\n");
    printf("\nVERDICT\n");
    printf("  RAW (emission-order) digest agrees for every searcher x T : %s",
           raw_all_agree ? "YES\n" : "NO\n");
    if (!raw_all_agree)
        printf("    -> %d (searcher,T) combos diverged on RAW order.\n", raw_fail_count);
    printf("  SORTED (multiset) digest agrees for every searcher x T    : %s",
           sorted_all_agree ? "YES\n" : "NO\n");
    if (!sorted_all_agree)
        printf("    -> %d (searcher,T) combos diverged on SORTED -- REAL CORRECTNESS BUG.\n",
               sorted_fail_count);

    ac_match_list_free(&base);
    ac_automaton_destroy(&aut);
    free(text);
    ac_patterns_free(pats, plens, npats);
    return sorted_all_agree ? 0 : 1;
}
