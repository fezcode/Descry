#include "scriptorium.h"
#include "scriptorium_core.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static const uint32_t INK_HEX[SCR_INK_COUNT] = {
    0x4a3a2a, 0x8c7a62, 0xb22d3a, 0xb08a3c, 0x3f5a8c,
    0x6b7a4a, 0xe9c9c0, 0xdfd1b6, 0xe8dcc6,
};

/* All art sits on a grid of "art pixels", each u x u device pixels. */
#define STITCH_ART   6       /* a stitch cell: a 5 x 5 cross plus a gap */
#define TILE_CELLS   16      /* the stitch tile is 16 x 16 cells */
#define TEXT_CACHE   256
#define CAP_CACHE    4
#define ORN_ART      2       /* ornaments are drawn at 2 art pixels a dot */
#define RUBRIC_PAD   2       /* art pixels between a rubric's hems and letters */
#define RUBRIC_ROWS  (2 * SCR_SMALL_ROWS + 2)
#define KIND_RUBRIC  SCR_FONT_COUNT

typedef struct { int w, h; uint32_t* p; } Bmp;      /* 0xAARRGGBB */

typedef struct {
    SDL_Texture* tex;
    int          w, h;                              /* device pixels */
} Tex;

struct TextEntry {
    char*    text;                                  /* glyph bytes */
    int      kind;                                  /* ScrFont or KIND_RUBRIC */
    Tex      t;
    unsigned used;
};

struct CapEntry { char letter; int size; Tex t; unsigned used; };

struct Scriptorium {
    SDL_Renderer*    r;
    float            scale;
    int              u;                             /* device px per art px */
    Tex              stitch;                        /* white; tinted on draw */
    Tex              orn[SCR_ORN_COUNT];
    struct TextEntry text[TEXT_CACHE];
    struct CapEntry  cap[CAP_CACHE];
    unsigned         tick;
};

/* ------------------------------------------------------------- bitmaps */

static Bmp bmp_new(int w, int h)
{
    Bmp b = { w, h, NULL };
    if (w > 0 && h > 0) b.p = calloc((size_t)w * h, sizeof *b.p);
    return b;
}

static void bmp_rect(Bmp* b, int x, int y, int w, int h, uint32_t argb)
{
    if (!b->p) return;
    for (int yy = y < 0 ? 0 : y; yy < y + h && yy < b->h; ++yy)
        for (int xx = x < 0 ? 0 : x; xx < x + w && xx < b->w; ++xx)
            b->p[yy * b->w + xx] = argb;
}

static Tex bmp_upload(SDL_Renderer* r, Bmp* b)
{
    Tex t = { NULL, 0, 0 };
    if (b->p) {
        t.tex = SDL_CreateTexture(r, SDL_PIXELFORMAT_ARGB8888,
                                  SDL_TEXTUREACCESS_STATIC, b->w, b->h);
        if (t.tex) {
            SDL_UpdateTexture(t.tex, NULL, b->p, b->w * 4);
            SDL_SetTextureBlendMode(t.tex, SDL_BLENDMODE_BLEND);
            SDL_SetTextureScaleMode(t.tex, SDL_ScaleModeNearest);
            t.w = b->w;
            t.h = b->h;
        }
    }
    free(b->p);
    b->p = NULL;
    return t;
}

static void tex_free(Tex* t)
{
    if (t->tex) SDL_DestroyTexture(t->tex);
    t->tex = NULL;
    t->w = t->h = 0;
}

#define OPAQUE(rgb) (0xff000000u | (rgb))
#define WHITE       0xffffffffu

SDL_Color scr_ink(ScrInk i)
{
    uint32_t h = INK_HEX[(i >= 0 && i < SCR_INK_COUNT) ? i : SCR_INK];
    SDL_Color c = { (Uint8)(h >> 16), (Uint8)(h >> 8), (Uint8)h, 255 };
    return c;
}

static void tint(SDL_Texture* t, ScrInk i)
{
    SDL_Color c = scr_ink(i);
    SDL_SetTextureColorMod(t, c.r, c.g, c.b);
}

