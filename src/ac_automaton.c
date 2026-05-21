#include "ac_automaton.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- Construction-time scaffolding -------------------------------------
 * During the build phase we keep a "raw" trie alongside the deterministic
 * transition table. The raw trie tells us, for every state, which children
 * are real (i.e. a pattern character takes us there) versus which are
 * synthesised failure transitions. We need this distinction to compute
 * fail/dict_suffix links correctly. After construction these scaffold
 * tables are freed and only the read-only structures survive. */

typedef struct {
    int32_t *raw_child;     /* num_states * 256, AC_NIL or child state */
    int32_t *fail;          /* num_states                              */
    int32_t  capacity;
} ac_build_t;

static int build_grow(ac_automaton_t *aut, ac_build_t *b, int32_t need)
{
    if (need <= b->capacity) return AC_OK;
    int32_t new_cap = b->capacity ? b->capacity * 2 : 64;
    while (new_cap < need) new_cap *= 2;

    int32_t *raw = realloc(b->raw_child, (size_t)new_cap * AC_ALPHABET_SIZE * sizeof(int32_t));
    if (!raw) return AC_E_NOMEM;
    b->raw_child = raw;

    int32_t *go = realloc(aut->goto_tbl, (size_t)new_cap * AC_ALPHABET_SIZE * sizeof(int32_t));
    if (!go) return AC_E_NOMEM;
    aut->goto_tbl = go;

    int32_t *fail = realloc(b->fail, (size_t)new_cap * sizeof(int32_t));
    if (!fail) return AC_E_NOMEM;
    b->fail = fail;

    int32_t *own = realloc(aut->own_out_head, (size_t)new_cap * sizeof(int32_t));
    if (!own) return AC_E_NOMEM;
    aut->own_out_head = own;

    int32_t *ds = realloc(aut->dict_suffix, (size_t)new_cap * sizeof(int32_t));
    if (!ds) return AC_E_NOMEM;
    aut->dict_suffix = ds;

    /* Initialise newly-allocated states */
    for (int32_t s = b->capacity; s < new_cap; s++) {
        for (int c = 0; c < AC_ALPHABET_SIZE; c++) {
            b->raw_child[(size_t)s * AC_ALPHABET_SIZE + c] = AC_NIL;
        }
        b->fail[s] = AC_NIL;
        aut->own_out_head[s] = AC_NIL;
        aut->dict_suffix[s]  = AC_NIL;
    }
    b->capacity = new_cap;
    return AC_OK;
}

static int alloc_state(ac_automaton_t *aut, ac_build_t *b)
{
    int32_t s = aut->num_states;
    int rc = build_grow(aut, b, s + 1);
    if (rc != AC_OK) return rc;
    aut->num_states = s + 1;
    return s;
}

static int push_output(ac_automaton_t *aut, int32_t *cap, int32_t state, int32_t pattern_id)
{
    if (aut->num_outputs == *cap) {
        int32_t new_cap = *cap ? *cap * 2 : 64;
        ac_output_entry_t *p = realloc(aut->outputs, (size_t)new_cap * sizeof(*p));
        if (!p) return AC_E_NOMEM;
        aut->outputs = p;
        *cap = new_cap;
    }
    aut->outputs[aut->num_outputs].pattern_id = pattern_id;
    aut->outputs[aut->num_outputs].next = aut->own_out_head[state];
    aut->own_out_head[state] = aut->num_outputs++;
    return AC_OK;
}

