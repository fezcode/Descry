#ifndef DESCRY_SCRIPTORIUM_H
#define DESCRY_SCRIPTORIUM_H

#include <SDL.h>
#include <stdbool.h>

/* The Scriptorium theme's material layer: an illuminated manuscript.
 * Borders are rows of cross stitches, chrome lettering is a pixel hand in
 * capitals and small capitals (scriptorium_core.c holds the charts), section
 * headers are rubrics, and a few marginal drawings fill empty corners.
 *
 * Everything is pixel art on a grid of whole device pixels, rasterized once
 * into SDL textures and cached; drawing is texture copies. */

typedef enum {
    SCR_INK,          /* sepia text */
    SCR_FADED,        /* faded ink: quiet labels */
    SCR_CINNABAR,     /* rubric red */
    SCR_GOLD,         /* ochre gold leaf */
    SCR_LAPIS,
    SCR_VERDIGRIS,
    SCR_ROSE,         /* the pale band behind a rubric */
    SCR_VELLUM,       /* a shade under the page */
    SCR_PARCHMENT,    /* the page */
    SCR_INK_COUNT
} ScrInk;

typedef enum {
    SCR_FONT_SMALL,   /* labels, rows, status */
    SCR_FONT_BIG,     /* titles: twice the size */
    SCR_FONT_COUNT
} ScrFont;

typedef enum {
    SCR_ORN_CANDLE,
    SCR_ORN_CANDLE_LIT,
    SCR_ORN_CHAPEL,
    SCR_ORN_SNAIL,
    SCR_ORN_PEDIMENT,
    SCR_ORN_COUNT
} ScrOrnament;

typedef struct Scriptorium Scriptorium;

Scriptorium* scr_create(SDL_Renderer* r);
void      scr_destroy(Scriptorium* s);
/* Device pixels per logical point. Changing it drops every cached texture. */
void      scr_set_scale(Scriptorium* s, float scale);

SDL_Color scr_ink(ScrInk i);

/* Size of one stitch cell in logical pixels (rounded up). */
int       scr_cell(const Scriptorium* s);
/* A run of stitches: as many whole stitches as fit, centred in the run. */
void      scr_stitch_row(Scriptorium* s, int x, int y, int w, ScrInk i);
void      scr_stitch_col(Scriptorium* s, int x, int y, int h, ScrInk i);
/* A stitched frame `depth` stitches deep, just inside r. */
void      scr_frame(Scriptorium* s, SDL_Rect r, int depth, ScrInk i);

/* Text in the hand. scr_text_ok is false when the text holds something the
 * hand cannot chart (CJK, emoji, ...) so the caller can use its own font. */
bool      scr_text_ok(const char* str, size_t len);
int       scr_text_w(const Scriptorium* s, const char* str, size_t len, ScrFont f);
int       scr_text_h(const Scriptorium* s, ScrFont f);
/* Draws str with its top-left at (x, y), the first letter in `initial`
 * (pass `ink` again for none), elided with "..." to max_w (<= 0: no limit).
 * Returns the width drawn, or -1 if the text can't be charted. */
int       scr_text(Scriptorium* s, const char* str, size_t len, ScrFont f,
                   int x, int y, ScrInk ink, ScrInk initial, int max_w);

/* A rubric: a rose band across r.x .. r.x + r.w with its top at r.y (r.h is
 * ignored), hemmed with cinnabar stitches, the text in outlined capitals.
 * The band is always drawn; returns false if the text could not be charted
 * (the caller then letters the band itself). */
int       scr_rubric_h(const Scriptorium* s);
bool      scr_rubric(Scriptorium* s, SDL_Rect r, const char* text);

/* An illuminated initial filling the square `tile`. */
void      scr_dropcap(Scriptorium* s, SDL_Rect tile, char letter);

int       scr_orn_w(const Scriptorium* s, ScrOrnament o);
int       scr_orn_h(const Scriptorium* s, ScrOrnament o);
void      scr_ornament(Scriptorium* s, ScrOrnament o, int x, int y);

#endif
