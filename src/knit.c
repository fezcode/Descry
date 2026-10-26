#include "knit.h"
#include "knit_fonts.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Yarns, as in fezcodex's YARN table. */
static const uint32_t YARN_HEX[YARN_COUNT] = {
    0x2c4a7c, 0x213a63, 0x182b4a, 0xe3d8c4, 0xaaa39b, 0xc9952b,
    0xc3202b, 0xefefe9, 0xe0936b, 0x8a3529, 0xd2b26a, 0x3e4a3c,
    0xe8c51a, 0x1d1c1c, 0x1f6b6b, 0x141414, 0x3d5a80, 0x6b3a5b,
};

/* Gauges (stitch width x height, logical px). GAUGE is the fabric; FINE
 * the charted labels; TIGHT a step finer still for dense lists. */
#define GAUGE_W 5.0f
#define GAUGE_H 4.2f
#define FINE_W  4.0f
#define FINE_H  3.4f
#define TIGHT_W 3.2f
#define TIGHT_H 2.8f

/* Stitches snap to whole device pixels: fractional sizes alias into dark
 * seams where rounding leaves the ply shadow showing. */
struct Knit;
static float snap(const struct Knit* k, float g);

#define VARIANTS    5
#define TILE_COLS   48       /* wool tile: 240 x 168 logical px */
#define TILE_ROWS   40
#define BAND_COLS   60       /* a multiple of every band's repeat */
#define TEXT_CACHE  256
#define SPRITE_MAX  512

#define FELT_RGB    0xefe6d6
#define THREAD_RGB  0xb8362c

/* ------------------------------------------------------------- canvas */

/* Premultiplied RGBA float pixels. */
typedef struct { int w, h; float* p; } Canvas;

static Canvas canvas_new(int w, int h)
{
    Canvas c = { w, h, calloc((size_t)w * h * 4, sizeof(float)) };
    return c;
}

static void over(float* d, float r, float g, float b, float a)
{
    float k = 1.0f - a;
    d[0] = r + d[0] * k;
    d[1] = g + d[1] * k;
    d[2] = b + d[2] * k;
    d[3] = a + d[3] * k;
}

static SDL_Texture* canvas_upload(SDL_Renderer* r, const Canvas* c)
{
    if (c->w <= 0 || c->h <= 0) return NULL;
    SDL_Texture* t = SDL_CreateTexture(r, SDL_PIXELFORMAT_RGBA32,
                                       SDL_TEXTUREACCESS_STATIC, c->w, c->h);
    if (!t) return NULL;
    uint8_t* px = malloc((size_t)c->w * c->h * 4);
    if (!px) { SDL_DestroyTexture(t); return NULL; }
    for (int i = 0; i < c->w * c->h; ++i) {
        const float* s = c->p + i * 4;
        float a = s[3] > 1.0f ? 1.0f : s[3];
        float inv = a > 1e-5f ? 1.0f / a : 0.0f;
        for (int ch = 0; ch < 3; ++ch) {
            float v = s[ch] * inv * 255.0f + 0.5f;
            px[i * 4 + ch] = (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v);
        }
        px[i * 4 + 3] = (uint8_t)(a * 255.0f + 0.5f);
    }
    SDL_UpdateTexture(t, NULL, px, c->w * 4);
    SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
    free(px);
    return t;
}

/* ------------------------------------------------------------- colour */

typedef struct { float r, g, b; } RGBf;

static RGBf rgb_of(uint32_t hex)
{
    RGBf c = { (float)((hex >> 16) & 255), (float)((hex >> 8) & 255),
               (float)(hex & 255) };
    return c;
}

static RGBf shade(RGBf c, float f)
{
    if (f >= 0) {
        c.r += (255 - c.r) * f; c.g += (255 - c.g) * f; c.b += (255 - c.b) * f;
    } else {
        c.r *= 1 + f; c.g *= 1 + f; c.b *= 1 + f;
    }
    return c;
}

static RGBf mix(RGBf a, RGBf b, float t)
{
    RGBf c = { a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t,
               a.b + (b.b - a.b) * t };
    return c;
}

static double khash(double a, double b)
{
    double x = sin(a * 127.1 + b * 311.7) * 43758.5453;
    return x - floor(x);
}

SDL_Color knit_yarn(KnitYarn y)
{
    uint32_t h = YARN_HEX[y];
    SDL_Color c = { (Uint8)(h >> 16), (Uint8)(h >> 8), (Uint8)h, 255 };
    return c;
}

/* ------------------------------------------------------------- stitches */

typedef struct {
    uint32_t rgb;
    int      variant;
    float    ws, hs;
    Canvas   c;
} Sprite;

struct TextEntry {
    char*        text;      /* normalized glyph bytes */
    int          font;
    int          fg;
    SDL_Texture* tex;
    float        w, h;      /* texture size, logical */
    float        pad_x, pad_y;
    unsigned     used;
};

typedef struct {
    SDL_Texture* tex;
    float        w, h, pad_x, pad_y;
} Tex;

struct Knit {
    SDL_Renderer*    r;
    float            scale;
    Sprite           sprites[SPRITE_MAX];
    int              sprite_count;
    Tex              wool[YARN_COUNT];
    Tex              band[KNIT_BAND_COUNT];
    Tex              icon[KNIT_ICON_COUNT][YARN_COUNT];
    Tex              felt;
    Tex              frame_h[YARN_COUNT];   /* selvedge rows / columns */
    Tex              frame_v[YARN_COUNT];
    struct TextEntry text[TEXT_CACHE];
    unsigned         tick;
};