/* Copy a device-pixel region of t to logical (x, y) + a device offset. */
static void put(Scriptorium* s, const Tex* t, int sx, int sy, int sw, int sh,
                float x, float y)
{
    SDL_Rect  src = { sx, sy, sw, sh };
    SDL_FRect dst = { x, y, sw / s->scale, sh / s->scale };
    SDL_RenderCopyF(s->r, t->tex, &src, &dst);
}

/* ------------------------------------------------------------- lifecycle */

Scriptorium* scr_create(SDL_Renderer* r)
{
    Scriptorium* s = calloc(1, sizeof *s);
    if (!s) return NULL;
    s->r = r;
    s->scale = 1.0f;
    s->u = 1;
    return s;
}

static void scr_flush(Scriptorium* s)
{
    tex_free(&s->stitch);
    for (int i = 0; i < SCR_ORN_COUNT; ++i) tex_free(&s->orn[i]);
    for (int i = 0; i < TEXT_CACHE; ++i) {
        tex_free(&s->text[i].t);
        free(s->text[i].text);
    }
    memset(s->text, 0, sizeof s->text);
    for (int i = 0; i < CAP_CACHE; ++i) tex_free(&s->cap[i].t);
    memset(s->cap, 0, sizeof s->cap);
}

void scr_destroy(Scriptorium* s)
{
    if (!s) return;
    scr_flush(s);
    free(s);
}

void scr_set_scale(Scriptorium* s, float scale)
{
    if (!s || scale <= 0 || scale == s->scale) return;
    scr_flush(s);
    s->scale = scale;
    s->u = (int)floorf(scale + 0.5f);
    if (s->u < 1) s->u = 1;
}

/* ------------------------------------------------------------- stitches */

static int cell_dev(const Scriptorium* s) { return STITCH_ART * s->u; }

int scr_cell(const Scriptorium* s)
{
    return s ? (int)ceilf(cell_dev(s) / s->scale) : 4;
}

static bool stitch_ready(Scriptorium* s)
{
    if (s->stitch.tex) return true;
    int c = cell_dev(s), u = s->u;
    Bmp b = bmp_new(TILE_CELLS * c, TILE_CELLS * c);
    int arm = STITCH_ART - 1;                 /* the cross is arm x arm */
    for (int cy = 0; cy < TILE_CELLS; ++cy)
        for (int cx = 0; cx < TILE_CELLS; ++cx)
            for (int i = 0; i < arm; ++i) {
                bmp_rect(&b, cx * c + i * u, cy * c + i * u, u, u, WHITE);
                bmp_rect(&b, cx * c + (arm - 1 - i) * u, cy * c + i * u,
                         u, u, WHITE);
            }
    s->stitch = bmp_upload(s->r, &b);
    return s->stitch.tex != NULL;
}

/* nx x ny stitches with their top-left device pixel at (dx, dy). */
static void stitch_block(Scriptorium* s, int dx, int dy, int nx, int ny)
{
    int c = cell_dev(s);
    for (int j = 0; j < ny; j += TILE_CELLS)
        for (int i = 0; i < nx; i += TILE_CELLS) {
            int w = nx - i < TILE_CELLS ? nx - i : TILE_CELLS;
            int h = ny - j < TILE_CELLS ? ny - j : TILE_CELLS;
            put(s, &s->stitch, 0, 0, w * c, h * c,
                (dx + i * c) / s->scale, (dy + j * c) / s->scale);
        }
}

/* Whole stitches in a logical length, and the device offset centring them
 * (the cell's own trailing gap is not counted as part of the last stitch). */
static int fit(const Scriptorium* s, int len, int* off)
{
    int c = cell_dev(s);
    int dev = (int)floorf(len * s->scale);
    int n = (dev + s->u) / c;
    if (n < 0) n = 0;
    *off = (dev - (n * c - s->u)) / 2;
    return n;
}

void scr_stitch_row(Scriptorium* s, int x, int y, int w, ScrInk i)
{
    if (!s || !stitch_ready(s)) return;
    int off, n = fit(s, w, &off);
    tint(s->stitch.tex, i);
    stitch_block(s, (int)lroundf(x * s->scale) + off,
                 (int)lroundf(y * s->scale), n, 1);
}

