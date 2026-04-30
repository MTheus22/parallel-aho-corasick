#ifndef AC_MATCH_H
#define AC_MATCH_H

#include "ac_common.h"

/* A single match: pattern `pattern_id` matched in the text ending at
 * absolute byte position `end_pos` (inclusive). The starting position is
 * `end_pos - pattern_length + 1`. */
typedef struct {
    int64_t end_pos;
    int32_t pattern_id;
    int32_t _pad;
} ac_match_t;

/* Dynamic, append-only list of matches. Used both as the final result
 * container and as a thread-local list inside parallel searchers (so it
 * MUST contain no shared/locked state). */
typedef struct {
    ac_match_t *data;
    size_t      count;
    size_t      capacity;
} ac_match_list_t;

void   ac_match_list_init(ac_match_list_t *l);
void   ac_match_list_free(ac_match_list_t *l);
int    ac_match_list_reserve(ac_match_list_t *l, size_t cap);
int    ac_match_list_push(ac_match_list_t *l, ac_match_t m);

/* Append the contents of `src` to `dst` and reset `src` to empty (without
 * freeing). Used by parallel searchers to merge thread-local lists into a
 * unified result after pthread_join. */
int    ac_match_list_extend_consume(ac_match_list_t *dst, ac_match_list_t *src);

/* Sort by (end_pos, pattern_id). Useful for canonicalizing results when
 * comparing implementations against the sequential baseline. */
void   ac_match_list_sort(ac_match_list_t *l);

#endif /* AC_MATCH_H */
