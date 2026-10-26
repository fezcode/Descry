#ifndef DESCRY_KNIT_H
#define DESCRY_KNIT_H

#include <SDL.h>
#include <stdbool.h>

/* The Knit theme's material layer, ported from fezcodex's knit engine
 * (src/components/knit/engine.js). Every knitted surface is a grid of
 * stockinette stitches: each stitch two twisted plies forming a V, drawn
 * bottom-up so every V's tip covers the shoulders of the stitch below.
 *
 * Stitches are rasterized in software at device resolution (supersampled
 * plies with gradient, ply grooves, sheen and a soft cast shadow), then
 * uploaded once as SDL textures and cached: seamless wool tiles per yarn,
 * horizontally seamless colourwork bands, and charted text (the 5x7 / 3x5
 * stitch fonts). Drawing is texture copies only. */

typedef enum {
    YARN_WALNUT,   /* page ground: deep Fair Isle navy */
    YARN_BARK,     /* a shade down: rows, buttons */
    YARN_PEAT,     /* darkest: sidebar, title bar */
    YARN_CREAM,
    YARN_OAT,
    YARN_GOLD,
    YARN_POPPY,
    YARN_SNOW,
    YARN_PEACH,
    YARN_RUST,
    YARN_WHEAT,
    YARN_MOSS,
    YARN_LEMON,
    YARN_INK,
    YARN_TEAL,
    YARN_JET,
    YARN_DENIM,
    YARN_PLUM,
    YARN_COUNT
} KnitYarn;

typedef enum {
    KNIT_BAND_PEERIE,      /* gold rows with a poppy peerie dot on bark */
    KNIT_BAND_ZIGZAG,      /* gold zigzag on peat (the sidebar's edge) */
    KNIT_BAND_HENS,        /* the hens: a row of knitted chickens */
    KNIT_BAND_COUNT
} KnitBand;

typedef enum {
    KNIT_FONT_SMALL,       /* 3x5 font, FINE gauge: labels, nav, buttons */
    KNIT_FONT_TIGHT,       /* 3x5 font, tighter gauge: dense lists */
    KNIT_FONT_BIG,         /* 5x7 font, FINE gauge: titles */
    KNIT_FONT_COUNT
} KnitFont;

typedef enum {
    KNIT_ICON_FILE,
    KNIT_ICON_FOLDER,
    KNIT_ICON_FOLDER_OPEN,
    KNIT_ICON_CARET_RIGHT,
    KNIT_ICON_CARET_DOWN,
    KNIT_ICON_COUNT
} KnitIcon;

typedef struct Knit Knit;

Knit*     knit_create(SDL_Renderer* r);
void      knit_destroy(Knit* k);
/* Device pixels per logical point. Changing it drops every cached texture. */
void      knit_set_scale(Knit* k, float scale);

SDL_Color knit_yarn(KnitYarn y);

/* Plain stockinette in one yarn, tiled over r (anchored to the window
 * origin so neighbouring fills of the same yarn line up). Respects and
 * restores the renderer's current clip rect. */
void      knit_fill(Knit* k, SDL_Rect r, KnitYarn y);

/* A colourwork band across r.x .. r.x + r.w, its top at r.y. Returns the
 * band's height in logical pixels (r.h is ignored). */
int       knit_band(Knit* k, SDL_Rect r, KnitBand b);
int       knit_band_h(const Knit* k, KnitBand b);

/* A one-stitch selvedge around the window (0,0)-(w,h) in yarn, with a
 * poppy peerie dot every sixth stitch. */
void      knit_frame(Knit* k, int w, int h, KnitYarn yarn);

/* Knitted text. Capitals only: lower case is raised, accents are dropped
 * and a few typographic marks are mapped. Returns false when the text holds
 * something the stitch font cannot chart (CJK, emoji, ...) so the caller
 * can fall back to its regular font. */
bool      knit_text_ok(const char* s, size_t len);
int       knit_text_w(const Knit* k, const char* s, size_t len, KnitFont f);
int       knit_text_h(const Knit* k, KnitFont f);
/* Draws s with its top-left at (x, y), elided with "..." to max_w (<= 0 for
 * no limit). Returns the width drawn, or -1 if the text can't be knitted. */
int       knit_text(Knit* k, const char* s, size_t len, KnitFont f,
                    int x, int y, KnitYarn fg, int max_w);

int       knit_icon_w(const Knit* k, KnitIcon i);
int       knit_icon_h(const Knit* k, KnitIcon i);
void      knit_icon(Knit* k, KnitIcon i, int x, int y, KnitYarn fg);

/* A cream felt patch: cast shadow, speckled felt, and a dashed red running
 * stitch inset from the edge. */
void      knit_felt(Knit* k, SDL_Rect r);
/* The running stitch alone (for cards that are already filled). */
void      knit_running_stitch(Knit* k, SDL_Rect r, int inset, SDL_Color c);

#endif
