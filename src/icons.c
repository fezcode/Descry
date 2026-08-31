#include "icons.h"
#include "icon_raster.h"

#include "nanosvg.h"   /* parse API only; the implementation lives in icon_raster.c */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Lucide-style icons (ISC-licensed inspiration). 24x24 viewBox, 2px stroke,
 * round caps + joins, stroke='white' so a single texture serves any tint.
 *
 * Each icon is a main SVG plus an optional "cut" SVG: shapes in the cut
 * layer are knocked OUT of the main layer (alpha subtraction), which is how
 * the filled variants get transparent grooves — a slit between two panes,
 * list rules inside a solid card — without knowing the background colour. */
#define SVG24_OPEN(sw) \
    "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 24 24' fill='none' " \
    "stroke='white' stroke-width='" sw "' stroke-linecap='round' stroke-linejoin='round'>"
#define SVG24      SVG24_OPEN("2")
#define SVG24_BUTT \
    "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 24 24' fill='none' " \
    "stroke='white' stroke-width='2' stroke-linecap='butt' stroke-linejoin='round'>"
/* Window controls: 12-unit grid, 1-unit strokes on half-integer coordinates
 * so that at the 12 px they are drawn at every line is exactly one crisp
 * pixel — the same trick the OS caption glyphs use. */
#define SVG12_OPEN(sw, cap) \
    "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 12 12' fill='none' " \
    "stroke='white' stroke-width='" sw "' stroke-linecap='" cap "' stroke-linejoin='miter'>"
#define SVG_END "</svg>"

typedef struct {
    const char* svg;   /* main layer  */
    const char* cut;   /* knockout layer (may be NULL) */
} IconSrc;