static float snap(const Knit* k, float g)
{
    float d = roundf(g * k->scale);
    if (d < 2) d = 2;
    return d / k->scale;
}

/* One leg of the V: an ellipse rotated outwards with a cross-ply gradient,
 * diagonal ply grooves (each with a lit edge), a soft sheen and a cast
 * shadow — fezcodex's drawLeg, rasterized with 4x4 supersampling. */
static void draw_leg(Canvas* cv, int side, RGBf base, float ws, float hs,
                     float s, float W, float H, double (*rnd)(int, void*),
                     void* ud)
{
    float rx  = ws * 0.29f, ry = hs * 0.66f;
    float cx  = W / 2 + side * ws * 0.215f;
    float cy  = H / 2 - hs * 0.12f;
    float ang = side * (0.4f + ((float)rnd(side + 4, ud) - 0.5f) * 0.12f);
    float ca  = cosf(ang), sa = sinf(ang);
    float blur  = fmaxf(s, ws * 0.27f);
    float offy  = fmaxf(0.6f * s, ws * 0.13f);
    float sigma = blur / 2;
    float rmin  = rx < ry ? rx : ry;

    /* Cast shadow: the ellipse, dropped and blurred. */
    for (int py = 0; py < cv->h; ++py)
        for (int px = 0; px < cv->w; ++px) {
            float dx = px + 0.5f - cx, dy = py + 0.5f - (cy + offy);
            float u =  dx * ca + dy * sa;
            float v = -dx * sa + dy * ca;
            float q = sqrtf((u / rx) * (u / rx) + (v / ry) * (v / ry));
            float d = (q - 1.0f) * rmin;
            float a = 0.55f * 0.5f * erfcf(d / (sigma * 1.41421f));
            if (a > 0.002f) over(cv->p + (py * cv->w + px) * 4, 0, 0, 0, a);
        }

    RGBf g0 = shade(base, -0.45f), g1 = shade(base, 0.18f),
         g3 = shade(base, -0.5f);
    RGBf groove = shade(base, -0.55f), litc = shade(base, 0.35f);
    RGBf white = { 255, 255, 255 };
    float step = ry / 3, lit = fmaxf(0.5f * s, ws * 0.1f);
    float dark_w = fmaxf(0.6f * s, ws * 0.07f);
    float lit_w  = fmaxf(0.5f * s, ws * 0.045f);
    const float norm = 1.48661f;            /* sqrt(1 + 1.1^2) */

    int x0 = (int)floorf(cx - ry - 1), x1 = (int)ceilf(cx + ry + 1);
    int y0 = (int)floorf(cy - ry - 1), y1 = (int)ceilf(cy + ry + 1);
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > cv->w) x1 = cv->w;
    if (y1 > cv->h) y1 = cv->h;
    for (int py = y0; py < y1; ++py)
        for (int px = x0; px < x1; ++px) {
            float ar = 0, ag = 0, ab = 0;
            int   n = 0;
            for (int sy = 0; sy < 4; ++sy)
                for (int sx = 0; sx < 4; ++sx) {
                    float dx = px + (sx + 0.5f) / 4 - cx;
                    float dy = py + (sy + 0.5f) / 4 - cy;
                    float u =  dx * ca + dy * sa;
                    float v = -dx * sa + dy * ca;
                    if ((u / rx) * (u / rx) + (v / ry) * (v / ry) > 1.0f)
                        continue;
                    float t = (u + rx) / (2 * rx);
                    RGBf c;
                    if (t < 0.42f)      c = mix(g0, g1, t / 0.42f);
                    else if (t < 0.62f) c = mix(g1, base, (t - 0.42f) / 0.2f);
                    else                c = mix(base, g3, (t - 0.62f) / 0.38f);
                    /* Grooves run along v = t0 - 1.1u, t0 every ry/3. */
                    float bv = v + u * 1.1f;
                    float kq = roundf((bv + ry * 1.4f) / step);
                    float t0 = -ry * 1.4f + kq * step;
                    if (t0 < ry * 1.4f && fabsf(bv - t0) / norm <= dark_w / 2)
                        c = mix(c, groove, 0.5f);
                    float kl = roundf((bv - lit + ry * 1.4f) / step);
                    float tl = -ry * 1.4f + kl * step;
                    if (tl < ry * 1.4f &&
                        fabsf(bv - lit - tl) / norm <= lit_w / 2)
                        c = mix(c, litc, 0.25f);
                    float sd = hypotf(u + rx * 0.3f, v + ry * 0.35f);
                    float sh = 0.16f * (1.0f - sd / ry);
                    if (sh > 0) c = mix(c, white, sh);
                    ar += c.r; ag += c.g; ab += c.b; ++n;
                }
            if (!n) continue;
            float cov = n / 16.0f;
            over(cv->p + (py * cv->w + px) * 4,
                 ar / n / 255.0f * cov, ag / n / 255.0f * cov,
                 ab / n / 255.0f * cov, cov);
        }
}

typedef struct { int variant; double seed; } RandCtx;

static double sprite_rand(int i, void* ud)
{
    RandCtx* c = ud;
    return khash(c->variant * 13.7 + i, c->seed * 0.37 + i * 0.7);
}