void scr_stitch_col(Scriptorium* s, int x, int y, int h, ScrInk i)
{
    if (!s || !stitch_ready(s)) return;
    int off, n = fit(s, h, &off);
    tint(s->stitch.tex, i);
    stitch_block(s, (int)lroundf(x * s->scale),
                 (int)lroundf(y * s->scale) + off, 1, n);
}

void scr_frame(Scriptorium* s, SDL_Rect r, int depth, ScrInk i)
{
    if (!s || r.w <= 0 || r.h <= 0 || !stitch_ready(s)) return;
    int offx, offy;
    int nx = fit(s, r.w, &offx), ny = fit(s, r.h, &offy);
    if (depth < 1) depth = 1;
    if (nx < 2 * depth || ny < 2 * depth) return;
    int c  = cell_dev(s);
    int x0 = (int)lroundf(r.x * s->scale) + offx;
    int y0 = (int)lroundf(r.y * s->scale) + offy;
    tint(s->stitch.tex, i);
    stitch_block(s, x0, y0, nx, depth);
    stitch_block(s, x0, y0 + (ny - depth) * c, nx, depth);
    stitch_block(s, x0, y0 + depth * c, depth, ny - 2 * depth);
    stitch_block(s, x0 + (nx - depth) * c, y0 + depth * c, depth,
                 ny - 2 * depth);
}

/* ------------------------------------------------------------- the hand */

static int font_px(const Scriptorium* s, int f)
{
    return f == SCR_FONT_BIG ? 2 * s->u : s->u;
}

bool scr_text_ok(const char* str, size_t len)
{
    char tmp[512];
    return scr_norm(str, len, tmp, sizeof tmp) >= 0;
}

int scr_text_h(const Scriptorium* s, ScrFont f)
{
    return (int)lroundf(SCR_ROWS * font_px(s, f) / s->scale);
}

int scr_text_w(const Scriptorium* s, const char* str, size_t len, ScrFont f)
{
    char g[512];
    int n = scr_norm(str, len, g, sizeof g);
    if (n < 0) return -1;
    return (int)lroundf(scr_text_cols(g, n) * font_px(s, f) / s->scale);
}

/* Stamp glyph ch into b at art scale px, top-left (x, y). */
static void stamp(Bmp* b, unsigned char ch, int x, int y, int px, uint32_t argb)
{
    const char* const* rows = scr_glyph(ch);
    if (!rows) return;
    for (int r = 0; r < SCR_ROWS; ++r)
        for (int c = 0; rows[r][c]; ++c)
            if (rows[r][c] == '1')
                bmp_rect(b, x + c * px, y + r * px, px, px, argb);
}

static char upper(char c) { return (c >= 'a' && c <= 'z') ? (char)(c - 32) : c; }

/* Width in art pixels of one rubric letter: the capital doubled, plus the
 * outline on either side. */
static int rubric_cols(unsigned char ch) { return 2 * scr_glyph_cols(ch) + 2; }

static int rubric_text_cols(const char* g, int n)
{
    int cols = 0;
    for (int i = 0; i < n; ++i) cols += rubric_cols((unsigned char)g[i]) + 1;
    return n > 0 ? cols - 1 : 0;
}

/* Outlined letters: each seven-row form doubled, fattened by one art pixel, then
 * drawn as a cinnabar outline round a parchment body. */
