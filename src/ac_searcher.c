#include "ac_searcher.h"

#include <stdio.h>
#include <string.h>

#define AC_MAX_SEARCHERS 32

static const ac_searcher_t *g_searchers[AC_MAX_SEARCHERS];
static size_t                g_count = 0;

int ac_searcher_register(const ac_searcher_t *s)
{
    if (!s || !s->name || !s->search) return AC_E_INVAL;
    if (g_count >= AC_MAX_SEARCHERS) return AC_E_NOMEM;
    /* Reject duplicates by name -- helps catch accidental double-registration. */
    for (size_t i = 0; i < g_count; i++) {
        if (strcmp(g_searchers[i]->name, s->name) == 0) return AC_E_INVAL;
    }
    g_searchers[g_count++] = s;
    return AC_OK;
}

const ac_searcher_t *ac_searcher_find(const char *name)
{
    if (!name) return NULL;
    for (size_t i = 0; i < g_count; i++) {
        if (strcmp(g_searchers[i]->name, name) == 0) return g_searchers[i];
    }
    return NULL;
}

size_t ac_searcher_count(void) { return g_count; }

const ac_searcher_t *ac_searcher_at(size_t i)
{
    return i < g_count ? g_searchers[i] : NULL;
}
