#include "icons.h"

#define NANOSVG_IMPLEMENTATION
#define NANOSVGRAST_IMPLEMENTATION
#include "nanosvg.h"
#include "nanosvgrast.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Lucide-style icons (ISC-licensed inspiration). 24x24 viewBox, 2px stroke,
 * round caps + joins. Using stroke='white' so a single texture serves any
 * tint via SDL_SetTextureColorMod. */
static const char* SVG_SRC[ICON_COUNT] = {
    /* SETTINGS — three sliders, each with a circular knob */
    [ICON_SETTINGS] =
    "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 24 24' fill='none' "
    "stroke='white' stroke-width='2' stroke-linecap='round' stroke-linejoin='round'>"
    "<line x1='21' y1='4' x2='14' y2='4'/>"
    "<line x1='10' y1='4' x2='3' y2='4'/>"
    "<line x1='21' y1='12' x2='12' y2='12'/>"
    "<line x1='8' y1='12' x2='3' y2='12'/>"
    "<line x1='21' y1='20' x2='16' y2='20'/>"
    "<line x1='12' y1='20' x2='3' y2='20'/>"
    "<circle cx='12' cy='4' r='2'/>"
    "<circle cx='10' cy='12' r='2'/>"
    "<circle cx='14' cy='20' r='2'/>"
    "</svg>",

    /* FIND — magnifying glass */
    [ICON_FIND] =
    "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 24 24' fill='none' "
    "stroke='white' stroke-width='2' stroke-linecap='round' stroke-linejoin='round'>"
    "<circle cx='11' cy='11' r='8'/>"
    "<line x1='21' y1='21' x2='16.65' y2='16.65'/>"
    "</svg>",

    /* SIDEBAR_OPEN — panel with left third filled */
    [ICON_SIDEBAR_OPEN] =
    "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 24 24' fill='none' "
    "stroke='white' stroke-width='2' stroke-linecap='round' stroke-linejoin='round'>"
    "<rect x='3' y='3' width='18' height='18' rx='2'/>"
    "<line x1='9' y1='3' x2='9' y2='21'/>"
    "<rect x='3' y='3' width='6' height='18' rx='1' fill='white' stroke='none'/>"
    "</svg>",

    /* SIDEBAR_CLOSED — panel outline only with divider */
    [ICON_SIDEBAR_CLOSED] =
    "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 24 24' fill='none' "
    "stroke='white' stroke-width='2' stroke-linecap='round' stroke-linejoin='round'>"
    "<rect x='3' y='3' width='18' height='18' rx='2'/>"
    "<line x1='9' y1='3' x2='9' y2='21'/>"
    "</svg>",

    /* OUTLINE — list with bullet dots */
    [ICON_OUTLINE] =
    "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 24 24' fill='none' "
    "stroke='white' stroke-width='2' stroke-linecap='round' stroke-linejoin='round'>"
    "<line x1='8' y1='6' x2='21' y2='6'/>"
    "<line x1='8' y1='12' x2='21' y2='12'/>"
    "<line x1='8' y1='18' x2='21' y2='18'/>"
    "<circle cx='4' cy='6'  r='1' fill='white' stroke='none'/>"
    "<circle cx='4' cy='12' r='1' fill='white' stroke='none'/>"
    "<circle cx='4' cy='18' r='1' fill='white' stroke='none'/>"
    "</svg>",

    /* FOLDER — closed folder shape */
    [ICON_FOLDER] =
    "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 24 24' fill='none' "
    "stroke='white' stroke-width='2' stroke-linecap='round' stroke-linejoin='round'>"
    "<path d='M22 19a2 2 0 0 1-2 2H4a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2h5l2 3h9a2 2 0 0 1 2 2z'/>"
    "</svg>",

    /* FOLDER_OPEN — folder with peeking-up lid */
    [ICON_FOLDER_OPEN] =
    "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 24 24' fill='none' "
    "stroke='white' stroke-width='2' stroke-linecap='round' stroke-linejoin='round'>"
    "<path d='m6 14 1.45-2.9A2 2 0 0 1 9.24 10H20a2 2 0 0 1 1.94 2.5l-1.55 6a2 2 0 0 1-1.94 1.5H4a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2h3.9a2 2 0 0 1 1.69.9l.81 1.2a2 2 0 0 0 1.67.9H18a2 2 0 0 1 2 2v2'/>"
    "</svg>",

    /* FILE — document with folded corner */
    [ICON_FILE] =
    "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 24 24' fill='none' "
    "stroke='white' stroke-width='2' stroke-linecap='round' stroke-linejoin='round'>"
    "<path d='M14 2H6a2 2 0 0 0-2 2v16a2 2 0 0 0 2 2h12a2 2 0 0 0 2-2V8z'/>"
    "<polyline points='14 2 14 8 20 8'/>"
    "</svg>",

    /* CARET_RIGHT — chevron pointing right */
    [ICON_CARET_RIGHT] =
    "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 24 24' fill='none' "
    "stroke='white' stroke-width='2.5' stroke-linecap='round' stroke-linejoin='round'>"
    "<polyline points='9 18 15 12 9 6'/>"
    "</svg>",

    /* CARET_DOWN — chevron pointing down */
    [ICON_CARET_DOWN] =
    "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 24 24' fill='none' "
    "stroke='white' stroke-width='2.5' stroke-linecap='round' stroke-linejoin='round'>"
    "<polyline points='6 9 12 15 18 9'/>"
    "</svg>",

    /* WIN_MIN — minimize: single horizontal line */
    [ICON_WIN_MIN] =
    "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 24 24' fill='none' "
    "stroke='white' stroke-width='1.5' stroke-linecap='square'>"
    "<line x1='5' y1='12' x2='19' y2='12'/>"
    "</svg>",

    /* WIN_MAX — maximize: single square */
    [ICON_WIN_MAX] =
    "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 24 24' fill='none' "
    "stroke='white' stroke-width='1.5' stroke-linejoin='miter'>"
    "<rect x='5' y='5' width='14' height='14'/>"
    "</svg>",

    /* WIN_RESTORE — two stacked squares (restore from maximized) */
    [ICON_WIN_RESTORE] =
    "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 24 24' fill='none' "
    "stroke='white' stroke-width='1.5' stroke-linejoin='miter'>"
    "<rect x='7' y='4' width='13' height='13'/>"
    "<rect x='4' y='7' width='13' height='13'/>"
    "</svg>",

    /* WIN_CLOSE — X */
    [ICON_WIN_CLOSE] =
    "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 24 24' fill='none' "
    "stroke='white' stroke-width='1.5' stroke-linecap='square'>"
    "<line x1='5' y1='5' x2='19' y2='19'/>"
    "<line x1='19' y1='5' x2='5' y2='19'/>"
    "</svg>",

    /* CHEVRON_LEFT / RIGHT — used by settings adjusters. Solid filled. */
    [ICON_CHEVRON_LEFT] =
    "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 24 24' fill='none' "
    "stroke='white' stroke-width='2.5' stroke-linecap='round' stroke-linejoin='round'>"
    "<polyline points='15 18 9 12 15 6'/>"
    "</svg>",

    [ICON_CHEVRON_RIGHT] =
    "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 24 24' fill='none' "
    "stroke='white' stroke-width='2.5' stroke-linecap='round' stroke-linejoin='round'>"
    "<polyline points='9 18 15 12 9 6'/>"
    "</svg>",

    /* VAULT_SEARCH — text-search: stacked text rules + magnifying lens
     * over the bottom-right. Reads as "find across documents". */
    [ICON_VAULT_SEARCH] =
    "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 24 24' fill='none' "
    "stroke='white' stroke-width='2' stroke-linecap='round' stroke-linejoin='round'>"
    "<path d='M21 6H3'/>"
    "<path d='M10 12H3'/>"
    "<path d='M10 18H3'/>"
    "<circle cx='17' cy='15' r='3'/>"
    "<path d='m21 19-1.9-1.9'/>"
    "</svg>",

    /* COMMAND — terminal-prompt style chevron + underscore. Reads as "run
     * a command" to anyone who's seen a shell, and is visually different
     * from the magnifier icons next to it. */
    [ICON_COMMAND] =
    "<svg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 24 24' fill='none' "
    "stroke='white' stroke-width='2' stroke-linecap='round' stroke-linejoin='round'>"
    "<polyline points='4 17 10 11 4 5'/>"
    "<line x1='12' y1='19' x2='20' y2='19'/>"
    "</svg>",
};

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
    NSVGrasterizer*  rast;
    CacheEntry*      cache_head;
    PillEntry*       pill_head;
} g;