static Tex rubric_build(Scriptorium* s, const char* g, int n)
{
    int cols = rubric_text_cols(g, n), rows = RUBRIC_ROWS;
    Tex none = { NULL, 0, 0 };
    if (cols <= 0) return none;
    unsigned char* m = calloc((size_t)cols * rows, 1);
    unsigned char* d = calloc((size_t)cols * rows, 1);
    if (!m || !d) { free(m); free(d); return none; }
    int x = 1;
    for (int i = 0; i < n; ++i) {
        const char* const* gr = scr_glyph((unsigned char)g[i]);
        /* The seven-row forms: rows 2.. of the chart. */
        for (int r = 0; gr && r < SCR_SMALL_ROWS; ++r)
            for (int c = 0; gr[r + 2][c]; ++c)
                if (gr[r + 2][c] == '1')
                    for (int k = 0; k < 4; ++k)
                        m[(1 + 2 * r + (k >> 1)) * cols + x + 2 * c + (k & 1)] = 1;
        x += rubric_cols((unsigned char)g[i]) + 1;
    }
    for (int r = 0; r < rows; ++r)
        for (int c = 0; c < cols; ++c)
            for (int dy = -1; dy <= 1 && !d[r * cols + c]; ++dy)
                for (int dx = -1; dx <= 1; ++dx) {
                    int rr = r + dy, cc = c + dx;
                    if (rr >= 0 && rr < rows && cc >= 0 && cc < cols &&
                        m[rr * cols + cc]) { d[r * cols + c] = 1; break; }
                }
    int u = s->u;
    Bmp b = bmp_new(cols * u, rows * u);
    for (int r = 0; r < rows; ++r)
        for (int c = 0; c < cols; ++c) {
            if (!d[r * cols + c]) continue;
            bool edge = r == 0 || r == rows - 1 || c == 0 || c == cols - 1 ||
                        !d[(r - 1) * cols + c] || !d[(r + 1) * cols + c] ||
                        !d[r * cols + c - 1]   || !d[r * cols + c + 1];
            bmp_rect(&b, c * u, r * u, u, u,
                     OPAQUE(edge ? INK_HEX[SCR_CINNABAR] : INK_HEX[SCR_PARCHMENT]));
        }
    free(m);
    free(d);
    return bmp_upload(s->r, &b);
}

static struct TextEntry* text_get(Scriptorium* s, const char* g, int kind)
{
    struct TextEntry* free_slot = NULL;
    struct TextEntry* oldest = &s->text[0];
    for (int i = 0; i < TEXT_CACHE; ++i) {
        struct TextEntry* e = &s->text[i];
        if (!e->text) { if (!free_slot) free_slot = e; continue; }
        if (e->kind == kind && !strcmp(e->text, g)) {
            e->used = ++s->tick;
            return e;
        }
        if (e->used < oldest->used) oldest = e;
    }
    struct TextEntry* e = free_slot ? free_slot : oldest;
    tex_free(&e->t);
    free(e->text);
    memset(e, 0, sizeof *e);

    int n = (int)strlen(g);
    if (kind == KIND_RUBRIC) {
        e->t = rubric_build(s, g, n);
    } else {
        int px = font_px(s, kind), cols = scr_text_cols(g, n);
        Bmp b = bmp_new(cols * px, SCR_ROWS * px);
        int x = 0;
        for (int i = 0; i < n; ++i) {
            stamp(&b, (unsigned char)g[i], x * px, 0, px, WHITE);
            x += scr_glyph_cols((unsigned char)g[i]) + 1;
        }
        e->t = bmp_upload(s->r, &b);
    }
    e->text = strdup(g);
    e->kind = kind;
    e->used = ++s->tick;
    return e;
}

int scr_text(Scriptorium* s, const char* str, size_t len, ScrFont f,
             int x, int y, ScrInk ink, ScrInk initial, int max_w)
{
    if (!s) return -1;
    char g[512];
    int n = scr_norm(str, len, g, sizeof g - 4);
    if (n < 0) return -1;
    if (n == 0) return 0;
    float px = font_px(s, f) / s->scale;
    /* Rounded, as scr_text_w reports it: a caller that passes the measured
     * width back as the limit must not see its own text elided. */
    if (max_w > 0 && lroundf(scr_text_cols(g, n) * px) > max_w) {
        /* Elide: shorten until the text plus "..." fits. */
        while (n > 0) {
            --n;
            while (n > 0 && g[n - 1] == ' ') --n;
            memcpy(g + n, "...", 4);
            if (scr_text_cols(g, n + 3) * px <= max_w) break;
        }
        if (n == 0 && scr_text_cols("...", 3) * px > max_w) return 0;
        n += 3;
        g[n] = 0;
    }
    struct TextEntry* e = text_get(s, g, f);
    if (!e || !e->t.tex) return -1;
    tint(e->t.tex, ink);
    put(s, &e->t, 0, 0, e->t.w, e->t.h, (float)x, (float)y);
    if (initial != ink) {
        int w0 = scr_glyph_cols((unsigned char)g[0]) * font_px(s, f);
        tint(e->t.tex, initial);
        put(s, &e->t, 0, 0, w0, e->t.h, (float)x, (float)y);
    }
    return (int)lroundf(scr_text_cols(g, n) * px);
}