static const Sprite* sprite_get(Knit* k, uint32_t rgb, int variant,
                                float ws, float hs)
{
    for (int i = 0; i < k->sprite_count; ++i) {
        Sprite* sp = &k->sprites[i];
        if (sp->rgb == rgb && sp->variant == variant &&
            sp->ws == ws && sp->hs == hs)
            return sp;
    }
    if (k->sprite_count == SPRITE_MAX) {
        for (int i = 0; i < k->sprite_count; ++i) free(k->sprites[i].c.p);
        k->sprite_count = 0;
    }
    Sprite* sp = &k->sprites[k->sprite_count++];
    sp->rgb = rgb; sp->variant = variant; sp->ws = ws; sp->hs = hs;
    float W = ws * 1.6f, H = hs * 2.0f;
    sp->c = canvas_new((int)ceilf(W), (int)ceilf(H));

    /* The seed mirrors fezcodex: the char codes of the hex digits at 1,3,5. */
    char hex[8];
    snprintf(hex, sizeof hex, "#%06x", (unsigned)rgb);
    RandCtx rc = { variant, (double)(hex[1] + hex[3] + hex[5]) };
    RGBf base = shade(rgb_of(rgb), ((float)sprite_rand(1, &rc) - 0.5f) * 0.12f);
    draw_leg(&sp->c, -1, base, ws, hs, k->scale, W, H, sprite_rand, &rc);
    draw_leg(&sp->c,  1, base, ws, hs, k->scale, W, H, sprite_rand, &rc);
    return sp;
}

static void blit(Canvas* dst, const Canvas* src, int ox, int oy)
{
    for (int y = 0; y < src->h; ++y) {
        int dy = oy + y;
        if (dy < 0 || dy >= dst->h) continue;
        for (int x = 0; x < src->w; ++x) {
            int dx = ox + x;
            if (dx < 0 || dx >= dst->w) continue;
            const float* s = src->p + (y * src->w + x) * 4;
            if (s[3] <= 0) continue;
            over(dst->p + (dy * dst->w + dx) * 4, s[0], s[1], s[2], s[3]);
        }
    }
}

/* Knit a grid of cells (0 = no stitch, else 0xff000000 | rgb). A wrapped
 * axis makes the result tile seamlessly: its size snaps to whole device
 * pixels and stitches spilling over one edge are knitted again on the
 * other. An unwrapped axis gets padding for the spill. */
static Tex grid_render(Knit* k, const uint32_t* cells, int cols, int rows,
                       float gw, float gh, bool wrap_x, bool wrap_y)
{
    Tex out = { 0 };
    float s  = k->scale;
    float ws = roundf(gw * s), hs = roundf(gh * s);
    int pad_x = 0, pad_y = 0, cw, ch;
    if (wrap_x) { cw = (int)lroundf(cols * ws); ws = (float)cw / cols; }
    else        { pad_x = (int)ceilf(ws * 0.4f) + 1;
                  cw = (int)ceilf(cols * ws) + 2 * pad_x; }
    if (wrap_y) { ch = (int)lroundf(rows * hs); hs = (float)ch / rows; }
    else        { pad_y = (int)ceilf(hs * 0.7f) + 2;
                  ch = (int)ceilf(rows * hs) + 2 * pad_y; }
    Canvas cv = canvas_new(cw, ch);
    if (!cv.p) return out;

    /* The shadow between plies: each stitch's cell in a deep shade. */
    for (int r = 0; r < rows; ++r)
        for (int c = 0; c < cols; ++c) {
            uint32_t cell = cells[r * cols + c];
            if (!cell) continue;
            RGBf g = shade(rgb_of(cell & 0xffffff), -0.72f);
            int xa = pad_x + (int)(c * ws), xb = pad_x + (int)((c + 1) * ws);
            int ya = pad_y + (int)(r * hs), yb = pad_y + (int)((r + 1) * hs);
            for (int y = ya; y < yb && y < ch; ++y)
                for (int x = xa; x < xb && x < cw; ++x) {
                    float* d = cv.p + (y * cw + x) * 4;
                    d[0] = g.r / 255; d[1] = g.g / 255; d[2] = g.b / 255; d[3] = 1;
                }
        }

    int r_hi = wrap_y ? rows : rows - 1, r_lo = wrap_y ? -1 : 0;
    int c_lo = wrap_x ? -1 : 0,          c_hi = wrap_x ? cols : cols - 1;
    for (int r = r_hi; r >= r_lo; --r)
        for (int c = c_lo; c <= c_hi; ++c) {
            int rr = (r + rows) % rows, cc = (c + cols) % cols;
            uint32_t cell = cells[rr * cols + cc];
            if (!cell) continue;
            int variant = (int)floor(khash(rr, cc) * VARIANTS);
            const Sprite* sp = sprite_get(k, cell & 0xffffff, variant, ws, hs);
            float ox = pad_x + c * ws + ws / 2 - ws * 0.8f;
            float oy = pad_y + r * hs + hs / 2 - hs;
            blit(&cv, &sp->c, (int)floorf(ox + 0.5f), (int)floorf(oy + 0.5f));
        }

    out.tex   = canvas_upload(k->r, &cv);
    out.w     = cw / s;
    out.h     = ch / s;
    out.pad_x = pad_x / s;
    out.pad_y = pad_y / s;
    free(cv.p);
    return out;
}

static void tex_free(Tex* t)
{
    if (t->tex) SDL_DestroyTexture(t->tex);
    memset(t, 0, sizeof *t);
}

/* ------------------------------------------------------------- lifecycle */

