#include "font.h"

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_OUTLINE_H
#include <hb.h>
#include <hb-ft.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct GlyphSlot {
    unsigned int      glyph_index;
    SDL_Texture*      tex;
    int               w, h;
    int               bearing_x, bearing_y;
    struct GlyphSlot* next;
} GlyphSlot;

#define GLYPH_BUCKETS 256

struct Font {
    SDL_Renderer* renderer;
    FT_Library    ft_lib;
    FT_Face       ft_face;
    hb_font_t*    hb_font;
    hb_buffer_t*  hb_buf;
    int           pixel_size;
    int           ascent;
    int           descent;
    int           line_height;
    FontStyle     style;
    GlyphSlot*    cache[GLYPH_BUCKETS];
    Font*         fallback;
    unsigned int  gi_backtick;   /* glyph index for U+0060 (backtick) */
    hb_position_t bt_advance;    /* its natural advance, cached on load */
};

/* ---------- UTF-8 codepoint decode ------------------------------------- */
static uint32_t utf8_decode(const char* s, size_t len, size_t* adv)
{
    if (len == 0) { *adv = 0; return 0; }
    unsigned char c = (unsigned char)s[0];
    if (c < 0x80) { *adv = 1; return c; }
    if ((c & 0xE0) == 0xC0 && len >= 2) {
        *adv = 2;
        return ((uint32_t)(c & 0x1F) << 6) |
               ((unsigned char)s[1] & 0x3F);
    }
    if ((c & 0xF0) == 0xE0 && len >= 3) {
        *adv = 3;
        return ((uint32_t)(c & 0x0F) << 12) |
               ((uint32_t)((unsigned char)s[1] & 0x3F) << 6) |
               ((unsigned char)s[2] & 0x3F);
    }
    if ((c & 0xF8) == 0xF0 && len >= 4) {
        *adv = 4;
        return ((uint32_t)(c & 0x07) << 18) |
               ((uint32_t)((unsigned char)s[1] & 0x3F) << 12) |
               ((uint32_t)((unsigned char)s[2] & 0x3F) << 6) |
               ((unsigned char)s[3] & 0x3F);
    }
    *adv = 1; return 0xFFFD;
}

/* ---------- glyph cache (per-Font) ------------------------------------- */
static GlyphSlot* glyph_load(Font* f, unsigned int gi)
{
    GlyphSlot** bucket = &f->cache[gi & (GLYPH_BUCKETS - 1)];
    for (GlyphSlot* s = *bucket; s; s = s->next)
        if (s->glyph_index == gi) return s;

    /* FT_LOAD_TARGET_LIGHT: less aggressive vertical hinting, preserves
     * horizontal proportions. Renders body text at 14–18 px noticeably
     * smoother than the default "normal" target, especially with regard
     * to letter spacing and the perceived weight of strokes. */
    int bold = (f->style & FONT_STYLE_BOLD) != 0;
    int load_flags = FT_LOAD_TARGET_LIGHT;
    if (!bold) load_flags |= FT_LOAD_RENDER;
    if (FT_Load_Glyph(f->ft_face, gi, load_flags) != 0) return NULL;

    if (bold && f->ft_face->glyph->format == FT_GLYPH_FORMAT_OUTLINE) {
        FT_Outline_Embolden(&f->ft_face->glyph->outline, 64);
        /* Render with the same target so bold + regular metrics agree. */
        if (FT_Render_Glyph(f->ft_face->glyph, FT_RENDER_MODE_LIGHT) != 0)
            return NULL;
    }

    FT_GlyphSlot g = f->ft_face->glyph;
    GlyphSlot* slot = calloc(1, sizeof *slot);
    slot->glyph_index = gi;
    slot->w           = (int)g->bitmap.width;
    slot->h           = (int)g->bitmap.rows;
    slot->bearing_x   = g->bitmap_left;
    slot->bearing_y   = g->bitmap_top;

    if (slot->w > 0 && slot->h > 0) {
        SDL_Surface* surf = SDL_CreateRGBSurfaceWithFormat(
            0, slot->w, slot->h, 32, SDL_PIXELFORMAT_ARGB8888);
        if (surf) {
            uint32_t* px = (uint32_t*)surf->pixels;
            const unsigned char* src = g->bitmap.buffer;
            const int pitch = g->bitmap.pitch;        /* may be negative */
            const int dst_pitch = surf->pitch / 4;

            /* MS Gothic and other CJK fonts often deliver embedded MONO
             * (1bpp, MSB-packed) bitmaps. Older code assumed GRAY (8bpp)
             * and read 8 bits as a single alpha byte — visual garbage. */
            switch (g->bitmap.pixel_mode) {
                case FT_PIXEL_MODE_GRAY:
                    for (int y = 0; y < slot->h; ++y)
                        for (int x = 0; x < slot->w; ++x) {
                            uint32_t a = src[y * pitch + x];
                            px[y * dst_pitch + x] = (a << 24) | 0x00FFFFFFu;
                        }
                    break;
                case FT_PIXEL_MODE_MONO:
                    for (int y = 0; y < slot->h; ++y)
                        for (int x = 0; x < slot->w; ++x) {
                            int byte = src[y * pitch + (x >> 3)];
                            int bit  = (byte >> (7 - (x & 7))) & 1;
                            uint32_t a = bit ? 255u : 0u;
                            px[y * dst_pitch + x] = (a << 24) | 0x00FFFFFFu;
                        }
                    break;
                case FT_PIXEL_MODE_BGRA:
                    /* color bitmap (e.g. emoji); already premultiplied BGRA */
                    for (int y = 0; y < slot->h; ++y)
                        memcpy(px + y * dst_pitch,
                               src + y * pitch, (size_t)slot->w * 4);
                    break;
                default:
                    fprintf(stderr,
                        "[glyph_load] gi=%u unsupported pixel_mode=%d\n",
                        gi, g->bitmap.pixel_mode);
                    break;
            }

            slot->tex = SDL_CreateTextureFromSurface(f->renderer, surf);
            SDL_FreeSurface(surf);
            if (slot->tex) SDL_SetTextureBlendMode(slot->tex, SDL_BLENDMODE_BLEND);
        }
    }
    slot->next = *bucket;
    *bucket    = slot;
    return slot;
}