/* ------------------------------------------------------------- rubric */

int scr_rubric_h(const Scriptorium* s)
{
    int dev = 2 * cell_dev(s) + (RUBRIC_ROWS + 2 * RUBRIC_PAD) * s->u;
    return (int)ceilf(dev / s->scale);
}

bool scr_rubric(Scriptorium* s, SDL_Rect r, const char* text)
{
    if (!s || r.w <= 0) return false;
    int h = scr_rubric_h(s), cell = scr_cell(s);
    SDL_Color rose = scr_ink(SCR_ROSE);
    SDL_SetRenderDrawColor(s->r, rose.r, rose.g, rose.b, 255);
    SDL_Rect band = { r.x, r.y, r.w, h };
    SDL_RenderFillRect(s->r, &band);
    scr_stitch_row(s, r.x, r.y, r.w, SCR_CINNABAR);
    scr_stitch_row(s, r.x, r.y + h - cell, r.w, SCR_CINNABAR);

    char g[128];
    int n = text ? scr_norm(text, strlen(text), g, sizeof g) : -1;
    if (n < 0) return false;
    /* Rubrics are lettered in the seven-row forms, i.e. the lower case. */
    for (int i = 0; i < n; ++i)
        if (g[i] >= 'A' && g[i] <= 'Z') g[i] = (char)(g[i] + 32);
    float px  = s->u / s->scale;
    int   pad = 8;
    while (n > 0 && rubric_text_cols(g, n) * px > r.w - 2 * pad) --n;
    while (n > 0 && g[n - 1] == ' ') --n;
    g[n] = 0;
    if (n == 0) return true;
    struct TextEntry* e = text_get(s, g, KIND_RUBRIC);
    if (!e || !e->t.tex) return false;
    put(s, &e->t, 0, 0, e->t.w, e->t.h, (float)(r.x + pad),
        r.y + (cell_dev(s) + RUBRIC_PAD * s->u) / s->scale);
    return true;
}

/* ------------------------------------------------------------- drop cap */

void scr_dropcap(Scriptorium* s, SDL_Rect tile, char letter)
{
    if (!s || tile.w <= 0) return;
    int size = (int)lroundf(tile.w * s->scale);
    letter = upper(letter);
    struct CapEntry* e = NULL;
    struct CapEntry* oldest = &s->cap[0];
    for (int i = 0; i < CAP_CACHE; ++i) {
        struct CapEntry* c = &s->cap[i];
        if (c->t.tex && c->letter == letter && c->size == size) { e = c; break; }
        if (c->used < oldest->used) oldest = c;
    }
    if (!e) {
        e = oldest;
        tex_free(&e->t);
        int u = s->u;
        Bmp b = bmp_new(size, size);
        /* Gold leaf border, an ink rule, then a cinnabar ground diapered
         * with rose dots. */
        bmp_rect(&b, 0, 0, size, size, OPAQUE(INK_HEX[SCR_GOLD]));
        bmp_rect(&b, 2 * u, 2 * u, size - 4 * u, size - 4 * u,
                 OPAQUE(INK_HEX[SCR_INK]));
        int in = 3 * u, iw = size - 2 * in;
        bmp_rect(&b, in, in, iw, iw, OPAQUE(INK_HEX[SCR_CINNABAR]));
        for (int y = 0, row = 0; y + u <= iw; y += 3 * u, ++row)
            for (int x = (row & 1) ? 3 * u : 0; x + u <= iw; x += 6 * u)
                bmp_rect(&b, in + x + u, in + y + u, u, u,
                         OPAQUE(INK_HEX[SCR_ROSE]));
        /* Corner studs on the gold. */
        for (int k = 0; k < 4; ++k)
            bmp_rect(&b, (k & 1) ? size - 2 * u : 0, (k & 2) ? size - 2 * u : 0,
                     2 * u, 2 * u, OPAQUE(INK_HEX[SCR_CINNABAR]));
        int cols = scr_glyph_cols((unsigned char)letter);
        if (cols > 0) {
            int px = (iw - 4 * u) / SCR_ROWS;
            if (px < 1) px = 1;
            int lx = in + (iw - cols * px) / 2, ly = in + (iw - SCR_ROWS * px) / 2;
            stamp(&b, (unsigned char)letter, lx + u, ly + u, px,
                  OPAQUE(INK_HEX[SCR_INK]));
            stamp(&b, (unsigned char)letter, lx, ly, px,
                  OPAQUE(INK_HEX[SCR_PARCHMENT]));
        }
        e->t = bmp_upload(s->r, &b);
        e->letter = letter;
        e->size = size;
    }
    e->used = ++s->tick;
    if (!e->t.tex) return;
    SDL_SetTextureColorMod(e->t.tex, 255, 255, 255);
    put(s, &e->t, 0, 0, e->t.w, e->t.h, (float)tile.x, (float)tile.y);
}