int icons_init(SDL_Renderer* r)
{
    (void)r;
    g.rast = nsvgCreateRasterizer();
    if (!g.rast) {
        fprintf(stderr, "icons_init: nsvgCreateRasterizer failed\n");
        return -1;
    }
    for (int i = 0; i < ICON_COUNT; ++i) {
        if (!SVG_SRC[i]) continue;
        /* nsvgParse mutates its input, so dup first. */
        char* copy = strdup(SVG_SRC[i]);
        if (!copy) continue;
        g.parsed[i] = nsvgParse(copy, "px", 96.0f);
        free(copy);
        if (!g.parsed[i])
            fprintf(stderr, "icons_init: nsvgParse failed for icon %d\n", i);
    }
    return 0;
}

void icons_shutdown(void)
{
    for (int i = 0; i < ICON_COUNT; ++i) {
        if (g.parsed[i]) nsvgDelete(g.parsed[i]);
        g.parsed[i] = NULL;
    }
    if (g.rast) nsvgDeleteRasterizer(g.rast);
    g.rast = NULL;
    for (CacheEntry* e = g.cache_head; e; ) {
        CacheEntry* n = e->next;
        if (e->tex) SDL_DestroyTexture(e->tex);
        free(e);
        e = n;
    }
    g.cache_head = NULL;
    for (PillEntry* e = g.pill_head; e; ) {
        PillEntry* n = e->next;
        if (e->tex) SDL_DestroyTexture(e->tex);
        free(e);
        e = n;
    }
    g.pill_head = NULL;
}