static const IconSrc SRC[ICON_COUNT] = {
    /* SETTINGS — three sliders, each with a circular knob */
    [ICON_SETTINGS] = { SVG24
        "<line x1='21' y1='4' x2='14' y2='4'/>"
        "<line x1='10' y1='4' x2='3' y2='4'/>"
        "<line x1='21' y1='12' x2='12' y2='12'/>"
        "<line x1='8' y1='12' x2='3' y2='12'/>"
        "<line x1='21' y1='20' x2='16' y2='20'/>"
        "<line x1='12' y1='20' x2='3' y2='20'/>"
        "<circle cx='12' cy='4' r='2'/>"
        "<circle cx='10' cy='12' r='2'/>"
        "<circle cx='14' cy='20' r='2'/>"
        SVG_END, NULL },

    /* FIND — magnifying glass */
    [ICON_FIND] = { SVG24
        "<circle cx='11' cy='11' r='8'/>"
        "<line x1='21' y1='21' x2='16.65' y2='16.65'/>"
        SVG_END, NULL },

    /* SIDEBAR_OPEN — panel with the left third filled. The fill follows the
     * frame's rounded corners so it reads as one solid pane. */
    [ICON_SIDEBAR_OPEN] = { SVG24
        "<path d='M3 5a2 2 0 0 1 2-2h4v18H5a2 2 0 0 1-2-2z' fill='white' stroke='none'/>"
        "<rect x='3' y='3' width='18' height='18' rx='2'/>"
        "<line x1='9' y1='3' x2='9' y2='21'/>"
        SVG_END, NULL },

    /* SIDEBAR_CLOSED — panel outline only with divider */
    [ICON_SIDEBAR_CLOSED] = { SVG24
        "<rect x='3' y='3' width='18' height='18' rx='2'/>"
        "<line x1='9' y1='3' x2='9' y2='21'/>"
        SVG_END, NULL },

    /* OUTLINE — list with bullet dots */
    [ICON_OUTLINE] = { SVG24
        "<line x1='8' y1='6' x2='21' y2='6'/>"
        "<line x1='8' y1='12' x2='21' y2='12'/>"
        "<line x1='8' y1='18' x2='21' y2='18'/>"
        "<circle cx='4' cy='6'  r='1' fill='white' stroke='none'/>"
        "<circle cx='4' cy='12' r='1' fill='white' stroke='none'/>"
        "<circle cx='4' cy='18' r='1' fill='white' stroke='none'/>"
        SVG_END, NULL },

    /* FOLDER — closed folder shape */
    [ICON_FOLDER] = { SVG24
        "<path d='M22 19a2 2 0 0 1-2 2H4a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2h5l2 3h9a2 2 0 0 1 2 2z'/>"
        SVG_END, NULL },

    /* FOLDER_OPEN — folder with peeking-up lid */
    [ICON_FOLDER_OPEN] = { SVG24
        "<path d='m6 14 1.45-2.9A2 2 0 0 1 9.24 10H20a2 2 0 0 1 1.94 2.5l-1.55 6a2 2 0 0 1-1.94 1.5H4a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2h3.9a2 2 0 0 1 1.69.9l.81 1.2a2 2 0 0 0 1.67.9H18a2 2 0 0 1 2 2v2'/>"
        SVG_END, NULL },

    /* FILE — document with folded corner */
    [ICON_FILE] = { SVG24
        "<path d='M14 2H6a2 2 0 0 0-2 2v16a2 2 0 0 0 2 2h12a2 2 0 0 0 2-2V8z'/>"
        "<polyline points='14 2 14 8 20 8'/>"
        SVG_END, NULL },

    /* CARET_RIGHT / CARET_DOWN — chevrons */
    [ICON_CARET_RIGHT] = { SVG24_OPEN("2.5")
        "<polyline points='9 18 15 12 9 6'/>"
        SVG_END, NULL },
    [ICON_CARET_DOWN] = { SVG24_OPEN("2.5")
        "<polyline points='6 9 12 15 18 9'/>"
        SVG_END, NULL },

    /* Window controls (12-grid, see SVG12_OPEN). */
    [ICON_WIN_MIN] = { SVG12_OPEN("1", "butt")
        "<path d='M1 6.5H11'/>"
        SVG_END, NULL },
    [ICON_WIN_MAX] = { SVG12_OPEN("1", "butt")
        "<rect x='1.5' y='1.5' width='9' height='9'/>"
        SVG_END, NULL },
    [ICON_WIN_RESTORE] = { SVG12_OPEN("1", "butt")
        "<rect x='1.5' y='3.5' width='7' height='7'/>"
        "<path d='M3.5 3.5V1.5H10.5V8.5H8.5'/>"
        SVG_END, NULL },
    /* The X is diagonal, so it is anti-aliased whatever we do; a hair over
     * 1 unit keeps its weight optically matched to the crisp bars. */
    [ICON_WIN_CLOSE] = { SVG12_OPEN("1.2", "round")
        "<path d='M1.5 1.5L10.5 10.5M10.5 1.5L1.5 10.5'/>"
        SVG_END, NULL },

    /* CHEVRON_LEFT / RIGHT — settings adjusters. */
    [ICON_CHEVRON_LEFT] = { SVG24_OPEN("2.5")
        "<polyline points='15 18 9 12 15 6'/>"
        SVG_END, NULL },
    [ICON_CHEVRON_RIGHT] = { SVG24_OPEN("2.5")
        "<polyline points='9 18 15 12 9 6'/>"
        SVG_END, NULL },

    /* VAULT_SEARCH — text rules + magnifying lens over the bottom-right.
     * Reads as "find across documents". */
    [ICON_VAULT_SEARCH] = { SVG24
        "<path d='M21 6H3'/>"
        "<path d='M10 12H3'/>"
        "<path d='M10 18H3'/>"
        "<circle cx='17' cy='15' r='3'/>"
        "<path d='m21 19-1.9-1.9'/>"
        SVG_END, NULL },

    /* COMMAND — terminal prompt chevron + underscore. */
    [ICON_COMMAND] = { SVG24
        "<polyline points='4 17 10 11 4 5'/>"
        "<line x1='12' y1='19' x2='20' y2='19'/>"
        SVG_END, NULL },

    /* SPLIT — panel divided into two columns (live-preview split) */
    [ICON_SPLIT] = { SVG24
        "<rect x='3' y='3' width='18' height='18' rx='2'/>"
        "<line x1='12' y1='3' x2='12' y2='21'/>"
        SVG_END, NULL },

    /* PLUGIN — a plug: two prongs, a body, a cord. */
    [ICON_PLUGIN] = { SVG24
        "<line x1='9' y1='2' x2='9' y2='8'/>"
        "<line x1='15' y1='2' x2='15' y2='8'/>"
        "<path d='M5 8h14v4a7 7 0 0 1-14 0V8z'/>"
        "<line x1='12' y1='19' x2='12' y2='22'/>"
        SVG_END, NULL },

    /* ---- filled variants ------------------------------------------- */

    /* FILE_FILLED — solid page; the corner flap stays an outline so the
     * silhouette keeps its dog-ear. */
    [ICON_FILE_FILLED] = { SVG24
        "<path d='M14 2H6a2 2 0 0 0-2 2v16a2 2 0 0 0 2 2h12a2 2 0 0 0 2-2V8z' fill='white'/>"
        "<polyline points='14 2 14 8 20 8'/>"
        SVG_END, NULL },

    /* FOLDER_FILLED — solid folder silhouette. */
    [ICON_FOLDER_FILLED] = { SVG24
        "<path d='M22 19a2 2 0 0 1-2 2H4a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2h5l2 3h9a2 2 0 0 1 2 2z' fill='white'/>"
        SVG_END, NULL },

    /* FOLDER_OPEN_FILLED — solid open folder; a groove cut along the top
     * of the front flap separates it from the back panel. */
    [ICON_FOLDER_OPEN_FILLED] = { SVG24
        "<path d='M2 5a2 2 0 0 1 2-2h3.9a2 2 0 0 1 1.69.9l.81 1.2a2 2 0 0 0 1.67.9H18a2 2 0 0 1 2 2v2a2 2 0 0 1 1.94 2.5l-1.55 6a2 2 0 0 1-1.94 1.5H4a2 2 0 0 1-2-2z' fill='white' stroke='none'/>"
        "<path d='m6 14 1.45-2.9A2 2 0 0 1 9.24 10H20a2 2 0 0 1 1.94 2.5l-1.55 6a2 2 0 0 1-1.94 1.5H4a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2h3.9a2 2 0 0 1 1.69.9l.81 1.2a2 2 0 0 0 1.67.9H18a2 2 0 0 1 2 2v2'/>"
        SVG_END,
        SVG24_BUTT
        "<path d='m6 14 1.45-2.9A2 2 0 0 1 9.24 10H19'/>"
        SVG_END },

    /* OUTLINE_FILLED — solid card with the list rules and bullets knocked
     * out (the Fluent "filled" treatment for list glyphs). */
    [ICON_OUTLINE_FILLED] = { SVG24
        "<rect x='3' y='3' width='18' height='18' rx='2' fill='white'/>"
        SVG_END,
        SVG24
        "<line x1='10.5' y1='8'  x2='17' y2='8'/>"
        "<line x1='10.5' y1='12' x2='17' y2='12'/>"
        "<line x1='10.5' y1='16' x2='17' y2='16'/>"
        "<circle cx='7' cy='8'  r='1.25' fill='white' stroke='none'/>"
        "<circle cx='7' cy='12' r='1.25' fill='white' stroke='none'/>"
        "<circle cx='7' cy='16' r='1.25' fill='white' stroke='none'/>"
        SVG_END },

    /* SPLIT_FILLED — two solid panes with a slit between them. */
    [ICON_SPLIT_FILLED] = { SVG24
        "<rect x='3' y='3' width='18' height='18' rx='2' fill='white'/>"
        SVG_END,
        SVG24
        "<line x1='12' y1='1' x2='12' y2='23'/>"
        SVG_END },

    /* PLUGIN_FILLED — solid plug body. */
    [ICON_PLUGIN_FILLED] = { SVG24
        "<line x1='9' y1='2' x2='9' y2='8'/>"
        "<line x1='15' y1='2' x2='15' y2='8'/>"
        "<path d='M5 8h14v4a7 7 0 0 1-14 0V8z' fill='white'/>"
        "<line x1='12' y1='19' x2='12' y2='22'/>"
        SVG_END, NULL },
};