Knit* knit_create(SDL_Renderer* r)
{
    Knit* k = calloc(1, sizeof *k);
    if (!k) return NULL;
    k->r = r;
    k->scale = 1.0f;
    return k;
}

static void knit_flush(Knit* k)
{
    for (int i = 0; i < k->sprite_count; ++i) free(k->sprites[i].c.p);
    k->sprite_count = 0;
    for (int i = 0; i < YARN_COUNT; ++i) tex_free(&k->wool[i]);
    for (int i = 0; i < KNIT_BAND_COUNT; ++i) tex_free(&k->band[i]);
    for (int i = 0; i < KNIT_ICON_COUNT; ++i)
        for (int j = 0; j < YARN_COUNT; ++j) tex_free(&k->icon[i][j]);
    tex_free(&k->felt);
    for (int i = 0; i < YARN_COUNT; ++i) {
        tex_free(&k->frame_h[i]);
        tex_free(&k->frame_v[i]);
    }
    for (int i = 0; i < TEXT_CACHE; ++i) {
        if (k->text[i].tex) SDL_DestroyTexture(k->text[i].tex);
        free(k->text[i].text);
    }
    memset(k->text, 0, sizeof k->text);
}

void knit_destroy(Knit* k)
{
    if (!k) return;
    knit_flush(k);
    free(k);
}

void knit_set_scale(Knit* k, float scale)
{
    if (!k || scale <= 0 || scale == k->scale) return;
    knit_flush(k);
    k->scale = scale;
}

/* ------------------------------------------------------------- drawing */

/* Clip to r ∩ the current clip. Returns false when nothing is visible. */
static bool clip_push(Knit* k, SDL_Rect r, SDL_Rect* saved, bool* had)
{
    *had = SDL_RenderIsClipEnabled(k->r);
    if (*had) SDL_RenderGetClipRect(k->r, saved);
    SDL_Rect c = r;
    if (*had && !SDL_IntersectRect(saved, &r, &c)) return false;
    SDL_RenderSetClipRect(k->r, &c);
    return true;
}

static void clip_pop(Knit* k, const SDL_Rect* saved, bool had)
{
    SDL_RenderSetClipRect(k->r, had ? saved : NULL);
}

/* Tile t over r on a grid anchored at the window origin. */
static void tile(Knit* k, const Tex* t, SDL_Rect r, bool tile_y, float y0)
{
    if (!t->tex || r.w <= 0 || r.h <= 0) return;
    SDL_Rect saved; bool had;
    if (clip_push(k, r, &saved, &had)) {
        float tx0 = floorf(r.x / t->w) * t->w;
        if (tile_y) {
            float ty0 = floorf(r.y / t->h) * t->h;
            for (float y = ty0; y < r.y + r.h; y += t->h)
                for (float x = tx0; x < r.x + r.w; x += t->w) {
                    SDL_FRect d = { x, y, t->w, t->h };
                    SDL_RenderCopyF(k->r, t->tex, NULL, &d);
                }
        } else {
            for (float x = tx0; x < r.x + r.w; x += t->w) {
                SDL_FRect d = { x, y0, t->w, t->h };
                SDL_RenderCopyF(k->r, t->tex, NULL, &d);
            }
        }
    }
    clip_pop(k, &saved, had);
}

void knit_fill(Knit* k, SDL_Rect r, KnitYarn y)
{
    if (!k || y < 0 || y >= YARN_COUNT) return;
    Tex* t = &k->wool[y];
    if (!t->tex) {
        static uint32_t cells[TILE_COLS * TILE_ROWS];
        for (int i = 0; i < TILE_COLS * TILE_ROWS; ++i)
            cells[i] = 0xff000000u | YARN_HEX[y];
        *t = grid_render(k, cells, TILE_COLS, TILE_ROWS, snap(k, GAUGE_W), snap(k, GAUGE_H),
                         true, true);
    }
    tile(k, t, r, true, 0);
}

/* ------------------------------------------------------------- bands */

static const char* const HEN[10] = {
    "...rr.....", "..rrrr....", "..WWWW....", "rWkWWW..WW", ".WWWWWWWWW",
    ".WWWWWWWWW", "..WWWHWWW.", "...WWWWW..", "....y.y...", "....y.y...",
};

static void band_spec(const Knit* k, KnitBand b, int* rows, float* gw, float* gh)
{
    switch (b) {
        case KNIT_BAND_PEERIE: *rows = 3;  *gw = GAUGE_W; *gh = GAUGE_H; break;
        case KNIT_BAND_ZIGZAG: *rows = 5;  *gw = FINE_W;  *gh = FINE_H;  break;
        case KNIT_BAND_HENS:   *rows = 14; *gw = FINE_W;  *gh = FINE_H;  break;
        default:               *rows = 1;  *gw = GAUGE_W; *gh = GAUGE_H; break;
    }
    *gw = snap(k, *gw);
    *gh = snap(k, *gh);
}

int knit_band_h(const Knit* k, KnitBand b)
{
    int rows; float gw, gh;
    band_spec(k, b, &rows, &gw, &gh);
    return (int)lroundf(rows * gh);
}