/* Oversample factor: rasterize the SVG at OVERSAMPLE x the requested size,
 * then let SDL bilinear-downscale on render. Without this, a 16x16 raster
 * of a 24-viewBox SVG with 2px strokes turns into mush — strokes become
 * ~1.3px and nanosvg's modest AA can't recover the missing detail. */
#define OVERSAMPLE 3

/* Rasterize the icon at the requested size and stash the SDL_Texture in the
 * per-(id, sz) cache. Subsequent calls for the same key hit the cache. */
static SDL_Texture* icon_get(SDL_Renderer* r, IconId id, int sz)
{
    if (id < 0 || id >= ICON_COUNT || sz <= 0) return NULL;
    for (CacheEntry* e = g.cache_head; e; e = e->next)
        if (e->id == id && e->sz == sz) return e->tex;

    NSVGimage* img = g.parsed[id];
    if (!img || img->width <= 0 || img->height <= 0) return NULL;

    int W = sz * OVERSAMPLE, H = sz * OVERSAMPLE;
    unsigned char* px = (unsigned char*)calloc((size_t)W * H * 4, 1);
    if (!px) return NULL;

    /* Square viewBox (24x24) → uniform scale based on the larger dim. */
    float scale_w = (float)W / img->width;
    float scale_h = (float)H / img->height;
    float scale   = scale_w < scale_h ? scale_w : scale_h;
    nsvgRasterize(g.rast, img, 0, 0, scale, px, W, H, W * 4);

    /* nanosvg outputs PREMULTIPLIED RGBA. SDL_BLENDMODE_BLEND expects
     * straight alpha, so un-premultiply each pixel. Without this, the
     * tint appears double-multiplied and edges get darker than intended. */
    for (int i = 0; i < W * H; ++i) {
        unsigned char a = px[i*4 + 3];
        if (a == 0) continue;
        if (a == 255) continue;
        px[i*4 + 0] = (unsigned char)(px[i*4 + 0] * 255 / a);
        px[i*4 + 1] = (unsigned char)(px[i*4 + 1] * 255 / a);
        px[i*4 + 2] = (unsigned char)(px[i*4 + 2] * 255 / a);
    }

    /* On little-endian machines, ABGR8888 is byte order R,G,B,A — matches
     * what nanosvg writes. */
    SDL_Surface* surf = SDL_CreateRGBSurfaceWithFormatFrom(
        px, W, H, 32, W * 4, SDL_PIXELFORMAT_ABGR8888);
    SDL_Texture* tex = surf ? SDL_CreateTextureFromSurface(r, surf) : NULL;
    if (surf) SDL_FreeSurface(surf);
    free(px);
    if (!tex) {
        fprintf(stderr, "icon_get: texture create failed for icon %d sz %d (%s)\n",
                id, sz, SDL_GetError());
        return NULL;
    }
    SDL_SetTextureBlendMode(tex, SDL_BLENDMODE_BLEND);
    /* Bilinear downscale at render time; the SCALE_QUALITY hint set in
     * app_init drives this. */
    SDL_SetTextureScaleMode(tex, SDL_ScaleModeLinear);

    CacheEntry* e = (CacheEntry*)calloc(1, sizeof *e);
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