int ac_automaton_build(ac_automaton_t *aut,
                       const char *const *patterns,
                       const size_t *lengths,
                       size_t num_patterns)
{
    if (!aut || (num_patterns > 0 && (!patterns || !lengths))) return AC_E_INVAL;

    memset(aut, 0, sizeof(*aut));
    aut->min_pattern_len = 0;

    ac_build_t b = (ac_build_t){0};
    int rc;

    /* State 0 = root */
    int s0 = alloc_state(aut, &b);
    if (s0 < 0) { rc = s0; goto fail; }
    assert(s0 == 0);

    /* Copy patterns and insert into trie. */
    if (num_patterns > 0) {
        aut->patterns = calloc(num_patterns, sizeof(ac_pattern_t));
        if (!aut->patterns) { rc = AC_E_NOMEM; goto fail; }
    }
    aut->num_patterns = (int32_t)num_patterns;

    int32_t out_cap = 0;
    int32_t max_len = 0;
    int32_t min_len = 0;

    for (size_t i = 0; i < num_patterns; i++) {
        size_t L = lengths[i];
        if (L == 0) { rc = AC_E_PATTERN_EMPTY; goto fail; }
        char *buf = malloc(L);
        if (!buf) { rc = AC_E_NOMEM; goto fail; }
        memcpy(buf, patterns[i], L);
        aut->patterns[i].text = buf;
        aut->patterns[i].length = (int32_t)L;
        if ((int32_t)L > max_len) max_len = (int32_t)L;
        if (i == 0 || (int32_t)L < min_len) min_len = (int32_t)L;

        int32_t state = 0;
        for (size_t j = 0; j < L; j++) {
            uint8_t c = (uint8_t)buf[j];
            int32_t *slot = &b.raw_child[(size_t)state * AC_ALPHABET_SIZE + c];
            if (*slot == AC_NIL) {
                int ns = alloc_state(aut, &b);
                if (ns < 0) { rc = ns; goto fail; }
                /* alloc_state may have reallocated -- refresh slot pointer */
                slot = &b.raw_child[(size_t)state * AC_ALPHABET_SIZE + c];
                *slot = ns;
            }
            state = *slot;
        }
        rc = push_output(aut, &out_cap, state, (int32_t)i);
        if (rc != AC_OK) goto fail;
    }
    aut->max_pattern_len = max_len;
    aut->min_pattern_len = min_len;

    /* ---- BFS to compute failure links AND build delta in-place ---------
     * Strategy: process states in BFS order. For root (state 0) the delta
     * is its raw children; missing transitions self-loop to root. For any
     * subsequent state u with parent p reached via byte c, the delta on
     * any byte c' is either u's raw child (if present) or delta[fail[u]]
     * on c'. Because we process in BFS order, fail[u]'s delta row is
     * already finalised when we reach u. */
    int32_t *queue = malloc((size_t)aut->num_states * sizeof(int32_t));
    if (!queue) { rc = AC_E_NOMEM; goto fail; }
    int32_t qhead = 0, qtail = 0;

    /* Root row */
    for (int c = 0; c < AC_ALPHABET_SIZE; c++) {
        int32_t v = b.raw_child[c];
        if (v == AC_NIL) {
            aut->goto_tbl[c] = 0;            /* self-loop on root */
        } else {
            aut->goto_tbl[c] = v;
            b.fail[v] = 0;
            queue[qtail++] = v;
        }
    }

    while (qhead < qtail) {
        int32_t u = queue[qhead++];
        int32_t f = b.fail[u];

        /* Propagate dictionary suffix link: nearest ancestor in fail
         * chain (or itself) with own outputs. */
        if (aut->own_out_head[f] != AC_NIL) {
            aut->dict_suffix[u] = f;
        } else {
            aut->dict_suffix[u] = aut->dict_suffix[f];
        }

        /* Build delta row for u. */
        for (int c = 0; c < AC_ALPHABET_SIZE; c++) {
            int32_t v = b.raw_child[(size_t)u * AC_ALPHABET_SIZE + c];
            if (v == AC_NIL) {
                aut->goto_tbl[(size_t)u * AC_ALPHABET_SIZE + c] =
                    aut->goto_tbl[(size_t)f * AC_ALPHABET_SIZE + c];
            } else {
                aut->goto_tbl[(size_t)u * AC_ALPHABET_SIZE + c] = v;
                b.fail[v] = aut->goto_tbl[(size_t)f * AC_ALPHABET_SIZE + c];
                queue[qtail++] = v;
            }
        }
    }
    free(queue);

    /* Free build scaffolding -- post-build state is read-only. */
    free(b.raw_child);
    free(b.fail);

    /* Trim oversized arrays down to num_states (optional but tidy). */
    if (aut->num_states < b.capacity) {
        int32_t n = aut->num_states;
        int32_t *go = realloc(aut->goto_tbl,
                              (size_t)n * AC_ALPHABET_SIZE * sizeof(int32_t));
        if (go) aut->goto_tbl = go;
        int32_t *own = realloc(aut->own_out_head, (size_t)n * sizeof(int32_t));
        if (own) aut->own_out_head = own;
        int32_t *ds = realloc(aut->dict_suffix, (size_t)n * sizeof(int32_t));
        if (ds) aut->dict_suffix = ds;
    }
    if (aut->num_outputs > 0) {
        ac_output_entry_t *p = realloc(aut->outputs,
                                       (size_t)aut->num_outputs * sizeof(*p));
        if (p) aut->outputs = p;
    }

    /* ---- Idea 5: build flat output table ------------------------------
     * For every state s, materialise a contiguous run of pattern_ids
     * equivalent to the (own_out_head, dict_suffix, outputs) chain
     * walk. Done in two passes: pass 1 counts per state (to compute
     * offsets and total size); pass 2 fills flat_pids. Ordering
     * matches the chain walk exactly (own first, then ancestors), so
     * chain-walking and flat-reading searchers produce match lists
     * that are bytewise identical under ac_match_list_sort.
     *
     * Cost is O(sum chain_length(s)) -- in practice ~num_outputs --
     * which is a single-digit percent of the BFS cost on the
     * dictionaries shipped under data/. Read-only after this pass,
     * so it satisfies the "automaton is immutable during search"
     * invariant the same way goto_tbl does. */
    {
        int32_t n = aut->num_states;
        aut->flat_offset = malloc((size_t)n * sizeof(int32_t));
        aut->flat_count  = malloc((size_t)n * sizeof(int32_t));
        if (!aut->flat_offset || !aut->flat_count) {
            rc = AC_E_NOMEM;
            goto fail_post;
        }

        int32_t total = 0;
        for (int32_t s = 0; s < n; s++) {
            int32_t l = (aut->own_out_head[s] != AC_NIL) ? s : aut->dict_suffix[s];
            int32_t c = 0;
            while (l != AC_NIL) {
                for (int32_t o = aut->own_out_head[l]; o != AC_NIL; o = aut->outputs[o].next) c++;
                l = aut->dict_suffix[l];
            }
            aut->flat_offset[s] = total;
            aut->flat_count[s]  = c;
            /* Defensive overflow guard: in pathological dictionaries
             * (every state is dict_suffix-reachable from every other)
             * the sum could theoretically exceed INT32_MAX. Bail out
             * rather than silently wrap. */
            if (c > 0 && total > INT32_MAX - c) {
                rc = AC_E_NOMEM;
                goto fail_post;
            }
            total += c;
        }

        if (total > 0) {
            aut->flat_pids = malloc((size_t)total * sizeof(int32_t));
            if (!aut->flat_pids) { rc = AC_E_NOMEM; goto fail_post; }
        }
        aut->total_flat_pids = total;

        for (int32_t s = 0; s < n; s++) {
            if (aut->flat_count[s] == 0) continue;
            int32_t l = (aut->own_out_head[s] != AC_NIL) ? s : aut->dict_suffix[s];
            int32_t pos = aut->flat_offset[s];
            while (l != AC_NIL) {
                for (int32_t o = aut->own_out_head[l]; o != AC_NIL; o = aut->outputs[o].next) {
                    aut->flat_pids[pos++] = aut->outputs[o].pattern_id;
                }
                l = aut->dict_suffix[l];
            }
        }
    }

    return AC_OK;

fail_post:
    /* Build succeeded up to the chain walk but the flat-output pass
     * could not allocate. Tear down the entire automaton (we cannot
     * partially expose a struct whose readers may assume the new
     * fields are valid). */
    ac_automaton_destroy(aut);
    return rc;

fail:
    free(b.raw_child);
    free(b.fail);
    ac_automaton_destroy(aut);
    return rc;
}

