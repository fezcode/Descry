#include "decor.h"

#include <stdlib.h>
#include <string.h>

void decor_init(DecorSet* d) { memset(d, 0, sizeof *d); d->last_hit = -1; }

void decor_free(DecorSet* d)
{
    if (!d) return;
    free(d->items);
    memset(d, 0, sizeof *d);
    d->last_hit = -1;
}

void decor_clear(DecorSet* d)
{
    d->count = 0;
    d->sorted = true;        /* empty is trivially sorted */
    d->last_hit = -1;
}

void decor_add(DecorSet* d, size_t start, size_t end,
               int has_fg, SDL_Color fg,
               int has_bg, SDL_Color bg,
               int has_ul, SDL_Color ul)
{
    if (end <= start) return;
    if (d->count >= d->cap) {
        d->cap = d->cap ? d->cap * 2 : 64;
        d->items = realloc(d->items, d->cap * sizeof *d->items);
    }
    Decoration* it = &d->items[d->count++];
    it->start = start; it->end = end;
    it->has_fg = has_fg != 0; it->fg = fg;
    it->has_bg = has_bg != 0; it->bg = bg;
    it->has_ul = has_ul != 0; it->ul = ul;
    d->sorted = false;
    d->last_hit = -1;
}

static int cmp_decor(const void* a, const void* b)
{
    const Decoration* x = a;
    const Decoration* y = b;
    if (x->start < y->start) return -1;
    if (x->start > y->start) return  1;
    return 0;
}

const Decoration* decor_at(DecorSet* d, size_t off)
{
    if (d->count == 0) return NULL;
    if (!d->sorted) {
        qsort(d->items, d->count, sizeof *d->items, cmp_decor);
        d->sorted = true;
        d->last_hit = -1;
    }
    /* Cache: the render walk scans byte offsets roughly in order, so the
     * previous hit usually still covers (or immediately precedes) `off`. */
    if (d->last_hit >= 0 && d->last_hit < d->count) {
        const Decoration* c = &d->items[d->last_hit];
        if (off >= c->start && off < c->end) return c;
    }
    /* Binary search for the greatest start <= off. */
    int lo = 0, hi = d->count - 1, cand = -1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (d->items[mid].start <= off) { cand = mid; lo = mid + 1; }
        else                            { hi = mid - 1; }
    }
    if (cand >= 0 && off < d->items[cand].end) {
        d->last_hit = cand;
        return &d->items[cand];
    }
    return NULL;
}
