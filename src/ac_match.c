#include "ac_match.h"

#include <stdlib.h>
#include <string.h>

void ac_match_list_init(ac_match_list_t *l)
{
    l->data = NULL;
    l->count = 0;
    l->capacity = 0;
}

void ac_match_list_free(ac_match_list_t *l)
{
    free(l->data);
    l->data = NULL;
    l->count = 0;
    l->capacity = 0;
}

int ac_match_list_reserve(ac_match_list_t *l, size_t cap)
{
    if (cap <= l->capacity) return AC_OK;
    size_t new_cap = l->capacity ? l->capacity : 64;
    while (new_cap < cap) new_cap *= 2;
    ac_match_t *p = realloc(l->data, new_cap * sizeof(*p));
    if (!p) return AC_E_NOMEM;
    l->data = p;
    l->capacity = new_cap;
    return AC_OK;
}

int ac_match_list_push(ac_match_list_t *l, ac_match_t m)
{
    if (AC_UNLIKELY(l->count == l->capacity)) {
        int rc = ac_match_list_reserve(l, l->capacity ? l->capacity * 2 : 64);
        if (rc != AC_OK) return rc;
    }
    l->data[l->count++] = m;
    return AC_OK;
}

int ac_match_list_extend_consume(ac_match_list_t *dst, ac_match_list_t *src)
{
    if (src->count == 0) return AC_OK;
    int rc = ac_match_list_reserve(dst, dst->count + src->count);
    if (rc != AC_OK) return rc;
    memcpy(dst->data + dst->count, src->data, src->count * sizeof(ac_match_t));
    dst->count += src->count;
    src->count = 0;
    return AC_OK;
}

static int match_cmp(const void *a, const void *b)
{
    const ac_match_t *x = a, *y = b;
    if (x->end_pos != y->end_pos) return x->end_pos < y->end_pos ? -1 : 1;
    if (x->pattern_id != y->pattern_id) return x->pattern_id - y->pattern_id;
    return 0;
}

void ac_match_list_sort(ac_match_list_t *l)
{
    qsort(l->data, l->count, sizeof(ac_match_t), match_cmp);
}