static void band_build(Knit* k, KnitBand b)
{
    int rows; float gw, gh;
    band_spec(k, b, &rows, &gw, &gh);
    uint32_t cells[BAND_COLS * 14];
    #define Y(n) (0xff000000u | YARN_HEX[n])
    #define SET(c_, r_, v_) cells[(r_) * BAND_COLS + (c_)] = (v_)
    for (int i = 0; i < BAND_COLS * rows; ++i) cells[i] = 0;
    switch (b) {
        case KNIT_BAND_PEERIE:
            for (int c = 0; c < BAND_COLS; ++c) {
                SET(c, 0, Y(YARN_GOLD));
                SET(c, 1, c % 6 == 3 ? Y(YARN_POPPY) : Y(YARN_BARK));
                SET(c, 2, Y(YARN_GOLD));
            }
            break;
        case KNIT_BAND_ZIGZAG: {
            static const int wave[6] = { 0, 1, 2, 3, 2, 1 };
            for (int i = 0; i < BAND_COLS * rows; ++i) cells[i] = Y(YARN_PEAT);
            for (int c = 0; c < BAND_COLS; ++c) {
                int w = wave[c % 6];
                SET(c, 4 - w, Y(YARN_GOLD));
                if (w > 0) SET(c, 4 - w + 1, Y(YARN_GOLD));
            }
            break;
        }
        case KNIT_BAND_HENS: {
            static const KnitYarn bodies[4] = {
                YARN_SNOW, YARN_PEACH, YARN_RUST, YARN_WHEAT };
            for (int i = 0; i < BAND_COLS * rows; ++i) cells[i] = Y(YARN_WALNUT);
            for (int i = 0, x = 3; x < BAND_COLS; ++i, x += 15) {
                KnitYarn body = bodies[i % 4];
                for (int r = 0; r < 10; ++r)
                    for (int c = 0; HEN[r][c]; ++c) {
                        uint32_t v = 0;
                        switch (HEN[r][c]) {
                            case 'r': v = Y(YARN_POPPY); break;
                            case 'W': v = Y(body); break;
                            case 'k': v = Y(YARN_JET); break;
                            case 'y': v = Y(YARN_GOLD); break;
                            case 'H': v = Y(body == YARN_SNOW ? YARN_OAT
                                                              : YARN_CREAM); break;
                        }
                        if (v && x + c < BAND_COLS) SET(x + c, 2 + r, v);
                    }
            }
            break;
        }
        default: break;
    }
    #undef SET
    #undef Y
    k->band[b] = grid_render(k, cells, BAND_COLS, rows, gw, gh, true, false);
}

int knit_band(Knit* k, SDL_Rect r, KnitBand b)
{
    if (!k || b < 0 || b >= KNIT_BAND_COUNT) return 0;
    if (!k->band[b].tex) band_build(k, b);
    Tex* t = &k->band[b];
    SDL_Rect area = { r.x, (int)floorf(r.y - t->pad_y), r.w, (int)ceilf(t->h) + 1 };
    tile(k, t, area, false, r.y - t->pad_y);
    return knit_band_h(k, b);
}

/* ------------------------------------------------------------- frame */

void knit_frame(Knit* k, int w, int h, KnitYarn yarn)
{
    if (!k || w <= 0 || h <= 0) return;
    Tex* th = &k->frame_h[yarn];
    Tex* tv = &k->frame_v[yarn];
    if (!th->tex || !tv->tex) {
        /* One stitch wide, a poppy peerie dot every sixth stitch. */
        uint32_t cells[BAND_COLS];
        for (int i = 0; i < BAND_COLS; ++i)
            cells[i] = 0xff000000u |
                       (i % 6 == 3 ? YARN_HEX[YARN_POPPY] : YARN_HEX[yarn]);
        float gw = snap(k, FINE_W), gh = snap(k, FINE_H);
        tex_free(th);
        tex_free(tv);
        *th = grid_render(k, cells, BAND_COLS, 1, gw, gh, true, false);
        /* Down the sides the stitches stack as a column. */
        *tv = grid_render(k, cells, 1, BAND_COLS, gw, gh, false, true);
    }
    if (!th->tex || !tv->tex) return;

    float sw = snap(k, FINE_W), sh = snap(k, FINE_H);
    SDL_Rect saved; bool had;
    SDL_Rect all = { 0, 0, w, h };
    if (clip_push(k, all, &saved, &had)) {
        /* Sides first, so the top and bottom rows lie over their ends. */
        float xs[2] = { 0, w - sw };
        for (int s = 0; s < 2; ++s)
            for (float y = 0; y < h; y += tv->h) {
                SDL_FRect d = { xs[s] - tv->pad_x, y, tv->w, tv->h };
                SDL_RenderCopyF(k->r, tv->tex, NULL, &d);
            }
        float ys[2] = { 0, h - sh };
        for (int s = 0; s < 2; ++s)
            for (float x = 0; x < w; x += th->w) {
                SDL_FRect d = { x, ys[s] - th->pad_y, th->w, th->h };
                SDL_RenderCopyF(k->r, th->tex, NULL, &d);
            }
    }
    clip_pop(k, &saved, had);
}

/* ------------------------------------------------------------- text */

static int font_rows(int f) { return f == KNIT_FONT_BIG ? 7 : 5; }

static const char* glyph_row(int f, unsigned char ch, int row)
{
    if (ch >= 128) return NULL;
    return f == KNIT_FONT_BIG ? KNIT_BIG_GLYPHS[ch][row]
                              : KNIT_SMALL_GLYPHS[ch][row];
}

static void font_gauge(const Knit* k, int f, float* gw, float* gh)
{
    if (f == KNIT_FONT_TIGHT) { *gw = TIGHT_W; *gh = TIGHT_H; }
    else                      { *gw = FINE_W;  *gh = FINE_H; }
    *gw = snap(k, *gw);
    *gh = snap(k, *gh);
}