/* Below this many device pixels a stroke is widened to the floor, so tiny
 * glyphs (7 px macOS traffic lights) do not fade into a gray smear. */
#define MIN_STROKE_PX 1.0f

typedef struct CacheEntry {
    IconId             id;
    int                sz;
    SDL_Texture*       tex;
    struct CacheEntry* next;
} CacheEntry;

typedef struct PillEntry {
    int                w, h, radius;
    SDL_Texture*       tex;
    struct PillEntry*  next;
} PillEntry;

static struct {
    NSVGimage*       parsed[ICON_COUNT];
    NSVGimage*       cut[ICON_COUNT];
    float            scale;          /* 0 until set -> treated as 1.0 */
    CacheEntry*      cache_head;
    PillEntry*       pill_head;
} g;

static NSVGimage* parse_svg(const char* src, int idx, const char* what)
{
    if (!src) return NULL;
    /* nsvgParse mutates its input, so dup first. */
    char* copy = strdup(src);
    if (!copy) return NULL;
    NSVGimage* img = nsvgParse(copy, "px", 96.0f);
    free(copy);
    if (!img)
        fprintf(stderr, "icons_init: nsvgParse failed for icon %d (%s)\n", idx, what);
    return img;
}

int icons_init(SDL_Renderer* r)
{
    (void)r;
    for (int i = 0; i < ICON_COUNT; ++i) {
        g.parsed[i] = parse_svg(SRC[i].svg, i, "main");
        g.cut[i]    = parse_svg(SRC[i].cut, i, "cut");
    }
    return 0;
}