/* ------------------------------------------------------------- ornaments */

/* r cinnabar, g gold, k ink, f faded, p parchment; anything else is empty.
 * Rows may be ragged; the widest sets the width. */
static const char* const CANDLE[] = {
    "......r......",
    ".....rgr.....",
    ".....rgr.....",
    "....rgggr....",
    "....rgggr....",
    ".....rgr.....",
    "......k......",
    "......k......",
    "....rrrrr....",
    "....r...r....",
    "....r.r.r....",
    "....r...r....",
    "....r...r....",
    "....r...r....",
    "....r...r....",
    "....r...r....",
    "....r...r....",
    "....r...r....",
    "....r...r....",
    "....r...r....",
    "....r...r....",
    "....r...r....",
    "....r...r....",
    "....r...r....",
    "..rrrrrrrrr..",
    ".r.........r.",
    ".r.........r.",
    "..rrrrrrrrr..",
    NULL
};
#define CANDLE_FLAME_ROWS 6

static const char* const CHAPEL[] = {
    "..............r..............",
    "..............r..............",
    ".............rrr.............",
    "..............r..............",
    "..............r..............",
    ".............r.r.............",
    "............r...r............",
    "...........r.....r...........",
    "..........r.......r..........",
    ".........r....g....r.........",
    "........r....ggg....r........",
    ".......r......g......r.......",
    "......r...............r......",
    ".....rrrrrrrrrrrrrrrrrrr.....",
    ".....r.................r.....",
    ".....r..rr.........rr..r.....",
    ".....r.r..r.......r..r.r.....",
    ".....r.r..r.......r..r.r.....",
    ".....r.rrrr..rrr..rrrr.r.....",
    ".....r......r...r......r.....",
    ".....r......r...r......r.....",
    ".....r......r..gr......r.....",
    ".....r......r...r......r.....",
    "..rrrrrrrrrrrrrrrrrrrrrrrrr..",
    NULL
};

static const char* const SNAIL[] = {
    "........................r...r.",
    ".........rrrrr..........r...r.",
    ".......rr.....rr.........r.r..",
    "......r...ggg...r.......rrrr..",
    ".....r...g...g...r.....r....r.",
    ".....r..g..g..g..r.....r..k.r.",
    ".....r..g..gg.g..r....r.....r.",
    ".....r...g....g..r...r.....r..",
    "......r...gggg..r...r....rr...",
    "..rrrrrrrrrrrrrrrrrr....r.....",
    ".r.....................r......",
    "..rrrrrrrrrrrrrrrrrrrrr.......",
    NULL
};