static uint32_t utf8_next(const unsigned char** p, const unsigned char* end)
{
    const unsigned char* s = *p;
    uint32_t c = *s++;
    int extra = 0;
    if      (c >= 0xf0) { c &= 0x07; extra = 3; }
    else if (c >= 0xe0) { c &= 0x0f; extra = 2; }
    else if (c >= 0xc0) { c &= 0x1f; extra = 1; }
    else if (c >= 0x80) { *p = s; return 0xfffd; }
    while (extra-- > 0 && s < end && (*s & 0xc0) == 0x80)
        c = (c << 6) | (*s++ & 0x3f);
    *p = s;
    return c;
}

/* Latin letters with marks, folded to their base capital (0 = unknown). */
static const char* fold_latin(uint32_t c)
{
    if (c >= 0xc0 && c <= 0xc5) return "A";
    if (c >= 0xe0 && c <= 0xe5) return "A";
    if (c == 0xc7 || c == 0xe7) return "C";
    if (c >= 0xc8 && c <= 0xcb) return "E";
    if (c >= 0xe8 && c <= 0xeb) return "E";
    if (c >= 0xcc && c <= 0xcf) return "I";
    if (c >= 0xec && c <= 0xef) return "I";
    if (c == 0xd0 || c == 0xf0) return "D";
    if (c == 0xd1 || c == 0xf1) return "N";
    if ((c >= 0xd2 && c <= 0xd6) || c == 0xd8) return "O";
    if ((c >= 0xf2 && c <= 0xf6) || c == 0xf8) return "O";
    if (c >= 0xd9 && c <= 0xdc) return "U";
    if (c >= 0xf9 && c <= 0xfc) return "U";
    if (c == 0xdd || c == 0xfd || c == 0xff) return "Y";
    if (c == 0xdf) return "SS";
    if (c == 0xd7) return "X";
    if (c >= 0x100 && c <= 0x17f) {
        /* Latin Extended-A, grouped by base letter. */
        static const struct { uint16_t lo, hi; const char* to; } R[] = {
            { 0x100, 0x105, "A" }, { 0x106, 0x10d, "C" }, { 0x10e, 0x111, "D" },
            { 0x112, 0x11b, "E" }, { 0x11c, 0x123, "G" }, { 0x124, 0x127, "H" },
            { 0x128, 0x131, "I" }, { 0x134, 0x135, "J" }, { 0x136, 0x138, "K" },
            { 0x139, 0x142, "L" }, { 0x143, 0x14b, "N" }, { 0x14c, 0x151, "O" },
            { 0x154, 0x159, "R" }, { 0x15a, 0x161, "S" }, { 0x162, 0x167, "T" },
            { 0x168, 0x173, "U" }, { 0x174, 0x175, "W" }, { 0x176, 0x178, "Y" },
            { 0x179, 0x17e, "Z" },
        };
        for (size_t i = 0; i < sizeof R / sizeof R[0]; ++i)
            if (c >= R[i].lo && c <= R[i].hi) return R[i].to;
    }
    switch (c) {
        case 0xa0:   return " ";
        case 0xb7:   return "\x01";
        case 0x2022: return "\x01";
        case 0x2192: return "\x02";
        case 0x2190: return "\x03";
        case 0x2197: return "\x04";
        case 0x2665: case 0x2764: return "\x05";
        case 0x2013: case 0x2014: return "-";
        case 0x2018: case 0x2019: return "'";
        case 0x201c: case 0x201d: return "\"";
        case 0x2026: return "...";
        case 0x203a: return ">";
        case 0x2039: return "<";
        case 0xd7:   return "X";
    }
    return NULL;
}

/* Normalize UTF-8 into glyph bytes. Returns the length, or -1 if some
 * character has no stitch form. */
static int knit_norm(const char* s, size_t len, char* out, int cap)
{
    const unsigned char* p = (const unsigned char*)s;
    const unsigned char* end = p + len;
    int n = 0;
    bool last_space = false;
    while (p < end) {
        uint32_t c = utf8_next(&p, end);
        const char* rep = NULL;
        char one[2] = { 0, 0 };
        if (c >= 0x300 && c <= 0x36f) continue;          /* combining mark */
        if (c < 128) {
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                if (last_space || n == 0) continue;
                rep = " ";
            } else {
                char u = (char)(c >= 'a' && c <= 'z' ? c - 32 : c);
                if      (u == '{') u = '(';
                else if (u == '}') u = ')';
                else if (u == '~') u = '-';
                else if (u == '`') u = '\'';
                if (!KNIT_SMALL_GLYPHS[(unsigned char)u][0]) continue;
                one[0] = u;
                rep = one;
            }
        } else {
            rep = fold_latin(c);
            if (!rep) return -1;
        }
        for (const char* q = rep; *q; ++q) {
            if (n >= cap - 1) return n;
            out[n++] = *q;
        }
        last_space = rep[0] == ' ';
    }
    while (n > 0 && out[n - 1] == ' ') --n;
    out[n] = 0;
    return n;
}

static int glyph_cols(int f, unsigned char ch)
{
    const char* r0 = glyph_row(f, ch, 0);
    return r0 ? (int)strlen(r0) : 0;
}

static int text_cols(int f, const char* g, int n)
{
    if (n <= 0) return 0;
    int cols = 0;
    for (int i = 0; i < n; ++i) cols += glyph_cols(f, (unsigned char)g[i]) + 1;
    return cols - 1;
}

