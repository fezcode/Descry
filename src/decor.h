#ifndef DESCRY_DECOR_H
#define DESCRY_DECOR_H

#include <SDL.h>
#include <stdbool.h>
#include <stddef.h>

/* Plugin-driven text decorations: a styled byte range over the SOURCE buffer
 * (the same coordinate space as descry.buffer.* — offsets include any
 * frontmatter). The editor applies them directly; the preview maps them
 * through MdDoc.src_map. Plugins push these in on('text_change')/on('open')
 * handlers, so the render path never calls Lua. */
typedef struct {
    size_t    start, end;     /* [start, end) buffer byte offsets */
    bool      has_fg; SDL_Color fg;
    bool      has_bg; SDL_Color bg;
    bool      has_ul; SDL_Color ul;   /* underline */
} Decoration;

typedef struct {
    Decoration* items;
    int         count, cap;
    bool        sorted;       /* items sorted by start; lazy on first lookup */
    int         last_hit;     /* cache: index returned by the previous decor_at */
} DecorSet;

void decor_init(DecorSet* d);
void decor_free(DecorSet* d);
void decor_clear(DecorSet* d);
void decor_add(DecorSet* d, size_t start, size_t end,
               int has_fg, SDL_Color fg,
               int has_bg, SDL_Color bg,
               int has_ul, SDL_Color ul);

/* The decoration covering byte `off`, or NULL. Decorations are assumed
 * non-overlapping; if they overlap, the one with the greatest start <= off
 * wins. Sorts lazily and caches the last hit, so sequential scans are cheap. */
const Decoration* decor_at(DecorSet* d, size_t off);

#endif