void ac_automaton_destroy(ac_automaton_t *aut)
{
    if (!aut) return;
    free(aut->goto_tbl);
    free(aut->own_out_head);
    free(aut->dict_suffix);
    free(aut->outputs);
    free(aut->flat_offset);
    free(aut->flat_count);
    free(aut->flat_pids);
    if (aut->patterns) {
        for (int32_t i = 0; i < aut->num_patterns; i++) free(aut->patterns[i].text);
        free(aut->patterns);
    }
    memset(aut, 0, sizeof(*aut));
}

size_t ac_automaton_memory_bytes(const ac_automaton_t *aut)
{
    size_t n = (size_t)aut->num_states;
    size_t bytes = 0;
    bytes += n * AC_ALPHABET_SIZE * sizeof(int32_t);  /* goto_tbl */
    bytes += n * sizeof(int32_t) * 2;                 /* own_out_head + dict_suffix */
    bytes += (size_t)aut->num_outputs * sizeof(ac_output_entry_t);
    bytes += n * sizeof(int32_t) * 2;                 /* flat_offset + flat_count */
    bytes += (size_t)aut->total_flat_pids * sizeof(int32_t);  /* flat_pids */
    for (int32_t i = 0; i < aut->num_patterns; i++) bytes += (size_t)aut->patterns[i].length;
    bytes += (size_t)aut->num_patterns * sizeof(ac_pattern_t);
    return bytes;
}