bool knit_text_ok(const char* s, size_t len)
{
    char tmp[512];
    return knit_norm(s, len, tmp, sizeof tmp) >= 0;
}

int knit_text_h(const Knit* k, KnitFont f)
{
    float gw, gh;
    font_gauge(k, f, &gw, &gh);
    return (int)lroundf(font_rows(f) * gh);
}

int knit_text_w(const Knit* k, const char* s, size_t len, KnitFont f)
{
    char g[512];
    int n = knit_norm(s, len, g, sizeof g);
    if (n < 0) return -1;
    float gw, gh;
    font_gauge(k, f, &gw, &gh);
    return (int)lroundf(text_cols(f, g, n) * gw);
}

static struct TextEntry* text_get(Knit* k, const char* g, KnitFont f, KnitYarn fg)
{
    struct TextEntry* free_slot = NULL;
    struct TextEntry* oldest = &k->text[0];
    for (int i = 0; i < TEXT_CACHE; ++i) {
        struct TextEntry* e = &k->text[i];
        if (!e->text) { if (!free_slot) free_slot = e; continue; }
        if (e->font == (int)f && e->fg == (int)fg && !strcmp(e->text, g)) {
            e->used = ++k->tick;
            return e;
        }
        if (e->used < oldest->used) oldest = e;
    }
    struct TextEntry* e = free_slot ? free_slot : oldest;
    if (e->tex) SDL_DestroyTexture(e->tex);
    free(e->text);
    memset(e, 0, sizeof *e);

    int n = (int)strlen(g);
    int cols = text_cols(f, g, n), rows = font_rows(f);
    if (cols <= 0) cols = 1;
    uint32_t* cells = calloc((size_t)cols * rows, sizeof *cells);
    if (!cells) return NULL;
    uint32_t yarn = 0xff000000u | YARN_HEX[fg];
    int x = 0;
    for (int i = 0; i < n; ++i) {
        unsigned char ch = (unsigned char)g[i];
        int gwid = glyph_cols(f, ch);
        for (int r = 0; r < rows; ++r) {
            const char* row = glyph_row(f, ch, r);
            for (int c = 0; row && c < gwid; ++c)
                if (row[c] == '1' && x + c < cols) cells[r * cols + x + c] = yarn;
        }
        x += gwid + 1;
    }
    float gw, gh;
    font_gauge(k, f, &gw, &gh);
    Tex t = grid_render(k, cells, cols, rows, gw, gh, false, false);
    free(cells);
    e->text  = strdup(g);
    e->font  = f;
    e->fg    = fg;
    e->tex   = t.tex;
    e->w     = t.w;     e->h     = t.h;
    e->pad_x = t.pad_x; e->pad_y = t.pad_y;
    e->used  = ++k->tick;
    return e;
}

int knit_text(Knit* k, const char* s, size_t len, KnitFont f,
              int x, int y, KnitYarn fg, int max_w)
{
    if (!k) return -1;
    char g[512];
    int n = knit_norm(s, len, g, sizeof g - 4);
    if (n < 0) return -1;
    if (n == 0) return 0;
    float gw, gh;
    font_gauge(k, f, &gw, &gh);
    if (max_w > 0 && text_cols(f, g, n) * gw > max_w) {
        /* Elide: shorten until the text plus "..." fits. */
        while (n > 0) {
            --n;
            while (n > 0 && g[n - 1] == ' ') --n;
            memcpy(g + n, "...", 4);
            if (text_cols(f, g, n + 3) * gw <= max_w) break;
        }
        if (n == 0 && text_cols(f, "...", 3) * gw > max_w) return 0;
        n += 3;
        g[n] = 0;
    }
    struct TextEntry* e = text_get(k, g, f, fg);
    if (!e || !e->tex) return -1;
    SDL_FRect d = { x - e->pad_x, y - e->pad_y, e->w, e->h };
    SDL_RenderCopyF(k->r, e->tex, NULL, &d);
    return (int)lroundf(text_cols(f, g, n) * gw);
}

/* ------------------------------------------------------------- icons */

static const char* const ICONS[KNIT_ICON_COUNT][5] = {
    [KNIT_ICON_FILE]        = { "11100", "10110", "10011", "10001", "11111" },
    [KNIT_ICON_FOLDER]      = { "11000", "11111", "10001", "10001", "11111" },
    [KNIT_ICON_FOLDER_OPEN] = { "11000", "10111", "11111", "10001", "11111" },
    [KNIT_ICON_CARET_RIGHT] = { "100",   "110",   "111",   "110",   "100"   },
    [KNIT_ICON_CARET_DOWN]  = { "00000", "11111", "01110", "00100", "00000" },
};

int knit_icon_w(const Knit* k, KnitIcon i)
{
    return (int)lroundf((float)strlen(ICONS[i][0]) * snap(k, FINE_W));
}

int knit_icon_h(const Knit* k, KnitIcon i)
{
    (void)i;
    return (int)lroundf(5 * snap(k, FINE_H));
}

