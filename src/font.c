#include "font.h"

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_OUTLINE_H
#include <hb.h>
#include <hb-ft.h>

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Device render scale for HiDPI (set via font_set_render_scale). Fonts created
 * while this is >1 rasterize glyph bitmaps at scale x their point size but
 * report logical (÷scale) metrics, so layout stays in points and text is
 * crisp. 1.0 = standard DPI; at 1.0 all the scale math below is the identity. */
static float g_render_scale = 1.0f;

void font_set_render_scale(float scale)
{
    g_render_scale = (scale >= 1.0f) ? scale : 1.0f;
}

typedef struct GlyphSlot {
    unsigned int      glyph_index;
    SDL_Texture*      tex;
    int               w, h;
    int               bearing_x, bearing_y;
    int               is_color;     /* BGRA bitmap (emoji): don't tint */
    struct GlyphSlot* next;
} GlyphSlot;

#define GLYPH_BUCKETS 256

struct Font {
    SDL_Renderer* renderer;
    FT_Library    ft_lib;
    FT_Face       ft_face;
    hb_font_t*    hb_font;
    hb_buffer_t*  hb_buf;
    int           pixel_size;   /* logical point size requested by the caller */
    float         scale;        /* device render scale; 1.0 on standard DPI    */
    /* Logical points per rasterized glyph pixel. 1/scale for outline fonts.
     * For bitmap-strike fonts (Apple Color Emoji, Noto Color Emoji) that only
     * ship fixed sizes, the nearest strike is selected and this also folds in
     * the strike->requested-size ratio, so the bitmap is scaled into place. */
    double        px_to_pt;
    int           is_color;     /* FT_HAS_COLOR: a color-emoji capable face */
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

/* ---------- Unicode classification for run building -------------------- */

/* Codepoints that never start a run of their own: they modify the preceding
 * base and must be shaped together with it (in the same font) or the shaper
 * can't compose/ligate them. Covers combining marks (so NFD text such as
 * macOS filenames "s" + U+0327 composes to "ş"), ZWJ/ZWNJ, variation
 * selectors, emoji skin-tone modifiers, tag characters and the keycap mark. */
static int cp_is_attached(uint32_t cp)
{
    return (cp >= 0x0300  && cp <= 0x036F)  ||  /* combining diacriticals    */
           (cp >= 0x1AB0  && cp <= 0x1AFF)  ||  /* combining ext.            */
           (cp >= 0x1DC0  && cp <= 0x1DFF)  ||  /* combining supplement      */
           (cp >= 0x20D0  && cp <= 0x20FF)  ||  /* combining for symbols     */
           (cp >= 0xFE20  && cp <= 0xFE2F)  ||  /* combining half marks      */
           cp == 0x200C || cp == 0x200D     ||  /* ZWNJ / ZWJ                */
           (cp >= 0xFE00  && cp <= 0xFE0F)  ||  /* variation selectors       */
           (cp >= 0xE0100 && cp <= 0xE01EF) ||  /* variation selectors supp. */
           (cp >= 0x1F3FB && cp <= 0x1F3FF) ||  /* emoji skin tones          */
           (cp >= 0xE0020 && cp <= 0xE007F);    /* tag chars (subdiv. flags) */
}

/* Unicode Emoji_Presentation=Yes: codepoints that default to the colorful
 * emoji glyph rather than a text/symbol glyph. Anything else in an emoji
 * font (☺, ❤, ⚠, digits, ©…) stays text-presentation unless followed by
 * U+FE0F. Ranges from emoji-data.txt (Unicode 15). */
static int cp_is_emoji_presentation(uint32_t cp)
{
    static const struct { uint32_t lo, hi; } R[] = {
        {0x231A,0x231B},{0x23E9,0x23EC},{0x23F0,0x23F0},{0x23F3,0x23F3},
        {0x25FD,0x25FE},{0x2614,0x2615},{0x2648,0x2653},{0x267F,0x267F},
        {0x2693,0x2693},{0x26A1,0x26A1},{0x26AA,0x26AB},{0x26BD,0x26BE},
        {0x26C4,0x26C5},{0x26CE,0x26CE},{0x26D4,0x26D4},{0x26EA,0x26EA},
        {0x26F2,0x26F3},{0x26F5,0x26F5},{0x26FA,0x26FA},{0x26FD,0x26FD},
        {0x2705,0x2705},{0x270A,0x270B},{0x2728,0x2728},{0x274C,0x274C},
        {0x274E,0x274E},{0x2753,0x2755},{0x2757,0x2757},{0x2795,0x2797},
        {0x27B0,0x27B0},{0x27BF,0x27BF},{0x2B1B,0x2B1C},{0x2B50,0x2B50},
        {0x2B55,0x2B55},
        {0x1F004,0x1F004},{0x1F0CF,0x1F0CF},{0x1F18E,0x1F18E},
        {0x1F191,0x1F19A},{0x1F1E6,0x1F1FF},{0x1F201,0x1F201},
        {0x1F21A,0x1F21A},{0x1F22F,0x1F22F},{0x1F232,0x1F236},
        {0x1F238,0x1F23A},{0x1F250,0x1F251},{0x1F300,0x1F320},
        {0x1F32D,0x1F335},{0x1F337,0x1F37C},{0x1F37E,0x1F393},
        {0x1F3A0,0x1F3CA},{0x1F3CF,0x1F3D3},{0x1F3E0,0x1F3F0},
        {0x1F3F4,0x1F3F4},{0x1F3F8,0x1F43E},{0x1F440,0x1F440},
        {0x1F442,0x1F4FC},{0x1F4FF,0x1F53D},{0x1F54B,0x1F54E},
        {0x1F550,0x1F567},{0x1F57A,0x1F57A},{0x1F595,0x1F596},
        {0x1F5A4,0x1F5A4},{0x1F5FB,0x1F64F},{0x1F680,0x1F6C5},
        {0x1F6CC,0x1F6CC},{0x1F6D0,0x1F6D2},{0x1F6D5,0x1F6D7},
        {0x1F6DC,0x1F6DF},{0x1F6EB,0x1F6EC},{0x1F6F4,0x1F6FC},
        {0x1F7E0,0x1F7EB},{0x1F7F0,0x1F7F0},{0x1F90C,0x1F93A},
        {0x1F93C,0x1F945},{0x1F947,0x1F9FF},{0x1FA70,0x1FA7C},
        {0x1FA80,0x1FA88},{0x1FA90,0x1FABD},{0x1FABF,0x1FAC5},
        {0x1FACE,0x1FADB},{0x1FAE0,0x1FAE8},{0x1FAF0,0x1FAF8},
    };
    if (cp < 0x231A) return 0;
    for (size_t i = 0; i < sizeof R / sizeof R[0]; ++i)
        if (cp >= R[i].lo && cp <= R[i].hi) return 1;
    return 0;
}

/* Should the base codepoint at `cp` (whose next codepoint starts at byte
 * `next`) be drawn with a color-emoji face? U+FE0F after it forces emoji,
 * U+FE0E forces text, otherwise the Unicode default decides. */
static int emoji_wanted(const char* s, size_t len, size_t next, uint32_t cp)
{
    if (next < len) {
        size_t a;
        uint32_t n = utf8_decode(s + next, len - next, &a);
        if (n == 0xFE0F) return 1;
        if (n == 0xFE0E) return 0;
    }
    return cp_is_emoji_presentation(cp);
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
     * to letter spacing and the perceived weight of strokes.
     * FT_LOAD_COLOR: ask for color glyphs (COLR layers, sbix/CBDT bitmaps)
     * from emoji faces; ignored by ordinary fonts. */
    int bold = (f->style & FONT_STYLE_BOLD) != 0;
    int load_flags = FT_LOAD_TARGET_LIGHT;
    if (f->is_color) load_flags |= FT_LOAD_COLOR;
    if (!bold) load_flags |= FT_LOAD_RENDER;
    if (FT_Load_Glyph(f->ft_face, gi, load_flags) != 0) return NULL;

    if (bold) {
        if (f->ft_face->glyph->format == FT_GLYPH_FORMAT_OUTLINE)
            FT_Outline_Embolden(&f->ft_face->glyph->outline, 64);
        /* Render with the same target so bold + regular metrics agree.
         * (Bitmap glyphs come back already rendered; this is a no-op.) */
        if (f->ft_face->glyph->format != FT_GLYPH_FORMAT_BITMAP &&
            FT_Render_Glyph(f->ft_face->glyph, FT_RENDER_MODE_LIGHT) != 0)
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
                    /* Color bitmap (emoji). FreeType hands us premultiplied
                     * BGRA; SDL's BLEND mode expects straight alpha, so
                     * un-premultiply or the edges render dark and muddy. */
                    slot->is_color = 1;
                    for (int y = 0; y < slot->h; ++y) {
                        const unsigned char* row = src + y * pitch;
                        for (int x = 0; x < slot->w; ++x) {
                            uint32_t b = row[x * 4 + 0], gg = row[x * 4 + 1];
                            uint32_t r = row[x * 4 + 2], a  = row[x * 4 + 3];
                            if (a > 0 && a < 255) {
                                r = (r * 255 + a / 2) / a; if (r > 255) r = 255;
                                gg= (gg* 255 + a / 2) / a; if (gg> 255) gg= 255;
                                b = (b * 255 + a / 2) / a; if (b > 255) b = 255;
                            }
                            px[y * dst_pitch + x] =
                                (a << 24) | (r << 16) | (gg << 8) | b;
                        }
                    }
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

/* Bitmap-only color fonts (Apple Color Emoji: sbix, Noto Color Emoji: CBDT)
 * ship a handful of fixed strikes and FT_Set_Pixel_Sizes rejects any other
 * size — which used to silently drop the emoji fallback at almost every font
 * size. Select the smallest strike that is >= the wanted size (else the
 * largest available) and report how much the bitmaps need scaling. Returns
 * 0 on success. */
static int select_nearest_strike(FT_Face face, int want_px, double* bm_scale)
{
    if (face->num_fixed_sizes <= 0) return -1;
    int best = -1, best_px = 0;
    for (int i = 0; i < face->num_fixed_sizes; ++i) {
        int px = (int)(face->available_sizes[i].y_ppem >> 6);
        if (px <= 0) px = face->available_sizes[i].height;
        if (px <= 0) continue;
        if (best < 0) { best = i; best_px = px; continue; }
        int cur_ok = best_px >= want_px, this_ok = px >= want_px;
        if ((this_ok && (!cur_ok || px < best_px)) ||
            (!this_ok && !cur_ok && px > best_px)) {
            best = i; best_px = px;
        }
    }
    if (best < 0 || FT_Select_Size(face, best) != 0) return -1;
    *bm_scale = (double)want_px / (double)best_px;
    return 0;
}

Font* font_create(SDL_Renderer* r, const char* ttf_path,
                  int pixel_size, FontStyle style)
{
    Font* f = calloc(1, sizeof *f);
    f->renderer   = r;
    f->pixel_size = pixel_size;
    f->scale      = (g_render_scale >= 1.0f) ? g_render_scale : 1.0f;
    f->style      = style;

    if (FT_Init_FreeType(&f->ft_lib) != 0) {
        fprintf(stderr, "FT_Init_FreeType failed\n");
        free(f); return NULL;
    }
    if (FT_New_Face(f->ft_lib, ttf_path, 0, &f->ft_face) != 0) {
        fprintf(stderr, "FT_New_Face failed for %s\n", ttf_path);
        FT_Done_FreeType(f->ft_lib); free(f); return NULL;
    }
    f->is_color = FT_HAS_COLOR(f->ft_face) ? 1 : 0;

    /* Rasterize at the device scale: a 16pt font on a 2x Retina display loads
     * 32px glyph bitmaps. Metrics below are converted back to logical points
     * so callers lay out unchanged. */
    int    want_px  = (int)lround(pixel_size * f->scale);
    double bm_scale = 1.0;
    int    bitmap_only = (f->is_color && f->ft_face->num_fixed_sizes > 0) ||
                         !FT_IS_SCALABLE(f->ft_face);
    int ok = bitmap_only
        ? select_nearest_strike(f->ft_face, want_px, &bm_scale) == 0
        : FT_Set_Pixel_Sizes(f->ft_face, 0, (FT_UInt)want_px) == 0;
    if (!ok) {
        fprintf(stderr, "FT_Set_Pixel_Sizes failed for %s\n", ttf_path);
        FT_Done_Face(f->ft_face); FT_Done_FreeType(f->ft_lib);
        free(f); return NULL;
    }
    f->px_to_pt = bm_scale / (double)f->scale;

    if (style & FONT_STYLE_ITALIC) {
        FT_Matrix m = {
            0x10000, (FT_Fixed)(0.21 * 0x10000),
            0,       0x10000
        };
        FT_Set_Transform(f->ft_face, &m, NULL);
    }

    /* Logical-point metrics (raster px -> points). At scale 1.0 these are the
     * exact original integer values. */
    f->ascent      = (int)lround((f->ft_face->size->metrics.ascender  >> 6) * f->px_to_pt);
    f->descent     = (int)lround(-(f->ft_face->size->metrics.descender >> 6) * f->px_to_pt);
    f->line_height = (int)lround((f->ft_face->size->metrics.height     >> 6) * f->px_to_pt);

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

    /* Pen runs in logical points. Glyph bitmaps are rasterized at device
     * resolution, so their offsets/bearings/sizes (raster px) are converted
     * with px_to_pt; SDL_RenderSetLogicalSize then scales the canvas back up
     * to device pixels for the blit.
     *
     * Destination rects are FLOAT on purpose. On a 2x Retina display one
     * logical point is two device pixels; rounding each glyph's top and
     * height to whole points (as SDL_RenderCopy's int rect forces) shifted
     * glyphs up/down by a device pixel depending on their bearing parity
     * and stretched odd-height bitmaps — text visibly wobbled off the
     * baseline and blurred. Float rects land every glyph on its exact
     * device pixel. At scale 1.0 every term reduces to the original integer
     * arithmetic. */
    const double k = f->px_to_pt;
    double pen_x = x;
    double pen_y = y_baseline;
    for (unsigned int i = 0; i < n; ++i) {
        if (draw) {
            GlyphSlot* g = glyph_load(f, infos[i].codepoint);
            if (g && g->tex) {
                int x_off = poss[i].x_offset >> 6;
                int y_off = poss[i].y_offset >> 6;
                SDL_FRect dst = {
                    (float)(pen_x + (x_off + g->bearing_x) * k),
                    (float)(pen_y - (y_off + g->bearing_y) * k),
                    (float)(g->w * k),
                    (float)(g->h * k)
                };
                if (g->is_color) {
                    /* Emoji carry their own colors; only the alpha follows
                     * the text color (so muted/dimmed text dims its emoji). */
                    SDL_SetTextureColorMod(g->tex, 255, 255, 255);
                } else {
                    SDL_SetTextureColorMod(g->tex, color.r, color.g, color.b);
                }
                SDL_SetTextureAlphaMod(g->tex, color.a);
                SDL_RenderCopyF(f->renderer, g->tex, NULL, &dst);
            }
        }
        pen_x += (poss[i].x_advance >> 6) * k;
        pen_y += (poss[i].y_advance >> 6) * k;
    }
    return (int)lround(pen_x - x);
}

/* For each base codepoint, pick the font in the chain that should draw it:
 * for emoji-presentation codepoints (and anything followed by U+FE0F) the
 * first COLOR face that has the glyph, so emoji render as emoji rather than
 * as the monochrome outline a symbol font earlier in the chain may carry;
 * otherwise the first face with the glyph. Consecutive codepoints that map
 * to the same font — plus attached marks/ZWJ/selectors, which always stay
 * with their base — are grouped into one run and shaped with HarfBuzz, so
 * ZWJ sequences, flags, keycaps and NFD diacritics compose correctly. */
static Font* font_for_cp(Font* f, uint32_t cp, int want_emoji)
{
    if (want_emoji) {
        for (Font* c = f; c; c = c->fallback)
            if (c->is_color && FT_Get_Char_Index(c->ft_face, cp) != 0)
                return c;
    }
    for (Font* c = f; c; c = c->fallback)
        if (FT_Get_Char_Index(c->ft_face, cp) != 0) return c;
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
        Font* chosen = font_for_cp(f, cp, emoji_wanted(utf8, len, i + adv, cp));

        size_t run_start = i;
        size_t run_end   = i + adv;
        while (run_end < len) {
            size_t adv2;
            uint32_t cp2 = utf8_decode(utf8 + run_end, len - run_end, &adv2);
            if (!cp_is_attached(cp2)) {
                int we = emoji_wanted(utf8, len, run_end + adv2, cp2);
                if (font_for_cp(f, cp2, we) != chosen) break;
            }
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