/* ---------- create / destroy / fallback chain -------------------------- */
Font* font_create(SDL_Renderer* r, const char* ttf_path,
                  int pixel_size, FontStyle style)
{
    Font* f = calloc(1, sizeof *f);
    f->renderer   = r;
    f->pixel_size = pixel_size;
    f->style      = style;

    if (FT_Init_FreeType(&f->ft_lib) != 0) {
        fprintf(stderr, "FT_Init_FreeType failed\n");
        free(f); return NULL;
    }
    if (FT_New_Face(f->ft_lib, ttf_path, 0, &f->ft_face) != 0) {
        fprintf(stderr, "FT_New_Face failed for %s\n", ttf_path);
        FT_Done_FreeType(f->ft_lib); free(f); return NULL;
    }
    if (FT_Set_Pixel_Sizes(f->ft_face, 0, pixel_size) != 0) {
        fprintf(stderr, "FT_Set_Pixel_Sizes failed\n");
        FT_Done_Face(f->ft_face); FT_Done_FreeType(f->ft_lib);
        free(f); return NULL;
    }
    if (style & FONT_STYLE_ITALIC) {
        FT_Matrix m = {
            0x10000, (FT_Fixed)(0.21 * 0x10000),
            0,       0x10000
        };
        FT_Set_Transform(f->ft_face, &m, NULL);
    }

    f->ascent      = f->ft_face->size->metrics.ascender    >> 6;
    f->descent     = -(f->ft_face->size->metrics.descender >> 6);
    f->line_height = f->ft_face->size->metrics.height      >> 6;

    f->hb_font = hb_ft_font_create_referenced(f->ft_face);
    f->hb_buf  = hb_buffer_create();

    /* Cache the backtick glyph index and its natural advance so shape_run
     * can restore a zero advance if the font/shaper combines it as a diacritic. */
    f->gi_backtick = FT_Get_Char_Index(f->ft_face, 0x0060);
    f->bt_advance  = 0;
    if (f->gi_backtick &&
        FT_Load_Glyph(f->ft_face, f->gi_backtick, FT_LOAD_TARGET_LIGHT) == 0)
    {
        f->bt_advance = (hb_position_t)f->ft_face->glyph->advance.x;
    }

    return f;
}

void font_add_fallback(Font* f, const char* ttf_path)
{
    if (!f) return;
    while (f->fallback) f = f->fallback;
    f->fallback = font_create(f->renderer, ttf_path, f->pixel_size,
                              FONT_STYLE_REGULAR);
    if (!f->fallback)
        fprintf(stderr, "font_add_fallback: failed to load %s\n", ttf_path);
}

void font_destroy(Font* f)
{
    if (!f) return;
    Font* fb = f->fallback;
    for (int i = 0; i < GLYPH_BUCKETS; ++i) {
        for (GlyphSlot* s = f->cache[i]; s; ) {
            GlyphSlot* n = s->next;
            if (s->tex) SDL_DestroyTexture(s->tex);
            free(s);
            s = n;
        }
    }
    if (f->hb_buf)  hb_buffer_destroy(f->hb_buf);
    if (f->hb_font) hb_font_destroy(f->hb_font);
    if (f->ft_face) FT_Done_Face(f->ft_face);
    if (f->ft_lib)  FT_Done_FreeType(f->ft_lib);
    free(f);
    font_destroy(fb);
}