static Bmp chart_bmp(const char* const* rows, int skip_rows_blank, int px)
{
    int h = 0, w = 0;
    for (; rows[h]; ++h) {
        int l = (int)strlen(rows[h]);
        if (l > w) w = l;
    }
    Bmp b = bmp_new(w * px, h * px);
    for (int r = skip_rows_blank; r < h; ++r)
        for (int c = 0; rows[r][c]; ++c) {
            uint32_t ink;
            switch (rows[r][c]) {
                case 'r': ink = INK_HEX[SCR_CINNABAR];  break;
                case 'g': ink = INK_HEX[SCR_GOLD];      break;
                case 'k': ink = INK_HEX[SCR_INK];       break;
                case 'f': ink = INK_HEX[SCR_FADED];     break;
                case 'p': ink = INK_HEX[SCR_PARCHMENT]; break;
                default:  continue;
            }
            bmp_rect(&b, c * px, r * px, px, px, OPAQUE(ink));
        }
    return b;
}

/* A stitched gold gable with a cinnabar boss, as over the reference's
 * title bar: 41 x 11 dots. */
#define PED_W 41
#define PED_H 11
static Bmp pediment_bmp(int px)
{
    Bmp b = bmp_new(PED_W * px, PED_H * px);
    for (int y = 0; y < PED_H; ++y)
        for (int x = 0; x < PED_W; ++x) {
            int dx = abs(x - PED_W / 2);
            if (dx > 2 * y) continue;
            bool boss = dx <= 1 && y >= 5 && y <= 7;
            bool on   = y == PED_H - 1 || ((x + y) & 1) == 0;
            if (boss)    bmp_rect(&b, x * px, y * px, px, px,
                                  OPAQUE(INK_HEX[SCR_CINNABAR]));
            else if (on) bmp_rect(&b, x * px, y * px, px, px,
                                  OPAQUE(INK_HEX[SCR_GOLD]));
        }
    return b;
}

static const Tex* orn_get(Scriptorium* s, ScrOrnament o)
{
    if (!s || o < 0 || o >= SCR_ORN_COUNT) return NULL;
    Tex* t = &s->orn[o];
    if (!t->tex) {
        int px = ORN_ART * s->u;
        Bmp b;
        switch (o) {
            case SCR_ORN_CANDLE:     b = chart_bmp(CANDLE, CANDLE_FLAME_ROWS, px); break;
            case SCR_ORN_CANDLE_LIT: b = chart_bmp(CANDLE, 0, px); break;
            case SCR_ORN_CHAPEL:     b = chart_bmp(CHAPEL, 0, px); break;
            case SCR_ORN_SNAIL:      b = chart_bmp(SNAIL, 0, px);  break;
            default:                 b = pediment_bmp(s->u);       break;
        }
        *t = bmp_upload(s->r, &b);
    }
    return t->tex ? t : NULL;
}

/* Sizes come from the charts, so they are known before any texture is. */
static void orn_dots(ScrOrnament o, int* w, int* h, int* px_art)
{
    const char* const* rows = o == SCR_ORN_CHAPEL ? CHAPEL
                            : o == SCR_ORN_SNAIL  ? SNAIL : CANDLE;
    *px_art = ORN_ART;
    if (o == SCR_ORN_PEDIMENT) { *w = PED_W; *h = PED_H; *px_art = 1; return; }
    *w = *h = 0;
    for (; rows[*h]; ++*h) {
        int l = (int)strlen(rows[*h]);
        if (l > *w) *w = l;
    }
}

int scr_orn_w(const Scriptorium* s, ScrOrnament o)
{
    int w, h, a;
    orn_dots(o, &w, &h, &a);
    return (int)ceilf(w * a * s->u / s->scale);
}

int scr_orn_h(const Scriptorium* s, ScrOrnament o)
{
    int w, h, a;
    orn_dots(o, &w, &h, &a);
    return (int)ceilf(h * a * s->u / s->scale);
}

void scr_ornament(Scriptorium* s, ScrOrnament o, int x, int y)
{
    const Tex* t = orn_get(s, o);
    if (!t) return;
    put(s, t, 0, 0, t->w, t->h, (float)x, (float)y);
}