static void flush_icon_cache(void)
{
    for (CacheEntry* e = g.cache_head; e; ) {
        CacheEntry* n = e->next;
        if (e->tex) SDL_DestroyTexture(e->tex);
        free(e);
        e = n;
    }
    g.cache_head = NULL;
}

void icons_shutdown(void)
{
    for (int i = 0; i < ICON_COUNT; ++i) {
        if (g.parsed[i]) nsvgDelete(g.parsed[i]);
        if (g.cut[i])    nsvgDelete(g.cut[i]);
        g.parsed[i] = NULL;
        g.cut[i]    = NULL;
    }
    icon_raster_shutdown();
    flush_icon_cache();
    for (PillEntry* e = g.pill_head; e; ) {
        PillEntry* n = e->next;
        if (e->tex) SDL_DestroyTexture(e->tex);
        free(e);
        e = n;
    }
    g.pill_head = NULL;
}

void icons_set_render_scale(float s)
{
    if (s <= 0.0f) s = 1.0f;
    float cur = g.scale > 0.0f ? g.scale : 1.0f;
    g.scale = s;
    if (s != cur) flush_icon_cache();
}

/* Rasterize the icon at sz * scale device pixels and stash the SDL_Texture
 * in the per-(id, sz) cache. Subsequent calls for the same key hit the
 * cache. */
static SDL_Texture* icon_get(SDL_Renderer* r, IconId id, int sz)
{
    if (id < 0 || id >= ICON_COUNT || sz <= 0) return NULL;
    for (CacheEntry* e = g.cache_head; e; e = e->next)
        if (e->id == id && e->sz == sz) return e->tex;

    NSVGimage* img = g.parsed[id];
    if (!img) return NULL;

    float scale = g.scale > 0.0f ? g.scale : 1.0f;
    int P = (int)((float)sz * scale + 0.5f);
    if (P < 1) P = 1;

    unsigned char* px = icon_raster(img, g.cut[id], P, MIN_STROKE_PX);
    if (!px) return NULL;

    /* On little-endian machines, ABGR8888 is byte order R,G,B,A — matches
     * what icon_raster writes. */
    SDL_Surface* surf = SDL_CreateRGBSurfaceWithFormatFrom(
        px, P, P, 32, P * 4, SDL_PIXELFORMAT_ABGR8888);
    SDL_Texture* tex = surf ? SDL_CreateTextureFromSurface(r, surf) : NULL;
    if (surf) SDL_FreeSurface(surf);
    free(px);
    if (!tex) {
        fprintf(stderr, "icon_get: texture create failed for icon %d sz %d (%s)\n",
                id, sz, SDL_GetError());
        return NULL;
    }
    SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
    /* 1:1 on integer scales; linear only matters on fractional DPI. */
    SDL_SetTextureScaleMode(tex, SDL_ScaleModeLinear);

    CacheEntry* e = (CacheEntry*)calloc(1, sizeof *e);
    if (!e) { SDL_DestroyTexture(tex); return NULL; }
    e->id = id; e->sz = sz; e->tex = tex; e->next = g.cache_head;
    g.cache_head = e;
    return tex;
}