/* ---------- shape + draw / measure (single font, no fallback) ---------- */
static int shape_run(Font* f, const char* utf8, size_t len,
                     int x, int y_baseline, SDL_Color color, int draw)
{
    hb_buffer_clear_contents(f->hb_buf);
    hb_buffer_add_utf8(f->hb_buf, utf8, (int)len, 0, (int)len);
    hb_buffer_guess_segment_properties(f->hb_buf);
    hb_shape(f->hb_font, f->hb_buf, NULL, 0);

    unsigned int n = 0;
    hb_glyph_info_t*     infos = hb_buffer_get_glyph_infos(f->hb_buf, &n);
    hb_glyph_position_t* poss  = hb_buffer_get_glyph_positions(f->hb_buf, &n);

    /* Some fonts (or GPOS rules) give U+0060 (backtick) zero advance so it
     * combines diacritically onto the next glyph.  Restore its natural advance
     * so it never overlaps with the following character. */
    if (f->gi_backtick && f->bt_advance > 0) {
        for (unsigned int k = 0; k < n; ++k) {
            if (infos[k].codepoint == f->gi_backtick && poss[k].x_advance == 0)
                poss[k].x_advance = f->bt_advance;
        }
    }

    int pen_x = x;
    int pen_y = y_baseline;
    for (unsigned int i = 0; i < n; ++i) {
        if (draw) {
            GlyphSlot* g = glyph_load(f, infos[i].codepoint);
            if (g && g->tex) {
                int x_off = poss[i].x_offset >> 6;
                int y_off = poss[i].y_offset >> 6;
                SDL_Rect dst = {
                    pen_x + x_off + g->bearing_x,
                    pen_y - y_off - g->bearing_y,
                    g->w, g->h
                };
                SDL_SetTextureColorMod(g->tex, color.r, color.g, color.b);
                SDL_SetTextureAlphaMod(g->tex, color.a);
                SDL_RenderCopy(f->renderer, g->tex, NULL, &dst);
            }
        }
        pen_x += poss[i].x_advance >> 6;
        pen_y += poss[i].y_advance >> 6;
    }
    return pen_x - x;
}

/* For each codepoint, pick the first font in the chain whose FreeType face
 * has a glyph for it (FT_Get_Char_Index != 0). Groups consecutive codepoints
 * that map to the same font into a run, shapes that run with HarfBuzz. */
static Font* font_for_cp(Font* f, uint32_t cp)
{
    Font* c = f;
    while (c) {
        if (FT_Get_Char_Index(c->ft_face, cp) != 0) return c;
        c = c->fallback;
    }
    return f;  /* nothing has it; let primary tofu */
}

static int run_with_fallback(Font* f, const char* utf8, size_t len,
                             int x, int y_baseline, SDL_Color color, int draw)
{
    if (!f || !utf8 || len == 0) return 0;

    int x_start = x;
    size_t i = 0;
    while (i < len) {
        size_t adv;
        uint32_t cp = utf8_decode(utf8 + i, len - i, &adv);
        Font* chosen = font_for_cp(f, cp);

        size_t run_start = i;
        size_t run_end   = i + adv;
        while (run_end < len) {
            size_t adv2;
            uint32_t cp2 = utf8_decode(utf8 + run_end, len - run_end, &adv2);
            if (font_for_cp(f, cp2) != chosen) break;
            run_end += adv2;
        }
        x += shape_run(chosen, utf8 + run_start, run_end - run_start,
                       x, y_baseline, color, draw);
        i = run_end;
    }
    return x - x_start;
}

int font_draw_line(Font* f, const char* utf8, size_t len,
                   int x, int y, SDL_Color color)
{
    return run_with_fallback(f, utf8, len, x, y, color, 1);
}

int font_measure(Font* f, const char* utf8, size_t len)
{
    SDL_Color c = {0,0,0,0};
    return run_with_fallback(f, utf8, len, 0, 0, c, 0);
}

int font_line_height(const Font* f) { return f ? f->line_height : 0; }
int font_ascent     (const Font* f) { return f ? f->ascent      : 0; }
int font_descent    (const Font* f) { return f ? f->descent     : 0; }

void font_diag(Font* f)
{
    static const struct { uint32_t cp; const char* label; } Q[] = {
        { 0x0041, "A     " },
        { 0x4F60, "你    " },
        { 0x597D, "好    " },
        { 0x4E16, "世    " },
        { 0x754C, "界    " },
        { 0x3053, "こ    " },
        { 0x3093, "ん    " },
        { 0x65E5, "日    " },
        { 0x6F22, "漢    " },
        { 0x2605, "star  " },
        { 0x2713, "check " },
    };
    fprintf(stderr, "[font_diag] chain coverage (gi=0 means missing):\n");
    for (size_t i = 0; i < sizeof(Q)/sizeof(Q[0]); ++i) {
        fprintf(stderr, "  %s U+%04X:", Q[i].label, Q[i].cp);
        int depth = 0;
        for (Font* c = f; c; c = c->fallback, ++depth) {
            unsigned int gi = FT_Get_Char_Index(c->ft_face, Q[i].cp);
            fprintf(stderr, " [d%d=gi%u]", depth, gi);
        }
        fprintf(stderr, "\n");
    }
}