/* ---- Pattern file loader ---------------------------------------------- */

int ac_patterns_load_file(const char *path,
                          char ***out_patterns,
                          size_t **out_lengths,
                          size_t *out_count)
{
    FILE *f = fopen(path, "rb");
    if (!f) return AC_E_NOT_FOUND;

    char **arr = NULL;
    size_t *lens = NULL;
    size_t cap = 0, count = 0;
    char *line = NULL;
    size_t line_cap = 0;
    ssize_t n;
    int rc = AC_OK;

    while ((n = getline(&line, &line_cap, f)) != -1) {
        /* Strip trailing \r/\n */
        while (n > 0 && (line[n-1] == '\n' || line[n-1] == '\r')) n--;
        if (n == 0) continue;
        if (line[0] == '#') continue;

        if (count == cap) {
            size_t nc = cap ? cap * 2 : 16;
            char  **na = realloc(arr,  nc * sizeof(*na));
            size_t *nl = realloc(lens, nc * sizeof(*nl));
            if (!na || !nl) { free(na); free(nl); rc = AC_E_NOMEM; goto out; }
            arr = na; lens = nl; cap = nc;
        }
        char *buf = malloc((size_t)n);
        if (!buf) { rc = AC_E_NOMEM; goto out; }
        memcpy(buf, line, (size_t)n);
        arr[count]  = buf;
        lens[count] = (size_t)n;
        count++;
    }

    *out_patterns = arr;
    *out_lengths  = lens;
    *out_count    = count;
    arr = NULL; lens = NULL;

out:
    free(line);
    fclose(f);
    if (rc != AC_OK) {
        for (size_t i = 0; i < count; i++) free(arr ? arr[i] : NULL);
        free(arr); free(lens);
    }
    return rc;
}

void ac_patterns_free(char **patterns, size_t *lengths, size_t count)
{
    if (patterns) for (size_t i = 0; i < count; i++) free(patterns[i]);
    free(patterns);
    free(lengths);
}