void icon_draw(SDL_Renderer* r, IconId id, int x, int y, int sz, SDL_Color c)
{
    SDL_Texture* tex = icon_get(r, id, sz);
    if (!tex) return;
    SDL_SetTextureColorMod(tex, c.r, c.g, c.b);
    SDL_SetTextureAlphaMod(tex, c.a);
    SDL_Rect dst = { x, y, sz, sz };
    SDL_RenderCopy(r, tex, NULL, &dst);
}

/* Analytic SDF rasterizer for a rounded rect. We compute, per pixel, the
 * signed distance to the rounded-rect edge and convert it to coverage via
 * `clamp(0.5 - dist, 0, 1)`. Result: razor-clean edges at any size, no
 * polygon stepping, no oversample tax — much sharper than nanosvgrast's
 * AA at the small radii (4-16px) we use throughout the UI. */
static SDL_Texture* pill_get(SDL_Renderer* r, int w, int h, int radius)
{
    if (w <= 0 || h <= 0) return NULL;
    if (radius < 0) radius = 0;
    if (radius > w / 2) radius = w / 2;
    if (radius > h / 2) radius = h / 2;

    for (PillEntry* e = g.pill_head; e; e = e->next)
        if (e->w == w && e->h == h && e->radius == radius) return e->tex;

    unsigned char* px = (unsigned char*)calloc((size_t)w * h * 4, 1);
    if (!px) return NULL;

    float cx = w * 0.5f;
    float cy = h * 0.5f;
    /* "Inner" half-extents — the part of the rounded rect that isn't on a
     * rounded corner. Distance from the inner box edge to a pixel center,
     * minus radius, is the SDF for the rounded-rect interior. */
    float hwx = cx - (float)radius;
    float hyy = cy - (float)radius;
    float rr  = (float)radius;

    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            float fx = (float)x + 0.5f;
            float fy = (float)y + 0.5f;
            float dx = fabsf(fx - cx) - hwx; if (dx < 0) dx = 0;
            float dy = fabsf(fy - cy) - hyy; if (dy < 0) dy = 0;
            float dist = sqrtf(dx * dx + dy * dy) - rr;
            float a = 0.5f - dist;
            if (a < 0.0f) a = 0.0f;
            if (a > 1.0f) a = 1.0f;
            unsigned char A = (unsigned char)(a * 255.0f + 0.5f);
            int o = (y * w + x) * 4;
            px[o + 0] = 255;
            px[o + 1] = 255;
            px[o + 2] = 255;
            px[o + 3] = A;
        }
    }

    SDL_Surface* surf = SDL_CreateRGBSurfaceWithFormatFrom(
        px, w, h, 32, w * 4, SDL_PIXELFORMAT_ABGR8888);
    SDL_Texture* tex = surf ? SDL_CreateTextureFromSurface(r, surf) : NULL;
    if (surf) SDL_FreeSurface(surf);
    free(px);
    if (!tex) return NULL;
    SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
    SDL_SetTextureScaleMode(tex, SDL_ScaleModeLinear);

    PillEntry* e = (PillEntry*)calloc(1, sizeof *e);
    e->w = w; e->h = h; e->radius = radius; e->tex = tex; e->next = g.pill_head;
    g.pill_head = e;
    return tex;
}

void pill_draw(SDL_Renderer* r, int x, int y, int w, int h,
               int radius, SDL_Color c)
{
    SDL_Texture* tex = pill_get(r, w, h, radius);
    if (!tex) return;
    SDL_SetTextureColorMod(tex, c.r, c.g, c.b);
    SDL_SetTextureAlphaMod(tex, c.a);
    SDL_Rect dst = { x, y, w, h };
    SDL_RenderCopy(r, tex, NULL, &dst);
}