void knit_icon(Knit* k, KnitIcon i, int x, int y, KnitYarn fg)
{
    if (!k || i < 0 || i >= KNIT_ICON_COUNT) return;
    Tex* t = &k->icon[i][fg];
    if (!t->tex) {
        int cols = (int)strlen(ICONS[i][0]);
        uint32_t cells[5 * 5] = { 0 };
        for (int r = 0; r < 5; ++r)
            for (int c = 0; c < cols; ++c)
                if (ICONS[i][r][c] == '1')
                    cells[r * cols + c] = 0xff000000u | YARN_HEX[fg];
        *t = grid_render(k, cells, cols, 5, snap(k, FINE_W), snap(k, FINE_H), false, false);
    }
    if (!t->tex) return;
    SDL_FRect d = { x - t->pad_x, y - t->pad_y, t->w, t->h };
    SDL_RenderCopyF(k->r, t->tex, NULL, &d);
}

/* ------------------------------------------------------------- felt */

static void fill_round(SDL_Renderer* r, SDL_Rect b, int rad)
{
    for (int y = 0; y < b.h; ++y) {
        int inset = 0;
        int dy = y < rad ? rad - y : (y >= b.h - rad ? y - (b.h - rad - 1) : 0);
        if (dy > 0) {
            float dx = rad - sqrtf((float)(rad * rad - dy * dy));
            inset = (int)ceilf(dx - 0.25f);
        }
        SDL_Rect row = { b.x + inset, b.y + y, b.w - 2 * inset, 1 };
        SDL_RenderFillRect(r, &row);
    }
}

static void felt_build(Knit* k)
{
    /* Speckled felt: a fine brown dot grid, scattered pale fibres and a
     * little per-pixel noise, at device resolution. */
    int n = (int)lroundf(96 * k->scale);
    Canvas cv = canvas_new(n, n);
    if (!cv.p) return;
    int dot = (int)lroundf(3 * k->scale); if (dot < 2) dot = 2;
    for (int y = 0; y < n; ++y)
        for (int x = 0; x < n; ++x) {
            float* d = cv.p + (y * n + x) * 4;
            double h = khash(x * 0.913, y * 1.731);
            if (x % dot == 0 && y % dot == 0)
                over(d, 90 / 255.f * 0.10f, 64 / 255.f * 0.10f, 40 / 255.f * 0.10f, 0.10f);
            if (h > 0.985)
                over(d, 0.45f, 0.45f, 0.45f, 0.45f);
            else if (h < 0.05)
                over(d, 90 / 255.f * 0.07f, 64 / 255.f * 0.07f, 40 / 255.f * 0.07f, 0.07f);
        }
    k->felt.tex = canvas_upload(k->r, &cv);
    k->felt.w = k->felt.h = n / k->scale;
    free(cv.p);
}

void knit_running_stitch(Knit* k, SDL_Rect r, int inset, SDL_Color c)
{
    if (!k) return;
    SDL_SetRenderDrawBlendMode(k->r, SDL_BLENDMODE_BLEND);
    SDL_SetRenderDrawColor(k->r, c.r, c.g, c.b, c.a);
    int x0 = r.x + inset, y0 = r.y + inset;
    int x1 = r.x + r.w - inset, y1 = r.y + r.h - inset;
    const int dash = 6, gap = 4, th = 2;
    for (int x = x0 + 2; x < x1 - 2; x += dash + gap) {
        int w = (x + dash > x1 - 2) ? x1 - 2 - x : dash;
        SDL_Rect a = { x, y0, w, th }, b = { x, y1 - th, w, th };
        SDL_RenderFillRect(k->r, &a);
        SDL_RenderFillRect(k->r, &b);
    }
    for (int y = y0 + 2; y < y1 - 2; y += dash + gap) {
        int h = (y + dash > y1 - 2) ? y1 - 2 - y : dash;
        SDL_Rect a = { x0, y, th, h }, b = { x1 - th, y, th, h };
        SDL_RenderFillRect(k->r, &a);
        SDL_RenderFillRect(k->r, &b);
    }
}

void knit_felt(Knit* k, SDL_Rect r)
{
    if (!k || r.w <= 0 || r.h <= 0) return;
    SDL_SetRenderDrawBlendMode(k->r, SDL_BLENDMODE_BLEND);
    /* Cast shadow onto the wool. */
    static const struct { int dx, dy, grow, a; } SH[] = {
        { 0, 4, 2, 28 }, { 0, 3, 1, 40 }, { 0, 2, 0, 60 },
    };
    for (size_t i = 0; i < sizeof SH / sizeof SH[0]; ++i) {
        SDL_SetRenderDrawColor(k->r, 0, 0, 0, (Uint8)SH[i].a);
        SDL_Rect s = { r.x + SH[i].dx - SH[i].grow, r.y + SH[i].dy - SH[i].grow,
                       r.w + 2 * SH[i].grow, r.h + 2 * SH[i].grow };
        fill_round(k->r, s, 7);
    }
    SDL_SetRenderDrawColor(k->r, (FELT_RGB >> 16) & 255, (FELT_RGB >> 8) & 255,
                           FELT_RGB & 255, 255);
    fill_round(k->r, r, 6);
    if (!k->felt.tex) felt_build(k);
    SDL_Rect in = { r.x + 2, r.y + 2, r.w - 4, r.h - 4 };
    tile(k, &k->felt, in, true, 0);
    /* Inset highlight along the top edge. */
    SDL_SetRenderDrawColor(k->r, 255, 255, 255, 64);
    SDL_Rect hl = { r.x + 6, r.y + 1, r.w - 12, 1 };
    SDL_RenderFillRect(k->r, &hl);
    SDL_Color thread = { (THREAD_RGB >> 16) & 255, (THREAD_RGB >> 8) & 255,
                         THREAD_RGB & 255, 255 };
    knit_running_stitch(k, r, 7, thread);
}
